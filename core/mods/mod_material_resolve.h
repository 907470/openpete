/* mod_material_resolve.h - the material table and its resolution rules.
 *
 * The material selector tier's state (the material table, the per-present
 * interned slot table, the pushed level, the owner hold list) and every
 * rule that picks a material for a key: the selector predicates, the
 * winner scans of the moby, terrain and draw-time paths, the refine calls
 * and their slot interning, and the postfx pass ranking. No GPU, glslang
 * or SDL dependency: mod_shader.c links it and adds the shader handles,
 * pipelines and the registry lock; tests/unit/mod_material_resolve_test.c
 * links it against stub loader hooks.
 *
 * The loader hooks it calls: openpete_mods_is_enabled,
 * openpete_mods_guard_enter / _leave, openpete_mods_toggle_epoch,
 * openpete_mods_id_of, openpete_mods_in_present, and the renderer's
 * openpete_native_set_tag_split. */
#ifndef OPENPETE_MOD_MATERIAL_RESOLVE_H
#define OPENPETE_MOD_MATERIAL_RESOLVE_H

#include "openpete_mods.h"

#include <stdint.h>

/* A real bound, not a table size: a retained-terrain pass id is the
 * material index + 1 in 8 bits (OPENPETE_MATERIAL_TER_BUCKET_MAX), and a
 * walked terrain tag lands on OPENPETE_MATERIAL_TERRAIN_DECL | index in 9.
 * A material registered above index 254 would succeed and then never
 * shade a terrain face. */
#define MOD_MATERIAL_MAX OPENPETE_MATERIAL_TER_BUCKET_MAX

/* Per-present interned per-instance slots. Index 0 reserved
 * (= "defer to draw-time class resolve"); 1.. are interned outcomes. */
#define MOD_MATERIAL_SLOTS 128

/* The first manifest api pin whose same-mod material ties go to the later
 * registration; a lower pin keeps the first registration. */
#define MOD_MAT_API_LATER_TIE 13
/* The first manifest api pin whose materials compose: a material of a mod
 * pinned below it is terminal. */
#define MOD_MAT_API_COMPOSE 13

typedef struct {
    const char* owner;
    openpete_mod_material_selector_t sel;
    int  vert, frag;       /* shader handles; -1 = stock stage */
    openpete_mod_material_fn per_instance;   /* NULL = declarative */
    int  load_index;
    /* The owner's manifest api pin. It selects the same-mod tie direction:
     * below MOD_MAT_API_LATER_TIE the first registration wins, from it the
     * later one does (mod_mat_outranks). */
    int  api_pin;
    /* OPENPETE_MATERIAL_CONTRACT_* of the fragment stage; SCENE for a
     * material with the stock fragment stage. */
    int  contract;
    /* Composition verdicts of the fragment stage, copied from its shader at
     * registration and again when openpete_mods_shader_link_resolve decides
     * them (mod_mat_link.h): bit p (0 stock, 1 terrain, 2 portal) of
     * link_mask is set when that profile is linkable into a combined module
     * (0 while pending), of identity_mask when its op_material returns its
     * parameter. Both 0 for the stock fragment stage. */
    unsigned char link_mask, identity_mask;
    /* One built pipeline per draw-state variant this material can be
     * asked for: [0..3] = PSX blend classes (0 opaque, 1 average,
     * 2 additive, 3 subtract; a material reshades inside whatever group
     * it lands in and must not change how that group composites), [4] =
     * opaque with depth write off (portal sky), [5] = the DECAL PROFILE
     * (moby shadows: a different vertex stage, descriptor sets and push
     * block - see openpete_psx_decal.glsl). Executor-cached rhi_pipeline*. */
    void* pipeline[OPENPETE_MATERIAL_PIPE_VARIANTS];
    void* pipeline_mark;   /* stencil-MARK twin (hole-fill WORLD/ACTOR groups) */
    /* Retained-terrain pipelines, one per (arena vertex stage x colour
     * slot). Separate from `pipeline` because the terrain layout, the
     * vertex input and the engine half of the contract all differ. */
    void* pipeline_ter[OPENPETE_MATERIAL_TER_PIPES];
    /* Portal-profile pipelines (OPENPETE_MATERIAL_PORTAL_PIPES). */
    void* pipeline_portal[OPENPETE_MATERIAL_PORTAL_PIPES];
    int   user_enabled;    /* material_enable - the mod's own on/off */
    /* Static uniform block (material_set_params). Declarative materials have
     * no per-instance callback to mint a slot, so the block is interned into
     * one at every frame reset and reused for the whole present. */
    unsigned char params[OPENPETE_MATERIAL_UNIFORMS_MAX];
    uint32_t      params_len;
    int           static_slot;
    /* Per-instance TERRAIN materials: the refine outcome for this present,
     * memoized so the callback runs ONCE however many faces and resolution
     * classes need it and whichever residency they are in. A terrain refine
     * selector has exactly one key (its Tiledef id, or the untextured band),
     * so one slot covers the whole material. */
    int           refine_slot;
    unsigned      refine_present;
    /* The same outcome kept for the tick (memo stamp), so a later present
     * of the tick re-interns it without a call: the block, or -1 bytes for
     * a decline. */
    unsigned      refine_stamp;
    int           refine_kept_len;
    unsigned char refine_kept[OPENPETE_MATERIAL_UNIFORMS_MAX];
    /* A selector retuned from inside a present (material_set_selector) parks
     * here until the tick boundary. Selection has to be constant for a whole
     * present or half the frame shades under the old key. */
    openpete_mod_material_selector_t sel_pending;
    int           sel_dirty;
    /* Refine calls and declines (moby extraction and terrain) since
     * registration or the last selector change; the overlap diagnostic
     * reports a refine called at least once that never declined as
     * accept-all. */
    unsigned      obs_calls, obs_declines;
    int  live;
    unsigned generation;   /* slot_generation_next at register and at each retirement */
} mod_material_t;

