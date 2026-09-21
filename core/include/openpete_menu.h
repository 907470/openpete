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
    float row_y[16];       /* per-row center y (capacity >= ROW_COUNT) */
    float row_h;           /* selection-bar height */
    float title_y, title_cap_h;
    float row_cap_h;       /* glyph cap height for a row */
    float label_x, value_x;/* left edge of the label / value glyph columns */
    /* The close glyph: a square at the panel's top right holding an X. The
     * glyph builder places the X inside it; the emitted ink is published
     * back through openpete_menu_note_close_rect and is what the mouse
     * consumer tests, so the hit region is the pixels that were drawn. */
    float close_x, close_y, close_cap_h;   /* top-left corner, side */
} openpete_menu_layout_t;

void openpete_menu_get_layout(openpete_menu_layout_t* out);

/* The bounding box, in overlay units, of the close glyph's emitted ink. Only
 * the text builder knows a glyph's drawn extent: the layout square is the
 * advance box, which is wider than the X and would put the hit region beside
 * the visible pixels. Until the builder has run once the consumer falls back
 * to the layout square. */
void openpete_menu_note_close_rect(float x0, float y0, float x1, float y1);

/* The cap height the builder actually drew the rows at. row_cap_h is a
 * ceiling; the builder shrinks every row uniformly to fit the widest value in
 * its column, and only it knows the font metrics that decide by how much. The
 * layout needs the drawn height to sit the bottom-anchored EXIT row's ink at
 * the panel's bottom inset. Until the builder has run once the layout uses
 * row_cap_h. */
void openpete_menu_note_row_cap(float cap_h);

/* Text + wobble phase for the game-side glyph builder (text_moby_native). */
const char* openpete_menu_title(void);
const char* openpete_menu_row_label(int row);
const char* openpete_menu_row_value(int row, char* buf, int buflen);
unsigned    openpete_menu_wobble_phase(void);

/* 1 for an action row drawn CENTERED in the panel with no value column (EXIT
 * GAME): the glyph builder centres its label instead of splitting it across the
 * label/value columns. Its label carries the row's state — EXIT arms on the
 * first press and reads "REALLY EXIT?" until it is pressed again or the cursor
 * moves off it. */
int         openpete_menu_row_centered(int row);

/* Move the row cursor: dy < 0 = up, dy > 0 = down (wraps). No-op unless open. */
void openpete_menu_nav(int dy);

/* Put the cursor on `row` (the pointer's hit row). Disarms EXIT like nav, so
 * callers invoke it only when the row differs from the cursor. */
void openpete_menu_set_cursor(int row);

/* The mouse consumer. Called once per host iteration while the menu is open,
 * after the keyboard/pad nav: hover moves the cursor on a pointer move, a
 * left press acts on the hit row (adjust +1, or activate on EXIT) or closes
 * the menu through the X glyph, a right press adjusts -1 on value rows, and
 * each wheel notch over a value row adjusts by one. DISPLAY, SETTINGS and
 * EXIT take left click only. Reads the host mouse channel as its MENU owner
 * and snaps its markers while the dev overlay owns the mouse. */
void openpete_menu_mouse_poll(void);

/* Change the focused row's value: dx < 0 = previous, dx > 0 = next (wraps), and
 * apply it live via the matching setter. No-op unless open, and on an ACTION
 * row (openpete_menu_row_centered) — cycling a value must never fire a button. */
void openpete_menu_adjust(int dx);

/* Press the focused ACTION row (EXIT GAME): arms on the first call, fires on
 * the second. No-op unless open and the row is an action row. */
void openpete_menu_activate(void);

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
