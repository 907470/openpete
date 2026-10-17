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
 * Asked once per tick: the game's render of a tick opens its scene with
 * openpete_mods_material_frame_reset(1), and the re-extracts that rebuild
 * the scene for the tick's sub-tick presents replay the outcomes the tick
 * computed (moby walk outcomes per (address, class), terrain refine
 * outcomes per material) instead of calling the callbacks again. A
 * callback reads tick-stable guest RAM through the mod api, which bypasses
 * the sub-tick view, so a replay equals a second call. Any change to the
 * material table, the mod toggles or the level drops the kept outcomes.
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
static void overlap_reset(void);
void mod_mat_reset(void) {
    memset(s_mats, 0, sizeof s_mats);
    s_mat_count = 0;
    s_mat_sel_dirty = 0;
    s_mat_has_instance = 0;
    reset_level_and_view();
    overlap_reset();
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
/* The moby field's declarative landing is OPENPETE_MATERIAL_SLOT_DECL |
 * material index: every index and every slot id stays below the bit, and
 * the largest landing below OPENPETE_MATERIAL_SLOT_STOCK. */
_Static_assert(MOD_MATERIAL_MAX - 1 < 0x7FF, "material index fits the declarative landing");
_Static_assert(MOD_MATERIAL_SLOTS <= OPENPETE_MATERIAL_SLOT_DECL, "slot ids stay below the declarative bit");
_Static_assert((OPENPETE_MATERIAL_SLOT_DECL | (MOD_MATERIAL_MAX - 1)) < OPENPETE_MATERIAL_SLOT_STOCK,
               "no declarative landing aliases the stock outcome");

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
static _Atomic unsigned s_cen_memo_replays;    /* walk and terrain outcomes replayed from the tick's memo */
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
    atomic_store_explicit(&s_cen_memo_replays, 0u, memory_order_relaxed);
}
unsigned mod_mat_census_view_resolves(void) {
    return atomic_load_explicit(&s_cen_view_resolves, memory_order_relaxed);
}
unsigned mod_mat_census_memo_replays(void) {
    return atomic_load_explicit(&s_cen_memo_replays, memory_order_relaxed);
}
/** @brief Count one outcome replayed from the tick's memo when the census is on. */
static void cen_memo_replay(void) {
    if (mod_mat_census_on()) atomic_fetch_add_explicit(&s_cen_memo_replays, 1u, memory_order_relaxed);
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
    fprintf(stderr, "[material-census] view-installed resolves=%u tick-memo replays=%u\n",
            atomic_load_explicit(&s_cen_view_resolves, memory_order_relaxed),
            atomic_load_explicit(&s_cen_memo_replays, memory_order_relaxed));
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
/* Moves on every material table change (mod_mat_rearm_split) and level
 * change; the tick memo is void once it moves. */
static unsigned s_mat_gen;

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
    s_mat_gen++;   /* every table change re-arms: the tick's kept outcomes are void */
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

int mod_mat_refine_valid(const char* id, const openpete_mod_material_selector_t* sel,
                         int contract) {
    if (sel->channel != -1 && sel->channel != OP_CHAN_MOBY && sel->channel != OP_CHAN_TERRAIN) {
        OPL_ERROR_F("mods", "%s: material: a refine callback takes effect on the moby and "
                    "terrain channels only, not channel %d - refused", id, sel->channel);
        return 0;
    }
    if (contract != OPENPETE_MATERIAL_CONTRACT_SCENE) {
        OPL_ERROR_F("mods", "%s: material: a refine callback needs a scene-contract shader; "
                    "a decal material resolves on shadow draws, which take declarative "
                    "materials only - refused", id);
        return 0;
    }
    if (sel->channel == OP_CHAN_TERRAIN && sel->texture == -1 &&
        !(sel->flags & (unsigned)OP_SEL_UNTEXTURED)) {
        OPL_ERROR_F("mods", "%s: material: a terrain refine is keyed on one texture id or on "
                    "OP_SEL_UNTEXTURED; this selector has neither - refused", id);
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
    mod_mat_overlap_bump();
    OPL_INFO_F("mods", "%s: material selector armed (channel %d ident %d class %d "
               "flags %#x%s)", id, sel->channel, sel->ident, sel->moby_class, sel->flags,
               per_instance ? ", per-instance" : "");
    return handle;
}

int mod_mat_enable(int material, int on) {
    if (material < 0 || material >= s_mat_count || !s_mats[material].live)
        return -1;
    if (s_mats[material].user_enabled != (on ? 1 : 0)) mod_mat_overlap_bump();
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

/** @brief Install a selector on a material; a change of its bytes restarts the refine observation and bumps the overlap inputs.
 *
 * A retune to the bytes already installed changes nothing.
 * @param m The material.
 * @param sel The new selector. */
static void sel_store(mod_material_t* m, const openpete_mod_material_selector_t* sel) {
    if (memcmp(&m->sel, sel, sizeof *sel) == 0) return;
    m->sel = *sel;
    m->obs_calls = m->obs_declines = 0;
    mod_mat_overlap_bump();
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
    if (m->per_instance && !mod_mat_refine_valid(id, sel, m->contract)) return -1;
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
    sel_store(m, sel);
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
        sel_store(&s_mats[i], &s_mats[i].sel_pending);
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
        s_cur_level = level_id; s_mat_ter_epoch++; s_mat_gen++;
    }
}

/** @brief Clear the pushed level, the slot table, the installed view and the hold list for mod_mat_reset. */
static void reset_level_and_view(void) {
    s_cur_level = -1;
    s_mat_gen++;
    s_mat_slot_n = 1;
    s_mat_view = NULL;
    s_hold_n = 0;
}

/** @brief Return the level id the game last pushed, or -1 before any push. */
static int openpete_mods_current_level(void) { return s_cur_level; }

/* Bumped every present, so a per-present memo (the terrain refine slot) can
 * tell "computed this present" from "computed under the last one". */
static unsigned s_mat_present = 1;
/* Bumped when the game's render opens a new tick's scene. */
static unsigned s_mat_tick = 1;

/* The tick memo: refine outcomes kept from the first present of a tick for
 * the re-extracts of that tick. It is current while the tick, the table
 * generation and the mod toggle epoch it was opened under still hold;
 * s_memo_stamp names that state, and an entry stamped with another value
 * is empty. */
static unsigned s_memo_tick, s_memo_gen, s_memo_toggle;
static unsigned s_memo_stamp = 1;
/* Accepted refine blocks of the tick, deduplicated, for the moby memo to
 * re-intern on replay. */
static mat_slot_t s_tick_out[MOD_MATERIAL_SLOTS];
static int s_tick_out_n;
/* Moby walk outcomes of the tick by (address, class): open addressing,
 * filled to at most three quarters. A value below MEMO_REFINE is the tag
 * field to stamp; MEMO_REFINE | k names s_tick_out[k]. */
#define MOBY_MEMO_N 2048
#define MEMO_REFINE 0x10000
typedef struct { uint32_t vaddr; int cls; unsigned stamp; int out; } moby_memo_t;
static moby_memo_t s_moby_memo[MOBY_MEMO_N];
static int s_moby_memo_n;

/** @brief Return the stamp of the current tick memo, opening a new memo when the tick, table or toggles moved.
 *
 * @return The stamp an entry of the current memo carries. */
static unsigned memo_stamp(void) {
    const unsigned toggle = openpete_mods_toggle_epoch();
    if (s_memo_tick != s_mat_tick || s_memo_gen != s_mat_gen || s_memo_toggle != toggle) {
        s_memo_tick = s_mat_tick; s_memo_gen = s_mat_gen; s_memo_toggle = toggle;
        s_memo_stamp++;
        s_tick_out_n = 0;
        s_moby_memo_n = 0;
    }
    return s_memo_stamp;
}

void openpete_mods_material_frame_reset(int tick_open) {
    s_mat_slot_n = 1;
    s_mat_present++;
    if (tick_open) s_mat_tick++;
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

/** @brief Rank two materials from their table indices, owners' load indices and the first one's api pin.
 *
 * @param a Index of the first material.
 * @param load_a Its owner's load index.
 * @param pin_a Its owner's api pin.
 * @param b Index of the second material.
 * @param load_b Its owner's load index.
 * @return 1 when the first ranks above the second. */
static int rank_above(int a, int load_a, int pin_a, int b, int load_b) {
    if (load_a != load_b) return load_a > load_b;
    /* One mod: registration order, its direction selected by the pin. */
    return pin_a >= MOD_MAT_API_LATER_TIE ? a > b : a < b;
}

int mod_mat_outranks(int a, int b) {
    if (b < 0) return 1;
    return rank_above(a, s_mats[a].load_index, s_mats[a].api_pin, b, s_mats[b].load_index);
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

/** @brief Test the one moby predicate: whether a material may shade a moby of a class.
 *
 * Shared by the extraction walk and the draw-time resolve of a moby tag.
 * Live, user-enabled materials of enabled mods with the shading contract
 * @p contract, on channel -1 or OP_CHAN_MOBY, whose class key and identity
 * are -1 or @p cls (a moby's identity is its class) and whose level key is
 * -1 or @p level (every level key matches while @p level is negative). A
 * texture key or an env, HP-tier, LP-tier or untextured flag excludes the
 * material, since those keys exist on terrain only; OP_SEL_TEXTURED
 * accepts, since moby tags carry no texturedness. The owner hold is not
 * tested: the extraction runs outside the compose that holds owners.
 * @param m The material.
 * @param cls Moby class.
 * @param level The level the moby is drawn in, or -1.
 * @param contract The OPENPETE_MATERIAL_CONTRACT_* of the draw site.
 * @return 1 when the material matches. */
static int material_matches_moby(const mod_material_t* m, int cls, int level, int contract) {
    if (!m->live || !m->user_enabled || m->contract != contract ||
        !openpete_mods_is_enabled(m->owner)) return 0;
    if (m->sel.channel != -1 && m->sel.channel != OP_CHAN_MOBY) return 0;
    if (m->sel.level != -1 && level >= 0 && m->sel.level != level) return 0;
    if (m->sel.ident != -1 && m->sel.ident != cls) return 0;
    if (m->sel.moby_class != -1 && m->sel.moby_class != cls) return 0;
    if (m->sel.texture != -1) return 0;
    return (m->sel.flags & (OP_SEL_ENV_ANIM | OP_SEL_HP_TIER | OP_SEL_LP_TIER |
                            OP_SEL_UNTEXTURED)) == 0;
}

/** @brief Return the highest-ranked scene material matching a moby that ranks below a given one, or -1.
 *
 * One step of the extraction walk down the priority order.
 * @param cls Moby class.
 * @param level The pushed level.
 * @param above The material the step continues below, or -1 for the top.
 * @return The material index, or -1 when the walk is exhausted. */
static int moby_walk_next(int cls, int level, int above) {
    int best = -1;
    for (int i = 0; i < s_mat_count; i++) {
        if (above >= 0 && !mod_mat_outranks(above, i)) continue;
        if (material_matches_moby(&s_mats[i], cls, level, OPENPETE_MATERIAL_CONTRACT_SCENE) &&
            mod_mat_outranks(i, best))
            best = i;
    }
    return best;
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

/** @brief Keep an accepted refine block in the tick's outcome list, reusing an identical entry.
 *
 * @param w Material index.
 * @param buf The block.
 * @param len Bytes in @p buf.
 * @return The entry index, or -1 when the list is full. */
static int tick_out_keep(int w, const unsigned char* buf, int len) {
    for (int k = 0; k < s_tick_out_n; k++)
        if (s_tick_out[k].mat == w && s_tick_out[k].len == (uint32_t)len &&
            memcmp(s_tick_out[k].bytes, buf, (size_t)len) == 0)
            return k;
    if (s_tick_out_n >= MOD_MATERIAL_SLOTS) return -1;
    const int k = s_tick_out_n++;
    s_tick_out[k].mat = w;
    s_tick_out[k].len = (uint32_t)len;
    memcpy(s_tick_out[k].bytes, buf, (size_t)len);
    return k;
}

/** @brief Find a moby's entry in the tick memo, or the empty entry it would take.
 *
 * @param stamp The current memo stamp.
 * @param vaddr Guest address of the moby.
 * @param cls Moby class.
 * @return The entry, or NULL when the memo holds no entry for the moby and
 *         is full. */
static moby_memo_t* moby_memo_find(unsigned stamp, uint32_t vaddr, int cls) {
    unsigned h = (vaddr >> 2) * 2654435761u ^ (unsigned)cls;
    for (int n = 0; n < MOBY_MEMO_N; n++, h++) {
        moby_memo_t* e = &s_moby_memo[h & (MOBY_MEMO_N - 1)];
        if (e->stamp != stamp) return s_moby_memo_n < MOBY_MEMO_N * 3 / 4 ? e : NULL;
        if (e->vaddr == vaddr && e->cls == cls) return e;
    }
    return NULL;
}

/* Advanced on the first call and the first decline of a moby refine, the
 * two events that move its accept-all observation. Sim thread only. */
static unsigned s_ovl_obs;

/** @brief Count one moby refine call and its decline for the overlap diagnostic's accept-all observation.
 *
 * @param m The refine material.
 * @param declined Nonzero when the callback returned -1. */
static void refine_observe(mod_material_t* m, int declined) {
    if (m->obs_calls++ == 0) s_ovl_obs++;
    if (declined && m->obs_declines++ == 0) s_ovl_obs++;
}

/** @brief Run the extraction walk for one moby: the first material down the priority order that takes it.
 *
 * A declarative material lands; a refine material is asked, inside a
 * guarded stretch owned by its mod, and lands on acceptance; its -1 passes
 * the moby on. A decline followed by another candidate counts as a
 * fall-through in the census.
 * @param cls Moby class.
 * @param vaddr Guest address of the moby.
 * @param buf Receives an accepting refine's block, OPENPETE_MATERIAL_UNIFORMS_MAX bytes.
 * @param len Receives the block's byte count, or -1 for no refine landing.
 * @return The material index landed on, or -1 for an exhausted walk. */
static int moby_walk(int cls, uint32_t vaddr, unsigned char* buf, int* len) {
    const int level = s_cur_level;
    int declined = -1;
    *len = -1;
    for (int w = moby_walk_next(cls, level, -1); w >= 0; w = moby_walk_next(cls, level, w)) {
        if (declined >= 0) cen_bump(declined, MOD_MAT_CENSUS_FALLTHROUGHS);
        mod_material_t* m = &s_mats[w];
        if (!m->per_instance) { cen_bump(w, MOD_MAT_CENSUS_EXTRACT_WINS); return w; }
        openpete_mod_material_key_t key;
        memset(&key, 0, sizeof key);
        key.struct_size       = (uint32_t)sizeof key;
        key.version           = OPENPETE_MATERIAL_KEY_VER_MOBY;
        key.channel           = OP_CHAN_MOBY;
        key.u.moby.vaddr      = vaddr;
        key.u.moby.moby_class = cls;
        openpete_mods_guard_enter(m->owner);
        const int rc = m->per_instance(&key, buf, OPENPETE_MATERIAL_UNIFORMS_MAX);
        openpete_mods_guard_leave();
        cen_bump(w, MOD_MAT_CENSUS_REFINE_CALLS);
        refine_observe(m, rc < 0);
        if (rc >= 0) {
            cen_bump(w, MOD_MAT_CENSUS_EXTRACT_WINS);
            *len = rc > (int)OPENPETE_MATERIAL_UNIFORMS_MAX ? (int)OPENPETE_MATERIAL_UNIFORMS_MAX : rc;
            return w;
        }
        cen_bump(w, MOD_MAT_CENSUS_DECLINES);
        declined = w;
    }
    return -1;
}

/** @brief Turn a refine landing into this present's slot, or stock when the slot table is full.
 *
 * @param w Material index.
 * @param buf The accepted block.
 * @param len Bytes in @p buf.
 * @return The slot, or OPENPETE_MATERIAL_SLOT_STOCK. */
static int moby_refine_slot(int w, const unsigned char* buf, int len) {
    const int s = slot_intern(w, buf, len);
    return s < 0 ? OPENPETE_MATERIAL_SLOT_STOCK : s;
}

int openpete_mods_material_moby_slot(CPUState* cpu, int moby_class,
                                     uint32_t moby_vaddr) {
    (void)cpu;   /* refine reads guest RAM through the mod api's accessors */
    if (!s_mat_has_instance) return 0;
    if (mod_mat_census_on() && moby_class >= 0 && moby_class < CEN_CLASSES)
        atomic_fetch_add_explicit(&s_cen_class[moby_class], 1u, memory_order_relaxed);
    const unsigned stamp = memo_stamp();
    moby_memo_t* e = moby_memo_find(stamp, moby_vaddr, moby_class);
    if (e && e->stamp == stamp) {
        cen_memo_replay();
        if (!(e->out & MEMO_REFINE)) return e->out;
        const mat_slot_t* o = &s_tick_out[e->out & (MEMO_REFINE - 1)];
        return moby_refine_slot(o->mat, o->bytes, (int)o->len);
    }
    unsigned char buf[OPENPETE_MATERIAL_UNIFORMS_MAX];
    int len;
    const int w = moby_walk(moby_class, moby_vaddr, buf, &len);
    int out = OPENPETE_MATERIAL_SLOT_STOCK, keep;
    if (w >= 0 && len < 0) out = OPENPETE_MATERIAL_SLOT_DECL | w;
    if (w >= 0 && len >= 0) {
        out = moby_refine_slot(w, buf, len);
        /* A full slot table is this present's limit, not the tick's answer. */
        keep = out == OPENPETE_MATERIAL_SLOT_STOCK ? -1 : tick_out_keep(w, buf, len);
        if (keep < 0) e = NULL;
        else keep |= MEMO_REFINE;
    } else {
        keep = out;
    }
    if (e) {
        e->vaddr = moby_vaddr; e->cls = moby_class; e->stamp = stamp; e->out = keep;
        s_moby_memo_n++;
    }
    return out;
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
 * per tick on the sim thread. A terrain refine selector carries one key,
 * a Tiledef id or the untextured band, so the material's outcome is the
 * outcome of every face it reaches and one memo per material serves the
 * walked and the retained terrain alike. The slot memo is keyed on the
 * present counter openpete_mods_material_frame_reset() advances; the view
 * captures it for the compose (mod_mat_terrain_bucket_slot). The block
 * itself is kept for the tick (memo_stamp), and a later present of the tick
 * re-interns it without a call. The callback runs inside a guarded stretch
 * owned by the material's mod and receives a terrain key with the
 * selector's texture (-1 for the untextured band); its block, capped at
 * OPENPETE_MATERIAL_UNIFORMS_MAX, reuses an interned slot of the same
 * material with identical bytes, else takes a new slot. */
int mod_mat_terrain_refine_slot(int w) {
    if (w < 0 || w >= s_mat_count) return 0;
    mod_material_t* m = &s_mats[w];
    if (!m->per_instance) return 0;
    if (m->refine_present == s_mat_present) return m->refine_slot;
    m->refine_present = s_mat_present;
    m->refine_slot = OPENPETE_MATERIAL_TERRAIN_STOCK;
    const unsigned stamp = memo_stamp();
    if (m->refine_stamp == stamp) {
        cen_memo_replay();
        if (m->refine_kept_len < 0) return m->refine_slot;
        const int s = slot_intern(w, m->refine_kept, m->refine_kept_len);
        return s < 0 ? m->refine_slot : (m->refine_slot = s);
    }

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
    if (rc > (int)sizeof buf) rc = (int)sizeof buf;
    m->refine_stamp = stamp;
    m->refine_kept_len = rc < 0 ? -1 : rc;
    if (rc < 0) return m->refine_slot;          /* declined -> stock */
    memcpy(m->refine_kept, buf, (size_t)rc);
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

/** @brief Test whether a material stamped at extraction is still live, user-enabled and its owner enabled.
 *
 * A stamped outcome can outlive its material's usability: extraction
 * stamps it, the tick boundary (live-toggle apply and hot-reload sweep)
 * runs next, and only then does the present draw. A disabled material or
 * owner must stop that present. A swept material has freed SPIR-V and a
 * retired pipeline slot no later sweep revisits, so a rebuild through it
 * would cache a pipeline nothing destroys. Each case draws stock.
 * @param m Material index.
 * @return Nonzero when usable. */
static int material_usable(int m) {
    return m >= 0 && m < s_mat_count && s_mats[m].live && s_mats[m].user_enabled &&
           openpete_mods_is_enabled(s_mats[m].owner);
}
/** @brief Test whether an interned slot's material is usable (material_usable).
 *
 * @param slot Slot index in the current view.
 * @return Nonzero when usable. */
static int material_slot_usable(int slot) { return material_usable(view_slot_mat(slot)); }

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

/** @brief Read the interned material slot or declarative landing a tag names directly.
 *
 * A moby tag carries the extraction walk's outcome in bits [27:16]
 * (OPENPETE_MATERIAL_SLOT_STOCK = exhausted, OPENPETE_MATERIAL_SLOT_DECL |
 * index = a declarative landing, else an interned slot); a terrain tag
 * carries a per-present slot in bits [24:16]
 * (OPENPETE_MATERIAL_TERRAIN_STOCK = declined). Slot 0 defers to the
 * declarative resolve. A slot at or past the view's slot count is stale,
 * and a slot or landing whose material is no longer usable
 * (material_usable) does not resolve.
 * @param tag Provenance tag.
 * @param path 0 moby, 1 terrain; other paths carry no slot.
 * @param slot Receives the slot on a hit, or the material index of a
 *        declarative landing.
 * @return 1 with the slot, 2 with a declarative landing's material, 0 to
 *         fall through to the declarative resolve, -1 when the outcome is
 *         stock or no longer resolves. */
static int material_slot_from_tag(uint32_t tag, int path, int* slot) {
    int s;
    if (path == 0) {
        s = (int)((tag >> 16) & 0xFFFu);
        if (s == OPENPETE_MATERIAL_SLOT_STOCK) return -1;    /* exhausted */
        if (s & OPENPETE_MATERIAL_SLOT_DECL) {
            s &= OPENPETE_MATERIAL_SLOT_DECL - 1;
            if (!material_usable(s)) return -1;
            *slot = s;
            return 2;
        }
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

/** @brief Test whether a declarative material may shade a decoded tag key at a contract's draw site.
 *
 * A material with a per-instance callback never matches: it resolves
 * through slots only. A moby tag tests the extraction walk's predicate
 * (material_matches_moby); another tag tests liveness, the user and owner
 * enable, the contract and material_selector_matches.
 * @param m The material.
 * @param k The decoded tag key.
 * @param contract The draw site's OPENPETE_MATERIAL_CONTRACT_*.
 * @return 1 when the material matches. */
static int material_decl_matches_key(const mod_material_t* m, const material_key_t* k, int contract) {
    if (m->per_instance) return 0;
    if (k->path == 0) return material_matches_moby(m, k->cls, k->level, contract);
    return m->live && m->user_enabled && m->contract == contract &&
           openpete_mods_is_enabled(m->owner) && material_selector_matches(m, k);
}

/** @brief Return the declarative material that wins a decoded tag key, or -1.
 *
 * Considers live, user-enabled materials of enabled mods with the shading
 * contract @p contract and without a per-instance callback, which resolve
 * through slots only; among those the key's predicate accepts
 * (material_matches_moby for a moby tag, material_selector_matches
 * otherwise), the highest ranked (mod_mat_outranks) wins.
 * @param k The decoded tag key.
 * @param contract The draw site's OPENPETE_MATERIAL_CONTRACT_*. */
static int material_winner(const material_key_t* k, int contract) {
    int best = -1;
    for (int i = 0; i < s_mat_count; i++) {
        if (material_decl_matches_key(&s_mats[i], k, contract) && mod_mat_outranks(i, best)) best = i;
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
    case 2:
        /* A slot or landing whose material has another contract draws stock;
         * a landing binds its material's static block. */
        best = from_tag == 1 ? view_slot_mat(*slot) : *slot;
        if (from_tag == 2) *slot = 0;
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

/* ------------------------------------------------------------------ */
/* Overlap diagnostic. */

/* A material's key set per channel. A moby class or a channel identity is
 * either every value, one value, or none: every selector key is -1 or one
 * value, so the resolver's predicates, asked at the material's own key and
 * at a value no selector names, decide which. Terrain keys are enumerated:
 * (texid -1..127) x env x tier, the bake's key space. */
enum { OVL_NONE = 0, OVL_ONE, OVL_ALL };
#define OVL_SENTINEL (-0x7FFFFFFF)   /* a class or identity no selector names */
#define OVL_TER_KEYS (129 * 4)
#define OVL_TER_WORDS ((OVL_TER_KEYS + 63) / 64)

typedef struct { int kind, val; } ovl_set_t;
typedef struct {
    ovl_set_t ch[OP_CHAN_PORTAL + 1];   /* by OP_CHAN_*; the terrain entry is unused */
    uint64_t  ter[OVL_TER_WORDS];
    int load, pin, refine;
    int asked, declined;                /* moby refine observation */
} ovl_foot_t;

static _Atomic unsigned s_ovl_gen = 1;
/* The inputs the last snapshot was taken under. */
static unsigned s_ovl_seen_gen, s_ovl_seen_toggle, s_ovl_seen_obs;
static int s_ovl_seen_level, s_ovl_snapped;
static ovl_foot_t s_ovl_foot[MOD_MATERIAL_MAX];
static int s_ovl_n;
/* Computed and published verdicts per (hi, lo), and the listed pairs. */
static unsigned char s_ovl_next[MOD_MATERIAL_MAX][MOD_MATERIAL_MAX];
static unsigned char s_ovl_prev[MOD_MATERIAL_MAX][MOD_MATERIAL_MAX];
static mod_mat_overlap_t s_ovl_stage[MOD_MAT_OVERLAP_MAX], s_ovl_pub[MOD_MAT_OVERLAP_MAX];
static int s_ovl_stage_total, s_ovl_pub_total, s_ovl_prev_n;

void mod_mat_overlap_bump(void) { atomic_fetch_add_explicit(&s_ovl_gen, 1u, memory_order_relaxed); }

/** @brief Clear the published verdicts and the refine observation counter for mod_mat_reset, and mark a snapshot due. */
static void overlap_reset(void) {
    memset(s_ovl_prev, 0, sizeof s_ovl_prev);
    s_ovl_prev_n = s_ovl_pub_total = s_ovl_stage_total = s_ovl_n = 0;
    s_ovl_snapped = 0;
    s_ovl_obs = 0;
    mod_mat_overlap_bump();
}

int mod_mat_overlap_due(void) {
    return !s_ovl_snapped || s_ovl_seen_gen != atomic_load_explicit(&s_ovl_gen, memory_order_relaxed) ||
           s_ovl_seen_toggle != openpete_mods_toggle_epoch() || s_ovl_seen_level != s_cur_level ||
           s_ovl_seen_obs != s_ovl_obs;
}

/** @brief Return the draw-site contract of a channel: DECAL for shadows, SCENE for every other channel. */
static int ovl_chan_contract(int ch) {
    return ch == OP_CHAN_SHADOW ? OPENPETE_MATERIAL_CONTRACT_DECAL : OPENPETE_MATERIAL_CONTRACT_SCENE;
}

/** @brief Return the moby classes a material can take: through a declarative landing or a refine call of the extraction walk.
 *
 * @param m The material.
 * @param level The pushed level. */
static ovl_set_t ovl_moby_set(const mod_material_t* m, int level) {
    const int c = OPENPETE_MATERIAL_CONTRACT_SCENE;
    ovl_set_t s = {OVL_NONE, 0};
    const int named = m->sel.moby_class != -1 ? m->sel.moby_class : m->sel.ident;
    if (material_matches_moby(m, OVL_SENTINEL, level, c)) s.kind = OVL_ALL;
    else if (named != -1 && material_matches_moby(m, named, level, c)) { s.kind = OVL_ONE; s.val = named; }
    return s;
}

/** @brief Return the identities of a declarative channel (player through portal) a material can shade.
 *
 * @param m The material.
 * @param ch OP_CHAN_* other than moby and terrain.
 * @param level The pushed level. */
static ovl_set_t ovl_chan_set(const mod_material_t* m, int ch, int level) {
    material_key_t k = {ch - 1, -1, -1, ch, OVL_SENTINEL, level, 0, 0, 0};
    ovl_set_t s = {OVL_NONE, 0};
    if (material_decl_matches_key(m, &k, ovl_chan_contract(ch))) { s.kind = OVL_ALL; return s; }
    k.ident = m->sel.ident;
    if (m->sel.ident != -1 && material_decl_matches_key(m, &k, ovl_chan_contract(ch))) {
        s.kind = OVL_ONE; s.val = m->sel.ident;
    }
    return s;
}

/** @brief Set one bit per terrain key the bake would bucket to a material when it wins.
 *
 * @param m The material.
 * @param level The pushed level.
 * @param bits Receives OVL_TER_WORDS words, bit (texid + 1) * 4 + env * 2 + lp. */
static void ovl_terrain_bits(const mod_material_t* m, int level, uint64_t* bits) {
    memset(bits, 0, OVL_TER_WORDS * sizeof *bits);
    if (!terrain_sel_usable(m, level)) return;
    for (int k = 0; k < OVL_TER_KEYS; k++) {
        const int texid = k / 4 - 1, env = (k >> 1) & 1, lp = k & 1;
        const int hit = m->per_instance ? terrain_refine_key_matches(m, texid < 0, texid)
                                        : terrain_decl_matches(m, texid, env, lp);
        if (hit) bits[k >> 6] |= 1ull << (k & 63);
    }
}

void mod_mat_overlap_snapshot(void) {
    const int level = s_cur_level;
    s_ovl_seen_gen = atomic_load_explicit(&s_ovl_gen, memory_order_relaxed);
    s_ovl_seen_toggle = openpete_mods_toggle_epoch();
    s_ovl_seen_level = level;
    s_ovl_seen_obs = s_ovl_obs;
    s_ovl_snapped = 1;
    s_ovl_n = s_mat_count;
    for (int i = 0; i < s_mat_count; i++) {
        const mod_material_t* m = &s_mats[i];
        ovl_foot_t* f = &s_ovl_foot[i];
        memset(f, 0, sizeof *f);
        f->load = m->load_index;
        f->pin = m->api_pin;
        f->refine = m->per_instance != NULL;
        f->asked = m->obs_calls > 0;
        f->declined = m->obs_declines > 0;
        f->ch[OP_CHAN_MOBY] = ovl_moby_set(m, level);
        for (int ch = OP_CHAN_PLAYER; ch <= OP_CHAN_PORTAL; ch++) f->ch[ch] = ovl_chan_set(m, ch, level);
        ovl_terrain_bits(m, level, f->ter);
    }
}

/** @brief Test whether two key sets share a value. */
static int ovl_meets(ovl_set_t a, ovl_set_t b) {
    return a.kind && b.kind && (a.kind == OVL_ALL || b.kind == OVL_ALL || a.val == b.val);
}
/** @brief Test whether key set @p l lies inside key set @p h. */
static int ovl_within(ovl_set_t l, ovl_set_t h) {
    return !l.kind || h.kind == OVL_ALL || (l.kind == OVL_ONE && h.kind == OVL_ONE && l.val == h.val);
}

/** @brief Compare the terrain keys of a pair: whether they share one, and whether the lower's lie inside the higher's.
 *
 * @param h The higher material's footprint.
 * @param l The lower material's footprint.
 * @param within Receives 1 when every key of @p l is a key of @p h.
 * @return 1 when a key is shared. */
static int ovl_ter_meets(const ovl_foot_t* h, const ovl_foot_t* l, int* within) {
    int meet = 0;
    *within = 1;
    for (int w = 0; w < OVL_TER_WORDS; w++) {
        if (l->ter[w] & h->ter[w]) meet = 1;
        if (l->ter[w] & ~h->ter[w]) *within = 0;
    }
    return meet;
}

/** @brief Return the shadow verdict of a higher-ranked material over a lower-ranked one from their footprints.
 *
 * Full when the higher takes every key of the lower: on terrain any higher
 * material that matches a key takes it (a refine's decline draws stock); on
 * the moby walk a declarative takes the keys it matches, and a refine takes
 * them only when it has been asked and never declined (accept-all); on the
 * declarative channels only declaratives match. Partial when a key is
 * shared but not every key is taken; none when no key is shared.
 * @param hi Higher-ranked material index.
 * @param lo Lower-ranked material index.
 * @param chans Receives the OP_CHAN_* bits of the channels with a shared key.
 * @param accept_all Receives 1 when a full verdict rests on an accept-all moby refine.
 * @return MOD_MAT_SHADOW_*. */
static int ovl_pair(int hi, int lo, unsigned* chans, int* accept_all) {
    const ovl_foot_t* h = &s_ovl_foot[hi];
    const ovl_foot_t* l = &s_ovl_foot[lo];
    int within = 1;
    *chans = ovl_ter_meets(h, l, &within) ? 1u << OP_CHAN_TERRAIN : 0u;
    int full = within;
    for (int ch = OP_CHAN_MOBY; ch <= OP_CHAN_PORTAL; ch++) {
        if (ch == OP_CHAN_TERRAIN || !l->ch[ch].kind) continue;
        if (ovl_meets(l->ch[ch], h->ch[ch])) *chans |= 1u << ch;
        const int takes = !h->refine || (h->asked && !h->declined);
        if (!takes || !ovl_within(l->ch[ch], h->ch[ch])) full = 0;
    }
    *accept_all = full && h->refine && (*chans & (1u << OP_CHAN_MOBY)) != 0;
    if (!*chans) return MOD_MAT_SHADOW_NONE;
    return full ? MOD_MAT_SHADOW_FULL : MOD_MAT_SHADOW_PARTIAL;
}

void mod_mat_overlap_compute(void) {
    s_ovl_stage_total = 0;
    memset(s_ovl_next, 0, sizeof s_ovl_next);
    for (int a = 0; a < s_ovl_n; a++)
        for (int b = a + 1; b < s_ovl_n; b++) {
            const int a_hi = rank_above(a, s_ovl_foot[a].load, s_ovl_foot[a].pin, b, s_ovl_foot[b].load);
            const int hi = a_hi ? a : b, lo = a_hi ? b : a;
            unsigned chans;
            int aa;
            const int v = ovl_pair(hi, lo, &chans, &aa);
            if (v == MOD_MAT_SHADOW_NONE) continue;
            s_ovl_next[hi][lo] = (unsigned char)v;
            if (s_ovl_stage_total < MOD_MAT_OVERLAP_MAX) {
                mod_mat_overlap_t* p = &s_ovl_stage[s_ovl_stage_total];
                p->hi = hi; p->lo = lo; p->verdict = v; p->chans = chans; p->accept_all = aa;
            }
            s_ovl_stage_total++;
        }
}

const char* mod_mat_shadow_name(int verdict) {
    return verdict == MOD_MAT_SHADOW_FULL ? "full" : verdict == MOD_MAT_SHADOW_PARTIAL ? "partial" : "none";
}

/* Channel names by OP_CHAN_*. */
static const char* const k_ovl_chan[] = {"", "moby", "terrain", "player", "flame", "particle",
                                         "sky", "shadow", "tracer", "portal"};

/** @brief Write a material's kind and the selector keys it sets into a buffer.
 *
 * @param i Material index.
 * @param buf Receives the text.
 * @param cap Bytes in @p buf. */
static void ovl_describe(int i, char* buf, size_t cap) {
    const mod_material_t* m = &s_mats[i];
    const int ch = m->sel.channel;
    int n = snprintf(buf, cap, "%s #%d %s %s", m->owner ? m->owner : "?", i,
                     m->per_instance ? "refine" : "decl",
                     ch == -1 ? "any" : ch >= OP_CHAN_MOBY && ch <= OP_CHAN_PORTAL ? k_ovl_chan[ch] : "?");
    if (m->sel.moby_class != -1 && n < (int)cap) n += snprintf(buf + n, cap - (size_t)n, " class %d", m->sel.moby_class);
    if (m->sel.ident != -1 && n < (int)cap) n += snprintf(buf + n, cap - (size_t)n, " ident %d", m->sel.ident);
    if (m->sel.texture != -1 && n < (int)cap) n += snprintf(buf + n, cap - (size_t)n, " tex %d", m->sel.texture);
    if (m->sel.level != -1 && n < (int)cap) n += snprintf(buf + n, cap - (size_t)n, " level %d", m->sel.level);
    if (m->sel.flags && n < (int)cap) snprintf(buf + n, cap - (size_t)n, " flags %#x", m->sel.flags);
}

/** @brief Write the channel names of an OP_CHAN_* bit set, comma separated.
 *
 * @param chans The bits.
 * @param buf Receives the text.
 * @param cap Bytes in @p buf. */
static void ovl_chan_list(unsigned chans, char* buf, size_t cap) {
    size_t n = 0;
    buf[0] = 0;
    for (int ch = OP_CHAN_MOBY; ch <= OP_CHAN_PORTAL && n < cap; ch++)
        if (chans & (1u << ch)) n += (size_t)snprintf(buf + n, cap - n, "%s%s", n ? "," : "", k_ovl_chan[ch]);
}

/** @brief Log one pair whose verdict changed, naming both materials, the shared channels and the refine observation.
 *
 * @param hi Higher-ranked material index.
 * @param lo Lower-ranked material index.
 * @param verdict The new MOD_MAT_SHADOW_*. */
static void ovl_log(int hi, int lo, int verdict) {
    char a[160], b[160], c[96];
    unsigned chans = 0;
    int aa = 0;
    if (verdict != MOD_MAT_SHADOW_NONE) (void)ovl_pair(hi, lo, &chans, &aa);
    ovl_describe(hi, a, sizeof a);
    ovl_describe(lo, b, sizeof b);
    ovl_chan_list(chans, c, sizeof c);
    const ovl_foot_t* h = &s_ovl_foot[hi];
    const char* why = !h->refine || !(chans & (1u << OP_CHAN_MOBY)) ? "" : aa ? " (accept-all refine)"
                    : h->declined ? " (refine declines)" : !h->asked ? " (refine not yet asked)" : "";
    OPL_INFO_F("mods", "material overlap: %s over %s: %s%s%s%s", a, b, mod_mat_shadow_name(verdict),
               *c ? " on " : "", c, verdict == MOD_MAT_SHADOW_NONE ? "" : why);
}

void mod_mat_overlap_publish(void) {
    const int n = s_ovl_n > s_ovl_prev_n ? s_ovl_n : s_ovl_prev_n;
    for (int hi = 0; hi < n; hi++)
        for (int lo = 0; lo < n; lo++)
            if (s_ovl_next[hi][lo] != s_ovl_prev[hi][lo]) ovl_log(hi, lo, s_ovl_next[hi][lo]);
    memcpy(s_ovl_prev, s_ovl_next, sizeof s_ovl_prev);
    s_ovl_prev_n = s_ovl_n;
    s_ovl_pub_total = s_ovl_stage_total;
    const int listed = s_ovl_stage_total < MOD_MAT_OVERLAP_MAX ? s_ovl_stage_total : MOD_MAT_OVERLAP_MAX;
    memcpy(s_ovl_pub, s_ovl_stage, (size_t)listed * sizeof *s_ovl_pub);
}

int mod_mat_overlap_verdict(int hi, int lo) {
    if (hi < 0 || lo < 0 || hi >= MOD_MATERIAL_MAX || lo >= MOD_MATERIAL_MAX) return MOD_MAT_SHADOW_NONE;
    return s_ovl_prev[hi][lo];
}

int mod_mat_overlap_total(void) { return s_ovl_pub_total; }

const mod_mat_overlap_t* mod_mat_overlap_at(int i) {
    const int listed = s_ovl_pub_total < MOD_MAT_OVERLAP_MAX ? s_ovl_pub_total : MOD_MAT_OVERLAP_MAX;
    return i >= 0 && i < listed ? &s_ovl_pub[i] : NULL;
}