/** @brief Return the number of material table rows, live or swept.
 *
 * @return The row count. */
int mod_mat_count(void);
/** @brief Return the material table row at an index.
 *
 * @param i Index below mod_mat_count().
 * @return The row; the caller bounds the index. */
mod_material_t* mod_mat_at(int i);

/** @brief Empty the material table and clear the level, slot table, view and hold list.
 *
 * The engine never calls it; the resolver unit test starts each case
 * from an empty table. */
void mod_mat_reset(void);

/** @brief Check that this engine can honour a material selector exactly as written.
 *
 * Refuses, with an error log, a struct_size other than this SDK's, flag
 * bits outside OP_SEL_FLAGS_KNOWN, OP_SEL_ENV_ANIM with OP_SEL_ENV_STATIC,
 * OP_SEL_EXCLUSIVE on a mod pinned below
 * MOD_MAT_API_COMPOSE, a channel other than -1 outside
 * OP_CHAN_MOBY..OP_CHAN_PORTAL, and a texture key other than a Tiledef id
 * 0..127 on channel -1 or OP_CHAN_TERRAIN without OP_SEL_UNTEXTURED. The
 * flag check exists because a new flag bit never changes sizeof. Register
 * and set_selector share it, so a retune cannot install a key the register
 * path would refuse.
 * @param id Mod id for log lines.
 * @param sel The selector.
 * @param api_pin The owner's manifest api pin.
 * @return 1 when valid, 0 when refused. */
int mod_mat_sel_valid(const char* id, const openpete_mod_material_selector_t* sel, int api_pin);

/** @brief Check that a refine material can take effect where its selector points.
 *
 * Refine callbacks are asked on the moby and terrain channels only, and
 * only for scene-contract materials. Refuses, with an error log naming
 * the rule, a channel other than -1, OP_CHAN_MOBY and OP_CHAN_TERRAIN, a
 * contract other than OPENPETE_MATERIAL_CONTRACT_SCENE, and a terrain
 * selector with neither a texture key nor OP_SEL_UNTEXTURED. Registration
 * of a refine material and every retune of one (before a retune is parked)
 * share it; a refusal leaves a retuned material's selector in force.
 * @param id Mod id for log lines.
 * @param sel The selector.
 * @param contract OPENPETE_MATERIAL_CONTRACT_* of the material's fragment stage.
 * @return 1 when the refine can take effect, 0 when refused. */
