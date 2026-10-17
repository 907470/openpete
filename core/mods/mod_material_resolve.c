/* mod_material_resolve.c - the material table and its resolution rules.
 *
 * Declarative materials: whole-class overrides matched at DRAW time
 * against the provenance tag - extractors stay selector-agnostic.
 * Per-instance (refine) materials: a callback runs at EXTRACTION (the moby
 * walk, cpu available) per matching instance, returns a per-draw uniform
 * block or declines. Its outcome is interned to a small per-present SLOT
 * id stamped into the tag's spare bits, so the batcher splits draws by
 * outcome (two instances of one class with different callback results
 * land in different batches); the slot's uniform block binds at the
 * override draw. Conflicts: enabled-list priority, the later-loaded mod
 * wins; inside one mod the registration order decides, by the owner's api
 * pin (mod_mat_outranks).
 *
 * Per-present, not identity-replayed: reextract_record_pass re-walks
 * every present, and the callback reads TICK-STABLE guest RAM (the
 * sub-tick spine interpolates render-side only, never mutating
 * gameplay RAM - the oracle guarantees it), so per-present invocation
 * yields identical results across a canonical+intermediates span (no
 * strobing). Canonical-compute + identity-replay would only save the
 * callback cost on intermediates; deferred as a pure optimization.
 *
 * The GPU half (shader handles, pipelines, the registry lock and the
 * public entry points that take it) is in mod_shader.c. */

#include "mod_material_resolve.h"
#include "openpete_log.h"
#include "mod_vec.h"
#include "openpete_native_render.h"   /* set_tag_split arming (material selectors) */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mod_material_t s_mats[MOD_MATERIAL_MAX];
static int s_mat_count;
/* Census only: the table cannot grow past the bucket-id bound, so the
 * row reports how much of that bound a run's content uses. */
static mod_fixed s_mat_census;
static int s_mat_sel_dirty;      /* any material carries a parked selector */
static int s_mat_has_instance;   /* cached: any live selector has a callback */

int mod_mat_count(void) { return s_mat_count; }
/* Forward: the reset clears state defined further down. */
static void reset_level_and_view(void);
void mod_mat_reset(void) {
    memset(s_mats, 0, sizeof s_mats);
    s_mat_count = 0;
    s_mat_sel_dirty = 0;
    s_mat_has_instance = 0;
    reset_level_and_view();
}
mod_material_t* mod_mat_at(int i) { return &s_mats[i]; }

_Static_assert(MOD_MATERIAL_SLOTS == OPENPETE_MODS_MATERIAL_VIEW_SLOTS, "view mirrors the slot table");
_Static_assert(MOD_MATERIAL_MAX == OPENPETE_MODS_MATERIAL_VIEW_MATS, "view mirrors the material table");
typedef struct {
    int      mat;            /* winning selector index */
    uint32_t len;
    unsigned char bytes[OPENPETE_MATERIAL_UNIFORMS_MAX];
} mat_slot_t;
static mat_slot_t s_mat_slots[MOD_MATERIAL_SLOTS];
static int s_mat_slot_n = 1;    /* next free; 0 is the reserved sentinel */

/* The present thread's view of the slot table (NULL = the live table, for
 * the single-threaded path). See openpete_mods_material_view_t. */
static const openpete_mods_material_view_t* s_mat_view;
/** @brief Return the slot count of the installed view, else of the live table. */
static int view_slot_n(void) { return s_mat_view ? s_mat_view->slot_n : s_mat_slot_n; }
/** @brief Return the material index of an interned slot in the installed view, else in the live table.
 *
 * @param s Slot index below view_slot_n(). */
static int view_slot_mat(int s) { return s_mat_view ? s_mat_view->slots[s].mat : s_mat_slots[s].mat; }
int mod_mat_view_static_slot(int m) { return s_mat_view ? s_mat_view->static_slot[m] : s_mats[m].static_slot; }

/* Slot ids are minted from ONE global counter but stamped into per-path
 * tag fields of differing width, so the table can never outgrow the
 * NARROWEST field that carries an id. Terrain's is the narrowest at 9
 * bits, and its decode masks (tag >> 16) & 0x1FF: an id of 512 would
 * come back as 0, which reads as "defer to declarative resolve" - the
 * wrong material, silently, with no crash and no log. 0x1FF itself is
 * the declined->stock sentinel, so the last usable id is 510. Raising
 * MOD_MATERIAL_SLOTS means widening that field first. */
typedef char nr_assert_mat_slots_fit_narrowest_tag_field[
    (MOD_MATERIAL_SLOTS <= OPENPETE_MATERIAL_TERRAIN_STOCK) ? 1 : -1];

/* ------------------------------------------------------------------ */
/* Resolve census (OPENPETE_MATERIAL_STATS). */

static _Atomic int s_census = -1;   /* -1 = not read yet */
static _Atomic unsigned s_cen[MOD_MATERIAL_MAX][MOD_MAT_CENSUS_N];
/* Terrain key histograms: (texid -1..127) x env x lp, the bake's
 * openpete_mods_material_terrain_bucket queries and the walked draw-time
 * resolves. Each key keeps its count and the last bucket it resolved to
 * (material index + 1, 0 stock). */
#define CEN_TER_KEYS (129 * 4)
static _Atomic unsigned s_cen_bake_n[CEN_TER_KEYS];
static _Atomic int      s_cen_bake_b[CEN_TER_KEYS];
static _Atomic unsigned s_cen_walk_n[CEN_TER_KEYS];
static _Atomic int      s_cen_walk_b[CEN_TER_KEYS];
/* Moby classes seen by extraction (moby_slot calls), ids below 1024. */
#define CEN_CLASSES 1024
static _Atomic unsigned s_cen_class[CEN_CLASSES];
static _Atomic unsigned s_cen_presents;
static _Atomic unsigned s_cen_view_resolves;   /* draw-time resolves under an installed view */
static int s_cen_atexit;

int mod_mat_census_on(void) {
    if (s_census < 0) {
        const char* e = getenv("OPENPETE_MATERIAL_STATS");
        s_census = (e && *e && *e != '0');
        if (s_census && !s_cen_atexit) { s_cen_atexit = 1; atexit(mod_mat_census_print); }
    }
    return s_census;
}
void mod_mat_census_force(int on) { s_census = on ? 1 : 0; }
void mod_mat_census_reset(void) {
    for (int i = 0; i < MOD_MATERIAL_MAX; i++)
        for (int c = 0; c < MOD_MAT_CENSUS_N; c++) atomic_store_explicit(&s_cen[i][c], 0u, memory_order_relaxed);
    for (int i = 0; i < CEN_TER_KEYS; i++) {
        atomic_store_explicit(&s_cen_bake_n[i], 0u, memory_order_relaxed);
        atomic_store_explicit(&s_cen_walk_n[i], 0u, memory_order_relaxed);
    }
    for (int i = 0; i < CEN_CLASSES; i++) atomic_store_explicit(&s_cen_class[i], 0u, memory_order_relaxed);
    atomic_store_explicit(&s_cen_presents, 0u, memory_order_relaxed);
    atomic_store_explicit(&s_cen_view_resolves, 0u, memory_order_relaxed);
}
unsigned mod_mat_census_view_resolves(void) {
    return atomic_load_explicit(&s_cen_view_resolves, memory_order_relaxed);
}
/** @brief Add one to a material's census counter when the census is on.
 *
 * @param m Material index; out-of-range indices are ignored.
 * @param which MOD_MAT_CENSUS_* counter. */
