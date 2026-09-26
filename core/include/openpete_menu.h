// core/include/openpete_menu.h — in-game settings menu (toggled by `O`).
//
// A native-renderer overlay that live-changes presentation settings: aspect
// ratio, target FPS, vsync, SSAA, draw distance, plus a row for the debug
// free camera. Styled after Spyro 1's pause menu (gold border,
// semi-transparent panel, 3D gold-glyph text, wobbling selected item — the
// draw half lands in a later step). Display-only: every value maps to a
// native-renderer / host-pacer setter, never gameplay state — except FREECAM,
// which fronts the same intentionally-divergent debug toggle as the F key.
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

/** @brief Seed the menu's rows from the launch state and build the FPS ladder.
 *
 * The ladder is 30, 60, every multiple of 60 below the refresh, the refresh
 * itself when above 60, then UNLIMITED, capped at 16 entries. The FPS row
 * seeds from the live pacer target (30 when it reads 0 or less); WOBBLE,
 * PIXELATION, TEXTURES and FOG from the values the renderer and no_lod
 * already resolved; ASPECT, FOV, VSYNC, SHOW FPS, SSAA, DITHER and LOD from
 * @p opts, a numeric value to its nearest ladder entry. Resets the cursor to
 * the first row. Applies nothing. Called once before the host loop.
 * @param opts The launch options; NULL builds the ladder and seeds nothing.
 * @param refresh_hz Monitor refresh in Hz; below 60 is treated as 60. */
void openpete_menu_init(const openpete_options_t* opts, int refresh_hz);

/** @brief Open the menu when closed, close it when open.
 *
 * Opening re-syncs the rows a hotkey or the dev overlay can change while the
 * menu is closed (SHOW FPS, LOD, FOG, FREECAM, WOBBLE, PIXELATION, TEXTURES,
 * DITHER) from their live state, disarms EXIT, and snaps the mouse markers
 * so a press made before the open is never a click. Both edges are logged.
 * @return The new state: 1 open, 0 closed. */
int  openpete_menu_toggle(void);

/** @brief Report whether the menu is open.
 *
 * The host loop reads it to route input to the menu and suppress the game
 * pad; the renderer and the glyph builder read it to draw the panel; the
 * mouse channel reads it to name the menu as the pointer's owner.
 * @return 1 while open, else 0. */
int  openpete_menu_is_open(void);

/** @brief Read the cursor row.
 *
 * @return The row index, 0 .. row_count-1. */
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

/** @brief Publish the bounding box, in overlay units, of the close glyph's emitted ink.
 *
 * Only the text builder knows a glyph's drawn extent: the layout square is
 * the advance box, which is wider than the X and would put the hit region
 * beside the visible pixels. Until the builder has run once, the mouse
 * consumer tests the layout square. A changed rect is written to the mouse
 * probe log.
 * @param x0 Left edge.
 * @param y0 Top edge.
 * @param x1 Right edge.
 * @param y1 Bottom edge. */
void openpete_menu_note_close_rect(float x0, float y0, float x1, float y1);

/** @brief Publish the cap height the text builder drew the rows at.
 *
 * row_cap_h is a ceiling; the builder shrinks every row uniformly to fit the
 * widest value in its column, and only it knows the font metrics that decide
 * by how much. openpete_menu_get_layout uses the drawn height to sit the
 * bottom-anchored EXIT row's ink at the panel's bottom inset, and uses
 * row_cap_h until a positive height is published.
 * @param cap_h The drawn cap height; a non-positive value is stored as 0. */
void openpete_menu_note_row_cap(float cap_h);

/** @brief Report whether the pointer sits inside the close glyph's ink.
 *
 * The hover is taken on each openpete_menu_mouse_poll. The glyph builder
 * wobbles the X on it as it wobbles the row under the cursor.
 * @return Non-zero while the menu is open and the pointer was inside the
 *         close glyph at the last poll. */
int openpete_menu_close_hovered(void);

/* Text + wobble phase for the game-side glyph builder (text_moby_native). */
/** @brief Name the menu's title text.
 *
 * @return The constant string "SETTINGS". */
const char* openpete_menu_title(void);
const char* openpete_menu_row_label(int row);
const char* openpete_menu_row_value(int row, char* buf, int buflen);
unsigned    openpete_menu_wobble_phase(void);

/** @brief Report whether a row is the centred action row with no value column.
 *
 * Only EXIT GAME is centred. The glyph builder centres its label instead of
 * splitting it across the label and value columns; the label carries the
 * row's state, reading REALLY EXIT? once armed. openpete_menu_activate acts
 * only on this row.
 * @param row The row index.
 * @return 1 for the EXIT row, else 0. */
int         openpete_menu_row_centered(int row);

/** @brief Move the row cursor one row, wrapping at either end.
 *
 * Disarms EXIT and logs the new cursor. No-op unless open.
 * @param dy Positive moves down; zero or negative moves up. */
void openpete_menu_nav(int dy);

/** @brief Put the cursor on a row, the pointer's hit row.
 *
 * Disarms EXIT and logs the new cursor, as openpete_menu_nav does. No-op
 * unless open, for a row outside [0, row_count), and for the row the cursor
 * already holds, so an unchanged hit keeps EXIT armed.
 * @param row The row index. */
void openpete_menu_set_cursor(int row);

/** @brief Apply the mouse's presses, wheel notches, and movement since the last poll to the menu.
 *
 * Called once per host iteration while the menu is open, after the keyboard
 * and pad navigation. Consumes the host mouse channel's press and wheel
 * totals against its own markers, which the open edge snaps so a press made
 * before the menu opened is never a click. While the dev overlay owns the
 * mouse it snaps the markers, clears the close hover, and does nothing else.
 * A pointer move onto a different row moves the cursor there; a resting
 * pointer never overrides keyboard navigation. Each left press closes the
 * menu on the close glyph, activates EXIT, or adjusts the hit row by +1; each
 * right press adjusts by -1 and each wheel notch by one step in its
 * direction, on value rows only (DISPLAY, SETTINGS and EXIT ignore both).
 * Presses outside every row are ignored. No-op unless open. */
void openpete_menu_mouse_poll(void);

/** @brief Step the cursor row's value one entry, wrapping, and apply it live.
 *
 * Applies through the row's setter, logs it, and raises the row's OSD toast.
 * DISPLAY requests a fullscreen toggle in either direction. On SETTINGS the
 * step picks the action (SAVE, CLEAR SAVED, or none) and the apply performs
 * it at once. FREECAM toggles the free camera when the row disagrees with
 * the live state. No-op unless open, and on the EXIT row, so cycling a value
 * never fires it.
 * @param dx Positive steps to the next entry; zero or negative to the
 *        previous. */
void openpete_menu_adjust(int dx);

/** @brief Press the EXIT GAME row when the cursor is on it.
 *
 * The first press arms the row (its label reads REALLY EXIT?); the second
 * requests a cooperative shutdown through openpete_request_shutdown. Moving
 * the cursor or reopening the menu disarms it. No-op unless open and the
 * cursor row is the centred action row. */
void openpete_menu_activate(void);

/** @brief Advance the phase openpete_menu_wobble_phase returns by one.
 *
 * The glyph builder calls it on every second menu build, matching the game's
 * pause menu, which advances its rotation counter (D_800758B8) once per drawn
 * image. */
void openpete_menu_tick(void);

/** @brief Do nothing; an empty entry point with no caller.
 *
 * The native renderer draws the panel (draw_menu_overlay) and the glyph
 * builder (text_moby_native_menu) the rows. */
void openpete_menu_draw(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_MENU_H */
