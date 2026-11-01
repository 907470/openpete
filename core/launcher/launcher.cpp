// core/launcher/launcher.cpp - the launcher's window lifecycle, its screens
// and the task runner (contract in openpete_launcher.h).
//
// Three screens, drawn through imgui on the launcher's Vulkan device
// (openpete_native_launcher_present):
//   drop      - the library is empty and no disc was named: the game page
//               (launcher_page.cpp) with every box empty; a box, or a disc
//               dragged onto the window, chooses a disc.
//   selector  - library entries to pick from: the game page; an ingested
//               box picks its entry, an empty one chooses a disc.
//   pending   - the library scan runs: the title and a reading line, drawn
//               from the first frame until the scan returns.
//   progress  - the ingest copy, the first-boot module build and the mod
//               compile report a stage and a bar; after the hand-off the
//               renderer's pipeline precompile draws the same screen on the
//               device it adopted (openpete_launcher_draw_progress).
// Every screen draws in a 640x448 layout space scaled into the window. The
// harness variable OPENPETE_LAUNCHER_SHOT=<base> reads back the first frame
// of each screen, logs how many pixels it drew, and writes <base>.<screen>.png;
// the precompile's progress screen is the screen "precompile".
//
// The window is resizable and can be maximized; Alt+Enter and a double-click
// outside the screen's buttons and entries toggle borderless fullscreen.
// --launcher-fullscreen and --launcher-window set its first state. A toggle,
// resize or maximize the player makes here replaces the booted game's
// fullscreen and window settings at the hand-off (carry_window_state). The
// harness variable OPENPETE_LAUNCHER_INPUT=<ms>:<action>[,...] performs each
// action that long after the first launcher frame: altenter, browse (Space),
// dblclick and quit post the input events, maximize, restore and resize=WxH call the
// window operations a window manager answers, and key=, pad=, stick= and
// mouse= post key, gamepad and pointer events (scripted_event). OPENPETE_LAUNCHER_MANIFEST=<base>
// writes each screen's first frame's elements, by the reference layout's ids, to
// <base>.<screen>.json (launcher_manifest.cpp). The contrast check's harness
// variables: OPENPETE_LAUNCHER_CLEAR=r,g,b replaces the clear colour, and
// OPENPETE_LAUNCHER_TEXT_HIDE=1 draws imgui's output at alpha 0, so a shot
// holds the background and the shape pass alone.
#include "launcher_internal.h"
#include "launcher_shapes.h"
#include "launcher_sky.h"

#include "imgui.h"

#include <SDL3/SDL.h>

#include "openpete_devui.h"
#include "openpete_fatal.h"
#include "openpete_input_lock.h"
#include "openpete_log.h"
#include "openpete_native_render.h"
#include "openpete_time.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>

namespace {

/* Layout space of every screen; the splash this launcher replaced used the
 * same 640x448 (Spyro 1's game window), and the screens were authored in a
 * 640x360 band centred in it. */
constexpr float kLayoutW = 640.0f;
constexpr float kLayoutH = 448.0f;
constexpr float kBandY = (kLayoutH - 360.0f) / 2.0f;

constexpr uint8_t kBg[3] = {24, 16, 40};   /* deep purple */

enum class Screen { Progress, Drop, Select, Pending };

/* One screen's content for one frame, and what the player did in it. */
struct View {
    Screen screen = Screen::Progress;
    const char* name = nullptr;   /* the game's display name; NULL or "" for none */
    const char* stage = "";
    int done = 0, total = 0;
    const char* const* entry_games = nullptr;   /* the game page's library entries' games */
    int count = 0;
    int activated = LAUNCHER_PAGE_NONE;   /* the box the mouse activated this frame */
    int focus = 0;                        /* the game page's focused box */
    int hovered = -1;                     /* the box under the pointer this frame */
    bool quit_asked = false;              /* the east button's ask shows ("Press again to quit") */
};

struct State {
    const openpete_launcher_game* game = nullptr;
    const openpete_options_t* opts = nullptr;
    bool ui = false;           /* screens may show */
    bool failed = false;       /* the device could not be created */
    bool handed_off = false;
    SDL_Gamepad* pad = nullptr;
    char drop_path[4096] = {};
    bool scripted_taken = false;   /* OPENPETE_LAUNCHER_DROP was consumed */
    SDL_AtomicInt dialog_pending = {};
    SDL_AtomicInt dialog_got = {};
    unsigned shot_done = 0;    /* one bit per Screen already read back */
    /* Task runner mailbox: the worker writes stage/done/total under lock. */
    SDL_Mutex* lock = nullptr;
    bool task_active = false;
    bool task_done = false;
    bool have_progress = false;
    bool quit_pending = false;   /* a quit arrived while a wait was drawn */
    bool fail_deferred = false;  /* the device failed; its message waits for the next open */
    std::string fail_why;        /* the deferred message's reason */
    Uint32 wake_event = 0;       /* posted by the scan worker when it returns */
    SDL_AtomicInt scan_done = {};
    SDL_AtomicInt scan_wakeable = {};   /* the event queue runs: the worker may post */
    /* The window's state as the player left it (window_event), and the
     * state it opened in. A fullscreen toggle counts as touched; the size
     * counts when the maximized state or the windowed size at the hand-off
     * differs from the opening one. */
    bool fs_now = false, max_now = false;
    int win_w = 0, win_h = 0;    /* the windowed size last seen */
    bool max_open = false;
    int open_w = 0, open_h = 0;
    bool fs_touched = false;
    bool fs_pending = false;     /* a toggle's enter or leave event is due */
    bool item_hovered = false;   /* the last frame's mouse was over a button or entry */
    bool presented = false;      /* a launcher present on the launcher's device was drawn */
    bool title_held = false;     /* this present holds the cached 3D title back (prepare_title) */
    bool pointer_moved = false;  /* a pointer motion arrived since the selector last looked */
    bool pointer_placed = false; /* the window's first pointer motion arrived: it placed the pointer */
    SDL_ThreadID ui_tid = 0;
    char stage[128] = {};
    int done = 0, total = 0;
    char last_stage[128] = {};   /* the last stage any report named */
};
State s;

/* A launch-time job of one library entry: its module check (and rebuild)
 * on the job thread, its asset cache check (and extraction) on the ingest
 * thread, both under the one install of its game. */
struct Job {
    const openpete_launcher_game* g = nullptr;
    std::string entry, disc, sha;
    int rc = 0;
    int arc = 0;          /* the asset part's status (openpete_launcher_game.update_assets) */
    bool module = true;   /* the job checks the module (no current failed-build marker) */
    bool assets = false;  /* the asset part started */
    bool built = false;   /* the module was missing and a build ran */
    bool running = false; /* started and not yet reaped */
};

/* The job runner: a queue, the running job on its thread, the in-flight
 * count installs are refused under, and the running job's progress
 * mailbox (kept apart from a launcher task's). */
struct Jobs {
    std::deque<Job> queue;
    Job cur;
    SDL_Thread* th = nullptr;
    SDL_AtomicInt done = {};
    SDL_AtomicInt cancel = {};
    std::atomic<int> in_flight{0};
    char stage[128] = {};
    int sdone = 0, stotal = 0;
    bool have = false;
};
Jobs s_jobs;
/* Set on a thread while it runs a job, the job thread or the inline form. */
thread_local bool t_job = false;

/* The asset job (openpete_launcher_game.update_assets) of one game on the ingest
 * thread, which binds the `ingest` role: started by a launch-time job, an
 * ingest or a pick, with that game installed, and reaped on the launcher's
 * thread. busy runs from the start to the reap. */
struct Ingest {
    const openpete_launcher_game* g = nullptr;
    std::string disc, sha, context;
    SDL_AtomicInt* cancel = nullptr;   /* the stop flag the job reads */
    SDL_AtomicInt own_cancel = {};     /* the flag of a job outside the launch-time queue */
    SDL_Thread* th = nullptr;
    SDL_AtomicInt done = {};
    bool busy = false;
    int rc = 0;
};
Ingest s_ing;
/* The box picture render's progress (openpete_launcher_preview_progress),
 * written by the ingest thread; total 0 when no render runs. */
std::atomic<int> s_preview_done{0}, s_preview_total{0};
/* Bumped by openpete_launcher_picture_changed; the page rereads its box
 * pictures when it differs from the value of its last read. */
std::atomic<unsigned> s_picture_gen{0};
/* The stop flag openpete_launcher_job_cancelled reads on the calling thread. */
thread_local SDL_AtomicInt* t_cancel = nullptr;

void ingest_poll();

/* Process start for launcher_stamp: this translation unit's static
 * initialization, which runs before main. */
const op_time_t k_process_start_ns = op_now();

/** @brief Return the display name of the game the launcher was started for.
 *
 * @return The name, or NULL while the launcher names no game (no game is
 *         installed yet). */
const char* game_name() { return s.game ? s.game->display_name : nullptr; }

/* The layout-to-window mapping of the frame being drawn. */
struct Frame { float scale, ox, oy; };

/** @brief Map the 640x448 layout space into a window of a size, letterboxed and centred.
 *
 * @param d The window size.
 * @return The scale and the offset of the layout's origin. */
Frame frame_map_for(ImVec2 d) {
    const float sc = (d.x / kLayoutW < d.y / kLayoutH) ? d.x / kLayoutW : d.y / kLayoutH;
    return {sc, (d.x - kLayoutW * sc) * 0.5f, (d.y - kLayoutH * sc) * 0.5f};
}

/** @brief Map the 640x448 layout space into the frame imgui draws, letterboxed and centred.
 *
 * @return The scale and the offset of the layout's origin. */
Frame frame_map() { return frame_map_for(ImGui::GetIO().DisplaySize); }

/* Fredoka's capitals are 700/1210 of its line, ProggyClean's 8/13: the body
 * font is pushed this much larger than a layout size so its capitals stand
 * as tall as ProggyClean's at that size. */
constexpr float kBodyScale = (8.0f / 13.0f) / (700.0f / 1210.0f);

/** @brief Push the body font (launcher_body_font) at a size whose capitals match ProggyClean's at @p px, or the current font at @p px when the body font is not loaded.
 *
 * @param px The text size in window pixels, as ProggyClean would be drawn. */
void push_body_font(float px) {
    if (ImFont* body = launcher_body_font()) ImGui::PushFont(body, px * kBodyScale);
    else ImGui::PushFont(nullptr, px);
}

}  // namespace

void launcher_push_body_font(float px) { push_body_font(px); }

