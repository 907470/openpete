// core/launcher/launcher_panel.cpp - the game page's message panel: one
// message at a time, centred below the boxes, in Spyro 3's dialogue box
// (launcher_shape_panel) and the body font, with the error's id and URL on
// its last line when it has one, a link to the error's troubleshooting entry
// (contract in launcher_internal.h).
//
// The panel's fill follows the sky: it fades in with the first dome and is
// not drawn over the flat background, where the text draws alone.
#include "launcher_internal.h"
#include "launcher_shapes.h"
#include "launcher_sky.h"

#include "imgui.h"
#include "imgui_internal.h"   /* ImGuiButtonFlags_PressedOnClick */

#include <SDL3/SDL.h>

#include "openpete_error_ids.h"
#include "openpete_log.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

/* Layout px: the message's text size (as ProggyClean, launcher_push_body_font)
 * and the reference line's, the padding inside the box, the gap between
 * lines, the gap below the box row, the bottom margin; the panel's widest,
 * of the window width. */
constexpr float kTextPx = 12.0f;
constexpr float kRefPx = 8.0f;
constexpr float kPadX = 18.0f, kPadY = 12.0f;
constexpr float kLineGap = 4.0f;
constexpr float kBelowRow = 14.0f;
constexpr float kMargin = 12.0f;
constexpr float kMaxW = 0.86f;
constexpr ImU32 kTextCol = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kRefCol = IM_COL32(168, 152, 201, 255);

struct Panel {
    bool shown = false;
    bool quits = false;        /* the dismissal quits the launcher */
    int err = -1;              /* the openpete_error_t of the reference line, -1 for none */
    std::string text;          /* lines separated by '\n' */
    bool drawn = false;        /* the raised message was drawn once */
    bool fill_drawn = false;   /* its fill was drawn once */
    bool full = false;         /* the last draw's dome cover was 1 */
    bool link_hovered = false; /* the pointer was over the reference line in the last draw */
};
Panel s_panel;

/** @brief Split text into its lines at each newline character.
 *
 * @param text The text.
 * @return The lines, at least one. */
std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> out;
    size_t at = 0;
    for (size_t nl; (nl = text.find('\n', at)) != std::string::npos; at = nl + 1) out.push_back(text.substr(at, nl - at));
    out.push_back(text.substr(at));
    return out;
}

/* One line of the panel as laid out: its text, size and colour. */
struct Line {
    std::string text;
    float px;
    ImU32 col;
    ImVec2 sz;
};

/** @brief Measure a line wrapped to a width at a body font size.
 *
 * @param l The line; receives its size.
 * @param wrap The wrap width in window pixels. */
void measure(Line& l, float wrap) {
    launcher_push_body_font(l.px);
    l.sz = ImGui::CalcTextSize(l.text.c_str(), nullptr, false, wrap);
    ImGui::PopFont();
}

/** @brief Open a URL in the platform's browser, or only log it under the harness variable OPENPETE_LAUNCHER_OPEN_URL_STUB=1.
 *
 * SDL_OpenURL hands the URL to ShellExecuteW on Windows, to the desktop
 * portal's OpenURI over D-Bus or else xdg-open on Linux, and to
 * NSWorkspace on macOS. A failure is logged.
 * @param url The URL. */
void open_url(const char* url) {
    const char* stub = getenv("OPENPETE_LAUNCHER_OPEN_URL_STUB");
    if (stub && *stub && *stub != '0') {
        OPL_INFO_F("launcher", "error panel link: opened %s (stubbed: nothing opened)", url);
        return;
    }
    if (SDL_OpenURL(url)) OPL_INFO_F("launcher", "error panel link: opened %s", url);
    else OPL_WARN_F("launcher", "error panel link: could not open %s: %s", url, SDL_GetError());
}

/** @brief Make the reference line a link: an invisible button over its rect, the hand cursor and an underline while hovered, the error's URL opened on a press.
 *
 * Logs each start of a hover.
 * @param l The line as drawn.
 * @param x Its left edge in window pixels.
 * @param y Its top edge.
 * @param scale Window pixels per layout pixel.
 * @return true when the line was pressed this frame. */
bool ref_link(const Line& l, float x, float y, float scale) {
    const openpete_error_t e = (openpete_error_t)s_panel.err;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    const bool pressed = ImGui::InvisibleButton("##panel.error.ref", ImVec2(std::max(1.0f, l.sz.x), std::max(1.0f, l.sz.y)),
                                                (ImGuiButtonFlags)ImGuiButtonFlags_PressedOnClick);
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const float u = std::max(1.0f, std::round(0.5f * scale));
        ImGui::GetWindowDrawList()->AddLine(ImVec2(x, y + l.sz.y), ImVec2(x + l.sz.x, y + l.sz.y), l.col, u);
    }
    if (hovered && !s_panel.link_hovered)
        OPL_INFO_F("launcher", "error panel link: hovered, the hand cursor shows (%s)", openpete_error_id(e));
    s_panel.link_hovered = hovered;
    if (pressed) {
        OPL_INFO_F("launcher", "error panel link: pressed (%s)", openpete_error_id(e));
        open_url(openpete_error_url(e));
    }
    return pressed;
}

}  // namespace

