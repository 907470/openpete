// core/runtime/options.c — command-line flag parser.
//
// Recognized flags come from notes/plans/openpete-pc-port.md Phase 0b
// and §11.7. Unknown flags exit with OPENPETE_EXIT_SETUP_ERROR (3).

#include "openpete.h"
#include "openpete_log.h"
#include "game_extras.h"  /* game_resolve_level_arg (--skip-to-level names) */

#include <SDL3/SDL.h>     /* SDL_GetBasePath — locate openpete.opts next to the exe */

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

/* Tokenize a sidecar args-file buffer in place. Whitespace separates
 * tokens; '#' to end-of-line is a comment. Modifies buf (NUL-inserts
 * after each token). Returns token count, or -1 on overflow. */
static int tokenize_sidecar_args(char* buf, char** tokens, int max_tokens) {
    int n = 0;
    char* p = buf;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        if (*p == '#') { while (*p && *p != '\n') p++; continue; }
        if (n >= max_tokens) {
            fprintf(stderr, "openpete: too many tokens in args file (max %d)\n", max_tokens);
            return -1;
        }
        tokens[n++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        if (*p) *p++ = '\0';
    }
    return n;
}

/* Read an args file at `path` (≤4096 bytes), tokenize it (whitespace-
 * separated, '#'..EOL comments), and splice the tokens into argv right
 * after argv[0] so they parse BEFORE the trailing args — the trailing
 * args (further right) then win scalar conflicts (last-write). `label`
 * names the source in diagnostics.
 *
 * Returns:
 *    1  spliced; *out_argc / *out_argv hold the new argv (heap-owned,
 *       leaked for program lifetime — token strings point into a malloc
 *       buffer that has to outlive opts).
 *    0  file absent or empty/comments-only; caller keeps `argv`.
 *   -1  error (logged); caller should bail with SETUP_ERROR.
 */
static int splice_args_file(const char* path, const char* label,
                            int argc, char** argv,
                            int* out_argc, char*** out_argv) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0; /* absent is the common no-override case */
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0 || sz > 4096) {
        fprintf(stderr, "openpete: %s %s rejected (size=%ld; cap 4096)\n",
                label, path, sz);
        fclose(f); return -1;
    }
    rewind(f);
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "openpete: short read on %s\n", path);
        fclose(f); free(buf); return -1;
    }
    buf[sz] = '\0';
    fclose(f);

    enum { MAX_SIDECAR_TOKENS = 64 };
    char** tokens = (char**)malloc(sizeof(char*) * MAX_SIDECAR_TOKENS);
    if (!tokens) { free(buf); return -1; }
    int ntoks = tokenize_sidecar_args(buf, tokens, MAX_SIDECAR_TOKENS);
    if (ntoks < 0) { free(buf); free(tokens); return -1; }
    if (ntoks == 0) {
        free(buf); free(tokens);
        return 0; /* empty/comments-only — nothing to splice */
    }

    int new_argc = argc + ntoks;
    char** new_argv = (char**)malloc(sizeof(char*) * (size_t)(new_argc + 1));
    if (!new_argv) { free(buf); free(tokens); return -1; }
    int o = 0;
    new_argv[o++] = argv[0];
    for (int i = 0; i < ntoks; i++) new_argv[o++] = tokens[i];
    for (int i = 1; i < argc; i++)  new_argv[o++] = argv[i];
    new_argv[o] = NULL;

    fprintf(stderr, "openpete: %s %s -> %d arg(s) prepended\n",
            label, path, ntoks);

    free(tokens);
    /* buf is intentionally retained — token pointers reference it. */

    *out_argc = new_argc;
    *out_argv = new_argv;
    return 1;
}

/* If argv contains "--bk2 PATH" and PATH.args exists on disk, splice its
 * tokens in after argv[0] (parsed before the user's flags — user CLI wins
 * scalar conflicts). See splice_args_file for the return contract. */
