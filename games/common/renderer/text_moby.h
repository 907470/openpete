// games/common/renderer/text_moby.h
//
// Render arbitrary text using the game's own extruded 3D glyph models (Spyro
// 1's gem and life counters, "DEMO MODE", dragon names and pause menu; Spyro
// 3's home-world portal names). game_layout.font says where the models are
// and how they are packed; see the .c for both formats.
//
// The glyph models are not permanently resident, so their decoded geometry is
// snapshotted once, the first time they are available
// (text_moby_native_cache), and every draw works from the snapshot. A game
// whose font is a per-level asset also keeps the snapshot in a cache file
// (OPENPETE_FONT_CACHE), loaded at boot by later sessions. Until a snapshot
// exists, ready() is false and the caller renders nothing.
//
// Pure presentation — reads glyph-model geometry from RAM, writes no
// gameplay-readable state.

#ifndef OPENPETE_TEXT_MOBY_NATIVE_H
#define OPENPETE_TEXT_MOBY_NATIVE_H

#include "openpete_native_render.h"  /* openpete_hud_vert */

struct CPUState;

/* Snapshot the glyph-model geometry the first time it is available: from the
 * cache file where the game keeps one, else from RAM once the font asset is
 * resident. A cheap no-op once cached; call every tick (the cache survives the
 * models being unloaded later). Reads model RAM only. */
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
 * 0-9, A-Z (case-insensitive), the punctuation the game's font has, and ':'
 * faked from two period dots; a space (or any glyph not in the cache) just
 * advances. Works
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

/* Per-tick OSD consumer: builds every live toast's frame + text into the
 * renderer's fixed slot grid and publishes it with the slot table
 * (openpete_native_set_osd_glyphs). Rebuilt EVERY tick a toast is live — the
 * gold shimmer is baked into vertex colours at emit time, so cached geometry
 * would show a frozen glint. No-op with the OSD off, the ring empty, or the
 * font not yet cached. */
void text_moby_native_osd(struct CPUState* cpu);

/* The gold shimmer's phase — SpecularUpdate's travelling light, reproduced
 * host-side. Advanced once per LOGIC frame by text_moby_native_shimmer_tick
 * (call once per sim tick, before the builders); speculative and replayed
 * ticks sit it out. Shared by the settings menu and the OSD, so a toast
 * shimmers whether or not the menu happens to be open. */
void     text_moby_native_shimmer_tick(void);
unsigned text_moby_native_shimmer_phase(void);

#endif /* OPENPETE_TEXT_MOBY_NATIVE_H */
