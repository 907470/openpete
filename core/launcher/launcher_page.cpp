// core/launcher/launcher_page.cpp - the game page: one box per game the
// executable can get a module for, centred as a group, each in its game's own
// frame drawing (contract in launcher_internal.h).
//
// A box is 3:4 portrait, sized from the window height (42% of it, its row
// at 40%) and shrunk with the others when the three-box row would pass 92%
// of the window width; one or two boxes keep the three-box size. A box with
// a library disc shows its game's picture (launcher_page_set_picture) when
// one is set, else the vertical fill ramp, and a game without one the
// empty-box drawing (the fill ramp, a disc outline and the game's
// name in the body font), as does a game whose library entry holds a failed
// module build (launcher_page_set_failed); any other game with a library
// disc adds its title logo,
// centred and overlapping the frame's top edge by a quarter of the logo's
// height. The focused box tints its frame toward its focus palette and grows
// to 1.08x about its centre, easing with a 40 ms time constant. Every shape
// is an ImDrawList call: convex fills, vertical or horizontal colour ramps,
// circles, polylines and arcs; the frame units are u = box width / 300.
//
// The page's lines (launcher_page_lines_draw): below each box its game's disc
// serial, and while a game has no disc, the disc hint once, centred on the
// corner chips' row. A disc goes to the box its serial names whichever box
// chose it, so the hint belongs to the page and not to a box. Each line sits
// on a chip that fills with the sky's dome cover (launcher_sky_cover): over the
// flat background the text draws alone and the lines add no shape to the pass.
#include "launcher_internal.h"
#include "launcher_page_layout.h"
#include "launcher_shapes.h"
#include "launcher_sky.h"

#include "imgui.h"
#include "imgui_internal.h"   /* ImGui::RegisterUserTexture */

#include "openpete_log.h"
#include "openpete_time.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace {

using namespace launcher_layout;

constexpr double kGrowTau = 0.040;   /* the grow's time constant, seconds */

/* The title logo: its width at most kLogoW of the box width, its height at
 * most kLogoMaxH of it, kLogoDrop of its height below the frame's top edge;
 * a texel kLogoPar as wide as it is tall (the games draw it 1:1 into a
 * 512x240 display shown 4:3). */
constexpr float kLogoW = 0.92f;
constexpr float kLogoMaxH = 0.42f;
constexpr float kLogoDrop = 0.25f;
constexpr float kLogoPar = (4.0f / 3.0f) / (512.0f / 240.0f);

/* A box's empty-box name: laid out in a box of the resting size, its size
 * 0.07 of the box width (at most 22 u), each line fitted to 0.86 of the
 * width, the first line's top at 0.70 of the height, lines 1.35 sizes apart. */
constexpr float kNameSize = 0.07f;
constexpr float kNameFit = 0.86f;
constexpr float kNameTop = 0.70f;
constexpr float kNameLead = 1.35f;
constexpr uint8_t kNameCol[4] = {220, 210, 240, 255};

/* A colour; alpha 255 unless given. */
struct C4 { uint8_t r, g, b, a; };

/** @brief Pack a colour for imgui.
 *
 * @param c The colour.
 * @return The packed colour. */
ImU32 col(C4 c) { return IM_COL32(c.r, c.g, c.b, c.a); }

/** @brief Mix two colours by @p k, rounding each channel; the alpha is the first colour's.
 *
 * @param a The colour at k = 0.
 * @param b The colour at k = 1.
 * @param k The mix.
 * @return The mixed colour. */
C4 mix(C4 a, C4 b, float k) {
    const auto m = [k](uint8_t x, uint8_t y) {
        return (uint8_t)std::lround((float)x + ((float)y - (float)x) * k);
    };
    return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), a.a};
}

/** @brief Return a colour with another alpha.
 *
 * @param c The colour.
 * @param a The alpha.
 * @return The colour at @p a. */
C4 alpha(C4 c, uint8_t a) { return {c.r, c.g, c.b, a}; }

constexpr C4 kOutline = {0, 0, 0, 150};
constexpr C4 kGemDeep = {70, 46, 150, 255}, kGemBody = {132, 104, 226, 255};
constexpr C4 kGemLight = {190, 176, 255, 255}, kGemRim = {222, 214, 255, 170};

enum class FrameKind { Silver, Columns, Rule };

/* One game's frame: its kind, the empty box's fill ramp and line colour, the
 * interior inset in u, and the metal at rest and at focus (hi, mid, lo, ink;
 * the rule frame uses hi as its rule). */
struct Theme {
    const char* game;
    FrameKind kind;
    C4 fill0, fill1, line;
    float inset;
    C4 rest[4], focus[4];
};

constexpr Theme kThemes[] = {
    {"spyro-1", FrameKind::Silver, {24, 28, 66, 225}, {8, 10, 26, 225}, {200, 194, 226, 200}, 4.0f,
     {{238, 234, 250, 255}, {176, 170, 200, 255}, {132, 126, 160, 255}, {78, 70, 112, 255}},
     {{255, 244, 180, 255}, {232, 186, 74, 255}, {176, 122, 30, 255}, {112, 66, 8, 255}}},
    {"spyro-2", FrameKind::Columns, {72, 32, 74, 225}, {30, 14, 36, 225}, {234, 220, 178, 200}, 4.0f,
     {{252, 246, 222, 255}, {232, 216, 172, 255}, {196, 174, 124, 255}, {128, 104, 64, 255}},
     {{255, 244, 184, 255}, {246, 200, 84, 255}, {206, 146, 40, 255}, {130, 82, 12, 255}}},
    {"spyro-3", FrameKind::Rule, {42, 20, 84, 225}, {16, 8, 38, 225}, {255, 228, 0, 200}, 3.0f,
     {{255, 230, 0, 255}, {255, 230, 0, 255}, {255, 230, 0, 255}, {255, 230, 0, 255}},
     {{255, 160, 16, 255}, {255, 160, 16, 255}, {255, 160, 16, 255}, {255, 160, 16, 255}}},
};

/** @brief Return a game's theme: its row of kThemes, else the rule frame's.
 *
 * @param game The game id.
 * @return The theme. */
const Theme& theme_of(const char* game) {
    for (const Theme& t : kThemes)
        if (game && !strcmp(game, t.game)) return t;
    return kThemes[2];
}