namespace {

/** @brief Draw one line of text horizontally centred in the layout and record it in the element manifest.
 *
 * The manifest rect is the text's origin as imgui draws it (truncated to
 * whole pixels) and its size at the pushed font.
 * @param f The frame mapping.
 * @param id The element's manifest id.
 * @param y The line's top in layout pixels.
 * @param size The text height in layout pixels, as ProggyClean; the body
 *             font draws at the size whose capitals match (push_body_font).
 * @param col The colour.
 * @param text The text. */
void text_centred(const Frame& f, const char* id, float y, float size, ImU32 col, const char* text) {
    push_body_font(size * f.scale);
    const ImVec2 sz = ImGui::CalcTextSize(text);
    ImGui::SetCursorPos(ImVec2(f.ox + (kLayoutW * f.scale - sz.x) * 0.5f, f.oy + y * f.scale));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float x0 = std::trunc(at.x), y0 = std::trunc(at.y);
    launcher_manifest_item(id, "text", x0, y0, x0 + sz.x, y0 + sz.y);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

/* The banner and footer of a progress stage, chosen by the stage text's
 * prefix; counted stages show their counts after the stage. The last row's
 * empty prefix matches every other stage. */
struct StageLook {
    const char* prefix;
    const char* banner;
    const char* footer;
    bool counted;
};
constexpr StageLook kStageLooks[] = {
    {"COMPILING MODS", "PREPARING MODS", "CHANGED MOD SOURCES ARE COMPILED FOR THIS PC.", true},
    {"COMPILING SHADERS", "PREPARING GRAPHICS",
     "SHADERS ARE COMPILED FOR THIS PC'S GRAPHICS DRIVER.", true},
    {"", "FIRST-TIME SETUP", "YOUR DISC IS REBUILT FOR THIS PC. THIS HAPPENS ONCE.", false},
};

/** @brief Return the look of a progress stage: the first row of kStageLooks whose prefix starts the stage text.
 *
 * @param stage The stage text.
 * @return The row; the last row when no other matches. */
const StageLook& stage_look(const char* stage) {
    for (const StageLook& l : kStageLooks)
        if (strncmp(stage, l.prefix, strlen(l.prefix)) == 0) return l;
    return kStageLooks[sizeof kStageLooks / sizeof kStageLooks[0] - 1];
}

/* The progress view's step line top, in layout px: the place the step chip
 * and the game page's quit chip draw at. */
constexpr float kStepChipY = 196 + kBandY;
/* A centred chip's padding around its line's box, layout px. */
constexpr float kChipPadX = 12.0f, kChipPadY = 7.0f;

/** @brief Draw one line of text horizontally centred in the layout on a chip (launcher_chip_text): the box, the text scrim, the text.
 *
 * The line's origin is its centred position unrounded (imgui truncates it
 * when it draws); the chip spans the line's box grown by kChipPadX x
 * kChipPadY layout px, and the text records as @p id, the chip as
 * <id>.panel and the scrim as <id>.scrim.
 * @param f The frame mapping.
 * @param id The text's manifest id.
 * @param y The line's top in layout pixels.
 * @param size The text height in layout pixels, as ProggyClean (push_body_font).
 * @param col The text colour.
 * @param text The text. */
void chip_centred(const Frame& f, const char* id, float y, float size, ImU32 col, const char* text) {
    push_body_font(size * f.scale);
    const ImVec2 sz = ImGui::CalcTextSize(text);
    const ImVec2 at(f.ox + (kLayoutW * f.scale - sz.x) * 0.5f, f.oy + y * f.scale);
    launcher_chip_text(id, at, ImVec2(kChipPadX, kChipPadY), f.scale, col, text);
    ImGui::PopFont();
}

/* The progress bar (layout px): its left inset, top below the band's top,
 * height and border; light track, dark border, orange fill. */
constexpr float kBarX = 80.0f, kBarY = 240.0f, kBarH = 22.0f, kBarBorder = 2.0f;

/** @brief Draw the progress screen: banner, step chip, bar and footer.
 *
 * The stage's row of kStageLooks gives the banner and footer. A counted
 * stage shows done/total after the stage text, or done alone while total
 * is 0 and done is not. The bar (launcher_shape_bar) fills done/total,
 * clamped to 1, and is empty when total is 0.
 * @param f The frame mapping.
 * @param v The view. */
void draw_progress(const Frame& f, const View& v) {
    const StageLook& look = stage_look(v.stage);
    text_centred(f, "banner", 160 + kBandY, 14, IM_COL32(220, 210, 240, 255), look.banner);
    char line[160];
    if (look.counted && v.total > 0) snprintf(line, sizeof line, "%s %d/%d", v.stage, v.done, v.total);
    else if (look.counted && v.done > 0) snprintf(line, sizeof line, "%s %d", v.stage, v.done);
    else snprintf(line, sizeof line, "%s", v.stage);
    chip_centred(f, "step", kStepChipY, 14, IM_COL32_WHITE, line);
    const float frac = v.total > 0 ? (float)v.done / (float)v.total : 0.0f;
    const ImVec2 a(f.ox + kBarX * f.scale, f.oy + (kBarY + kBandY) * f.scale);
    const ImVec2 b(f.ox + (kLayoutW - kBarX) * f.scale, a.y + kBarH * f.scale);
    launcher_shape_bar("bar", a, b, f.scale, frac, IM_COL32(150, 130, 190, 255), IM_COL32(240, 138, 30, 255),
                       IM_COL32(50, 38, 80, 255), kBarBorder);
    text_centred(f, "footer", 292 + kBandY, 8, IM_COL32(150, 130, 190, 255), look.footer);
}

/** @brief Draw the quit ask's chip, "Press again to quit", at the step chip's place: white text on the step chip's box (chip_centred).
 *
 * @param f The frame mapping. */
void draw_quit_ask(const Frame& f) {
    chip_centred(f, "quit.ask", kStepChipY, 14, IM_COL32_WHITE, "Press again to quit");
}

/** @brief Draw the pending screen's line where the selector's first entry goes.
 *
 * @param f The frame mapping. */
void draw_pending(const Frame& f) {
    text_centred(f, "reading", 168 + kBandY, 14, IM_COL32(110, 95, 150, 255), "READING YOUR LIBRARY...");
}

/* The pixel title: ProggyClean at kTitleScale times the 28 layout px the
 * screens were first drawn at, its top kTitleTop layout px below the band's
 * top. */
constexpr float kTitleScale = 2.4f;
constexpr float kTitleTop = 8.0f;

/** @brief Return the step, in window coordinates, of a grid whose every line is both a whole window coordinate and a whole framebuffer pixel.
 *
 * imgui truncates a text origin to whole window coordinates and the
 * framebuffer holds @p density pixels per window coordinate, so an origin
 * on a multiple of the step lands on a framebuffer pixel edge: 1 at
 * density 1 or 2, 2 at 1.5, 4 at 1.25. A density with no such step up to 8
 * returns 1.
 * @param density Framebuffer pixels per window coordinate.
 * @return The step, 1 to 8. */
float fb_origin_step(float density) {
    for (int q = 1; q <= 8; q++) {
        const float v = density * (float)q;
        if (std::fabs(v - std::round(v)) < 1e-3f) return (float)q;
    }
    return 1.0f;
}

/** @brief Draw the pixel title OPENPETE centred in the layout band in whole-pixel cells, and record it in the element manifest.
 *
 * ProggyClean's outlines are squares of 128 font units, 13 to its pixel
 * height, so rasterized at 13 x c framebuffer pixels each font pixel covers
 * exactly c x c of them and every edge is sharp. The framebuffer holds d
 * pixels per window coordinate (imgui's DisplayFramebufferScale, the window's
 * pixel size over its size, which the font is also rasterized at): c is the
 * title size (28 x kTitleScale layout px, times d) over 13, rounded to a
 * whole pixel, at least 1, so the cell is c / d window coordinates. The
 * title is 7 c / d wide per glyph and 13 c / d tall, centred horizontally,
 * its origin truncated to the fb_origin_step grid so it starts on a
 * framebuffer pixel edge. While the present draws the 3D title
 * (launcher_title_drawn) the pixel title is not drawn and the 3D title's
 * rect is recorded as title.moby instead. While the present holds the 3D
 * title back (the launcher's first present, prepare_title) no title is
 * drawn. The drawn title's centre is the sky reveal's centre
 * (launcher_sky_title_at).
 * @param f The frame mapping. */
void draw_title(const Frame& f) {
    static const char kTitle[] = "OPENPETE";
    float r[4];
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    if (launcher_title_drawn(r)) {
        launcher_manifest_item("title.moby", "mesh3d", r[0], r[1], r[2], r[3]);
        if (disp.x > 0.0f && disp.y > 0.0f)
            launcher_sky_title_at(0.5f * (r[0] + r[2]) / disp.x, 0.5f * (r[1] + r[3]) / disp.y);
        return;
    }
    if (s.title_held) return;
    const float d = std::max(1.0f, ImGui::GetIO().DisplayFramebufferScale.x);
    const float q = fb_origin_step(d);
    const float p = 28.0f * kTitleScale * f.scale * d;
    const float cell = std::max(1.0f, std::round(p / 13.0f)) / d;
    const float w = 7.0f * cell * (float)(sizeof kTitle - 1);
    const float x = std::trunc((f.ox + (kLayoutW * f.scale - w) * 0.5f) / q) * q;
    const float y = std::trunc((f.oy + (kTitleTop + kBandY) * f.scale) / q) * q;
    ImGui::PushFont(nullptr, 13.0f * cell);
    ImGui::GetWindowDrawList()->AddText(ImVec2(x, y), IM_COL32(240, 138, 30, 255), kTitle);
    ImGui::PopFont();
    launcher_manifest_item("title", "text", x, y, x + w, y + 13.0f * cell);
    if (disp.x > 0.0f && disp.y > 0.0f)
        launcher_sky_title_at((x + 0.5f * w) / disp.x, (y + 6.5f * cell) / disp.y);
}

/** @brief Emit the 3D title for the next present on the launcher's device, fitted to the pixel title's box.
 *
 * The title spans the pixel title's width (7 cells per glyph, centred
 * without truncation), with a gap of 0.004 of the layout height after each
 * glyph but the last, and is centred on the ink of the pixel title's
 * capitals: ProggyClean's baseline at trunc(top) + ceil(9.5 c) + c and its
 * capitals 8 cells tall, c the cell. On another device (the adopted one
 * after the hand-off) the 3D title does not draw. The launcher's first
 * present holds the 3D title back and draws no title in its place when a
 * game's 3D title is chosen (launcher_title_chosen), so the title pass's
 * pipelines and target are created on a later present and the first
 * present shows no pixel title the next one would replace.
 * @param dev The presenting device; NULL for the launcher's. */
void prepare_title(struct rhi_device* dev) {
    int w = 0, h = 0;
    s.title_held = !dev && !s.presented && launcher_title_chosen();
    if (!dev && !s.presented) {
        (void)launcher_title_prepare(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    SDL_Window* win = openpete_native_launcher_window();
    if (dev || !win || !SDL_GetWindowSize(win, &w, &h) || w <= 0 || h <= 0) {
        (void)launcher_title_prepare(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    const Frame f = frame_map_for(ImVec2((float)w, (float)h));
    const float cell = std::max(1.0f, std::round(28.0f * kTitleScale * f.scale / 13.0f));
    const float width = 7.0f * cell * 8.0f;
    const float x0 = f.ox + (kLayoutW * f.scale - width) * 0.5f;
    const float top = std::trunc(f.oy + (kTitleTop + kBandY) * f.scale);
    const float cy = top + std::ceil(9.5f * cell) + cell - 4.0f * cell;
    (void)launcher_title_prepare((float)w, (float)h, x0, cy, width, 0.004f * kLayoutH * f.scale);
}

/** @brief Draw one frame of a screen: the pixel title, then the screen's content, in one borderless window over the whole display.
 *
 * The per-present callback openpete_native_launcher_present runs between
 * imgui's NewFrame and Render.
 * @param ud The View. */
void draw_view(void* ud) {
    View& v = *static_cast<View*>(ud);
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus);
    static const bool hide = getenv("OPENPETE_LAUNCHER_TEXT_HIDE") != nullptr;
    if (hide) ImGui::GetStyle().Alpha = 0.0f;
    const Frame f = frame_map();
    launcher_manifest_begin();
    draw_title(f);
    if (v.screen == Screen::Progress) draw_progress(f, v);
    else if (v.screen == Screen::Pending) draw_pending(f);
    else {
        v.activated = launcher_page_draw(v.entry_games, v.count, v.focus, &v.hovered);
        if (v.quit_asked) draw_quit_ask(f);
    }
    s.item_hovered = ImGui::IsAnyItemHovered();
    ImGui::End();
}

/* Shot tags, indexed by Screen, then the precompile's progress screen, then
 * the first frames showing the sky's opaque dome and the 3D title. */
constexpr const char* kShotTag[] = {"progress", "drop", "selector", "pending", "precompile", "sky", "title3d"};
constexpr int kShotPrecompile = 4;
constexpr int kShotSky = 5;   /* the first frame whose sky is one opaque dome */
constexpr int kShotTitle = 6;   /* the first frame drawing the 3D title */

/** @brief Return the clear colour of every launcher present: kBg, or the harness variable OPENPETE_LAUNCHER_CLEAR=r,g,b read once.
 *
 * @return R, G, B bytes. */
const uint8_t* launcher_clear() {
    static uint8_t c[3] = {kBg[0], kBg[1], kBg[2]};
    static bool read = false;
    if (!read) {
        read = true;
        const char* e = getenv("OPENPETE_LAUNCHER_CLEAR");
        int r = 0, g = 0, b = 0;
        if (e && sscanf(e, "%d,%d,%d", &r, &g, &b) == 3) {
            c[0] = (uint8_t)std::clamp(r, 0, 255);
            c[1] = (uint8_t)std::clamp(g, 0, 255);
            c[2] = (uint8_t)std::clamp(b, 0, 255);
            OPL_INFO_F("launcher", "clear colour (%d,%d,%d) (OPENPETE_LAUNCHER_CLEAR)", c[0], c[1], c[2]);
        }
    }
    return c;
}

/** @brief Read back the first frame of a shot tag when OPENPETE_LAUNCHER_SHOT is set, logging how many pixels it drew.
 *
 * @param dev The device the frame was presented on; NULL for the launcher's.
 * @param tag The kShotTag index. */
void maybe_shot(struct rhi_device* dev, int tag) {
    const unsigned bit = 1u << (unsigned)tag;
    const char* base = getenv("OPENPETE_LAUNCHER_SHOT");
    if (!base || !*base || (s.shot_done & bit)) return;
    s.shot_done |= bit;
    const std::string path = std::string(base) + "." + kShotTag[tag] + ".png";
    unsigned differ = 0, total = 0;
    const int rc = openpete_native_launcher_shot(dev, path.c_str(), launcher_clear(), &differ, &total);
    OPL_INFO_F("launcher", "screen %s: %u of %u pixels drawn over the clear (shot %s, rc %d)",
               kShotTag[tag], differ, total, path.c_str(), rc);
}

/** @brief Present one frame of a view on a device.
 *
 * @param dev The device; NULL for the launcher's.
 * @param v The view.
 * @param tag The kShotTag index of its first-frame readback.
 * @return true when a frame was drawn. */
bool present_on(struct rhi_device* dev, View& v, int tag) {
    prepare_title(dev);
    const uint8_t* bg = launcher_clear();
    const bool drawn = openpete_native_launcher_present(dev, bg[0] / 255.0f, bg[1] / 255.0f,
                                                        bg[2] / 255.0f, draw_view, &v) != 0;
    if (!drawn) return false;
    maybe_shot(dev, tag);
    const ImVec2 d = ImGui::GetIO().DisplaySize;
    launcher_manifest_write((unsigned)tag, kShotTag[tag], d.x, d.y);
    return true;
}

void scripted_input_poll();

/** @brief Present one frame of a view on the launcher's device, naming the launcher's game, then run the harness input that is due.
 *
 * @param v The view. */
void present(View& v) {
    v.name = game_name();
    launcher_sky_present(op_now(), s.presented);   /* the present's instant: the sky's motion follows it */
    if (!present_on(nullptr, v, (int)v.screen)) return;
    s.presented = true;
    if (launcher_sky_opaque()) maybe_shot(nullptr, kShotSky);
    if (launcher_title_drawn(nullptr)) {
        maybe_shot(nullptr, kShotTitle);
        const ImVec2 d = ImGui::GetIO().DisplaySize;
        launcher_manifest_write((unsigned)kShotTitle, kShotTag[kShotTitle], d.x, d.y);
    }
    launcher_stamp(LAUNCHER_EV_FIRST_PRESENT);
    scripted_input_poll();
    if (v.screen == Screen::Select && v.count > 0) launcher_stamp(LAUNCHER_EV_LIST_SHOWN);
}

/** @brief Opens the first connected gamepad unless one is already open. */
void open_first_gamepad() {
    if (s.pad) return;
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    if (ids) {
        if (n > 0) s.pad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }
}

/** @brief Closes the gamepad the launcher opened. */
void close_gamepad() {
    if (s.pad) { SDL_CloseGamepad(s.pad); s.pad = nullptr; }
}

/* The launcher window's size when neither --launcher-window nor an explicit
 * game window size is set: the Steam Deck's panel, in window coordinates. */
constexpr int kLauncherDefaultW = 1280;
constexpr int kLauncherDefaultH = 800;

/** @brief Return the launcher window's first size and state: --launcher-window, else an explicit game window size (--window or its config key), else 1280x800; maximized as either asks; --launcher-fullscreen.
 *
 * The options before the hand-off are the game-neutral tiers, so a
 * [game.<id>] window value does not reach the launcher, and the game
 * layout's default window size does not apply to it. A window first opened
 * at the hand-off (no screen showed before it) is the game's window and
 * takes the game window's size (openpete_game_window_size), the 640x448
 * layout space when no game is installed.
 * @param opts The options.
 * @param w Receives the width; the size a maximized or fullscreen window returns to.
 * @param h Receives the height.
 * @param open Receives the OPENPETE_WINDOW_OPEN_* bits. */
void launcher_window_request(const openpete_options_t* opts, int* w, int* h, int* open) {
    *open = 0;
    if (opts->launcher_window_maximized == 0) {
        *w = opts->launcher_window_w;
        *h = opts->launcher_window_h;
    } else if (s.handed_off) {
        openpete_game_window_size(opts, w, h);
        if (*w <= 0 || *h <= 0) { *w = (int)kLayoutW; *h = (int)kLayoutH; }
    } else if (opts->window_w > 0 && opts->window_h > 0) {
        *w = opts->window_w;
        *h = opts->window_h;
    } else {
        *w = kLauncherDefaultW;
        *h = kLauncherDefaultH;
    }
    if (opts->launcher_window_maximized > 0 ||
        (opts->launcher_window_maximized < 0 && opts->window_maximized > 0))
        *open |= OPENPETE_WINDOW_OPEN_MAXIMIZED;
    if (opts->launcher_fullscreen > 0) *open |= OPENPETE_WINDOW_OPEN_FULLSCREEN;
}

/* A display's full and usable bounds, in window coordinates. */
struct DisplayBounds { int full_w, full_h, use_w, use_h; };

/** @brief Read the bounds of the display the launcher window opens on: the primary display's full bounds and its usable bounds (without panels).
 *
 * The harness variable OPENPETE_LAUNCHER_DISPLAY_STUB=<W>x<H>,<W>x<H> gives
 * the full and the usable bounds instead, so the fit rule runs against a
 * display the test machine does not have.
 * @param b Receives the bounds.
 * @return true when the bounds were read or stubbed. */
bool display_bounds(DisplayBounds* b) {
    if (const char* e = getenv("OPENPETE_LAUNCHER_DISPLAY_STUB"); e && *e)
        return sscanf(e, "%dx%d,%dx%d", &b->full_w, &b->full_h, &b->use_w, &b->use_h) == 4;
    const SDL_DisplayID d = SDL_GetPrimaryDisplay();
    SDL_Rect full, use;
    if (!d || !SDL_GetDisplayBounds(d, &full) || !SDL_GetDisplayUsableBounds(d, &use)) return false;
    *b = {full.w, full.h, use.w, use.h};
    return true;
}

/** @brief Fit the launcher window's requested windowed size to the display: as requested when it fits the usable bounds, maximized when it fits only the full bounds, else clamped to 90% of the usable bounds at the request's aspect; under gamescope the output size.
 *
 * Sizes are window coordinates, so a request opens at the same apparent
 * size on a scaled display as on an unscaled one. A request already
 * maximized or fullscreen, or a display whose bounds cannot be read, is
 * left as it is. Gamescope sets GAMESCOPE_WAYLAND_DISPLAY for every child it
 * launches; its output is the display's full bounds. The decision is
 * logged.
 * @param w The requested width; receives the width the window opens at.
 * @param h The requested height; receives the height.
 * @param open The OPENPETE_WINDOW_OPEN_* bits; gains MAXIMIZED when the
 *             request fits only the full bounds. */
void launcher_window_fit(int* w, int* h, int* open) {
    DisplayBounds b;
    if ((*open & (OPENPETE_WINDOW_OPEN_MAXIMIZED | OPENPETE_WINDOW_OPEN_FULLSCREEN)) ||
        !display_bounds(&b) || b.use_w <= 0 || b.use_h <= 0)
        return;
    const int rw = *w, rh = *h;
    const char* how = "as requested";
    if (const char* gs = getenv("GAMESCOPE_WAYLAND_DISPLAY"); gs && *gs) {
        *w = b.full_w;
        *h = b.full_h;
        how = "at the gamescope output size";
    } else if (rw > b.use_w || rh > b.use_h) {
        if (rw <= b.full_w && rh <= b.full_h) {
            *open |= OPENPETE_WINDOW_OPEN_MAXIMIZED;
            how = "maximized";
        } else {
            const float kw = 0.9f * (float)b.use_w / (float)rw, kh = 0.9f * (float)b.use_h / (float)rh;
            const float k = kw < kh ? kw : kh;
            *w = (int)std::lround((float)rw * k);
            *h = (int)std::lround((float)rh * k);
            how = "clamped to 90% of the usable bounds";
        }
    }
    OPL_INFO_F("launcher", "window fit: request %dx%d, display %dx%d, usable %dx%d: opens %s (%dx%d)",
               rw, rh, b.full_w, b.full_h, b.use_w, b.use_h, how, *w, *h);
}

/** @brief Take the state the launcher window was asked to open in as the untouched state.
 *
 * The request, not the window's flags: a window manager applies a maximize
 * or fullscreen after the window maps, and its event then confirms the
 * state instead of changing it.
 * @param opts The options; --background drops a fullscreen request.
 * @param w The requested width, the windowed size while the window opens maximized or fullscreen.
 * @param h The requested height.
 * @param open The OPENPETE_WINDOW_OPEN_* bits it was created with. */
void window_state_init(const openpete_options_t* opts, int w, int h, int open) {
    s.fs_now = (open & OPENPETE_WINDOW_OPEN_FULLSCREEN) != 0 && opts->background <= 0;
    s.max_now = s.max_open = (open & OPENPETE_WINDOW_OPEN_MAXIMIZED) != 0;
    s.win_w = s.open_w = w;
    s.win_h = s.open_h = h;
}

/** @brief Record a failed open: tell the player now, or hold the message for the next open when @p continues is negative.
 *
 * @param why The reason.
 * @param continues As launcher_open's. */
void open_failed(const std::string& why, int continues) {
    s.failed = true;
    if (continues < 0) {
        s.fail_deferred = true;
        s.fail_why = why;
        return;
    }
    openpete_launcher_vk_failure(why.c_str(), continues);
}

/** @brief Open the launcher's window, device and imgui context unless they exist, telling the player once when the device cannot be created.
 *
 * The window takes launcher_window_request's size; before the hand-off that
 * size is fitted to the display (launcher_window_fit). A device created for a screen, before the
 * hand-off, starts the pipeline precompile on it
 * (openpete_native_launcher_precompile_start); one created at the hand-off
 * leaves the compile to bring-up.
 * @param opts The options the window is created from.
 * @param continues Non-zero when the run can continue without the device
 *                  (a disc is or will be named); passed to the message.
 *                  Negative while that is not known yet (the library scan
 *                  runs): the message waits for the next call that knows.
 * @return 0 when the device exists, -1 when it does not. */
int launcher_open(const openpete_options_t* opts, int continues) {
    if (openpete_native_launcher_active()) return 0;
    if (s.failed) {
        if (s.fail_deferred && continues >= 0) {
            s.fail_deferred = false;
            openpete_launcher_vk_failure(s.fail_why.c_str(), continues);
        }
        return -1;
    }
    if (!opts) return -1;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        open_failed(std::string("The video system could not start (") + SDL_GetError() + ").",
                    continues);
        return -1;
    }
    int w, h, open = 0;
    launcher_window_request(opts, &w, &h, &open);
    if (!s.handed_off) launcher_window_fit(&w, &h, &open);
    if (openpete_native_launcher_open(opts, w, h, open, "OpenPete") != 0) {
        open_failed(openpete_native_launcher_error(), continues);
        return -1;
    }
    window_state_init(opts, w, h, open);
    (void)launcher_body_font();
    launcher_stamp(LAUNCHER_EV_WINDOW);
    open_first_gamepad();
    if (!s.handed_off) (void)openpete_native_launcher_precompile_start(opts);
    return 0;
}

/** @brief SDL file-dialog callback: store the chosen path and flag it, logging the outcome (chosen, cancelled or failed).
 *
 * @param userdata Unused.
 * @param files A NULL-terminated list of chosen paths, empty on a cancel, or NULL on an error.
 * @param filter Unused. */
void SDLCALL on_file_chosen(void* userdata, const char* const* files, int filter) {
    (void)userdata; (void)filter;
    if (files && files[0]) {
        snprintf(s.drop_path, sizeof s.drop_path, "%s", files[0]);
        SDL_SetAtomicInt(&s.dialog_got, 1);
        OPL_INFO_F("launcher", "file dialog: chosen %s", files[0]);
    } else if (files) {
        OPL_INFO_F("launcher", "file dialog: cancelled");
    } else {
        OPL_WARN_F("launcher", "file dialog: failed: %s", SDL_GetError());
    }
    SDL_SetAtomicInt(&s.dialog_pending, 0);
}

/** @brief Show the native open-file dialog for a .cue or .bin image unless one is open. */
void open_file_dialog() {
    if (SDL_GetAtomicInt(&s.dialog_pending)) return;
    static const SDL_DialogFileFilter filters[] = {
        {"PSX disc image (.cue, .bin)", "cue;bin"},
        {"All files", "*"},
    };
    SDL_SetAtomicInt(&s.dialog_pending, 1);
    SDL_SetAtomicInt(&s.dialog_got, 0);
    OPL_INFO_F("launcher", "file dialog: requested (video driver %s)", SDL_GetCurrentVideoDriver());
    SDL_ShowOpenFileDialog(on_file_chosen, nullptr, openpete_native_launcher_window(),
                           filters, 2, nullptr, false);
}

/** @brief Toggle the launcher window between windowed and borderless-desktop fullscreen (openpete_native_launcher_window_op). */
void toggle_fullscreen() {
    const int on = openpete_native_launcher_window_op(OPENPETE_LAUNCHER_WINDOW_FULLSCREEN_TOGGLE, 0, 0);
    if (on < 0) return;
    s.fs_pending = true;
    OPL_INFO_F("launcher", "fullscreen %s", on ? "on (borderless desktop)" : "off");
}

/** @brief Record the window's fullscreen, maximized and windowed-size state that a window event reports.
 *
 * The flags are read at the event, so the order of a platform's resize and
 * state events does not matter. A fullscreen change marks the fullscreen
 * touched. A resize while fullscreen, or while a toggle's enter or leave
 * event is due, is the toggle's and leaves the windowed size alone.
 * @param e The event. */
void window_event(const SDL_Event& e) {
    if (e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN || e.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN)
        s.fs_pending = false;
    else if (e.type != SDL_EVENT_WINDOW_RESIZED && e.type != SDL_EVENT_WINDOW_MAXIMIZED &&
             e.type != SDL_EVENT_WINDOW_RESTORED)
        return;
    SDL_Window* win = openpete_native_launcher_window();
    if (!win) return;
    const SDL_WindowFlags f = SDL_GetWindowFlags(win);
    const bool fs = (f & SDL_WINDOW_FULLSCREEN) != 0;
    if (fs != s.fs_now) {
        s.fs_now = fs;
        s.fs_touched = true;
    }
    if (fs || s.fs_pending) return;
    s.max_now = (f & SDL_WINDOW_MAXIMIZED) != 0;
    int w = 0, h = 0;
    if (s.max_now || !SDL_GetWindowSize(win, &w, &h)) return;
    s.win_w = w;
    s.win_h = h;
}

/** @brief Act on the window gestures of one event: Alt+Enter and a double-click outside the buttons and entries toggle fullscreen; window events update the recorded state.
 *
 * @param e The event.
 * @return true when the event was a gesture and is consumed. */
bool window_gesture(const SDL_Event& e) {
    window_event(e);
    const bool alt_enter = e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                           (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) &&
                           (e.key.mod & SDL_KMOD_ALT) != 0;
    const bool dbl = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT &&
                     e.button.clicks == 2 && !s.item_hovered;
    if (!alt_enter && !dbl) return false;
    toggle_fullscreen();
    return true;
}

/** @brief Harness: build the event of a key or pad-button action: altenter, browse, key=<name> or pad=<button>.
 *
 * Keys: left, right, up, down, enter, escape, space; altenter is Return
 * with the left Alt held, browse is Space. Pad buttons: south, east, west, left, right, up,
 * down (the d-pad). Key events carry OPENPETE_INJECT_DEVICE_ID.
 * @param a The action.
 * @param id The launcher window's id.
 * @param e Receives the event.
 * @return true when @p a is one of these actions. */
bool scripted_button_event(const std::string& a, SDL_WindowID id, SDL_Event& e) {
    static const struct { const char* n; SDL_Keycode k; SDL_Scancode sc; SDL_Keymod mod; } k_keys[] = {
        {"altenter", SDLK_RETURN, SDL_SCANCODE_RETURN, SDL_KMOD_LALT},
        {"browse", SDLK_SPACE, SDL_SCANCODE_SPACE, 0},
        {"key=left", SDLK_LEFT, SDL_SCANCODE_LEFT, 0}, {"key=right", SDLK_RIGHT, SDL_SCANCODE_RIGHT, 0},
        {"key=up", SDLK_UP, SDL_SCANCODE_UP, 0}, {"key=down", SDLK_DOWN, SDL_SCANCODE_DOWN, 0},
        {"key=enter", SDLK_RETURN, SDL_SCANCODE_RETURN, 0},
        {"key=escape", SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, 0}, {"key=space", SDLK_SPACE, SDL_SCANCODE_SPACE, 0}};
    static const struct { const char* n; SDL_GamepadButton b; } k_pad[] = {
        {"pad=south", SDL_GAMEPAD_BUTTON_SOUTH}, {"pad=east", SDL_GAMEPAD_BUTTON_EAST},
        {"pad=west", SDL_GAMEPAD_BUTTON_WEST}, {"pad=left", SDL_GAMEPAD_BUTTON_DPAD_LEFT},
        {"pad=right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT}, {"pad=up", SDL_GAMEPAD_BUTTON_DPAD_UP},
        {"pad=down", SDL_GAMEPAD_BUTTON_DPAD_DOWN}};
    for (const auto& k : k_keys)
        if (a == k.n) {
            e.type = SDL_EVENT_KEY_DOWN;
            e.key.windowID = id;
            e.key.key = k.k;
            e.key.scancode = k.sc;
            e.key.mod = k.mod;
            e.key.down = true;
            e.key.which = OPENPETE_INJECT_DEVICE_ID;
            return true;
        }
    for (const auto& b : k_pad)
        if (a == b.n) {
            e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
            e.gbutton.button = (Uint8)b.b;
            e.gbutton.down = true;
            return true;
        }
    return false;
}

/** @brief Harness: build the event of an input action: a key or pad button (scripted_button_event), stick=<x|y>:<value>, mouse=<x>/<y>, dblclick or quit.
 *
 * stick sets the left stick's axis to the value (-32768 to 32767). mouse
 * warps the pointer to a point in the 640x448 layout space of the frame
 * last drawn and builds no event (e.type stays 0): the platform then
 * reports the motion and the pointer's window, which imgui's backend reads
 * each frame. dblclick is a left double-click at the bottom centre of the
 * window, below the screens' buttons and entries; quit is the event closing
 * the window posts.
 * @param a The action.
 * @param win The launcher's window.
 * @param e Receives the event.
 * @return true when @p a is one of these actions. */
bool scripted_event(const std::string& a, SDL_Window* win, SDL_Event& e) {
    const SDL_WindowID id = SDL_GetWindowID(win);
    if (scripted_button_event(a, id, e)) return true;
    char axis = 0;
    int value = 0, w = 0, h = 0;
    float x = 0, y = 0;
    if (sscanf(a.c_str(), "stick=%c:%d", &axis, &value) == 2 && (axis == 'x' || axis == 'y')) {
        e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
        e.gaxis.axis = (Uint8)(axis == 'x' ? SDL_GAMEPAD_AXIS_LEFTX : SDL_GAMEPAD_AXIS_LEFTY);
        e.gaxis.value = (Sint16)std::clamp(value, -32768, 32767);
    } else if (sscanf(a.c_str(), "mouse=%f/%f", &x, &y) == 2) {
        const Frame f = frame_map();
        SDL_WarpMouseInWindow(win, f.ox + x * f.scale, f.oy + y * f.scale);
    } else if (a == "dblclick") {
        SDL_GetWindowSize(win, &w, &h);
        e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.windowID = id;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.down = true;
        e.button.clicks = 2;
        e.button.x = (float)w * 0.5f;
        e.button.y = (float)h - 4.0f;
    } else if (a == "quit") {
        e.type = SDL_EVENT_QUIT;
    } else {
        return false;
    }
    return true;
}

/** @brief Harness: call the window operation of a window action: maximize, restore or resize=WxH.
 *
 * @param a The action; any other is ignored. */
void scripted_window_op(const std::string& a) {
    int w = 0, h = 0;
    if (a == "maximize")
        (void)openpete_native_launcher_window_op(OPENPETE_LAUNCHER_WINDOW_MAXIMIZE, 0, 0);
    else if (a == "restore")
        (void)openpete_native_launcher_window_op(OPENPETE_LAUNCHER_WINDOW_RESTORE, 0, 0);
    else if (sscanf(a.c_str(), "resize=%dx%d", &w, &h) == 2)
        (void)openpete_native_launcher_window_op(OPENPETE_LAUNCHER_WINDOW_RESIZE, w, h);
}

/** @brief Harness: run the OPENPETE_LAUNCHER_INPUT actions that are due, timed from the first call (the first launcher frame).
 *
 * An input action (scripted_event) posts its event; a window action
 * (scripted_window_op) calls the window operation a window manager
 * answers. Each action runs once. */
void scripted_input_poll() {
    static Uint64 first_ms = 0;
    static unsigned done = 0;   /* one bit per action, in list order */
    const char* spec = getenv("OPENPETE_LAUNCHER_INPUT");
    SDL_Window* win = openpete_native_launcher_window();
    if (!spec || !*spec || !win) return;
    const Uint64 now = SDL_GetTicks();
    if (!first_ms) first_ms = now ? now : 1;
    unsigned idx = 0;
    for (const char* p = spec; *p && idx < 32; idx++) {
        unsigned long ms = strtoul(p, nullptr, 10);
        const char* act = strchr(p, ':');
        const char* end = strchr(p, ',');
        const std::string a = act ? std::string(act + 1, end ? (size_t)(end - act - 1) : strlen(act + 1)) : "";
        p = end ? end + 1 : p + strlen(p);
        if ((done & (1u << idx)) || now - first_ms < ms) continue;
        done |= 1u << idx;
        OPL_INFO_F("launcher", "scripted input: %s", a.c_str());
        SDL_Event e = {};
        if (!scripted_event(a, win, e)) scripted_window_op(a);
        else if (e.type) (void)SDL_PushEvent(&e);
    }
}

/** @brief Write the launcher window's state the player changed into the booted game's options: fullscreen when toggled, the size or maximized when resized or maximized.
 *
 * @param opts The game's options. */
void carry_window_state(openpete_options_t* opts) {
    const bool size_touched = s.max_now != s.max_open ||
                              (!s.max_now && (s.win_w != s.open_w || s.win_h != s.open_h));
    if (!s.fs_touched && !size_touched) return;
    if (s.fs_touched) opts->fullscreen = s.fs_now ? 1 : 0;
    if (size_touched) {
        opts->window_maximized = s.max_now ? 1 : 0;
        opts->window_w = s.max_now ? 0 : s.win_w;
        opts->window_h = s.max_now ? 0 : s.win_h;
    }
    char size[32];
    if (!size_touched) snprintf(size, sizeof size, "untouched");
    else if (s.max_now) snprintf(size, sizeof size, "maximized");
    else snprintf(size, sizeof size, "%dx%d", s.win_w, s.win_h);
    OPL_INFO_F("launcher", "the launcher window's state carries to the game: fullscreen %s, window %s",
               s.fs_touched ? (s.fs_now ? "on" : "off") : "untouched", size);
}

/* What one input event asks of a waiting screen. */
/* Quit is immediate (Escape, the window closing); Back is the gamepad's east
 * button, which the game page asks once for and the other screens take as
 * Quit. */
enum class Ask { None, Quit, Back, Pick, Browse, Up, Down, Left, Right, Dropped };

/** @brief Read a left-stick axis motion as a move: X as left or right, Y as up or down.
 *
 * Each axis moves once per push past a quarter of its range and must
 * re-centre before it moves again.
 * @param m The axis motion.
 * @return The move, or Ask::None. */
Ask stick_ask(const SDL_GamepadAxisEvent& m) {
    static bool armed[2] = {true, true};   /* X and Y: re-centred since the last move */
    const bool x = m.axis == SDL_GAMEPAD_AXIS_LEFTX;
    if (!x && m.axis != SDL_GAMEPAD_AXIS_LEFTY) return Ask::None;
    bool& arm = armed[x ? 0 : 1];
    if (m.value > -8000 && m.value < 8000) {
        arm = true;
        return Ask::None;
    }
    if (!arm) return Ask::None;
    arm = false;
    if (x) return m.value < 0 ? Ask::Left : Ask::Right;
    return m.value < 0 ? Ask::Up : Ask::Down;
}

/** @brief Read one event as a launcher command: keys, gamepad buttons, the left stick (stick_ask), a dropped file, the window closing.
 *
 * A pointer motion is no command; every motion after the window's first
 * is recorded for the game page's focus (State::pointer_moved). A window gesture (window_gesture) is acted on first and is no command:
 * Alt+Enter toggles fullscreen and does not pick. A dropped file's path is stored for launcher_dropped_path; an added
 * gamepad is opened.
 * @param e The event.
 * @return The command. */
Ask ask_of(const SDL_Event& e) {
    if (window_gesture(e)) return Ask::None;
    switch (e.type) {
        case SDL_EVENT_QUIT: return Ask::Quit;
        case SDL_EVENT_MOUSE_MOTION:
            /* The window's first motion report places the pointer (a window
             * opening under a still pointer reports one); later ones move it. */
            if (s.pointer_placed) s.pointer_moved = true;
            s.pointer_placed = true;
            return Ask::None;
        case SDL_EVENT_DROP_FILE:
            snprintf(s.drop_path, sizeof s.drop_path, "%s", e.drop.data);
            return Ask::Dropped;
        case SDL_EVENT_GAMEPAD_ADDED: open_first_gamepad(); return Ask::None;
        case SDL_EVENT_KEY_DOWN: {
            static const struct { SDL_Keycode k; Ask a; } k_keys[] = {
                {SDLK_ESCAPE, Ask::Quit}, {SDLK_RETURN, Ask::Pick}, {SDLK_KP_ENTER, Ask::Pick},
                {SDLK_SPACE, Ask::Browse}, {SDLK_UP, Ask::Up}, {SDLK_DOWN, Ask::Down},
                {SDLK_LEFT, Ask::Left}, {SDLK_RIGHT, Ask::Right}};
            for (const auto& k : k_keys) if (e.key.key == k.k) return k.a;
            return Ask::None;
        }
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
            static const struct { Uint8 b; Ask a; } k_buttons[] = {
                {SDL_GAMEPAD_BUTTON_SOUTH, Ask::Pick}, {SDL_GAMEPAD_BUTTON_EAST, Ask::Back},
                {SDL_GAMEPAD_BUTTON_WEST, Ask::Browse},
                {SDL_GAMEPAD_BUTTON_DPAD_UP, Ask::Up}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, Ask::Down},
                {SDL_GAMEPAD_BUTTON_DPAD_LEFT, Ask::Left}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, Ask::Right}};
            for (const auto& b : k_buttons) if (e.gbutton.button == b.b) return b.a;
            return Ask::None;
        }
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: return stick_ask(e.gaxis);
        default: return Ask::None;
    }
}

/** @brief Wait up to 100 ms for input (8 ms while a game page box animates), stepping the launcher's precompile at each wake, then hand every pending event to imgui and return the first command among them.
 *
 * A returning compile job pushes a wake event
 * (openpete_native_launcher_precompile_event); a batch of only wake events
 * steps the precompile and keeps waiting for the rest of the 100 ms, so the
 * caller presents at the cadence of its input, not of the compiles. The
 * precompile is also stepped when the wait ends.
 * @return The first command the events carry, or Ask::None. */
Ask wait_input() {
    const Uint32 wake = openpete_native_launcher_precompile_event();
    const Uint64 end = SDL_GetTicks() + (launcher_page_animating() ? 8 : 100);
    for (;;) {
        const Uint64 now = SDL_GetTicks();
        SDL_Event e;
        if (now >= end || !SDL_WaitEventTimeout(&e, (Sint32)(end - now))) {
            (void)openpete_native_launcher_precompile_step();
            return Ask::None;
        }
        Ask first = Ask::None;
        bool input = false;
        do {
            if (wake && e.type == wake) continue;
            input = true;
            openpete_devui_process_event(&e);
            const Ask a = ask_of(e);
            if (first == Ask::None) first = a;
        } while (SDL_PollEvent(&e));
        (void)openpete_native_launcher_precompile_step();
        if (input) return first;
    }
}

/** @brief Harness: whether the scripted pick or drop may act yet.
 *
 * OPENPETE_LAUNCHER_PICK_DELAY_MS=<ms> holds OPENPETE_LAUNCHER_SELECT and
 * OPENPETE_LAUNCHER_DROP until that long after the first call, which the
 * selector and the drop screen make after their first frame; the screens
 * keep presenting meanwhile. Unset, or without screens, they act at once.
 * @return true when the scripted action may run. */
bool scripted_due() {
    static Uint64 first_ms = 0;
    const char* e = getenv("OPENPETE_LAUNCHER_PICK_DELAY_MS");
    if (!e || !*e || !s.ui) return true;
    const Uint64 now = SDL_GetTicks();
    if (!first_ms) {
        first_ms = now ? now : 1;
        OPL_INFO_F("launcher", "scripted pick held for %s ms", e);
    }
    return now - first_ms >= strtoull(e, nullptr, 10);
}

/** @brief Harness: the entry OPENPETE_LAUNCHER_SELECT_GAME names (the first of that game), else the index OPENPETE_LAUNCHER_SELECT names, or -1 when unset or out of range.
 *
 * @param games Each entry's game id.
 * @param count The entry count.
 * @return The scripted pick. */
int scripted_pick(const char* const* games, int count) {
    if (const char* g = getenv("OPENPETE_LAUNCHER_SELECT_GAME"); g && *g) {
        for (int i = 0; i < count; i++)
            if (games && games[i] && !strcmp(games[i], g)) return i;
        return -1;
    }
    const char* e = getenv("OPENPETE_LAUNCHER_SELECT");
    if (!e || !*e) return -1;
    const int pick = atoi(e);
    return (pick >= 0 && pick < count) ? pick : -1;
}

/** @brief Harness: whether the scripted pick waits for idle jobs and they are not idle yet.
 *
 * @return true while OPENPETE_LAUNCHER_SELECT_IDLE is set and a launcher job runs or is queued. */
bool scripted_pick_held() {
    const char* e = getenv("OPENPETE_LAUNCHER_SELECT_IDLE");
    return e && *e && *e != '0' && !launcher_jobs_idle();
}

/* page_command's result while the page stays up. */
constexpr int kPageStays = -100;

/** @brief Act on one command on the game page: move the focus, activate or browse for the focused box, quit at once, or ask once before quitting.
 *
 * Escape and the window closing quit at once; the east button asks once
 * (launcher_quit_ask_press); up and down do nothing.
 * @param a The command.
 * @param focus The page's focus.
 * @param ask The east button's ask state.
 * @param games Each library entry's game id (page_loop's).
 * @param count The entry count.
 * @return The activated box's entry index, LAUNCHER_SEL_QUIT, LAUNCHER_SEL_DROPPED, or kPageStays; an
 *         activated box without an entry opens the file dialog. */
int page_command(Ask a, launcher_focus& focus, launcher_quit_ask& ask, const char* const* games, int count) {
    const int act = launcher_page_activation(games, count, focus.index);
    const char* g = launcher_page_box_game(focus.index);
    switch (a) {
        case Ask::Quit:
            OPL_INFO_F("launcher", "command: quit (Escape or the window closed)");
            return LAUNCHER_SEL_QUIT;
        case Ask::Back: return launcher_quit_ask_press(&ask, SDL_GetTicks()) ? LAUNCHER_SEL_QUIT : kPageStays;
        case Ask::Dropped: return LAUNCHER_SEL_DROPPED;
        case Ask::Browse:
            OPL_INFO_F("launcher", "command: file picker for box %d (%s)", focus.index, g);
            open_file_dialog();
            return kPageStays;
        case Ask::Pick:
            OPL_INFO_F("launcher", "command: activate box %d (%s)", focus.index, g);
            if (act >= 0) return act;
            open_file_dialog();
            return kPageStays;
        case Ask::Left: case Ask::Right:
            (void)launcher_focus_move(&focus, a == Ask::Left ? -1 : 1);
            return kPageStays;
        case Ask::Up: case Ask::Down:
            OPL_INFO_F("launcher", "command: %s does nothing on the game page", a == Ask::Up ? "up" : "down");
            return kPageStays;
        default: return kPageStays;
    }
}

/** @brief Present the game page until a box with a library entry is activated, a disc is dropped or chosen, or the player quits.
 *
 * An activated box without an entry opens the file dialog. The harness drop
 * (launcher_scripted_drop) acts after the first frame; a scripted pick
 * (scripted_pick) returns once due (scripted_due, scripted_pick_held).
 * @param games Each library entry's game id, most recently played first; NULL with @p count 0 for the empty library.
 * @param count The entry count.
 * @param scripted The scripted pick's entry index, or -1.
 * @return The activated entry's index, LAUNCHER_SEL_QUIT or LAUNCHER_SEL_DROPPED (its path in s.drop_path). */
int page_loop(const char* const* games, int count, int scripted) {
    View v;
    v.screen = count > 0 ? Screen::Select : Screen::Drop;
    v.entry_games = games;
    v.count = count;
    /* The page opens on the box of the game a disc was just added for, else
     * the first, held: the pointer takes the focus only after it moves. */
    const int first = launcher_page_box_of_game(games, count, launcher_focus_game());
    if (first >= 0) OPL_INFO_F("launcher", "command: focus starts on box %d (%s, the disc just added)", first,
                               launcher_focus_game());
    launcher_focus_game_set(nullptr);
    launcher_focus focus = {std::max(0, first), launcher_page_box_count(games, count), true};
    launcher_quit_ask ask = {};
    s.pointer_moved = false;
    for (;;) {
        v.activated = LAUNCHER_PAGE_NONE;
        v.focus = focus.index;
        const bool asked = launcher_quit_ask_shown(&ask, SDL_GetTicks());
        if (asked != v.quit_asked)
            OPL_INFO_F("launcher", "command: quit ask chip %s", asked ? "shown" : "hidden");
        v.quit_asked = asked;
        present(v);
        (void)launcher_focus_hover(&focus, v.hovered);
        launcher_jobs_pump();
        if (launcher_scripted_drop()) return LAUNCHER_SEL_DROPPED;
        if (scripted >= 0 && scripted_due() && !scripted_pick_held()) {
            OPL_INFO_F("launcher", "scripted pick: entry %d (%s)", scripted, games[scripted] ? games[scripted] : "");
            return scripted;
        }
        if (v.activated >= 0) return v.activated;
        if (v.activated == LAUNCHER_PAGE_BROWSE) open_file_dialog();
        if (SDL_GetAtomicInt(&s.dialog_got)) {
            SDL_SetAtomicInt(&s.dialog_got, 0);
            return LAUNCHER_SEL_DROPPED;
        }
        const Ask a = wait_input();
        if (s.pointer_moved) launcher_focus_pointer_moved(&focus);
        s.pointer_moved = false;
        const int r = page_command(a, focus, ask, games, count);
        if (r != kPageStays) return r;
    }
}

/** @brief Run the game page with every box empty until a disc image is dropped or chosen, or the player quits.
 *
 * The harness variable OPENPETE_LAUNCHER_DROP=<path> acts as a drop of that
 * path after the first frame (launcher_scripted_drop).
 * @return The path, owned by the launcher, or NULL on quit. */
const char* wait_for_disc() {
    const int r = page_loop(nullptr, 0, -1);
    launcher_page_closed();
    return r == LAUNCHER_SEL_DROPPED ? s.drop_path : nullptr;
}

/** @brief Log the library entries that own no box on the game page: an entry of a game whose box another (more recent) entry owns, or of no game the page draws.
 *
 * @param labels The entry labels.
 * @param games Each entry's game id.
 * @param count The entry count. */
void log_boxless_entries(const char* const* labels, const char* const* games, int count) {
    for (int i = 0; i < count; i++) {
        if (launcher_page_owns_box(games, i)) continue;
        OPL_INFO_F("launcher", "game page: library entry %d (%s, %s) has no box: %s", i, labels[i],
                   games[i] && *games[i] ? games[i] : "unidentified",
                   games[i] && *games[i] ? "a more recently played entry of its game owns it"
                                         : "no game claims it");
    }
}

/** @brief Hand pending events to imgui while a wait is drawn, remembering a quit for the hand-off.
 *
 * A wait (the ingest copy, the module build, the mod compile) cannot be
 * cancelled, so a quit (the window closed, or SIGTERM, which SDL turns into
 * a quit event) is held and re-posted at the hand-off, where the game's
 * loop ends the run, instead of being consumed here. The launcher's
 * precompile is stepped after the events, and its wake events
 * (openpete_native_launcher_precompile_event) are consumed. */
void drain_events() {
    const Uint32 wake = openpete_native_launcher_precompile_event();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (wake && e.type == wake) continue;
        if (e.type == SDL_EVENT_QUIT) {
            s.quit_pending = true;
            continue;
        }
        openpete_devui_process_event(&e);
        (void)window_gesture(e);
    }
    (void)openpete_native_launcher_precompile_step();
}

/** @brief Draw the progress screen with the mailbox's stage when the worker has reported one.
 *
 * Takes a snapshot of the stage and counts under the lock.
 * @return true when a progress report existed. */
bool draw_mailbox() {
    View v;
    char stage[sizeof s.stage];
    SDL_LockMutex(s.lock);
    const bool have = s.have_progress;
    snprintf(stage, sizeof stage, "%s", s.stage);
    v.done = s.done;
    v.total = s.total;
    SDL_UnlockMutex(s.lock);
    if (!have) return false;
    v.stage = stage;
    present(v);
    return true;
}

/** @brief SDL event watch that redraws the progress screen on window expose and move events.
 *
 * Runs synchronously on the thread that pushes the event, which inside the
 * Windows modal move loop is the launcher's thread: the pump is blocked
 * there, but WM_MOVE and WM_PAINT still reach the window procedure. Redraws
 * only while a task runs, on the launcher's thread.
 * @param ud Unused.
 * @param e The event.
 * @return true always. */
bool SDLCALL task_event_watch(void* ud, SDL_Event* e) {
    (void)ud;
    if (e->type != SDL_EVENT_WINDOW_EXPOSED && e->type != SDL_EVENT_WINDOW_MOVED) return true;
    if (!s.task_active || !openpete_native_launcher_active()) return true;
    if (SDL_GetCurrentThreadID() != s.ui_tid) return true;
    (void)draw_mailbox();
    return true;
}

/* A task in flight: the function, its argument and its result. */
struct TaskCtx { openpete_launcher_task_fn fn; void* ud; int rc; };

/** @brief Worker thread entry: run the task, store its result and flag completion.
 *
 * @param p The TaskCtx.
 * @return 0. */
int SDLCALL task_main(void* p) {
    TaskCtx* t = static_cast<TaskCtx*>(p);
    t->rc = t->fn(t->ud);
    SDL_LockMutex(s.lock);
    s.task_done = true;
    SDL_UnlockMutex(s.lock);
    return 0;
}

/** @brief Keep the launcher drawn while a task's worker runs: open the window at its first report, drain events, redraw every 16 ms.
 *
 * Returns when the worker flags completion. */
void task_wait_loop() {
    bool watch = false;
    for (;;) {
        SDL_LockMutex(s.lock);
        const bool done = s.task_done, have = s.have_progress;
        SDL_UnlockMutex(s.lock);
        /* The window opens only once there is something to show: a task that
         * finishes before its first report never flashes one. */
        if (have && launcher_open(s.opts, 1) == 0) {
            if (!watch) { SDL_AddEventWatch(task_event_watch, nullptr); watch = true; }
            drain_events();
            (void)draw_mailbox();
        }
        ingest_poll();
        if (done) break;
        SDL_Delay(16);
    }
    if (watch) SDL_RemoveEventWatch(task_event_watch, nullptr);
}

/** @brief Scan worker entry: run the read, flag completion and wake the pending screen's wait.
 *
 * @param p The TaskCtx.
 * @return 0. */
int SDLCALL scan_main(void* p) {
    TaskCtx* t = static_cast<TaskCtx*>(p);
    t->rc = t->fn(t->ud);
    SDL_SetAtomicInt(&s.scan_done, 1);
    if (s.wake_event && SDL_GetAtomicInt(&s.scan_wakeable)) {
        SDL_Event e = {};
        e.type = s.wake_event;
        (void)SDL_PushEvent(&e);
    }
    return 0;
}

/** @brief Present the pending screen until the scan worker flags completion, keeping the first quit or drop among the events.
 *
 * The wait between presents (wait_input) steps the launcher's precompile
 * and ends at the first event other than a compile wake, the worker's wake
 * event included.
 * @return 0, LAUNCHER_SEL_QUIT or LAUNCHER_SEL_DROPPED. */
int pending_loop() {
    View v;
    v.screen = Screen::Pending;
    int ask = 0;
    while (!SDL_GetAtomicInt(&s.scan_done)) {
        present(v);
        launcher_jobs_pump();
        if (SDL_GetAtomicInt(&s.scan_done)) break;
        const Ask a = wait_input();
        if (ask == 0 && (a == Ask::Quit || a == Ask::Back)) ask = LAUNCHER_SEL_QUIT;
        else if (ask == 0 && a == Ask::Dropped) ask = LAUNCHER_SEL_DROPPED;
    }
    return ask;
}

}  // namespace

