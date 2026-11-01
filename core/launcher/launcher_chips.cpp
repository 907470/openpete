// core/launcher/launcher_chips.cpp - the launcher's corner chips (contract in
// launcher_chips.h).
#include "launcher_chips.h"

#include "launcher_internal.h"
#include "launcher_shapes.h"
#include "launcher_sky.h"

#include "imgui.h"
#include "imgui_internal.h"   /* ImGuiButtonFlags_PressedOnClick */

#include "openpete_log.h"
#include "openpete_native_render.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

/* The corner chips' metrics, layout px: the text size (as ProggyClean,
 * launcher_push_body_font), the padding around the line, the margin to the
 * window's edges, and the spinner's gap before the text and stroke. */
constexpr float kChipPx = 10.0f;
constexpr float kPadX = 8.0f, kPadY = 6.0f;
constexpr float kMargin = 12.0f;
constexpr float kSpinGap = 6.0f;
constexpr float kSpinStroke = 1.5f;
/* The spinner's arc and its turn: 270 degrees, once per second. */
constexpr float kSpinSweep = 1.5f * 3.14159265358979f;
constexpr double kSpinPeriodS = 1.0;

/* The window-coordinate boxes of one corner chip. */
struct Corner {
    ImVec2 a, b;   /* the chip */
    ImVec2 text;   /* the text's origin */
};

/** @brief Place one corner chip at a bottom corner of the window for a line at the pushed font.
 *
 * @param text The line.
 * @param scale Window pixels per layout pixel.
 * @param left true for the bottom-left corner, false for the bottom right.
 * @param lead The width before the text inside the padding (the spinner and its gap), window coordinates.
 * @return The chip's box and the text's origin. */
Corner place(const char* text, float scale, bool left, float lead) {
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float w = ts.x + 2.0f * kPadX * scale + lead, h = ts.y + 2.0f * kPadY * scale;
    const float x0 = left ? kMargin * scale : disp.x - kMargin * scale - w;
    const float y0 = disp.y - kMargin * scale - h;
    return {ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), ImVec2(x0 + kPadX * scale + lead, y0 + kPadY * scale)};
}

/** @brief Add a corner chip's box, the text scrim and the text, and record the text in the element manifest.
 *
 * @param id The text's manifest id; the box records as <id>.panel, the scrim as <id>.scrim.
 * @param c The placed chip.
 * @param scale Window pixels per layout pixel.
 * @param text The line. */
void chip_body(const char* id, const Corner& c, float scale, const char* text) {
    launcher_shape_panel((std::string(id) + ".panel").c_str(), c.a, c.b, scale, kChipAlpha);
    launcher_shape_scrim(id, nullptr, ImGui::GetFontSize(), c.text, text, scale);
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float x0 = std::trunc(c.text.x), y0 = std::trunc(c.text.y);
    launcher_manifest_item(id, "text", x0, y0, x0 + ts.x, y0 + ts.y);
    /* GetColorU32 applies the style's alpha, which the text-hiding harness
     * variable sets to 0. */
    ImGui::GetWindowDrawList()->AddText(c.text, ImGui::GetColorU32(IM_COL32_WHITE), text);
}

/** @brief Draw the spinner: a 270 degree arc at the sky clock's turn, round caps, centred in a square of the text height before the text.
 *
 * @param c The placed chip.
 * @param scale Window pixels per layout pixel. */