/* A theme's metal at one focus mix. */
struct Metal { C4 hi, mid, lo, ink; };

/** @brief Return a theme's metal mixed toward its focus palette by @p k.
 *
 * @param t The theme.
 * @param k 0 at rest, 1 at full focus.
 * @return The metal. */
Metal metal(const Theme& t, float k) {
    return {mix(t.rest[0], t.focus[0], k), mix(t.rest[1], t.focus[1], k), mix(t.rest[2], t.focus[2], k),
            mix(t.rest[3], t.focus[3], k)};
}

/* ---- draw-list helpers ---------------------------------------------------- */

/* The four sides' colours of a ring: top, right, bottom, left. */
struct Sides { C4 t, r, b, l; };

/** @brief Return a ring's sides all in one colour.
 *
 * @param c The colour.
 * @return The sides. */
Sides flat(C4 c) { return {c, c, c, c}; }

/** @brief Return a raised bevel's sides: light top and left, dark bottom and right.
 *
 * @param light The light colour.
 * @param dark The dark colour.
 * @return The sides. */
Sides raised(C4 light, C4 dark) { return {light, dark, dark, light}; }

/** @brief Fill a quadrilateral, its corners in clockwise order on screen (AddConvexPolyFilled).
 *
 * @param dl The draw list.
 * @param a First corner.
 * @param b Second corner.
 * @param c Third corner.
 * @param d Fourth corner.
 * @param cl The colour. */
void quad(ImDrawList* dl, ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d, C4 cl) {
    const ImVec2 p[4] = {a, b, c, d};
    dl->AddConvexPolyFilled(p, 4, col(cl));
}

/** @brief Draw a rectangular ring between the inward offsets d0 and d1 (in u) of a rect, one colour per side.
 *
 * The whole ring is first stroked in the bottom side's colour, so the
 * antialiased mitres between the sides leave no crack; the four mitred
 * trapezoids then draw over it unless every side has that colour.
 * @param dl The draw list.
 * @param b The rect.
 * @param u The unit.
 * @param d0 The outer edge's inward offset; negative lies outside.
 * @param d1 The inner edge's inward offset.
 * @param s The sides' colours. */
void ring(ImDrawList* dl, const Rect& b, float u, float d0, float d1, const Sides& s) {
    const float o[4] = {b.x + d0 * u, b.y + d0 * u, b.x + b.w - d0 * u, b.y + b.h - d0 * u};
    const float i[4] = {b.x + d1 * u, b.y + d1 * u, b.x + b.w - d1 * u, b.y + b.h - d1 * u};
    const float m = (d0 + d1) * 0.5f * u;
    dl->PathRect(ImVec2(b.x + m, b.y + m), ImVec2(b.x + b.w - m, b.y + b.h - m));
    dl->PathStroke(col(s.b), (d1 - d0) * u, ImDrawFlags_Closed);
    if (!memcmp(&s.t, &s.b, sizeof s.t) && !memcmp(&s.l, &s.b, sizeof s.t) && !memcmp(&s.r, &s.b, sizeof s.t))
        return;
    quad(dl, {o[0], o[1]}, {o[2], o[1]}, {i[2], i[1]}, {i[0], i[1]}, s.t);
    quad(dl, {o[2], o[1]}, {o[2], o[3]}, {i[2], i[3]}, {i[2], i[1]}, s.r);
    quad(dl, {o[2], o[3]}, {o[0], o[3]}, {i[0], i[3]}, {i[2], i[3]}, s.b);
    quad(dl, {o[0], o[3]}, {o[0], o[1]}, {i[0], i[1]}, {i[0], i[3]}, s.l);
}

/** @brief Fill a rect with a vertical (top to bottom) or horizontal (left to right) colour ramp (AddRectFilledMultiColor).
 *
 * @param dl The draw list.
 * @param x0 Left.
 * @param y0 Top.
 * @param x1 Right.
 * @param y1 Bottom.
 * @param a The colour at the top or left.
 * @param b The colour at the bottom or right.
 * @param vertical true for a vertical ramp. */
void ramp(ImDrawList* dl, float x0, float y0, float x1, float y1, C4 a, C4 b, bool vertical) {
    if (vertical) dl->AddRectFilledMultiColor({x0, y0}, {x1, y1}, col(a), col(a), col(b), col(b));
    else dl->AddRectFilledMultiColor({x0, y0}, {x1, y1}, col(a), col(b), col(b), col(a));
}

/** @brief Fill a rect in one colour.
 *
 * @param dl The draw list.
 * @param x0 Left.
 * @param y0 Top.
 * @param x1 Right.
 * @param y1 Bottom.
 * @param c The colour. */
void rect(ImDrawList* dl, float x0, float y0, float x1, float y1, C4 c) {
    dl->AddRectFilled({x0, y0}, {x1, y1}, col(c));
}

/** @brief Fill a circle.
 *
 * @param dl The draw list.
 * @param x Centre x.
 * @param y Centre y.
 * @param r Radius.
 * @param c The colour. */
void disc(ImDrawList* dl, float x, float y, float r, C4 c) { dl->AddCircleFilled({x, y}, r, col(c)); }

/** @brief Stroke an arc from angle a0 to a1 (radians, clockwise on screen from +x).
 *
 * @param dl The draw list.
 * @param x Centre x.
 * @param y Centre y.
 * @param r Radius.
 * @param a0 Start angle.
 * @param a1 End angle.
 * @param c The colour.
 * @param th The stroke width. */
void arc(ImDrawList* dl, float x, float y, float r, float a0, float a1, C4 c, float th) {
    dl->PathArcTo({x, y}, r, a0, a1);
    dl->PathStroke(col(c), th);
}

/** @brief Draw a faceted amethyst: a dark rim disc, a body disc and a light disc each shifted toward the upper left, a specular dot and a lower-right rim light.
 *
 * @param dl The draw list.
 * @param x Centre x.
 * @param y Centre y.
 * @param r Radius. */
void amethyst(ImDrawList* dl, float x, float y, float r) {
    disc(dl, x, y, r, kGemDeep);
    disc(dl, x - r * 0.10f, y - r * 0.10f, r * 0.82f, kGemBody);
    disc(dl, x - r * 0.24f, y - r * 0.24f, r * 0.48f, kGemLight);
    disc(dl, x - r * 0.36f, y - r * 0.36f, r * 0.20f, {255, 255, 255, 235});
    arc(dl, x, y, r * 0.74f, 0.15f * IM_PI, 0.45f * IM_PI, kGemRim, std::max(1.0f, r * 0.14f));
}

