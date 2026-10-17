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

/* A real bound, not a table size: the bucket id stamped into the baked
 * face material is the index + 1, and it has 8 bits
 * (OPENPETE_MATERIAL_TER_BUCKET_MAX). A material registered above index
 * 254 would succeed and then never shade a terrain face. */
#define MOD_MATERIAL_MAX OPENPETE_MATERIAL_TER_BUCKET_MAX

/* Per-present interned per-instance slots. Index 0 reserved
 * (= "defer to draw-time class resolve"); 1.. are interned outcomes. */
#define MOD_MATERIAL_SLOTS 128

/* The first manifest api pin whose same-mod material ties go to the later
 * registration; a lower pin keeps the first registration. */
#define MOD_MAT_API_LATER_TIE 13

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
     * memoized so the callback runs ONCE however many faces need it and
     * whichever residency they are in. A terrain refine selector has exactly
     * one key (its Tiledef id, or the untextured band), so one slot covers
     * the whole material. */
    int           refine_slot;
    unsigned      refine_present;
    /* Set once the retained bake has stamped this refine's bucket into a
     * face; kept for the session, since a retained face keeps its bucket
     * until a rebake that need not restamp it. The sim-thread refine pass
     * asks only refines a retained face can draw through. */
    int           bake_won;
    /* A selector retuned from inside a present (material_set_selector) parks
     * here until the tick boundary. Selection has to be constant for a whole
     * present or half the frame shades under the old key. */
    openpete_mod_material_selector_t sel_pending;
    int           sel_dirty;
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
 * bits outside OP_SEL_FLAGS_KNOWN, a channel other than -1 outside
 * OP_CHAN_MOBY..OP_CHAN_PORTAL, and a texture key other than a Tiledef id
 * 0..127 on channel -1 or OP_CHAN_TERRAIN without OP_SEL_UNTEXTURED. The
 * flag check exists because a new flag bit never changes sizeof. Register
 * and set_selector share it, so a retune cannot install a key the register
 * path would refuse.
 * @param id Mod id for log lines.
 * @param sel The selector.
 * @return 1 when valid, 0 when refused. */
int mod_mat_sel_valid(const char* id, const openpete_mod_material_selector_t* sel);

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
 * mod_mat_sel_valid. Inside a present the new selector is parked for
 * mod_mat_apply_pending; outside one it applies at once.
 * @param id The calling mod's id.
 * @param material Material handle.
 * @param sel The new selector.
 * @return 0, or -1 for a NULL argument, an unknown or swept material, a
 *         material another mod owns, or an invalid selector. */
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
 * the counter openpete_mods_material_frame_reset advances); a block
 * identical to an interned slot of the same material reuses that slot.
 * Sim thread only: extraction and openpete_mods_material_terrain_refine_present.
 * @param w Material index, or -1 for none.
 * @return 0 for no material or a declarative one, an interned slot, or
 *         OPENPETE_MATERIAL_TERRAIN_STOCK when the callback declined or
 *         the slot table is full. */
int mod_mat_terrain_refine_slot(int w);

/** @brief Return the refine outcome a retained-terrain bucket pass binds, without calling a callback.
 *
 * Reads the installed view's outcome for the material, else, with no view
 * installed, the material's memo when it was computed this present. A memo
 * from another present, or a view with no outcome for the material, reads as
 * stock; it counts in the census (MOD_MAT_CENSUS_STALE_MEMO) when a baked
 * face carries the material's bucket.
 * @param w Material index of a per-instance material.
 * @return An interned slot, or OPENPETE_MATERIAL_TERRAIN_STOCK. */
int mod_mat_terrain_bucket_slot(int w);

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
/** @brief Count one retained-terrain pass bound to a material's bucket.
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
/** @brief Return the number of draw-time resolves made with a material view installed.
 *
 * @return The count since boot or the last mod_mat_census_reset. */
unsigned mod_mat_census_view_resolves(void);

enum {
    MOD_MAT_CENSUS_DRAWS = 0,        /* draw-time resolves won (walked runs) */
    MOD_MAT_CENSUS_BUCKET_PASSES,    /* retained-terrain passes bound */
    MOD_MAT_CENSUS_EXTRACT_WINS,     /* moby extraction winner (refine or decl) */
    MOD_MAT_CENSUS_REFINE_CALLS,     /* refine callback invocations */
    MOD_MAT_CENSUS_DECLINES,         /* refine returned -1 */
    MOD_MAT_CENSUS_FALLTHROUGHS,     /* decline handed to a lower material */
    MOD_MAT_CENSUS_DECLINE_SHADOWS,  /* decline drawn stock while a lower material matched */
    MOD_MAT_CENSUS_OUTRANKED,        /* refine called while a higher declarative matched its key */
    MOD_MAT_CENSUS_STALE_MEMO,       /* bucket pass found no outcome of the present it composes */
    MOD_MAT_CENSUS_N
};

#endif