int mod_mat_refine_valid(const char* id, const openpete_mod_material_selector_t* sel,
                         int contract);

/** @brief Append a validated material to the table, registered enabled.
 *
 * Refuses a material with neither stage and a full table. Bumps the
 * terrain epoch when the material could bucket terrain and re-arms the tag
 * split. The caller holds the registry write lock and has validated the
 * selector and the shader handles.
 * @param owner Mod id (loader-owned string).
 * @param sel The selector.
 * @param vert Vertex shader handle, or -1 for the stock stage.
 * @param frag Fragment shader handle, or -1 for the stock stage.
 * @param contract OPENPETE_MATERIAL_CONTRACT_* of the fragment stage.
 * @param per_instance Refine callback, or NULL for a declarative material.
 * @param load_index The owner's enabled-list position.
 * @param api_pin The owner's manifest api pin.
 * @param generation A fresh slot generation.
 * @return The material handle, or -1. */
int mod_mat_append(const char* owner, const openpete_mod_material_selector_t* sel,
                   int vert, int frag, int contract,
                   openpete_mod_material_fn per_instance, int load_index,
                   int api_pin, unsigned generation);

/** @brief Record the composition verdicts of a fragment shader on every material it shades: its linkable and identity profiles.
 *
 * The caller holds the registry write lock.
 * @param frag The fragment shader handle.
 * @param link_mask Bit p set for each linkable profile p.
 * @param identity_mask Bit p set for each identity profile p. */
void mod_mat_set_link_of_frag(int frag, unsigned link_mask, unsigned identity_mask);

/** @brief Test whether material @p a ranks above material @p b.
 *
 * The owner with the higher load_index (later in the enabled list) ranks
 * higher. Two materials of one mod rank by registration: the later one
 * ranks higher when the owner's api pin is at least MOD_MAT_API_LATER_TIE,
 * the earlier one otherwise. Every winner scan ranks through it.
 * @param a Material index.
 * @param b Material index, or -1 for none (every material outranks none).
 * @return 1 when @p a ranks above @p b, else 0. */
int mod_mat_outranks(int a, int b);

/** @brief Turn one material on or off; the caller holds the registry write lock.
 *
 * @param material Material handle.
 * @param on Nonzero to enable.
 * @return 0, or -1 for an unknown or swept material. */
int mod_mat_enable(int material, int on);

/** @brief Copy a material's static uniform block; the caller holds the registry write lock.
 *
 * @param material Material handle.
 * @param block The bytes; may be NULL when @p len is 0.
 * @param len Byte count, at most OPENPETE_MATERIAL_UNIFORMS_MAX.
 * @return 0, or -1 for an unknown or swept material, an oversize block, or
 *         a NULL block with nonzero @p len. */
int mod_mat_set_params(int material, const void* block, uint32_t len);

/** @brief Replace a live material's selector; the caller holds the registry write lock.
 *
 * Only the owning mod may retune, and the selector must pass
 * mod_mat_sel_valid and, for a refine material, mod_mat_refine_valid. Inside a present the new selector is parked for
 * mod_mat_apply_pending; outside one it applies at once.
 * @param id The calling mod's id.
 * @param material Material handle.
 * @param sel The new selector.
 * @return 0, or -1 for a NULL argument, an unknown or swept material, a
 *         material another mod owns, an invalid selector, or a refine
 *         selector mod_mat_refine_valid refuses. */
int mod_mat_set_selector(const char* id, int material,
                         const openpete_mod_material_selector_t* sel);

/** @brief Return whether any material carries a selector parked by a retune inside a present.
 *
 * @return Nonzero when a selector is parked. */
int mod_mat_pending(void);

/** @brief Install the parked selectors; the caller holds the registry write lock. */
void mod_mat_apply_pending(void);

/** @brief Advance the terrain epoch after a change that can move a terrain bucket. */
void mod_mat_ter_epoch_bump(void);

/** @brief Recompute the per-instance flag and arm the batcher tag split from the live materials. */
void mod_mat_rearm_split(void);

/** @brief Return the static uniform slot of a material in the installed view, else in the live table.
 *
 * @param m Material index below mod_mat_count().
 * @return The slot, or 0 for no params. */
