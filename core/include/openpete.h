// core/include/openpete.h — runtime entry point + program options.
//
// Each game's `main.cpp` is a tiny wrapper that fills in `openpete_options_t`
// and calls `openpete_run()`. All command-line parsing, window setup,
// screenshot logic, and the run loop live in core/.

#ifndef OPENPETE_H
#define OPENPETE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exit codes — see notes/plans/openpete-pc-port.md Phase 0b. */
#define OPENPETE_EXIT_OK           0  /* clean termination */
#define OPENPETE_EXIT_PANIC        1  /* panic / crash / timeout */
#define OPENPETE_EXIT_DIVERGENCE   2  /* state-divergence detected (Phase 1c+) */
#define OPENPETE_EXIT_SETUP_ERROR  3  /* disc not found, unknown flag, etc. */

/* Log format. */
typedef enum {
    OPENPETE_LOG_PLAIN = 0,
    OPENPETE_LOG_JSON  = 1,
} openpete_log_format_t;

/* All command-line options live here. Per-game main.cpp may override
 * `display_name` before passing to `openpete_run`. */
typedef struct {
    /* Identity (filled by per-game main; rest from CLI) */
    const char* display_name;     /* window title etc. */

    /* Run-loop control */
    int frame_limit;              /* 0 = unlimited */
    int max_runtime_seconds;      /* 0 = unlimited */
    bool headless;                /* no on-screen window (offscreen video driver) */

    /* Screenshot */
    int         screenshot_on_frame;   /* -1 = disabled */
    const char* screenshot_output;     /* path; default ./shot.png */

    /* Logging */
    openpete_log_format_t log_format;
    const char* log_level_spec;        /* e.g. "cd:debug,*:info" or NULL */

    /* Disc / data — Phase 1b uses these; Phase 0b ignores them. */
    const char* disc_path;
    const char* exe_path;          /* Direct PSX EXE bytes (overrides disc) */

    /* Phase 1b bring-up smoke test: skip the run loop and instead boot the
     * PSX EXE, dispatch a single recompiled function at this address, and
     * exit reporting cpu->v0. 0 = disabled. */
    uint32_t test_recomp_call_addr;
    uint32_t test_recomp_expected_v0;  /* 0 = don't check */
    uint32_t test_recomp_gp;            /* override cpu->gp before dispatch (0 = leave alone) */
    uint32_t probe_addr;                /* read 4 bytes at this PSX vaddr post-dispatch (0 = off) */
    bool     test_recomp_segv;          /* deliberately fault to verify crash handler */
} openpete_options_t;

/* Defaults: 0 / unlimited / not-headless / log plain. */
void openpete_options_init(openpete_options_t* opts);

/* Parse argv into opts. Unknown flags return OPENPETE_EXIT_SETUP_ERROR;
 * caller should propagate the exit code. argv[0] is treated as program name. */
int  openpete_options_parse(openpete_options_t* opts, int argc, char** argv);

/* Run the main loop. Returns one of OPENPETE_EXIT_*. */
int  openpete_run(const openpete_options_t* opts);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_H */