static void cen_bump(int m, int which) {
    if (!mod_mat_census_on() || m < 0 || m >= MOD_MATERIAL_MAX) return;
    atomic_fetch_add_explicit(&s_cen[m][which], 1u, memory_order_relaxed);
}
/** @brief Record one terrain key query and the bucket it resolved to.
 *
 * @param n Count array of CEN_TER_KEYS.
 * @param b Last-bucket array of CEN_TER_KEYS.
 * @param texid Tiledef id, or -1.
 * @param env Env-rewrite flag.
 * @param lp LP-tier flag.
 * @param bucket Material index + 1, or 0 for stock. */
static void cen_ter(_Atomic unsigned* n, _Atomic int* b, int texid, int env, int lp, int bucket) {
    if (!mod_mat_census_on() || texid < -1 || texid > 127) return;
    const int k = (texid + 1) * 4 + (env ? 2 : 0) + (lp ? 1 : 0);
    atomic_fetch_add_explicit(&n[k], 1u, memory_order_relaxed);
    atomic_store_explicit(&b[k], bucket, memory_order_relaxed);
}
void mod_mat_census_bucket_pass(int m) { cen_bump(m, MOD_MAT_CENSUS_BUCKET_PASSES); }
unsigned mod_mat_census_get(int m, int which) {
    if (m < 0 || m >= MOD_MATERIAL_MAX || which < 0 || which >= MOD_MAT_CENSUS_N) return 0;
    return atomic_load_explicit(&s_cen[m][which], memory_order_relaxed);
}

/** @brief Print the terrain key comparison: bake keys, walked keys, keys both saw, and disagreements.
 *
 * A disagreement is a key both paths queried whose last buckets differ;
 * the first eight are listed. */
static void census_print_terrain(void) {
    unsigned bake_keys = 0, walk_keys = 0, both = 0, differ = 0, bake_q = 0, walk_q = 0;
    unsigned untex_bake = 0;
    for (int k = 0; k < CEN_TER_KEYS; k++) {
        const unsigned bn = atomic_load_explicit(&s_cen_bake_n[k], memory_order_relaxed);
        const unsigned wn = atomic_load_explicit(&s_cen_walk_n[k], memory_order_relaxed);
        bake_q += bn; walk_q += wn;
        if (bn) { bake_keys++; if (k < 4) untex_bake += bn; }
        if (wn) walk_keys++;
        if (!bn || !wn) continue;
        both++;
        const int bb = atomic_load_explicit(&s_cen_bake_b[k], memory_order_relaxed);
        const int wb = atomic_load_explicit(&s_cen_walk_b[k], memory_order_relaxed);
        if (bb == wb) continue;
        if (differ++ < 8)
            fprintf(stderr, "[material-census] terrain key texid=%d env=%d lp=%d: bake bucket %d, walked %d\n",
                    k / 4 - 1, (k >> 1) & 1, k & 1, bb, wb);
    }
    fprintf(stderr, "[material-census] terrain bake: %u queries over %u keys (%u with texid -1); "
                    "walked: %u resolves over %u keys; both %u, last bucket differs on %u\n",
            bake_q, bake_keys, untex_bake, walk_q, walk_keys, both, differ);
}

/** @brief Print the five moby classes extraction saw most, with their counts. */
static void census_print_classes(void) {
    int top[5] = {-1, -1, -1, -1, -1};
    unsigned topv[5] = {0, 0, 0, 0, 0};
    for (int cl = 0; cl < CEN_CLASSES; cl++) {
        const unsigned v = atomic_load_explicit(&s_cen_class[cl], memory_order_relaxed);
        int t = 5;
        while (t > 0 && (top[t - 1] < 0 || topv[t - 1] < v)) t--;
        if (!v || t >= 5) continue;
        for (int u = 4; u > t; u--) { top[u] = top[u - 1]; topv[u] = topv[u - 1]; }
        top[t] = cl; topv[t] = v;
    }
    fprintf(stderr, "[material-census] moby classes most extracted:");
    for (int t = 0; t < 5 && top[t] >= 0; t++) fprintf(stderr, " %d(%u)", top[t], topv[t]);
    fprintf(stderr, "\n");
}

void mod_mat_census_print(void) {
    if (s_census <= 0) return;
    unsigned long long engaged = 0;
    fprintf(stderr, "[material-census] presents=%u materials=%d\n",
            atomic_load_explicit(&s_cen_presents, memory_order_relaxed), s_mat_count);
    fprintf(stderr, "[material-census] idx owner chan class ident tex level flags kind contract load"
                    " | draws bucket_passes extract_wins refine_calls declines fallthroughs"
                    " decline_shadows outranked stale_memo\n");
    for (int i = 0; i < s_mat_count; i++) {
        const mod_material_t* m = &s_mats[i];
        unsigned c[MOD_MAT_CENSUS_N];
        for (int j = 0; j < MOD_MAT_CENSUS_N; j++) c[j] = mod_mat_census_get(i, j);
        engaged += (unsigned long long)c[MOD_MAT_CENSUS_DRAWS] + c[MOD_MAT_CENSUS_BUCKET_PASSES] +
                   c[MOD_MAT_CENSUS_REFINE_CALLS];
        fprintf(stderr, "[material-census] %3d %s %d %d %d %d %d %#x %s %d %d | %u %u %u %u %u %u %u %u %u\n",
                i, m->owner ? m->owner : "?", m->sel.channel, m->sel.moby_class, m->sel.ident,
                m->sel.texture, m->sel.level, m->sel.flags, m->per_instance ? "refine" : "decl",
                m->contract, m->load_index,
                c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8]);
    }
    fprintf(stderr, "[material-census] view-installed resolves=%u\n",
            atomic_load_explicit(&s_cen_view_resolves, memory_order_relaxed));
    census_print_terrain();
    census_print_classes();
    fprintf(stderr, "[material-census] engaged=%llu (draws + bucket passes + refine calls)\n", engaged);
}

/* ------------------------------------------------------------------ */
/* Owner hold list. */

/* Owners whose postfx passes and materials the compose in progress leaves
 * out (openpete_mods_hold_owners). Written and read by the display owner
 * only, inside the compose: only the compose-side lookups read it. */
#define MOD_HOLD_MAX 16
static const char* s_hold_ids[MOD_HOLD_MAX];
static int s_hold_n;

void openpete_mods_hold_owners(const char* const* ids, int n) {
    s_hold_n = 0;
    for (int i = 0; ids && i < n && s_hold_n < MOD_HOLD_MAX; i++) s_hold_ids[s_hold_n++] = ids[i];
}

int mod_owner_held(const char* owner) {
    for (int i = 0; i < s_hold_n; i++)
        if (strcmp(s_hold_ids[i], owner) == 0) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Postfx ranking. */

int mod_postfx_rank(mod_postfx_rank_t* v, int n, int cap) {
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0; j--) {
            const mod_postfx_rank_t *a = &v[j - 1], *b = &v[j];
            int swap = (a->load_index > b->load_index) ||
                       (a->load_index == b->load_index && a->order > b->order) ||
                       (a->load_index == b->load_index && a->order == b->order &&
                        a->index > b->index);
            if (!swap) break;
            { mod_postfx_rank_t t = v[j - 1]; v[j - 1] = v[j]; v[j] = t; }
        }
    if (cap < 0) cap = 0;
    if (n <= cap) return n;
    /* Over the cap the lowest-priority passes, at the front, are dropped. */
    memmove(v, v + (n - cap), (size_t)cap * sizeof *v);
    return cap;
}