static int expand_bk2_sidecar(int argc, char** argv, int* out_argc, char*** out_argv) {
    int bk2_idx = -1;
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--bk2") == 0) { bk2_idx = i; break; }
    }
    if (bk2_idx < 0) return 0;

    const char* bk2_path = argv[bk2_idx + 1];
    size_t plen = strlen(bk2_path);
    char* args_path = (char*)malloc(plen + 6);
    if (!args_path) return -1;
    memcpy(args_path, bk2_path, plen);
    memcpy(args_path + plen, ".args", 6);

    int rc = splice_args_file(args_path, "bk2 sidecar", argc, argv, out_argc, out_argv);
    free(args_path);
    return rc;
}

/* If "openpete.opts" sits next to the executable, splice its tokens in
 * after argv[0]. These are the lowest-priority source — the bk2 sidecar
 * and explicit CLI both override (see openpete_options_parse for the
 * splice order). Lets a packaged build ship, or a user drop, persistent
 * flags (renderer, FOV, aspect, …) next to the binary without retyping
 * them every launch. Resolved via SDL_GetBasePath (the executable dir),
 * the same convention as the music wav-override search path. */
static int expand_options_file(int argc, char** argv, int* out_argc, char*** out_argv) {
    const char* base = SDL_GetBasePath(); /* SDL3: owned by SDL, do not free */
    if (!base) return 0; /* can't resolve exe dir — silently skip */

    static const char name[] = "openpete.opts";
    size_t blen = strlen(base);
    char* path = (char*)malloc(blen + sizeof(name)); /* sizeof includes NUL */
    if (!path) return -1;
    memcpy(path, base, blen);
    memcpy(path + blen, name, sizeof(name));

    int rc = splice_args_file(path, "options file", argc, argv, out_argc, out_argv);
    free(path);
    return rc;
}

void openpete_options_init(openpete_options_t* opts) {
    memset(opts, 0, sizeof(*opts));
    opts->display_name        = "OpenPete";
    opts->screenshot_frame_count = 0;
    opts->screenshot_ticks_keyed = false;
    opts->screenshot_output      = "./screenshot.png";
    opts->screenshot_presents_start = -1;
    opts->screenshot_presents_count = 1;
    opts->ram_dump_on_frame   = -1;
    opts->ram_dump_output     = "./ram.bin";
    opts->log_format          = OPENPETE_LOG_PLAIN;
    opts->renderer            = OPENPETE_RENDERER_VK;  /* native Vulkan is the default display (windowed); --renderer psycross for the legacy GL path. Inert under --headless (native disabled → PsyCross). */
    opts->mute                = false; /* audio on by default; --mute 1 to silence */
    opts->pgxp_enable         = -1;    /* -1 = unset; defaults applied at boot */
    opts->substitute_clamp_px = -1.0f; /* -1 = unset; use compiled default */
    opts->debug_highlight_smoothed = -1; /* -1 = unset; fall back to env */
    opts->debug_black_intermediates = -1; /* -1 = unset; fall back to env */
    opts->ssaa                = 1;     /* native vk supersampling: 1 = no-op */
    opts->fps_overlay         = true;  /* native vk FPS overlay on by default; --show-fps 0 / F11 */
    opts->tick_scale          = 1.0;   /* engine speed multiplier: 1.0 = real-time */
    opts->record_encoder      = "libx264";
    opts->record_crf          = 23;
    opts->record_gop          = 120;
    opts->record_pixfmt       = "yuv420p";
    opts->record_audio_encoder = "aac";
}

