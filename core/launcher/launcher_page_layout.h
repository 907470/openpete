/* launcher_page_layout.h - the game page's geometry and the lines below its
 * boxes, as pure functions of the window, the measured text and the library,
 * shared by the page's draw (launcher_page.cpp) and
 * tests/unit/launcher_page_lines_test.cpp. Window pixels unless a name says
 * layout px. Header-only, no state, no imgui. */
#ifndef OPENPETE_LAUNCHER_PAGE_LAYOUT_H
#define OPENPETE_LAUNCHER_PAGE_LAYOUT_H

#include "launcher_chips.h"

#include "openpete_identify.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

namespace launcher_layout {

/* Box geometry, as fractions of the window. */
constexpr float kBoxTop = 0.40f;     /* the row's band top, of the height */
constexpr float kBoxH = 0.42f;       /* box height, of the height */
constexpr float kBoxGap = 0.05f;     /* gap between boxes, of the height */
constexpr float kBoxMaxRow = 0.92f;  /* the three-box row's widest, of the width */
constexpr float kBoxAspect = 0.75f;  /* width over height */
constexpr float kGrow = 1.08f;       /* the focused box's scale */

/* How far a frame reaches below its box, in u = box width / 300: Spyro 1's
 * corner collar, the deepest of the three frames (10.75 u against the column
 * frame's medallion at 10.4 u and the rule frame's 3.8 u). */
constexpr float kFrameReach = 10.75f;

/* A serial's chip, layout px: the padding around its line and the gap below
 * the row's lowest frame. The line is the corner chips' size (kCornerPx). */
constexpr float kIdPadX = 8.0f, kIdPadY = 3.0f;
constexpr float kIdGap = 2.0f;

/* The line a game without a disc adds to the page, as worded. */
constexpr const char* kDiscHint = "Drag and drop or select your own copy of the game disc";

/* A rect in window pixels. */
struct Rect { float x, y, w, h; };

/** @brief Return a rect scaled about its centre.
 *
 * @param r The rect.
 * @param s The scale.
 * @return The scaled rect. */
inline Rect scaled(const Rect& r, float s) {
    const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    return {cx - r.w * s * 0.5f, cy - r.h * s * 0.5f, r.w * s, r.h * s};
}

/** @brief Return a rect grown on each side by a padding in layout px.
 *
 * @param r The rect.
 * @param pad_x The horizontal padding.
 * @param pad_y The vertical padding.
 * @param scale Window pixels per layout pixel.
 * @return The grown rect. */
inline Rect padded(const Rect& r, float pad_x, float pad_y, float scale) {
    return {r.x - pad_x * scale, r.y - pad_y * scale, r.w + 2.0f * pad_x * scale, r.h + 2.0f * pad_y * scale};
}

/** @brief Return the boxes' resting rects: the three-box row's size, the drawn boxes centred as a group.
 *
 * @param n The number of boxes.
 * @param W The window width.
 * @param H The window height.
 * @return The rects, left to right. */
inline std::vector<Rect> box_rects(size_t n, float W, float H) {
    float h = kBoxH * H, w = kBoxAspect * h, gap = kBoxGap * H;
    const float k = std::min(1.0f, kBoxMaxRow * W / (3.0f * w + 2.0f * gap));
    h *= k; w *= k; gap *= k;
    const float x0 = (W - ((float)n * w + (float)(n - 1) * gap)) * 0.5f, y0 = kBoxTop * H + (kBoxH * H - h) * 0.5f;
    std::vector<Rect> out;
    for (size_t i = 0; i < n; i++) out.push_back({x0 + (float)i * (w + gap), y0, w, h});
    return out;
}

/** @brief Return the bottom of a box's frame at its grown size, the lowest any box of the row reaches.
 *
 * @param rest The box at rest.
 * @return The y. */
inline float frame_bottom(const Rect& rest) {
    const Rect g = scaled(rest, kGrow);
    return g.y + g.h + kFrameReach * g.w / 300.0f;
}

/** @brief Return a serial's line: centred below its box, its chip clear of the box's frame at the grown size, so the line stays put while the box grows.
 *
 * @param rest The box at rest.
 * @param tw The line's width.
 * @param th The line's height.
 * @param scale Window pixels per layout pixel.
 * @return The line's rect. */
inline Rect id_text(const Rect& rest, float tw, float th, float scale) {
    return {rest.x + (rest.w - tw) * 0.5f, frame_bottom(rest) + (kIdGap + kIdPadY) * scale, tw, th};
}

/** @brief Return the disc hint's line: centred in the window on the corner chips' row, a chip of their padding at their margin from the bottom edge.
 *
 * @param W The window width.
 * @param H The window height.
 * @param tw The line's width.
 * @param th The line's height.
 * @param scale Window pixels per layout pixel.
 * @return The line's rect. */
inline Rect hint_text(float W, float H, float tw, float th, float scale) {
    return {(W - tw) * 0.5f, H - (kCornerMargin + kCornerPadY) * scale - th, tw, th};
}

/* What the page's lines say on one present. */
struct PageLines {
    std::vector<const char*> ids;   /* per box, its disc's serial; NULL for no line */
    const char* hint;               /* the disc hint, NULL for none */
};

/** @brief Resolve the page's lines: each box's serial from the game registry (openpete_serial_of_game), and the disc hint while a box's game has no library entry.
 *
 * The registry holds one serial per game and an ingest refuses any other, so
 * a box with an entry and a box without read the same table. The message
 * panel covers the serials' row (the shortest panel with an error reference
 * rises to it), so no line shows while the panel does.
 * @param box_games Each box's game id, left to right.
 * @param entry_games Each library entry's game id; NULL with @p count 0 for none.
 * @param count The entry count.
 * @param panel_shown true while the message panel shows.
 * @return The lines. */
inline PageLines page_lines(const std::vector<const char*>& box_games, const char* const* entry_games, int count,
                            bool panel_shown) {
    PageLines out = {std::vector<const char*>(box_games.size(), nullptr), nullptr};
    if (panel_shown) return out;
    for (size_t b = 0; b < box_games.size(); b++) {
        out.ids[b] = openpete_serial_of_game(box_games[b]);
        bool held = false;
        for (int i = 0; i < count && !held; i++)
            held = entry_games[i] && !std::strcmp(entry_games[i], box_games[b]);
        if (!held) out.hint = kDiscHint;
    }
    return out;
}

}  // namespace launcher_layout

#endif /* OPENPETE_LAUNCHER_PAGE_LAYOUT_H */