/* ------------------------------------------------------------------ */
/* Table mutation. */

/* native_render arms the batcher tag-split while any selector lives;
 * s_mat_has_instance gates the moby walk's per-instance resolve.
 *
 * Arming keys on REGISTRATION (live), never on the owner's live-toggle
 * state: entry - and therefore material_register - runs before the
 * loader marks a mod enabled, so an enabled-keyed arm would read 0 at
 * boot and never re-arm. A disabled owner's selectors therefore still
 * cost a batch split and a winner scan, but can produce no override:
 * every winner/resolve path consults openpete_mods_is_enabled. */
/* The retained terrain cache bakes each face's winning selector into the face
 * itself, so it must re-bake whenever the TERRAIN-relevant selector set moves.
 * This counter is that key, and it is deliberately NARROW: a counter that also
 * moved for moby, sky, flame and particle materials - and for every enable
 * toggle of one - re-baked the whole cache for changes that cannot alter a
 * single terrain bucket, which cost 2.7x the frame rate while a mod was
 * toggling rows (terrain.build 33% -> 73% of sim_tick). Move it only for
 * changes terrain_winner() could actually see. */
static unsigned s_mat_ter_epoch;

void mod_mat_ter_epoch_bump(void) { s_mat_ter_epoch++; }

/** @brief Test whether a material could bucket a terrain face in some enable state and level.
 *
 * True for a live material on channel -1 or OP_CHAN_TERRAIN. The mutators
 * use it to decide whether a change bumps s_mat_ter_epoch, so it ignores
 * the enable state, which is the change being reacted to, and the level,
 * which is not known at every bump site. A wider answer costs a spurious
 * re-bake; a narrower one would leave faces stamped for a selector that no
 * longer wins. */
static int terrain_sel_relevant(const mod_material_t* m) {
    return m->live && (m->sel.channel == -1 || m->sel.channel == OP_CHAN_TERRAIN);
}

/* Runs under the registry write lock or on the compose reader thread. The
 * split is armed while any material is live, whatever its owner's enabled
 * state, and s_mat_has_instance is set while any live material has a
 * per-instance callback. */
void mod_mat_rearm_split(void) {
    int any = 0, inst = 0;
    for (int i = 0; i < s_mat_count; i++)
        if (s_mats[i].live) { any = 1; if (s_mats[i].per_instance) inst = 1; }
    s_mat_has_instance = inst;
    openpete_native_set_tag_split(any);
}

int openpete_mods_material_has_instance(void) { return s_mat_has_instance; }

unsigned openpete_mods_material_terrain_epoch(void) {
    /* Sum of two monotonic counters - strictly increases on any change the
     * terrain bake cares about; +1 keeps 0 free as the latch's "unset". The
     * mod toggle epoch rides along because terrain_sel_usable() consults
     * openpete_mods_is_enabled(owner). */
    return 1u + s_mat_ter_epoch + openpete_mods_toggle_epoch();
}

int mod_mat_sel_valid(const char* id, const openpete_mod_material_selector_t* sel) {
    if (sel->struct_size != (uint32_t)sizeof *sel) {
        OPL_ERROR_F("mods", "%s: material: selector size %u, engine expects %u "
                    "- rebuild the mod against this SDK",
                    id, sel->struct_size, (unsigned)sizeof *sel);
        return 0;
    }
    if (sel->flags & ~(unsigned)OP_SEL_FLAGS_KNOWN) {
        OPL_ERROR_F("mods", "%s: material: selector carries unknown flag bits "
                    "%#x - this engine would match wider than you asked",
                    id, sel->flags & ~(unsigned)OP_SEL_FLAGS_KNOWN);
        return 0;
    }
    if (sel->channel != -1 &&
        (sel->channel < OP_CHAN_MOBY || sel->channel > OP_CHAN_PORTAL)) {
        OPL_ERROR_F("mods", "%s: material: unknown channel %d", id,
                    sel->channel);
        return 0;
    }
    if (sel->texture != -1 &&
        (sel->texture < 0 || sel->texture > 0x7F ||
         (sel->channel != -1 && sel->channel != OP_CHAN_TERRAIN) ||
         (sel->flags & (unsigned)OP_SEL_UNTEXTURED))) {
        OPL_ERROR_F("mods", "%s: material texture selector must be a terrain "
                    "Tiledef id 0..127 on a textured path (got %d)", id,
                    sel->texture);
        return 0;
    }
    return 1;
}

int mod_mat_append(const char* id, const openpete_mod_material_selector_t* sel,
                   int vert_shader, int frag_shader, int contract,
                   openpete_mod_material_fn per_instance, int load_index,
                   int api_pin, unsigned generation) {
    if (vert_shader < 0 && frag_shader < 0) return -1;   /* nothing to do */
    if (s_mat_count >= MOD_MATERIAL_MAX) {
        OPL_ERROR_F("mods", "%s: material table full (%d)", id,
                    MOD_MATERIAL_MAX);
        return -1;
    }
    mod_material_t* m = &s_mats[s_mat_count];
    memset(m, 0, sizeof *m);
    m->owner = id;
    m->sel = *sel;
    m->vert = vert_shader;
    m->frag = frag_shader;
    m->contract = contract;
    m->per_instance = per_instance;
    m->load_index = load_index;
    m->api_pin = api_pin;
    m->live = 1;
    m->generation = generation;
    m->user_enabled = 1;          /* registration arms it; enable() toggles */
    int handle = s_mat_count++;
    if (!s_mat_census.storage)
        mod_fixed_init(&s_mat_census, "materials", 32, MOD_MATERIAL_MAX);
    mod_fixed_set(&s_mat_census, (size_t)s_mat_count);
    if (terrain_sel_relevant(m)) s_mat_ter_epoch++;
    mod_mat_rearm_split();
    OPL_INFO_F("mods", "%s: material selector armed (channel %d ident %d class %d "
               "flags %#x%s)", id, sel->channel, sel->ident, sel->moby_class, sel->flags,
               per_instance ? ", per-instance" : "");
    return handle;
}

int mod_mat_enable(int material, int on) {
    if (material < 0 || material >= s_mat_count || !s_mats[material].live)
        return -1;
    s_mats[material].user_enabled = on ? 1 : 0;
    if (terrain_sel_relevant(&s_mats[material])) s_mat_ter_epoch++;
    mod_mat_rearm_split();
    return 0;
}

int mod_mat_set_params(int material, const void* block, uint32_t len) {
    if (material < 0 || material >= s_mat_count || !s_mats[material].live)
        return -1;
    if (len > OPENPETE_MATERIAL_UNIFORMS_MAX) return -1;
    if (len && !block) return -1;
    s_mats[material].params_len = len;
    if (len) memcpy(s_mats[material].params, block, len);
    return 0;
}