int openpete_options_parse(openpete_options_t* opts, int argc, char** argv) {
    /* Per-bk2 sidecar: <path>.bk2.args next to the movie can supply
     * scenario-specific defaults (e.g. --skip-to-level 10 for an
     * Artisans-only movie). Spliced before user argv so explicit CLI
     * flags still override. See games/spyro-1/bk2/README.md. */
    int sidecar_argc = 0;
    char** sidecar_argv = NULL;
    int sidecar_rc = expand_bk2_sidecar(argc, argv, &sidecar_argc, &sidecar_argv);
    if (sidecar_rc < 0) return OPENPETE_EXIT_SETUP_ERROR;
    if (sidecar_rc == 1) { argc = sidecar_argc; argv = sidecar_argv; }

    /* Options file next to the executable (openpete.opts): persistent
     * default flags. Spliced after the bk2 splice but still before argv[0]'s
     * trailing args, so the resulting precedence (left parsed first, last
     * write wins) is: options file < bk2 sidecar < explicit CLI. */
    int optfile_argc = 0;
    char** optfile_argv = NULL;
    int optfile_rc = expand_options_file(argc, argv, &optfile_argc, &optfile_argv);
    if (optfile_rc < 0) return OPENPETE_EXIT_SETUP_ERROR;
    if (optfile_rc == 1) { argc = optfile_argc; argv = optfile_argv; }

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
        } else if (strcmp(a, "--renderer") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            if      (strcmp(v, "psycross") == 0) opts->renderer = OPENPETE_RENDERER_PSYCROSS;
            else if (strcmp(v, "vk")       == 0) opts->renderer = OPENPETE_RENDERER_VK;
            else if (strcmp(v, "ab")       == 0) opts->renderer = OPENPETE_RENDERER_AB;
            else if (strcmp(v, "diff")     == 0) opts->renderer = OPENPETE_RENDERER_DIFF;
            else {
                fprintf(stderr, "openpete: --renderer expects psycross|vk|ab|diff, got: %s\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            i++;
        } else if (strcmp(a, "--render-paths") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->render_paths = v; i++;
        } else if (strcmp(a, "--ssaa") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->ssaa = atoi(v); i++;
            if (opts->ssaa < 1 || opts->ssaa > 32) {
                fprintf(stderr, "openpete: --ssaa expects 1..32 (1 = off), got: %s\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
        } else if (strcmp(a, "--render-stats") == 0) {
            opts->render_stats = true;
        } else if (strcmp(a, "--show-fps") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            /* Accept 0|1 (preferred) plus on|off, matching --mute. */
            if      (strcmp(v, "1") == 0 || strcmp(v, "on")  == 0) opts->fps_overlay = true;
            else if (strcmp(v, "0") == 0 || strcmp(v, "off") == 0) opts->fps_overlay = false;
            else {
                fprintf(stderr, "openpete: --show-fps expects 0|1 (on|off also accepted), got: %s\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            i++;
        } else if (strcmp(a, "--render-fps") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->render_fps = atoi(v); i++;
            if (opts->render_fps < 0) {
                fprintf(stderr, "openpete: --render-fps expects a non-negative integer\n");
                return OPENPETE_EXIT_SETUP_ERROR;
            }
        } else if (strcmp(a, "--tick-scale") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->tick_scale = atof(v); i++;
            if (!(opts->tick_scale > 0.0)) {
                fprintf(stderr, "openpete: --tick-scale expects a positive number (e.g. 0.5), got: %s\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
        } else if (strcmp(a, "--window") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            int w = 0, h = 0;
            if (sscanf(v, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) {
                fprintf(stderr, "openpete: --window expects WxH (e.g. 1280x720), got: %s\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            opts->window_w = w;
            opts->window_h = h;
            i++;
        } else if (strcmp(a, "--aspect") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            int aw = 0, ah = 0;
            if (sscanf(v, "%d:%d", &aw, &ah) != 2 || aw <= 0 || ah <= 0) {
                fprintf(stderr, "openpete: --aspect expects W:H (e.g. 16:9), got: %s\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            opts->aspect_w_over_h = (float)aw / (float)ah;
            i++;
        } else if (strcmp(a, "--fov") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->fov_deg = (float)atof(v); i++;
            if (opts->fov_deg < 30.0f || opts->fov_deg > 140.0f) {
                fprintf(stderr, "openpete: --fov clamped to [30,140]; got %.2f\n",
                        opts->fov_deg);
                if (opts->fov_deg < 30.0f) opts->fov_deg = 30.0f;
                if (opts->fov_deg > 140.0f) opts->fov_deg = 140.0f;
            }
        } else if (strcmp(a, "--no-lod") == 0) {
            opts->no_lod = true;
        } else if (strcmp(a, "--pgxp") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            int n = atoi(v);
            if (n != 0 && n != 1) {
                fprintf(stderr, "openpete: --pgxp expects 0 or 1, got: %s\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            opts->pgxp_enable = n; i++;
        } else if (strcmp(a, "--screenshot-on-frame") == 0 ||
                   strcmp(a, "--screenshot-on-tick") == 0) {
            bool ticks = (a[16] == 't');   /* "--screenshot-on-[t]ick" */
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            if (opts->screenshot_frame_count > 0 &&
                opts->screenshot_ticks_keyed != ticks) {
                fprintf(stderr, "openpete: --screenshot-on-frame and --screenshot-on-tick "
                                "are mutually exclusive\n");
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            opts->screenshot_frame_count = 0;
            opts->screenshot_ticks_keyed = ticks;
            const char* p = v;
            while (*p) {
                while (*p == ',' || *p == ' ') p++;
                if (!*p) break;
                if (opts->screenshot_frame_count >= OPENPETE_MAX_SCREENSHOT_FRAMES) {
                    fprintf(stderr, "openpete: %s supports up to %d frames\n",
                            a, OPENPETE_MAX_SCREENSHOT_FRAMES);
                    return OPENPETE_EXIT_SETUP_ERROR;
                }
                char* end = NULL;
                long n = strtol(p, &end, 10);
                if (end == p || n < 0) {
                    fprintf(stderr, "openpete: %s expects N[,N2,...], got: %s\n", a, v);
                    return OPENPETE_EXIT_SETUP_ERROR;
                }
                opts->screenshot_frames[opts->screenshot_frame_count++] = (int)n;
                p = end;
            }
            i++;
        } else if (strcmp(a, "--screenshot-output") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->screenshot_output = v; i++;
        } else if (strcmp(a, "--screenshot-presents-start") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->screenshot_presents_start = atoi(v); i++;
        } else if (strcmp(a, "--screenshot-presents-count") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->screenshot_presents_count = atoi(v); i++;
        } else if (strcmp(a, "--ram-dump-on-frame") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->ram_dump_on_frame = atoi(v); i++;
        } else if (strcmp(a, "--ram-dump-output") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->ram_dump_output = v; i++;
        } else if (strcmp(a, "--seed-ram") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->seed_ram_path = v; i++;
        } else if (strcmp(a, "--seed-cpu") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->seed_cpu_path = v; i++;
        } else if (strcmp(a, "--halt-at-pc") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->halt_at_pc = (uint32_t)strtoul(v, NULL, 0); i++;
        } else if (strcmp(a, "--halt-cpu-dump-output") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->halt_cpu_dump_output = v; i++;
        } else if (strcmp(a, "--halt-hit") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->halt_hit_target = (uint32_t)strtoul(v, NULL, 0); i++;
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
        } else if (strcmp(a, "--exe-path") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->exe_path = v; i++;
        } else if (strcmp(a, "--test-recomp-call") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->test_recomp_call_addr = (uint32_t)strtoul(v, NULL, 0); i++;
        } else if (strcmp(a, "--test-recomp-expect-v0") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->test_recomp_expected_v0 = (uint32_t)strtoul(v, NULL, 0); i++;
        } else if (strcmp(a, "--test-recomp-gp") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->test_recomp_gp = (uint32_t)strtoul(v, NULL, 0); i++;
        } else if (strcmp(a, "--probe") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->probe_addr = (uint32_t)strtoul(v, NULL, 0); i++;
        } else if (strcmp(a, "--test-segv") == 0) {
            opts->test_recomp_segv = true;
        } else if (strcmp(a, "--run-recomp") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->run_recomp_addr = (uint32_t)strtoul(v, NULL, 0); i++;
        } else if (strcmp(a, "--debug-triangle") == 0) {
            opts->debug_triangle = true;
        } else if (strcmp(a, "--skip-to-level") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->skip_to_level = game_resolve_level_arg(v);
            if (opts->skip_to_level < 0) {
                fprintf(stderr, "openpete: --skip-to-level: unknown level '%s'\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            i++;
        } else if (strcmp(a, "--bk2") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->bk2_path = v; i++;
        } else if (strcmp(a, "--bk2-frame-offset") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->bk2_frame_offset = atoi(v);
            opts->bk2_frame_offset_set = true;
            i++;
        } else if (strcmp(a, "--bk2-stop-on-end") == 0) {
            opts->bk2_stop_on_end = true;
        } else if (strcmp(a, "--bk2-record") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->bk2_record_path = v; i++;
        } else if (strcmp(a, "--record") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->record_path = v; i++;
        } else if (strcmp(a, "--record-encoder") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->record_encoder = v; i++;
        } else if (strcmp(a, "--record-crf") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->record_crf = atoi(v); i++;
        } else if (strcmp(a, "--record-gop") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->record_gop = atoi(v); i++;
            if (opts->record_gop < 0) {
                fprintf(stderr, "openpete: --record-gop expects a non-negative integer\n");
                return OPENPETE_EXIT_SETUP_ERROR;
            }
        } else if (strcmp(a, "--record-pixfmt") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->record_pixfmt = v; i++;
        } else if (strcmp(a, "--no-record-audio") == 0) {
            opts->record_no_audio = true;
        } else if (strcmp(a, "--record-audio-encoder") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->record_audio_encoder = v; i++;
        } else if (strcmp(a, "--unlock-frames") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->unlock_frames_spec = v;
            /* Parse "A-B,C-D,..." into ranges. Permissive: skip empty
             * tokens. Cap at OPENPETE_UNLOCK_RANGES_MAX. */
            opts->unlock_ranges_count = 0;
            const char* p = v;
            while (*p && opts->unlock_ranges_count < OPENPETE_UNLOCK_RANGES_MAX) {
                while (*p == ',' || *p == ' ') p++;
                if (!*p) break;
                char* end = NULL;
                long lo = strtol(p, &end, 10);
                if (end == p) break;
                p = end;
                long hi = lo;
                if (*p == '-') {
                    p++;
                    hi = strtol(p, &end, 10);
                    if (end == p) break;
                    p = end;
                }
                if (hi < lo) { long t = lo; lo = hi; hi = t; }
                opts->unlock_ranges[opts->unlock_ranges_count].lo = (int)lo;
                opts->unlock_ranges[opts->unlock_ranges_count].hi = (int)hi;
                opts->unlock_ranges_count++;
            }
            i++;
        } else if (strcmp(a, "--position-log") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->position_log_path = v; i++;
        } else if (strcmp(a, "--gameplay-hash-log") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->gameplay_hash_log_path = v; i++;
        } else if (strcmp(a, "--frame-time-log") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->frame_time_log_path = v; i++;
        } else if (strcmp(a, "--vertex-trace-log") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->vertex_trace_log_path = v; i++;
        } else if (strcmp(a, "--substitute-clamp-px") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->substitute_clamp_px = (float)atof(v); i++;
        } else if (strcmp(a, "--debug-highlight-smoothed") == 0) {
            opts->debug_highlight_smoothed = 1;  /* boolean flag, no value */
        } else if (strcmp(a, "--debug-black-intermediates") == 0) {
            opts->debug_black_intermediates = 1; /* boolean flag, no value */
        } else if (strcmp(a, "--psxtrace-verify") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->psxtrace_verify_path = v; i++;
        } else if (strcmp(a, "--psxtrace-keep-going") == 0) {
            opts->psxtrace_keep_going = true;
        } else if (strcmp(a, "--psxtrace-anchor-pc") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->psxtrace_anchor_pc = (uint32_t)strtoul(v, NULL, 0); i++;
        } else if (strcmp(a, "--psxtrace-anchor-hit") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->psxtrace_anchor_hit = (uint32_t)strtoul(v, NULL, 0); i++;
        } else if (strcmp(a, "--mute") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            /* Accept 0|1 (preferred) plus the legacy on|off so existing
             * scripts/movies keep working. */
            if      (strcmp(v, "1") == 0 || strcmp(v, "on")  == 0) opts->mute = true;
            else if (strcmp(v, "0") == 0 || strcmp(v, "off") == 0) opts->mute = false;
            else {
                fprintf(stderr, "openpete: --mute expects 0|1 (on|off also accepted), got: %s\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            i++;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            fprintf(stderr,
                "OpenPete %s\n"
                "Usage: %s [options]\n"
                "  An 'openpete.opts' file next to the executable supplies default flags\n"
                "  (whitespace/newline-separated, '#' comments). Explicit CLI flags override it.\n"
                "  --frame-limit N            exit after N rendered frames\n"
                "  --max-runtime-seconds N    wall-clock SIGTERM after N seconds\n"
                "  --headless                 no on-screen window (offscreen video driver)\n"
                "  --renderer MODE            display renderer: vk (native, full-window — default) | psycross (legacy GL) |\n"
                "                             ab (native|psycross side-by-side) | diff (|native - psycross| colour diff).\n"
                "                             Native modes ignored with --headless (falls back to psycross). In any native mode,\n"
                "                             --screenshot-on-frame dumps every variant: <out>-og/-vk/-ab/-diff.png.\n"
                "  --render-paths LIST        debug: render ONLY these paths in both renderers, e.g.\n"
                "                             cyclorama,spyro,moby,terrain,shadow,flame,particles. Lets a path be\n"
                "                             compared in isolation. Presentation-only; gameplay state unaffected.\n"
                "  --render-stats             debug: with --renderer diff, log per-frame + aggregate colour-diff\n"
                "                             statistics (mean/max/within-tol/diff-px); pair with --render-paths to\n"
                "                             measure one path's accuracy. Opt-in (per-frame GPU readback).\n"
                "  --render-fps N             target render Hz (0=match sim/30fps, >0=wall-clock dual-pacer)\n"
                "  --tick-scale F             engine speed x F (default 1.0); scales sim AND present cadence\n"
                "                             together so sub-tick frames-per-tick is invariant. 0.5 = half-\n"
                "                             speed slow-mo to inspect high-FPS interpolation. Display-only.\n"
                "  --ssaa N                   native vk supersampling 1..32 (default 1=off); renders the live\n"
                "                             window at NxN res and box-downsamples. Auto-capped to the GPU's\n"
                "                             max image size / VRAM. Display-only.\n"
                "  --show-fps 0|1             native vk FPS overlay (default 1 = on; on|off also accepted; F11 toggles live)\n"
                "  --window WxH               SDL window size (default 640x480; internal render still 512x240)\n"
                "  --aspect W:H               Hor+ widescreen (e.g. 16:9, 21:9; widens DISPENV/DRAWENV + GTE offset)\n"
                "  --fov DEG                  horizontal FOV override; intercepts SetGeomScreen (default ~50°, range 30-140)\n"
                "  --no-lod                   draw all visible sectors at high-poly (collapses sector LOD band; cull distance unchanged)\n"
                "  --pgxp 0|1                 PGXP sub-pixel vertex precision (default 0; --pgxp 1 enables perspective UV)\n"
                "  --screenshot-on-frame N[,N2,...]  save image at each listed frame; can be passed once with a comma list.\n"
                "  --screenshot-on-tick N[,N2,...]   same, keyed on canonical draw-ticks instead of VBL frames.\n"
                "                             Tick-keyed captures hit the same sim moment run-to-run (the\n"
                "                             frame<->tick mapping drifts with host timing); used by the visual gates.\n"
                "                             The PsyCross render is written as <out>-og.png; native modes add -vk/-ab/-diff.png\n"
                "  --screenshot-output PATH   screenshot path template (default ./screenshot.png); supports {n} for sim frame and {p} for present index; .png ext writes PNG, otherwise BMP\n"
                "  --screenshot-presents-start N  capture every present (canonical + intermediate) starting at sim frame N (default -1 = off)\n"
                "  --screenshot-presents-count K  number of consecutive presents to capture when --screenshot-presents-start is set (default 1)\n"
                "  --ram-dump-on-frame N      write 2MB main RAM to --ram-dump-output after frame N\n"
                "  --ram-dump-output PATH     RAM dump path (default ./ram.bin)\n"
                "  --seed-ram PATH            pre-fill g_ram from a 2MB dump before dispatch\n"
                "  --seed-cpu PATH            pre-fill CPUState (GPRs/pc/hi/lo/cop0/GTE) from 528B dump\n"
                "  --halt-at-pc HEX           dump RAM to --ram-dump-output at first call to fn HEX, then exit\n"
                "  --halt-cpu-dump-output PATH also write 528B 'CSv1' CPU dump alongside RAM at halt\n"
                "  --halt-hit N               dump on Nth (1-indexed) call to --halt-at-pc (default 1)\n"
                "  --psxtrace-verify PATH     consume per-frame .psxtrace + diff against PSX oracle\n"
                "  --psxtrace-keep-going      don't exit on first divergence (collect all)\n"
                "  --psxtrace-anchor-pc HEX   PC where advance() fires (default: trace's first-record pc)\n"
                "  --psxtrace-anchor-hit N    skip first N-1 hits before starting (default 1 = first)\n"
                "  --log-format json|plain    structured stderr (default plain)\n"
                "  --log-level SPEC           e.g. cd:debug,*:info; env OPENPETE_LOG=...\n"
                "  --disc PATH                disc image (BIN/CUE)\n"
                "  --exe-path PATH            direct PSX EXE bytes (overrides --disc)\n"
                "  --test-recomp-call HEX     boot, dispatch one fn at HEX, exit\n"
                "  --test-recomp-expect-v0 N  verify cpu->v0 after --test-recomp-call\n"
                "  --test-recomp-gp HEX       set cpu->gp before --test-recomp-call (skips PSYQ _96_init)\n"
                "  --probe HEX                read 4 bytes at this PSX vaddr post-dispatch and log\n"
                "  --test-segv                deliberately raise SIGSEGV (crash-handler smoke test)\n"
                "  --run-recomp HEX           dispatch fn at HEX in a fiber; main loop with VSync yields (defaults to the game's entry point)\n"
                "  --debug-triangle           emit a hardcoded POLY_F3 each frame (infra-only validation)\n"
                "  --bk2 PATH                 replay BizHawk movie (Octoshock P1 pad: buttons + analog sticks)\n"
                "  --bk2-frame-offset N       map bk2 frame 0 to openpete_frame N (default: -first_input)\n"
                "  --bk2-stop-on-end          exit after last bk2 frame instead of holding last input\n"
                "  --bk2-record PATH          write live pad input to a BizHawk .bk2 (combinable with --bk2 to extend a movie)\n"
                "  --record PATH              capture true-VFR gameplay video (.mkv recommended; .mp4 ok); reads the present FBO (no letterbox bars)\n"
                "  --record-encoder NAME      libavcodec encoder (default libx264; e.g. h264_nvenc, libopenh264)\n"
                "  --record-crf N             encoder quality for libx264/libx265 (default 23; lower = better)\n"
                "  --record-gop N             keyframe interval in frames (default 120)\n"
                "  --record-pixfmt NAME       encoder pixel format (default yuv420p)\n"
                "  --no-record-audio          record video only (default: also capture mixed SPU audio)\n"
                "  --record-audio-encoder NAME libavcodec audio encoder (default aac)\n"
                "  --skip-to-level L          load directly into level L: numeric id (10=Artisans, 63=Gnasty) or\n"
                "                             lower_snake_case name (stone_hill, doctor_shemp, gnasty_gnorc, ...);\n"
                "                             an unknown name prints the full list\n"
                "  --unlock-frames LIST       force fast-forward inside ranges, e.g. 100-200,500-600 (openpete_frame)\n"
                "  --position-log PATH        append CSV (frame,vbl,x,y,z) of g_Spyro.m_Position each yield\n"
                "  --gameplay-hash-log PATH   append CSV (frame,vbl,hex_hash) of FNV1a-64 over the\n"
                "                             gameplay-state region (manifest/gameplay_state_regions.toml).\n"
                "                             Primary determinism gate — see check_aspect_determinism.sh\n"
                "  --frame-time-log PATH      write per-present binary record (32B header + 32B/record)\n"
                "  --vertex-trace-log PATH    write per-matcher-batch binary trace (32B header + 72B/record) for scripts/analyze-vtrace.py\n"
                "  --substitute-clamp-px N    max per-axis per-vertex applied delta in pixels (default 0 = substitute off); >0 enables clamped extrapolation\n"
                "  --debug-highlight-smoothed paint actively-smoothed batches solid red (diagnostic); live-toggle F9\n"
                "  --debug-black-intermediates present a black prim instead of the interp OT on intermediate frames (presentation test); live-toggle F10\n"
                "                             for scripts/measure-pacing.py. Off by default.\n"
                "  --mute 0|1                 mute all audio at the OpenAL layer (default 0 = audio on; on|off also accepted)\n",
                opts->display_name, argv[0]);
            return OPENPETE_EXIT_SETUP_ERROR; /* treat as setup-only invocation */
        } else {
            fprintf(stderr, "openpete: unknown arg: %s\n", a);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
    }

    /* Required-flag validation. Without a dispatch path the runtime falls
     * through to an SDL spin loop that never enters game code, which is
     * silently confusing — surface the missing flag explicitly.
     *
     * Test/diagnostic modes that legitimately don't need --run-recomp
     * (--test-segv stays at the crash-handler smoke stage; --test-recomp-call
     * dispatches its own fn and exits) are exempt. Both still require an
     * EXE source (--disc or --exe-path) — except --test-segv which just
     * raises a signal and never touches the EXE. */
    bool is_segv_test = opts->test_recomp_segv;
    bool is_call_test = opts->test_recomp_call_addr != 0;

    if (!is_segv_test && !opts->disc_path && !opts->exe_path) {
        fprintf(stderr, "openpete: --disc PATH or --exe-path PATH is "
                        "required (no PSX EXE source given)\n");
        return OPENPETE_EXIT_SETUP_ERROR;
    }

    if (!is_segv_test && !is_call_test && opts->run_recomp_addr == 0) {
        if (opts->run_recomp_hint_addr != 0) {
            /* No explicit --run-recomp: dispatch the game's default entry
             * (the per-game hint). The user can still override on the CLI. */
            opts->run_recomp_addr = opts->run_recomp_hint_addr;
        } else {
            fprintf(stderr, "openpete: --run-recomp HEX is required (no entry "
                            "point given; pass the PSX vaddr of the game's main)\n");
            return OPENPETE_EXIT_SETUP_ERROR;
        }
    }

    return OPENPETE_EXIT_OK;
}