/* ---- the frames ----------------------------------------------------------- */

/** @brief Draw the silver frame's engraving: along each side, between the corner collars, a sine of amplitude 2 u whose period is the nearest to 14 u that fits the side a whole number of times, with a dot in each lobe.
 *
 * @param dl The draw list.
 * @param b The box.
 * @param u The unit.
 * @param c The metal. */
void silver_scroll(ImDrawList* dl, const Rect& b, float u, const Metal& c) {
    const float d = 1.75f * u, a = 2.0f * u, th = std::max(1.0f, 1.1f * u), dot = std::max(0.7f, 0.9f * u);
    const C4 ink = alpha(c.ink, 230);
    const float sides[4][4] = {{b.x + d, b.y + d, 1, 0}, {b.x + d, b.y + b.h - d, 1, 0},
                               {b.x + d, b.y + d, 0, 1}, {b.x + b.w - d, b.y + d, 0, 1}};
    std::vector<ImVec2> pts;
    for (const auto& sd : sides) {
        const float x0 = sd[0], y0 = sd[1], dx = sd[2], dy = sd[3];
        const float len = dx != 0 ? b.w - 2 * d : b.h - 2 * d;
        const int n = std::max(2, (int)std::lround(len / (14.0f * u)));
        const float per = len / (float)n;
        pts.clear();
        for (int k = 0; k <= 12 * n; k++) {
            const float s = per / 12.0f * (float)k, o = a * std::sin(2.0f * IM_PI * s / per);
            pts.push_back({x0 + dx * s - dy * o, y0 + dy * s + dx * o});
        }
        dl->AddPolyline(pts.data(), (int)pts.size(), col(ink), th);
        for (int j = 0; j < 2 * n; j++) {
            const float s = ((float)j + 0.5f) * per / 2.0f, o = (j % 2 ? 1.0f : -1.0f) * a * 0.85f;
            disc(dl, x0 + dx * s - dy * o, y0 + dy * s + dx * o, dot, ink);
        }
    }
}

/** @brief Draw the cast-silver frame: a 12.5 u band (raised outer rim, channel with the engraving, sunk inner rim) over a black outline, with a faceted amethyst in a bevelled collar at each corner.
 *
 * @param dl The draw list.
 * @param b The box.
 * @param u The unit.
 * @param c The metal. */
void frame_silver(ImDrawList* dl, const Rect& b, float u, const Metal& c) {
    ring(dl, b, u, -5.5f, 9.5f, flat(kOutline));
    ring(dl, b, u, -4.0f, -1.5f, raised(c.hi, c.lo));
    const C4 ml = mix(c.mid, c.hi, 0.18f);
    ring(dl, b, u, -1.5f, 5.0f, raised(ml, c.mid));
    silver_scroll(dl, b, u, c);
    ring(dl, b, u, 5.0f, 7.5f, raised(c.lo, c.hi));
    ring(dl, b, u, 7.5f, 8.5f, flat({0, 0, 0, 160}));
    const float d = 1.75f * u;
    const float corners[4][2] = {{b.x + d, b.y + d}, {b.x + b.w - d, b.y + d}, {b.x + d, b.y + b.h - d},
                                 {b.x + b.w - d, b.y + b.h - d}};
    for (const auto& p : corners) {
        disc(dl, p[0], p[1], 12.5f * u, kOutline);
        disc(dl, p[0] + 0.8f * u, p[1] + 0.8f * u, 11.0f * u, c.lo);
        disc(dl, p[0] - 0.6f * u, p[1] - 0.6f * u, 10.2f * u, c.hi);
        disc(dl, p[0], p[1], 8.4f * u, c.ink);
        amethyst(dl, p[0], p[1], 7.4f * u);
    }
}

/** @brief Draw one moulded rail of the column frame: an outlined vertical ramp with a groove 4 u from its outer edge and a light line beside the groove.
 *
 * @param dl The draw list.
 * @param b The box.
 * @param u The unit.
 * @param c The metal.
 * @param ya The rail's top.
 * @param yb The rail's bottom.
 * @param top true for the top rail (the groove near its top). */
void columns_rail(ImDrawList* dl, const Rect& b, float u, const Metal& c, float ya, float yb, bool top) {
    const float o = 1.2f * u, x1 = b.x + b.w;
    rect(dl, b.x - o, ya - o, x1 + o, yb + o, kOutline);
    ramp(dl, b.x, ya, x1, yb, c.hi, c.lo, true);
    const float g = top ? ya + 4.0f * u : yb - 4.0f * u;
    dl->AddLine({b.x, g}, {x1, g}, col(alpha(c.ink, 200)), std::max(1.0f, u));
    dl->AddLine({b.x, g + u}, {x1, g + u}, col(c.hi), std::max(0.8f, 0.8f * u));
}

/* One column of the column frame, in column units v = 1.2 u about its
 * anchor: x = cx + a v, a height t v below the box's top or above its bottom. */
struct Column {
    float cx, top, bottom, v, o;
    /** @brief Return the x at a column units from the axis.
     *
     * @param a The offset.
     * @return The x. */
    float X(float a) const { return cx + a * v; }
    /** @brief Return the y at t column units below the box's top edge.
     *
     * @param t The offset.
     * @return The y. */
    float T(float t) const { return top + t * v; }
    /** @brief Return the y at t column units below the box's bottom edge.
     *
     * @param t The offset; negative lies above the edge.
     * @return The y. */
    float B(float t) const { return bottom + t * v; }
};

/** @brief Draw a column's shaft (three ramps shading a lit cylinder, two flutes) and its finial (a neck and a lit ball).
 *
 * @param dl The draw list.
 * @param k The column.
 * @param u The unit.
 * @param c The metal. */
