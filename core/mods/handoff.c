// core/mods/handoff.c — process handoff: checkpoint → exec → restore.
//
// Binary-shape operations (install/uninstall a site-hook mod, SDK
// regen) need a NEW process image; the handoff carries the live session
// across it in seconds: at the run loop's quiescent point (game fiber
// yielded — the same guarantees as the savestate hotkeys) the engine
// saves the full session to a handoff file (engine snapshot + per-mod
// participation blobs), exec()s its own image with the original argv
// plus --resume-handoff, and the fresh process restores the file at its
// FIRST quiescent point — RAM, CPU, frame counter, and mod state all
// continue.
//
// v1 scope, stated plainly:
//   - POSIX exec (Windows spawn+exit rides distribution D-track, which
//     owns the Windows process story).
//   - Same-binary exec is fully supported (savestates are build-hash
//     gated; the CROSS-build handoff — the re-link case — additionally
//     needs a gate override once the rebuild flow exists to drive it;
//     the mechanism here is the seam it will ride).
//   - Triggers: OPENPETE_HANDOFF_AT=<frame> (CI/dev) and
//     openpete_handoff_request() (the future mod menu's restart tier).

#include "openpete_handoff.h"

#include "openpete_log.h"
#include "openpete_state.h"
#include "openpete_fiber.h" /* openpete_frame */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#endif

static const char* s_resume_path = NULL; /* --resume-handoff */
static int         s_resume_done = 0;
static unsigned    s_trigger_at = 0;     /* 0 = disarmed */
static int         s_exec_done = 0;
static int         s_requested = 0;

static int    s_argc = 0;
static char** s_argv = NULL;

void openpete_handoff_init(const char* resume_path, int argc, char** argv) {
    s_resume_path = resume_path;
    s_argc = argc;
    s_argv = argv;
    const char* at = getenv("OPENPETE_HANDOFF_AT");
    if (at && *at) {
        s_trigger_at = (unsigned)strtoul(at, NULL, 0);
        OPL_INFO_F("handoff", "armed at frame %u", s_trigger_at);
    }
}

void openpete_handoff_request(void) {
    s_requested = 1;
}

static void handoff_path(char* out, size_t cap) {
    char dir[1024] = ".";
    (void)openpete_state_resolve_dir(dir, sizeof dir);
    snprintf(out, cap, "%s/handoff.state", dir);
}

void openpete_handoff_poll(CPUState* cpu) {
    /* Resume leg: first quiescent point of the fresh process. Consumes
     * any env trigger too — the schedule fired in the PREVIOUS image;
     * without this the resumed frame counter (>= the trigger) would
     * re-fire it every quiescent pass, exec-looping forever. */
    if (s_resume_path && !s_resume_done) {
        s_resume_done = 1;
        s_trigger_at = 0;
        s_exec_done = 1;
        unsigned frame = openpete_frame;
        if (openpete_state_load_file(s_resume_path, cpu, &frame) == 0) {
            openpete_frame = frame;
            remove(s_resume_path);
            OPL_INFO_F("handoff", "session resumed at frame %u", frame);
        } else {
            OPL_ERROR_F("handoff", "resume load failed (%s) — continuing "
                        "as a fresh session", s_resume_path);
        }
        return;
    }

    /* Checkpoint leg. */
    int due = s_requested ||
              (s_trigger_at && openpete_frame >= s_trigger_at);
    if (!due || s_exec_done) return;
    s_exec_done = 1;
    s_requested = 0;

#ifdef _WIN32
    OPL_ERROR_F("handoff", "process handoff is POSIX-only for now "
                "(Windows rides distribution D-track)");
#else
    char path[1280];
    handoff_path(path, sizeof path);
    if (openpete_state_save_file(path, cpu, openpete_frame) != 0) {
        OPL_ERROR_F("handoff", "checkpoint save failed — handoff aborted");
        return;
    }

    /* Rebuild argv: original minus any prior --resume-handoff pair,
     * plus ours. Exec via /proc/self/exe so argv[0] needs no PATH. */
    char** nv = (char**)calloc((size_t)s_argc + 3, sizeof(char*));
    if (!nv) return;
    int n = 0;
    for (int i = 0; i < s_argc; i++) {
        if (s_argv[i] && strcmp(s_argv[i], "--resume-handoff") == 0) {
            i++; /* skip its value too */
            continue;
        }
        nv[n++] = s_argv[i];
    }
    static char s_path_copy[1280];
    snprintf(s_path_copy, sizeof s_path_copy, "%s", path);
    nv[n++] = (char*)"--resume-handoff";
    nv[n++] = s_path_copy;
    nv[n] = NULL;

    OPL_INFO_F("handoff", "exec at frame %u (checkpoint %s)",
               openpete_frame, path);
    fflush(NULL);
    /* Put the real stderr back on fd 2 first. The log mirror routes fd 2
     * through a pipe drained by a thread, and exec destroys every thread
     * while keeping the fds: the successor would write into a pipe with
     * no reader and wedge as soon as it filled. It opens its own mirror
     * on the way up, carrying this run over to openpete.log.1. */
    openpete_log_file_close();
    execv("/proc/self/exe", nv);
    /* Only reached on failure — never brick the session. */
    OPL_ERROR_F("handoff", "execv failed: continuing without handoff");
    free(nv);
#endif
}