int launcher_await_scan(openpete_launcher_task_fn fn, void* userdata) {
    if (!s.ui || s.handed_off) { (void)fn(userdata); return 0; }
    if (!s.wake_event) s.wake_event = SDL_RegisterEvents(1);
    SDL_SetAtomicInt(&s.scan_done, 0);
    SDL_SetAtomicInt(&s.scan_wakeable, 0);
    TaskCtx ctx = {fn, userdata, -1};
    SDL_Thread* th = SDL_CreateThread(scan_main, "openpete-scan", &ctx);
    if (!th) { (void)fn(userdata); return 0; }
    /* The video system and the device start while the scan runs; without a
     * device the scan is only waited for, and the failure is told once the
     * scan decides whether the run can continue. The worker posts its wake
     * event only once the queue runs: a scan that ends before that is seen
     * by the loop's check after its first present. */
    int ask = 0;
    if (launcher_open(s.opts, -1) == 0) {
        SDL_SetAtomicInt(&s.scan_wakeable, 1);
        ask = pending_loop();
    }
    SDL_WaitThread(th, nullptr);
    return ask;
}

void launcher_stamp(launcher_event ev) {
    static std::atomic<unsigned> logged{0};
    static const char* const k_name[LAUNCHER_EV_COUNT] = {
        "window_shown", "first_present", "scan_start", "scan_end", "list_shown"};
    if ((unsigned)ev >= LAUNCHER_EV_COUNT) return;
    const unsigned bit = 1u << (unsigned)ev;
    if (logged.fetch_or(bit) & bit) return;
    OPL_INFO_F("launcher", "stamp %s %.1f ms after process start", k_name[ev],
               (double)(op_now() - k_process_start_ns) / 1e6);
}