int mod_mat_set_selector(const char* id, int material,
                         const openpete_mod_material_selector_t* sel) {
    if (!id || !sel) return -1;
    if (material < 0 || material >= s_mat_count || !s_mats[material].live)
        return -1;
    mod_material_t* m = &s_mats[material];
    if (m->owner != id && strcmp(m->owner, id) != 0) {
        OPL_ERROR_F("mods", "%s: material %d belongs to \"%s\"", id, material,
                    m->owner);
        return -1;
    }
    if (!mod_mat_sel_valid(id, sel)) return -1;
    /* Inside a present the new key is parked: a selector that changed mid-
     * present would shade part of the image under each key, and the retained
     * terrain bake keys off the material epoch, which may only move between
     * ticks. Outside one there is nothing to straddle, so it lands now. */
    if (openpete_mods_in_present) {
        m->sel_pending = *sel;
        m->sel_dirty = 1;
        s_mat_sel_dirty = 1;
        return 0;
    }
    const int was_ter = terrain_sel_relevant(m);
    m->sel = *sel;
    m->sel_dirty = 0;
    if (was_ter || terrain_sel_relevant(m)) s_mat_ter_epoch++;
    mod_mat_rearm_split();
    return 0;
}

int mod_mat_pending(void) { return s_mat_sel_dirty; }

void mod_mat_apply_pending(void) {
    if (!s_mat_sel_dirty) return;
    s_mat_sel_dirty = 0;
    int n = 0;
    for (int i = 0; i < s_mat_count; i++) {
        if (!s_mats[i].sel_dirty) continue;
        const int was_ter = terrain_sel_relevant(&s_mats[i]);
        s_mats[i].sel = s_mats[i].sel_pending;
        s_mats[i].sel_dirty = 0;
        if (was_ter || terrain_sel_relevant(&s_mats[i])) s_mat_ter_epoch++;
        n++;
    }
    if (n) mod_mat_rearm_split();
}

/* ------------------------------------------------------------------ */
/* Level key and present counter. */

/* Level id for level-scoped selector keys. The mods layer cannot read it
 * itself - the symbol is game-specific - so the game pushes it while it
 * renders. Draw-time resolve and the terrain bucket queries
 * (terrain_sel_usable) consume it, and -1 (nothing has pushed yet) makes
 * every level key inert rather than wrongly matching. */
static int s_cur_level = -1;
/* No lock: a compose blocked on the display never delays the push. */
void openpete_mods_set_level(int level_id) {
    /* The retained-terrain claim latches its verdict against
     * openpete_mods_material_terrain_epoch(), and that verdict depends on
     * the level (selectors are level-scoped, because sector and texture ids
     * are per-level). So a level change has to move the epoch, or a sector
     * latched in one level keeps that answer into the next. */
    if (level_id != s_cur_level) {
        s_cur_level = level_id; s_mat_ter_epoch++;
    }
}

/** @brief Clear the pushed level, the slot table, the installed view and the hold list for mod_mat_reset. */
static void reset_level_and_view(void) {
    s_cur_level = -1;
    s_mat_slot_n = 1;
    s_mat_view = NULL;
    s_hold_n = 0;
}

/** @brief Return the level id the game last pushed, or -1 before any push. */
static int openpete_mods_current_level(void) { return s_cur_level; }

/* Bumped every present, so a per-present memo (the terrain refine slot) can
 * tell "computed this present" from "computed under the last one". */
static unsigned s_mat_present = 1;

void openpete_mods_material_frame_reset(void) {
    s_mat_slot_n = 1;
    s_mat_present++;
    if (mod_mat_census_on()) atomic_fetch_add_explicit(&s_cen_presents, 1u, memory_order_relaxed);
    /* Re-intern every live material's static block, so a declarative
     * material has a slot to point at for the rest of this present. */
    for (int i = 0; i < s_mat_count; i++) {
        mod_material_t* m = &s_mats[i];
        m->static_slot = 0;
        if (!m->live || !m->params_len) continue;
        if (!m->user_enabled || !openpete_mods_is_enabled(m->owner)) continue;
        if (s_mat_slot_n >= MOD_MATERIAL_SLOTS) continue;   /* full -> no params */
        int sl = s_mat_slot_n++;
        s_mat_slots[sl].mat = i;
        s_mat_slots[sl].len = m->params_len;
        memcpy(s_mat_slots[sl].bytes, m->params, m->params_len);
        m->static_slot = sl;
    }
}
int  openpete_mods_material_slot_count(void) { return s_mat_slot_n; }

/* ------------------------------------------------------------------ */
/* Priority. */

int mod_mat_outranks(int a, int b) {
    if (b < 0) return 1;
    const mod_material_t* ma = &s_mats[a];
    const mod_material_t* mb = &s_mats[b];
    if (ma->load_index != mb->load_index) return ma->load_index > mb->load_index;
    /* One mod: registration order, its direction selected by the pin. */
    return ma->api_pin >= MOD_MAT_API_LATER_TIE ? a > b : a < b;
}

/* ------------------------------------------------------------------ */
/* Terrain: bake-side winner and bucket list. */

/** @brief Test whether a material can bucket terrain faces in a level.
 *
 * True when the material is live, user-enabled and owned by an enabled
 * mod, has the scene shading contract (retained terrain is a scene-contract
 * site), is on channel -1 or OP_CHAN_TERRAIN, and has no level key or one
 * equal to @p level; a negative @p level matches every level key. The
 * level test mirrors the draw-time resolve's, so a level-scoped selector
 * buckets faces only in its level. */
static int terrain_sel_usable(const mod_material_t* m, int level) {
    if (!m->live || !m->user_enabled || !openpete_mods_is_enabled(m->owner))
        return 0;
    if (m->contract != OPENPETE_MATERIAL_CONTRACT_SCENE) return 0;
    if (m->sel.channel != -1 && m->sel.channel != OP_CHAN_TERRAIN) return 0;
    if (m->sel.level != -1 && level >= 0 && m->sel.level != level) return 0;
    return 1;
}

/** @brief Test whether a terrain refine material's one key is a face's key.
 *
 * A terrain refine is keyed on one Tiledef id (texture key set,
 * OP_SEL_UNTEXTURED clear) or on the untextured band (OP_SEL_UNTEXTURED
 * set, no texture key); env and tier are not part of its key. Shared by the
 * bake, the walked refine winners and the draw-time re-derivation, so both
 * terrain paths rank one candidate set.
 * @param m The material.
 * @param untex Nonzero for an untextured face.
 * @param texid The face's Tiledef id when textured. */
static int terrain_refine_key_matches(const mod_material_t* m, int untex, int texid) {
    return untex ? ((m->sel.flags & OP_SEL_UNTEXTURED) && m->sel.texture == -1)
                 : (!(m->sel.flags & OP_SEL_UNTEXTURED) && m->sel.texture != -1 &&
                    m->sel.texture == texid);
}

/** @brief Test whether a declarative terrain material's selector accepts a face key.
 *
 * The draw-time resolve's predicate over the face's tag bits: the texture
 * key and the OP_SEL_ENV_ANIM, tier and textured/untextured flags.
 * @param m The material.
 * @param texid Tiledef id, or -1 for an untextured face.
 * @param env Nonzero for a face in the env-rewrite set.
 * @param lp Nonzero for an LP-tier face. */
