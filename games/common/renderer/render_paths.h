/* render_paths.h - per-path render isolation registry (--render-paths debug).
 *
 * Lets a debugging session restrict rendering to a chosen set of render paths
 * (cyclorama, Spyro, mobys, ...) in BOTH renderers, so a single path can be
 * compared native-vs-PsyCross in isolation (no occlusion noise from paths that
 * native doesn't draw yet). The native side gates its feed on the selection;
 * the PsyCross side drops the other paths' geometry prims during the OT walk
 * (psyq_DrawOTag). It is presentation-only - it never writes game RAM (the OT
 * filter just doesn't *submit* a prim), so gameplay state is unaffected.
 *
 * Mechanism: each render path brackets the polybuf range it emits each frame
 * (POLYBUF_CURSOR before/after its dispatch) and records it here; the OT walk
 * then keeps a geometry prim only if its PSX address falls inside a SELECTED
 * path's recorded interval. Control/env prims (draw-env, loads, anchors) always
 * pass so the framebuffer is still set up. */

#ifndef OPENPETE_SPYRO_RENDER_PATHS_H
#define OPENPETE_SPYRO_RENDER_PATHS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RP_CYCLORAMA = 0,
    RP_SPYRO,
    RP_MOBY,      /* regular + specular + shaded mobys; also HUD mobys (CopyHudMobysToShaded) */
    RP_TERRAIN,   /* world/environment geometry (func_8002B9CC -> func_800258F0) */
    RP_SHADOW,    /* the player's drop-shadow: Spyro 1 func_80059A48, the Spyro 2/3 player
                   * shadow routine */
    RP_FLAME,     /* Spyro's flame breath (func_80058D64) */
    RP_PARTICLES, /* particle systems (func_800573C8) */
    RP_TRACER,    /* additive tracer/beam (func_800189F0) - charge streak, fireball trail */
    RP_TRANSITION,/* level-exit / balloonist cross-fade backdrop (func_8004F000) */
    RP_PORTAL,    /* portal skybox previews (func_80050BE0 RenderPortals) */
    RP_GLOW,      /* additive glow rings (func_800580F4); a separate .s renderer the game
                   * skips in some states (dragon-rescue cutscenes) while the pool stays active */
    RP_SPARKLE,   /* sparkle twinkles (func_800584C4), same independence as RP_GLOW */
    RP_MOBY_SHADOW,/* moby drop-shadows (Spyro 1 func_80059F8C). Separate from RP_SHADOW:
                   * the rescue-cutscene states 5 and 6 draw the player's shadow and not
                   * the moby shadows while the moby draw still fills their list */
    RP_UI,        /* the 2D screen-space layer: every primitive linked through the
                   * game's 2D ordering-table insert (ui_game.ot_insert - logo, HUD
                   * sprites, dialog boxes, the fade) and the glyph chains
                   * (ui_game.sprt_chain_emitter) */
    RP_COUNT
} render_path_t;

/* Note: the 3D HUD (Spyro 1's gold counters and the other extruded glyph
 * models) is copied into the shaded moby list (CopyHudMobysToShaded) and
 * rendered through the moby pipeline, so it is part of RP_MOBY; RP_UI is the
 * 2D layer only. */

/** @brief Parse the --render-paths CSV.
 *
 * NULL/empty = no selection (all paths render, filtering off). The name
 * "none" selects no path without a warning; other unknown names are warned
 * and ignored. A non-empty spec with no known name selects no path, and
 * nothing renders. The selected set is logged to stderr.
 * @param spec comma- or space-separated path names */
void render_paths_init(const char* spec);

/** @brief Replaces the selection from `spec` and re-syncs the native renderer's isolation background.
 *
 * Runs render_paths_init on `spec`, then openpete_native_set_isolation with
 * render_paths_active(). The dev overlay's render_paths row calls it on edit.
 * The selection gates prim submission and the native feed only and writes no
 * guest RAM, so it may change at any point of a session.
 * @param spec Comma- or space-separated path names; NULL or empty clears the
 *        selection. */
void render_paths_set(const char* spec);

