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

/* Mod draw-service rasterizer (matches openpete_mods_text_emit_fn; void*
 * out = openpete_hud_vert[]). Registered with the core in main.cpp. */
int text_moby_native_mod_emit(const char* str, float x, float y,
                              float cap_h, unsigned rgb,
                              void* out_verts, int cap);

/* Per-frame performance-stats consumer: refreshes the cache, then (once ready)
 * builds the overlay's lines for the current level (openpete_perf_stats.h) and
 * hands them to the native renderer's overlay slot
 * (openpete_native_set_overlay_glyphs). Called from game_on_frame_pre; a no-op
 * unless the native renderer is active. */
void text_moby_native_perf_overlay(struct CPUState* cpu);

/* Per-frame settings-menu text consumer: when the in-game menu (O) is open and
 * the font is cached, builds the title + each row's label/value (selected row
 * wobbling) and hands it to openpete_native_set_menu_glyphs. No-op otherwise. */
void text_moby_native_menu(struct CPUState* cpu);

#endif /* OPENPETE_TEXT_MOBY_NATIVE_H */
