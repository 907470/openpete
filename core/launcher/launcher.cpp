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
// click=<x>/<y>, click=@<element id>, dblclick and quit post the input events, maximize, restore and resize=WxH call the
// window operations a window manager answers, and key=, pad=, stick= and
// mouse= post key, gamepad and pointer events (scripted_event). OPENPETE_LAUNCHER_MANIFEST=<base>
// writes each screen's first frame's elements, by the reference layout's ids, to
// <base>.<screen>.json (launcher_manifest.cpp). The contrast check's harness
// variables: OPENPETE_LAUNCHER_CLEAR=r,g,b replaces the clear colour, and
// OPENPETE_LAUNCHER_TEXT_HIDE=1 draws imgui's output at alpha 0, so a shot
// holds the background and the shape pass alone.
#include "launcher_internal.h"
#include "launcher_pace.h"
#include "launcher_picker.h"
#include "launcher_shapes.h"
#include "launcher_chips.h"
#include "launcher_progress.h"
#include "launcher_sky.h"
#include "launcher_quit.h"

#include "imgui.h"

#include <SDL3/SDL.h>

#include "host_spawn.h"
#include "openpete_devui.h"
#include "openpete_fatal.h"
#include "openpete_input_lock.h"
#include "openpete_launcher_fit.h"
#include "openpete_log.h"
#include "openpete_native_render.h"
#include "openpete_time.h"
#include "openpete_window_restack.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#ifdef __linux__
#include <sys/resource.h>
#endif

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
    launcher_picker_result picked = LAUNCHER_PICKER_STAYS;   /* what a click in the drawn picker did */
    bool clicked = false;                 /* the left mouse button was pressed this frame */
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
    SDL_AtomicInt dialog_failed = {};   /* the dialog's callback reported a failure: the drawn picker opens */
    unsigned shot_done = 0;    /* one bit per Screen already read back */
    /* Task runner mailbox: the worker writes stage/done/total under lock. */
    SDL_Mutex* lock = nullptr;
    bool task_active = false;
    bool task_done = false;
    bool have_progress = false;
    bool fail_deferred = false;  /* the device failed; its message waits for the next open */
    bool panel_only = false;     /* the page draws the message panel and no box (openpete_launcher_refuse) */
    bool no_precompile = false;  /* the window opens without the pipeline precompile */
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

/* The progress view's step line top, in layout px: the place the step chip
 * and the game page's quit chip draw at. */
constexpr float kStepChipY = 196 + kBandY;
/* A centred chip's padding around its line's box, layout px. */
constexpr float kChipPadX = 12.0f, kChipPadY = 7.0f;

/** @brief Draw one line of text horizontally centred in the layout on a chip (launcher_chip_text): the box, then the text.
 *
 * The line's origin is its centred position unrounded (imgui truncates it
 * when it draws); the chip spans the line's box grown by kChipPadX x
 * kChipPadY layout px, and the text records as @p id, the chip as
 * <id>.panel.
 * @param f The frame mapping.
 * @param id The text's manifest id.
 * @param y The line's top in layout pixels.
 * @param size The text height in layout pixels, as ProggyClean (push_body_font).
 * @param col The text colour.
 * @param text The text. */
void chip_centred(const Frame& f, const char* id, float y, float size, ImU32 col, const char* text) {
    push_body_font(size * f.scale);
    const std::string t = launcher_chip_case(text);
    const ImVec2 sz = ImGui::CalcTextSize(t.c_str());
    const ImVec2 at(f.ox + (kLayoutW * f.scale - sz.x) * 0.5f, f.oy + y * f.scale);
    launcher_chip_text(id, at, ImVec2(kChipPadX, kChipPadY), f.scale, col, t.c_str());
    ImGui::PopFont();
}

/* The progress bar (layout px): its left inset, top below the band's top,
 * height and border; light track, dark border, orange fill. */
constexpr float kBarX = 80.0f, kBarY = 240.0f, kBarH = 22.0f, kBarBorder = 2.0f;

/** @brief Draw the quit ask's chip, "Press again to quit", at the step chip's place: white text on the step chip's box (chip_centred).
 *
 * @param f The frame mapping. */
void draw_quit_ask(const Frame& f) {
    chip_centred(f, "quit.ask", kStepChipY, 14, IM_COL32_WHITE, "Press again to quit");
}

/** @brief Draw the progress screen: the step chip and the bar below it, with no banner, footer or panel.
 *
 * The step chip reads the stage's step line (launcher_progress_line: its
 * step name in sentence case, a counted step's counts in parentheses). The
 * bar (launcher_shape_bar) fills to launcher_progress_bar's fraction: the
 * first boot's stages as one bar weighted by their measured time, any
 * other stage by its own count; light track, dark border, orange fill.
 * @param f The frame mapping.
 * @param v The view. */