void launcher_panel_raise(int err, const char* text, bool quits) {
    s_panel = {};
    s_panel.shown = true;
    s_panel.quits = quits;
    s_panel.err = err;
    s_panel.text = text ? text : "";
    std::string flat = s_panel.text;
    std::replace(flat.begin(), flat.end(), '\n', ' ');
    OPL_INFO_F("launcher", "error panel raised%s: %s%s%s", quits ? " (its dismissal quits)" : "", flat.c_str(),
               err >= 0 ? " " : "", err >= 0 ? openpete_error_ref((openpete_error_t)err) : "");
}

bool launcher_panel_shown(void) { return s_panel.shown; }

bool launcher_panel_quits(void) { return s_panel.shown && s_panel.quits; }

bool launcher_panel_full(void) { return s_panel.shown && s_panel.full; }

bool launcher_panel_open_ref(void) {
    if (!s_panel.shown || s_panel.err < 0) return false;
    const openpete_error_t e = (openpete_error_t)s_panel.err;
    OPL_INFO_F("launcher", "error panel link: activated (%s)", openpete_error_id(e));
    open_url(openpete_error_url(e));
    return true;
}

void launcher_panel_dismiss(const char* why) {
    if (!s_panel.shown) return;
    OPL_INFO_F("launcher", "error panel dismissed (%s)%s", why, s_panel.quits ? "; the launcher quits" : "");
    s_panel.shown = false;
}

bool launcher_panel_draw(float scale, float top) {
    if (!s_panel.shown) return false;
    const ImVec2 d = ImGui::GetIO().DisplaySize;
    const float wrap = d.x * kMaxW - 2.0f * kPadX * scale;
    std::vector<Line> lines;
    for (const std::string& t : lines_of(s_panel.text)) lines.push_back({t, kTextPx * scale, kTextCol, {}});
    if (s_panel.err >= 0) {
        const openpete_error_t e = (openpete_error_t)s_panel.err;
        lines.push_back({std::string(openpete_error_id(e)) + "   " + openpete_error_url(e), kRefPx * scale, kRefCol, {}});
    }
    float w = 0.0f, h = 0.0f;
    for (Line& l : lines) {
        measure(l, wrap);
        w = std::max(w, l.sz.x);
        h += l.sz.y;
    }
    h += kLineGap * scale * (float)(lines.size() - 1);
    const float bw = w + 2.0f * kPadX * scale, bh = h + 2.0f * kPadY * scale;
    const float x0 = (d.x - bw) * 0.5f;
    const float y0 = std::max(0.0f, std::min(top + kBelowRow * scale, d.y - kMargin * scale - bh));
    const float cover = launcher_sky_cover();
    const float fill = kChipAlpha * cover;
    s_panel.full = cover >= 1.0f;
    if (fill > 0.002f) launcher_shape_panel("panel.error", ImVec2(x0, y0), ImVec2(x0 + bw, y0 + bh), scale, fill);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y = y0 + kPadY * scale;
    bool link_pressed = false;
    for (size_t i = 0; i < lines.size(); i++) {
        const Line& l = lines[i];
        /* A line that fits draws centred; a wrapped one draws as a block at the left padding. */
        const float x = l.sz.x < wrap ? (d.x - l.sz.x) * 0.5f : x0 + kPadX * scale;
        launcher_push_body_font(l.px);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x, y), l.col, l.text.c_str(), nullptr, wrap);
        ImGui::PopFont();
        const bool ref = i + 1 == lines.size() && s_panel.err >= 0;
        const std::string id = ref ? std::string("panel.error.ref") : "panel.error.line." + std::to_string(i);
        launcher_manifest_item(id.c_str(), "text", x, y, x + l.sz.x, y + l.sz.y);
        if (ref) link_pressed = ref_link(l, x, y, scale);
        y += l.sz.y + kLineGap * scale;
    }
    if (!s_panel.drawn) {
        s_panel.drawn = true;
        OPL_INFO_F("launcher", "error panel drawn: %zu lines in %.0f,%.0f-%.0f,%.0f of %.0fx%.0f, dome cover %.2f, "
                   "fill %s", lines.size(), x0, y0, x0 + bw, y0 + bh, d.x, d.y, cover,
                   fill > 0.002f ? "drawn" : "not drawn (the flat background)");
    }
    if (fill > 0.002f && !s_panel.fill_drawn) {
        s_panel.fill_drawn = true;
        OPL_INFO_F("launcher", "error panel fill drawn at %.2f opacity (dome cover %.2f)", fill, cover);
    }
    return link_pressed;
}

void launcher_panel_build_failed(const char* game, const char* name, const char* step) {
    const char* log = openpete_log_file_path();
    const std::string text = std::string(name && *name ? name : game) + " could not be built from its disc (step: " +
                             (step && *step ? step : "start") + ").\nThe log file " +
                             (log && *log ? log : "openpete.log") +
                             " names the failure. Choose the box to try again.";
    if (launcher_screens()) launcher_page_set_failed(game, true);
    launcher_panel_raise(OP_E_MODULE, text.c_str(), false);
}
