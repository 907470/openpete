// core/launcher/launcher.cpp - the launcher's window lifecycle, its screens
// and the task runner (contract in openpete_launcher.h).
//
// Three screens, drawn through imgui on the launcher's Vulkan device
// (openpete_native_launcher_present):
//   drop      - the library is empty and no disc was named: drag a .cue/.bin
//               onto the window, or browse for one.
//   selector  - several library entries, or any while no game is named:
//               pick one, or drop a disc image to add and boot it.
//   progress  - the ingest copy, the first-boot module build and the mod
//               compile report a stage and a bar; after the hand-off the
//               renderer's pipeline precompile draws the same screen on the
//               device it adopted (openpete_launcher_draw_progress).
// Every screen draws in a 640x448 layout space scaled into the window. The
// harness variable OPENPETE_LAUNCHER_SHOT=<base> reads back the first frame
// of each screen, logs how many pixels it drew, and writes <base>.<screen>.png;
// the precompile's progress screen is the screen "precompile".
#include "launcher_internal.h"

#include "imgui.h"

#include <SDL3/SDL.h>

#include "openpete_devui.h"
#include "openpete_fatal.h"
#include "openpete_log.h"
#include "openpete_native_render.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

/* Layout space of every screen; the splash this launcher replaced used the
 * same 640x448 (Spyro 1's game window), and the screens were authored in a
 * 640x360 band centred in it. */
constexpr float kLayoutW = 640.0f;
constexpr float kLayoutH = 448.0f;
constexpr float kBandY = (kLayoutH - 360.0f) / 2.0f;

constexpr uint8_t kBg[3] = {24, 16, 40};   /* deep purple */

enum class Screen { Progress, Drop, Select };

/* One screen's content for one frame, and what the player did in it. */
struct View {
    Screen screen = Screen::Progress;
    const char* name = nullptr;   /* the game's display name; NULL or "" for none */
    const char* stage = "";
    int done = 0, total = 0;
    const char* const* labels = nullptr;
    int count = 0;
    int sel = 0;
    int clicked = -1;   /* the label the mouse picked this frame */
    bool browse = false;   /* the browse button was pressed this frame */
    bool games = false;    /* the selector lists every game's entries */
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
    SDL_ThreadID ui_tid = 0;
    char stage[128] = {};
    int done = 0, total = 0;
};
State s;

/** @brief Return the display name a view shows, upper-cased for the screens' headings.
 *
 * @param v The view.
 * @return The view's game name in capitals, or "" when it names none. */
std::string upper_name(const View& v) {
    std::string n = v.name ? v.name : "";
    for (char& c : n) c = (char)toupper((unsigned char)c);
    return n;
}

/** @brief Return the display name of the game the launcher was started for.
 *
 * @return The name, or NULL while the launcher names no game (no game is
 *         installed yet). */
const char* game_name() { return s.game ? s.game->display_name : nullptr; }

/* The layout-to-window mapping of the frame being drawn. */
struct Frame { float scale, ox, oy; };

/** @brief Map the 640x448 layout space into the window, letterboxed and centred.
 *
 * @return The scale and the offset of the layout's origin. */
Frame frame_map() {
    const ImVec2 d = ImGui::GetIO().DisplaySize;
    const float sc = (d.x / kLayoutW < d.y / kLayoutH) ? d.x / kLayoutW : d.y / kLayoutH;
    return {sc, (d.x - kLayoutW * sc) * 0.5f, (d.y - kLayoutH * sc) * 0.5f};
}

/** @brief Draw one line of text horizontally centred in the layout.
 *
 * @param f The frame mapping.
 * @param y The line's top in layout pixels.
 * @param size The text height in layout pixels.
 * @param col The colour.
 * @param text The text. */
