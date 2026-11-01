// core/launcher/launcher_pace.cpp - the launcher's present rate (contract in
// launcher_pace.h): one present per display refresh while the window can
// show, none while it cannot.
#include "launcher_pace.h"

#include "openpete_log.h"
#include "openpete_native_render.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdlib>

namespace {

/* The period assumed when the display reports no refresh. */
constexpr double kFallbackHz = 60.0;
/* The highest present rate of a build stage on a software rasterizer, whose
 * draw takes CPU time from the build. */
constexpr double kBuildStageSoftwareHz = 10.0;

struct Pace {
    Uint64 last_ns = 0;        /* start of the last present that ran */
    uint64_t count = 0;        /* presents that ran */
    bool shown = true;         /* the window could show at the last decision */
    Uint64 win_start_ns = 0;   /* start of the rollup's second */
    unsigned win_count = 0;    /* presents in it */
    Uint64 win_busy_ns = 0;    /* time inside those presents (launcher_pace_begin to launcher_pace_done) */
    unsigned win_late = 0;     /* presents that began more than half a period after they were due */
    int log = -1;              /* OPENPETE_LAUNCHER_PRESENT_LOG, read once */
    bool build_stage = false;  /* launcher_pace_build_stage */
} s_pace;

/** @brief Return the refresh rate of the launcher's display: the swapchain's, or kFallbackHz when unknown. */
double refresh_hz() {
    const float hz = openpete_native_launcher_refresh_hz();
    return hz > 1.0f ? (double)hz : kFallbackHz;
}

/** @brief Return the refreshes per present: 1, or during a build stage on a software rasterizer the smallest n with refresh / n at or below kBuildStageSoftwareHz. */
int divisor() {
    if (!s_pace.build_stage || !openpete_native_launcher_software()) return 1;
    const int n = (int)std::ceil(refresh_hz() / kBuildStageSoftwareHz - 1e-3);
    return n > 1 ? n : 1;
}

/** @brief Return the present period in ns: the refresh period times the divisor. */
Uint64 period_ns() {
    return (Uint64)(1e9 * divisor() / refresh_hz());
}

/** @brief Return whether OPENPETE_LAUNCHER_PRESENT_LOG is set to a value not starting with 0 (read once; 2 also logs every present's interval). */
bool log_on() {
    if (s_pace.log < 0) {
        const char* e = getenv("OPENPETE_LAUNCHER_PRESENT_LOG");
        s_pace.log = (e && *e && *e != '0') ? (*e == '2' ? 2 : 1) : 0;
    }
    return s_pace.log > 0;
}

}  // namespace

bool launcher_pace_visible(void) {
    SDL_Window* win = openpete_native_launcher_window();
    if (!win) return false;
    return (SDL_GetWindowFlags(win) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN | SDL_WINDOW_OCCLUDED)) == 0;
}

bool launcher_pace_begin(bool presented_before, bool may_wait) {
    const bool shown = launcher_pace_visible();
    if (shown != s_pace.shown) {
        s_pace.shown = shown;
        s_pace.last_ns = 0;
        s_pace.win_start_ns = 0;   /* the rollup measures presents, not the pause */
        s_pace.win_count = 0;
        s_pace.win_busy_ns = 0;
        s_pace.win_late = 0;
        OPL_INFO_F("launcher", "window %s: presents %s (%llu presents so far)",
                   shown ? "shown" : "minimised or hidden", shown ? "resume at the refresh" : "stop",
                   (unsigned long long)s_pace.count);
    }
    if (!shown && presented_before) return false;
    const Uint64 now = SDL_GetTicksNS();  /* clock-guard: wall: present pacing */
    const Uint64 period = period_ns();
    const Uint64 due = s_pace.last_ns + period;
    if (s_pace.last_ns && now > due + period / 2) s_pace.win_late++;
    if (s_pace.last_ns && now < due) {
        if (!may_wait) return false;
        SDL_DelayPrecise(due - now);
    }
    const Uint64 start = SDL_GetTicksNS();  /* clock-guard: wall: present pacing */
    if (s_pace.last_ns && log_on() && s_pace.log > 1)
        OPL_INFO_F("launcher", "launcher present interval %.2f ms (period %.2f ms)",
                   (double)(start - s_pace.last_ns) / 1e6, (double)period / 1e6);
    s_pace.last_ns = start;
    return true;
}

void launcher_pace_done(void) {
    s_pace.count++;
    if (!log_on()) return;
    const Uint64 now = SDL_GetTicksNS();  /* clock-guard: wall: rollup window */
    if (!s_pace.win_start_ns) {
        s_pace.win_start_ns = now;
        s_pace.win_count = 0;
        s_pace.win_busy_ns = 0;
        s_pace.win_late = 0;
        return;
    }
    s_pace.win_count++;
    s_pace.win_busy_ns += now - s_pace.last_ns;
    const Uint64 span = now - s_pace.win_start_ns;
    if (span < 1000000000ull) return;
    OPL_INFO_F("launcher",
               "launcher presents: %u in %.1f ms (%.2f per s, refresh %.3f Hz, divisor %d, busy %.2f ms per present, "
               "%u late)",
               s_pace.win_count, (double)span / 1e6, (double)s_pace.win_count * 1e9 / (double)span,
               (double)openpete_native_launcher_refresh_hz(), divisor(),
               (double)s_pace.win_busy_ns / 1e6 / s_pace.win_count, s_pace.win_late);
    s_pace.win_start_ns = now;
    s_pace.win_count = 0;
    s_pace.win_busy_ns = 0;
    s_pace.win_late = 0;
}

uint64_t launcher_pace_count(void) { return s_pace.count; }

void launcher_pace_build_stage(bool on) {
    if (on == s_pace.build_stage) return;
    s_pace.build_stage = on;
    s_pace.win_start_ns = 0;   /* a rollup holds one divisor */
    OPL_INFO_F("launcher", "build stage %s", on ? "starts: presents at the refresh (at most 10 Hz on a software rasterizer)"
                                                 : "ends: presents at the refresh");
}