static int terrain_decl_matches(const mod_material_t* m, int texid, int env, int lp) {
    const int untex = texid < 0;
    if (m->sel.texture != -1 && (untex || m->sel.texture != texid)) return 0;
    if ((m->sel.flags & OP_SEL_ENV_ANIM) && !env) return 0;
    if ((m->sel.flags & OP_SEL_HP_TIER) && lp) return 0;
    if ((m->sel.flags & OP_SEL_LP_TIER) && !lp) return 0;
    if ((m->sel.flags & OP_SEL_TEXTURED) && untex) return 0;
    if ((m->sel.flags & OP_SEL_UNTEXTURED) && !untex) return 0;
    return 1;
}

/** @brief Return the index of the winning terrain material for a face key, or -1.
 *
 * Over the materials terrain_sel_usable() accepts for the current level,
 * a refine material whose key is the face's (terrain_refine_key_matches)
 * and a declarative one whose selector accepts the face
 * (terrain_decl_matches) compete in one ranking (mod_mat_outranks), as the
 * walked draw-time resolve ranks a tag's refine slot against its
 * declarative winner.
 * @param texid Tiledef id, or -1 for an untextured face.
 * @param env Nonzero for a face in the env-rewrite set.
 * @param lp Nonzero for an LP-tier face. */
static int terrain_winner(int texid, int env, int lp) {
    if (s_mat_count == 0) return -1;
    const int untex = texid < 0;
    const int level = openpete_mods_current_level();
    int best = -1;
    for (int i = 0; i < s_mat_count; i++) {
        const mod_material_t* m = &s_mats[i];
        if (!terrain_sel_usable(m, level)) continue;
        const int match = m->per_instance ? terrain_refine_key_matches(m, untex, texid)
                                          : terrain_decl_matches(m, texid, env, lp);
        if (match && mod_mat_outranks(i, best)) best = i;
    }
    return best;
}

int openpete_mods_material_terrain_bucket(int texid, int env, int lp) {
    int w = terrain_winner(texid, env, lp);
    int b = (w < 0) ? 0 : ((w + 1 <= OPENPETE_MATERIAL_TER_BUCKET_MAX) ? w + 1 : 0);
    if (b && s_mats[w].per_instance) s_mats[w].bake_won = 1;
    cen_ter(s_cen_bake_n, s_cen_bake_b, texid, env, lp, b);
    return b;
}

/** @brief List the live terrain bucket ids for the pushed level, ascending.
 *
 * Ascending by construction: the scan runs in index order and the id is
 * the index + 1.
 * @param out Receives the ids.
 * @param cap Capacity of @p out.
 * @return Ids written. */
static int terrain_bucket_list_live(int* out, int cap) {
    int n = 0, level = openpete_mods_current_level();
    for (int i = 0; i < s_mat_count && n < cap; i++) {
        if (!terrain_sel_usable(&s_mats[i], level)) continue;
        if (i + 1 > OPENPETE_MATERIAL_TER_BUCKET_MAX) break;
        out[n++] = i + 1;
    }
    return n;
}

int openpete_mods_material_terrain_bucket_list(int* out, int cap) {
    if (!out || cap <= 0) return 0;
    if (s_mat_view) {
        int n = s_mat_view->ter_bucket_n;
        if (n > cap) n = cap;
        for (int i = 0; i < n; i++) out[i] = s_mat_view->ter_bucket[i];
        return n;
    }
    return terrain_bucket_list_live(out, cap);
}

/* ------------------------------------------------------------------ */
/* Per-present view. */

/* Per-scene snapshot of the interned per-instance slots (see the ring in
 * native_render.c): slots 1 up to the live count, the static slot of every
 * material index, and the terrain bucket list. Each slot's bytes sit in a
 * fixed OPENPETE_MATERIAL_UNIFORMS_MAX array, so the compose indexes them
 * the same way as the live table. */
void openpete_mods_material_view_capture(openpete_mods_material_view_t* out) {
    out->slot_n = s_mat_slot_n;
    for (int i = 1; i < s_mat_slot_n; i++) {
        out->slots[i].mat = s_mat_slots[i].mat;
        out->slots[i].len = s_mat_slots[i].len;
        memcpy(out->slots[i].bytes, s_mat_slots[i].bytes, s_mat_slots[i].len);
    }
    for (int i = 0; i < MOD_MATERIAL_MAX; i++) {
        out->static_slot[i] = s_mats[i].static_slot;
        /* A refine outcome belongs to the view only when this present
         * computed it; 0 marks none. */
        out->ter_refine[i] = (i < s_mat_count && s_mats[i].per_instance &&
                              s_mats[i].refine_present == s_mat_present)
                                 ? s_mats[i].refine_slot : 0;
    }
    out->level = s_cur_level;
    /* The live list, whatever view the compose thread has installed. */
    out->ter_bucket_n = terrain_bucket_list_live(out->ter_bucket, OPENPETE_MODS_MATERIAL_VIEW_MATS);
}
void openpete_mods_material_view_set(const openpete_mods_material_view_t* v) { s_mat_view = v; }
const openpete_mods_material_view_t* openpete_mods_material_view_get(void) { return s_mat_view; }

const void* openpete_mods_material_slot_uniforms(int slot, uint32_t* len) {
    if (slot <= 0 || slot >= s_mat_slot_n) { if (len) *len = 0; return NULL; }
    if (len) *len = s_mat_slots[slot].len;
    return s_mat_slots[slot].bytes;
}

/* ------------------------------------------------------------------ */
/* Moby extraction. */

/** @brief Test the moby extraction predicate: whether a material may win a moby class.
 *
 * Live, user-enabled materials of enabled mods on channel -1 or
 * OP_CHAN_MOBY whose class key is -1 or `cls`. A texture key or an env,
 * HP-tier, LP-tier or untextured flag excludes the material, since those
 * refine terrain only. The level key, the contract and the identity are
 * not tested.
 * @return 1 when the material is a candidate. */
static int material_moby_candidate(const mod_material_t* m, int cls) {
    if (!m->live || !m->user_enabled ||
        !openpete_mods_is_enabled(m->owner)) return 0;
    if (m->sel.channel != -1 && m->sel.channel != OP_CHAN_MOBY) return 0;
    if (m->sel.moby_class != -1 && m->sel.moby_class != cls) return 0;
    if (m->sel.texture != -1) return 0;   /* texture keys are terrain-only */
    if (m->sel.flags & (OP_SEL_ENV_ANIM | OP_SEL_HP_TIER | OP_SEL_LP_TIER |
                        OP_SEL_UNTEXTURED))
        return 0;   /* terrain-only refinements never match a moby */
    return 1;
}

/** @brief Return the winning material for a moby class, or -1.
 *
 * Among the materials material_moby_candidate() accepts, the highest
 * ranked (mod_mat_outranks) wins. */
static int material_moby_winner(int cls) {
    int best = -1;
    for (int i = 0; i < s_mat_count; i++) {
        if (material_moby_candidate(&s_mats[i], cls) && mod_mat_outranks(i, best)) best = i;
    }
    return best;
}

/** @brief Census: count a moby decline that another candidate for the class could have taken.
 *
 * @param w The declining material.
 * @param cls Moby class. */
