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
    opts->screenshot_output   = "./shot.bmp";
    opts->ram_dump_on_frame   = -1;
    opts->ram_dump_output     = "./ram.bin";
    opts->log_format          = OPENPETE_LOG_PLAIN;
    opts->mute                = true;  /* silent by default; --mute off to enable audio */
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
        } else if (strcmp(a, "--render-fps") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->render_fps = atoi(v); i++;
            if (opts->render_fps < 0) {
                fprintf(stderr, "openpete: --render-fps expects a non-negative integer\n");
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
        } else if (strcmp(a, "--fov") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->fov_deg = (float)atof(v); i++;
            if (opts->fov_deg < 30.0f || opts->fov_deg > 140.0f) {
                fprintf(stderr, "openpete: --fov clamped to [30,140]; got %.2f\n",
                        opts->fov_deg);
                if (opts->fov_deg < 30.0f) opts->fov_deg = 30.0f;
                if (opts->fov_deg > 140.0f) opts->fov_deg = 140.0f;
            }
        } else if (strcmp(a, "--screenshot-on-frame") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->screenshot_on_frame = atoi(v); i++;
        } else if (strcmp(a, "--screenshot-output") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->screenshot_output = v; i++;
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
            opts->skip_to_level = atoi(v); i++;
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
        } else if (strcmp(a, "--mute") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            if      (strcmp(v, "on")  == 0) opts->mute = true;
            else if (strcmp(v, "off") == 0) opts->mute = false;
            else {
                fprintf(stderr, "openpete: --mute expects on|off, got: %s\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            i++;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            fprintf(stderr,
                "OpenPete %s\n"
                "Usage: %s [options]\n"
                "  --frame-limit N            exit after N rendered frames\n"
                "  --max-runtime-seconds N    wall-clock SIGTERM after N seconds\n"
                "  --headless                 no on-screen window (offscreen video driver)\n"
                "  --render-fps N             target render Hz (0=match sim/30fps, >0=wall-clock dual-pacer)\n"
                "  --window WxH               SDL window size (default 640x480; internal render still 512x240)\n"
                "  --fov DEG                  horizontal FOV override; intercepts SetGeomScreen (default ~50°, range 30-140)\n"
                "  --screenshot-on-frame N    save BMP of frame N to --screenshot-output then continue\n"
                "  --screenshot-output PATH   screenshot path (default ./shot.bmp)\n"
                "  --ram-dump-on-frame N      write 2MB main RAM to --ram-dump-output after frame N\n"
                "  --ram-dump-output PATH     RAM dump path (default ./ram.bin)\n"
                "  --seed-ram PATH            pre-fill g_ram from a 2MB dump before dispatch\n"
                "  --seed-cpu PATH            pre-fill CPUState (GPRs/pc/hi/lo/cop0/GTE) from 528B dump\n"
                "  --halt-at-pc HEX           dump RAM to --ram-dump-output at first call to fn HEX, then exit\n"
                "  --halt-cpu-dump-output PATH also write 528B 'CSv1' CPU dump alongside RAM at halt\n"
                "  --halt-hit N               dump on Nth (1-indexed) call to --halt-at-pc (default 1)\n"
                "  --log-format json|plain    structured stderr (default plain)\n"
                "  --log-level SPEC           e.g. cd:debug,*:info; env OPENPETE_LOG=...\n"
                "  --disc PATH                disc image (BIN/CUE)\n"
                "  --exe-path PATH            direct PSX EXE bytes (overrides --disc)\n"
                "  --test-recomp-call HEX     boot, dispatch one fn at HEX, exit\n"
                "  --test-recomp-expect-v0 N  verify cpu->v0 after --test-recomp-call\n"
                "  --test-recomp-gp HEX       set cpu->gp before --test-recomp-call (skips PSYQ _96_init)\n"
                "  --probe HEX                read 4 bytes at this PSX vaddr post-dispatch and log\n"
                "  --test-segv                deliberately raise SIGSEGV (crash-handler smoke test)\n"
                "  --run-recomp HEX           dispatch fn at HEX in a fiber; main loop with VSync yields\n"
                "  --debug-triangle           emit a hardcoded POLY_F3 each frame (infra-only validation)\n"
                "  --bk2 PATH                 replay BizHawk movie (Octoshock P1 digital pad)\n"
                "  --bk2-frame-offset N       map bk2 frame 0 to openpete_frame N (default: -first_input)\n"
                "  --bk2-stop-on-end          exit after last bk2 frame instead of holding last input\n"
                "  --bk2-record PATH          write live pad input to a BizHawk .bk2 (combinable with --bk2 to extend a movie)\n"
                "  --skip-to-level N          load directly into level N (e.g. 10=Artisans, 20=PeaceKeepers, 63=Gnasty)\n"
                "  --unlock-frames LIST       force fast-forward inside ranges, e.g. 100-200,500-600 (openpete_frame)\n"
                "  --position-log PATH        append CSV (frame,vbl,x,y,z) of g_Spyro.m_Position each yield\n"
                "  --mute on|off              mute all audio at the OpenAL layer (default on)\n",
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
            fprintf(stderr, "openpete: --run-recomp HEX is required (no entry "
                            "point given; e.g. --run-recomp 0x%08X for %s)\n",
                    opts->run_recomp_hint_addr,
                    opts->display_name ? opts->display_name : "this game");
        } else {
            fprintf(stderr, "openpete: --run-recomp HEX is required (no entry "
                            "point given; pass the PSX vaddr of the game's main)\n");
        }
        return OPENPETE_EXIT_SETUP_ERROR;
    }

    return OPENPETE_EXIT_OK;
}
