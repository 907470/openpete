/* launcher_shapes.h - the launcher's boxes: Spyro 3's dialogue-box panel and
 * chip and the progress bar, added to the
 * present's shape pass (openpete_native_launcher_shape_add) and recorded in
 * the element manifest. Every call takes window coordinates (imgui's) and
 * the layout scale (window pixels per layout pixel); the pass receives
 * framebuffer pixels. Launcher thread, inside a present's imgui callback. */
#ifndef OPENPETE_LAUNCHER_SHAPES_H
#define OPENPETE_LAUNCHER_SHAPES_H

#include "imgui.h"

#include <string>

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

/** @brief Draw one line of text on a chip: the chip box (launcher_shape_panel at @p alpha) padded around the line's box, then the text in imgui; each recorded in the element manifest.
 *
 * The text's box is its origin and CalcTextSize at the current font; the
 * chip spans it grown by @p pad (layout px) and is recorded as <id>.panel,
 * the text as <id>.
 * @param id The text's manifest id.
 * @param pos The text's origin, window coordinates.
 * @param pad The chip's padding around the text box, layout px.
 * @param scale Window pixels per layout pixel.
 * @param col The text colour.
 * @param text The text.
 * @param alpha The box's opacity; at 0 the text draws alone, no shape is added and no <id>.panel recorded. */
void launcher_chip_text(const char* id, ImVec2 pos, ImVec2 pad, float scale, ImU32 col, const char* text,
                        float alpha = kChipAlpha);

/** @brief Return a chip's text in the chips' letter case: as written, or upper case when the launcher draws its chips in upper case.
 *
 * Every chip's text (the step chip, the quit chip, the corner chips) passes
 * through it before it is measured and drawn.
 * @param text The text, UTF-8; NULL reads as empty.
 * @return The text to draw. */
std::string launcher_chip_case(const char* text);

#endif /* OPENPETE_LAUNCHER_SHAPES_H */