static void census_moby_decline(int w, int cls) {
    for (int i = 0; i < s_mat_count; i++)
        if (i != w && material_moby_candidate(&s_mats[i], cls)) {
            cen_bump(w, MOD_MAT_CENSUS_DECLINE_SHADOWS);
            return;
        }
}

/** @brief Intern a material's uniform block into the slot table, reusing an identical outcome.
 *
 * Identical (material, bytes) outcomes share a slot so they stay in ONE
 * batch; the scan is linear since the interned set is small.
 * @param w Material index.
 * @param buf The block.
 * @param len Bytes in @p buf.
 * @return The slot, or -1 when the table is full. */
static int slot_intern(int w, const unsigned char* buf, int len) {
    for (int s = 1; s < s_mat_slot_n; s++)
        if (s_mat_slots[s].mat == w && s_mat_slots[s].len == (uint32_t)len &&
            memcmp(s_mat_slots[s].bytes, buf, (size_t)len) == 0)
            return s;
    if (s_mat_slot_n >= MOD_MATERIAL_SLOTS) return -1;
    int s = s_mat_slot_n++;
    s_mat_slots[s].mat = w;
    s_mat_slots[s].len = (uint32_t)len;
    memcpy(s_mat_slots[s].bytes, buf, (size_t)len);
    return s;
}

int openpete_mods_material_moby_slot(CPUState* cpu, int moby_class,
                                     uint32_t moby_vaddr) {
    if (!s_mat_has_instance) return 0;
    if (mod_mat_census_on() && moby_class >= 0 && moby_class < CEN_CLASSES)
        atomic_fetch_add_explicit(&s_cen_class[moby_class], 1u, memory_order_relaxed);
    int w = material_moby_winner(moby_class);
    cen_bump(w, MOD_MAT_CENSUS_EXTRACT_WINS);
    if (w < 0 || !s_mats[w].per_instance) return 0;   /* declarative -> draw-time */

    (void)cpu;   /* refine reads guest RAM through the mod api's accessors */
    openpete_mod_material_key_t key;
    memset(&key, 0, sizeof key);
    key.struct_size    = (uint32_t)sizeof key;
    key.version        = OPENPETE_MATERIAL_KEY_VER_MOBY;
    key.channel        = OP_CHAN_MOBY;
    key.u.moby.vaddr   = moby_vaddr;
    key.u.moby.moby_class = moby_class;
    unsigned char buf[OPENPETE_MATERIAL_UNIFORMS_MAX];
    openpete_mods_guard_enter(s_mats[w].owner);
    int rc = s_mats[w].per_instance(&key, buf, sizeof buf);
    openpete_mods_guard_leave();
    cen_bump(w, MOD_MAT_CENSUS_REFINE_CALLS);
    if (rc < 0) {
        cen_bump(w, MOD_MAT_CENSUS_DECLINES);
        if (mod_mat_census_on()) census_moby_decline(w, moby_class);
        return OPENPETE_MATERIAL_SLOT_STOCK;   /* declined -> stock */
    }
    if (rc > (int)sizeof buf) rc = (int)sizeof buf;
    int s = slot_intern(w, buf, rc);
    return s < 0 ? OPENPETE_MATERIAL_SLOT_STOCK : s;   /* table full -> graceful stock */
}

/* ------------------------------------------------------------------ */
/* Walked terrain refines. */

/** @brief Return the winning per-instance terrain material for one refine key, or -1.
 *
 * The walked path's refine winner. Considers the per-instance materials
 * terrain_sel_usable() accepts for @p level (the bake's predicate: live,
 * enabled, SCENE contract, terrain channel, level key) whose one key is
 * the face's (terrain_refine_key_matches); the highest ranked
 * (mod_mat_outranks) wins. A texture-less refine without OP_SEL_UNTEXTURED
 * has no key and never wins.
 * @param untex Nonzero for the untextured band (the beyond-LOD gouraud terrain).
 * @param texid The Tiledef id of a textured face.
 * @param level The level the key resolves in. */
static int material_terrain_refine_winner(int untex, int texid, int level) {
    int best = -1;
    for (int i = 0; i < s_mat_count; i++) {
        const mod_material_t* m = &s_mats[i];
        if (!m->per_instance || !terrain_sel_usable(m, level)) continue;
        if (terrain_refine_key_matches(m, untex, texid) && mod_mat_outranks(i, best)) best = i;
    }
    return best;
}

/** @brief Census: classify a terrain refine call against the other usable terrain materials on its key.
 *
 * Counts the call as outranked when a usable declarative material ranked
 * above it (mod_mat_outranks) matches the refine's key on some face (texture key -1
 * or equal, texturedness compatible; env and tier are face keys the
 * refine key does not carry), and a decline as shadowing when any other
 * usable material matches that key.
 * @param w The refine material.
 * @param declined Nonzero when the callback returned -1. */
static void census_terrain_refine(int w, int declined) {
    const mod_material_t* r = &s_mats[w];
    const int untex = (r->sel.flags & OP_SEL_UNTEXTURED) != 0;
    const int level = openpete_mods_current_level();
    int outranked = 0, other = 0;
    for (int i = 0; i < s_mat_count; i++) {
        const mod_material_t* m = &s_mats[i];
        if (i == w || !terrain_sel_usable(m, level)) continue;
        if (m->sel.texture != -1 && (untex || m->sel.texture != r->sel.texture)) continue;
        if ((m->sel.flags & OP_SEL_TEXTURED) && untex) continue;
        if ((m->sel.flags & OP_SEL_UNTEXTURED) && !untex) continue;
        other = 1;
        if (!m->per_instance && mod_mat_outranks(i, w)) outranked = 1;
    }
    if (outranked) cen_bump(w, MOD_MAT_CENSUS_OUTRANKED);
    if (declined && other) cen_bump(w, MOD_MAT_CENSUS_DECLINE_SHADOWS);
}

/* The refine outcome of one per-instance terrain material, computed once
 * per present on the sim thread. A terrain refine selector carries one key,
 * a Tiledef id or the untextured band, so the material's outcome is the
 * outcome of every face it reaches and one memo per material serves the
 * walked and the retained terrain alike. The memo is keyed on the present
 * counter openpete_mods_material_frame_reset() advances; the view captures
 * it for the compose (mod_mat_terrain_bucket_slot). The callback runs inside
 * a guarded stretch owned by the material's mod and receives a
 * terrain key with the selector's texture (-1 for the untextured band);
 * its block, capped at OPENPETE_MATERIAL_UNIFORMS_MAX, reuses an interned
 * slot of the same material with identical bytes, else takes a new slot. */
