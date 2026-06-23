// games/spyro-1/features/text_moby_native.h
//
// Render arbitrary text using Spyro 1's own 3D HUD "text mobys" — the gold
// extruded glyph models the game draws for the gem/life counters, "DEMO MODE",
// dragon names, "THE ADVENTURE BEGINS", the pause & inventory menus, etc. See
// the .c for the model format.
//
// The glyph models are NOT permanently resident — they load with the HUD font
// asset (~frame 300, title/attract) but get EVICTED during e.g. the level-intro
// cutscene. So we SNAPSHOT their decoded geometry once, the first time they are
// available (text_moby_native_cache), and render purely from that cache forever
// after — immune to later unloading. Until the snapshot is taken, ready() is
// false and the caller renders nothing (there is no bitmap fallback).
//
// Pure presentation — reads glyph-model geometry from RAM, writes no
// gameplay-readable state.

#ifndef OPENPETE_TEXT_MOBY_NATIVE_H
#define OPENPETE_TEXT_MOBY_NATIVE_H

#include "openpete_native_render.h"  /* openpete_hud_vert */

struct CPUState;

/* Snapshot the glyph-model geometry from RAM the first time the HUD font asset
 * is resident; a cheap no-op once cached. Call every frame (the cache survives
 * the models being unloaded later). Reads model RAM only. */
void text_moby_native_cache(struct CPUState* cpu);

/* True once the glyph cache is populated (the font has been seen at least once). */
int text_moby_native_ready(void);

/* Lay out `str` as 3D text-moby glyph triangles (3 verts per tri) into `out`,
 * in window-height-fraction overlay space (origin top-left, +y down; x shares
 * the y scale so glyphs stay square at any aspect). The text's top-left sits at
 * (origin_x, origin_y) and glyph height is `cap_h` (all fractions of window
 * height). `base_rgb` is the 0xRRGGBB BASE/tint colour (the unlit back-colour);
 * a white directional light is ADDED per face on top, matching how the game
 * shades these mobys (dark base -> brighter toward the light). Supports
 * 0-9, A-Z (case-insensitive), common punctuation, and ':' faked from two
 * period dots; a space (or any glyph not in the cache) just advances. Works
 * entirely from the cache — returns 0 (nothing emitted) until ready(). */
int text_moby_native_emit(const char* str,
                          float origin_x, float origin_y, float cap_h,
                          unsigned base_rgb,
                          openpete_hud_vert* out, int out_cap);

/* Per-frame FPS-overlay consumer: refreshes the cache, then (once ready) builds
 * "FPS:<value>" and hands it to the native renderer's overlay slot
 * (openpete_native_set_overlay_glyphs). Called from game_on_frame_pre; a no-op
 * unless the native renderer is active. */
void text_moby_native_fps_overlay(struct CPUState* cpu);

#endif /* OPENPETE_TEXT_MOBY_NATIVE_H */