void column_shaft(ImDrawList* dl, const Column& k, float u, const Metal& c) {
    const C4 shadow = mix(c.lo, c.ink, 0.6f);
    rect(dl, k.X(-7) - k.o, k.top, k.X(7) + k.o, k.bottom, kOutline);
    ramp(dl, k.X(-7), k.top, k.X(-3), k.bottom, c.lo, c.hi, false);
    ramp(dl, k.X(-3), k.top, k.X(1.5f), k.bottom, c.hi, c.mid, false);
    ramp(dl, k.X(1.5f), k.top, k.X(7), k.bottom, c.mid, shadow, false);
    for (const float fx : {-2.2f, 3.4f})
        dl->AddLine({k.X(fx), k.T(16)}, {k.X(fx), k.B(-14)}, col(alpha(c.ink, 110)), std::max(1.0f, u));
    rect(dl, k.X(-3.2f) - k.o, k.T(-9.5f), k.X(3.2f) + k.o, k.T(-6), kOutline);
    ramp(dl, k.X(-3.2f), k.T(-9.5f), k.X(3.2f), k.T(-6), c.hi, c.lo, false);
    disc(dl, k.cx, k.T(-14), 5.5f * k.v + k.o, kOutline);
    disc(dl, k.X(0.6f), k.T(-13.4f), 5.5f * k.v, c.lo);
    disc(dl, k.X(-0.5f), k.T(-14.5f), 4.6f * k.v, c.hi);
    disc(dl, k.X(-1.9f), k.T(-15.9f), 1.3f * k.v, {255, 255, 255, 210});
}

/** @brief Draw a column's capital: the bevelled abacus slab, the block holding an amethyst, the collar and a volute under each abacus end.
 *
 * @param dl The draw list.
 * @param k The column.
 * @param u The unit.
 * @param c The metal. */
void column_capital(ImDrawList* dl, const Column& k, float u, const Metal& c) {
    rect(dl, k.X(-12.5f) - k.o, k.T(-6) - k.o, k.X(12.5f) + k.o, k.T(-1.5f) + k.o, kOutline);
    rect(dl, k.X(-10) - k.o, k.T(-1.5f), k.X(10) + k.o, k.T(12.5f), kOutline);
    rect(dl, k.X(-8.5f) - k.o, k.T(12.5f), k.X(8.5f) + k.o, k.T(15.5f) + k.o, kOutline);
    const Rect slab = {k.X(-12.5f), k.T(-6), 25.0f * k.v, k.T(-1.5f) - k.T(-6)};
    ring(dl, slab, 1.0f, 0.0f, std::min(12.5f * k.v, slab.h * 0.5f), raised(c.hi, c.lo));
    ramp(dl, k.X(-10), k.T(-1.5f), k.X(10), k.T(12.5f), c.mid, c.lo, false);
    ramp(dl, k.X(-10), k.T(-1.5f), k.X(-6), k.T(12.5f), c.hi, c.mid, false);
    rect(dl, k.X(-8.5f), k.T(12.5f), k.X(8.5f), k.T(15.5f), c.lo);
    for (const float sx : {-1.0f, 1.0f}) {
        const float vx = k.X(sx * 11), vy = k.T(1.2f), a0 = sx > 0 ? 0.0f : IM_PI;
        disc(dl, vx, vy, 3.2f * k.v + k.o, kOutline);
        disc(dl, vx, vy, 3.2f * k.v, c.mid);
        arc(dl, vx, vy, 1.9f * k.v, a0, a0 + 1.5f * IM_PI, alpha(c.ink, 220), std::max(1.0f, 0.9f * u));
        disc(dl, vx, vy, 0.8f * k.v, c.ink);
    }
    disc(dl, k.cx, k.T(5.5f), 5.6f * k.v, c.ink);
    amethyst(dl, k.cx, k.T(5.5f), 4.4f * k.v);
}

/** @brief Draw a column's plinth: a torus ramp over a block with a mid face inside a 2.5 v raised bevel.
 *
 * @param dl The draw list.
 * @param k The column.
 * @param c The metal. */
void column_plinth(ImDrawList* dl, const Column& k, const Metal& c) {
    rect(dl, k.X(-9.5f) - k.o, k.B(-14) - k.o, k.X(9.5f) + k.o, k.B(-10), kOutline);
    rect(dl, k.X(-12.5f) - k.o, k.B(-10) - k.o, k.X(12.5f) + k.o, k.B(7) + k.o, kOutline);
    ramp(dl, k.X(-9.5f), k.B(-14), k.X(9.5f), k.B(-10), c.hi, c.lo, false);
    rect(dl, k.X(-12.5f), k.B(-10), k.X(12.5f), k.B(7), c.mid);
    ring(dl, {k.X(-12.5f), k.B(-10), 25.0f * k.v, 17.0f * k.v}, k.v, 0.0f, 2.5f, raised(c.hi, c.lo));
}

/** @brief Draw the column frame: moulded top and bottom rails, an ivory column with capital, finial and plinth on the left and right edges, and a lozenge medallion holding an amethyst at the bottom centre.
 *
 * @param dl The draw list.
 * @param b The box.
 * @param u The unit.
 * @param c The metal. */
void frame_columns(ImDrawList* dl, const Rect& b, float u, const Metal& c) {
    const float y1 = b.y + b.h;
    columns_rail(dl, b, u, c, b.y - 3.5f * u, b.y + 5.5f * u, true);
    columns_rail(dl, b, u, c, y1 - 5.5f * u, y1 + 3.5f * u, false);
    for (const float cx : {b.x, b.x + b.w}) {
        const Column k = {cx, b.y, y1, 1.2f * u, 1.2f * u};
        column_shaft(dl, k, u, c);
        column_capital(dl, k, u, c);
        column_plinth(dl, k, c);
    }
    const float mx = b.x + b.w * 0.5f, my = y1 - u, dw = 15.0f * u, dh = 10.0f * u, e = 1.4f * u;
    quad(dl, {mx - dw - e, my}, {mx, my - dh - e}, {mx + dw + e, my}, {mx, my + dh + e}, kOutline);
    quad(dl, {mx - dw, my}, {mx, my - dh}, {mx + dw, my}, {mx, my + dh}, c.lo);
    quad(dl, {mx - dw, my}, {mx, my - dh}, {mx + dw * 0.5f, my - dh * 0.5f}, {mx - dw * 0.5f, my + dh * 0.5f}, c.hi);
    disc(dl, mx, my, 6.2f * u, c.ink);
    amethyst(dl, mx, my, 5.2f * u);
}

/** @brief Draw the rule frame: a flat 5.5 u outer rule and a 1.8 u inner rule 2.2 u inside it, over a black ring that gives both hairline edges; square corners.
 *
 * @param dl The draw list.
 * @param b The box.
 * @param u The unit.
 * @param c The metal; hi is the rule. */
