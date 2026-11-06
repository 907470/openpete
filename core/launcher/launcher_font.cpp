// core/launcher/launcher_font.cpp - the launcher's body font: Fredoka Medium,
// embedded in the executable (fonts/fredoka_medium.inc), added to the imgui
// context the launcher creates and the game's dev overlay adopts. The font
// is pushed only in the launcher's draws; io.FontDefault stays imgui's
// default font, ProggyClean, which the pixel title and the dev overlay draw
// with (contract in launcher_internal.h).
#include "launcher_internal.h"

#include "imgui.h"

#include "openpete_log.h"

#include <cstdio>

namespace {

#include "fonts/fredoka_medium.inc"

ImFont* s_body = nullptr;
bool s_tried = false;   /* one attempt per process: a failed add is not retried */

/* Fredoka's capitals are 700/1210 of its line, ProggyClean's 8/13: the body
 * font is pushed this much larger than a layout size so its capitals stand
 * as tall as ProggyClean's at that size. */
constexpr float kBodyScale = (8.0f / 13.0f) / (700.0f / 1210.0f);

}  // namespace

ImFont* launcher_body_font(void) {
    if (s_body || s_tried || !ImGui::GetCurrentContext()) return s_body;
    s_tried = true;
    ImGuiIO& io = ImGui::GetIO();
    /* The first font added to an atlas is io.FontDefault's fallback, and an
     * empty atlas gets imgui's default font at the first frame; the default
     * is added and named first so the body font never becomes it. */
    if (io.Fonts->Fonts.Size == 0) io.FontDefault = io.Fonts->AddFontDefault();
    else if (!io.FontDefault) io.FontDefault = io.Fonts->Fonts[0];
    ImFontConfig cfg;
    snprintf(cfg.Name, sizeof cfg.Name, "%s", "Fredoka Medium");
    s_body = io.Fonts->AddFontFromMemoryCompressedBase85TTF(fredoka_medium_compressed_data_base85,
                                                            14.0f, &cfg);
    OPL_INFO_F("launcher", "body font %s (embedded, %zu bytes base85); default font %s",
               s_body ? s_body->GetDebugName() : "(not loaded: the default font draws the body text)",
               sizeof fredoka_medium_compressed_data_base85 - 1,
               io.FontDefault ? io.FontDefault->GetDebugName() : "(none)");
    return s_body;
}

void launcher_push_body_font(float px) {
    if (ImFont* body = launcher_body_font()) ImGui::PushFont(body, px * kBodyScale);
    else ImGui::PushFont(nullptr, px);
}