const openpete_launcher_game* launcher_game(void) { return s.game; }

const char* launcher_dropped_path(void) { return s.drop_path; }

bool launcher_neutral(void) { return !s.game || !s.game->game_id; }

const char* launcher_scripted_drop(void) {
    if (s.scripted_taken) return nullptr;
    const char* e = getenv("OPENPETE_LAUNCHER_DROP");
    if (!e || !*e || !scripted_due()) return nullptr;
    s.scripted_taken = true;
    snprintf(s.drop_path, sizeof s.drop_path, "%s", e);
    OPL_INFO_F("launcher", "scripted drop: %s", s.drop_path);
    return s.drop_path;
}

int launcher_select(const char* const* labels, const char* const* games, int count) {
    const bool screen = s.ui && !s.handed_off && count > 0 && launcher_open(s.opts, 1) == 0;
    if (!screen) return launcher_scripted_drop() ? LAUNCHER_SEL_DROPPED : LAUNCHER_SEL_NOSCREEN;
    log_boxless_entries(labels, games, count);
    const int r = page_loop(games, count, scripted_pick(games, count));
    launcher_page_closed();
    return r;
}

void openpete_launcher_begin(const openpete_launcher_game* game,
                             const openpete_options_t* neutral, int ui) {
    s.game = game;
    s.opts = neutral;
    s.ui = ui != 0;
}

