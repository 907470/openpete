/* tests/unit/launcher_page_lines_test.cpp - the game page's lines below its
 * boxes, without a window.
 *
 * The lines' text: page_lines over the three registered games with 0, 1, 2
 * and 3 of them in the library, with the message panel raised and not. Their
 * fit: the real body font (launcher_font.cpp, an imgui context with no
 * backend) measures the serials, the disc hint and the widest corner chips,
 * and at each supported window, with 1, 2 and 3 boxes, every chip stays
 * inside the window; a serial's chip lies below the lowest frame of the grown
 * box, within its box's width, and above the corner chips' row; the hint's
 * chip is clear of both corner chips. Each row's rects are printed. */
#include "launcher_internal.h"
#include "launcher_page_layout.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace launcher_layout;

namespace {

int g_bad = 0;

/** @brief Count a failed check and print it.
 *
 * @param ok The check's result.
 * @param what What was checked. */
void check(bool ok, const std::string& what) {
    if (ok) return;
    printf("FAIL %s\n", what.c_str());
    g_bad++;
}

/** @brief Report whether two rects overlap.
 *
 * @param a One rect.
 * @param b The other.
 * @return true when they share area. */
bool overlap(const Rect& a, const Rect& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

/** @brief Report whether a rect lies inside the window.
 *
 * @param r The rect.
 * @param W The window width.
 * @param H The window height.
 * @return true when it does. */
bool inside(const Rect& r, float W, float H) { return r.x >= 0 && r.y >= 0 && r.x + r.w <= W && r.y + r.h <= H; }

/** @brief Measure a line of the body font at the corner chips' size.
 *
 * @param text The line.
 * @param scale Window pixels per layout pixel.
 * @return Its size. */
ImVec2 measure(const char* text, float scale) {
    launcher_push_body_font(kCornerPx * scale);
    const ImVec2 sz = ImGui::CalcTextSize(text);
    ImGui::PopFont();
    return sz;
}

/** @brief Check the lines' text for every subset of the three games in the library, the panel raised and not. */
void check_text() {
    const std::vector<const char*> games = {"spyro-1", "spyro-2", "spyro-3"};
    const char* const serials[3] = {"SCUS-94228", "SCUS-94425", "SCUS-94467"};
    for (unsigned held = 0; held < 8; held++) {
        std::vector<const char*> entries;
        for (unsigned g = 0; g < 3; g++)
            if (held >> g & 1) entries.push_back(games[g]);
        const int n = (int)entries.size();
        const PageLines on = page_lines(games, entries.data(), n, false);
        const PageLines off = page_lines(games, entries.data(), n, true);
        const char* want = n < 3 ? "Drag and drop or select your own copy of the game disc" : nullptr;
        check((!on.hint && !want) || (on.hint && want && !strcmp(on.hint, want)), "hint for held mask " + std::to_string(held));
        check(!off.hint, "the panel hides the hint");
        for (unsigned g = 0; g < 3; g++) {
            check(on.ids[g] && !strcmp(on.ids[g], serials[g]), std::string("serial of ") + games[g]);
            check(!off.ids[g], "the panel hides the serials");
        }
        printf("%d ingested (mask %u): ids %s %s %s; hint %s\n", n, held, on.ids[0], on.ids[1], on.ids[2],
               on.hint ? on.hint : "(none)");
    }
    const std::vector<const char*> empty = {"spyro-1"};
    check(page_lines(empty, nullptr, 0, false).hint != nullptr, "an empty library shows the hint");
    check(page_lines({"spyro-9"}, nullptr, 0, false).ids[0] == nullptr, "an unregistered game has no line");
}

/** @brief Check the chips of one window with one box count.
 *
 * @param W The window width.
 * @param H The window height.
 * @param n The box count.
 * @param print true to print the row. */
void check_fit(float W, float H, size_t n, bool print) {
    const float scale = std::min(W / 640.0f, H / 448.0f);
    const std::string at = std::to_string((int)W) + "x" + std::to_string((int)H) + " " + std::to_string(n) + " boxes: ";
    const ImVec2 hs = measure(kDiscHint, scale);
    const Rect hint = padded(hint_text(W, H, hs.x, hs.y, scale), kCornerPadX, kCornerPadY, scale);
    /* The corner chips (launcher_chips.cpp place): the widest shader line with its spinner, the longest level name. */
    const ImVec2 ls = measure("Compiling shaders 100%", scale), rs = measure("Mushroom Speedway", scale);
    const float lw = ls.x + (2.0f * kCornerPadX + kCornerPx + kSpinGap) * scale, rw = rs.x + 2.0f * kCornerPadX * scale;
    const float ch = ls.y + 2.0f * kCornerPadY * scale, cy = H - kCornerMargin * scale - ch;
    const Rect left = {kCornerMargin * scale, cy, lw, ch}, right = {W - kCornerMargin * scale - rw, cy, rw, ch};
    check(inside(hint, W, H), at + "the hint's chip leaves the window");
    check(!overlap(hint, left), at + "the hint's chip meets the shader chip");
    check(!overlap(hint, right), at + "the hint's chip meets the level chip");
    const std::vector<Rect> rects = box_rects(n, W, H);
    Rect id_chip = {};
    for (size_t i = 0; i < n; i++) {
        const ImVec2 sz = measure("SCUS-94228", scale);
        id_chip = padded(id_text(rects[i], sz.x, sz.y, scale), kIdPadX, kIdPadY, scale);
        check(inside(id_chip, W, H), at + "a serial's chip leaves the window");
        check(id_chip.y >= frame_bottom(rects[i]), at + "a serial's chip meets its grown frame");
        check(id_chip.w <= rects[i].w, at + "a serial's chip is wider than its box");
        check(id_chip.y + id_chip.h <= cy, at + "a serial's chip reaches the corner chips' row");
    }
    if (!print) return;
    printf("%s scale %.3f, grown frame bottom %.1f, id chip y %.1f-%.1f w %.1f (box w %.1f), row y %.1f-%.1f, "
           "gap %.1f; hint chip x %.1f-%.1f, shader chip right %.1f, level chip left %.1f\n",
           at.c_str(), (double)scale, (double)frame_bottom(rects[0]), (double)id_chip.y,
           (double)(id_chip.y + id_chip.h), (double)id_chip.w, (double)rects[0].w, (double)cy, (double)(cy + ch),
           (double)(cy - id_chip.y - id_chip.h), (double)hint.x, (double)(hint.x + hint.w), (double)(left.x + left.w),
           (double)right.x);
}

}  // namespace

/** @brief Run the text and fit checks.
 *
 * @return 0 when every check holds. */
int main() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 800);
    io.DeltaTime = 1.0f / 60.0f;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    check(launcher_body_font() != nullptr, "the body font loads");
    ImGui::NewFrame();
    check_text();
    static const int sizes[][2] = {{1024, 640}, {1024, 768}, {1280, 800}, {1361, 851}, {1920, 1080},
                                   {2560, 1080}, {2560, 1440}, {3840, 2160}};
    for (const auto& s : sizes)
        for (size_t n = 1; n <= 3; n++) check_fit((float)s[0], (float)s[1], n, n == 3);
    ImGui::EndFrame();
    ImGui::DestroyContext();
    printf("launcher_page_lines_test: %s\n", g_bad ? "FAIL" : "PASS");
    return g_bad ? 1 : 0;
}