void frame_rule(ImDrawList* dl, const Rect& b, float u, const Metal& c) {
    ring(dl, b, u, -3.8f, 8.3f, flat({0, 0, 0, 190}));
    ring(dl, b, u, -2.5f, 3.0f, flat(c.hi));
    ring(dl, b, u, 5.2f, 7.0f, flat(c.hi));
}

/* ---- the page's state ------------------------------------------------------ */

/* An image the page draws from a texture it owns. */
struct Image {
    ImTextureData* tex = nullptr;
    int w = 0, h = 0;
};

/* One game's box across presents. */
struct BoxState {
    std::string game;
    float scale = 1.0f;
    Image logo, picture;
    std::vector<unsigned char> logo_src;   /* the logo as the cache holds it, logo_sw x logo_sh RGBA */
    int logo_sw = 0, logo_sh = 0;
    op_time_t logo_tried = 0;   /* the last logo read attempt; 0 for none */
    unsigned picture_gen = ~0u; /* launcher_picture_gen at the last picture read; ~0 for none */
    bool failed = false;        /* the owning entry holds a failed module build (launcher_page_set_failed) */
    bool unsupported = false;   /* the owning entry's disc is not a supported build (launcher_page_set_unsupported) */
};

std::deque<BoxState> s_boxes;    /* by game, in first-seen order; a push keeps references valid */
op_time_t s_last = 0;            /* the last page present; 0 while the page is not showing */
bool s_animating = false;        /* a box's scale had not reached its target at the last present */

/** @brief Return a game's box state, adding it on first use.
 *
 * @param game The game id.
 * @return The state. */
BoxState& box_state(const char* game) {
    for (BoxState& b : s_boxes)
        if (b.game == game) return b;
    s_boxes.push_back({});
    s_boxes.back().game = game;
    return s_boxes.back();
}

/** @brief Upload RGBA pixels as a texture the page owns, registered with imgui and created by its renderer backend at the next render.
 *
 * @param img Receives the texture; a previous one is released first.
 * @param rgba width * height * 4 bytes.
 * @param w The width.
 * @param h The height. */
void image_set(Image& img, const unsigned char* rgba, int w, int h) {
    if (img.tex) {
        img.tex->DestroyPixels();   /* else the backend's destroy re-creates it from them */
        img.tex->UnusedFrames = 1 << 20;
        img.tex->SetStatus(ImTextureStatus_WantDestroy);
        img.tex = nullptr;   /* stays registered; the backend destroys it at its next render */
    }
    auto* t = IM_NEW(ImTextureData)();
    t->Create(ImTextureFormat_RGBA32, w, h);
    memcpy(t->Pixels, rgba, (size_t)w * (size_t)h * 4u);
    t->UseColors = true;
    t->UsedRect = {0, 0, (unsigned short)w, (unsigned short)h};
    ImGui::RegisterUserTexture(t);
    img = {t, w, h};
}

/** @brief Release an image's pixels once its texture is created: the backend reads them only to create it.
 *
 * @param img The image. */
void image_settle(Image& img) {
    if (img.tex && img.tex->Status == ImTextureStatus_OK && img.tex->Pixels) img.tex->DestroyPixels();
}

/** @brief Set the logo's texture to its source resampled to at most @p w x @p h texels (launcher_image_fit), unless the texture already has the size that gives.
 *
 * @param bs The box state holding the source.
 * @param w The drawn width in pixels, rounded up.
 * @param h The drawn height in pixels, rounded up. */
void logo_fit(BoxState& bs, int w, int h) {
    const int fw = std::clamp(w, 1, bs.logo_sw), fh = std::clamp(h, 1, bs.logo_sh);
    if (bs.logo.tex && bs.logo.w == fw && bs.logo.h == fh) return;
    std::vector<unsigned char> px(bs.logo_src.size());
    int ow = 0, oh = 0;
    launcher_image_fit(bs.logo_src.data(), bs.logo_sw, bs.logo_sh, fw, fh, px.data(), &ow, &oh);
    image_set(bs.logo, px.data(), ow, oh);
}

/** @brief Read a game's title logo from its asset cache unless loaded, at most every 250 ms while the cache holds none.
 *
 * @param g The game.
 * @param bs Its box state.
 * @param now The present's time. */
void logo_poll(const openpete_launcher_game* g, BoxState& bs, op_time_t now) {
    image_settle(bs.logo);
    image_settle(bs.picture);
    if (bs.logo.tex || !g->load_logo || (bs.logo_tried && now - bs.logo_tried < 250 * 1000000LL)) return;
    bs.logo_tried = now;
    int w = 0, h = 0;
    unsigned char* rgba = g->load_logo(g, &w, &h);
    if (!rgba) return;
    bs.logo_src.assign(rgba, rgba + (size_t)w * (size_t)h * 4u);
    bs.logo_sw = w;
    bs.logo_sh = h;
    free(rgba);
    logo_fit(bs, w, h);
    OPL_INFO_F("launcher", "game page: %s title logo %dx%d read from the asset cache", bs.game.c_str(), w, h);
}

/** @brief Read a game's box picture from its asset cache into the box (launcher_page_set_picture) at the first present and after each picture change.
 *
 * A read that finds no picture keeps the one the box shows.
 * @param g The game.
 * @param bs Its box state. */
void picture_poll(const openpete_launcher_game* g, BoxState& bs) {
    const unsigned gen = launcher_picture_gen();
    if (!g->load_picture || bs.picture_gen == gen) return;
    bs.picture_gen = gen;
    int w = 0, h = 0;
    unsigned char* rgba = g->load_picture(g, &w, &h);
    if (!rgba) return;
    launcher_page_set_picture(bs.game.c_str(), rgba, w, h);
    free(rgba);
    OPL_INFO_F("launcher", "game page: %s box picture %dx%d read from the asset cache", bs.game.c_str(), w, h);
}

/* One box of the page being drawn. */
struct Box {
    const openpete_launcher_game* g;
    int entry;   /* the library entry owning the box, -1 for none */
    BoxState* st;
};

/** @brief Return the page's games: the launcher's per-game entries marked on_page, or the launcher's own game when it names one.
 *
 * @return The games, in the descriptor's order. */