void draw_progress(const Frame& f, const View& v) {
    char line[160];
    launcher_progress_line(v.stage, v.done, v.total, line, sizeof line);
    /* While East's quit ask shows (launcher_quit_gesture) the quit chip
     * takes the step chip's place. */
    static bool armed_logged = false;
    const bool armed = launcher_quit_armed();
    if (armed && !armed_logged) OPL_INFO_F("launcher", "%s", "progress view: the quit chip reads Press again to quit");
    armed_logged = armed;
    if (armed) draw_quit_ask(f);
    else chip_centred(f, "step", kStepChipY, 14, IM_COL32_WHITE, line);
    const float frac = launcher_progress_bar(v.stage, v.done, v.total);
    const ImVec2 a(f.ox + kBarX * f.scale, f.oy + (kBarY + kBandY) * f.scale);
    const ImVec2 b(f.ox + (kLayoutW - kBarX) * f.scale, a.y + kBarH * f.scale);
    launcher_shape_bar("bar", a, b, f.scale, frac, IM_COL32(150, 130, 190, 255), IM_COL32(240, 138, 30, 255),
                       IM_COL32(50, 38, 80, 255), kBarBorder);
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
 * imgui's NewFrame and Render. Before the hand-off and outside the drawn
 * picker, the corner chips (launcher_corner_chips_draw) draw before the
 * content, the shader chip left out on the shader step.
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
    if (!s.handed_off && !launcher_picker_shown())
        launcher_corner_chips_draw(f.scale, v.screen == Screen::Progress && v.stage &&
                                                strncmp(v.stage, "COMPILING SHADERS", 17) == 0);
    if (v.screen == Screen::Progress) draw_progress(f, v);
    else if (v.screen == Screen::Pending) draw_pending(f);
    else if (launcher_picker_shown()) {
        v.picked = launcher_picker_draw(f.scale, f.ox, f.oy);
    } else {
        if (!s.panel_only) v.activated = launcher_page_draw(v.entry_games, v.count, v.focus, &v.hovered);
        if (v.quit_asked) draw_quit_ask(f);
        const bool link = launcher_panel_draw(
            f.scale, s.panel_only ? 0.45f * io.DisplaySize.y
                                  : launcher_page_row_bottom(io.DisplaySize.x, io.DisplaySize.y));
        v.clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !link;
    }
    s.item_hovered = ImGui::IsAnyItemHovered();
    ImGui::End();
}

/* Shot tags, indexed by Screen, then the precompile's progress screen, then
 * the first frames showing the sky's opaque dome, the 3D title, the drawn
 * picker and the message panel over a full dome. */
constexpr const char* kShotTag[] = {"progress", "drop", "selector", "pending", "precompile", "sky", "title3d",
                                    "picker", "panel"};
constexpr int kShotPrecompile = 4;
constexpr int kShotSky = 5;   /* the first frame whose sky is one opaque dome */
constexpr int kShotTitle = 6;   /* the first frame drawing the 3D title */
constexpr int kShotPicker = 7;  /* the first frame drawing the drawn picker */
constexpr int kShotPanel = 8;   /* the first frame drawing the message panel's fill at full dome cover */

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
void present(View& v, bool may_wait = true);

/** @brief Present one frame of a view on the launcher's device at the present rate, naming the launcher's game, then run the harness input that is due.
 *
 * None runs after a quit was taken (the window is hidden while the work
 * stops); otherwise launcher_pace_begin decides the present: none while the
 * window cannot show, else one per refresh period, and the harness input
 * runs either way.
 * @param v The view.
 * @param may_wait true to wait out the refresh period; false skips a
 *        present that comes before it (a caller doing the work itself). */