void text_centred(const Frame& f, float y, float size, ImU32 col, const char* text) {
    ImGui::PushFont(nullptr, size * f.scale);
    const ImVec2 sz = ImGui::CalcTextSize(text);
    ImGui::SetCursorPos(ImVec2(f.ox + (kLayoutW * f.scale - sz.x) * 0.5f, f.oy + y * f.scale));
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

/** @brief Draw the progress screen: banner, stage line, bar and footer.
 *
 * The stage's row of kStageLooks gives the banner and footer. A counted
 * stage shows done/total after the stage text, or done alone while total
 * is 0 and done is not. The bar fills done/total, clamped to 1, and is
 * empty when total is 0.
 * @param f The frame mapping.
 * @param v The view. */
void draw_progress(const Frame& f, const View& v) {
    const StageLook& look = stage_look(v.stage);
    text_centred(f, 160 + kBandY, 14, IM_COL32(220, 210, 240, 255), look.banner);
    char line[160];
    if (look.counted && v.total > 0) snprintf(line, sizeof line, "%s %d/%d", v.stage, v.done, v.total);
    else if (look.counted && v.done > 0) snprintf(line, sizeof line, "%s %d", v.stage, v.done);
    else snprintf(line, sizeof line, "%s", v.stage);
    text_centred(f, 196 + kBandY, 14, IM_COL32_WHITE, line);
    float frac = v.total > 0 ? (float)v.done / (float)v.total : 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    ImGui::SetCursorPos(ImVec2(f.ox + 80 * f.scale, f.oy + (240 + kBandY) * f.scale));
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, IM_COL32(240, 138, 30, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(50, 38, 80, 255));
    ImGui::ProgressBar(frac, ImVec2((kLayoutW - 160) * f.scale, 22 * f.scale), "");
    ImGui::PopStyleColor(2);
    text_centred(f, 292 + kBandY, 8, IM_COL32(150, 130, 190, 255), look.footer);
}

/** @brief Draw the drop screen: the request for the disc image, the browse button and the quit hint.
 *
 * @param f The frame mapping.
 * @param v The view; browse is set when the button is pressed. */
void draw_drop(const Frame& f, View& v) {
    const std::string name = upper_name(v);
    const std::string drag = name.empty() ? "DRAG YOUR" : "DRAG YOUR " + name;
    text_centred(f, 160 + kBandY, 14, IM_COL32_WHITE, drag.c_str());
    text_centred(f, 186 + kBandY, 14, IM_COL32_WHITE, "DISC IMAGE (.CUE / .BIN) HERE");
    text_centred(f, 232 + kBandY, 8, IM_COL32(220, 210, 240, 255),
                 "OR PRESS SPACE / (A) TO BROWSE");
    ImGui::PushFont(nullptr, 8 * f.scale);
    ImGui::SetCursorPos(ImVec2(f.ox + (kLayoutW * 0.5f - 60) * f.scale, f.oy + (246 + kBandY) * f.scale));
    if (ImGui::Button("BROWSE...", ImVec2(120 * f.scale, 0))) v.browse = true;
    ImGui::PopFont();
    text_centred(f, 276 + kBandY, 8, IM_COL32(150, 130, 190, 255),
                 "YOUR OWN DISC IS REQUIRED. NOTHING IS DOWNLOADED.");
    text_centred(f, 300 + kBandY, 8, IM_COL32(110, 95, 150, 255), "ESC / (B) TO QUIT");
}

/** @brief Draw the selector: the heading, up to six entries with the selection marked, the drop and browse hint, and the controls hint.
 *
 * The heading reads CHOOSE A GAME while the selector lists every game's
 * entries, else CHOOSE A VERSION.
 * @param f The frame mapping.
 * @param v The view; clicked is set to the entry the mouse picked. */
void draw_select(const Frame& f, View& v) {
    text_centred(f, 136 + kBandY, 14, IM_COL32(220, 210, 240, 255),
                 v.games ? "CHOOSE A GAME" : "CHOOSE A VERSION");
    ImGui::PushFont(nullptr, 14 * f.scale);
    float y = 168 + kBandY;
    for (int i = 0; i < v.count && i < 6; i++, y += 22) {
        ImGui::SetCursorPos(ImVec2(f.ox + 120 * f.scale, f.oy + y * f.scale));
        const bool on = i == v.sel;
        ImGui::PushStyleColor(ImGuiCol_Text, on ? IM_COL32(255, 214, 120, 255)
                                                : IM_COL32(150, 130, 190, 255));
        char line[160];
        snprintf(line, sizeof line, "%s %s##%d", on ? ">" : " ", v.labels[i], i);
        if (ImGui::Selectable(line, on, 0, ImVec2(400 * f.scale, 0))) v.clicked = i;
        ImGui::PopStyleColor();
    }
    ImGui::PopFont();
    text_centred(f, 304 + kBandY, 8, IM_COL32(150, 130, 190, 255),
                 "DRAG A DISC IMAGE (.CUE / .BIN) HERE TO ADD IT - SPACE / (X) TO BROWSE");
    text_centred(f, 320 + kBandY, 8, IM_COL32(110, 95, 150, 255),
                 "UP/DOWN OR STICK - ENTER/(A) PLAY - ESC/(B) QUIT");
}

/** @brief Draw one frame of a screen: the title lines, then the screen's content, in one borderless window over the whole display.
 *
 * The game's subtitle line is left out while the launcher names no game.
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
    const Frame f = frame_map();
    text_centred(f, 36 + kBandY, 28, IM_COL32(240, 138, 30, 255), "OPENPETE");
    if (const std::string name = upper_name(v); !name.empty()) {
        const std::string sub = name + " PC PORT";
        text_centred(f, 72 + kBandY, 14, IM_COL32(150, 130, 190, 255), sub.c_str());
    }
    if (v.screen == Screen::Progress) draw_progress(f, v);
    else if (v.screen == Screen::Drop) draw_drop(f, v);
    else draw_select(f, v);
    ImGui::End();
}

/* Shot tags, indexed by Screen, then the precompile's progress screen. */
constexpr const char* kShotTag[] = {"progress", "drop", "selector", "precompile"};
constexpr int kShotPrecompile = 3;

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
    const int rc = openpete_native_launcher_shot(dev, path.c_str(), kBg, &differ, &total);
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
    const bool drawn = openpete_native_launcher_present(dev, kBg[0] / 255.0f, kBg[1] / 255.0f,
                                                        kBg[2] / 255.0f, draw_view, &v) != 0;
    if (drawn) maybe_shot(dev, tag);
    return drawn;
}