std::vector<const openpete_launcher_game*> page_games() {
    std::vector<const openpete_launcher_game*> out;
    const openpete_launcher_game* d = launcher_game();
    if (!d) return out;
    for (int i = 0; i < d->game_count; i++)
        if (d->games[i] && d->games[i]->game_id && d->games[i]->on_page) out.push_back(d->games[i]);
    if (d->game_count == 0 && d->game_id) out.push_back(d);
    return out;
}

/** @brief Return the page's boxes, each with the first library entry of its game (the most recently played: the entries come most recent first).
 *
 * @param entry_games Each library entry's game id.
 * @param count Their number.
 * @return The boxes. */
std::vector<Box> page_boxes(const char* const* entry_games, int count) {
    std::vector<Box> out;
    for (const openpete_launcher_game* g : page_games()) {
        int entry = -1;
        for (int i = 0; i < count && entry < 0; i++)
            if (entry_games && entry_games[i] && !strcmp(entry_games[i], g->game_id)) entry = i;
        out.push_back({g, entry, &box_state(g->game_id)});
    }
    return out;
}

/** @brief Ease each box's scale toward its target (kGrow for the focused box, 1 for the others) by the time since the last page present; the first present of a page snaps them.
 *
 * @param boxes The page's boxes.
 * @param focus The focused box's index.
 * @param now The present's time. */
void animate(const std::vector<Box>& boxes, size_t focus, op_time_t now) {
    const double dt = s_last ? (double)(now - s_last) / 1e9 : -1.0;
    const float step = dt < 0 ? 1.0f : (float)(1.0 - std::exp(-dt / kGrowTau));
    s_animating = false;
    for (size_t i = 0; i < boxes.size(); i++) {
        float& s = boxes[i].st->scale;
        const float tgt = i == focus ? kGrow : 1.0f;
        s += (tgt - s) * step;
        if (std::fabs(s - tgt) < 1e-4f) s = tgt;
        else s_animating = true;
    }
    s_last = now;
}

/** @brief Record a box element in the element manifest under box.<game>.<part>.
 *
 * @param game The game id.
 * @param part The part's name.
 * @param kind Its kind.
 * @param r Its rect. */
void manifest_box(const char* game, const char* part, const char* kind, const Rect& r) {
    const std::string id = std::string("box.") + game + "." + part;
    launcher_manifest_item(id.c_str(), kind, r.x, r.y, r.x + r.w, r.y + r.h);
}

/** @brief Draw the empty box's game name: laid out at the box's resting size, then its vertices scaled about the box centre to the drawn size, so a growing box scales the rasterized text instead of re-laying it out in whole-pixel glyph cells.
 *
 * The name splits after its first ": " into two lines. The atlas is sampled
 * linearly, so the grown text is slightly soft.
 * @param dl The draw list.
 * @param game The game id, for the manifest.
 * @param name The game's display name.
 * @param b The drawn box.
 * @param rest The box at rest. */
void draw_name(ImDrawList* dl, const char* game, const char* name, const Rect& b, const Rect& rest) {
    const float w0 = std::round(rest.w), h0 = std::round(rest.h), k = b.w / w0;
    const float cx = b.x + b.w * 0.5f, cy = b.y + b.h * 0.5f, ox = cx - w0 * 0.5f, oy = cy - h0 * 0.5f;
    const std::string full = name ? name : game;
    const size_t cut = full.find(": ");
    const std::string lines[2] = {cut == std::string::npos ? full : full.substr(0, cut + 1),
                                  cut == std::string::npos ? "" : full.substr(cut + 2)};
    const float p = std::min(w0 * kNameSize, 22.0f * w0 / 300.0f);
    float y = h0 * kNameTop;
    const int vtx0 = dl->VtxBuffer.Size;
    for (int i = 0; i < 2 && !lines[i].empty(); i++, y += p * kNameLead) {
        launcher_push_body_font(p);
        float fit = p;
        if (const float tw = ImGui::CalcTextSize(lines[i].c_str()).x; tw > w0 * kNameFit) fit = p * w0 * kNameFit / tw;
        ImGui::PopFont();
        launcher_push_body_font(fit);
        const ImVec2 sz = ImGui::CalcTextSize(lines[i].c_str());
        const float x = (w0 - sz.x) * 0.5f;
        dl->AddText({ox + x, oy + y}, col({kNameCol[0], kNameCol[1], kNameCol[2], kNameCol[3]}), lines[i].c_str());
        ImGui::PopFont();
        const std::string part = "name." + std::to_string(i);
        manifest_box(game, part.c_str(), "text", {b.x + x * k, b.y + y * k, sz.x * k, fit * k});
    }
    for (int i = vtx0; i < dl->VtxBuffer.Size; i++) {
        ImVec2& p2 = dl->VtxBuffer[i].pos;
        p2 = {cx + (p2.x - cx) * k, cy + (p2.y - cy) * k};
    }
}

/** @brief Draw the theme's fill ramp inside the frame's inset: the interior of a box without a picture.
 *
 * @param dl The draw list.
 * @param bx The box.
 * @param b The drawn box. */
void draw_fill(ImDrawList* dl, const Box& bx, const Rect& b) {
    const Theme& t = theme_of(bx.g->game_id);
    const float in = t.inset * b.w / 300.0f;
    const Rect fill = {b.x + in, b.y + in, b.w - 2 * in, b.h - 2 * in};
    ramp(dl, fill.x, fill.y, fill.x + fill.w, fill.y + fill.h, t.fill0, t.fill1, true);
    manifest_box(bx.g->game_id, "fill", "rectGradient", fill);
}

/** @brief Draw the empty-box drawing: the fill ramp, a disc outline with a plus badge, and the game's name.
 *
 * @param dl The draw list.
 * @param bx The box.
 * @param b The drawn box.
 * @param rest The box at rest. */