int mod_mat_terrain_refine_slot(int w) {
    if (w < 0 || w >= s_mat_count) return 0;
    mod_material_t* m = &s_mats[w];
    if (!m->per_instance) return 0;
    if (m->refine_present == s_mat_present) return m->refine_slot;
    m->refine_present = s_mat_present;
    m->refine_slot = OPENPETE_MATERIAL_TERRAIN_STOCK;

    const int untex = (m->sel.flags & OP_SEL_UNTEXTURED) != 0;
    openpete_mod_material_key_t key;
    memset(&key, 0, sizeof key);
    key.struct_size       = (uint32_t)sizeof key;
    key.version           = OPENPETE_MATERIAL_KEY_VER_TERRAIN;
    key.channel           = OP_CHAN_TERRAIN;
    key.u.terrain.texture = untex ? -1 : m->sel.texture;
    key.u.terrain.flags   = untex ? OP_SEL_UNTEXTURED : m->sel.flags;
    unsigned char buf[OPENPETE_MATERIAL_UNIFORMS_MAX];
    openpete_mods_guard_enter(m->owner);
    int rc = m->per_instance(&key, buf, sizeof buf);
    openpete_mods_guard_leave();
    cen_bump(w, MOD_MAT_CENSUS_REFINE_CALLS);
    if (rc < 0) cen_bump(w, MOD_MAT_CENSUS_DECLINES);
    if (mod_mat_census_on()) census_terrain_refine(w, rc < 0);
    if (rc < 0) return m->refine_slot;          /* declined -> stock */
    if (rc > (int)sizeof buf) rc = (int)sizeof buf;
    int s = slot_intern(w, buf, rc);
    if (s < 0) return m->refine_slot;           /* table full -> graceful stock */
    return (m->refine_slot = s);
}

/* Per-instance material slot for one textured terrain Tiledef id. */
int openpete_mods_material_terrain_tex_slot(CPUState* cpu, int texid) {
    if (!s_mat_has_instance) return 0;
    (void)cpu;
    return mod_mat_terrain_refine_slot(material_terrain_refine_winner(0, texid, s_cur_level));
}

/* Per-instance material slot for the untextured terrain band. */
int openpete_mods_material_terrain_untex_slot(CPUState* cpu) {
    if (!s_mat_has_instance) return 0;
    (void)cpu;
    return mod_mat_terrain_refine_slot(material_terrain_refine_winner(1, -1, s_cur_level));
}

void openpete_mods_material_terrain_refine_present(void) {
    if (!s_mat_has_instance) return;
    const int level = s_cur_level;
    /* The refines a retained face draws through: the walk asks the ones it
     * reaches itself, and a refine whose faces are all retained has no
     * walked face to ask it. A refine asked here wins its own key among
     * the refines (as the walk would pick it) and won a face key at the
     * bake (no higher declarative took every face of its key), so an
     * outranked refine is never asked. */
    for (int i = 0; i < s_mat_count && i + 1 <= OPENPETE_MATERIAL_TER_BUCKET_MAX; i++) {
        const mod_material_t* m = &s_mats[i];
        if (!m->per_instance || !m->bake_won || !terrain_sel_usable(m, level)) continue;
        const int untex = (m->sel.flags & OP_SEL_UNTEXTURED) != 0;
        if (material_terrain_refine_winner(untex, m->sel.texture, level) != i) continue;
        (void)mod_mat_terrain_refine_slot(i);
    }
}

int mod_mat_terrain_bucket_slot(int w) {
    if (w < 0 || w >= s_mat_count) return OPENPETE_MATERIAL_TERRAIN_STOCK;
    const mod_material_t* m = &s_mats[w];
    int slot = 0;
    if (s_mat_view) slot = s_mat_view->ter_refine[w];
    else if (m->refine_present == s_mat_present) slot = m->refine_slot;
    if (slot <= 0) {
        /* A bucket no baked face carries draws nothing; only one that
         * does misses an outcome. */
        if (m->bake_won) cen_bump(w, MOD_MAT_CENSUS_STALE_MEMO);
        return OPENPETE_MATERIAL_TERRAIN_STOCK;
    }
    return slot;
}

/* ------------------------------------------------------------------ */
/* Draw-time resolve. */

/** @brief Test whether an interned slot's material is still live and its owner enabled.
 *
 * An interned slot can outlive its material's usability: extraction
 * interns the slot, the tick boundary (live-toggle apply and hot-reload
 * sweep) runs next, and only then does the present draw. A disabled owner
 * must stop that present. A swept material has freed SPIR-V and a
 * retired pipeline slot no later sweep revisits, so a rebuild through it
 * would cache a pipeline nothing destroys. Both cases draw stock.
 * @param slot Slot index in the current view.
 * @return Nonzero when usable. */
static int material_slot_usable(int slot) {
    int m = view_slot_mat(slot);
    return m >= 0 && m < s_mat_count && s_mats[m].live &&
           openpete_mods_is_enabled(s_mats[m].owner);
}

/* Draw-time resolve (native_render draw_tex_runs): the run's provenance tag ->
 * the winning override, or -1. Tag layout: [31:28] path+1; moby class
 * [15:0] + per-instance slot [27:16]; terrain env bit 27 / LP
 * bit 26 + sector [8:0]. Rank: mod_mat_outranks. */
/* The selector keys a tag carries, decoded once. `path` is 0 moby,
 * 1 terrain; the other members are -1 when the tag's path does not carry
 * that key. */
typedef struct {
    int path;
    int cls;     /* moby class */
    int tex;     /* terrain texture id */
    int chan;    /* raw path nibble: the feeder's channel */
    int ident;   /* channel-local identity */
    int level;
    int env, lp, untex;
} material_key_t;

/** @brief Decode a provenance tag into the selector keys it carries.
 *
 * Bits 31:28 hold the path plus one (a nibble of 0 gives path -1), and the
 * channel is that raw nibble. Identity is bits 15:0 for every path. A moby
 * tag (path 0) carries its class in bits 15:0. A terrain tag (path 1)
 * carries the env-rewrite flag in bit 27, the LP-tier flag in bit 26, the
 * untextured flag in bit 25, and, when textured, the Tiledef id in bits
 * 15:9. The level is the installed view's, captured with the scene, or with
 * no view installed the one last pushed.
 *
 * @param tag Provenance tag.
 * @param k Receives the decoded keys; a key the path does not carry is -1
 *        or 0. */
static void material_key_from_tag(uint32_t tag, material_key_t* k) {
    k->path  = (int)(tag >> 28) - 1;
    k->untex = (k->path == 1) && (tag & 0x02000000u);
    k->cls   = (k->path == 0) ? (int)(tag & 0xFFFFu) : -1;
    k->env   = (k->path == 1) && (tag & 0x08000000u);
    k->lp    = (k->path == 1) && (tag & 0x04000000u);
    k->tex   = (k->path == 1 && !k->untex) ? (int)((tag >> 9) & 0x7Fu) : -1;
    /* Channel == the raw path nibble. The other feeders (player, flame,
     * particle, sky, shadow, tracer) all carry their identity in [15:0],
     * so one comparison serves every one of them. */
    k->chan  = (int)(tag >> 28);
    k->ident = (int)(tag & 0xFFFFu);
    k->level = s_mat_view ? s_mat_view->level : openpete_mods_current_level();
}

/** @brief Read the interned material slot a tag names directly.
 *
 * A moby tag carries a per-instance slot in bits [27:16]
 * (OPENPETE_MATERIAL_SLOT_STOCK = declined); a terrain tag carries a
 * per-present slot in bits [24:16] (OPENPETE_MATERIAL_TERRAIN_STOCK =
 * declined). Slot 0 defers to the declarative resolve. A slot at or past
 * the view's slot count is stale, and one whose material is swept or whose
 * owner is disabled no longer resolves.
 * @param tag Provenance tag.
 * @param path 0 moby, 1 terrain; other paths carry no slot.
 * @param slot Receives the slot on a hit.
 * @return 1 with the slot, 0 to fall through to the declarative resolve,
 *         -1 when the slot declines the draw or no longer resolves. */