void present(View& v, bool may_wait) {
    if (launcher_quit_taken()) return;   /* the window is hidden while the work stops */
    if (!launcher_pace_begin(s.presented, may_wait)) {
        scripted_input_poll();
        return;
    }
    v.name = game_name();
    launcher_sky_present(op_now(), s.presented);   /* the present's instant: the sky's motion follows it */
    if (!present_on(nullptr, v, (int)v.screen)) return;
    launcher_pace_done();
    s.presented = true;
    if (launcher_sky_opaque()) {
        maybe_shot(nullptr, kShotSky);
        const ImVec2 d = ImGui::GetIO().DisplaySize;
        launcher_manifest_write((unsigned)kShotSky, kShotTag[kShotSky], d.x, d.y);
    }
    if (launcher_panel_full()) {
        maybe_shot(nullptr, kShotPanel);
        const ImVec2 d = ImGui::GetIO().DisplaySize;
        launcher_manifest_write((unsigned)kShotPanel, kShotTag[kShotPanel], d.x, d.y);
    }
    if (launcher_title_drawn(nullptr)) {
        maybe_shot(nullptr, kShotTitle);
        const ImVec2 d = ImGui::GetIO().DisplaySize;
        launcher_manifest_write((unsigned)kShotTitle, kShotTag[kShotTitle], d.x, d.y);
    }
    if (launcher_picker_shown()) {
        maybe_shot(nullptr, kShotPicker);
        const ImVec2 d = ImGui::GetIO().DisplaySize;
        launcher_manifest_write((unsigned)kShotPicker, kShotTag[kShotPicker], d.x, d.y);
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
 * game window size is set: the Steam Deck's panel, in window coordinates
 * (scaled by the display's content scale where those are pixels,
 * launcher_design_scale). */
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
 * @param open Receives the OPENPETE_WINDOW_OPEN_* bits.
 * @return true when the size is the 1280x800 design size (no size is configured), false for a configured or game size. */
bool launcher_window_request(const openpete_options_t* opts, int* w, int* h, int* open) {
    *open = 0;
    bool design = false;
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
        design = true;
    }
    if (opts->launcher_window_maximized > 0 ||
        (opts->launcher_window_maximized < 0 && opts->window_maximized > 0))
        *open |= OPENPETE_WINDOW_OPEN_MAXIMIZED;
    if (opts->launcher_fullscreen > 0) *open |= OPENPETE_WINDOW_OPEN_FULLSCREEN;
    return design;
}

/* True where SDL window coordinates are pixels and the display's content
 * scale is not applied to them: Windows. On Wayland and macOS the window
 * coordinates already carry the scale; X11 keeps its unscaled size. */
#ifdef _WIN32
constexpr bool kScaleDesignSize = true;
#else
constexpr bool kScaleDesignSize = false;
#endif

/** @brief Read the primary display's content scale, or the harness variable OPENPETE_LAUNCHER_CONTENT_SCALE=<f> when it is set.
 *
 * @return The scale; 0 when SDL cannot read it. */
float launcher_content_scale() {
    if (const char* e = getenv("OPENPETE_LAUNCHER_CONTENT_SCALE"); e && *e) return strtof(e, nullptr);
    const SDL_DisplayID d = SDL_GetPrimaryDisplay();
    return d ? SDL_GetDisplayContentScale(d) : 0.0f;
}

/** @brief Scale the launcher's design size by the primary display's content scale on Windows, so it opens at the same apparent size at 150% or 200% as at 100%; elsewhere leave it as it is.
 *
 * Only the design size is scaled: a --launcher-window or --window size is
 * taken as given (pixels on Windows). The harness variable
 * OPENPETE_LAUNCHER_CONTENT_SCALE=<f> gives the scale instead of
 * SDL_GetDisplayContentScale. The scale and the size requested are logged
 * on every platform.
 * @param w The design width; receives the requested width.
 * @param h The design height; receives the requested height. */
void launcher_design_scale(int* w, int* h) {
    const float scale = launcher_content_scale();
    const int dw = *w, dh = *h;
    if (kScaleDesignSize) openpete_launcher_scale_size(w, h, scale);
    OPL_INFO_F("launcher", "window scale: content scale %.2f, design size %dx%d: requests %dx%d (%s)",
               (double)scale, dw, dh, *w, *h,
               kScaleDesignSize ? "scaled, window coordinates are pixels"
                                : "unscaled, the window coordinates decide");
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

/* The fit taken before the window exists, kept for the check against the
 * decorations the window reports once created (launcher_window_refit). */
struct WindowFit {
    bool active = false;  /* a fit decision was taken (not gamescope, not maximized or fullscreen) */
    int rw = 0, rh = 0;   /* the requested client size */
    int dw = 0, dh = 0;   /* the decorations the decision counted */
    DisplayBounds b{};
    openpete_launcher_fit_how how = OPENPETE_LAUNCHER_FIT_AS_REQUESTED;
};
WindowFit s_fit;

/** @brief Name a fit decision for the log.
 * @param how The decision.
 * @return Its log text. */
const char* fit_how_text(openpete_launcher_fit_how how) {
    switch (how) {
    case OPENPETE_LAUNCHER_FIT_MAXIMIZED: return "maximized";
    case OPENPETE_LAUNCHER_FIT_CLAMPED: return "clamped to 90% of the usable bounds";
    case OPENPETE_LAUNCHER_FIT_AS_REQUESTED: break;
    }
    return "as requested";
}

/** @brief Read the decorations the launcher window will add before it exists: the harness variable OPENPETE_LAUNCHER_DECORATION_STUB=<W>x<H>, else the platform estimate (openpete_window_decoration_estimate; Win32 only, 0 elsewhere).
 *
 * @param dw Receives the decorations' total width.
 * @param dh Receives the decorations' total height.
 * @return Where the size came from, for the log. */
const char* decoration_estimate(int* dw, int* dh) {
    *dw = *dh = 0;
    if (const char* e = getenv("OPENPETE_LAUNCHER_DECORATION_STUB"); e && *e)
        return sscanf(e, "%dx%d", dw, dh) == 2 ? "stubbed" : "unreadable stub, none";
    return openpete_window_decoration_estimate(launcher_content_scale(), dw, dh)
               ? "estimated from the frame style"
               : "not known before the window maps";
}

/** @brief Fit the launcher window's requested client size with its decorations to the display: as requested when the outer size fits the usable bounds, maximized when it fits only the full bounds, else clamped to 90% of the usable bounds at the request's aspect; under gamescope the output size.
 *
 * Sizes are SDL window coordinates: on Wayland and macOS a request opens
 * at the same apparent size on a scaled display as on an unscaled one; on
 * Windows they are pixels, and the design size arrives scaled
 * (launcher_design_scale). The decorations are decoration_estimate's: the
 * frame and title bar on Windows, 0 where the size is not known before the
 * window maps (X11, Wayland, macOS), where the client size alone is
 * compared. The rule is openpete_launcher_fit. A request already maximized
 * or fullscreen, or a display whose bounds cannot be read, is left as it
 * is. Gamescope sets GAMESCOPE_WAYLAND_DISPLAY for every child it
 * launches; its output is the display's full bounds. The decision is
 * logged.
 * @param w The requested width; receives the width the window opens at.
 * @param h The requested height; receives the height.
 * @param open The OPENPETE_WINDOW_OPEN_* bits; gains MAXIMIZED when the
 *             outer size fits only the full bounds. */
void launcher_window_fit(int* w, int* h, int* open) {
    DisplayBounds b;
    s_fit.active = false;
    if ((*open & (OPENPETE_WINDOW_OPEN_MAXIMIZED | OPENPETE_WINDOW_OPEN_FULLSCREEN)) ||
        !display_bounds(&b) || b.use_w <= 0 || b.use_h <= 0)
        return;
    const int rw = *w, rh = *h;
    int dw = 0, dh = 0;
    const char* from = "none under gamescope";
    const char* how = "at the gamescope output size";
    if (const char* gs = getenv("GAMESCOPE_WAYLAND_DISPLAY"); gs && *gs) {
        *w = b.full_w;
        *h = b.full_h;
    } else {
        from = decoration_estimate(&dw, &dh);
        const openpete_launcher_fit_how fit =
            openpete_launcher_fit(rw, rh, dw, dh, b.full_w, b.full_h, b.use_w, b.use_h, w, h);
        if (fit == OPENPETE_LAUNCHER_FIT_MAXIMIZED) *open |= OPENPETE_WINDOW_OPEN_MAXIMIZED;
        how = fit_how_text(fit);
        s_fit = {true, rw, rh, dw, dh, b, fit};
    }
    OPL_INFO_F("launcher", "window fit: request %dx%d, display %dx%d, usable %dx%d: opens %s (%dx%d); "
               "decorations %dx%d (%s), outer %dx%d",
               rw, rh, b.full_w, b.full_h, b.use_w, b.use_h, how, *w, *h, dw, dh, from, *w + dw, *h + dh);
}

/** @brief Check the fit against the decorations the created window reports (SDL_GetWindowBordersSize) and correct the window when they change the decision: maximize it, or resize it to the clamp.
 *
 * Runs once, right after the window is created, when launcher_window_fit
 * took a decision. SDL measures the decorations on Windows and X11; Wayland
 * and macOS report none, and X11 reports 0 until the window manager's frame
 * extents arrive, so a measurement of 0 is no measurement and leaves the
 * window as it is. On Windows the measurement is the frame the estimate
 * computed, so a correction there means the window opened on a display
 * whose DPI differs from the primary's. The measurement and any correction
 * are logged.
 * @param w The width the window opened at; receives the corrected width.
 * @param h The height; receives the corrected height.
 * @param open The OPENPETE_WINDOW_OPEN_* bits; gains MAXIMIZED when the
 *             correction maximizes. */
void launcher_window_refit(int* w, int* h, int* open) {
    if (!s_fit.active || (*open & OPENPETE_WINDOW_OPEN_MAXIMIZED)) return;
    SDL_Window* win = openpete_native_launcher_window();
    int top = 0, left = 0, bottom = 0, right = 0;
    if (!win || !SDL_GetWindowBordersSize(win, &top, &left, &bottom, &right)) return;
    const int dw = left + right, dh = top + bottom;
    if (dw == 0 && dh == 0) return;
    int fw = 0, fh = 0;
    const openpete_launcher_fit_how fit = openpete_launcher_fit(
        s_fit.rw, s_fit.rh, dw, dh, s_fit.b.full_w, s_fit.b.full_h, s_fit.b.use_w, s_fit.b.use_h, &fw, &fh);
    const bool change = fit != s_fit.how || fw != *w || fh != *h;
    OPL_INFO_F("launcher", "window fit: measured decorations %dx%d (top %d, left %d, bottom %d, right %d), "
               "estimated %dx%d: %s%s",
               dw, dh, top, left, bottom, right, s_fit.dw, s_fit.dh, change ? "reopens " : "unchanged, ",
               fit_how_text(fit));
    if (!change) return;
    if (fit == OPENPETE_LAUNCHER_FIT_MAXIMIZED) {
        (void)openpete_native_launcher_window_op(OPENPETE_LAUNCHER_WINDOW_MAXIMIZE, 0, 0);
        *open |= OPENPETE_WINDOW_OPEN_MAXIMIZED;
    } else {
        (void)openpete_native_launcher_window_op(OPENPETE_LAUNCHER_WINDOW_RESIZE, fw, fh);
    }
    *w = fw;
    *h = fh;
    s_fit.how = fit;
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
 * The window takes launcher_window_request's size; before the hand-off the
 * design size is scaled (launcher_design_scale) and the size is fitted to
 * the display (launcher_window_fit). A device created for a screen, before the
 * hand-off, starts the pipeline precompile on it
 * (openpete_native_launcher_precompile_start) unless a refusal opened it
 * (openpete_launcher_refuse); one created at the hand-off leaves the
 * compile to bring-up.
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
    const bool design = launcher_window_request(opts, &w, &h, &open);
    if (!s.handed_off) {
        if (design) launcher_design_scale(&w, &h);
        launcher_window_fit(&w, &h, &open);
    }
    if (openpete_native_launcher_open(opts, w, h, open, "OpenPete") != 0) {
        open_failed(openpete_native_launcher_error(), continues);
        return -1;
    }
    if (!s.handed_off) launcher_window_refit(&w, &h, &open);
    window_state_init(opts, w, h, open);
    (void)launcher_body_font();
    launcher_stamp(LAUNCHER_EV_WINDOW);
    open_first_gamepad();
    if (!s.handed_off && !s.no_precompile) (void)openpete_native_launcher_precompile_start(opts);
    return 0;
}

/** @brief SDL file-dialog callback: store the chosen path and flag it, logging the outcome (chosen, cancelled or failed); a failure flags the drawn picker to open.
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
        SDL_SetAtomicInt(&s.dialog_failed, 1);
    }
    SDL_SetAtomicInt(&s.dialog_pending, 0);
}

/** @brief Choose a disc: under gamescope open the drawn picker (launcher_picker_only), else show the native open-file dialog for a .cue or .bin image unless one is open.
 *
 * The harness variable OPENPETE_LAUNCHER_DIALOG_FAIL=1 takes the dialog's
 * failure result without calling SDL, so the drawn picker opens as after a
 * real failure. */
void open_file_dialog() {
    if (launcher_picker_only()) {
        launcher_picker_open("gamescope: the file dialog is not shown there");
        return;
    }
    if (SDL_GetAtomicInt(&s.dialog_pending)) return;
    if (const char* f = getenv("OPENPETE_LAUNCHER_DIALOG_FAIL"); f && *f && *f != '0') {
        OPL_WARN_F("launcher", "file dialog: failed: forced (OPENPETE_LAUNCHER_DIALOG_FAIL)");
        SDL_SetAtomicInt(&s.dialog_failed, 1);
        return;
    }
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

/** @brief Harness: click the left button at a window point: post the pointer's motion there and the press, and build the release.
 *
 * A negative coordinate counts from the window's right or bottom edge.
 * @param win The launcher's window.
 * @param x The point's x, window coordinates.
 * @param y Its y.
 * @param e Receives the release. */
void scripted_click(SDL_Window* win, float x, float y, SDL_Event& e) {
    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);
    if (x < 0) x += (float)w;
    if (y < 0) y += (float)h;
    SDL_WarpMouseInWindow(win, x, y);
    SDL_Event m = {};
    m.type = SDL_EVENT_MOUSE_MOTION;
    m.motion.windowID = SDL_GetWindowID(win);
    m.motion.x = x;
    m.motion.y = y;
    (void)SDL_PushEvent(&m);
    SDL_Event d = {};
    d.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    d.button.windowID = m.motion.windowID;
    d.button.button = SDL_BUTTON_LEFT;
    d.button.down = true;
    d.button.clicks = 1;
    d.button.x = x;
    d.button.y = y;
    (void)SDL_PushEvent(&d);
    e = d;
    e.type = SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.down = false;
}

/** @brief Harness: build the event of an input action: a key or pad button (scripted_button_event), stick=<x|y>:<value>, mouse=<x>/<y>, click=<x>/<y>, click=@<id>, dblclick or quit.
 *
 * stick sets the left stick's axis to the value (-32768 to 32767). mouse
 * warps the pointer to a point in the 640x448 layout space of the frame
 * last drawn and builds no event (e.type stays 0): the platform then
 * reports the motion and the pointer's window, which imgui's backend reads
 * each frame. click is a left click at a window point (scripted_click);
 * click=@<id> clicks the centre of the element of that id in the last
 * frame's element manifest (launcher_manifest_find). dblclick is a left double-click at the bottom centre of the
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
    } else if (a.rfind("click=@", 0) == 0) {
        float r[4];
        if (!launcher_manifest_find(a.c_str() + 7, r)) {
            OPL_WARN_F("launcher", "scripted input: %s: the last frame drew no such element", a.c_str());
            return true;   /* e.type stays 0: no event */
        }
        scripted_click(win, 0.5f * (r[0] + r[2]), 0.5f * (r[1] + r[3]), e);
    } else if (sscanf(a.c_str(), "click=%f/%f", &x, &y) == 2) {
        scripted_click(win, x, y, e);
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

/** @brief Harness: call the window operation of a window action: maximize, restore, minimize, hide, show, resize=WxH or display=<Hz>.
 *
 * display=<Hz> stubs the refresh query and posts a display change of the
 * window, as a move to a display of that refresh does.
 * @param a The action; any other is ignored. */
void scripted_window_op(const std::string& a) {
    int w = 0, h = 0;
    if (a == "maximize")
        (void)openpete_native_launcher_window_op(OPENPETE_LAUNCHER_WINDOW_MAXIMIZE, 0, 0);
    else if (a == "restore")
        (void)openpete_native_launcher_window_op(OPENPETE_LAUNCHER_WINDOW_RESTORE, 0, 0);
    else if (sscanf(a.c_str(), "resize=%dx%d", &w, &h) == 2)
        (void)openpete_native_launcher_window_op(OPENPETE_LAUNCHER_WINDOW_RESIZE, w, h);
    else if (a == "minimize")
        (void)openpete_native_launcher_window_op(OPENPETE_LAUNCHER_WINDOW_MINIMIZE, 0, 0);
    else if (a == "hide")
        (void)openpete_native_launcher_window_op(OPENPETE_LAUNCHER_WINDOW_HIDE, 0, 0);
    else if (a == "show")
        (void)openpete_native_launcher_window_op(OPENPETE_LAUNCHER_WINDOW_SHOW, 0, 0);
    else if (sscanf(a.c_str(), "display=%d", &w) == 1)
        (void)openpete_native_launcher_window_op(OPENPETE_LAUNCHER_WINDOW_DISPLAY_STUB, w, 0);
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
/* Quit is immediate (the window closing) and so is Escape, except in the
 * drawn picker, which Escape closes; Back is the gamepad's east button, which
 * the game page asks once for and the other screens take as Quit. */
enum class Ask { None, Quit, Escape, Back, Pick, Browse, Up, Down, Left, Right, Dropped };

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
                {SDLK_ESCAPE, Ask::Escape}, {SDLK_RETURN, Ask::Pick}, {SDLK_KP_ENTER, Ask::Pick},
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

/* Commands of an input batch not yet returned by wait_input, oldest first. */
std::deque<Ask> s_asks;

/** @brief Return the next command of the input batches, waiting up to 100 ms for a batch only while the window cannot show, and stepping the launcher's precompile at each wake.
 *
 * While the window can show, the caller's present paces the loop at the
 * refresh (launcher_pace_begin) and the events already queued are taken
 * without a wait. Otherwise no present runs and this waits on events: a
 * returning compile job pushes a wake event
 * (openpete_native_launcher_precompile_event), and a batch of only wake
 * events steps the precompile and keeps waiting for the rest of the 100 ms.
 * Every pending event of a batch goes to imgui, and each command it carries
 * is queued in order; one is returned per call, so the caller presents
 * between commands and no press is lost behind a slow present. A quit
 * (Ask::Quit: a quit event, SIGTERM, the window closed) empties the queue
 * and is returned at once. The precompile is also stepped when the wait
 * ends.
 * @return The oldest queued command, the quit when a batch held one, or
 *         Ask::None. */
Ask wait_input() {
    const auto next = []() {
        const Ask a = s_asks.front();
        s_asks.pop_front();
        return a;
    };
    if (!s_asks.empty()) return next();
    const Uint32 wake = openpete_native_launcher_precompile_event();
    const bool paced = launcher_pace_visible();
    const Uint64 end = SDL_GetTicks() + (paced ? 0 : 100);
    for (;;) {
        const Uint64 now = SDL_GetTicks();
        SDL_Event e;
        const bool got = now < end ? SDL_WaitEventTimeout(&e, (Sint32)(end - now)) : SDL_PollEvent(&e);
        if (!got) {
            (void)openpete_native_launcher_precompile_step();
            return Ask::None;
        }
        bool input = false, quit = false;
        do {
            if (wake && e.type == wake) continue;
            input = true;
            openpete_devui_process_event(&e);
            const Ask a = ask_of(e);
            quit = quit || a == Ask::Quit;
            if (a != Ask::None) s_asks.push_back(a);
        } while (SDL_PollEvent(&e));
        (void)openpete_native_launcher_precompile_step();
        if (quit) {
            s_asks.clear();
            return Ask::Quit;
        }
        if (input || paced) return s_asks.empty() ? Ask::None : next();
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

/** @brief Harness: whether the scripted pick waits for the ahead walk and the walk has not reached the event named yet.
 *
 * OPENPETE_LAUNCHER_PICK_AFTER_WALK=end holds the pick while the ahead walk
 * runs; =<G> holds it while the walk runs and has not logged the return of
 * its first texture mapping's group G. The release logs "scripted pick
 * released: the ahead walk ended" or "...: group G returned" once.
 * @return true while the pick is held. */
bool scripted_walk_held() {
    const char* e = getenv("OPENPETE_LAUNCHER_PICK_AFTER_WALK");
    if (!e || !*e) return false;
    unsigned groups = 0;
    const bool runs = openpete_native_launcher_precompile_progress(&groups) != 0;
    const bool to_end = !strcmp(e, "end");
    const unsigned g = to_end ? 0u : (unsigned)strtoul(e, nullptr, 10);
    const bool back = !to_end && g < 32u && ((groups >> g) & 1u);
    if (runs && !back) return true;
    static bool told = false;
    if (!told) {
        told = true;
        if (back) OPL_INFO_F("launcher", "scripted pick released: group %u returned", g);
        else OPL_INFO_F("launcher", "%s", "scripted pick released: the ahead walk ended");
    }
    return false;
}

/** @brief Harness: whether the scripted pick waits for idle jobs or for the ahead walk, and that has not happened yet.
 *
 * @return true while OPENPETE_LAUNCHER_SELECT_IDLE is set and a launcher job
 *         runs or is queued, or while scripted_walk_held holds the pick. */
bool scripted_pick_held() {
    const char* e = getenv("OPENPETE_LAUNCHER_SELECT_IDLE");
    return (e && *e && *e != '0' && !launcher_jobs_idle()) || scripted_walk_held();
}

/* page_command's result while the page stays up. */
constexpr int kPageStays = -100;
/* panel_command's result for a command the panel lets through. */
constexpr int kPagePass = -101;

/** @brief Take a command while the message panel shows: it dismisses the panel and is consumed, except a quit, Escape and a dropped disc, which act after the dismissal; a panel whose dismissal quits turns every command into a quit.
 *
 * @param a The command (Ask::None for a click, with @p clicked).
 * @param clicked true when the command is a mouse click.
 * @return kPagePass when no panel shows or the command acts, kPageStays when it was consumed, or LAUNCHER_SEL_QUIT. */
int panel_command(Ask a, bool clicked) {
    if (!launcher_panel_shown() || (a == Ask::None && !clicked)) return kPagePass;
    static const char* const k_name[] = {"none", "quit", "escape", "east", "activate", "browse", "up", "down",
                                         "left", "right", "a dropped disc"};
    const bool quits = launcher_panel_quits();
    launcher_panel_dismiss(clicked ? "a click" : k_name[(int)a]);
    if (quits) return LAUNCHER_SEL_QUIT;
    return a == Ask::Quit || a == Ask::Escape || a == Ask::Dropped ? kPagePass : kPageStays;
}

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
        case Ask::Quit: case Ask::Escape:
            OPL_INFO_F("launcher", "command: quit (Escape, the window closed or a quit event such as SIGTERM)");
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

/** @brief Take what the drawn picker did: a chosen image becomes the drop path.
 *
 * @param r The picker's result.
 * @return LAUNCHER_SEL_DROPPED for a chosen image (its path in s.drop_path), else kPageStays. */
int picker_taken(launcher_picker_result r) {
    if (r != LAUNCHER_PICKER_CHOSEN) return kPageStays;
    snprintf(s.drop_path, sizeof s.drop_path, "%s", launcher_picker_chosen());
    return LAUNCHER_SEL_DROPPED;
}

/** @brief Act on one command while the drawn picker is open: up and down move its selection, Enter and South open the folder or choose the image, East and left go up (closing from the root list), Escape closes it, a dropped file closes it and is taken, the window closing quits.
 *
 * Space and West, and right, do nothing in it.
 * @param a The command.
 * @return LAUNCHER_SEL_DROPPED (the path in s.drop_path), LAUNCHER_SEL_QUIT, or kPageStays. */
int picker_command(Ask a) {
    switch (a) {
        case Ask::Quit:
            OPL_INFO_F("launcher", "command: quit (the window closed)");
            return LAUNCHER_SEL_QUIT;
        case Ask::Dropped:
            (void)launcher_picker_command(LAUNCHER_PICKER_CLOSE);
            return LAUNCHER_SEL_DROPPED;
        case Ask::Up: return picker_taken(launcher_picker_command(LAUNCHER_PICKER_UP));
        case Ask::Down: return picker_taken(launcher_picker_command(LAUNCHER_PICKER_DOWN));
        case Ask::Pick: return picker_taken(launcher_picker_command(LAUNCHER_PICKER_ACTIVATE));
        case Ask::Back: case Ask::Left: return picker_taken(launcher_picker_command(LAUNCHER_PICKER_BACK));
        case Ask::Escape: return picker_taken(launcher_picker_command(LAUNCHER_PICKER_CLOSE));
        default: return kPageStays;
    }
}

/** @brief Take the file dialog's result: a chosen path, or a failure, which opens the drawn picker.
 *
 * @return true when the dialog chose a path (in s.drop_path). */
bool dialog_poll() {
    if (SDL_GetAtomicInt(&s.dialog_failed)) {
        SDL_SetAtomicInt(&s.dialog_failed, 0);
        launcher_picker_open("the file dialog failed");
    }
    if (!SDL_GetAtomicInt(&s.dialog_got)) return false;
    SDL_SetAtomicInt(&s.dialog_got, 0);
    return true;
}

/** @brief Act on what one presented frame of the game page brought: the harness drop, a due scripted pick, a click (which only dismisses a shown panel), an activated box, the drawn picker's choice, a file dialog's choice or failure.
 *
 * An activated box without an entry opens the file dialog (dialog_poll opens
 * the drawn picker on its failure).
 * @param v The frame's view.
 * @param scripted The scripted pick's entry index, or -1.
 * @param games Each library entry's game id.
 * @return An entry index, LAUNCHER_SEL_QUIT, LAUNCHER_SEL_DROPPED, or kPageStays while the page stays up. */
int page_frame_result(const View& v, int scripted, const char* const* games) {
    if (launcher_scripted_drop()) return LAUNCHER_SEL_DROPPED;
    if (scripted >= 0 && scripted_due() && !scripted_pick_held()) {
        OPL_INFO_F("launcher", "scripted pick: entry %d (%s)", scripted, games[scripted] ? games[scripted] : "");
        return scripted;
    }
    if (const int pc = panel_command(Ask::None, v.clicked || v.activated != LAUNCHER_PAGE_NONE); pc != kPagePass)
        return pc;   /* the click dismissed the panel */
    if (v.activated >= 0) return v.activated;
    if (picker_taken(v.picked) == LAUNCHER_SEL_DROPPED) return LAUNCHER_SEL_DROPPED;
    if (v.activated == LAUNCHER_PAGE_BROWSE) open_file_dialog();
    if (dialog_poll()) return LAUNCHER_SEL_DROPPED;
    return kPageStays;
}

/** @brief Present the game page until a box with a library entry is activated, a disc is dropped or chosen, or the player quits.
 *
 * An activated box without an entry opens the file dialog (open_file_dialog);
 * a dialog whose callback reports a failure opens the drawn picker, which
 * takes the page's commands while it is open (picker_command). A chosen or
 * dropped image returns whichever box asked for it: the ingest reads its
 * game, and the page next opens on that game's box. While the message panel
 * shows, the next command or click dismisses it and is consumed
 * (panel_command). The harness drop
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
        v.picked = LAUNCHER_PICKER_STAYS;
        v.hovered = -1;
        v.focus = focus.index;
        const bool asked = launcher_quit_ask_shown(&ask, SDL_GetTicks());
        if (asked != v.quit_asked)
            OPL_INFO_F("launcher", "command: quit ask chip %s", asked ? "shown" : "hidden");
        v.quit_asked = asked;
        present(v);
        (void)launcher_focus_hover(&focus, v.hovered);
        launcher_jobs_pump();
        if (const int r = page_frame_result(v, scripted, games); r != kPageStays) return r;
        const Ask a = wait_input();
        if (s.pointer_moved) launcher_focus_pointer_moved(&focus);
        s.pointer_moved = false;
        int r = panel_command(a, false);
        if (r == kPagePass) r = launcher_picker_shown() ? picker_command(a) : page_command(a, focus, ask, games, count);
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
    (void)launcher_picker_command(LAUNCHER_PICKER_CLOSE);
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

/** @brief Hand pending events to imgui while a wait is drawn, reading each as the progress view's quit gesture first.
 *
 * A wait (the ingest copy, a module build, the mod compile, an asset job)
 * is a progress view: launcher_quit_gesture takes the quit (the window
 * closed, SIGTERM, which SDL turns into a quit event, Escape, or East twice)
 * or arms it (one East), and the work stops; the event is consumed. The
 * harness gesture (launcher_quit_script_poll) is posted first. The
 * launcher's precompile is stepped after the events, and its wake events
 * (openpete_native_launcher_precompile_event) are consumed. */
void drain_events() {
    const Uint32 wake = openpete_native_launcher_precompile_event();
    launcher_quit_script_poll(nullptr);
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (wake && e.type == wake) continue;
        if (launcher_quit_gesture(e) != QuitGesture::None) continue;
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

/** @brief Keep the launcher drawn while a task's worker runs: open the window at its first report, drain events, redraw at the present rate.
 *
 * The loop is a build stage (launcher_pace_build_stage). An iteration that
 * presented was paced by the present (launcher_pace_begin); one that did
 * not (no report yet, or the window cannot show) sleeps 16 ms. Returns when
 * the worker flags completion. */
void task_wait_loop() {
    bool watch = false;
    launcher_pace_build_stage(true);
    for (;;) {
        const uint64_t presents = launcher_pace_count();
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
        if (launcher_pace_count() == presents) SDL_Delay(16);
    }
    launcher_pace_build_stage(false);
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
        if (ask == 0 && (a == Ask::Quit || a == Ask::Escape || a == Ask::Back)) ask = LAUNCHER_SEL_QUIT;
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

/** @brief Read the calling thread's scheduler totals from /proc/thread-self/schedstat and the process's CPU time from getrusage.
 *
 * @param run_ms Receives the thread's CPU time in ms.
 * @param wait_ms Receives the thread's runnable wait (runnable, not on a CPU) in ms.
 * @param cpu_ms Receives the CPU time of every thread of the process, exited ones included, in ms.
 * @return true when both were read (Linux with schedstats), else false and all three 0. */
static bool stamp_sched(double* run_ms, double* wait_ms, double* cpu_ms) {
    *run_ms = *wait_ms = *cpu_ms = 0.0;
#ifdef __linux__
    FILE* f = fopen("/proc/thread-self/schedstat", "r");
    if (!f) return false;
    unsigned long long run = 0, wait = 0;
    const bool ok = fscanf(f, "%llu %llu", &run, &wait) == 2;
    fclose(f);
    struct rusage ru;
    if (!ok || getrusage(RUSAGE_SELF, &ru) != 0) return false;
    *run_ms = (double)run / 1e6;
    *wait_ms = (double)wait / 1e6;
    *cpu_ms = (double)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1e3 +
              (double)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e3;
    return true;
#else
    return false;
#endif
}

void launcher_stamp(launcher_event ev) {
    static std::atomic<unsigned> logged{0};
    static const char* const k_name[LAUNCHER_EV_COUNT] = {
        "window_shown", "first_present", "scan_start", "scan_end", "list_shown"};
    if ((unsigned)ev >= LAUNCHER_EV_COUNT) return;
    const unsigned bit = 1u << (unsigned)ev;
    if (logged.fetch_or(bit) & bit) return;
    double run = 0.0, wait = 0.0, cpu = 0.0;
    const bool sched = stamp_sched(&run, &wait, &cpu);
    const double at = (double)(op_now() - k_process_start_ns) / 1e6;
    if (sched)
        OPL_INFO_F("launcher", "stamp %s %.1f ms after process start; thread %llu ran %.3f ms, waited runnable "
                   "%.3f ms; process CPU %.3f ms", k_name[ev], at, (unsigned long long)SDL_GetCurrentThreadID(),
                   run, wait, cpu);
    else
        OPL_INFO_F("launcher", "stamp %s %.1f ms after process start", k_name[ev], at);
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
    (void)launcher_picker_command(LAUNCHER_PICKER_CLOSE);
    launcher_page_closed();
    return r;
}

void openpete_launcher_begin(const openpete_launcher_game* game,
                             const openpete_options_t* neutral, int ui) {
    openpete_host_spawn_set_stop(openpete_launcher_job_cancelled);
    s.game = game;
    s.opts = neutral;
    s.ui = ui != 0;
}

int openpete_launcher_resolve(openpete_options_t* opts, char* module_path,
                              size_t module_cap, int* exit_now) {
    *exit_now = 0;
    int rc = library_resolve(opts, module_path, module_cap);
    if (rc == LAUNCHER_LIB_QUIT || launcher_quit_taken()) {
        launcher_jobs_stop_other(nullptr, false);
        *exit_now = 1;
        return OPENPETE_EXIT_OK;
    }
    if (rc == LAUNCHER_LIB_REFUSED) {
        openpete_fatal_message(OP_E_INGEST, "OpenPete could not start",
                               "The disc image was not added to the game library.");
        return OPENPETE_EXIT_SETUP_ERROR;
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
            if (launcher_quit_taken()) {
                launcher_jobs_stop_other(nullptr, false);
                *exit_now = 1;
                return OPENPETE_EXIT_OK;
            }
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
    present(v, false);
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

int openpete_launcher_refuse(const char* text) {
    if (!s.ui || s.handed_off) return -1;
    s.no_precompile = true;
    openpete_native_launcher_pipeline_cache_off();
    if (launcher_open(s.opts, 0) != 0) return -1;
    s.panel_only = true;
    launcher_panel_raise(OP_E_LIBRARY, text, true);
    (void)page_loop(nullptr, 0, -1);
    launcher_page_closed();
    s.panel_only = false;
    openpete_launcher_close();
    return 0;
}

void openpete_launcher_close(void) {
    launcher_jobs_stop_other(nullptr, false);
    s_jobs.queue.clear();
    library_lock_release();
    close_gamepad();
    openpete_native_launcher_close();
    launcher_quit_report();
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
 * A failed build also raises the game page's panel and gives the game's box
 * its failed-build drawing (launcher_panel_build_failed). A stopped job (-2)
 * records nothing.
 * @param job The finished job. */
void job_record(const Job& job) {
    if (job.rc == -2) {
        OPL_INFO_F("launcher", "module rebuild of %s stopped", job.g->game_id);
    } else if (job.rc != 0) {
        library_marker_write(job.entry.c_str(), s.last_stage);
        OPL_ERROR_F("launcher", "module rebuild of %s failed at %s; see the log file %s", job.g->game_id,
                    s.last_stage[0] ? s.last_stage : "its start", "openpete.log");
        launcher_panel_build_failed(job.g->game_id, job.g->display_name, s.last_stage);
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
 * A drawn wait is a build stage (launcher_pace_build_stage). An iteration
 * that presented was paced by the present; one that did not sleeps 16 ms.
 * @param draw true to draw the job's last progress report at the present rate. */
void job_wait(bool draw) {
    launcher_pace_build_stage(draw);
    while (!job_finished()) {
        const uint64_t presents = launcher_pace_count();
        if (s.ui && openpete_native_launcher_active()) {
            if (s_ing.busy) launcher_quit_script_poll("assets");
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
        if (launcher_pace_count() == presents) SDL_Delay(16);
    }
    launcher_pace_build_stage(false);
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
    launcher_pace_build_stage(true);
    while (s_ing.busy) {
        const uint64_t presents = launcher_pace_count();
        ingest_poll();
        if (!s_ing.busy) break;
        if (s.ui && openpete_native_launcher_active()) {
            launcher_quit_script_poll("assets");
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
        if (launcher_pace_count() == presents) SDL_Delay(5);
    }
    launcher_pace_build_stage(false);
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
    return (t_cancel && SDL_GetAtomicInt(t_cancel)) || launcher_quit_taken() ? 1 : 0;
}

void* openpete_launcher_job_stop_handle(void) { return t_cancel; }

void openpete_launcher_job_stop_adopt(void* handle) { t_cancel = static_cast<SDL_AtomicInt*>(handle); }

int openpete_launcher_jobs_in_flight(void) { return s_jobs.in_flight.load(); }
