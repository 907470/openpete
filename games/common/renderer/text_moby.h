// games/common/renderer/text_moby.h
//
// Render arbitrary text using the game's own extruded 3D glyph models (Spyro
// 1's gem and life counters, "DEMO MODE", dragon names and pause menu; the
// end-credits glyphs of Spyro 2 and Spyro 3). game_layout.font says where the
// models are and how they are packed; see the .c for both formats.
//
// The glyph geometry is decoded once into a snapshot (text_moby_native_cache)
// and every draw works from the snapshot: Spyro 1's from guest RAM the first
// time its font is resident, Spyro 2's and 3's from the disc image at the
// first call. Until a snapshot exists, ready() is false and the caller
// renders nothing.
//
// Pure presentation - reads glyph-model geometry from RAM or the disc image,
// writes no gameplay-readable state.

#ifndef OPENPETE_TEXT_MOBY_NATIVE_H
#define OPENPETE_TEXT_MOBY_NATIVE_H

#include "openpete_native_render.h"  /* openpete_hud_vert */

struct CPUState;

/** @brief Snapshots the glyph-model geometry into the glyph cache the first time it is available.
 *
 * A GAME_FONT_WAD_MODEL font is read from the disc image at the first call
 * (one attempt per session) and kept only when every digit and letter
 * decodes. A GAME_FONT_CLASS_PER_GLYPH font is decoded from RAM once digit
 * '0' is a loaded simple model. A no-op once the cache is ready; called
 * every tick by the consumers below, and the cache survives the models
 * being unloaded later.
 * @param cpu The guest CPU state. */
void text_moby_native_cache(struct CPUState* cpu);

/** @brief Reports whether the glyph cache holds a snapshot.
 *
 * @return 1 once text_moby_native_cache has taken the snapshot, else 0. */
int text_moby_native_ready(void);

/** @brief Lays out a string as extruded glyph triangles, lit by the font's fixed light.
 *
 * Output is 3 vertices per triangle in overlay space: fractions of window
 * height, origin top-left, +y down, x on the y scale so glyphs stay square
 * at any aspect. Each face is base_rgb plus the font's light term times the
 * unsigned N.L against game_layout.font's fixed light (a dark base,
 * brighter toward the light). Draws 0-9, A-Z (case-insensitive), the font's
 * punctuation, a synthesized '.' when the descriptor sets synth_period, a
 * synthesized '-' when the font has none, and ':' as two scaled periods; a space or an uncached character only advances.
 * Digits share one tabular advance. Works from the cache alone.
 * @param str The string.
 * @param origin_x The text's left edge.
 * @param origin_y The text's top edge.
 * @param cap_h The glyph height.
 * @param base_rgb The unlit base colour, 0xRRGGBB.
 * @param out The vertex buffer, or NULL to emit nothing.
 * @param out_cap The buffer's capacity, in vertices; a triangle that does
 *        not fit is dropped.
 * @return The count of vertices written; 0 until the cache is ready. */
int text_moby_native_emit(const char* str,
                          float origin_x, float origin_y, float cap_h,
                          unsigned base_rgb,
                          openpete_hud_vert* out, int out_cap);

/** @brief Rasterizes a string for the mod draw service, with the signature of openpete_mods_text_emit_fn.
 *
 * Forwards to text_moby_native_emit; the untyped buffer keeps the core-side
 * function type free of openpete_hud_vert. Registered with
 * openpete_mods_draw_set_text_fn in games/spyro-1/main.cpp. Reads the
 * snapshot only, so mod text is absent until the game has loaded the font
 * once; there is no bitmap fallback.
 * @param str The string.
 * @param x The text's left edge, in overlay units.
 * @param y The text's top edge, in overlay units.
 * @param cap_h The glyph height, in overlay units.
 * @param rgb The unlit base colour, 0xRRGGBB.
 * @param out_verts The vertex buffer, an openpete_hud_vert array.
 * @param cap The buffer's capacity, in vertices.
 * @return The count of vertices written; 0 until the cache is ready. */
int text_moby_native_mod_emit(const char* str, float x, float y,
                              float cap_h, unsigned rgb,
                              void* out_verts, int cap);

/** @brief Builds the performance-stats readout from the glyph models and publishes it to the native renderer.
 *
 * Refreshes the cache, then emits the current level's lines
 * (openpete_perf_stats_lines), each aligned inside the region per the
 * anchor and snapped to a whole output pixel, sets the region extent and
 * publishes with openpete_native_set_overlay_glyphs. The region width is a
 * per-level constant so the supersample target does not resize as the
 * digits change; the geometry is rebuilt only when the lines or the anchor
 * change. With the readout off or the cache not ready it publishes nothing.
 * Called from game_on_frame_pre; a no-op unless the native scene is
 * enabled.
 * @param cpu The guest CPU state. */
void text_moby_native_perf_overlay(struct CPUState* cpu);

/** @brief Builds the settings menu's title, rows, close glyph and lit frame from the glyph models and publishes them.
 *
 * Refreshes the cache and, while the menu is open and the cache is ready,
 * advances the menu's wobble tick on every second call (the game's tick
 * rate), emits the centred title, the rows (labels right-aligned to the
 * layout's label column, values left-aligned at the value column, action
 * rows centred; every row shrunk to the cap at which "FULLSCREEN" fits the
 * value column), the close glyph and the frame, all lit by the rotating
 * specular, and publishes with openpete_native_set_menu_glyphs. The row
 * under the cursor and the hovered close glyph take the per-glyph yaw
 * wobble. Room for the frame is reserved, so overflowing text loses its
 * own tail. Publishes nothing while the menu is closed; a no-op unless the
 * native scene is enabled.
 * @param cpu The guest CPU state. */
void text_moby_native_menu(struct CPUState* cpu);

/** @brief Builds every live OSD toast's text and frame into the renderer's slot grid and publishes it with the slot table.
 *
 * Toast i is emitted in its own slot, starting at y = i * slot height, its
 * text trimmed to fit the slot width; a toast that fits no vertex in the
 * buffer gets no slot row. Text and slot table are published in one
 * openpete_native_set_osd_glyphs call. Rebuilt on every call while a toast
 * is live, since the shimmer is baked into vertex colours at emit time.
 * Publishes nothing with the OSD off, the ring empty or the cache not
 * ready; a no-op unless the native scene is enabled.
 * @param cpu The guest CPU state. */
void text_moby_native_osd(struct CPUState* cpu);

/** @brief Builds the build-version line (openpete_build_version) and publishes it with its region extent.
 *
 * The string is fixed at configure time, so the geometry is emitted once
 * while the line stays enabled; disabling it clears the renderer's copy
 * and a later enable emits it again. An emission of no vertices is retried
 * on the next call. A no-op unless the native scene is enabled.
 * @param cpu The guest CPU state. */
void text_moby_native_version(struct CPUState* cpu);

/** @brief Advances the gold shimmer's phase, SpecularUpdate's travelling light reproduced host-side.
 *
 * Called from game_on_frame_pre before the text builders; advances the
 * phase on every second call, the game's once-per-tick rate for its own
 * counter (++D_800758B8 in draw.c). Speculative and replayed
 * ticks do not advance it. The settings menu and the OSD share the phase,
 * so a toast shimmers whether or not the menu is open. */
void     text_moby_native_shimmer_tick(void);
/** @brief Returns the gold shimmer's phase.
 *
 * @return The phase, advanced by text_moby_native_shimmer_tick. */
unsigned text_moby_native_shimmer_phase(void);

#endif /* OPENPETE_TEXT_MOBY_NATIVE_H */
