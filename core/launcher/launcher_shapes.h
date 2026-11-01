/* launcher_shapes.h - the launcher's boxes: Spyro 3's dialogue-box panel and
 * chip, the chip line's text scrim and the progress bar, added to the
 * present's shape pass (openpete_native_launcher_shape_add) and recorded in
 * the element manifest. Every call takes window coordinates (imgui's) and
 * the layout scale (window pixels per layout pixel); the pass receives
 * framebuffer pixels. Launcher thread, inside a present's imgui callback. */
#ifndef OPENPETE_LAUNCHER_SHAPES_H
#define OPENPETE_LAUNCHER_SHAPES_H

#include "imgui.h"

/* Spyro 3's dialogue box: fill (8,8,8), elliptical corners of these radii
 * in layout px (the quarter ellipse fitted to its corner sprite on the 4:3
 * screen), drawn at half opacity as the game's blend mode 0 does. */
constexpr float kPanelRx = 22.8f;
constexpr float kPanelRy = 18.4f;
constexpr float kChipAlpha = 0.5f;

/** @brief Add a panel or chip: Spyro 3's dialogue box, (8,8,8) at an opacity, with its drop shadow, and record it in the element manifest.
 *
 * Radii too large for the box scale down together, keeping the corner's
 * aspect. The shadow is 35% black, 2 layout px down, 6 layout px blur,
 * scaled by @p alpha over kChipAlpha (at most 1), so a fading content panel
 * fades its shadow with it.
 * @param id The element's manifest id.
 * @param a The box's top left, window coordinates.
 * @param b Its bottom right.
 * @param scale Window pixels per layout pixel.
 * @param alpha The fill's opacity, 0..1. */
void launcher_shape_panel(const char* id, ImVec2 a, ImVec2 b, float scale, float alpha);

/** @brief Add a progress bar: the panel's corners scaled to fit the bar's height, a track, the fill clipped to the shape up to the fraction, a border ring inside the edge, and the drop shadow; recorded in the element manifest.
 *
 * @param id The element's manifest id.
 * @param a The bar's top left, window coordinates.
 * @param b Its bottom right.
 * @param scale Window pixels per layout pixel.
 * @param frac The filled fraction, clamped to 0..1.
 * @param track The track colour.
 * @param fill The fill colour.
 * @param border The border colour.
 * @param border_px The border width in layout px. */
void launcher_shape_bar(const char* id, ImVec2 a, ImVec2 b, float scale, float frac, ImU32 track, ImU32 fill,
                        ImU32 border, float border_px);

/** @brief Compute the ink bounds of one line of text as imgui draws it: the union of its glyphs' X0..X1, Y0..Y1 at the font's size, placed from the truncated origin by the advances.
 *
 * @param font The font, or NULL for the current one.
 * @param size The font size in window pixels.
 * @param pos The text's origin, as passed to AddText or the cursor.
 * @param text The text, UTF-8.
 * @param mn Receives the ink's top left.
 * @param mx Receives its bottom right.
 * @return false when no glyph of the text is visible. */
bool launcher_text_ink(ImFont* font, float size, ImVec2 pos, const char* text, ImVec2* mn, ImVec2* mx);

/** @brief Add a chip line's text scrim: (8,8,8) at 12% as a rounded rect over the line's ink grown by 3 layout px, radius 3 layout px, no shadow; recorded in the element manifest as <id>.scrim.
 *
 * Added after the chip's fill, so the pass draws it over the fill and
 * under the text. Draws nothing when the line has no visible glyph.
 * @param id The text's manifest id.
 * @param font The font the line draws with, or NULL for the current one.
 * @param size The font size in window pixels.
 * @param pos The text's origin.
 * @param text The text.
 * @param scale Window pixels per layout pixel. */
void launcher_shape_scrim(const char* id, ImFont* font, float size, ImVec2 pos, const char* text, float scale);

/** @brief Draw one line of text on a chip: the chip box (launcher_shape_panel at kChipAlpha) padded around the line's box, the text scrim, then the text in imgui; each recorded in the element manifest.
 *
 * The text's box is its origin and CalcTextSize at the current font; the
 * chip spans it grown by @p pad (layout px) and is recorded as <id>.panel,
 * the text as <id>, the scrim as <id>.scrim.
 * @param id The text's manifest id.
 * @param pos The text's origin, window coordinates.
 * @param pad The chip's padding around the text box, layout px.
 * @param scale Window pixels per layout pixel.
 * @param col The text colour.
 * @param text The text. */
void launcher_chip_text(const char* id, ImVec2 pos, ImVec2 pad, float scale, ImU32 col, const char* text);

#endif /* OPENPETE_LAUNCHER_SHAPES_H */