/** @brief Writes the current selection into `buf` as comma-separated path names.
 *
 * Writes "(all)" when no selection is in effect and "none" when a selection
 * matched no path; otherwise the selected names in render_path_t order.
 * Output longer than `cap` is truncated by snprintf.
 * @param buf Receives the NUL-terminated text.
 * @param cap The capacity of `buf`. */
void render_paths_current(char* buf, size_t cap);

/** @brief Reports whether a --render-paths selection is in effect.
 *
 * @return true after a non-empty spec, even one that matched no path; false
 *         when every path renders, the default. */
bool render_paths_active(void);

/** @brief Reports whether path `p` is selected.
 *
 * @param p The path.
 * @return true when no selection is in effect or `p` is selected; false for
 *         an unselected or out-of-range path under a selection. */
bool render_path_enabled(render_path_t p);

/** @brief Rolls the interval table into the previous bank and clears the table and the interval-less marks.
 *
 * A non-empty table is copied into the previous bank, which the OT-walk filter
 * consults for the OT built on the previous render VBL; an empty table leaves
 * the previous bank as it was. The per-canonical-present commit calls it after
 * render_paths_canon_snapshot. */
void render_paths_frame_reset(void);
/** @brief Appends one path's emitted polybuf range [start, end) to the interval table.
 *
 * An empty range and an append past the table's 1024 entries are dropped.
 * OPENPETE_RENDER_PATH_IVDUMP=N prints every range recorded while
 * openpete_frame equals N.
 * @param p The path that emitted the range.
 * @param start The polybuf cursor before the path's dispatch.
 * @param end The polybuf cursor after it. */
void render_path_record(render_path_t p, uint32_t start, uint32_t end);

/** @brief Marks path `p` drawn on the current canonical render without a polybuf interval.
 *
 * For a producer whose native feed is not one-to-one with its polybuf output:
 * RenderPortals under widescreen feeds portals the .s culled, with an empty
 * polybuf interval. The mark enters only the canon-drawn mask at the next
 * render_paths_canon_snapshot, never the OT-walk address filter, and
 * render_paths_frame_reset clears it. An out-of-range `p` is ignored.
 * @param p The path. */
void render_path_mark_drawn(render_path_t p);

/** @brief Latches which paths drew on the canonical render into the canon-drawn mask.
 *
 * The mask is the interval-less marks plus every path holding at least one
 * interval in the current table; render_path_record drops empty ranges, so a
 * path whose render function ran and emitted nothing reads as not drawn. The
 * per-canonical-present commit calls it before render_paths_frame_reset. The
 * sub-tick re-extract gates each feeder on the mask, so an intermediate
 * present draws the path set the canonical render drew: during a level
 * transition the game stops rendering terrain and the replay then feeds none.
 * The mask does not depend on the --render-paths selection. */
void render_paths_canon_snapshot(void);
/** @brief Reports whether path `p` drew on the most recent canonical render.
 *
 * @param p The path.
 * @return true when the canon-drawn mask holds `p`; false for an out-of-range path. */
bool render_path_canon_drawn(render_path_t p);

/** @brief Reports whether the OT walk submits the geometry prim at polybuf address `addr`.
 *
 * Searches the current interval table, then the previous bank, since the game
 * submits DrawOTag at the start of a VBL before that VBL's render functions
 * record, and returns whether the path owning the first interval containing
 * `addr` is selected. It does not test render_paths_active(); callers gate on
 * it first.
 * @param addr The prim's PSX address.
 * @return true when `addr` lies in a selected path's interval; false when it
 *         lies in an unselected path's interval or in none. */
bool render_path_addr_visible(uint32_t addr);

struct CPUState;
/** @brief Push the level the guest is in to the mods layer, for level-keyed material selectors.
 *
 * Reads game_layout.level_id and passes it to openpete_mods_set_level,
 * which is idempotent for an unchanged level. Every scene feeder calls it
 * before it extracts or opens the scene: the guest writes the level inside
 * its tick, before that tick's feeders, so the first feeder of a tick
 * pushes the level its moby resolves, terrain bake and material view read.
 * @param cpu Guest CPU state. */
void render_material_level_push(struct CPUState* cpu);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_SPYRO_RENDER_PATHS_H */