int mod_mat_view_static_slot(int m);

/** @brief Return the refine outcome of one per-instance terrain material for this present.
 *
 * The callback runs at most once per present per material (memo keyed on
 * the counter openpete_mods_material_frame_reset advances) and once per
 * tick (a later present of the tick re-interns the kept block); a block
 * identical to an interned slot of the same material reuses that slot.
 * Sim thread only: the resolution of a terrain class that has the
 * material as a member.
 * @param w Material index, or -1 for none.
 * @return 0 for no material or a declarative one, an interned slot, or
 *         OPENPETE_MATERIAL_TERRAIN_STOCK when the callback declined or
 *         the slot table is full. */
int mod_mat_terrain_refine_slot(int w);

/** @brief Return the refine outcome a retained-terrain material pass binds, without calling a callback.
 *
 * Reads the installed view's outcome for the material, else, with no view
 * installed, the material's memo when it was computed this present. A memo
 * from another present, a view with no outcome for the material, and a
 * declined outcome read as stock and count in the census
 * (MOD_MAT_CENSUS_STALE_MEMO): the pass list holds a refine only when a
 * class resolved to it.
 * @param w Material index of a per-instance material.
 * @return An interned slot, or OPENPETE_MATERIAL_TERRAIN_STOCK. */
int mod_mat_terrain_bucket_slot(int w);

/** @brief Return the number of terrain resolution classes interned this session.
 *
 * @return The count; class ids run 1 to the count. */
int mod_mat_terrain_class_count(void);

/** @brief Return the member a terrain resolution class resolved to this present.
 *
 * Reads the live memo, without resolving.
 * @param c Class id.
 * @return The material index, or -1 for stock, a class not resolved this
 *         present, or no such class. */
int mod_mat_terrain_class_resolved(int c);

/** @brief Pick the material that shades a draw's provenance tag, without the GPU description.
 *
 * The draw-time half of openpete_mods_material_resolve: a slot the tag
 * carries names its material (stock when the slot declined, is stale, is
 * unusable, has another contract or a held owner); slot 0 runs the
 * declarative scan.
 * @param tag Provenance tag.
 * @param contract The draw site's OPENPETE_MATERIAL_CONTRACT_*.
 * @param slot Receives the uniform slot (0 for the static block).
 * @return The material index, or -1 for stock. */
int mod_mat_resolve_index(uint32_t tag, int contract, int* slot);

/** @brief Return whether a material is terminal: pinned below MOD_MAT_API_COMPOSE or carrying OP_SEL_EXCLUSIVE.
 *
 * A terminal material takes no material beneath it; a material that is
 * not terminal composes over the accepting materials below it.
 * @param m The material.
 * @return 1 when terminal, else 0. */
int mod_mat_terminal(const mod_material_t* m);

/** @brief Return the material table generation, which moves on every table change and level change.
 *
 * @return The generation. */
unsigned mod_mat_table_gen(void);

/** @brief A moby chain draw resolved at compose time. */
typedef struct {
    int entry;      /* chain entry id in the installed view */
    int n;          /* members, lowest-ranked first */
    int mat[OPENPETE_MATERIAL_CHAIN_CAP];
    int slot[OPENPETE_MATERIAL_CHAIN_CAP];        /* uniform slot in the view; 0 = no params */
    int set_index[OPENPETE_MATERIAL_CHAIN_CAP];   /* index in `set`; -1 for a vertex-only member */
    const openpete_mods_material_chain_set_t* set; /* the installed member set it maps to */
} mod_mat_chain_res_t;

/** @brief Resolve a moby tag that names a chain entry into its members and their places in the installed member set.
 *
 * A chain draws stock, counted in the census, when its entry is past the
 * view's chain count (stale), the draw site's contract is not the scene
 * contract, a member is no longer usable or has another contract, a
 * member's owner is held for this compose, or the installed set lacks a
 * member's fragment shader (a set replaced since the extraction).
 * @param tag Provenance tag.
 * @param contract The draw site's OPENPETE_MATERIAL_CONTRACT_*.
 * @param out Receives the members on a resolve.
 * @return 1 resolved, 0 when the tag names no chain, -1 when the chain draws stock. */
