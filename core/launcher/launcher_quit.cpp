// core/launcher/launcher_quit.cpp - quitting while the launcher works
// (contract in launcher_quit.h).
//
// A progress view (the ingest copy, a module build, the mod compile, a wait
// for an asset job) quits on one gesture function, launcher_quit_gesture.
// The quit is one process-wide flag: openpete_launcher_job_cancelled reads
// it on every thread, so the copy stops at its next chunk, the module build
// and the mod compile kill their compiler children
// (openpete_host_spawn_argv_stoppable), and an asset job stops at its next
// check; the background pipeline compile takes no other pipeline (its
// pool stays paused through the end of a stopped job); the window hides at
// once and the process ends once the work stops.
#include "launcher_quit.h"
#include "launcher_internal.h"

#include <SDL3/SDL.h>

#include "openpete_log.h"
#include "openpete_native_render.h"
#include "openpete_time.h"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

SDL_AtomicInt s_taken = {};
op_time_t s_taken_ns = 0;
/* East's ask once, the game page's rule (launcher_quit_ask_press). */
launcher_quit_ask s_ask = {};

/* The harness gesture's progress (launcher_quit_script_poll). */
struct Script {
    bool parsed = false, active = false;
    std::string point, gesture;
    unsigned long delay_ms = 0;
    Uint64 seen_ms = 0;   /* the point was first seen */
    int sent = 0;         /* presses posted */
};
Script s_script;

/** @brief Post one input event of a scripted gesture.
 *
 * @param what quit, close, escape or east. */
void post_gesture(const std::string& what) {
    SDL_Event e = {};
    SDL_Window* win = openpete_native_launcher_window();
    if (what == "quit") {
        e.type = SDL_EVENT_QUIT;
    } else if (what == "close") {
        e.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
        e.window.windowID = win ? SDL_GetWindowID(win) : 0;
    } else if (what == "sigterm") {
        /* SDL turns the signal into a quit event, as for one sent from outside. */
        OPL_INFO_F("launcher", "%s", "scripted quit gesture: sigterm");
        (void)raise(SIGTERM);
        return;
    } else if (what == "escape") {
        e.type = SDL_EVENT_KEY_DOWN;
        e.key.windowID = win ? SDL_GetWindowID(win) : 0;
        e.key.key = SDLK_ESCAPE;
        e.key.scancode = SDL_SCANCODE_ESCAPE;
        e.key.down = true;
    } else {
        e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        e.gbutton.button = SDL_GAMEPAD_BUTTON_EAST;
        e.gbutton.down = true;
    }
    OPL_INFO_F("launcher", "scripted quit gesture: %s", what.c_str());
    (void)SDL_PushEvent(&e);
}

/** @brief Name the work point a progress stage belongs to.
 *
 * @param stage The stage text.
 * @return copy, read, translate, build, link, mods, or "" for another stage. */
const char* point_of_stage(const char* stage) {
    static const struct { const char* prefix; const char* point; } k_points[] = {
        {"COPYING", "copy"}, {"READING", "read"}, {"TRANSLATING", "translate"},
        {"BUILDING", "build"}, {"LINKING", "link"}, {"COMPILING MODS", "mods"}};
    for (const auto& p : k_points)
        if (stage && !strncmp(stage, p.prefix, strlen(p.prefix))) return p.point;
    return "";
}

/** @brief Parse OPENPETE_LAUNCHER_QUIT_AT once.
 *
 * @return true when a gesture is scripted. */
bool script_parse() {
    if (s_script.parsed) return s_script.active;
    s_script.parsed = true;
    const char* e = getenv("OPENPETE_LAUNCHER_QUIT_AT");
    const char* colon = e ? strchr(e, ':') : nullptr;
    if (!colon) return false;
    std::string point(e, (size_t)(colon - e));
    if (const size_t plus = point.find('+'); plus != std::string::npos) {
        s_script.delay_ms = strtoul(point.c_str() + plus + 1, nullptr, 10);
        point.resize(plus);
    }
    s_script.point = point;
    s_script.gesture = colon + 1;
    s_script.active = true;
    return true;
}

}  // namespace

QuitGesture launcher_quit_gesture(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_QUIT:
            launcher_quit_take("quit event");
            return QuitGesture::Quit;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            launcher_quit_take("window closed");
            return QuitGesture::Quit;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.key != SDLK_ESCAPE || e.key.repeat) return QuitGesture::None;
            launcher_quit_take("Escape");
            return QuitGesture::Quit;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            if (e.gbutton.button != SDL_GAMEPAD_BUTTON_EAST) return QuitGesture::None;
            if (launcher_quit_ask_press(&s_ask, SDL_GetTicks())) {
                launcher_quit_take("East pressed twice");
                return QuitGesture::Quit;
            }
            OPL_INFO_F("launcher", "%s", "quit armed: the quit chip reads Press again to quit");
            return QuitGesture::Armed;
        default:
            return QuitGesture::None;
    }
}

bool launcher_quit_armed(void) {
    if (!s_ask.asked_ms) return false;
    if (launcher_quit_ask_shown(&s_ask, SDL_GetTicks())) return true;
    s_ask.asked_ms = 0;
    OPL_INFO_F("launcher", "quit disarmed: no second East within %d ms", LAUNCHER_QUIT_ASK_MS);
    return false;
}

void launcher_quit_take(const char* how) {
    if (SDL_SetAtomicInt(&s_taken, 1) != 0) return;
    s_taken_ns = op_now();
    s_ask.asked_ms = 0;
    OPL_INFO_F("launcher", "quit during work (%s): the window hides and the work stops", how);
    openpete_native_launcher_compile_hold();
    if (SDL_Window* win = openpete_native_launcher_window()) SDL_HideWindow(win);
}

bool launcher_quit_taken(void) { return SDL_GetAtomicInt(&s_taken) != 0; }

int openpete_launcher_quit_taken(void) { return launcher_quit_taken() ? 1 : 0; }

void launcher_quit_report(void) {
    if (!launcher_quit_taken()) return;
    OPL_INFO_F("launcher", "quit during work: the work stopped and the launcher closed %.1f ms after the quit",
               (double)(op_now() - s_taken_ns) / 1e6);
}

void launcher_quit_script_poll(const char* mark) {
    if (!script_parse() || launcher_quit_taken()) return;
    const Uint64 now = SDL_GetTicks();
    if (!s_script.seen_ms) {
        const char* point = mark ? mark : point_of_stage(launcher_last_stage());
        if (s_script.point != point) return;
        s_script.seen_ms = now ? now : 1;
        OPL_INFO_F("launcher", "scripted quit point reached: %s", point);
    }
    const Uint64 due = s_script.seen_ms + s_script.delay_ms;
    const std::string& g = s_script.gesture;
    if (s_script.sent == 0 && now >= due) {
        post_gesture(g == "east2" ? "east" : g);
        s_script.sent = 1;
    } else if (s_script.sent == 1 && g == "east2" && now >= due + 500) {
        post_gesture("east");
        s_script.sent = 2;
    }
}
