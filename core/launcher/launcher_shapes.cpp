// core/launcher/launcher_shapes.cpp - the launcher's boxes on the present's
// shape pass (contract in launcher_shapes.h).
#include "launcher_shapes.h"

#include "launcher_internal.h"

#include "imgui.h"

#include "openpete_native_render.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

/* The edge's coverage ramp, framebuffer pixels. */
constexpr float kEdgeAA = 1.5f;
/* The drop shadow under panels, chips and the bar: black at this alpha,
 * this many layout px down, blurred over this many layout px. */
constexpr float kShadowAlpha = 0.35f;
constexpr float kShadowDy = 2.0f;
constexpr float kShadowBlur = 6.0f;
/* Spyro 3's box colour. */
constexpr float kBox = 8.0f / 255.0f;

/** @brief Write an ImU32 colour as straight RGBA floats in 0..1.
 *
 * @param c The colour.
 * @param out Receives R, G, B, A. */
void rgba(ImU32 c, float out[4]) {
    const ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    out[0] = v.x; out[1] = v.y; out[2] = v.z; out[3] = v.w;
}

/** @brief Report whether shapes draw their drop shadow: false when the harness variable OPENPETE_LAUNCHER_SHADOW is 0 (the corner profile check reads the edge ramp alone), read once.
 *
 * @return true unless the shadow is turned off. */
bool shadow_on() {
    static const bool on = [] {
        const char* e = getenv("OPENPETE_LAUNCHER_SHADOW");
        return !(e && !strcmp(e, "0"));
    }();
    return on;
}

/* The chips' letter case: false draws each chip's text as written (step
 * names in sentence case, level names as titled), true in upper case. */
constexpr bool kChipUpperCase = false;

/** @brief Start a shape over a window-coordinate rect: its rect in framebuffer pixels, its radii scaled to fit, the coverage ramp, and the drop shadow at an alpha.
 *
 * @param s Receives the shape, zeroed first.
 * @param kind Its kind.
 * @param a The rect's top left, window coordinates.
 * @param b Its bottom right.
 * @param rx The horizontal corner radius, window coordinates.
 * @param ry The vertical one.
 * @param scale Window pixels per layout pixel.
 * @param shadow The shadow's alpha; 0 for none. */
void shape_init(openpete_launcher_shape* s, openpete_launcher_shape_kind kind, ImVec2 a, ImVec2 b, float rx,
                float ry, float scale, float shadow) {
    *s = {};
    const ImVec2 fb = ImGui::GetIO().DisplayFramebufferScale;
    const float k = std::min({1.0f, (b.x - a.x) * 0.5f / std::max(rx, 1e-3f), (b.y - a.y) * 0.5f / std::max(ry, 1e-3f)});
    s->kind = kind;
    s->rect[0] = a.x * fb.x; s->rect[1] = a.y * fb.y;
    s->rect[2] = b.x * fb.x; s->rect[3] = b.y * fb.y;
    s->rx = std::max(0.0f, rx * k * fb.x);
    s->ry = std::max(0.0f, ry * k * fb.y);
    s->aa = kEdgeAA;
    s->shadow_a = shadow_on() ? shadow : 0.0f;
    s->shadow_dy = kShadowDy * scale * fb.y;
    s->shadow_blur = kShadowBlur * scale * fb.y;
}

}  // namespace

void launcher_shape_panel(const char* id, ImVec2 a, ImVec2 b, float scale, float alpha) {
    if (alpha <= 0.0f || b.x <= a.x || b.y <= a.y) return;
    openpete_launcher_shape s;
    shape_init(&s, OPENPETE_LAUNCHER_SHAPE_FILL, a, b, kPanelRx * scale, kPanelRy * scale, scale,
               kShadowAlpha * std::min(1.0f, alpha / kChipAlpha));
    s.col[0] = s.col[1] = s.col[2] = kBox;
    s.col[3] = alpha;
    (void)openpete_native_launcher_shape_add(&s);
    launcher_manifest_item(id, "panel", a.x, a.y, b.x, b.y);
}

void launcher_shape_bar(const char* id, ImVec2 a, ImVec2 b, float scale, float frac, ImU32 track, ImU32 fill,
                        ImU32 border, float border_px) {
    if (b.x <= a.x || b.y <= a.y) return;
    frac = std::clamp(frac, 0.0f, 1.0f);
    openpete_launcher_shape s;
    shape_init(&s, OPENPETE_LAUNCHER_SHAPE_BAR, a, b, kPanelRx * scale, kPanelRy * scale, scale, kShadowAlpha);
    rgba(track, s.track);
    rgba(fill, s.fill);
    rgba(border, s.border);
    s.fill_x = s.rect[0] + (s.rect[2] - s.rect[0]) * frac;
    s.border_w = border_px * scale * ImGui::GetIO().DisplayFramebufferScale.y;
    (void)openpete_native_launcher_shape_add(&s);
    launcher_manifest_item(id, "bar", a.x, a.y, b.x, b.y);
}

void launcher_chip_text(const char* id, ImVec2 pos, ImVec2 pad, float scale, ImU32 col, const char* text) {
    const ImVec2 sz = ImGui::CalcTextSize(text);
    const ImVec2 a(pos.x - pad.x * scale, pos.y - pad.y * scale);
    const ImVec2 b(pos.x + sz.x + pad.x * scale, pos.y + sz.y + pad.y * scale);
    launcher_shape_panel((std::string(id) + ".panel").c_str(), a, b, scale, kChipAlpha);
    const float x0 = std::trunc(pos.x), y0 = std::trunc(pos.y);
    launcher_manifest_item(id, "text", x0, y0, x0 + sz.x, y0 + sz.y);
    /* GetColorU32 applies the style's alpha, which the text-hiding harness
     * variable sets to 0. */
    ImGui::GetWindowDrawList()->AddText(pos, ImGui::GetColorU32(col), text);
}

std::string launcher_chip_case(const char* text) {
    std::string t = text ? text : "";
    if (kChipUpperCase)
        for (char& c : t)
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return t;
}
