// core/runtime/options.c — command-line flag parser.
//
// Recognized flags come from notes/plans/openpete-pc-port.md Phase 0b
// and §11.7. Unknown flags exit with OPENPETE_EXIT_SETUP_ERROR (3).

#include "openpete.h"
#include "openpete_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int needs_value(const char* flag, int argc, char** argv, int i, const char** out) {
    if (i + 1 >= argc) {
        fprintf(stderr, "openpete: missing value for %s\n", flag);
        return -1;
    }
    *out = argv[i + 1];
    return 0;
}

void openpete_options_init(openpete_options_t* opts) {
    memset(opts, 0, sizeof(*opts));
    opts->display_name        = "OpenPete";
    opts->screenshot_on_frame = -1;
    opts->screenshot_output   = "./shot.png";
    opts->log_format          = OPENPETE_LOG_PLAIN;
}

int openpete_options_parse(openpete_options_t* opts, int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        const char* v = NULL;

        if (strcmp(a, "--frame-limit") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->frame_limit = atoi(v); i++;
        } else if (strcmp(a, "--max-runtime-seconds") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->max_runtime_seconds = atoi(v); i++;
        } else if (strcmp(a, "--headless") == 0) {
            opts->headless = true;
        } else if (strcmp(a, "--screenshot-on-frame") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->screenshot_on_frame = atoi(v); i++;
        } else if (strcmp(a, "--output") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->screenshot_output = v; i++;
        } else if (strcmp(a, "--log-format") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            if      (strcmp(v, "json")  == 0) opts->log_format = OPENPETE_LOG_JSON;
            else if (strcmp(v, "plain") == 0) opts->log_format = OPENPETE_LOG_PLAIN;
            else {
                fprintf(stderr, "openpete: unknown --log-format value: %s\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            i++;
        } else if (strcmp(a, "--log-level") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->log_level_spec = v; i++;
        } else if (strcmp(a, "--disc") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->disc_path = v; i++;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            fprintf(stderr,
                "OpenPete %s\n"
                "Usage: %s [options]\n"
                "  --frame-limit N            exit after N rendered frames\n"
                "  --max-runtime-seconds N    wall-clock SIGTERM after N seconds\n"
                "  --headless                 no on-screen window (offscreen video driver)\n"
                "  --screenshot-on-frame N    save PNG of frame N to --output then continue\n"
                "  --output PATH              screenshot path (default ./shot.png)\n"
                "  --log-format json|plain    structured stderr (default plain)\n"
                "  --log-level SPEC           e.g. cd:debug,*:info; env OPENPETE_LOG=...\n"
                "  --disc PATH                disc image (BIN/CUE; ignored in Phase 0b)\n",
                opts->display_name, argv[0]);
            return OPENPETE_EXIT_SETUP_ERROR; /* treat as setup-only invocation */
        } else {
            fprintf(stderr, "openpete: unknown arg: %s\n", a);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
    }
    return OPENPETE_EXIT_OK;
}