/** @brief Present one frame of a view on the launcher's device, naming the launcher's game.
 *
 * @param v The view. */
void present(View& v) {
    v.name = game_name();
    (void)present_on(nullptr, v, (int)v.screen);
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

/** @brief Open the launcher's window, device and imgui context unless they exist, telling the player once when the device cannot be created.
 *
 * The window takes the game window's size, or the 640x448 layout space
 * before a game is installed.
 * @param opts The options the window is created from.
 * @param continues Non-zero when the run can continue without the device
 *                  (a disc is or will be named); passed to the message.
 * @return 0 when the device exists, -1 when it does not. */
int launcher_open(const openpete_options_t* opts, int continues) {
    if (openpete_native_launcher_active()) return 0;
    if (s.failed || !opts) return -1;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        s.failed = true;
        const std::string why = std::string("The video system could not start (") + SDL_GetError() + ").";
        openpete_launcher_vk_failure(why.c_str(), continues);
        return -1;
    }
    int w, h;
    openpete_game_window_size(opts, &w, &h);
    /* No game installed yet: no game window size, so the layout space's. */
    if (w <= 0 || h <= 0) { w = (int)kLayoutW; h = (int)kLayoutH; }
    if (openpete_native_launcher_open(opts, w, h, "OpenPete") != 0) {
        s.failed = true;
        openpete_launcher_vk_failure(openpete_native_launcher_error(), continues);
        return -1;
    }
    open_first_gamepad();
    return 0;
}

/** @brief SDL file-dialog callback: store the chosen path and flag it.
 *
 * @param userdata Unused.
 * @param files A NULL-terminated list of chosen paths, or NULL on an error.
 * @param filter Unused. */