void draw_empty(ImDrawList* dl, const Box& bx, const Rect& b, const Rect& rest) {
    const Theme& t = theme_of(bx.g->game_id);
    const float u = b.w / 300.0f;
    draw_fill(dl, bx, b);
    const float cx = b.x + b.w * 0.5f, cy = b.y + b.h * 0.38f, r = b.w * 0.24f;
    const ImU32 line = col(t.line);
    dl->AddCircle({cx, cy}, r, line, 0, 3.0f * u);
    dl->AddCircle({cx, cy}, r * 0.2f, line, 0, 3.0f * u);
    const float px = cx + r * 0.72f, py = cy + r * 0.72f, pr = r * 0.3f;
    dl->AddCircleFilled({px, py}, pr, IM_COL32(10, 8, 18, 242));
    dl->AddCircle({px, py}, pr, line, 0, 3.0f * u);
    dl->AddLine({px - pr * 0.5f, py}, {px + pr * 0.5f, py}, line, 3.5f * u);
    dl->AddLine({px, py - pr * 0.5f}, {px, py + pr * 0.5f}, line, 3.5f * u);
    manifest_box(bx.g->game_id, "disc", "paths", {cx - r, cy - r, 2 * r, 2 * r});
    draw_name(dl, bx.g->game_id, bx.g->display_name, b, rest);
}

/** @brief Draw the box's picture cover-cropped into the frame's inset.
 *
 * @param dl The draw list.
 * @param bx The box.
 * @param b The drawn box. */
void draw_picture(ImDrawList* dl, const Box& bx, const Rect& b) {
    const Image& im = bx.st->picture;
    const float in = theme_of(bx.g->game_id).inset * b.w / 300.0f;
    const Rect r = {b.x + in, b.y + in, b.w - 2 * in, b.h - 2 * in};
    /* Cover: the UV window keeps the picture's aspect and fills the rect. */
    const float sx = r.w / (float)im.w, sy = r.h / (float)im.h, s = std::max(sx, sy);
    const float uw = r.w / ((float)im.w * s), vh = r.h / ((float)im.h * s);
    dl->AddImage(im.tex->GetTexRef(), {r.x, r.y}, {r.x + r.w, r.y + r.h}, {(1 - uw) * 0.5f, (1 - vh) * 0.5f},
                 {(1 + uw) * 0.5f, (1 + vh) * 0.5f});
    manifest_box(bx.g->game_id, "picture", "image", r);
}

/** @brief Draw the box's title logo centred on it, kLogoDrop of its height below the frame's top edge, at kLogoPar texel aspect, linearly filtered from a texture resampled to its size at full focus.
 *
 * The texture holds the source resampled to the logo's drawn size at the
 * focused scale (kGrow), so a box at any scale between rest and focus
 * samples it at 1 to kGrow texels per pixel.
 * @param dl The draw list.
 * @param bx The box.
 * @param b The drawn box. */
void draw_logo(ImDrawList* dl, const Box& bx, const Rect& b) {
    BoxState& st = *bx.st;
    const float aw = (float)st.logo_sw * kLogoPar;
    const float s = std::min(b.w * kLogoW / aw, b.w * kLogoMaxH / (float)st.logo_sh);
    const float w = aw * s, h = (float)st.logo_sh * s, x = b.x + (b.w - w) * 0.5f, y = b.y - h * (1.0f - kLogoDrop);
    const float grow = kGrow / std::max(st.scale, 1e-3f);
    logo_fit(st, (int)std::ceil(w * grow), (int)std::ceil(h * grow));
    dl->AddImage(st.logo.tex->GetTexRef(), {x, y}, {x + w, y + h});
    manifest_box(bx.g->game_id, "logo", "image", {x, y, w, h});
}

/** @brief Draw a box's frame in its game's style at a focus mix.
 *
 * @param dl The draw list.
 * @param bx The box.
 * @param b The drawn box.
 * @param k The focus mix: 0 at rest, 1 at full focus. */
void draw_frame(ImDrawList* dl, const Box& bx, const Rect& b, float k) {
    const Theme& t = theme_of(bx.g->game_id);
    const Metal c = metal(t, k);
    const float u = b.w / 300.0f;
    if (t.kind == FrameKind::Silver) frame_silver(dl, b, u, c);
    else if (t.kind == FrameKind::Columns) frame_columns(dl, b, u, c);
    else frame_rule(dl, b, u, c);
    manifest_box(bx.g->game_id, "frame", "frame", b);
}

/** @brief Hit-test one box with an invisible button over its drawn rect, showing the hand cursor while the pointer is over it.
 *
 * @param i The box's index, for the button's id.
 * @param b The drawn box.
 * @param hovered Set to @p i when the pointer is over the box.
 * @return true when the box was clicked. */
bool box_input(size_t i, const Rect& b, int* hovered) {
    ImGui::SetCursorScreenPos({b.x, b.y});
    const std::string id = "##box." + std::to_string(i);
    const bool clicked = ImGui::InvisibleButton(id.c_str(), {std::max(1.0f, b.w), std::max(1.0f, b.h)});
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        *hovered = (int)i;
    }
    return clicked;
}

/** @brief Return what activating a box does: its library entry's index, or LAUNCHER_PAGE_BROWSE for a box without one.
 *
 * @param bx The box.
 * @return The activation. */
int activation(const Box& bx) { return bx.entry >= 0 ? bx.entry : LAUNCHER_PAGE_BROWSE; }

}  // namespace

