// core/include/openpete_menu.h — in-game settings menu (toggled by `O`).
//
// A native-renderer overlay that live-changes presentation settings: aspect
// ratio, target FPS, vsync, SSAA, draw distance, plus a row for the debug
// free camera. Styled after Spyro 1's pause menu (gold border,
// semi-transparent panel, 3D gold-glyph text, wobbling selected item — the
// draw half lands in a later step). Display-only: every value maps to a
// native-renderer / host-pacer setter, never gameplay state — except FREECAM,
// which fronts the same intentionally-divergent debug toggle as the F key.
// See notes/plans/openpete-ingame-settings-menu.md.
//
// The host loop (core/runtime/run.c) owns input: it toggles on `O`, feeds arrow
// keys to nav()/adjust() while open, suppresses the game pad, and (later) calls
// draw() on the live vk present. Native-renderer only — inert under psycross.

#ifndef OPENPETE_MENU_H
#define OPENPETE_MENU_H

#include "openpete.h"   /* openpete_options_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Seed the menu's value model from launch options and build the dynamic FPS
 * ladder (30, 60, k*60 up to refresh, then refresh). `refresh_hz` is the
 * monitor refresh in Hz (<= 0 → treated as 60). Applies nothing — it only makes
 * the menu reflect the flags the game launched with. Call once before the loop. */
void openpete_menu_init(const openpete_options_t* opts, int refresh_hz);

/* Toggle open/closed; returns the new open state (1 = open). */
int  openpete_menu_toggle(void);

/* 1 while the menu is open. The host loop reads this to suppress pad input and
 * (later) to draw the overlay. */
int  openpete_menu_is_open(void);

/* Current cursor row (0 .. row_count-1). */
int  openpete_menu_cursor(void);

/* Menu layout in the renderer's overlay space: x in units of window HEIGHT, y in
 * [0,1], origin top-left, +y down (same space as the FPS overlay). Shared by the
 * native panel/border/selection draw and (later) the game-side glyph placement,
 * so text lands on the same rows as the panel geometry — one source of truth. */
typedef struct {
    float panel_x0, panel_y0, panel_x1, panel_y1;  /* panel rect */
    int   row_count;
    float row_y[8];        /* per-row center y */
    float row_h;           /* selection-bar height */
    float title_y, title_cap_h;
    float row_cap_h;       /* glyph cap height for a row */
    float label_x, value_x;/* left edge of the label / value glyph columns */
} openpete_menu_layout_t;

void openpete_menu_get_layout(openpete_menu_layout_t* out);

/* Text + wobble phase for the game-side glyph builder (text_moby_native). */
const char* openpete_menu_title(void);
const char* openpete_menu_row_label(int row);
const char* openpete_menu_row_value(int row, char* buf, int buflen);
unsigned    openpete_menu_wobble_phase(void);

/* Move the row cursor: dy < 0 = up, dy > 0 = down (wraps). No-op unless open. */
void openpete_menu_nav(int dy);

/* Change the focused row's value: dx < 0 = previous, dx > 0 = next (wraps), and
 * apply it live via the matching setter. No-op unless open. */
void openpete_menu_adjust(int dx);

/* Advance the selected-item wobble / border-shimmer phase. Call once per
 * presented frame from the draw step. */
void openpete_menu_tick(void);

/* Draw the overlay (panel + gold border + 3D glyph rows). Called from the native
 * renderer's compose step on the live vk present. (Stub until the draw step.) */
void openpete_menu_draw(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_MENU_H */