void spinner(const Corner& c, float scale) {
    const float p = kChipPx * scale, stroke = kSpinStroke * scale;
    const ImVec2 o(c.a.x + kPadX * scale + 0.5f * p, c.a.y + kPadY * scale + 0.5f * p);
    const float r = 0.5f * (p - stroke);
    const double turn = std::fmod(launcher_sky_clock() / kSpinPeriodS, 1.0);
    const float a0 = (float)(turn * 2.0 * 3.14159265358979), a1 = a0 + kSpinSweep;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = ImGui::GetColorU32(IM_COL32_WHITE);
    dl->PathArcTo(o, r, a0, a1, 32);
    dl->PathStroke(col, stroke);
    /* Round caps: a disc of the stroke's width at each end. */
    dl->AddCircleFilled(ImVec2(o.x + r * std::cos(a0), o.y + r * std::sin(a0)), 0.5f * stroke, col, 12);
    dl->AddCircleFilled(ImVec2(o.x + r * std::cos(a1), o.y + r * std::sin(a1)), 0.5f * stroke, col, 12);
    launcher_manifest_item("status.shaders.spinner", "arc", o.x - 0.5f * p, o.y - 0.5f * p, o.x + 0.5f * p,
                           o.y + 0.5f * p);
}

/** @brief Draw the shader compile's chip while the launcher's precompile runs: "Compiling shaders N%", N the share of its create infos returned, rounded down, with the spinner.
 *
 * @param scale Window pixels per layout pixel. */
void shader_chip(float scale) {
    unsigned done = 0, total = 0;
    if (!openpete_native_launcher_precompile_counts(&done, &total) || total == 0) return;
    char line[64];
    snprintf(line, sizeof line, "Compiling shaders %u%%", (unsigned)((unsigned long long)done * 100ull / total));
    const std::string text = launcher_chip_case(line);
    const Corner c = place(text.c_str(), scale, true, (kChipPx + kSpinGap) * scale);
    chip_body("status.shaders", c, scale, text.c_str());
    spinner(c, scale);
}

/** @brief Return the display name of a level of a game through the game's launcher entry, or the level id when no entry names it.
 *
 * @param game The game id.
 * @param level The level id.
 * @return The name. */
const char* level_display(const char* game, const char* level) {
    const openpete_launcher_game* d = launcher_game();
    const openpete_launcher_game* g = nullptr;
    if (d && d->game_id && game && !strcmp(d->game_id, game)) g = d;
    for (int i = 0; d && !g && i < d->game_count; i++)
        if (d->games[i] && d->games[i]->game_id && game && !strcmp(d->games[i]->game_id, game)) g = d->games[i];
    const char* name = g && g->level_name ? g->level_name(g, level) : nullptr;
    return name ? name : level;
}

/** @brief Draw the level chip while a dome shows: the shown level's name, a button whose press shows the next dome, with the hand cursor while hovered.
 *
 * Logs each change of the name with the sky clock, each start of a hover
 * and each press.
 * @param scale Window pixels per layout pixel. */
void level_chip(float scale) {
    static std::string logged;
    const char *game = nullptr, *level = nullptr;
    if (!launcher_sky_shown(&game, &level) || !level || !*level) {
        logged.clear();
        return;
    }
    const std::string text = launcher_chip_case(level_display(game, level));
    if (text != logged) {
        logged = text;
        OPL_INFO_F("launcher", "level chip: %s (%s/%s) at sky clock %.3f s", text.c_str(), game, level,
                   launcher_sky_clock());
    }
    const Corner c = place(text.c_str(), scale, false, 0.0f);
    chip_body("status.skybox", c, scale, text.c_str());
    ImGui::SetCursorScreenPos(c.a);
    const bool pressed = ImGui::InvisibleButton("##status.skybox", ImVec2(c.b.x - c.a.x, c.b.y - c.a.y),
                                                (ImGuiButtonFlags)ImGuiButtonFlags_PressedOnClick);
    static bool was_hovered = false;
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (hovered && !was_hovered) OPL_INFO_F("launcher", "%s", "level chip: hovered, the hand cursor shows");
    was_hovered = hovered;
    if (pressed) {
        OPL_INFO_F("launcher", "level chip: pressed at sky clock %.3f s", launcher_sky_clock());
        (void)launcher_sky_next();
    }
}

}  // namespace

void launcher_corner_chips_draw(float scale, bool shader_step) {
    launcher_push_body_font(kChipPx * scale);
    if (!shader_step) shader_chip(scale);
    level_chip(scale);
    ImGui::PopFont();
}