void SDLCALL on_file_chosen(void* userdata, const char* const* files, int filter) {
    (void)userdata; (void)filter;
    if (files && files[0]) {
        snprintf(s.drop_path, sizeof s.drop_path, "%s", files[0]);
        SDL_SetAtomicInt(&s.dialog_got, 1);
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
    SDL_ShowOpenFileDialog(on_file_chosen, nullptr, openpete_native_launcher_window(),
                           filters, 2, nullptr, false);
}

/* What one input event asks of a waiting screen. */
enum class Ask { None, Quit, Pick, Browse, Up, Down, Dropped };

/** @brief Read one event as a launcher command: keys, gamepad buttons, the left stick, a dropped file, the window closing.
 *
 * A dropped file's path is stored for launcher_dropped_path; an added
 * gamepad is opened.
 * @param e The event.
 * @return The command. */
Ask ask_of(const SDL_Event& e) {
    static bool armed = true;   /* left-stick debounce: re-centre to move again */
    switch (e.type) {
        case SDL_EVENT_QUIT: return Ask::Quit;
        case SDL_EVENT_DROP_FILE:
            snprintf(s.drop_path, sizeof s.drop_path, "%s", e.drop.data);
            return Ask::Dropped;
        case SDL_EVENT_GAMEPAD_ADDED: open_first_gamepad(); return Ask::None;
        case SDL_EVENT_KEY_DOWN: {
            static const struct { SDL_Keycode k; Ask a; } k_keys[] = {
                {SDLK_ESCAPE, Ask::Quit}, {SDLK_RETURN, Ask::Pick}, {SDLK_KP_ENTER, Ask::Pick},
                {SDLK_SPACE, Ask::Browse}, {SDLK_UP, Ask::Up}, {SDLK_DOWN, Ask::Down}};
            for (const auto& k : k_keys) if (e.key.key == k.k) return k.a;
            return Ask::None;
        }
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
            static const struct { Uint8 b; Ask a; } k_buttons[] = {
                {SDL_GAMEPAD_BUTTON_SOUTH, Ask::Pick}, {SDL_GAMEPAD_BUTTON_EAST, Ask::Quit},
                {SDL_GAMEPAD_BUTTON_WEST, Ask::Browse},
                {SDL_GAMEPAD_BUTTON_DPAD_UP, Ask::Up}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, Ask::Down}};
            for (const auto& b : k_buttons) if (e.gbutton.button == b.b) return b.a;
            return Ask::None;
        }
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (e.gaxis.axis != SDL_GAMEPAD_AXIS_LEFTY) return Ask::None;
            if (e.gaxis.value > -8000 && e.gaxis.value < 8000) { armed = true; return Ask::None; }
            if (!armed) return Ask::None;
            armed = false;
            return e.gaxis.value < 0 ? Ask::Up : Ask::Down;
        default: return Ask::None;
    }
}

/** @brief Wait up to 100 ms for input, then hand every pending event to imgui and return the first command among them.
 *
 * @return The first command the events carry, or Ask::None. */
Ask wait_input() {
    SDL_Event e;
    Ask first = Ask::None;
    if (!SDL_WaitEventTimeout(&e, 100)) return first;
    do {
        openpete_devui_process_event(&e);
        const Ask a = ask_of(e);
        if (first == Ask::None) first = a;
    } while (SDL_PollEvent(&e));
    return first;
}

/** @brief Harness: the index OPENPETE_LAUNCHER_SELECT names, or -1 when unset or out of range.
 *
 * @param count The entry count.
 * @return The scripted pick. */
int scripted_pick(int count) {
    const char* e = getenv("OPENPETE_LAUNCHER_SELECT");
    if (!e || !*e) return -1;
    const int pick = atoi(e);
    return (pick >= 0 && pick < count) ? pick : -1;
}

/** @brief Run the drop screen until a disc image is dropped or chosen, or the player quits.
 *
 * The harness variable OPENPETE_LAUNCHER_DROP=<path> acts as a drop of that
 * path after the first frame (launcher_scripted_drop).
 * @return The path, owned by the launcher, or NULL on quit. */
const char* wait_for_disc() {
    View v;
    v.screen = Screen::Drop;
    for (;;) {
        v.browse = false;
        present(v);
        if (const char* scripted = launcher_scripted_drop()) return scripted;
        if (v.browse) open_file_dialog();
        if (SDL_GetAtomicInt(&s.dialog_got)) return s.drop_path;
        const Ask a = wait_input();
        if (a == Ask::Quit) return nullptr;
        if (a == Ask::Dropped) return s.drop_path;
        if (a == Ask::Pick || a == Ask::Browse) open_file_dialog();
    }
}