int mod_mat_resolve_chain(uint32_t tag, int contract, mod_mat_chain_res_t* out);

/** @brief Return whether the compose in progress holds an owner's passes and materials back.
 *
 * @param owner Mod id.
 * @return 1 when held, else 0. */
int mod_owner_held(const char* owner);

/** @brief One enabled postfx pass, the keys its rank is computed from. */
typedef struct {
    int index;        /* postfx table index */
    int load_index;   /* owner's enabled-list position */
    int order;        /* the pass's registered order */
} mod_postfx_rank_t;

/** @brief Sort enabled postfx passes into run order and apply the renderer's cap.
 *
 * Ascending by load_index, then order, then table index: the chain runs the
 * lowest-priority pass first and the highest last, outermost. Over @p cap,
 * the lowest-priority passes are dropped: the last @p cap entries move to
 * the front, still ascending.
 * @param v The passes; sorted in place.
 * @param n Entries in @p v.
 * @param cap Passes the renderer runs.
 * @return The kept count, min(n, cap). */
int mod_postfx_rank(mod_postfx_rank_t* v, int n, int cap);

/* Resolve census (OPENPETE_MATERIAL_STATS). Counters are relaxed atomics,
 * bumped only while the census is on. */

/** @brief Return whether the resolve census is on.
 *
 * On when OPENPETE_MATERIAL_STATS is set and not 0, read at the first call;
 * the first call that finds it on registers the exit-time print.
 * @return Nonzero when on. */
int mod_mat_census_on(void);
/** @brief Count one retained-terrain material pass bound to a material.
 *
 * @param m Material index. */
void mod_mat_census_bucket_pass(int m);
/** @brief Return a material's census counter.
 *
 * @param m Material index.
 * @param which MOD_MAT_CENSUS_* counter.
 * @return The count since boot, or 0 out of range. */
unsigned mod_mat_census_get(int m, int which);
/** @brief Print the census to stderr: per material counters, bake and walked terrain histograms, engagement. */
void mod_mat_census_print(void);
/** @brief Force the census on or off, for the unit test.
 *
 * @param on Nonzero for on. */
void mod_mat_census_force(int on);
/** @brief Zero every census counter, for the unit test. */
void mod_mat_census_reset(void);
/** @brief Return the number of extraction outcomes replayed from the tick memo instead of asking a refine.
 *
 * @return The count since boot or the last mod_mat_census_reset. */
unsigned mod_mat_census_memo_replays(void);
/** @brief Return the number of draw-time resolves made with a material view installed.
 *
 * @return The count since boot or the last mod_mat_census_reset. */
unsigned mod_mat_census_view_resolves(void);

/* Composition census counters (mod_mat_census_chain). */
enum {
    MOD_MAT_CHAIN_WALKS = 0,     /* chains the extraction walk formed */
    MOD_MAT_CHAIN_ABSENT,        /* members excluded at extraction as absent from the installed set */
    MOD_MAT_CHAIN_FULL,          /* chain outcomes a full slot or chain table sent to stock */
    MOD_MAT_CHAIN_HELD,          /* chain draws stock: a held member */
    MOD_MAT_CHAIN_LACKED,        /* chain draws stock: the installed set lacks a member */
    MOD_MAT_CHAIN_UNUSABLE,      /* chain draws stock: a member disabled, swept or of another contract */
    MOD_MAT_CHAIN_STALE,         /* chain draws stock: an entry past the view */
    MOD_MAT_CHAIN_CENSUS_N,
    MOD_MAT_CHAIN_DRAWS_2 = MOD_MAT_CHAIN_CENSUS_N   /* chain draws of 2, +1 of 3, +2 of 4 members */
};
/** @brief Return a composition census counter.
 *
 * @param which MOD_MAT_CHAIN_*.
 * @return The count since boot or the last mod_mat_census_reset; 0 out of range. */
unsigned mod_mat_census_chain(int which);