static int material_slot_from_tag(uint32_t tag, int path, int* slot) {
    int s;
    if (path == 0) {
        s = (int)((tag >> 16) & 0xFFFu);
        if (s == OPENPETE_MATERIAL_SLOT_STOCK) return -1;    /* declined */
    } else if (path == 1) {
        s = (int)((tag >> 16) & 0x1FFu);
        if (s == OPENPETE_MATERIAL_TERRAIN_STOCK) return -1; /* declined */
    } else {
        return 0;
    }
    if (s == 0) return 0;                        /* declarative resolve */
    if (s >= view_slot_n()) return -1;           /* stale (post-reset) */
    if (!material_slot_usable(s)) return -1;     /* toggled/swept */
    *slot = s;
    return 1;
}

/** @brief Test whether a material's selector accepts a decoded tag key.
 *
 * Each selector field narrows only when set: channel, level (inert while
 * no level has been pushed), identity (not on terrain, whose [15:0] holds
 * sector and texture id), moby class (moby path only), texture (textured
 * terrain only), and the env, HP-tier, LP-tier, textured and untextured
 * flags. Setting neither textured nor untextured selects both.
 * @return 1 when accepted, else 0. */
static int material_selector_matches(const mod_material_t* m,
                                     const material_key_t* k) {
    if (m->sel.channel != -1 && m->sel.channel != k->chan) return 0;
    if (m->sel.level != -1 && k->level >= 0 && m->sel.level != k->level)
        return 0;
    /* Identity is meaningful for every channel except terrain, whose
     * [15:0] is sector|texid rather than a channel-local id and which
     * has its own keys below. */
    if (m->sel.ident != -1 && k->path != 1 && m->sel.ident != k->ident)
        return 0;
    if (k->path == 0 && m->sel.moby_class != -1 &&
        m->sel.moby_class != k->cls)
        return 0;
    /* Texture key: terrain textured faces only. */
    if (m->sel.texture != -1 && (k->tex < 0 || m->sel.texture != k->tex))
        return 0;
    if ((m->sel.flags & OP_SEL_ENV_ANIM) && !k->env) return 0;
    if ((m->sel.flags & OP_SEL_HP_TIER) && k->lp) return 0;
    if ((m->sel.flags & OP_SEL_LP_TIER) && !k->lp) return 0;
    /* Texturedness narrows: setting neither member selects both, so an
     * unqualified terrain selector reaches the gouraud faces (the LP
     * tier and the beyond-LOD band) as well as the textured ones. */
    if ((m->sel.flags & OP_SEL_TEXTURED) && k->untex) return 0;
    if ((m->sel.flags & OP_SEL_UNTEXTURED) && !k->untex) return 0;
    return 1;
}

/** @brief Return the declarative material that wins a decoded tag key, or -1.
 *
 * Considers live, user-enabled materials of enabled mods with the shading
 * contract @p contract and without a per-instance callback, which resolve
 * through slots only; among those material_selector_matches accepts, the
 * highest ranked (mod_mat_outranks) wins.
 * @param k The decoded tag key.
 * @param contract The draw site's OPENPETE_MATERIAL_CONTRACT_*. */
static int material_winner(const material_key_t* k, int contract) {
    int best = -1;
    for (int i = 0; i < s_mat_count; i++) {
        mod_material_t* m = &s_mats[i];
        if (!m->live || !m->user_enabled || m->contract != contract ||
            !openpete_mods_is_enabled(m->owner)) continue;
        if (m->per_instance) continue;   /* per-instance winners resolve via slots only */
        if (material_selector_matches(m, k) && mod_mat_outranks(i, best)) best = i;
    }
    return best;
}

/** @brief Resolve a walked terrain tag whose refine slot is set, ranking the refine against the declarative winner.
 *
 * The tag carries the outcome of the walked refine winner for its texid
 * or untextured band; the declarative winner depends on the tag's env,
 * tier and texturedness as well. A claimed slot (@p claimed 1) keeps its
 * material unless the declarative winner outranks it. A declined refine
 * (@p claimed 0, the tag holds OPENPETE_MATERIAL_TERRAIN_STOCK) is
 * re-derived from the tag's keys: the draw is stock when that refine still
 * outranks the declarative winner, else the declarative winner draws.
 * @param k The decoded terrain key.
 * @param contract The draw site's OPENPETE_MATERIAL_CONTRACT_*.
 * @param claimed 1 for a claimed slot, 0 for a decline.
 * @param slot In: the claimed slot; out: the uniform slot, 0 for a
 *        declarative material's static block.
 * @return The material index, or -1 for stock. */
static int terrain_tag_resolve(const material_key_t* k, int contract, int claimed, int* slot) {
    const int d = material_winner(k, contract);
    const int r = claimed ? view_slot_mat(*slot)
                          : material_terrain_refine_winner(k->untex, k->tex, k->level);
    if (r >= 0 && mod_mat_outranks(r, d)) {
        if (!claimed) return -1;                       /* the refine's decline stands */
        return s_mats[r].contract == contract ? r : -1;
    }
    *slot = 0;
    return d;
}

/** @brief Census: record a draw-time outcome per material and, for terrain tags, per face key.
 *
 * @param k The decoded key.
 * @param best The resolved material, or -1 for stock. */
static void census_resolve(const material_key_t* k, int best) {
    if (!mod_mat_census_on()) return;
    cen_bump(best, MOD_MAT_CENSUS_DRAWS);
    if (k->path == 1)
        cen_ter(s_cen_walk_n, s_cen_walk_b, k->untex ? -1 : k->tex, k->env, k->lp,
                best < 0 ? 0 : best + 1);
}

int mod_mat_resolve_index(uint32_t tag, int contract, int* slot) {
    material_key_t k;
    int best;
    *slot = 0;
    if (!tag || s_mat_count == 0) return -1;
    if (s_mat_view && mod_mat_census_on())
        atomic_fetch_add_explicit(&s_cen_view_resolves, 1u, memory_order_relaxed);
    material_key_from_tag(tag, &k);
    const int from_tag = material_slot_from_tag(tag, k.path, slot);
    if (k.path == 1 && (from_tag == 1 ||
                        (from_tag == -1 && ((tag >> 16) & 0x1FFu) == OPENPETE_MATERIAL_TERRAIN_STOCK))) {
        /* Walked terrain: the refine slot or decline ranks against the
         * declarative winner of the tag's keys. A stale or unusable slot
         * stays stock below. */
        best = terrain_tag_resolve(&k, contract, from_tag == 1, slot);
        if (best >= 0 && mod_owner_held(s_mats[best].owner)) best = -1;
        census_resolve(&k, best);
        return best;
    }
    switch (from_tag) {
    case 1:
        /* A slot whose material has another contract draws stock. */
        best = view_slot_mat(*slot);
        if (s_mats[best].contract != contract || mod_owner_held(s_mats[best].owner)) best = -1;
        census_resolve(&k, best);
        return best;
    case -1: census_resolve(&k, -1); return -1;
    default: break;   /* declarative resolve */
    }
    *slot = 0;
    best = material_winner(&k, contract);
    if (best >= 0 && mod_owner_held(s_mats[best].owner)) best = -1;
    census_resolve(&k, best);
    return best;
}