/** @brief Hand pending events to imgui while a wait is drawn, remembering a quit for the hand-off.
 *
 * A wait (the ingest copy, the module build, the mod compile) cannot be
 * cancelled, so a quit (the window closed, or SIGTERM, which SDL turns into
 * a quit event) is held and re-posted at the hand-off, where the game's
 * loop ends the run, instead of being consumed here. */
void drain_events() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) s.quit_pending = true;
        else openpete_devui_process_event(&e);
    }
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
        if (done) break;
        SDL_Delay(16);
    }
    if (watch) SDL_RemoveEventWatch(task_event_watch, nullptr);
}

}  // namespace

const openpete_launcher_game* launcher_game(void) { return s.game; }

const char* launcher_dropped_path(void) { return s.drop_path; }

bool launcher_neutral(void) { return !s.game || !s.game->game_id; }

const char* launcher_scripted_drop(void) {
    if (s.scripted_taken) return nullptr;
    const char* e = getenv("OPENPETE_LAUNCHER_DROP");
    if (!e || !*e) return nullptr;
    s.scripted_taken = true;
    snprintf(s.drop_path, sizeof s.drop_path, "%s", e);
    OPL_INFO_F("launcher", "scripted drop: %s", s.drop_path);
    return s.drop_path;
}

int launcher_select(const char* const* labels, int count) {
    const bool screen = s.ui && !s.handed_off && count > 0 && launcher_open(s.opts, 1) == 0;
    if (!screen) return launcher_scripted_drop() ? LAUNCHER_SEL_DROPPED : LAUNCHER_SEL_NOSCREEN;
    View v;
    v.screen = Screen::Select;
    v.labels = labels;
    v.count = count;
    v.games = launcher_neutral();
    const int scripted = scripted_pick(count);
    for (;;) {
        v.clicked = -1;
        present(v);
        if (launcher_scripted_drop()) return LAUNCHER_SEL_DROPPED;
        if (scripted >= 0) return scripted;
        if (v.clicked >= 0) return v.clicked;
        if (SDL_GetAtomicInt(&s.dialog_got)) {
            SDL_SetAtomicInt(&s.dialog_got, 0);
            return LAUNCHER_SEL_DROPPED;
        }
        switch (wait_input()) {
            case Ask::Quit: return LAUNCHER_SEL_QUIT;
            case Ask::Dropped: return LAUNCHER_SEL_DROPPED;
            case Ask::Browse: open_file_dialog(); break;
            case Ask::Pick: return v.sel;
            case Ask::Up: if (v.sel > 0) v.sel--; break;
            case Ask::Down: if (v.sel < count - 1) v.sel++; break;
            default: break;
        }
    }
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
    if (rc == LAUNCHER_LIB_QUIT) { *exit_now = 1; return OPENPETE_EXIT_OK; }
    if (rc != 0) {
        openpete_fatal_message("OpenPete could not start", "The game library could not be read.");
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
        if (!dropped) { *exit_now = 1; return OPENPETE_EXIT_OK; }   /* window closed */
    }
    opts->ingest_path = dropped;
    if (library_resolve(opts, module_path, module_cap) != 0) {
        openpete_fatal_message("OpenPete could not start",
                               "The disc image could not be added to the game library.");
        return 1;
    }
    return OPENPETE_EXIT_OK;
}

void openpete_launcher_progress(const char* stage, int done, int total) {
    if (!s.ui || s.handed_off) return;
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

int openpete_launcher_handoff(openpete_options_t* opts) {
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
    close_gamepad();
    openpete_native_launcher_close();
}

void openpete_launcher_vk_failure(const char* why, int continues) {
    const char* next = continues
        ? "The game starts on the OpenGL fallback display instead."
        : "Install the latest driver for your graphics card. OpenPete will now close.";
    OPL_ERROR_F("launcher", "no Vulkan device: %s; %s", why ? why : "",
                continues ? "continuing on PsyCross's GL window" : "exiting");
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
    snprintf(text, sizeof text, "OpenPete could not start Vulkan graphics.\n\n%s\n\n%s",
             why ? why : "", next);
    const bool shown = SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                                                "OpenPete could not start graphics", text, nullptr);
    OPL_INFO_F("launcher", "message box %s", shown ? "shown and dismissed" : "could not be shown");
}
