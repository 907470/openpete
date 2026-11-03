// core/launcher/launcher_progress.cpp - the progress view's step names and
// the first boot's weighted bar (contract in launcher_progress.h).
#include "launcher_progress.h"

#include "openpete_log.h"
#include "openpete_time.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

/* One stage id: the prefix the work's text starts with, its step name, the
 * first-boot segment it fills and whether its line shows its counts. */
struct Step {
    const char* id;
    const char* name;
    LauncherSegment seg;
    bool counted;
};
constexpr Step kSteps[] = {
    {"COPYING DISC TO YOUR LIBRARY", "Copying disc to your library", LauncherSegment::Copy, false},
    {"READING GAME CODE FROM YOUR DISC", "Reading game code from your disc", LauncherSegment::Read, false},
    {"TRANSLATING GAME CODE", "Translating game code", LauncherSegment::Translate, false},
    {"BUILDING FOR YOUR PC", "Building for your PC", LauncherSegment::Build, false},
    {"LINKING", "Linking", LauncherSegment::Build, false},
    {"COMPILING MODS", "Compiling mods", LauncherSegment::None, true},
    {"COMPILING SHADERS", "Compiling shaders", LauncherSegment::None, true},
    {"RENDERING PREVIEW", "Rendering preview", LauncherSegment::None, false},
    {"PREPARING THE GAME", "Preparing the game", LauncherSegment::None, false},
};

/* The segments' shares of the first boot's time: the mean over twelve
 * launcher first boots, two per game on Linux and on Windows (copy 10.8 to
 * 26.8%, read 1.3 to 4.8%, translate 10.3 to 28.0%, build and link 50.3 to
 * 71.2% per run). */
constexpr float kWeights[(int)LauncherSegment::Count] = {0.188f, 0.023f, 0.165f, 0.624f};

/* Process start for the bar's log lines: this translation unit's static
 * initialization, which runs before main. */
const op_time_t k_start_ns = op_now();  /* clock-guard: wall: seconds from process start for the log */

/** @brief Return the kSteps row whose id starts a stage text.
 *
 * @param stage The stage text.
 * @return The row, or NULL. */
const Step* step_of(const char* stage) {
    if (!stage) return nullptr;
    for (const Step& s : kSteps)
        if (strncmp(stage, s.id, strlen(s.id)) == 0) return &s;
    return nullptr;
}

}  // namespace

LauncherSegment launcher_progress_segment(const char* stage) {
    const Step* s = step_of(stage);
    return s ? s->seg : LauncherSegment::None;
}

float launcher_progress_weight(LauncherSegment seg) {
    return seg == LauncherSegment::None || seg == LauncherSegment::Count ? 0.0f : kWeights[(int)seg];
}

void launcher_progress_line(const char* stage, int done, int total, char* out, size_t cap) {
    const Step* s = step_of(stage);
    const char* name = s ? s->name : (stage ? stage : "");
    if (s && s->counted && total > 0) snprintf(out, cap, "%s (%d/%d)", name, done, total);
    else if (s && s->counted && done > 0) snprintf(out, cap, "%s (%d)", name, done);
    else snprintf(out, cap, "%s", name);
}

float launcher_progress_frac(const char* stage, int done, int total, bool with_copy) {
    const Step* s = step_of(stage);
    float own = total > 0 ? (float)done / (float)total : 0.0f;
    if (s && strcmp(s->id, "LINKING") == 0) own = 1.0f;
    own = std::clamp(own, 0.0f, 1.0f);
    if (!s || s->seg == LauncherSegment::None) return own;
    const int first = with_copy ? 0 : 1;
    float before = 0.0f, sum = 0.0f;
    for (int i = first; i < (int)LauncherSegment::Count; i++) {
        if (i < (int)s->seg) before += kWeights[i];
        sum += kWeights[i];
    }
    if ((int)s->seg < first) return own;   /* a copy outside a run with the copy */
    return std::clamp((before + kWeights[(int)s->seg] * own) / sum, 0.0f, 1.0f);
}

float launcher_progress_bar(const char* stage, int done, int total) {
    static bool with_copy = false;
    static LauncherSegment last = LauncherSegment::None;
    const LauncherSegment seg = launcher_progress_segment(stage);
    if (seg == LauncherSegment::Copy) with_copy = true;
    else if (seg == LauncherSegment::Read && last != LauncherSegment::Read && last != LauncherSegment::Copy)
        with_copy = false;
    if (seg != LauncherSegment::None) last = seg;
    const float frac = launcher_progress_frac(stage, done, total, with_copy);
    static const bool log = getenv("OPENPETE_LAUNCHER_PROGRESS_LOG") != nullptr;
    if (log) {
        static std::string logged_stage;
        static int logged_pct = -1;
        const int pct = (int)std::floor(frac * 100.0f);
        if (pct != logged_pct || logged_stage != (stage ? stage : "")) {
            logged_pct = pct;
            logged_stage = stage ? stage : "";
            const double t = (double)(op_now() - k_start_ns) / (double)OP_NS_PER_SEC;  /* clock-guard: wall: log stamp */
            OPL_INFO_F("launcher", "progress bar: %.4f at %.3f s (%s %d/%d)", frac, t, logged_stage.c_str(), done,
                       total);
        }
    }
    return frac;
}