enum {
    MOD_MAT_CENSUS_DRAWS = 0,        /* draw-time resolves won (walked runs) */
    MOD_MAT_CENSUS_BUCKET_PASSES,    /* retained-terrain material passes bound */
    MOD_MAT_CENSUS_EXTRACT_WINS,     /* moby extraction walk landed on it (refine or decl) */
    MOD_MAT_CENSUS_REFINE_CALLS,     /* refine callback invocations */
    MOD_MAT_CENSUS_DECLINES,         /* refine returned -1 */
    MOD_MAT_CENSUS_FALLTHROUGHS,     /* decline carried to a lower material (moby walk, terrain class) */
    MOD_MAT_CENSUS_OUTRANKED,        /* refine called while a higher declarative matched its key */
    MOD_MAT_CENSUS_STALE_MEMO,       /* bucket pass found no outcome of the present it composes */
    MOD_MAT_CENSUS_N
};

/* Overlap diagnostic: per pair of materials whose selectors share a key,
 * how much of the lower material's key set the higher one takes. */

/** @brief Shadow verdict of one ordered material pair. */
enum {
    MOD_MAT_SHADOW_NONE = 0,     /* no key both materials match */
    MOD_MAT_SHADOW_PARTIAL,      /* the higher takes some keys, or may decline on them */
    MOD_MAT_SHADOW_FULL          /* the higher takes every key the lower matches */
};

/** @brief One overlapping pair: the higher-ranked material, the lower one, the verdict. */
typedef struct {
    int      hi, lo;      /* material indices; hi outranks lo (mod_mat_outranks) */
    int      verdict;     /* MOD_MAT_SHADOW_* */
    unsigned chans;       /* bit OP_CHAN_* set per channel the two share a key on */
    int      accept_all;  /* the verdict rests on hi being an accept-all moby refine */
} mod_mat_overlap_t;

/* Pairs the published list holds; pairs past it are counted, not listed. */
#define MOD_MAT_OVERLAP_MAX 1024

/** @brief Record a change the overlap verdicts depend on: a register, enable change, selector change or owner removal. */
void mod_mat_overlap_bump(void);

/** @brief Return whether the inputs of the overlap verdicts moved since the last snapshot.
 *
 * The inputs: the change counter mod_mat_overlap_bump advances, the mod
 * toggle epoch, the pushed level, and the first call and first decline of
 * each moby refine. Takes no lock.
 * @return Nonzero when a snapshot is due. */
int mod_mat_overlap_due(void);

/** @brief Record each material's key set and rank inputs for mod_mat_overlap_compute; the caller holds the registry write lock.
 *
 * The key set of a material is read through the resolver's own predicates
 * at the pushed level: the moby class and the channel identity of each
 * declarative channel, and every (texid, texturedness, env, tier) terrain
 * face key. */
void mod_mat_overlap_snapshot(void);

/** @brief Compute the overlap pairs from the last snapshot, without a lock.
 *
 * Sim thread only, between mod_mat_overlap_snapshot and
 * mod_mat_overlap_publish. */
void mod_mat_overlap_compute(void);

/** @brief Install the computed pairs as the published list and log each pair whose verdict changed; the caller holds the registry write lock.
 *
 * A pair that left the list logs verdict none. */
void mod_mat_overlap_publish(void);

/** @brief Return the shadow verdict of one ordered pair from the published list.
 *
 * @param hi Material index of the higher-ranked material.
 * @param lo Material index of the lower-ranked one.
 * @return MOD_MAT_SHADOW_*; NONE when the pair is not listed. */
int mod_mat_overlap_verdict(int hi, int lo);

/** @brief Return the number of overlapping pairs found, listed or not.
 *
 * @return The count; at most MOD_MAT_OVERLAP_MAX of them are listed. */
int mod_mat_overlap_total(void);

/** @brief Return one listed pair of the published list.
 *
 * @param i Index below min(mod_mat_overlap_total(), MOD_MAT_OVERLAP_MAX).
 * @return The pair, or NULL out of range. */
const mod_mat_overlap_t* mod_mat_overlap_at(int i);

/** @brief Return the name of a shadow verdict.
 *
 * @param verdict MOD_MAT_SHADOW_*.
 * @return "none", "partial" or "full". */
const char* mod_mat_shadow_name(int verdict);

#endif
