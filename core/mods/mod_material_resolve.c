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
static void ter_class_reset(void);
void mod_mat_reset(void) {
    memset(s_mats, 0, sizeof s_mats);
    s_mat_count = 0;
    s_mat_sel_dirty = 0;
    s_mat_has_instance = 0;
    reset_level_and_view();
    overlap_reset();
    ter_class_reset();
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

/* The member set of the installed combined modules; NULL while none is
 * installed, when every member is absent. */
static _Atomic(const openpete_mods_material_chain_set_t*) s_chain_set;

/* The chain entries interned this present, members lowest-ranked first:
 * the moby field names entry id as OPENPETE_MATERIAL_SLOT_CHAIN + id. */
typedef struct {
    int   n;
    short mat[OPENPETE_MATERIAL_CHAIN_CAP];
    short slot[OPENPETE_MATERIAL_CHAIN_CAP];
} mat_chain_t;
static mat_chain_t s_mat_chains[OPENPETE_MATERIAL_CHAINS];
static int s_mat_chain_n;

/* The members of one walk's chain, highest-ranked first, with each kept
 * refine block (len -1 for a declarative member). */
typedef struct {
    int n;
    int mat[OPENPETE_MATERIAL_CHAIN_CAP];
    int len[OPENPETE_MATERIAL_CHAIN_CAP];
    unsigned char bytes[OPENPETE_MATERIAL_CHAIN_CAP][OPENPETE_MATERIAL_UNIFORMS_MAX];
} chain_walk_t;

/* Chains the tick computed, for the moby memo to re-intern on replay. */
static chain_walk_t s_tick_chain[OPENPETE_MATERIAL_CHAINS];
static int s_tick_chain_n;


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
 * bits: its slot ids stay below the declarative bit
 * OPENPETE_MATERIAL_TERRAIN_DECL, every landing OPENPETE_MATERIAL_TERRAIN_DECL
 * | index stays below the stock outcome 0x1FF, and the decode masks
 * (tag >> 16) & 0x1FF, so a wider id would come back as another outcome
 * with no log. Raising MOD_MATERIAL_SLOTS or MOD_MATERIAL_MAX means
 * widening that field first. */
_Static_assert(MOD_MATERIAL_SLOTS <= OPENPETE_MATERIAL_TERRAIN_DECL, "slot ids stay below the terrain declarative bit");
_Static_assert(MOD_MATERIAL_MAX - 1 < 0xFF, "material index fits the terrain declarative landing");
_Static_assert((OPENPETE_MATERIAL_TERRAIN_DECL | (MOD_MATERIAL_MAX - 1)) < OPENPETE_MATERIAL_TERRAIN_STOCK,
               "no terrain declarative landing aliases the stock outcome");
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
/* Terrain key histograms over the face keys (ter_key_index: texid
 * -1..127 x texturedness x env x lp): the retained bake's class queries
 * (openpete_mods_material_terrain_class), keeping the last class id, and
 * the walked path's (openpete_mods_material_terrain_walk_slot), keeping the
 * last resolved member + 1 (0 stock). */
#define CEN_TER_KEYS (129 * 8)
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
static _Atomic int      s_cen_pass_max;        /* most terrain material passes one colour slot drew */
/* Composition: chains the extraction walk formed, members it excluded as
 * absent from the installed member set, chain outcomes a full slot or chain
 * table sent to stock; draw-time chain resolves by member count (index n),
 * and chain draws sent to stock for a held, absent or unusable member or a
 * stale entry. */
static _Atomic unsigned s_cen_chain_walks, s_cen_chain_absent, s_cen_chain_full;
static _Atomic unsigned s_cen_chain_draws[OPENPETE_MATERIAL_CHAIN_CAP + 1];
static _Atomic unsigned s_cen_chain_held, s_cen_chain_lacked, s_cen_chain_unusable, s_cen_chain_stale;
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
    atomic_store_explicit(&s_cen_pass_max, 0, memory_order_relaxed);
    _Atomic unsigned* chain[] = { &s_cen_chain_walks, &s_cen_chain_absent, &s_cen_chain_full, &s_cen_chain_held,
                                  &s_cen_chain_lacked, &s_cen_chain_unusable, &s_cen_chain_stale };
    for (size_t i = 0; i < sizeof chain / sizeof *chain; i++) atomic_store_explicit(chain[i], 0u, memory_order_relaxed);
    for (int i = 0; i <= OPENPETE_MATERIAL_CHAIN_CAP; i++)
        atomic_store_explicit(&s_cen_chain_draws[i], 0u, memory_order_relaxed);
}
unsigned mod_mat_census_chain(int which) {
    _Atomic unsigned* const c[MOD_MAT_CHAIN_CENSUS_N] = {
        &s_cen_chain_walks, &s_cen_chain_absent, &s_cen_chain_full, &s_cen_chain_held,
        &s_cen_chain_lacked, &s_cen_chain_unusable, &s_cen_chain_stale };
    if (which >= MOD_MAT_CHAIN_DRAWS_2 && which < MOD_MAT_CHAIN_DRAWS_2 + OPENPETE_MATERIAL_CHAIN_CAP - 1)
        return atomic_load_explicit(&s_cen_chain_draws[which - MOD_MAT_CHAIN_DRAWS_2 + 2], memory_order_relaxed);
    return which >= 0 && which < MOD_MAT_CHAIN_CENSUS_N ? atomic_load_explicit(c[which], memory_order_relaxed) : 0;
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
/** @brief Record one terrain key query and its outcome.
 *
 * @param n Count array of CEN_TER_KEYS.
 * @param b Last-outcome array of CEN_TER_KEYS.
 * @param k Face key index (ter_key_index).
 * @param v The outcome: a class id on the bake, a member + 1 on the walk. */
static void cen_ter(_Atomic unsigned* n, _Atomic int* b, int k, int v) {
    if (!mod_mat_census_on() || k < 0 || k >= CEN_TER_KEYS) return;
    atomic_fetch_add_explicit(&n[k], 1u, memory_order_relaxed);
    atomic_store_explicit(&b[k], v, memory_order_relaxed);
}
void mod_mat_census_bucket_pass(int m) { cen_bump(m, MOD_MAT_CENSUS_BUCKET_PASSES); }
unsigned mod_mat_census_get(int m, int which) {
    if (m < 0 || m >= MOD_MATERIAL_MAX || which < 0 || which >= MOD_MAT_CENSUS_N) return 0;
    return atomic_load_explicit(&s_cen[m][which], memory_order_relaxed);
}

static int census_class_member(int c);
static void census_print_classes_ter(void);
/** @brief Print the terrain key comparison: bake keys, walked keys, keys both saw, and disagreements.
 *
 * A bake key's member is its class's last resolution (census_class_member).
 * A disagreement is a key both paths queried whose members differ; the
 * first eight are listed. */
static void census_print_terrain(void) {
    unsigned bake_keys = 0, walk_keys = 0, both = 0, differ = 0, bake_q = 0, walk_q = 0;
    unsigned untex_bake = 0;
    for (int k = 0; k < CEN_TER_KEYS; k++) {
        const unsigned bn = atomic_load_explicit(&s_cen_bake_n[k], memory_order_relaxed);
        const unsigned wn = atomic_load_explicit(&s_cen_walk_n[k], memory_order_relaxed);
        bake_q += bn; walk_q += wn;
        if (bn) { bake_keys++; if (k < 8) untex_bake += bn; }
        if (wn) walk_keys++;
        if (!bn || !wn) continue;
        both++;
        const int bc = atomic_load_explicit(&s_cen_bake_b[k], memory_order_relaxed);
        const int bb = census_class_member(bc) + 1;
        const int wb = atomic_load_explicit(&s_cen_walk_b[k], memory_order_relaxed);
        if (bb == wb) continue;
        if (differ++ < 8)
            fprintf(stderr, "[material-census] terrain key texid=%d untex=%d env=%d lp=%d: bake class %d member %d, "
                    "walked member %d\n", k / 8 - 1, (k >> 2) & 1, (k >> 1) & 1, k & 1, bc, bb - 1, wb - 1);
    }
    fprintf(stderr, "[material-census] terrain bake: %u queries over %u keys (%u with texid -1); "
                    "walked: %u resolves over %u keys; both %u, resolved member differs on %u\n",
            bake_q, bake_keys, untex_bake, walk_q, walk_keys, both, differ);
    census_print_classes_ter();
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
                    " outranked stale_memo\n");
    for (int i = 0; i < s_mat_count; i++) {
        const mod_material_t* m = &s_mats[i];
        unsigned c[MOD_MAT_CENSUS_N];
        for (int j = 0; j < MOD_MAT_CENSUS_N; j++) c[j] = mod_mat_census_get(i, j);
        engaged += (unsigned long long)c[MOD_MAT_CENSUS_DRAWS] + c[MOD_MAT_CENSUS_BUCKET_PASSES] +
                   c[MOD_MAT_CENSUS_REFINE_CALLS];
        fprintf(stderr, "[material-census] %3d %s %d %d %d %d %d %#x %s %d %d | %u %u %u %u %u %u %u %u\n",
                i, m->owner ? m->owner : "?", m->sel.channel, m->sel.moby_class, m->sel.ident,
                m->sel.texture, m->sel.level, m->sel.flags, m->per_instance ? "refine" : "decl",
                m->contract, m->load_index,
                c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
    }
    fprintf(stderr, "[material-census] view-installed resolves=%u tick-memo replays=%u\n",
            atomic_load_explicit(&s_cen_view_resolves, memory_order_relaxed),
            atomic_load_explicit(&s_cen_memo_replays, memory_order_relaxed));
    fprintf(stderr, "[material-census] chains: walks=%u absent-excluded=%u table-full=%u; draws by members "
                    "2=%u 3=%u 4=%u; draws stock: held=%u lacked=%u unusable=%u stale=%u\n",
            mod_mat_census_chain(MOD_MAT_CHAIN_WALKS), mod_mat_census_chain(MOD_MAT_CHAIN_ABSENT),
            mod_mat_census_chain(MOD_MAT_CHAIN_FULL), mod_mat_census_chain(MOD_MAT_CHAIN_DRAWS_2),
            mod_mat_census_chain(MOD_MAT_CHAIN_DRAWS_2 + 1), mod_mat_census_chain(MOD_MAT_CHAIN_DRAWS_2 + 2),
            mod_mat_census_chain(MOD_MAT_CHAIN_HELD), mod_mat_census_chain(MOD_MAT_CHAIN_LACKED),
            mod_mat_census_chain(MOD_MAT_CHAIN_UNUSABLE), mod_mat_census_chain(MOD_MAT_CHAIN_STALE));
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
 * changes a terrain resolution class (ter_candidates) could actually see. */
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
    for (int i = 0; i < s_mat_count; i++) {
        const mod_material_t* m = &s_mats[i];
        if (!m->live) continue;
        any = 1;
        /* A composable moby material needs the walk as a refine does: a
         * chain is a per-moby outcome only the extraction can stamp. */
        if (m->per_instance || (!mod_mat_terminal(m) && m->contract == OPENPETE_MATERIAL_CONTRACT_SCENE &&
                                (m->sel.channel == -1 || m->sel.channel == OP_CHAN_MOBY)))
            inst = 1;
    }
    s_mat_has_instance = inst;
    openpete_native_set_tag_split(any);
}

int mod_mat_terminal(const mod_material_t* m) {
    return m->api_pin < MOD_MAT_API_COMPOSE || (m->sel.flags & (unsigned)OP_SEL_EXCLUSIVE) != 0;
}

unsigned mod_mat_table_gen(void) { return s_mat_gen; }

int openpete_mods_material_has_instance(void) { return s_mat_has_instance; }

unsigned openpete_mods_material_terrain_epoch(void) {
    /* Sum of two monotonic counters - strictly increases on any change the
     * terrain bake cares about; +1 keeps 0 free as the latch's "unset". The
     * mod toggle epoch rides along because terrain_sel_usable() consults
     * openpete_mods_is_enabled(owner). */
    return 1u + s_mat_ter_epoch + openpete_mods_toggle_epoch();
}

int mod_mat_sel_valid(const char* id, const openpete_mod_material_selector_t* sel, int api_pin) {
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
    if ((sel->flags & (unsigned)OP_SEL_EXCLUSIVE) && api_pin < MOD_MAT_API_COMPOSE) {
        OPL_ERROR_F("mods", "%s: material: OP_SEL_EXCLUSIVE needs a manifest api pin of %d or "
                    "later (this mod pins %d, whose materials are terminal already) - refused",
                    id, MOD_MAT_API_COMPOSE, api_pin);
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

void mod_mat_set_link_of_frag(int frag, unsigned link_mask, unsigned identity_mask) {
    for (int i = 0; i < s_mat_count; i++) {
        if (s_mats[i].frag != frag) continue;
        s_mats[i].link_mask = (unsigned char)link_mask;
        s_mats[i].identity_mask = (unsigned char)identity_mask;
    }
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
    if (!mod_mat_sel_valid(id, sel, m->api_pin)) return -1;
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
    s_mat_chain_n = 0;
    s_tick_chain_n = 0;
    openpete_mods_material_chain_set_publish(NULL);
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
static unsigned s_memo_tick, s_memo_gen, s_memo_toggle, s_memo_set;
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
#define MEMO_CHAIN  0x20000   /* with MEMO_REFINE: names s_tick_chain[k] */
typedef struct { uint32_t vaddr; int cls; unsigned stamp; int out; } moby_memo_t;
static moby_memo_t s_moby_memo[MOBY_MEMO_N];
static int s_moby_memo_n;

/** @brief Return the stamp of the current tick memo, opening a new memo when the tick, table or toggles moved.
 *
 * @return The stamp an entry of the current memo carries. */
static unsigned memo_stamp(void) {
    const unsigned toggle = openpete_mods_toggle_epoch();
    /* A chain names only members the installed set serves, so an outcome
     * walked against an earlier set is walked again, not replayed. */
    const openpete_mods_material_chain_set_t* cs = openpete_mods_material_chain_set_current();
    const unsigned set = cs ? cs->generation : 0u;
    if (s_memo_tick != s_mat_tick || s_memo_gen != s_mat_gen || s_memo_toggle != toggle || s_memo_set != set) {
        s_memo_tick = s_mat_tick; s_memo_gen = s_mat_gen; s_memo_toggle = toggle; s_memo_set = set;
        s_memo_stamp++;
        s_tick_out_n = 0;
        s_tick_chain_n = 0;
        s_moby_memo_n = 0;
    }
    return s_memo_stamp;
}

void openpete_mods_material_frame_reset(int tick_open) {
    s_mat_slot_n = 1;
    s_mat_chain_n = 0;
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
/* Terrain: resolution classes, the bake's class ids and the pass list. */

/** @brief Test whether a material can be a terrain face's candidate in a level.
 *
 * True when the material is live, user-enabled and owned by an enabled
 * mod, has the scene shading contract (retained terrain is a scene-contract
 * site), is on channel -1 or OP_CHAN_TERRAIN, and has no level key or one
 * equal to @p level; a negative @p level matches every level key. The
 * level test mirrors the draw-time resolve's, so a level-scoped selector
 * takes part in a face's class only in its level. */
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
 * set, no texture key); env and tier are not part of its key. A textured
 * face with no Tiledef id (@p texid -1) is no refine's key. The class table
 * (ter_candidates) serves both terrain paths, so both rank one candidate
 * set.
 * @param m The material.
 * @param untex Nonzero for an untextured face.
 * @param texid The face's Tiledef id when textured, or -1. */
static int terrain_refine_key_matches(const mod_material_t* m, int untex, int texid) {
    return untex ? ((m->sel.flags & OP_SEL_UNTEXTURED) && m->sel.texture == -1)
                 : (!(m->sel.flags & OP_SEL_UNTEXTURED) && texid >= 0 && m->sel.texture == texid);
}

/** @brief Test whether a declarative terrain material's selector accepts a face key.
 *
 * The draw-time resolve's predicate over the face's keys: the texture key
 * and the OP_SEL_ENV_ANIM, tier and textured/untextured flags. Texturedness
 * is the face's own, not inferred from the texid: a textured face whose
 * Tiledef id keys no selector (an imported sector's borrowed face) passes
 * texid -1 with @p untex 0, matches OP_SEL_TEXTURED and no texture key.
 * @param m The material.
 * @param texid Tiledef id, or -1 when the face has none.
 * @param untex Nonzero for an untextured face.
 * @param env Nonzero for a face in the env-rewrite set.
 * @param lp Nonzero for an LP-tier face. */
static int terrain_decl_matches(const mod_material_t* m, int texid, int untex, int env, int lp) {
    if (m->sel.texture != -1 && (untex || texid < 0 || m->sel.texture != texid)) return 0;
    if ((m->sel.flags & OP_SEL_ENV_ANIM) && !env) return 0;
    if ((m->sel.flags & OP_SEL_HP_TIER) && lp) return 0;
    if ((m->sel.flags & OP_SEL_LP_TIER) && !lp) return 0;
    if ((m->sel.flags & OP_SEL_TEXTURED) && untex) return 0;
    if ((m->sel.flags & OP_SEL_UNTEXTURED) && !untex) return 0;
    return 1;
}

/* Terrain resolution classes. A face key (texid, texturedness, env, tier)
 * has a candidate list: the refine materials whose key is the face's and
 * that outrank the face's declarative winner, highest first, then that
 * declarative winner when there is one. Refines below the declarative
 * winner are never reached, since the declarative takes every face it
 * matches. Equal lists are one class. The retained bake stamps the class
 * id into the face; each present resolves every class to one member, the
 * first accepting refine, else the declarative, else stock, and the
 * walked path stamps that member. The class id has the 8 bits of the
 * baked face field (OPENPETE_MATERIAL_TER_BUCKET_MAX), 0 = no candidate. */
#define TER_CLASS_N (OPENPETE_MATERIAL_TER_BUCKET_MAX + 1)
/* Members kept per class. A key with more refines above its declarative
 * winner keeps the highest-ranked ones; the rest are never asked. */
#define TER_CLASS_MEMBERS 32
/* Face keys: texid -1..127 x texturedness x env x tier. */
#define TER_KEYS (129 * 8)

typedef struct {
    int      n;                          /* members, highest ranked first */
    short    m[TER_CLASS_MEMBERS];       /* material indices */
    unsigned epoch;                      /* terrain epoch of the last intern, either path */
    unsigned bake_epoch;                 /* terrain epoch of the last retained-bake intern */
    unsigned res_present;                /* s_mat_present of `res` */
    int      res;                        /* resolved member, or -1 for stock */
} ter_class_t;
static ter_class_t s_cls[TER_CLASS_N];   /* [0] unused: no candidate */
static int s_cls_n = 1;
static int s_cls_recycled;               /* a full table reused a class id, logged once */
/* Face key -> class id, valid while the stamp equals the terrain epoch. */
static unsigned s_cls_key_stamp[TER_KEYS];
static unsigned char s_cls_key[TER_KEYS];

/** @brief Clear the class table and the key memo for mod_mat_reset. */
static void ter_class_reset(void) {
    memset(s_cls, 0, sizeof s_cls);
    memset(s_cls_key_stamp, 0, sizeof s_cls_key_stamp);
    s_cls_n = 1;
    s_cls_recycled = 0;
}

/** @brief Return the index of a face key in the class memo and the census histograms.
 *
 * @param texid Tiledef id -1..127.
 * @param untex Nonzero for an untextured face.
 * @param env Env-rewrite flag.
 * @param lp LP-tier flag. */
static int ter_key_index(int texid, int untex, int env, int lp) {
    return (((texid + 1) * 2 + (untex ? 1 : 0)) * 2 + (env ? 1 : 0)) * 2 + (lp ? 1 : 0);
}

/** @brief Compute a face key's candidate list at the pushed level.
 *
 * Over the materials terrain_sel_usable() accepts: the declarative winner
 * among those terrain_decl_matches() accepts (mod_mat_outranks), and the
 * refines whose one key is the face's (terrain_refine_key_matches) that
 * outrank it, highest first, at most TER_CLASS_MEMBERS - 1 of them; the
 * declarative winner ends the list.
 * @param texid Tiledef id, or -1.
 * @param untex Nonzero for an untextured face.
 * @param env Env-rewrite flag.
 * @param lp LP-tier flag.
 * @param out Receives TER_CLASS_MEMBERS material indices at most.
 * @return The member count, 0 when no material matches. */
static int ter_candidates(int texid, int untex, int env, int lp, short* out) {
    const int level = openpete_mods_current_level();
    int d = -1, n = 0;
    for (int i = 0; i < s_mat_count; i++) {
        const mod_material_t* m = &s_mats[i];
        if (!m->per_instance && terrain_sel_usable(m, level) &&
            terrain_decl_matches(m, texid, untex, env, lp) && mod_mat_outranks(i, d))
            d = i;
    }
    short r[MOD_MATERIAL_MAX];
    int rn = 0;
    for (int i = 0; i < s_mat_count; i++) {
        const mod_material_t* m = &s_mats[i];
        if (!m->per_instance || !terrain_sel_usable(m, level) ||
            !terrain_refine_key_matches(m, untex, texid) || !mod_mat_outranks(i, d))
            continue;
        int j = rn++;
        while (j > 0 && mod_mat_outranks(i, r[j - 1])) { r[j] = r[j - 1]; j--; }
        r[j] = (short)i;
    }
    for (n = 0; n < rn && n < TER_CLASS_MEMBERS - 1; n++) out[n] = r[n];
    if (d >= 0) out[n++] = (short)d;
    return n;
}

/** @brief Return the class id of a candidate list, interning it.
 *
 * An equal list reuses its class. With the table full, a class no path
 * interned in the current terrain epoch takes the new list: its faces were
 * baked under an earlier selector set, draw in the stock pass while the
 * class is not baked in the current epoch (ter_class_pass_live), and draw
 * the new list's member from the first bake that stamps the class again
 * until their sector rebakes.
 * @param list The members.
 * @param n Member count, at least 1.
 * @param ep The current terrain epoch.
 * @return The class id 1..TER_CLASS_N-1, or 0 when every class is current. */
static int ter_class_intern(const short* list, int n, unsigned ep) {
    for (int c = 1; c < s_cls_n; c++)
        if (s_cls[c].n == n && memcmp(s_cls[c].m, list, (size_t)n * sizeof *list) == 0) return c;
    int c = 0;
    if (s_cls_n < TER_CLASS_N) {
        c = s_cls_n++;
    } else {
        for (int r = 1; r < TER_CLASS_N && !c; r++)
            if (s_cls[r].epoch != ep) c = r;
        if (!c) return 0;
        if (!s_cls_recycled++)
            OPL_INFO_F("mods", "terrain material classes: table full (%d), reusing the classes of an "
                       "earlier selector set", TER_CLASS_N - 1);
    }
    memset(&s_cls[c], 0, sizeof s_cls[c]);
    s_cls[c].n = n;
    memcpy(s_cls[c].m, list, (size_t)n * sizeof *list);
    return c;
}

/** @brief Return the resolution class of a face key for the current terrain epoch, or 0 for none.
 *
 * Memoised per key until the terrain epoch moves.
 * @param texid Tiledef id, or -1.
 * @param untex Nonzero for an untextured face.
 * @param env Env-rewrite flag.
 * @param lp LP-tier flag. */
static int ter_class_of(int texid, int untex, int env, int lp) {
    if (s_mat_count == 0) return 0;
    if (texid < -1 || texid > 127) texid = -1;
    const int k = ter_key_index(texid, untex, env, lp);
    const unsigned ep = openpete_mods_material_terrain_epoch();
    if (s_cls_key_stamp[k] == ep) return s_cls_key[k];
    short list[TER_CLASS_MEMBERS];
    const int n = ter_candidates(texid, untex, env, lp, list);
    const int c = n ? ter_class_intern(list, n, ep) : 0;
    if (c) s_cls[c].epoch = ep;
    s_cls_key_stamp[k] = ep;
    s_cls_key[k] = (unsigned char)c;
    return c;
}

int openpete_mods_material_terrain_class(int texid, int untex, int env, int lp) {
    const int c = ter_class_of(texid, untex, env, lp);
    if (c) s_cls[c].bake_epoch = openpete_mods_material_terrain_epoch();
    if (mod_mat_census_on() && texid >= -1 && texid <= 127)
        cen_ter(s_cen_bake_n, s_cen_bake_b, ter_key_index(texid, untex, env, lp), c);
    return c;
}

/** @brief Return the pass a class draws in for the present the live memo describes: its resolved member + 1, or 0 for stock.
 *
 * Stock for a class the retained bake has not stamped in the current
 * terrain epoch, one not resolved this present, and one resolved to stock.
 * @param c Class id.
 * @param ep The current terrain epoch. */
static int ter_class_pass_live(int c, unsigned ep) {
    const ter_class_t* k = &s_cls[c];
    if (c <= 0 || c >= s_cls_n || k->bake_epoch != ep || k->res_present != s_mat_present || k->res < 0)
        return 0;
    return k->res + 1;
}

/** @brief Fill the pass of every class id from the live memo.
 *
 * @param out Receives TER_CLASS_N entries; class 0 and unused ids read 0. */
static void ter_class_pass_table_live(unsigned char* out) {
    const unsigned ep = openpete_mods_material_terrain_epoch();
    memset(out, 0, TER_CLASS_N);
    for (int c = 1; c < s_cls_n; c++) out[c] = (unsigned char)ter_class_pass_live(c, ep);
}

/** @brief List the material passes of a class-to-pass table, ascending.
 *
 * A pass is a material index + 1 that at least one class resolves to; the
 * stock pass 0 is not listed.
 * @param cp The class-to-pass table, TER_CLASS_N entries.
 * @param out Receives the pass ids.
 * @param cap Capacity of @p out.
 * @return Ids written. */
static int ter_pass_list(const unsigned char* cp, int* out, int cap) {
    unsigned char seen[TER_CLASS_N];
    memset(seen, 0, sizeof seen);
    for (int c = 1; c < TER_CLASS_N; c++) seen[cp[c]] = 1;
    int n = 0;
    for (int p = 1; p < TER_CLASS_N && n < cap; p++)
        if (seen[p]) out[n++] = p;
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
    unsigned char cp[TER_CLASS_N];
    ter_class_pass_table_live(cp);
    const int n = ter_pass_list(cp, out, cap);
    if (mod_mat_census_on() && n > atomic_load_explicit(&s_cen_pass_max, memory_order_relaxed))
        atomic_store_explicit(&s_cen_pass_max, n, memory_order_relaxed);
    return n;
}

int openpete_mods_material_terrain_pass_mask(int pass, uint32_t mask[8]) {
    unsigned char live[TER_CLASS_N];
    const unsigned char* cp = s_mat_view ? s_mat_view->ter_class_pass : live;
    if (!s_mat_view) ter_class_pass_table_live(live);
    int n = 0;
    memset(mask, 0, 8 * sizeof *mask);
    for (int c = 0; c < TER_CLASS_N; c++)
        if (cp[c] == pass) { mask[c >> 5] |= 1u << (c & 31); n++; }
    return n;
}

/** @brief Census: return a class's last resolved member, or -1 for stock, no resolution or no class.
 *
 * @param c Class id. */
static int census_class_member(int c) {
    return c > 0 && c < s_cls_n && s_cls[c].res_present ? s_cls[c].res : -1;
}

/** @brief Census: print the class count, the passes per colour slot, and each class's members and last resolution. */
static void census_print_classes_ter(void) {
    const unsigned ep = openpete_mods_material_terrain_epoch();
    int baked = 0;
    for (int c = 1; c < s_cls_n; c++) baked += s_cls[c].bake_epoch == ep;
    fprintf(stderr, "[material-census] terrain classes=%d (baked in the current epoch %d); "
                    "material passes per colour slot max=%d\n",
            s_cls_n - 1, baked, atomic_load_explicit(&s_cen_pass_max, memory_order_relaxed));
    for (int c = 1; c < s_cls_n && c <= 16; c++) {
        char buf[160];
        int n = 0;
        buf[0] = 0;
        for (int j = 0; j < s_cls[c].n && n < (int)sizeof buf - 8; j++)
            n += snprintf(buf + n, sizeof buf - (size_t)n, "%s%d%s", j ? " > " : "", s_cls[c].m[j],
                          s_mats[s_cls[c].m[j]].per_instance ? "r" : "d");
        fprintf(stderr, "[material-census] terrain class %d: %s; last resolved %d\n", c, buf, census_class_member(c));
    }
}

int mod_mat_terrain_class_count(void) { return s_cls_n - 1; }
int mod_mat_terrain_class_resolved(int c) {
    if (c <= 0 || c >= s_cls_n || s_cls[c].res_present != s_mat_present) return -1;
    return s_cls[c].res;
}

/* ------------------------------------------------------------------ */
/* Per-present view. */

/* Per-scene snapshot of the interned per-instance slots (see the ring in
 * native_render.c): slots 1 up to the live count, the static slot of every
 * material index, the terrain class-to-pass table and its pass list. Each slot's bytes sit in a
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
    out->chain_n = s_mat_chain_n;
    for (int i = 0; i < s_mat_chain_n; i++) {
        out->chains[i].n = s_mat_chains[i].n;
        memcpy(out->chains[i].mat, s_mat_chains[i].mat, sizeof out->chains[i].mat);
        memcpy(out->chains[i].slot, s_mat_chains[i].slot, sizeof out->chains[i].slot);
    }
    /* The live tables, whatever view the compose thread has installed. */
    ter_class_pass_table_live(out->ter_class_pass);
    out->ter_bucket_n = ter_pass_list(out->ter_class_pass, out->ter_bucket, OPENPETE_MODS_MATERIAL_VIEW_MATS);
    if (mod_mat_census_on() && out->ter_bucket_n > atomic_load_explicit(&s_cen_pass_max, memory_order_relaxed))
        atomic_store_explicit(&s_cen_pass_max, out->ter_bucket_n, memory_order_relaxed);
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

/* Advanced on the first call and the first decline of a refine, the two
 * events that move its accept-all observation. Sim thread only. */
static unsigned s_ovl_obs;

/** @brief Count one moby or terrain refine call and its decline for the overlap diagnostic's accept-all observation.
 *
 * @param m The refine material.
 * @param declined Nonzero when the callback returned -1. */
static void refine_observe(mod_material_t* m, int declined) {
    if (m->obs_calls++ == 0) s_ovl_obs++;
    if (declined && m->obs_declines++ == 0) s_ovl_obs++;
}

/* ------------------------------------------------------------------ */
/* Composition: the installed member set and the per-present chain table. */

void openpete_mods_material_chain_set_publish(const openpete_mods_material_chain_set_t* set) {
    atomic_store_explicit(&s_chain_set, set, memory_order_release);
}
const openpete_mods_material_chain_set_t* openpete_mods_material_chain_set_current(void) {
    return atomic_load_explicit(&s_chain_set, memory_order_acquire);
}

/** @brief Return a fragment shader's index in a member set, or -1 when the set is NULL or lacks it.
 *
 * @param set The member set.
 * @param frag Fragment shader handle. */
static int chain_set_index(const openpete_mods_material_chain_set_t* set, int frag) {
    for (int i = 0; set && i < set->n; i++)
        if (set->shader[i] == frag) return i;
    return -1;
}

/** @brief Test whether a material's stock-profile op_material returns its parameter. */
static int chain_identity(const mod_material_t* m) {
    return m->frag >= 0 && (m->identity_mask & 1u);
}

/** @brief Test whether a material can enter a chain drawn through the installed modules, counting an exclusion for absence.
 *
 * A vertex-only material contributes its vertex stage and no op_material,
 * so it needs no module. A material with a fragment stage needs a linkable
 * stock profile and a place in the installed member set; a linkable one
 * the set lacks (its module compile pending or failed, or none installed)
 * counts in the census as excluded for absence.
 * @param m The material.
 * @param set The installed member set, or NULL. */
static int chain_linkable(const mod_material_t* m, const openpete_mods_material_chain_set_t* set) {
    if (m->frag < 0) return m->vert >= 0;
    if (!(m->link_mask & 1u)) return 0;
    if (chain_set_index(set, m->frag) >= 0) return 1;
    if (mod_mat_census_on()) atomic_fetch_add_explicit(&s_cen_chain_absent, 1u, memory_order_relaxed);
    return 0;
}

_Static_assert(OPENPETE_MATERIAL_SLOT_CHAIN >= MOD_MATERIAL_SLOTS, "chain ids start above the slot ids");
_Static_assert(OPENPETE_MATERIAL_SLOT_CHAIN + OPENPETE_MATERIAL_CHAINS <= OPENPETE_MATERIAL_SLOT_DECL,
               "chain ids stay below the declarative bit");

/** @brief Intern a chain entry into this present's table, reusing an equal one.
 *
 * @param c The entry.
 * @return The entry id, or -1 when the table is full. */
static int chain_intern(const mat_chain_t* c) {
    for (int i = 0; i < s_mat_chain_n; i++) {
        const mat_chain_t* e = &s_mat_chains[i];
        if (e->n != c->n) continue;
        int same = 1;
        for (int k = 0; k < c->n && same; k++) same = e->mat[k] == c->mat[k] && e->slot[k] == c->slot[k];
        if (same) return i;
    }
    if (s_mat_chain_n >= OPENPETE_MATERIAL_CHAINS) return -1;
    s_mat_chains[s_mat_chain_n] = *c;
    return s_mat_chain_n++;
}

/** @brief Keep a walk's chain in the tick's list, reusing an equal entry.
 *
 * @param c The chain.
 * @return The entry index, or -1 when the list is full. */
static int tick_chain_keep(const chain_walk_t* c) {
    for (int k = 0; k < s_tick_chain_n; k++) {
        const chain_walk_t* e = &s_tick_chain[k];
        int same = e->n == c->n;
        for (int j = 0; j < c->n && same; j++)
            same = e->mat[j] == c->mat[j] && e->len[j] == c->len[j] &&
                   (c->len[j] < 0 || memcmp(e->bytes[j], c->bytes[j], (size_t)c->len[j]) == 0);
        if (same) return k;
    }
    if (s_tick_chain_n >= OPENPETE_MATERIAL_CHAINS) return -1;
    s_tick_chain[s_tick_chain_n] = *c;
    return s_tick_chain_n++;
}

/** @brief Turn a walk's chain into this present's chain entry: each member's uniform slot, then the entry.
 *
 * A refine member interns its block (slot_intern); a declarative member
 * takes its static slot. A full slot table or chain table draws the moby
 * stock for this present, counted.
 * @param c The chain, highest-ranked first.
 * @return The moby field value: OPENPETE_MATERIAL_SLOT_CHAIN + id, or
 *         OPENPETE_MATERIAL_SLOT_STOCK. */
static int chain_field(const chain_walk_t* c) {
    mat_chain_t e;
    e.n = c->n;
    for (int k = 0; k < c->n; k++) {
        const int j = c->n - 1 - k;      /* entries hold the lowest-ranked member first */
        const int w = c->mat[j];
        int sl = s_mats[w].static_slot;
        if (c->len[j] >= 0) {
            sl = slot_intern(w, c->bytes[j], c->len[j]);
            if (sl < 0) sl = -1;
        }
        if (sl < 0) {
            if (mod_mat_census_on()) atomic_fetch_add_explicit(&s_cen_chain_full, 1u, memory_order_relaxed);
            return OPENPETE_MATERIAL_SLOT_STOCK;
        }
        e.mat[k] = (short)w;
        e.slot[k] = (short)sl;
    }
    const int id = chain_intern(&e);
    if (id < 0) {
        if (mod_mat_census_on()) atomic_fetch_add_explicit(&s_cen_chain_full, 1u, memory_order_relaxed);
        return OPENPETE_MATERIAL_SLOT_STOCK;
    }
    return OPENPETE_MATERIAL_SLOT_CHAIN + id;
}

/* The outcome of an ask: a declarative acceptance, a decline, or a refine
 * block's byte count. */
#define MOBY_ASK_DECL (-2)
#define MOBY_ASK_DECLINE (-1)

/** @brief Ask one matching material whether it takes a moby: a declarative takes it, a refine runs inside a guarded stretch owned by its mod.
 *
 * @param w Material index.
 * @param cls Moby class.
 * @param vaddr Guest address of the moby.
 * @param buf Receives an accepting refine's block, OPENPETE_MATERIAL_UNIFORMS_MAX bytes.
 * @return MOBY_ASK_DECL, MOBY_ASK_DECLINE, or the accepted block's byte count. */
static int moby_ask(int w, int cls, uint32_t vaddr, unsigned char* buf) {
    mod_material_t* m = &s_mats[w];
    if (!m->per_instance) return MOBY_ASK_DECL;
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
    if (rc < 0) { cen_bump(w, MOD_MAT_CENSUS_DECLINES); return MOBY_ASK_DECLINE; }
    return rc > (int)OPENPETE_MATERIAL_UNIFORMS_MAX ? (int)OPENPETE_MATERIAL_UNIFORMS_MAX : rc;
}

/** @brief Add an accepted member to a walk's chain, dropping a vertex-stage member above it, which is linkable only as the lowest.
 *
 * @param c The chain.
 * @param w Material index.
 * @param a The ask's outcome: MOBY_ASK_DECL or the block's byte count.
 * @param buf The block. */
static void chain_push(chain_walk_t* c, int w, int a, const unsigned char* buf) {
    if (c->n && s_mats[c->mat[c->n - 1]].vert >= 0) c->n--;
    c->mat[c->n] = w;
    c->len[c->n] = a == MOBY_ASK_DECL ? -1 : a;
    if (a > 0) memcpy(c->bytes[c->n], buf, (size_t)a);
    c->n++;
}

/* The state of one moby's extraction walk: the top acceptance and its
 * block, the chain below it, and the member set it links against. */
typedef struct {
    int top, top_len;
    unsigned char* buf;             /* the top's block */
    chain_walk_t* cw;
    const openpete_mods_material_chain_set_t* set;
} walk_state_t;

/** @brief Take one acceptance into a walk: the top, or a member below it.
 *
 * The top draws alone, ending the walk, when it is terminal, has a vertex
 * stage, or is not linkable; otherwise it starts the chain. Below the top
 * an accepting member joins (an identity member does not), and a terminal
 * member or a full chain ends the walk.
 * @param st The walk.
 * @param w Material index.
 * @param a The ask's outcome: MOBY_ASK_DECL or the block's byte count.
 * @param tmp The block.
 * @return 1 when the walk stops, else 0. */
static int walk_accept(walk_state_t* st, int w, int a, const unsigned char* tmp) {
    const mod_material_t* m = &s_mats[w];
    const int identity = chain_identity(m);
    cen_bump(w, MOD_MAT_CENSUS_EXTRACT_WINS);
    if (st->top < 0) {
        st->top = w;
        st->top_len = a == MOBY_ASK_DECL ? -1 : a;
        if (st->top_len > 0) memcpy(st->buf, tmp, (size_t)st->top_len);
        if (mod_mat_terminal(m) || m->vert >= 0 || (!identity && !chain_linkable(m, st->set))) return 1;
        if (!identity) chain_push(st->cw, w, a, tmp);
        return 0;
    }
    if (!identity) chain_push(st->cw, w, a, tmp);
    return mod_mat_terminal(m) || st->cw->n == OPENPETE_MATERIAL_CHAIN_CAP;
}

/** @brief Finish a walk: a chain of two or more, else the one member that draws alone.
 *
 * One member under an identity top draws alone, the identity elided.
 * @param st The walk.
 * @param len Receives the alone member's block byte count, or -1.
 * @return The material that draws alone, or -1 for a chain or an exhausted walk. */
static int walk_result(walk_state_t* st, int* len) {
    chain_walk_t* cw = st->cw;
    if (cw->n >= 2) {
        if (mod_mat_census_on()) atomic_fetch_add_explicit(&s_cen_chain_walks, 1u, memory_order_relaxed);
        *len = -1;
        return -1;
    }
    *len = st->top_len;
    if (cw->n == 1 && cw->mat[0] != st->top) {
        *len = cw->len[0];
        if (*len > 0) memcpy(st->buf, cw->bytes[0], (size_t)*len);
        st->top = cw->mat[0];
    }
    cw->n = 0;
    return st->top;
}

/** @brief Run the extraction walk for one moby: every material down the priority order that takes it, to the first terminal one.
 *
 * The first acceptance is the top. A top that is terminal (pinned below
 * MOD_MAT_API_COMPOSE or OP_SEL_EXCLUSIVE), has a vertex stage, or is not
 * linkable (chain_linkable) draws alone, and the walk stops there. Below a
 * composable top the walk continues: an unlinkable member is skipped
 * without being asked, an accepting linkable member joins the chain, and
 * a terminal one ends the walk after joining; at OPENPETE_MATERIAL_CHAIN_CAP
 * members the walk stops, keeping the highest-ranked. An identity member
 * (its op_material returns its parameter) never joins; a terminal identity
 * still ends the walk. A decline passes the moby on, counted as a
 * fall-through when another material is asked after it.
 * @param cls Moby class.
 * @param vaddr Guest address of the moby.
 * @param buf Receives the single member's block, OPENPETE_MATERIAL_UNIFORMS_MAX bytes.
 * @param len Receives that block's byte count, or -1 for a declarative or no landing.
 * @param cw Receives the chain; cw->n is 2 or more when the moby composes.
 * @return The material that draws alone, -1 for a composed moby (cw) or an exhausted walk. */
static int moby_walk(int cls, uint32_t vaddr, unsigned char* buf, int* len, chain_walk_t* cw) {
    const int level = s_cur_level;
    walk_state_t st = { -1, -1, buf, cw, openpete_mods_material_chain_set_current() };
    int declined = -1;
    unsigned char tmp[OPENPETE_MATERIAL_UNIFORMS_MAX];
    cw->n = 0;
    for (int w = moby_walk_next(cls, level, -1); w >= 0; w = moby_walk_next(cls, level, w)) {
        const mod_material_t* m = &s_mats[w];
        if (st.top >= 0 && !chain_identity(m) && !chain_linkable(m, st.set)) continue;   /* skipped below the top */
        if (declined >= 0) cen_bump(declined, MOD_MAT_CENSUS_FALLTHROUGHS);
        const int a = moby_ask(w, cls, vaddr, tmp);
        if (a == MOBY_ASK_DECLINE) { declined = w; continue; }
        declined = -1;
        if (walk_accept(&st, w, a, tmp)) break;
    }
    return walk_result(&st, len);
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

/** @brief Replay a tick memo entry: a stamped field, a kept refine block or a kept chain, re-interned for this present.
 *
 * @param out The memo value. */
static int moby_memo_replay(int out) {
    cen_memo_replay();
    if (out & MEMO_CHAIN) return chain_field(&s_tick_chain[out & (MEMO_REFINE - 1)]);
    if (!(out & MEMO_REFINE)) return out;
    const mat_slot_t* o = &s_tick_out[out & (MEMO_REFINE - 1)];
    return moby_refine_slot(o->mat, o->bytes, (int)o->len);
}

int openpete_mods_material_moby_slot(CPUState* cpu, int moby_class,
                                     uint32_t moby_vaddr) {
    (void)cpu;   /* refine reads guest RAM through the mod api's accessors */
    if (!s_mat_has_instance) return 0;
    if (mod_mat_census_on() && moby_class >= 0 && moby_class < CEN_CLASSES)
        atomic_fetch_add_explicit(&s_cen_class[moby_class], 1u, memory_order_relaxed);
    const unsigned stamp = memo_stamp();
    moby_memo_t* e = moby_memo_find(stamp, moby_vaddr, moby_class);
    if (e && e->stamp == stamp) return moby_memo_replay(e->out);
    unsigned char buf[OPENPETE_MATERIAL_UNIFORMS_MAX];
    static chain_walk_t cw;   /* sim thread only; 1 KiB of blocks off the stack */
    int len;
    const int w = moby_walk(moby_class, moby_vaddr, buf, &len, &cw);
    int out = OPENPETE_MATERIAL_SLOT_STOCK, keep;
    if (cw.n >= 2) {
        /* A full slot or chain table is this present's limit, not the tick's answer. */
        out = chain_field(&cw);
        keep = out == OPENPETE_MATERIAL_SLOT_STOCK ? -1 : tick_chain_keep(&cw);
        if (keep < 0) e = NULL;
        else keep |= MEMO_REFINE | MEMO_CHAIN;
    } else if (w >= 0 && len >= 0) {
        out = moby_refine_slot(w, buf, len);
        keep = out == OPENPETE_MATERIAL_SLOT_STOCK ? -1 : tick_out_keep(w, buf, len);
        if (keep < 0) e = NULL;
        else keep |= MEMO_REFINE;
    } else {
        if (w >= 0) out = OPENPETE_MATERIAL_SLOT_DECL | w;
        keep = out;
    }
    if (e) {
        e->vaddr = moby_vaddr; e->cls = moby_class; e->stamp = stamp; e->out = keep;
        s_moby_memo_n++;
    }
    return out;
}
/* ------------------------------------------------------------------ */
/* Terrain refine outcomes and class resolution. */

/** @brief Census: count a terrain refine call as outranked when a higher declarative matches its key on some face.
 *
 * A usable declarative material ranked above the refine (mod_mat_outranks)
 * whose texture key is -1 or the refine's and whose texturedness flags
 * admit the refine's band matches the refine's key on some face; env and
 * tier are face keys the refine key does not carry.
 * @param w The refine material. */
static void census_terrain_refine(int w) {
    const mod_material_t* r = &s_mats[w];
    const int untex = (r->sel.flags & OP_SEL_UNTEXTURED) != 0;
    const int level = openpete_mods_current_level();
    for (int i = 0; i < s_mat_count; i++) {
        const mod_material_t* m = &s_mats[i];
        if (i == w || m->per_instance || !terrain_sel_usable(m, level)) continue;
        if (m->sel.texture != -1 && (untex || m->sel.texture != r->sel.texture)) continue;
        if ((m->sel.flags & OP_SEL_TEXTURED) && untex) continue;
        if ((m->sel.flags & OP_SEL_UNTEXTURED) && !untex) continue;
        if (mod_mat_outranks(i, w)) { cen_bump(w, MOD_MAT_CENSUS_OUTRANKED); return; }
    }
}

/* The refine outcome of one per-instance terrain material, computed once
 * per tick on the sim thread. A terrain refine selector carries one key,
 * a Tiledef id or the untextured band, so the material's outcome is the
 * outcome of every face it reaches and one memo per material serves every
 * class it is a member of, walked and retained alike. The slot memo is
 * keyed on the present counter openpete_mods_material_frame_reset()
 * advances; the view captures it for the compose
 * (mod_mat_terrain_bucket_slot). The block itself is kept for the tick
 * (memo_stamp), and a later present of the tick re-interns it without a
 * call. The callback runs inside a guarded stretch owned by the material's
 * mod and receives a terrain key with the selector's texture (-1 for the
 * untextured band); its block, capped at OPENPETE_MATERIAL_UNIFORMS_MAX,
 * reuses an interned slot of the same material with identical bytes, else
 * takes a new slot. */
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
    refine_observe(m, rc < 0);
    if (mod_mat_census_on()) census_terrain_refine(w);
    if (rc > (int)sizeof buf) rc = (int)sizeof buf;
    m->refine_stamp = stamp;
    m->refine_kept_len = rc < 0 ? -1 : rc;
    if (rc < 0) return m->refine_slot;          /* declined -> stock */
    memcpy(m->refine_kept, buf, (size_t)rc);
    int s = slot_intern(w, buf, rc);
    if (s < 0) return m->refine_slot;           /* table full -> graceful stock */
    return (m->refine_slot = s);
}

/** @brief Resolve a terrain class to its member for this present.
 *
 * Walks the class's members, highest ranked first, skipping one no longer
 * usable (terrain_sel_usable): a declarative member lands; a refine member
 * is asked through mod_mat_terrain_refine_slot and lands on acceptance,
 * while its decline passes the class to the next member, counted as a
 * fall-through when one follows. An accepted block that finds the slot
 * table full resolves to stock for this present. Memoised per present.
 * Sim thread only.
 * @param c Class id 1..s_cls_n-1.
 * @return The material index, or -1 for stock. */
static int ter_class_resolve(int c) {
    ter_class_t* k = &s_cls[c];
    if (k->res_present == s_mat_present) return k->res;
    k->res_present = s_mat_present;
    k->res = -1;
    const int level = s_cur_level;
    int declined = -1;
    for (int j = 0; j < k->n; j++) {
        const int i = k->m[j];
        const mod_material_t* m = &s_mats[i];
        if (!terrain_sel_usable(m, level)) continue;
        if (declined >= 0) cen_bump(declined, MOD_MAT_CENSUS_FALLTHROUGHS);
        if (!m->per_instance) { k->res = i; break; }
        const int s = mod_mat_terrain_refine_slot(i);
        if (s > 0 && s != OPENPETE_MATERIAL_TERRAIN_STOCK) { k->res = i; break; }
        if (m->refine_kept_len >= 0) break;     /* accepted, slot table full: stock */
        declined = i;
    }
    return k->res;
}

int openpete_mods_material_terrain_walk_slot(CPUState* cpu, int texid, int untex, int env, int lp) {
    (void)cpu;   /* refines read guest RAM through the mod api's accessors */
    if (s_mat_count == 0) return 0;
    const int c = ter_class_of(texid, untex, env, lp);
    const int r = c ? ter_class_resolve(c) : -1;
    if (mod_mat_census_on() && texid >= -1 && texid <= 127)
        cen_ter(s_cen_walk_n, s_cen_walk_b, ter_key_index(texid < 0 ? -1 : texid, untex, env, lp), r + 1);
    if (r < 0) return OPENPETE_MATERIAL_TERRAIN_STOCK;
    if (!s_mats[r].per_instance) return OPENPETE_MATERIAL_TERRAIN_DECL | r;
    return s_mats[r].refine_slot;
}

void openpete_mods_material_terrain_refine_present(void) {
    if (s_mat_count == 0) return;
    /* The classes the retained faces carry: the walk resolves the classes
     * it reaches itself, and a class whose faces are all retained has no
     * walked face to resolve it. A refine is asked only as a member of a
     * class above its key's declarative winner, so an outranked refine is
     * never asked. */
    const unsigned ep = openpete_mods_material_terrain_epoch();
    for (int c = 1; c < s_cls_n; c++)
        if (s_cls[c].bake_epoch == ep) (void)ter_class_resolve(c);
}

int mod_mat_terrain_bucket_slot(int w) {
    if (w < 0 || w >= s_mat_count) return OPENPETE_MATERIAL_TERRAIN_STOCK;
    const mod_material_t* m = &s_mats[w];
    int slot = 0;
    if (s_mat_view) slot = s_mat_view->ter_refine[w];
    else if (m->refine_present == s_mat_present) slot = m->refine_slot;
    if (slot <= 0 || slot == OPENPETE_MATERIAL_TERRAIN_STOCK) {
        /* The pass list holds a refine only when a class resolved to it, so
         * a missing outcome is a pass from another present's tables. */
        cen_bump(w, MOD_MAT_CENSUS_STALE_MEMO);
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
 * [15:0] + walk outcome [27:16]; terrain env bit 27 / LP bit 26 /
 * untextured bit 25 + class outcome [24:16] + sector [8:0]. Rank:
 * mod_mat_outranks. */
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
 * carries its face class's resolved member in bits [24:16]
 * (OPENPETE_MATERIAL_TERRAIN_STOCK = stock,
 * OPENPETE_MATERIAL_TERRAIN_DECL | index = a declarative landing, else an
 * interned slot). Slot 0 defers to the declarative resolve. A slot at or
 * past the view's slot count is stale, and a slot or landing whose
 * material is no longer usable (material_usable) does not resolve.
 * @param tag Provenance tag.
 * @param path 0 moby, 1 terrain; other paths carry no slot.
 * @param slot Receives the slot on a hit, or the material index of a
 *        declarative landing.
 * @return 1 with the slot, 2 with a declarative landing's material, 0 to
 *         fall through to the declarative resolve, -1 when the outcome is
 *         stock or no longer resolves. */
static int material_slot_from_tag(uint32_t tag, int path, int* slot) {
    int s, decl, stock;
    if (path == 0) {
        s = (int)((tag >> 16) & 0xFFFu);
        decl = OPENPETE_MATERIAL_SLOT_DECL; stock = OPENPETE_MATERIAL_SLOT_STOCK;
    } else if (path == 1) {
        s = (int)((tag >> 16) & 0x1FFu);
        decl = OPENPETE_MATERIAL_TERRAIN_DECL; stock = OPENPETE_MATERIAL_TERRAIN_STOCK;
    } else {
        return 0;
    }
    if (s == stock) return -1;
    /* A chain entry resolves through mod_mat_resolve_chain only. */
    if (path == 0 && s >= OPENPETE_MATERIAL_SLOT_CHAIN && s < OPENPETE_MATERIAL_SLOT_CHAIN + OPENPETE_MATERIAL_CHAINS)
        return -1;
    if (s & decl) {
        s &= decl - 1;
        if (!material_usable(s)) return -1;
        *slot = s;
        return 2;
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

/** @brief Census: record a draw-time outcome per material.
 *
 * @param best The resolved material, or -1 for stock. */
static void census_resolve(int best) {
    if (mod_mat_census_on()) cen_bump(best, MOD_MAT_CENSUS_DRAWS);
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
    switch (from_tag) {
    case 1:
    case 2:
        /* A slot or landing whose material has another contract draws stock;
         * a landing binds its material's static block. */
        best = from_tag == 1 ? view_slot_mat(*slot) : *slot;
        if (from_tag == 2) *slot = 0;
        if (s_mats[best].contract != contract || mod_owner_held(s_mats[best].owner)) best = -1;
        census_resolve(best);
        return best;
    case -1: census_resolve(-1); return -1;
    default: break;   /* declarative resolve */
    }
    *slot = 0;
    best = material_winner(&k, contract);
    if (best >= 0 && mod_owner_held(s_mats[best].owner)) best = -1;
    census_resolve(best);
    return best;
}

/** @brief Census: count one chain draw sent to stock.
 *
 * @param c The counter. */
static void cen_chain(_Atomic unsigned* c) {
    if (mod_mat_census_on()) atomic_fetch_add_explicit(c, 1u, memory_order_relaxed);
}


/** @brief Return the chain entry id a moby tag names, or -1 when it names none. */
static int chain_id_of_tag(uint32_t tag) {
    if ((tag >> 28) != 1u) return -1;
    const int f = (int)((tag >> 16) & 0xFFFu);
    return f >= OPENPETE_MATERIAL_SLOT_CHAIN && f < OPENPETE_MATERIAL_SLOT_CHAIN + OPENPETE_MATERIAL_CHAINS
               ? f - OPENPETE_MATERIAL_SLOT_CHAIN : -1;
}

/** @brief Read a chain entry of the installed view, else of the live table.
 *
 * @param id Entry id.
 * @param mat Receives its members' material indices, lowest-ranked first.
 * @param slot Receives its members' uniform slots.
 * @return The member count, or 0 for an id past the entry count. */
static int chain_entry(int id, const short** mat, const short** slot) {
    if (s_mat_view) {
        if (id >= s_mat_view->chain_n) return 0;
        *mat = s_mat_view->chains[id].mat;
        *slot = s_mat_view->chains[id].slot;
        return s_mat_view->chains[id].n;
    }
    if (id >= s_mat_chain_n) return 0;
    *mat = s_mat_chains[id].mat;
    *slot = s_mat_chains[id].slot;
    return s_mat_chains[id].n;
}

/** @brief Check one member of a stamped chain at compose: usable at the contract, its owner not held, its fragment shader in the installed set.
 *
 * @param m Material index.
 * @param contract The draw site's contract.
 * @param set The installed member set.
 * @param idx Receives the member's index in @p set, -1 for a vertex-only member.
 * @return NULL when the member draws, else the census counter of the reason the chain draws stock. */
static _Atomic unsigned* chain_member_check(int m, int contract, const openpete_mods_material_chain_set_t* set,
                                            int* idx) {
    if (!material_usable(m) || s_mats[m].contract != contract) return &s_cen_chain_unusable;
    if (mod_owner_held(s_mats[m].owner)) return &s_cen_chain_held;
    *idx = s_mats[m].frag >= 0 ? chain_set_index(set, s_mats[m].frag) : -1;
    return s_mats[m].frag >= 0 && *idx < 0 ? &s_cen_chain_lacked : NULL;
}

/** @brief Census: count one chain draw by its member count, and a draw for each member. */
static void census_chain_draw(const mod_mat_chain_res_t* r) {
    if (!mod_mat_census_on()) return;
    atomic_fetch_add_explicit(&s_cen_chain_draws[r->n], 1u, memory_order_relaxed);
    for (int k = 0; k < r->n; k++) cen_bump(r->mat[k], MOD_MAT_CENSUS_DRAWS);
}

/** @brief Resolve a chain tag at a draw site (mod_mat_resolve_chain), counting in the census only when @p count is set.
 *
 * @param tag Provenance tag.
 * @param contract The draw site's contract.
 * @param out Receives the members.
 * @param count Nonzero to count.
 * @return 1 resolved, 0 when the tag names no chain, -1 when the chain draws stock. */
static int resolve_chain(uint32_t tag, int contract, mod_mat_chain_res_t* out, int count) {
    const int id = s_mat_count ? chain_id_of_tag(tag) : -1;
    if (id < 0) return 0;
    const short *mat = NULL, *slot = NULL;
    const int n = chain_entry(id, &mat, &slot);
    _Atomic unsigned* fail = n < 2 || n > OPENPETE_MATERIAL_CHAIN_CAP || contract != OPENPETE_MATERIAL_CONTRACT_SCENE
                                 ? &s_cen_chain_stale : NULL;
    out->entry = id;
    out->n = n;
    out->set = openpete_mods_material_chain_set_current();
    for (int k = 0; k < n && !fail; k++) {
        out->mat[k] = mat[k];
        out->slot[k] = slot[k];
        fail = chain_member_check(mat[k], contract, out->set, &out->set_index[k]);
    }
    if (fail) {
        if (count) cen_chain(fail);
        return -1;
    }
    if (count) census_chain_draw(out);
    return 1;
}

int mod_mat_resolve_chain(uint32_t tag, int contract, mod_mat_chain_res_t* out) {
    return resolve_chain(tag, contract, out, 1);
}

/** @brief Resolve a chain tag of the scene contract without counting, for the chain block fill.
 *
 * @param tag Provenance tag.
 * @param out Receives the members. */
static int mod_mat_resolve_chain_quiet(uint32_t tag, mod_mat_chain_res_t* out) {
    return resolve_chain(tag, OPENPETE_MATERIAL_CONTRACT_SCENE, out, 0);
}

int openpete_mods_material_chain_count(void) { return s_mat_chain_n; }

int openpete_mods_material_chain_block(int entry, int profile, const void* engine, uint32_t engine_len,
                                       void* out) {
    if (entry < 0 || entry >= OPENPETE_MATERIAL_CHAINS || profile < 0 ||
        profile >= OPENPETE_MATERIAL_CHAIN_PROFILES || !out)
        return -1;
    mod_mat_chain_res_t ch;
    const uint32_t tag = (1u << 28) | ((uint32_t)(OPENPETE_MATERIAL_SLOT_CHAIN + entry) << 16);
    if (mod_mat_resolve_chain_quiet(tag, &ch) != 1) return -1;
    unsigned char* o = (unsigned char*)out;
    memset(o, 0, OPENPETE_MATERIAL_CHAIN_BLOCK_BYTES);
    uint32_t n = 0, ids[OPENPETE_MATERIAL_CHAIN_CAP];
    for (int k = 0; k < ch.n; k++) {
        if (ch.set_index[k] < 0) continue;            /* a vertex-only member calls nothing */
        const int id = ch.set->case_id[profile][ch.set_index[k]];
        if (id < 0) return -1;                        /* that profile's module lacks it */
        unsigned char* blk = o + OPENPETE_MATERIAL_UNIFORMS_MAX * (1u + n);
        const int s = ch.slot[k];
        if (s > 0) {
            const uint32_t len = s_mat_view ? s_mat_view->slots[s].len : s_mat_slots[s].len;
            const unsigned char* b = s_mat_view ? s_mat_view->slots[s].bytes : s_mat_slots[s].bytes;
            memcpy(blk, b, len > OPENPETE_MATERIAL_UNIFORMS_MAX ? OPENPETE_MATERIAL_UNIFORMS_MAX : len);
        } else if (engine) {
            /* A member with no params reads the engine block, as its own draw binds it. */
            memcpy(blk, engine, engine_len > OPENPETE_MATERIAL_UNIFORMS_MAX ? OPENPETE_MATERIAL_UNIFORMS_MAX
                                                                            : engine_len);
        }
        ids[n++] = (uint32_t)id;
    }
    memcpy(o, &n, sizeof n);
    memcpy(o + 16, ids, n * sizeof *ids);    /* std140: uvec4 op_chain_ids at offset 16 */
    return 0;
}

/* ------------------------------------------------------------------ */
/* Overlap diagnostic. */

/* A material's key set per channel. A moby class or a channel identity is
 * either every value, one value, or none: every selector key is -1 or one
 * value, so the resolver's predicates, asked at the material's own key and
 * at a value no selector names, decide which. Terrain keys are enumerated:
 * the face keys of the class table (ter_key_index). */
enum { OVL_NONE = 0, OVL_ONE, OVL_ALL };
#define OVL_SENTINEL (-0x7FFFFFFF)   /* a class or identity no selector names */
#define OVL_TER_KEYS TER_KEYS
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

/** @brief Set one bit per terrain face key a material is a candidate for.
 *
 * Keys a face cannot carry (a Tiledef id on an untextured face) stay clear.
 * @param m The material.
 * @param level The pushed level.
 * @param bits Receives OVL_TER_WORDS words, bit ter_key_index(texid, untex, env, lp). */
static void ovl_terrain_bits(const mod_material_t* m, int level, uint64_t* bits) {
    memset(bits, 0, OVL_TER_WORDS * sizeof *bits);
    if (!terrain_sel_usable(m, level)) return;
    for (int k = 0; k < OVL_TER_KEYS; k++) {
        const int texid = k / 8 - 1, untex = (k >> 2) & 1, env = (k >> 1) & 1, lp = k & 1;
        if (untex && texid >= 0) continue;
        const int hit = m->per_instance ? terrain_refine_key_matches(m, untex, texid)
                                        : terrain_decl_matches(m, texid, untex, env, lp);
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
 * Full when the higher takes every key of the lower: on the moby walk and
 * on terrain, where a refine's decline passes the key to the next
 * candidate, a declarative takes the keys it matches, and a refine takes
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
    const int takes = !h->refine || (h->asked && !h->declined);
    *chans = ovl_ter_meets(h, l, &within) ? 1u << OP_CHAN_TERRAIN : 0u;
    int full = within;
    if (!takes)
        for (int w = 0; w < OVL_TER_WORDS; w++)
            if (l->ter[w]) full = 0;
    for (int ch = OP_CHAN_MOBY; ch <= OP_CHAN_PORTAL; ch++) {
        if (ch == OP_CHAN_TERRAIN || !l->ch[ch].kind) continue;
        if (ovl_meets(l->ch[ch], h->ch[ch])) *chans |= 1u << ch;
        if (!takes || !ovl_within(l->ch[ch], h->ch[ch])) full = 0;
    }
    *accept_all = full && h->refine && (*chans & (1u << OP_CHAN_MOBY | 1u << OP_CHAN_TERRAIN)) != 0;
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
    const char* why = !h->refine || !(chans & (1u << OP_CHAN_MOBY | 1u << OP_CHAN_TERRAIN)) ? ""
                    : aa ? " (accept-all refine)"
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