int openpete_launcher_resolve(openpete_options_t* opts, char* module_path,
                              size_t module_cap, int* exit_now) {
    *exit_now = 0;
    int rc = library_resolve(opts, module_path, module_cap);
    if (rc == LAUNCHER_LIB_QUIT) {
        launcher_jobs_stop_other(nullptr, false);
        *exit_now = 1;
        return OPENPETE_EXIT_OK;
    }
    if (rc == LAUNCHER_LIB_BUILD_FAILED) {
        openpete_fatal_message(OP_E_MODULE, "OpenPete could not start",
                               "The game code could not be built from the disc.");
        return OPENPETE_EXIT_SETUP_ERROR;
    }
    if (rc != 0) {
        openpete_fatal_message(OP_E_LIBRARY, "OpenPete could not start",
                               "The game library could not be read.");
        return 1;
    }
    if (opts->disc_path || opts->exe_path) return OPENPETE_EXIT_OK;
    /* Nothing to boot anywhere: first run, the drop screen. With no device
     * there is no screen and no disc, so the run ends after the message.
     * Without screens only the scripted drop names a disc. */
    const char* dropped = nullptr;
    if (!s.ui) {
        dropped = launcher_scripted_drop();
        if (!dropped) return OPENPETE_EXIT_OK;
    } else {
        if (launcher_open(opts, 0) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        dropped = wait_for_disc();
        if (!dropped) {   /* window closed */
            launcher_jobs_stop_other(nullptr, false);
            *exit_now = 1;
            return OPENPETE_EXIT_OK;
        }
        if (launcher_neutral()) {
            /* The pick: the dropped disc is ingested and built, then the
             * library shows again. */
            library_ingest_drop(dropped);
            return openpete_launcher_resolve(opts, module_path, module_cap, exit_now);
        }
    }
    opts->ingest_path = dropped;
    if (library_resolve(opts, module_path, module_cap) != 0) {
        openpete_fatal_message(OP_E_INGEST, "OpenPete could not start",
                               "The disc image could not be added to the game library.");
        return 1;
    }
    return OPENPETE_EXIT_OK;
}

void openpete_launcher_progress(const char* stage, int done, int total) {
    snprintf(s.last_stage, sizeof s.last_stage, "%s", stage ? stage : "");
    if (!s.ui || s.handed_off) return;
    if (t_job) {
        /* A launcher job's thread: its own mailbox, drawn only when a pick
         * waits for the job (launcher_jobs_finish_game). */
        if (!s.lock) return;
        SDL_LockMutex(s.lock);
        snprintf(s_jobs.stage, sizeof s_jobs.stage, "%s", stage);
        s_jobs.sdone = done;
        s_jobs.stotal = total;
        s_jobs.have = true;
        SDL_UnlockMutex(s.lock);
        return;
    }
    if (s.task_active && s.lock && SDL_GetCurrentThreadID() != s.ui_tid) {
        /* Worker context: store only; every SDL video call stays on the
         * launcher's thread, whose runner loop draws the update. */
        SDL_LockMutex(s.lock);
        snprintf(s.stage, sizeof s.stage, "%s", stage);
        s.done = done;
        s.total = total;
        s.have_progress = true;
        SDL_UnlockMutex(s.lock);
        return;
    }
    /* The launcher's thread with no task: draw directly. The window opens
     * only when there is a wait to show, so a warm boot never flashes one. */
    if (launcher_open(s.opts, 1) != 0) return;
    View v;
    v.stage = stage;
    v.done = done;
    v.total = total;
    drain_events();
    present(v);
}

int openpete_launcher_run_task(openpete_launcher_task_fn fn, void* userdata) {
    if (!s.ui || s.handed_off) return fn(userdata);   /* headless or batch: inline */
    if (!s.lock) s.lock = SDL_CreateMutex();
    if (!s.lock) return fn(userdata);
    s.ui_tid = SDL_GetCurrentThreadID();
    TaskCtx ctx = {fn, userdata, -1};
    s.task_done = false;
    s.have_progress = false;
    s.task_active = true;
    SDL_Thread* th = SDL_CreateThread(task_main, "openpete-task", &ctx);
    if (!th) {
        s.task_active = false;
        return fn(userdata);   /* the worker could not start: run inline */
    }
    task_wait_loop();
    SDL_WaitThread(th, nullptr);
    s.task_active = false;
    return ctx.rc;
}

int openpete_launcher_run_task_exclusive(openpete_launcher_task_fn fn, void* userdata) {
    openpete_native_launcher_compile_pause(1);
    const int rc = openpete_launcher_run_task(fn, userdata);
    openpete_native_launcher_compile_pause(0);
    return rc;
}

int openpete_launcher_handoff(openpete_options_t* opts) {
    launcher_jobs_stop_other(nullptr, false);
    s_jobs.queue.clear();
    library_lock_release();
    launcher_page_release();
    carry_window_state(opts);
    close_gamepad();
    s.handed_off = true;
    if (s.quit_pending) {
        SDL_Event q = {};
        q.type = SDL_EVENT_QUIT;
        (void)SDL_PushEvent(&q);
        OPL_INFO_F("launcher", "%s", "a quit arrived during a wait; re-posted for the game");
    }
    if (opts->headless) return OPENPETE_EXIT_OK;
    if (launcher_open(opts, 1) != 0) {
        opts->native_unavailable = 1;
        return OPENPETE_EXIT_OK;
    }
    OPL_INFO_F("launcher", "%s", "window, device and imgui handed to the game");
    return OPENPETE_EXIT_OK;
}

int openpete_launcher_draw_progress(struct rhi_device* dev, const char* name,
                                    const char* stage, int done, int total) {
    View v;
    v.name = name;
    v.stage = stage ? stage : "";
    v.done = done;
    v.total = total;
    return present_on(dev, v, kShotPrecompile) ? 1 : 0;
}

void openpete_launcher_close(void) {
    launcher_jobs_stop_other(nullptr, false);
    s_jobs.queue.clear();
    library_lock_release();
    close_gamepad();
    openpete_native_launcher_close();
}

void openpete_launcher_vk_failure(const char* why, int continues) {
    const char* next = continues
        ? "The game starts on the OpenGL fallback display instead."
        : "Install the latest driver for your graphics card. OpenPete will now close.";
    OPL_ERROR_F("launcher", "no Vulkan device: %s; %s %s", why ? why : "",
                continues ? "continuing on PsyCross's GL window" : "exiting",
                openpete_error_ref(OP_E_VULKAN));
    if (!s.ui) return;
    if (const char* skip = getenv("OPENPETE_MSGBOX_SKIP"); skip && *skip && *skip != '0') {
        OPL_INFO_F("launcher", "%s", "message box suppressed (OPENPETE_MSGBOX_SKIP)");
        return;
    }
    /* Under gamescope (Steam Deck game mode) SDL's box is a zenity window
     * gamescope does not show, and the call blocks until the compositor
     * exits; the log line above is the player's report there. Gamescope
     * sets GAMESCOPE_WAYLAND_DISPLAY for every child it launches. */
    if (const char* gs = getenv("GAMESCOPE_WAYLAND_DISPLAY"); gs && *gs) {
        OPL_INFO_F("launcher", "message box skipped under gamescope "
                   "(GAMESCOPE_WAYLAND_DISPLAY=%s); the log holds the reason", gs);
        return;
    }
    char text[1024];
    snprintf(text, sizeof text,
             "OpenPete could not start Vulkan graphics.\n\n%s\n\n%s\n\nError %s, explained at:\n%s",
             why ? why : "", next, openpete_error_id(OP_E_VULKAN), openpete_error_url(OP_E_VULKAN));
    const bool shown = SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                                                "OpenPete could not start graphics", text, nullptr);
    OPL_INFO_F("launcher", "message box %s", shown ? "shown and dismissed" : "could not be shown");
}