int launcher_page_draw(const char* const* entry_games, int count, int focus, int* hovered) {
    *hovered = -1;
    std::vector<Box> boxes = page_boxes(entry_games, count);
    if (boxes.empty()) return LAUNCHER_PAGE_NONE;
    const op_time_t now = op_now();
    for (const Box& bx : boxes) {
        logo_poll(bx.g, *bx.st, now);
        picture_poll(bx.g, *bx.st);
    }
    const bool opening = s_last == 0;
    animate(boxes, (size_t)focus, now);
    const ImVec2 d = ImGui::GetIO().DisplaySize;
    const std::vector<Rect> rects = box_rects(boxes.size(), d.x, d.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    int act = LAUNCHER_PAGE_NONE;
    std::string counts;
    for (size_t i = 0; i < boxes.size(); i++) {
        const Box& bx = boxes[i];
        const Rect b = scaled(rects[i], bx.st->scale);
        if (box_input(i, b, hovered)) act = activation(bx);
        const bool failed = bx.entry >= 0 && bx.st->failed;
        if (failed) draw_empty(dl, bx, b, rects[i]);   /* the failed build: the game's name, no logo */
        else if (bx.entry >= 0 && bx.st->picture.tex) draw_picture(dl, bx, b);
        else if (bx.entry >= 0) draw_fill(dl, bx, b);   /* the logo names an ingested game */
        else draw_empty(dl, bx, b, rects[i]);
        const int v0 = dl->VtxBuffer.Size;
        draw_frame(dl, bx, b, std::clamp((bx.st->scale - 1.0f) / (kGrow - 1.0f), 0.0f, 1.0f));
        counts += " " + bx.st->game + " frame " + std::to_string(dl->VtxBuffer.Size - v0) + " vertices" +
                  (failed ? ", failed build" : bx.entry >= 0 ? (bx.st->logo.tex ? ", logo" : ", no logo yet") : ", empty");
        if (!failed && bx.entry >= 0 && bx.st->logo.tex) draw_logo(dl, bx, b);
    }
    if (opening)
        OPL_INFO_F("launcher", "game page: %zu boxes at %.0fx%.0f, box %d focused:%s", boxes.size(), d.x, d.y,
                   focus, counts.c_str());
    return act;
}

void launcher_page_lines_draw(const char* const* entry_games, int count, float scale) {
    const std::vector<const openpete_launcher_game*> games = page_games();
    if (games.empty()) return;
    std::vector<const char*> box_games;
    for (const openpete_launcher_game* g : games) box_games.push_back(g->game_id);
    const PageLines lines = page_lines(box_games, entry_games, count, launcher_panel_shown());
    const ImVec2 d = ImGui::GetIO().DisplaySize;
    const std::vector<Rect> rects = box_rects(games.size(), d.x, d.y);
    const float fill = kChipAlpha * launcher_sky_cover();
    launcher_push_body_font(kCornerPx * scale);
    for (size_t i = 0; i < lines.ids.size(); i++) {
        if (!lines.ids[i]) continue;
        const std::string text = launcher_chip_case(lines.ids[i]);
        const ImVec2 sz = ImGui::CalcTextSize(text.c_str());
        const Rect t = id_text(rects[i], sz.x, sz.y, scale);
        const std::string id = std::string("box.") + box_games[i] + ".id";
        launcher_chip_text(id.c_str(), {t.x, t.y}, {kIdPadX, kIdPadY}, scale, IM_COL32_WHITE, text.c_str(), fill);
        if (!box_state(box_games[i]).unsupported) continue;
        /* Below the serial's chip, its padding clear of it. */
        const std::string mark = launcher_chip_case("Unsupported disc");
        const ImVec2 ms = ImGui::CalcTextSize(mark.c_str());
        const Rect r = rects[i];
        const float mx = r.x + (r.w - ms.x) * 0.5f, my = t.y + t.h + 2.0f * kIdPadY * scale + kIdGap * scale;
        const std::string mid = std::string("box.") + box_games[i] + ".unsupported";
        launcher_chip_text(mid.c_str(), {mx, my}, {kIdPadX, kIdPadY}, scale, IM_COL32(240, 138, 30, 255), mark.c_str(),
                           fill);
    }
    if (lines.hint) {
        const std::string text = launcher_chip_case(lines.hint);
        const ImVec2 sz = ImGui::CalcTextSize(text.c_str());
        const Rect t = hint_text(d.x, d.y, sz.x, sz.y, scale);
        launcher_chip_text("page.hint", {t.x, t.y}, {kCornerPadX, kCornerPadY}, scale, IM_COL32_WHITE, text.c_str(),
                           fill);
    }
    ImGui::PopFont();
}

int launcher_page_box_count(const char* const* entry_games, int count) {
    return (int)page_boxes(entry_games, count).size();
}

int launcher_page_activation(const char* const* entry_games, int count, int box) {
    const std::vector<Box> boxes = page_boxes(entry_games, count);
    return box >= 0 && (size_t)box < boxes.size() ? activation(boxes[(size_t)box]) : LAUNCHER_PAGE_BROWSE;
}

const char* launcher_page_box_game(int box) {
    const std::vector<const openpete_launcher_game*> games = page_games();
    return box >= 0 && (size_t)box < games.size() ? games[(size_t)box]->game_id : "";
}

int launcher_page_box_of_game(const char* const* entry_games, int count, const char* game) {
    const std::vector<Box> boxes = page_boxes(entry_games, count);
    for (size_t i = 0; game && i < boxes.size(); i++)
        if (!strcmp(boxes[i].g->game_id, game)) return (int)i;
    return -1;
}

bool launcher_page_owns_box(const char* const* entry_games, int i) {
    for (const Box& bx : page_boxes(entry_games, i + 1))
        if (bx.entry == i) return true;
    return false;
}

void launcher_page_set_failed(const char* game, bool failed) {
    for (BoxState& b : s_boxes) {
        if (game && b.game != game) continue;
        if (b.failed != failed)
            OPL_INFO_F("launcher", "game page: box of %s %s", b.game.c_str(),
                       failed ? "shows the failed-build drawing" : "no longer shows the failed-build drawing");
        b.failed = failed;
    }
    if (game && failed) {
        bool known = false;
        for (const BoxState& b : s_boxes) known = known || b.game == game;
        if (!known) {
            box_state(game).failed = true;
            OPL_INFO_F("launcher", "game page: box of %s shows the failed-build drawing", game);
        }
    }
}

void launcher_page_set_unsupported(const char* game, bool unsupported) {
    if (!game) return;
    BoxState& b = box_state(game);
    if (b.unsupported != unsupported)
        OPL_INFO_F("launcher", "game page: box of %s %s", game,
                   unsupported ? "marks an unsupported disc" : "no longer marks an unsupported disc");
    b.unsupported = unsupported;
}

float launcher_page_row_bottom(float w, float h) {
    const size_t n = page_games().size();
    if (n == 0) return 0.45f * h;
    const Rect g = scaled(box_rects(n, w, h)[0], kGrow);
    return g.y + g.h;
}

void launcher_page_closed(void) {
    s_last = 0;
    s_animating = false;
}

bool launcher_page_animating(void) { return s_animating; }

void launcher_page_set_picture(const char* game, const unsigned char* rgba, int w, int h) {
    if (!game || !rgba || w <= 0 || h <= 0) return;
    image_set(box_state(game).picture, rgba, w, h);
}

void launcher_page_release(void) {
    for (BoxState& b : s_boxes)
        for (Image* im : {&b.logo, &b.picture}) {
            if (!im->tex) continue;
            im->tex->DestroyPixels();
            im->tex->UnusedFrames = 1 << 20;
            im->tex->SetStatus(ImTextureStatus_WantDestroy);
            *im = {};
        }
}