namespace {

/** @brief Report whether a file can be opened for reading.
 *
 * @param p The path.
 * @return true when it opens. */
bool file_readable(const char* p) {
    FILE* f = fopen(p, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

/** @brief Run one job's module part: compute the game's stamped module path for the entry and build the module when no file is there.
 *
 * Runs with the job's game installed, on the job thread or inline.
 * @param job The job; receives built.
 * @return 0 when the module is current or built (or the game has no
 *         pipeline, or the job checks no module), the build's status otherwise. */
int job_body(Job& job) {
    job.built = false;
    const openpete_launcher_game* g = job.g;
    char path[4096];
    if (!job.module || !g->module_path || !g->build_module ||
        g->module_path(g, job.entry.c_str(), path, sizeof path) != 0)
        return 0;
    if (file_readable(path)) {
        OPL_INFO_F("launcher", "launch-time check: the module of %s is current (%s)", g->game_id, path);
        return 0;
    }
    OPL_INFO_F("launcher", "launch-time module rebuild of %s for entry %s: %s", g->game_id,
               job.entry.c_str(), path);
    job.built = true;
    return g->build_module(g, job.disc.c_str(), path);
}

/** @brief Record a finished job's result in its entry: a failed build writes the failed-build marker, a completed build clears one.
 *
 * A stopped job (-2) records nothing.
 * @param job The finished job. */
void job_record(const Job& job) {
    if (job.rc == -2) {
        OPL_INFO_F("launcher", "module rebuild of %s stopped", job.g->game_id);
    } else if (job.rc != 0) {
        library_marker_write(job.entry.c_str(), s.last_stage);
        OPL_ERROR_F("launcher", "module rebuild of %s failed at %s; see the log file %s", job.g->game_id,
                    s.last_stage[0] ? s.last_stage : "its start", "openpete.log");
    } else if (job.built) {
        library_marker_clear(job.entry.c_str());
        OPL_INFO_F("launcher", "module rebuild of %s done", job.g->game_id);
    }
}

/** @brief Job thread entry: run the current job's module part and flag it done.
 *
 * @param p Unused.
 * @return 0. */
int SDLCALL job_main(void* p) {
    (void)p;
    t_job = true;
    t_cancel = &s_jobs.cancel;
    s_jobs.cur.rc = job_body(s_jobs.cur);
    t_cancel = nullptr;
    t_job = false;
    SDL_SetAtomicInt(&s_jobs.done, 1);
    return 0;
}

}  // namespace

/** @brief Run the current asset job (openpete_launcher_game.update_assets) in the `ingest` role.
 *
 * @return The job's status. */
static int ingest_body() OP_REQUIRES(ingest) {
    return s_ing.g->update_assets(s_ing.g, s_ing.disc.c_str(), s_ing.sha.c_str(), s_ing.context.c_str());
}

namespace {

/** @brief Ingest thread entry: bind `ingest`, run the asset job with its stop flag and flag it done.
 *
 * The form without screens calls it directly inside an enter of the role
 * (ingest_run_inline).
 * @param p Unused.
 * @return 0. */
int SDLCALL ingest_main(void* p) {
    (void)p;
    OP_ROLE_BIND(ingest);
    SDL_AtomicInt* const prior = t_cancel;
    t_cancel = s_ing.cancel;
    s_ing.rc = ingest_body();
    t_cancel = prior;
    SDL_SetAtomicInt(&s_ing.done, 1);
    return 0;
}

/** @brief Run the asset job on the calling thread inside the `ingest` role: the form without screens, and the fallback when the ingest thread cannot start. */
void ingest_run_inline() {
    unsigned prior = 0;
    OP_ROLE_ENTER(ingest, prior);
    (void)ingest_main(nullptr);
    op_role_leave(prior);
}

/** @brief Start a game's asset job: on the ingest thread while screens show, inline otherwise.
 *
 * The game must be installed and no asset job busy; a game without
 * launcher assets starts nothing.
 * @param g The game's per-game entry.
 * @param disc The library entry's disc.cue.
 * @param sha The entry's recorded content hash.
 * @param context What starts it: "launch-time", "ingest" or "pick".
 * @param cancel The stop flag the job reads, or NULL for its own (never set
 *        by any stop). */
void ingest_start(const openpete_launcher_game* g, const std::string& disc, const std::string& sha,
                  const char* context, SDL_AtomicInt* cancel) {
    if (!g->update_assets || s_ing.busy) return;
    s_ing.g = g;
    s_ing.disc = disc;
    s_ing.sha = sha;
    s_ing.context = context;
    s_ing.cancel = cancel ? cancel : &s_ing.own_cancel;
    SDL_SetAtomicInt(&s_ing.own_cancel, 0);
    SDL_SetAtomicInt(&s_ing.done, 0);
    s_ing.rc = 0;
    s_ing.busy = true;
    s_ing.th = launcher_screens() ? SDL_CreateThread(ingest_main, "openpete-ingest", nullptr) : nullptr;
    if (!s_ing.th) ingest_run_inline();
}

/** @brief On the launcher's thread: reap a finished asset job and log its outcome, the moment its assets are ready.
 *
 * Polled by the job runner, the task wait loop and launcher_assets_wait;
 * a no-op while no asset job is busy or the busy one has not ended. */
void ingest_poll() {
    if (!s_ing.busy || !SDL_GetAtomicInt(&s_ing.done)) return;
    if (s_ing.th) SDL_WaitThread(s_ing.th, nullptr);
    s_ing.th = nullptr;
    s_ing.busy = false;
    const int rc = s_ing.rc;
    const char* what = rc == 0 ? "ready" : rc == 1 ? "kept (a failed marker of this build)"
                     : rc == -2 ? "not updated (stopped)" : "failed";
    OPL_INFO_F("launcher", "launcher assets of %s %s (%s)", s_ing.g->game_id, what, s_ing.context.c_str());
    if (rc == 0 || rc == 1) launcher_sky_assets_ready(s_ing.g->game_id);
    if (rc == 0 || rc == 1) launcher_title_offer(s_ing.g, s_ing.context != "launch-time", "assets ready");
}

bool job_finished();

/** @brief Wait for the running job's two parts to flag done, keeping the window's events drained and, when asked, drawing the job's progress.
 *
 * @param draw true to draw the job's last progress report every 16 ms. */
void job_wait(bool draw) {
    while (!job_finished()) {
        if (s.ui && openpete_native_launcher_active()) {
            drain_events();
            if (draw) {
                char stage[sizeof s_jobs.stage];
                int d = 0, t = 0;
                SDL_LockMutex(s.lock);
                const bool have = s_jobs.have;
                snprintf(stage, sizeof stage, "%s", s_jobs.stage);
                d = s_jobs.sdone;
                t = s_jobs.stotal;
                SDL_UnlockMutex(s.lock);
                View v;
                v.stage = have ? stage : "PREPARING THE GAME";
                v.done = d;
                v.total = t;
                present(v);
            }
        }
        SDL_Delay(16);
    }
}

/** @brief Report whether the running job's module part and asset part have both ended, reaping a finished asset part.
 *
 * @return true when both have ended. */
bool job_finished() {
    ingest_poll();
    return !s_ing.busy && (!s_jobs.th || SDL_GetAtomicInt(&s_jobs.done));
}

/** @brief Join the finished job's threads, end its in-flight count and its compile pause, and record its result. */
void job_reap() {
    if (s_jobs.th) SDL_WaitThread(s_jobs.th, nullptr);
    s_jobs.th = nullptr;
    if (s_jobs.cur.assets) s_jobs.cur.arc = s_ing.rc;
    openpete_native_launcher_compile_pause(0);
    s_jobs.in_flight--;
    s_jobs.cur.running = false;
    job_record(s_jobs.cur);
}

/** @brief Start the queued job at the head: install its game, count it in flight, pause the pipeline compile, and start its asset part on the ingest thread and its module part on the job thread. */
void job_start() {
    s_jobs.cur = s_jobs.queue.front();
    s_jobs.queue.pop_front();
    Job& j = s_jobs.cur;
    j.g->install(j.g);
    s_jobs.in_flight++;
    j.running = true;
    j.rc = j.arc = 0;
    j.assets = j.g->update_assets != nullptr;
    openpete_native_launcher_compile_pause(1);
    SDL_SetAtomicInt(&s_jobs.done, 0);
    SDL_SetAtomicInt(&s_jobs.cancel, 0);
    SDL_LockMutex(s.lock);
    s_jobs.have = false;
    SDL_UnlockMutex(s.lock);
    if (j.assets) ingest_start(j.g, j.disc, j.sha, "launch-time", &s_jobs.cancel);
    if (j.module && j.g->build_module && j.g->module_path) {
        s_jobs.th = SDL_CreateThread(job_main, "openpete-job", nullptr);
        if (!s_jobs.th) {
            /* The thread could not start: run the module part here, as without screens. */
            t_job = true;
            t_cancel = &s_jobs.cancel;
            j.rc = job_body(j);
            t_cancel = nullptr;
            t_job = false;
        }
    }
    OPL_INFO_F("launcher", "launch-time job started: %s", j.g->game_id);
}

}  // namespace

void launcher_assets_start(const openpete_launcher_game* g, const char* disc, const char* sha, const char* context) {
    ingest_start(g, disc ? disc : "", sha ? sha : "", context, nullptr);
}

void openpete_launcher_preview_progress(int done, int total) {
    s_preview_done.store(done);
    s_preview_total.store(total);
}

void openpete_launcher_picture_changed(void) { s_picture_gen.fetch_add(1); }

unsigned launcher_picture_gen(void) { return s_picture_gen.load(); }

void launcher_assets_wait(void) {
    bool held = false;
    while (s_ing.busy) {
        ingest_poll();
        if (!s_ing.busy) break;
        if (s.ui && openpete_native_launcher_active()) {
            drain_events();
            const int total = s_preview_total.load();
            if (total > 0) {
                /* The build ended before the box picture: hold the progress
                 * screen on the render's step until the job ends (its time
                 * bound ends it). The presents also carry the render's draw. */
                if (!held) OPL_INFO_F("launcher", "%s", "the progress screen holds for the preview render");
                held = true;
                View v;
                v.stage = "RENDERING PREVIEW";
                v.done = s_preview_done.load();
                v.total = total;
                present(v);
            }
        }
        SDL_Delay(5);
    }
    if (held) OPL_INFO_F("launcher", "%s", "the preview hold ends");
}

void launcher_job_add(const openpete_launcher_game* g, const char* entry_dir, const char* disc, const char* sha,
                      bool module) {
    Job job;
    job.g = g;
    job.entry = entry_dir;
    job.disc = disc;
    job.sha = sha ? sha : "";
    job.module = module;
    s_jobs.queue.push_back(job);
    OPL_INFO_F("launcher", "launch-time job queued: %s%s of %s (entry %s)", module ? "module check" : "",
               g->update_assets ? (module ? " and asset cache check" : "asset cache check") : "", g->game_id, entry_dir);
}

void launcher_jobs_pump(void) {
    if (s_jobs.cur.running && job_finished()) job_reap();
    if (s_jobs.cur.running || s_jobs.queue.empty() || !s.ui || s.handed_off) return;
    if (!s.lock) s.lock = SDL_CreateMutex();
    if (!s.lock) return;
    job_start();
}

void launcher_jobs_run_inline(void) {
    while (!s_jobs.queue.empty()) {
        Job job = s_jobs.queue.front();
        s_jobs.queue.pop_front();
        job.g->install(job.g);
        s_jobs.in_flight++;
        SDL_SetAtomicInt(&s_jobs.cancel, 0);
        if (job.g->update_assets) {
            ingest_start(job.g, job.disc, job.sha, "launch-time", &s_jobs.cancel);
            launcher_assets_wait();
        }
        t_job = true;
        t_cancel = &s_jobs.cancel;
        job.rc = job_body(job);
        t_cancel = nullptr;
        t_job = false;
        s_jobs.in_flight--;
        job_record(job);
    }
}

void launcher_jobs_stop_other(const char* keep_game, bool requeue) {
    if (!s_jobs.cur.running) return;
    if (keep_game && !strcmp(s_jobs.cur.g->game_id, keep_game)) return;
    OPL_INFO_F("launcher", "stopping the launch-time job of %s%s", s_jobs.cur.g->game_id,
               requeue ? " (requeued)" : "");
    SDL_SetAtomicInt(&s_jobs.cancel, 1);
    job_wait(false);
    job_reap();
    if (requeue && (s_jobs.cur.rc == -2 || s_jobs.cur.arc == -2)) s_jobs.queue.push_front(s_jobs.cur);
}

void launcher_jobs_finish_game(const char* game) {
    for (auto it = s_jobs.queue.begin(); it != s_jobs.queue.end();)
        it = strcmp(it->g->game_id, game) ? std::next(it) : s_jobs.queue.erase(it);
    if (!s_jobs.cur.running || strcmp(s_jobs.cur.g->game_id, game)) return;
    OPL_INFO_F("launcher", "the pick of %s waits for its running launch-time job", game);
    if (s.ui && !s.handed_off) (void)launcher_open(s.opts, 1);
    job_wait(true);
    job_reap();
}

bool launcher_screens(void) { return s.ui && !s.handed_off; }

bool launcher_jobs_idle(void) { return !s_jobs.cur.running && s_jobs.queue.empty(); }

void launcher_jobs_token(int delta) { s_jobs.in_flight += delta; }

const char* launcher_last_stage(void) { return s.last_stage; }

int openpete_launcher_job_cancelled(void) {
    return t_cancel && SDL_GetAtomicInt(t_cancel) ? 1 : 0;
}

int openpete_launcher_jobs_in_flight(void) { return s_jobs.in_flight.load(); }
