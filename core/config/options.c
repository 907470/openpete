// core/config/options.c — command-line flag parser.
//
// Unknown flags exit with OPENPETE_EXIT_SETUP_ERROR (3).

#include "openpete.h"
#include "openpete_config.h"  /* openpete.toml — the layered config file */
#include "openpete_keymap.h"  /* [keys] — install the SDL name lookups */
#include "openpete_overlay_anchor.h"  /* --perf-position / --osd-position spellings */
#include "openpete_osd.h"             /* OPENPETE_OSD_TIMEOUT_MAX_MS */
#include "openpete_log.h"
#include "openpete_time.h"  /* OP_TICK_HZ — --record-cfr NTSC-multiple snap */
#include "game_extras.h"  /* game_resolve_level_arg (--skip-to-level names),
                           * game_validate_cheats_arg (--cheats names) */

#include "openpete_paths.h" /* where --bk2-record output is allowed to live */

#include <SDL3/SDL.h>     /* SDL_CreateDirectory — --bk2-record output dir */

#include <ctype.h>      /* tolower — case-insensitive disc-image extensions */
#include <math.h>       /* lround/fabs — --record-cfr NTSC-multiple validation */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>       /* time/localtime/strftime — default --bk2-record filename */

/** @brief Fetch the token after argv[i] as the value of flag, or report it missing.
 *
 * Prints "openpete: missing value for <flag>" to stderr when argv[i] is the
 * last token. The value is taken verbatim: a token that starts with '-' is
 * accepted.
 * @param flag The flag's spelling, for the message.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @param i Index of the flag in argv.
 * @param out Receives argv[i + 1] on success; untouched on failure.
 * @return 0 on success, -1 when no token follows. */
static int needs_value(const char* flag, int argc, char** argv, int i, const char** out) {
    if (i + 1 >= argc) {
        fprintf(stderr, "openpete: missing value for %s\n", flag);
        return -1;
    }
    *out = argv[i + 1];
    return 0;
}

/** @brief Parse a flag's value as a finite number and clamp it to [lo, hi].
 *
 * The whole value must parse (strtod with an end-pointer check) and be
 * finite, so "nan", "inf" and "9O" are refused rather than reaching a
 * clamp that NaN would pass through. A value outside [lo, hi] is clamped
 * with a message naming the flag.
 * @param flag The flag's spelling, for the messages.
 * @param v The value text.
 * @param lo Lower bound.
 * @param hi Upper bound.
 * @param out Receives the clamped value.
 * @return 0 on success, -1 with a message on a refused value. */
static int parse_finite_clamped(const char* flag, const char* v, float lo, float hi,
                                float* out) {
    char* end = NULL;
    double d = strtod(v, &end);
    if (!v[0] || end == v || *end || !isfinite(d)) {
        fprintf(stderr, "openpete: %s expects a number in [%g,%g], got: %s\n",
                flag, (double)lo, (double)hi, v);
        return -1;
    }
    if (d < lo || d > hi) {
        fprintf(stderr, "openpete: %s clamped to [%g,%g]; got %.2f\n",
                flag, (double)lo, (double)hi, d);
        d = d < lo ? lo : hi;
    }
    *out = (float)d;
    return 0;
}

/** @brief Parse a --sfx, --music or --vibration value on the game's 0..10 slider scale.
 *
 * "off" is 0 and "on" is 10; -1 means the game's own setting stands. On
 * success writes the level to *out and returns 0; otherwise prints the
 * accepted vocabulary and returns -1.
 * @param flag The flag or env var name, for the message.
 * @param v The value text.
 * @param out Receives the level. */
static int parse_volume_arg(const char* flag, const char* v, int* out) {
    if (strcmp(v, "off") == 0)      { *out = 0;  return 0; }
    if (strcmp(v, "on")  == 0)      { *out = 10; return 0; }
    char* end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v || *end != '\0' || n < -1 || n > 10) {
        /* "setting", not "slider": this parser is shared with --vibration,
         * which is a toggle plus a host-side strength rather than an in-game
         * slider. */
        fprintf(stderr, "openpete: %s expects 0..10 (on|off also accepted, "
                        "-1 = leave the game's own setting alone), got: %s\n",
                flag, v);
        return -1;
    }
    *out = (int)n;
    return 0;
}

/** @brief Tokenize an args-file buffer in place.
 *
 * Whitespace separates tokens and '#' starts a comment that runs to the end
 * of the line. A double-quoted span may hold whitespace, and inside it a
 * backslash escapes a double quote or a backslash. The buffer is rewritten: each token is unescaped and
 * NUL-terminated where it lies. Returns the token count, or -1 when the
 * tokens exceed max_tokens or a quote is unterminated.
 * @param buf The NUL-terminated file contents.
 * @param tokens Receives a pointer into buf per token.
 * @param max_tokens Capacity of tokens. */
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
        /* A token that is empty or carries whitespace is written
         * double-quoted, with \" and \\ escapes inside. Unescaping runs in
         * place: `out` never outruns `p`, since every escape sequence is
         * longer than the byte it produces and a quote produces none. */
        char* out = p;
        int   quoted = 0;
        tokens[n++] = out;
        while (*p) {
            if (quoted) {
                if (*p == '"') { quoted = 0; p++; continue; }
                if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) {
                    *out++ = p[1]; p += 2; continue;
                }
            } else {
                if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') break;
                if (*p == '"') { quoted = 1; p++; continue; }
            }
            *out++ = *p++;
        }
        if (quoted) {
            fprintf(stderr, "openpete: unterminated quote in args file\n");
            return -1;
        }
        if (*p) p++;      /* step past the separator before it is overwritten */
        *out = '\0';
    }
    return n;
}

/** @brief Splice the tokens of an args file into argv right after argv[0].
 *
 * The file is at most 4096 bytes and 64 tokens, tokenized by
 * tokenize_sidecar_args. The tokens parse before the existing arguments, so
 * the existing arguments win scalar conflicts under last-write. Returns 1
 * when spliced, with *out_argc and *out_argv holding the new argv (heap
 * memory kept for the process lifetime, since the tokens point into a buffer
 * that outlives opts); 0 when the file is absent or holds no tokens, and the
 * caller keeps argv; -1 on an error, for which the caller returns
 * OPENPETE_EXIT_SETUP_ERROR (an oversized file, a short read and a tokenize
 * failure print a message; a seek or allocation failure does not).
 * @param path The args file.
 * @param label Names the source in messages.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @param out_argc Receives the new argument count.
 * @param out_argv Receives the new argument vector. */
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

/** @brief Splice PATH.args after argv[0] when argv holds "--bk2 PATH" and that file exists.
 *
 * The first --bk2 followed by a value names the movie. The sidecar tokens
 * parse before the typed flags, so a typed flag wins a scalar conflict.
 * Returns as splice_args_file does, or -1 on allocation failure, and 0 when
 * argv has no --bk2.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @param out_argc Receives the new argument count.
 * @param out_argv Receives the new argument vector. */
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

/* Persistent settings come from openpete.toml, resolved by
 * core/config/config.c, which materializes the winning layer as an argv
 * splice or a setenv before this parser runs. */

/* --bk2-record args sidecar helpers -----------------------------------
 *
 * When --bk2-record captures a session, we drop a "<bk2>.args" sidecar
 * mirroring the launch flags so `--bk2 <recording>` re-applies the same
 * scenario (see expand_bk2_sidecar). The captured flags come from the
 * *original* argv — before the openpete.toml / bk2-sidecar splices — so
 * only what the user typed on the command line is recorded, not the
 * machine-local persistent defaults. Every --bk2* flag is filtered out:
 * they control recording/replay, not the scenario. Re-emitting
 * --bk2-record would make the replay re-record, and re-emitting the
 * --bk2 replay flag would make a recording-while-replaying session try
 * to replay the *old* movie instead of the new one. */

/** @brief 1 when a token must be written double-quoted in an args sidecar.
 *
 * A token survives a bare whitespace split only if it is non-empty, does not
 * open a comment, and holds no whitespace, double quote or backslash.
 * sidecar_emit_token quotes and escapes every other token, and
 * tokenize_sidecar_args restores it. */
static int sidecar_needs_quotes(const char* a) {
    if (!*a || *a == '#') return 1;
    for (const char* q = a; *q; q++)
        if (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r' ||
            *q == '"' || *q == '\\')
            return 1;
    return 0;
}

/** @brief Length in bytes of a's token in the args sidecar, quotes and escapes included.
 *
 * Matches what sidecar_emit_token writes: strlen(a) for a bare token, else
 * the two enclosing quotes plus one byte per character, doubled for '"' and
 * '\'. The terminating NUL and the separator are not counted. */
static size_t sidecar_token_len(const char* a) {
    size_t n;
    if (!sidecar_needs_quotes(a)) return strlen(a);
    n = 2;  /* the enclosing quotes */
    for (const char* q = a; *q; q++) n += (*q == '"' || *q == '\\') ? 2 : 1;
    return n;
}

/** @brief Write a's sidecar token at p and return the position after it.
 *
 * A token that needs no quotes is copied as is; any other is written
 * double-quoted with '"' and '\' preceded by a backslash. No NUL is written
 * and the caller has reserved sidecar_token_len(a) bytes.
 * @param p Output cursor.
 * @param a Token to emit. */
static char* sidecar_emit_token(char* p, const char* a) {
    if (!sidecar_needs_quotes(a)) {
        size_t n = strlen(a);
        memcpy(p, a, n);
        return p + n;
    }
    *p++ = '"';
    for (const char* q = a; *q; q++) {
        if (*q == '"' || *q == '\\') *p++ = '\\';
        *p++ = *q;
    }
    *p++ = '"';
    return p;
}

/** @brief 1 when a begins with "--bk2", which covers every recording and replay flag. */
static int is_bk2_flag(const char* a) {
    return strncmp(a, "--bk2", 5) == 0;
}

/** @brief How many following tokens (0 or 1) a --bk2* flag consumes as its value.
 *
 * The sidecar exclusion walk skips the value with the flag. --bk2 and
 * --bk2-frame-offset always take one; --bk2-record takes the next token only
 * when it does not start with '-'.
 * @param a The --bk2* flag.
 * @param next The token after a, or NULL at the end of argv. */
static int bk2_flag_value_tokens(const char* a, const char* next) {
    if (strcmp(a, "--bk2") == 0) return 1;                       /* replay PATH */
    if (strcmp(a, "--bk2-frame-offset") == 0) return 1;          /* always a value */
    if (strcmp(a, "--bk2-record") == 0)                           /* optional value */
        return (next && next[0] != '-') ? 1 : 0;
    return 0;   /* --bk2-stop-on-end and any other flag-only --bk2* */
}

/** @brief Build the <bk2>.args sidecar text from the typed argv, without the --bk2* flags.
 *
 * argv is the command line before the openpete.toml and sidecar splices.
 * Every --bk2* flag and its value are dropped. The text is a '#' comment
 * header, then the tokens space-separated (quoted where
 * sidecar_needs_quotes says so), then a newline. Returns a heap buffer kept
 * for the process lifetime, or NULL when allocation fails.
 * @param argc Argument count.
 * @param argv The typed argument vector. */
static char* build_bk2_record_args(int argc, char** argv) {
    static const char header[] =
        "# openpete --bk2-record: launch args auto-captured for replay via --bk2\n"
        "# (excludes --bk2* flags and anything sourced from openpete.toml)\n";
    const size_t hlen = sizeof(header) - 1;  /* without NUL */

    size_t len = hlen;
    for (int i = 1; i < argc; i++) {
        if (is_bk2_flag(argv[i])) {
            i += bk2_flag_value_tokens(argv[i], (i + 1 < argc) ? argv[i + 1] : NULL);
            continue;
        }
        len += sidecar_token_len(argv[i]) + 1;  /* token + separator */
    }

    char* buf = (char*)malloc(len + 2);  /* +2: newline fixup + NUL */
    if (!buf) return NULL;
    char* p = buf;
    memcpy(p, header, hlen); p += hlen;
    for (int i = 1; i < argc; i++) {
        if (is_bk2_flag(argv[i])) {
            i += bk2_flag_value_tokens(argv[i], (i + 1 < argc) ? argv[i + 1] : NULL);
            continue;
        }
        p = sidecar_emit_token(p, argv[i]);
        *p++ = ' ';
    }
    if (p > buf && p[-1] == ' ') p[-1] = '\n';  /* last separator -> newline */
    else                        *p++  = '\n';   /* no tokens: header already ends '\n' */
    *p = '\0';
    return buf;
}

/** @brief 1 when a bare argument names an existing disc image file.
 *
 * The extension must be .cue, .bin, .img, .iso, .chd or .pbp, compared
 * case-insensitively, and the file must open for reading. Both tests are
 * required: an existence check alone would take a mistyped path, and an
 * extension check alone would take a mistyped flag value.
 * @param a The argument. */
static int arg_is_disc_image(const char* a) {
    const char* dot = strrchr(a, '.');
    static const char* const kExt[] = { ".cue", ".bin", ".img", ".iso",
                                        ".chd", ".pbp" };
    if (!dot) return 0;
    size_t i;
    int match = 0;
    for (i = 0; i < sizeof kExt / sizeof kExt[0] && !match; i++) {
        const char* e = kExt[i];
        size_t k;
        for (k = 0; e[k]; k++)
            if (tolower((unsigned char)dot[k]) != e[k]) break;
        match = (e[k] == '\0' && dot[k] == '\0');
    }
    if (!match) return 0;
    FILE* f = fopen(a, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/** @brief The default --bk2-record path, "<bk2 dir>/<YYYYmmdd-HHMMSS>.bk2", creating the directory.
 *
 * The bk2 directory is the "bk2" data directory from openpete_paths_data_dir,
 * or "bk2" relative to the working directory when none resolves. A failed
 * directory creation is logged and the path is still returned. Returns a
 * heap buffer kept for the process lifetime, or NULL when allocation
 * fails. */
static char* resolve_default_bk2_record_path(void) {
    char stamp[24];
    time_t now = time(NULL);
    /* localtime() (not _r/_s): option parsing is single-threaded and runs
     * once, so the shared-buffer caveat doesn't apply, and this avoids the
     * platform-specific reentrant variants. */
    struct tm* tmv = localtime(&now);
    if (!tmv || strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", tmv) == 0)
        snprintf(stamp, sizeof(stamp), "recording");

    /* Recordings are output, so the directory has to be writable: next to
     * the exe in a portable install, under the per-user data directory
     * otherwise (an install medium we cannot write is exactly the case
     * that used to fail here). */
    char resolved[1024];
    char* dir;
    if (openpete_paths_data_dir("bk2", resolved, sizeof resolved, 0) == 0) {
        dir = (char*)malloc(strlen(resolved) + 1);
        if (!dir) return NULL;
        strcpy(dir, resolved);
    } else {
        dir = (char*)malloc(4);  /* "bk2" + NUL — relative to CWD */
        if (!dir) return NULL;
        memcpy(dir, "bk2", 4);
    }
    if (!SDL_CreateDirectory(dir))  /* mkdir -p; true if it already exists */
        fprintf(stderr, "openpete: --bk2-record: could not create %s: %s\n",
                dir, SDL_GetError());

    size_t need = strlen(dir) + 1 /* '/' */ + strlen(stamp) + 4 /* ".bk2" */ + 1;
    char* path = (char*)malloc(need);
    if (!path) { free(dir); return NULL; }
    snprintf(path, need, "%s/%s.bk2", dir, stamp);
    free(dir);
    return path;
}

void openpete_options_set_default_renderer(openpete_options_t* opts,
                                           openpete_renderer_t r) {
    opts->renderer         = r;
    opts->default_renderer = r;
}

/** @brief Print the command-line help for `display_name`'s build to stderr.
 *
 * The release block of user-facing flags always prints and marks
 * `default_renderer` as the default renderer. The --record family prints
 * only under OPENPETE_HAS_LIBAV. The debug block of dev and diagnostic
 * flags is compiled out under OPENPETE_HIDE_DEBUG_FLAGS, set by the
 * *-shared distribution presets, and a one-line note replaces it. The
 * omission is display-only: every flag parses in every build.
 * @param display_name Game title printed in the heading.
 * @param argv0 Program path printed in the usage line.
 * @param default_renderer Renderer the game starts with when --renderer is absent. */
static void print_help(const char* display_name, const char* argv0,
                       openpete_renderer_t default_renderer) {
    fprintf(stderr,
        "OpenPete %s\n"
        "Usage: %s [options] [DISC-IMAGE]\n"
        "  A bare path to a .cue/.bin/.img/.iso/.chd/.pbp means --ingest, so opening a\n"
        "  disc image from a file manager or dragging it onto the program just works.\n"
        "  An 'openpete.toml' next to the executable and one in your config dir supply\n"
        "  persistent settings, in layers: global < [game.ID] < [disc.SERIAL] < [disc.SHA]\n"
        "  < [saved] < environment < bk2 sidecar < the flags you type. Inside a layer,\n"
        "  bare keys < [env] < [args]. --config-report shows what won and what it beat.\n"
        "  --config PATH  use only this file      --no-config  ignore every config file\n"
        "  --config-report  print the resolution table and exit\n"
        "  Keys and pad buttons rebind the same way, under [keys.pad] / [keys.host]\n"
        "  in any scope (e.g. [game.spyro-1.keys.host]). Bind a key by its name\n"
        "  (\"F5\", \"Left Shift\"), a pad input with the pad: prefix (\"pad:south\",\n"
        "  \"pad:-leftx\"), several with an array, or none with \"none\" to unbind.\n"
        "  --keys-report    print every action, its binding and where it came from, and exit\n"
        "  --disc PATH                disc image: the .cue, or the .bin/.img/.iso itself. A cue\n"
        "                             whose FILE line names a missing bin falls back to the cue's\n"
        "                             own basename and then to the lone .bin beside it.\n"
        "  --disc-cache 0|1           keep the disc image resident in memory, filled by a background\n"
        "                             loader (default 1): guest disc reads never wait on the file. 0 =\n"
        "                             read the file per sector (A/B). Bytes identical either way.\n"
        "  --load-delay N             VBLs of guest time every level load waits (the game's default\n"
        "                             when omitted; 0 = instant). Recorded into a bk2's header and\n"
        "                             checked on replay.\n"
        "  --run-recomp HEX           dispatch fn at HEX in a fiber; main loop with VSync yields (defaults to the game's entry point)\n"
        "  --renderer MODE            which rasterizer draws: vk (native Vulkan%s) | psycross\n"
        "                             (the GL reference%s). The native window displays either.\n"
        "  --window WxH               window size (default: the game's drawn viewport at 2x,\n"
        "                             640x448 on Spyro 1, 640x432 on Spyro 2 and 3)\n"
        "  --aspect W:H|auto          Hor+ widescreen. auto (default) = follow the window/fullscreen\n"
        "                             shape, filling it live on resize; W:H (e.g. 16:9, 21:9) = fixed\n"
        "                             render aspect (sizes the window; bars when the window mismatches)\n"
        "  --fov DEG                  vertical FOV of the visible band, native renderer (default 55°,\n"
        "                             range 30-120; the horizontal FOV follows the Hor+ aspect: 74° at\n"
        "                             4:3, 90° at 16:9 for the default). Inert under --renderer psycross.\n"
        "  --ssaa N                   native vk supersampling 1..4 (default 1=off); renders the live\n"
        "                             window at NxN res and box-downsamples. The 4x ceiling keeps the\n"
        "                             render targets inside an 8GB card at 1080p; higher factors are\n"
        "                             clamped, and the extent is further capped to the GPU's max image\n"
        "                             size. Display-only.\n"
        "  --vsync 0|1                native vk present mode (default 1 = vsync on / FIFO; 0 = vsync off\n"
        "                             for uncapped fps / low latency; on|off also accepted). Display-only.\n"
        "  --fullscreen 0|1           launch in borderless-desktop fullscreen (default 0 = windowed;\n"
        "                             on|off also accepted). Alt+Enter / double-click / the O-menu\n"
        "                             DISPLAY row toggle it live. Display-only.\n"
        "  --background [0|1]         open the window at its normal size and visible, but do not take\n"
        "                             focus, do not activate on show, and sit behind the other windows\n"
        "                             where the platform allows it. Presents, pacing and screenshots are\n"
        "                             unchanged; clicking the window or its taskbar button focuses it. Wins over\n"
        "                             --fullscreen. For an unattended windowed run alongside\n"
        "                             --input-lock (default 0 = off; ignored with --headless).\n"
        "                             Display-only.\n"
        "  --perf-stats LEVEL         on-screen performance stats: off | fps (default) | detailed.\n"
        "                             detailed adds the VBL rate, slips, late ticks per 1000 and the\n"
        "                             shed / interpolation share. 0|1|2 also accepted. Display-only.\n"
        "  --show-fps 0|1             deprecated spelling of --perf-stats off|fps (on|off also accepted)\n"
        "  --perf-position ANCHOR     where the stats block sits: top-right (default) | top-left | top-center | bottom-left | bottom-center | bottom-right\n"
        "  --osd on|off               on-screen notifications when a setting changes (default on)\n"
        "  --osd-position ANCHOR      where notifications stack; same six anchors (default top-left)\n"
        "  --osd-timeout SECONDS      how long a notification stays up (default 3.0, max 30)\n"
        "  --version-osd on|off       the release build's two-second version line at the bottom\n"
        "                             left (default on). A distributed build that is not a clean\n"
        "                             tag shows its version for the whole session and this does\n"
        "                             not suppress it; a locally built binary shows none.\n"
        "  --rewind N                 hold Backspace to rewind up to N seconds (default 0 = off;\n"
        "                             ~2 MiB RAM per second of window; same-level only)\n"
        "  --runahead N               lead the presented timeline by N sim ticks (default 0 = off).\n"
        "                             1 recommended: cancels the interpolation tick exactly — real-\n"
        "                             PSX input feel with high-fps motion, every animation frame\n"
        "                             shown. 2 = sub-console latency but input edges skip one\n"
        "                             animation onset frame (reads as 'uncanny'). Costs ~N extra\n"
        "                             sim ticks/tick of CPU; gameplay state is UNCHANGED.\n"
        "  --render-fps N             target render Hz (default: the display's refresh rate; 0=match\n"
        "                             sim/30fps legacy pacer, >0=wall-clock dual-pacer + interpolation)\n"
        "  --tick-scale F             engine speed x F (default 1.0); scales sim AND present cadence\n"
        "                             together so sub-tick frames-per-tick is invariant. 0.5 = half-\n"
        "                             speed slow-mo to inspect high-FPS interpolation. Display-only.\n"
        "  --lod MODE                 sector level-of-detail: original (default = authentic PSX LOD band) |\n"
        "                             extended (draw every visible sector at high-poly, out to the cull\n"
        "                             distance and beyond). Native renderer only. (--no-lod = extended.)\n"
        "  --terrain-visibility MODE  terrain candidate set: frustum (default) = every sector whose\n"
        "                             bounding sphere is inside the displayed frame, hidden or not, so\n"
        "                             the geometry the original hides (inside-geometry views, the side\n"
        "                             wedges wider than 4:3) draws and the depth buffer decides what is\n"
        "                             seen - a departure from the original presentation | game = the\n"
        "                             game's own visibility verdict (faithful). Native renderer only.\n"
        "  --terrain-cache 0|1        retained-terrain GPU cache: static sectors render from persistent\n"
        "                             GPU meshes with the per-face verdicts mirrored in the vertex\n"
        "                             stage (default 1; display-only; needs a hardware GPU with fp64,\n"
        "                             else the CPU walk runs as before). 0 = kill switch.\n"
        "  --fog-scale N              terrain fog-distance multiplier: 1 = authentic PSX curve, N = fog\n"
        "                             saturates N x farther (100 ~= fog off, the far-detail look).\n"
        "                             Default: 1, or 100 under --lod extended. Range [0.25,128].\n"
        "  --vertex-precision MODE    native vk vertex screen-coordinate precision: subpixel (default =\n"
        "                             smooth sub-pixel projection) | integer (snap 3D-world vertices to the\n"
        "                             PSX 512x240 pixel grid = the original PS1 polygon wobble). 2D HUD stays\n"
        "                             smooth; depth/UVs untouched. Native renderer only. Display-only.\n"
        "  --raster-quantize N        point-resample the 3D scene (pre-HUD) to N x the PSX raster (1 =\n"
        "                             authentic 512x240 grid, of which the 224 visible lines span the\n"
        "                             screen — the letterband rows are trimmed, so on-screen this is\n"
        "                             512x224 as on a real TV; every N stays aligned with the\n"
        "                             --vertex-precision integer grid). 0 = off. Default: 1 under\n"
        "                             --vertex-precision integer, else off. Display-only.\n"
        "  --extract-threads N        worker threads for the native render extraction (terrain sector\n"
        "                             walk). 0 = auto (logical cores / 2, max 8), 1 = serial (no\n"
        "                             threads). Output is byte-identical at any count (item-order\n"
        "                             merge). Boot-latched. Display-only.\n"
        "  --dither N                 1 = PS1 15-bit colour: quantize the 3D scene to RGB555 with the\n"
        "                             GPU's 4x4 ordered dither matrix on the PSX 512x240 framebuffer\n"
        "                             grid (the authentic banding + crosshatch the smooth float\n"
        "                             pipeline removed). 0 = off (default). Display-only.\n"
        "  --psycross-native-raster N 1 = render the PsyCross reference on the console's own 512x240\n"
        "                             display grid (one texel per PSX pixel, authentic aliasing) instead\n"
        "                             of a square-pixel 4:3 grid sized from the window. The window blit\n"
        "                             scales it up. Default 0. Reference renderer only.\n"
        "  --texture-mapping MODE     texture/colour interpolation: perspective (default = modern\n"
        "                             perspective-correct) | affine (PSX screen-linear = the original\n"
        "                             PS1 texture swim/warp). Native renderer only. Display-only.\n"
        "  --sfxpack-dir PATH         directory of drop-in sound-effect WAVs named by the sfx census\n"
        "                             (default: env OPENPETE_SFXPACK_DIR, else assets/sfx next to the exe)\n"
        "  --texpack-dir PATH         directory of HD texture-replacement PNGs (default: env OPENPETE_TEXPACK_DIR,\n"
        "                             else <exe>/assets/textures/). Native renderer only.\n"
        "  --controller-db PATH       extra SDL controller-mapping database, loaded after the shipped one and\n"
        "                             the per-user gamecontrollerdb.txt, so it outranks both (default: env\n"
        "                             OPENPETE_CONTROLLER_DB, else neither).\n"
        "  --mods-dir PATH            mods root: each subdir with a mod.toml is a mod, compiled on load;\n"
        "                             enabled.txt fixes order (= priority). Default: env OPENPETE_MODS_DIR,\n"
        "                             else <exe>/mods. No directory = no mods, byte-identical vanilla.\n"
        "  --mods ID[,ID...]          run exactly these mods, overriding mods/enabled.txt (order IS\n"
        "                             priority, later = higher). --mods \"\" enables none (vanilla run).\n"
        "                             Default: env OPENPETE_MODS, else enabled.txt.\n"
        "  --card1 PATH               use a specific .mcr memory-card image for port A (default: <card-dir>/card1.mcr)\n"
        "  --card2 PATH               use a specific .mcr memory-card image for port B (default: <card-dir>/card2.mcr)\n"
        "  --card-dir PATH            directory holding the memory-card .mcr images (default: env OPENPETE_CARD_DIR,\n"
        "                             else <exe>/cards for a portable install, or the per-user state dir\n"
        "                             openpete/cards. Two formatted cards inserted by default.\n"
        "  --no-card                  present no memory card in either port (saving disabled)\n"
        "  --card-unformatted         provision a blank unformatted card so the in-game format screen runs\n"
        "  --adopt-saves              re-tag memory-card saves to the RUNNING modset (save safety: a card played\n"
        "                             under a different modset warns on load until explicitly adopted)\n"
        "  --mute 0|1                 mute all audio at the OpenAL layer (default 0 = audio on; on|off also accepted)\n"
        "  --sfx N                    preset the game's SOUND EFFECTS slider: 0 (OFF) .. 10 (max), or on|off.\n"
        "                             Survives the title screen and a save-file load; the game's own options\n"
        "                             menu takes it back the moment you move that slider. -1 = don't preset\n"
        "  --music N                  preset the game's MUSIC slider, same scale and rules as --sfx\n"
        "  --vibration N              controller rumble: 0 (off) .. 10 (max), or on|off.\n"
        "                             Default -1 leaves the game's own setting alone (= on);\n"
        "                             0 silences the motors without changing that setting.\n"
        "                             10 is the full measured response. Always silent when\n"
        "                             headless or when the window is not focused.\n"
        "  --mouse 0|1                host mouse channel: 0 ignores every mouse movement, button and\n"
        "                             wheel event and leaves the pointer a free desktop cursor over the\n"
        "                             window; mouse look, the settings menu pointer and the dev overlay\n"
        "                             all go quiet (default 1 = on; on|off also accepted)\n"
        "  --input-lock [0|1]         ignore every host input device: keyboard (game keys and every\n"
        "                             hotkey), mouse (as --mouse 0) and gamepads. A --bk2 replay still\n"
        "                             drives the game and closing the window still exits. For a window\n"
        "                             that must not react to the desk it runs on (default 0 = off)\n"
        "  --mouse-sensitivity S      mouse look scale for the free camera and camlock (default 1.0;\n"
        "                             0.5 = one full turn per 8192 counts). Mouse look binds through\n"
        "                             [keys.host] look_axis_x = \"pad:rightx,mouse:x\" and\n"
        "                             look_axis_y = \"pad:-righty,mouse:-y\"; \"mouse:y\" inverts, \"none\" unbinds\n"
        "  --skip-to-level L          load directly into level L: the game's numeric level id or its\n"
        "                             lower_snake_case name; no value or an unknown name prints this\n"
        "                             game's list. Windowed runs fast-forward the logos/title/loading\n"
        "                             (black window, audio muted) until the level is playable\n"
        "  --skip-to-cutscene C       play the named cutscene C. No value or an unknown name prints this\n"
        "                             game's list. Spyro 1: intro (the opening, from the title through\n"
        "                             New Game; not with --skip-to-level), ending (Gnasty defeated, from\n"
        "                             level 63), ending_loot (Gnasty's Loot, from level 64; the credits\n"
        "                             return to Artisans), credits / credits_100 (the short / full 100%%\n"
        "                             roll, from --skip-to-level's target or Artisans; the roll returns\n"
        "                             to the level it started from). Spyro 2: intro, intro_arrival,\n"
        "                             summer_forest, crush_intro, crush_defeated, autumn_plains,\n"
        "                             gulp_intro, gulp_defeated, winter_tundra, ripto_intro, ending (the\n"
        "                             Dragon Shores theater's replay, from level 65; playback returns\n"
        "                             to Dragon Shores); LEVEL_intro (entered from the homeworld\n"
        "                             portal, first visit) and LEVEL_outro (the level completed and\n"
        "                             left) for LEVEL idol_springs, colossus, hurricos, aquaria_towers,\n"
        "                             sunny_beach, skelos_badlands, crystal_glacier, breeze_harbor,\n"
        "                             zephyr, scorch, shady_oasis, magma_cone, fracture_hills,\n"
        "                             mystic_marsh, cloud_temples, robotica_farms, metropolis. Spyro 3\n"
        "                             (the game's loading captions; each from its trigger's level, the\n"
        "                             chain continues as in the game):\n"
        "                             an_evil_plot_unfolds, a_powerful_villain_emerges,\n"
        "                             a_desperate_rescue_begins (from the title through New Game; not\n"
        "                             with --skip-to-level), bianca_strikes_back, the_second_warning,\n"
        "                             no_hard_feelings (10), spike_is_born, hunters_tussle,\n"
        "                             byrd_james_byrd (20), a_monster_to_end_all_monsters,\n"
        "                             an_apology_and_lunch, a_duplicitous_larcenous_ursine (30),\n"
        "                             the_escape (37), the_dancing_bear (40), deja_vu,\n"
        "                             a_familiar_face, billy_in_the_wall, one_less_noble_warrior (47),\n"
        "                             the_end (50)\n"
        "  --skip-to-area L:A         load level L (number or name) and enter its sub-area A through the\n"
        "                             game's own sub-area transition, as its area-0 portal does. A is\n"
        "                             an index from 1; no value or an invalid target prints each level's\n"
        "                             valid indices. Replaces --skip-to-level's level (Spyro 3)\n"
        "  --cheats A,B,...           apply the game's own built-in cheat codes without entering them on\n"
        "                             the pad: 99_lives, unlock_all_levels (an unknown name prints the\n"
        "                             list). Applied once a level is live, and again for each new session\n"
        "                             started from the title screen. Changes gameplay state by design\n"
        "  --bk2 PATH                 replay BizHawk movie (Octoshock P1 pad: buttons + analog sticks)\n"
        "  --bk2-frame-offset N       map bk2 frame 0 to openpete_frame N (default: -first_input)\n"
        "  --bk2-stop-on-end          exit after last bk2 frame instead of holding last input\n"
        "  --bk2-record [PATH]        write live pad input to a BizHawk .bk2 (combinable with --bk2 to extend a movie).\n"
        "                             With no PATH, records to <exe_dir>/bk2/<timestamp>.bk2. Also writes a\n"
        "                             <PATH>.args sidecar of the launch flags (minus --bk2*) for replay via --bk2.\n",
        display_name, argv0,
        default_renderer == OPENPETE_RENDERER_VK       ? " — default" : "",
        default_renderer == OPENPETE_RENDERER_PSYCROSS ? " — default" : "");

    /* The --record family only exists where libav* could be linked. Without
     * OPENPETE_HAS_LIBAV record.c is the warn-and-ignore stub, so listing these
     * would advertise a feature this build cannot perform. They still parse. */
#ifdef OPENPETE_HAS_LIBAV
    fputs(
        "  --record PATH              capture true-VFR gameplay video (.mkv recommended; .mp4 ok); reads the present FBO (no letterbox bars)\n"
        "  --record-encoder NAME      libavcodec encoder (default libx264; e.g. h264_nvenc, libopenh264)\n"
        "  --record-crf N             encoder quality for libx264/libx265 (default 23; lower = better)\n"
        "  --record-gop N             keyframe interval in frames (default 120)\n"
        "  --record-pixfmt NAME       encoder pixel format (default yuv420p)\n"
        "  --no-record-audio          record video only (default: also capture mixed SPU audio)\n"
        "  --record-audio-encoder NAME libavcodec audio encoder (default aac)\n"
        "  --record-fps-overlay 0|1   burn the native FPS overlay into --record video (default 0 = clean\n"
        "                             capture; on|off also accepted). Display-only.\n"
        "  --record-cfr RATE          capture at a constant frame rate instead of true VFR. RATE must be an\n"
        "                             integer multiple of the NTSC tick rate: 29.913, 59.826, 119.652, ...\n"
        "                             Drives the host clock from the pacer, so the same span of GAME time is\n"
        "                             captured however slowly the machine renders. Implies --no-record-audio.\n"
        "  --record-frames N          finish the recording and exit after exactly N encoded frames (with\n"
        "                             --record-cfr, a window of exactly N/RATE seconds of game time)\n",
        stderr);
#endif

#ifdef OPENPETE_HIDE_DEBUG_FLAGS
    fputs("  (plus dev/diagnostic flags, hidden in this build)\n", stderr);
#else
    fputs(
        "\nDebug / diagnostic flags:\n"
        "  --headless                 no on-screen window (offscreen video driver)\n"
        "  --resume-handoff PATH      resume a session from a process-handoff checkpoint\n"
        "                             (written by the handoff exec; not for manual use)\n"
        "  --frame-limit N            exit after N rendered frames\n"
        "  --max-runtime-seconds N    wall-clock SIGTERM after N seconds\n"
        "  --exe-path PATH            direct PSX EXE bytes (overrides --disc)\n"
        "  --ingest PATH              copy a disc image (.cue/.bin) into the library\n"
        "                             next to the exe and boot it; later runs need no\n"
        "                             --disc at all (the original file can be deleted)\n"
        "  --recomp-module PATH       shared-linkage builds: load the recomp module\n"
        "                             from PATH instead of the default next to the exe\n"
        "  --extract-recomp-inputs DIR extract PSX.EXE + overlay blobs from --disc\n"
        "                             into DIR and exit (recomp pipeline stage)\n"
        "  --compile-mods             compile EVERY mod with src/ in the mods dir (see --mods-dir;\n"
        "                             ignores enabled.txt) against <exe>/sdk and exit; nonzero exit\n"
        "                             on any failed compile (the cmake build-time mods gate)\n"
        "  --dump-sfx-pack DIR        write every game sound on --disc as a content-named WAV\n"
        "                             (+ index.txt) into DIR and exit: the sfx pack census\n"
        "  --dump-sfx-wads A.wad,B.wad also walk these custom-level Data files (a [[level]]\n"
        "                             mod's assets/levels/<stem>.wad)\n"
        "  --render-paths LIST        render ONLY these paths in both renderers, e.g.\n"
        "                             cyclorama,spyro,moby,terrain,shadow,flame,particles,glow,sparkle,\n"
        "                             tracer,transition,portal,moby_shadow. Lets a path be\n"
        "                             compared in isolation. Presentation-only; gameplay state unaffected.\n"
        "  --wireframe 0|1            1 = draw every 3D scene triangle as its three edges, coloured by the\n"
        "                             vertex lighting (textures replaced by neutral grey, as NOTEX); the\n"
        "                             HUD and menus stay filled. Shows the terrain subdivision as drawn.\n"
        "                             Default: env OPENPETE_WIREFRAME, else 0. Native renderer only.\n"
        "                             Display-only.\n"
        "  --devui-open 0|1           open the dev config overlay (M panel) as soon as it\n"
        "                             initializes instead of waiting for the M key, with the Mods\n"
        "                             list and every mod node expanded once, so an unattended run\n"
        "                             reaches a mod UI section body (on|off also accepted).\n"
        "                             Windowed native-vk runs only; inert elsewhere.\n"
        "  --log-format json|plain    structured stderr (default plain)\n"
        "  --log-level SPEC           e.g. cd:debug,*:info; env OPENPETE_LOG=...\n"
        "  --log-file PATH            mirror stderr to PATH instead of the default\n"
        "                             <state>/logs/openpete.log; env OPENPETE_LOG_FILE=...\n"
        "  --no-log-file              do not write a log file at all\n"
        "  --screenshot-on-frame N[,N2,...]  save image at each listed frame; can be passed once with a comma list.\n"
        "                             In a windowed pure-vk run, -vk.png is a READBACK of the exact\n"
        "                             presented frame (sub-tick interp, mods, overlays included);\n"
        "                             headless runs recompose the canonical scene instead.\n"
        "  --screenshot-on-tick N[,N2,...]   same, keyed on canonical draw-ticks instead of VBL frames.\n"
        "                             Tick-keyed captures hit the same sim moment run-to-run (the\n"
        "                             frame<->tick mapping drifts with host timing); used by the visual gates.\n"
        "                             The PsyCross render is written as <out>-og.png; the native render as <out>-vk.png\n"
        "  --screenshot-native-only   capture the native -vk variant alone: skip PsyCross OT rasterization and -og (presentation-golden corpus path)\n"
        "  --screenshot-intermediate-on-frame N[,...]  deterministic sub-tick (alpha 0.5) native captures at each VBL, headless-capable; implies --screenshot-native-only\n"
        "  --screenshot-freeze-backdrop  maintain the green freeze backdrop during headless capture so pause/results/exit dumps aren't dark (renders an extra frame per present)\n"
        "  --screenshot-output PATH   screenshot path template (default ./screenshot.png); supports {n} for sim frame and {p} for present index; .png ext writes PNG, otherwise BMP\n"
        "  --screenshot-presents-start N  capture every present (canonical + intermediate) starting at sim frame N (default -1 = off)\n"
        "  --screenshot-presents-count K  number of consecutive presents to capture when --screenshot-presents-start is set (default 1)\n"
        "  --ram-dump-on-frame N      write 2MB main RAM to --ram-dump-output after frame N\n"
        "  --ram-dump-output PATH     RAM dump path (default ./ram.bin)\n"
        "  --state-save-on-frame N    savestate to --state-save-output once openpete_frame (VBL) reaches N\n"
        "  --state-save-output PATH   scheduled-save path (default ./frameN.state)\n"
        "  --state-load-on-frame N    load --state-load-input once openpete_frame reaches N (re-anchors the frame counter)\n"
        "  --state-load-input PATH    state file for the scheduled load\n"
        "  --state-stress N           every N ticks: in-memory save + immediate quiet restore (restore-at-yield gate)\n"
        "  --state-stress-rewind K    save, run K iterations, restore, repeat (runahead rollback shape)\n"
        "  --rewind-hold A:N          headless: simulate holding the rewind key for N iterations at frame A\n"
        "  --seed-ram PATH            pre-fill g_ram from a 2MB dump before dispatch\n"
        "  --seed-cpu PATH            pre-fill CPUState (GPRs/pc/hi/lo/cop0/GTE) from 528B dump\n"
        "  --halt-at-pc HEX           dump RAM to --ram-dump-output at first call to fn HEX, then exit\n"
        "  --halt-cpu-dump-output PATH also write 528B 'CSv1' CPU dump alongside RAM at halt\n"
        "  --halt-hit N               dump on Nth (1-indexed) call to --halt-at-pc (default 1)\n"
        "  --psxtrace-verify PATH     consume per-frame .psxtrace + diff against PSX oracle\n"
        "  --psxtrace-keep-going      don't exit on first divergence (collect all)\n"
        "  --psxtrace-anchor-pc HEX   PC where advance() fires (default: trace's first-record pc)\n"
        "  --psxtrace-anchor-hit N    skip first N-1 hits before starting (default 1 = first)\n"
        "  --test-recomp-call HEX     boot, dispatch one fn at HEX, exit\n"
        "  --test-recomp-expect-v0 N  verify cpu->v0 after --test-recomp-call\n"
        "  --test-recomp-gp HEX       set cpu->gp before --test-recomp-call (skips PSYQ _96_init)\n"
        "  --probe HEX                read 4 bytes at this PSX vaddr post-dispatch and log\n"
        "  --test-segv                deliberately raise SIGSEGV (crash-handler smoke test)\n"
        "  --debug-triangle           emit a hardcoded POLY_F3 each frame (infra-only validation)\n"
        "  --freecam-pose STR         engage the debug free camera at a pose once the level is\n"
        "                             live (same format as the freecam log: \"pos=(x, y, z) yaw=N\n"
        "                             pitch=N\"). CLI twin of OPENPETE_FREECAM_POSE — env vars don't\n"
        "                             cross the WSL->Windows interop boundary.\n"
        "  --player-pose STR          place the player once gameplay is live and under pad control:\n"
        "                             \"x,y,z[,yaw]\" or the exit pose line (\"pos=(x, y, z) yaw=N\",\n"
        "                             yaw 4096 per turn); the follow camera is re-seated behind him.\n"
        "                             CLI twin of OPENPETE_PLAYER_POSE. Every run prints its end\n"
        "                             state as openpete-pose: after openpete-exit:.\n"
        "  --freecam-path SPEC        engage the free camera and fly it along a looping waypoint\n"
        "                             path: \"[ticks=N;] WP(;WP)+\" where WP is \"topdown\" or\n"
        "                             \"x,y,z[,yaw,pitch]\". Interpolates over N ticks per segment\n"
        "                             (default 120). Exercises the costs only movement reaches\n"
        "                             (sector-cache churn, LOD crossings) that a static pose cannot.\n"
        "                             CLI twin of OPENPETE_FREECAM_PATH; supersedes --freecam-pose.\n"        "  --unlock-frames LIST       force fast-forward inside ranges, e.g. 100-200,500-600 (openpete_frame)\n"
        "  --position-log PATH        append CSV (frame,vbl,x,y,z) of g_Spyro.m_Position each yield\n"
        "  --gameplay-hash-log PATH   append CSV (frame,vbl,hex_hash) of FNV1a-64 over the\n"
        "                             gameplay-state region (games/<game>/manifest/gameplay_state_regions.toml).\n"
        "                             Primary determinism gate — see check_aspect_determinism.sh\n"
        "  --frame-time-log PATH      write per-present binary record (32B header + 32B/record) for scripts/measure-pacing.py\n"
        "  --pace-log PATH            write the unified pacing event log (every VBL + present, one timebase) for scripts/pacing/\n"
        "  --profile-out PATH         stream a binary .opp perf trace (CPU scopes + GPU zones + census,\n"
        "                             keyed per present / canonical tick) for scripts/profile-summary.py\n"
        "                             (plain text) and scripts/profile-to-trace.py (Perfetto flame chart).\n"
        "                             Windowed only for GPU/intermediate coverage; ~0.1% CPU overhead.\n",
        stderr);
#endif
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
    opts->state_save_on_frame = -1;
    opts->state_save_output   = "./frameN.state";
    opts->state_load_on_frame = -1;
    opts->state_load_input    = NULL;
    opts->state_stress        = 0;
    opts->state_stress_rewind = 0;
    opts->rewind_window       = 0;
    opts->rewind_hold_frame   = -1;
    opts->rewind_hold_iters   = 0;
    opts->runahead            = 0;
    opts->log_format          = OPENPETE_LOG_PLAIN;
    opts->log_file            = NULL;  /* unset: env, else <state>/logs/openpete.log */
    opts->renderer            = OPENPETE_RENDERER_VK;  /* the native Vulkan renderer both rasterizes and displays; --renderer psycross keeps the GL reference as the rasterizer and displays its frame in the same native window. Inert under --headless, where native stays off and PsyCross rasterizes for the oracle. openpete_options_set_default_renderer, called before parsing, changes both this and default_renderer for a game that starts elsewhere. */
    opts->default_renderer    = OPENPETE_RENDERER_VK;
    opts->mute                = false; /* audio on by default; --mute 1 to silence */
    opts->sfx_volume          = -1;    /* -1 = unset; the game's own slider stands */
    opts->music_volume        = -1;
    opts->vibration           = -1;    /* -1 = unset; the game's own setting stands */
    opts->mouse               = -1;    /* -1 = unset; the mouse channel is on */
    opts->input_lock          = -1;    /* -1 = unset; host input is taken */
    opts->background          = -1;    /* -1 = unset; the window activates normally */
    opts->mouse_sensitivity   = -1.0f; /* -1 = unset (1.0) */
    opts->ssaa                = 1;     /* native vk supersampling: 1 = no-op */
    opts->perf_stats          = OPENPETE_PERF_FPS;  /* the counter, no pacing readout; --perf-stats / O-menu SHOW FPS row */
    /* Top-RIGHT: the stats block and the notifications both default to a
     * corner, and putting them in the same one would have every toast push
     * itself down past a readout the player is watching. Opposite corners is
     * the arrangement that needs no explanation. */
    opts->perf_position       = OPENPETE_ANCHOR_TOP_RIGHT;
    opts->osd                 = 1;                          /* toasts on */
    opts->osd_position        = OPENPETE_ANCHOR_TOP_LEFT;
    opts->osd_timeout_ms      = 3000u;
    opts->version_osd         = 1;                          /* version line on */
    opts->vsync               = -1;    /* -1 = unset; default vsync ON (FIFO) unless OPENPETE_PRESENT_MODE */
    opts->render_fps          = -1;    /* -1 = unset; resolved at startup: display refresh (windowed) /
                                        * 0 legacy match-sim pacer (headless). --render-fps 0 = explicit legacy. */
    opts->fullscreen          = 0;     /* windowed by default; --fullscreen 1 = borderless-desktop launch */
    opts->tick_scale          = 1.0;   /* engine speed multiplier: 1.0 = real-time */
    opts->record_encoder      = "libx264";
    opts->record_crf          = 23;
    opts->record_gop          = 120;
    opts->record_pixfmt       = "yuv420p";
    opts->record_audio_encoder = "aac";
    opts->record_fps_overlay  = -1;    /* -1 = unset; default off (env OPENPETE_FPS_IN_RECORD) */
    opts->devui_open          = -1;    /* -1 = unset; default closed (env OPENPETE_DEVUI_OPEN) */
    opts->record_cfr          = 0.0;   /* 0 = true VFR (wall-clock PTS) */
    opts->record_frames       = 0;     /* 0 = record until the run ends */
    opts->texpack_dir         = NULL;  /* NULL = unset; env OPENPETE_TEXPACK_DIR / built-in dir */
    opts->controller_db       = NULL;  /* NULL = unset; env OPENPETE_CONTROLLER_DB */
    opts->sfxpack_dir         = NULL;  /* NULL = unset; env OPENPETE_SFXPACK_DIR / built-in dir */
    opts->mods_dir            = NULL;  /* NULL = unset; env OPENPETE_MODS_DIR / <exe>/mods */
    opts->mods_enabled        = NULL;  /* NULL = unset; env OPENPETE_MODS / mods/enabled.txt */
    opts->compile_mods        = 0;     /* --compile-mods: compile every mod and exit */
    opts->dump_sfx_pack_dir   = NULL;
    opts->dump_sfx_wads       = NULL;
    opts->card_dir            = NULL;  /* NULL = default appdir; env OPENPETE_CARD_DIR */
    opts->card1_path          = NULL;
    opts->card2_path          = NULL;
    opts->card_a_present      = 1;     /* both cards inserted by default */
    opts->card_b_present      = 1;
    opts->card_unformatted    = 0;     /* pre-formatted empty card by default */
    opts->adopt_saves         = 0;     /* modset-mismatch saves warn until adopted */
    opts->resume_handoff      = NULL;  /* process-handoff resume file */
    opts->handoff_argc        = 0;     /* original argv (exec rebuild) */
    opts->handoff_argv        = NULL;
    opts->vertex_precision    = OPENPETE_VERTEX_PRECISION_SUBPIXEL;  /* smooth sub-pixel; --vertex-precision integer for the PSX polygon wobble */
    opts->raster_quantize = -1;   /* auto: 1x under --vertex-precision integer, else off */
    opts->extract_threads = 0;    /* auto: logical cores / 2, clamped to [1, 8] */
    opts->dither          = 0;    /* --dither 1 for PS1 15-bit colour + hardware dither */
    /* Wireframe view: OPENPETE_WIREFRAME is the env spelling; --wireframe,
     * parsed later, wins. */
    { const char* e = getenv("OPENPETE_WIREFRAME");
      opts->wireframe = (e && *e && *e != '0'); }
    opts->psycross_native_raster = 0;  /* reference renders on the window-derived square-pixel grid */
    opts->texture_mapping = OPENPETE_TEXTURE_MAPPING_PERSPECTIVE;  /* --texture-mapping affine for the PSX texture swim */
    /* Retained-terrain GPU cache, default ON; --terrain-cache 0 is the kill
     * switch. Display-only (the CPU walk stays the fallback for claimed
     * sectors, software vk, and no-fp64 devices). OPENPETE_TERRAIN_CACHE is
     * the env spelling (harness runs that can't pass flags); the CLI,
     * parsed later, wins. */
    { const char* e = getenv("OPENPETE_TERRAIN_CACHE");
      opts->terrain_cache = (e && *e) ? (atoi(e) != 0) : 1; }
    { const char* e = getenv("OPENPETE_DISC_CACHE");
      opts->disc_cache = (e && *e) ? (atoi(e) != 0) : 1; }
    { const char* e = getenv("OPENPETE_LOAD_DELAY");
      opts->load_delay = (e && *e) ? atoi(e) : -1; }
    opts->lod                 = OPENPETE_LOD_ORIGINAL;  /* authentic PSX LOD band; --lod extended (a.k.a. --no-lod) for full detail at any distance */
    /* --terrain-visibility: every sector inside the displayed frustum, hidden
     * or not (the departure), unless the env spelling names the faithful
     * path. A later --terrain-visibility on the command line wins. */
    { const char* e = getenv("OPENPETE_TERRAIN_VISIBILITY");
      opts->terrain_visibility = (e && *e && strcmp(e, "game") == 0)
                                 ? OPENPETE_TERRAIN_VIS_GAME
                                 : OPENPETE_TERRAIN_VIS_FRUSTUM; }
}

/* Facts the flag chain learns that are not options: the post-parse phases
 * consume them. */
typedef struct {
    /* --bk2-record was given, with or without a path argument (the default
     * path is resolved after the chain). */
    bool bk2_record_requested;
    /* --player-pose was typed: the CLI spelling rides the .args sidecar, the
     * env spelling does not. */
    bool player_pose_flag;
} parse_notes_t;

/** @brief Walk the spliced argv left to right, one arm per flag, the last write winning.
 *
 * An argument no arm accepts fails the parse; the message names the openpete.toml table when the token came from
 * the config splice. Facts that are not options go to notes.
 * @param opts The options block.
 * @param argc Argument count.
 * @param argv The spliced argument vector.
 * @param notes Receives the post-parse facts. */
// complexity-ratchet: one chain of sibling arms over argv, one per flag, where a new flag adds an arm and no other structure
static int parse_flag_chain(openpete_options_t* opts, int argc, char** argv,
                            parse_notes_t* notes) {
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
            else {
                fprintf(stderr, "openpete: --renderer expects psycross|vk, got: %s\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            i++;
        } else if (strcmp(a, "--render-paths") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->render_paths = v; i++;
        } else if (strcmp(a, "--ssaa") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->ssaa = atoi(v); i++;
            if (opts->ssaa < 1) {
                fprintf(stderr, "openpete: --ssaa expects 1..%d (1 = off), got: %s\n",
                        OPENPETE_SSAA_MAX, v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            /* Clamp rather than reject: a config file may carry a higher
             * factor, and refusing to start is worse than rendering at the
             * highest safe one. */
            if (opts->ssaa > OPENPETE_SSAA_MAX) {
                fprintf(stderr, "openpete: --ssaa %s exceeds the %dx maximum (VRAM); using %dx\n",
                        v, OPENPETE_SSAA_MAX, OPENPETE_SSAA_MAX);
                opts->ssaa = OPENPETE_SSAA_MAX;
            }
        } else if (strcmp(a, "--osd") == 0) {
            const char *v = (i + 1 < argc) ? argv[++i] : "";
            if      (!strcmp(v, "on")  || !strcmp(v, "1")) opts->osd = 1;
            else if (!strcmp(v, "off") || !strcmp(v, "0")) opts->osd = 0;
            else {
                fprintf(stderr, "openpete: --osd expects on|off, got: %s\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
        } else if (strcmp(a, "--version-osd") == 0) {
            const char *v = (i + 1 < argc) ? argv[++i] : "";
            if      (!strcmp(v, "on")  || !strcmp(v, "1")) opts->version_osd = 1;
            else if (!strcmp(v, "off") || !strcmp(v, "0")) opts->version_osd = 0;
            else {
                fprintf(stderr, "openpete: --version-osd expects on|off, got: %s\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
        } else if (strcmp(a, "--osd-position") == 0) {
            const char *v = (i + 1 < argc) ? argv[++i] : "";
            int an = openpete_anchor_parse(v);
            if (an < 0) {
                fprintf(stderr, "openpete: --osd-position expects one of "
                        "top-left|top-center|top-right|bottom-left|bottom-center|"
                        "bottom-right, got: %s\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            opts->osd_position = an;
        } else if (strcmp(a, "--osd-timeout") == 0) {
            /* Seconds in, milliseconds out — the ONE conversion point. */
            const char *v = (i + 1 < argc) ? argv[++i] : "";
            char *end = NULL;
            double secs = strtod(v, &end);
            if (!v[0] || end == v || *end || !(secs > 0.0) ||
                secs > (double)OPENPETE_OSD_TIMEOUT_MAX_MS / 1000.0) {
                fprintf(stderr, "openpete: --osd-timeout expects seconds in "
                        "(0, %.0f], got: %s\n",
                        (double)OPENPETE_OSD_TIMEOUT_MAX_MS / 1000.0, v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            opts->osd_timeout_ms = (uint32_t)(secs * 1000.0 + 0.5);
        } else if (strcmp(a, "--perf-position") == 0) {
            const char *v = (i + 1 < argc) ? argv[++i] : "";
            int an = openpete_anchor_parse(v);
            if (an < 0) {
                fprintf(stderr, "openpete: --perf-position expects one of "
                        "top-left|top-center|top-right|bottom-left|bottom-center|"
                        "bottom-right, got: %s\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            opts->perf_position = an;
        } else if (strcmp(a, "--perf-stats") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            /* Words are the documented spelling (and what the config file
             * writes back); the numbers are accepted because the level is an
             * ordered scale and scripts reach for it that way. */
            if      (!strcmp(v, "off")      || !strcmp(v, "0")) opts->perf_stats = OPENPETE_PERF_OFF;
            else if (!strcmp(v, "fps")      || !strcmp(v, "1")) opts->perf_stats = OPENPETE_PERF_FPS;
            else if (!strcmp(v, "detailed") || !strcmp(v, "2")) opts->perf_stats = OPENPETE_PERF_DETAIL;
            else {
                fprintf(stderr, "openpete: --perf-stats expects off|fps|detailed (0|1|2 also accepted), got: %s\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            i++;
        } else if (strcmp(a, "--show-fps") == 0) {
            /* The two-state spelling this setting used to have. ON restores
             * the FPS level rather than the detailed one: a flag that only
             * ever meant "the counter" must not start turning on a readout
             * its author never asked for. */
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            /* Accept 0|1 (preferred) plus on|off, matching --mute. */
            if      (strcmp(v, "1") == 0 || strcmp(v, "on")  == 0) opts->perf_stats = OPENPETE_PERF_FPS;
            else if (strcmp(v, "0") == 0 || strcmp(v, "off") == 0) opts->perf_stats = OPENPETE_PERF_OFF;
            else {
                fprintf(stderr, "openpete: --show-fps expects 0|1 (on|off also accepted), got: %s\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            i++;
        } else if (strcmp(a, "--vsync") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            /* Accept 0|1 (preferred) plus on|off, matching --show-fps / --mute. */
            if      (strcmp(v, "1") == 0 || strcmp(v, "on")  == 0) opts->vsync = 1;
            else if (strcmp(v, "0") == 0 || strcmp(v, "off") == 0) opts->vsync = 0;
            else {
                fprintf(stderr, "openpete: --vsync expects 0|1 (on|off also accepted), got: %s\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            i++;
        } else if (strcmp(a, "--fullscreen") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            /* Accept 0|1 (preferred) plus on|off, matching --vsync / --mute. */
            if      (strcmp(v, "1") == 0 || strcmp(v, "on")  == 0) opts->fullscreen = 1;
            else if (strcmp(v, "0") == 0 || strcmp(v, "off") == 0) opts->fullscreen = 0;
            else {
                fprintf(stderr, "openpete: --fullscreen expects 0|1 (on|off also accepted), got: %s\n", v);
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
            if (strcmp(v, "auto") == 0) {
                /* Explicit auto = same as unset: the render aspect follows the
                 * window / fullscreen shape (Hor+ fill, the default). */
                opts->aspect_w_over_h = 0.0f;
                i++;
            } else {
                int aw = 0, ah = 0;
                if (sscanf(v, "%d:%d", &aw, &ah) != 2 || aw <= 0 || ah <= 0) {
                    fprintf(stderr, "openpete: --aspect expects W:H (e.g. 16:9) or auto, got: %s\n", v);
                    return OPENPETE_EXIT_SETUP_ERROR;
                }
                opts->aspect_w_over_h = (float)aw / (float)ah;
                i++;
            }
        } else if (strcmp(a, "--fov") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            if (parse_finite_clamped(a, v, 30.0f, 120.0f, &opts->fov_deg) != 0)
                return OPENPETE_EXIT_SETUP_ERROR;
            i++;
        } else if (strcmp(a, "--vertex-precision") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            if (strcmp(v, "subpixel") == 0) {
                opts->vertex_precision = OPENPETE_VERTEX_PRECISION_SUBPIXEL;
            } else if (strcmp(v, "integer") == 0) {
                opts->vertex_precision = OPENPETE_VERTEX_PRECISION_INTEGER;
            } else {
                fprintf(stderr, "openpete: --vertex-precision expects subpixel|integer, got: %s\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            i++;
        } else if (strcmp(a, "--raster-quantize") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            long mult = strtol(v, NULL, 10);
            if (mult < 0 || mult > 16) {
                fprintf(stderr, "openpete: --raster-quantize expects 0 (off) or a grid multiplier in [1,16], got: %s\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            opts->raster_quantize = (int)mult;
            i++;
        } else if (strcmp(a, "--extract-threads") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            long n = strtol(v, NULL, 10);
            if (n < 0 || n > 8) {
                fprintf(stderr, "openpete: --extract-threads expects 0 (auto), 1 (serial) or a count in [2,8], got: %s\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            opts->extract_threads = (int)n;
            i++;
        } else if (strcmp(a, "--dither") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->dither = (atoi(v) != 0);
            i++;
        } else if (strcmp(a, "--wireframe") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->wireframe = (atoi(v) != 0);
            i++;
        } else if (strcmp(a, "--psycross-native-raster") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->psycross_native_raster = (atoi(v) != 0);
            i++;
        } else if (strcmp(a, "--texture-mapping") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            if (strcmp(v, "perspective") == 0) {
                opts->texture_mapping = OPENPETE_TEXTURE_MAPPING_PERSPECTIVE;
            } else if (strcmp(v, "affine") == 0) {
                opts->texture_mapping = OPENPETE_TEXTURE_MAPPING_AFFINE;
            } else {
                fprintf(stderr, "openpete: --texture-mapping expects perspective|affine, got: %s\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            i++;
        } else if (strcmp(a, "--lod") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            if (strcmp(v, "original") == 0) {
                opts->lod = OPENPETE_LOD_ORIGINAL;
            } else if (strcmp(v, "extended") == 0) {
                opts->lod = OPENPETE_LOD_EXTENDED;
            } else {
                fprintf(stderr, "openpete: --lod expects original|extended, got: %s\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            i++;
        } else if (strcmp(a, "--terrain-visibility") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            if (strcmp(v, "game") == 0) {
                opts->terrain_visibility = OPENPETE_TERRAIN_VIS_GAME;
            } else if (strcmp(v, "frustum") == 0) {
                opts->terrain_visibility = OPENPETE_TERRAIN_VIS_FRUSTUM;
            } else {
                fprintf(stderr, "openpete: --terrain-visibility expects game|frustum, got: %s\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            i++;
        } else if (strcmp(a, "--no-lod") == 0) {
            opts->lod = OPENPETE_LOD_EXTENDED;   /* deprecated alias for --lod extended */
        } else if (strcmp(a, "--disc-cache") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->disc_cache = atoi(v) != 0; i++;
        } else if (strcmp(a, "--load-delay") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->load_delay = atoi(v); i++;
            if (opts->load_delay < 0) {
                fprintf(stderr, "openpete: --load-delay must be >= 0\n");
                return OPENPETE_EXIT_SETUP_ERROR;
            }
        } else if (strcmp(a, "--terrain-cache") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->terrain_cache = atoi(v) != 0; i++;
        } else if (strcmp(a, "--fog-scale") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            if (parse_finite_clamped(a, v, 0.25f, 128.0f, &opts->fog_scale) != 0)
                return OPENPETE_EXIT_SETUP_ERROR;
            i++;
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
        } else if (strcmp(a, "--screenshot-intermediate-on-frame") == 0) {
            /* Deterministic sub-tick (alpha 0.5) native capture targets, VBL-keyed.
             * Fills a separate list from --screenshot-on-frame; implies native-only. */
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->screenshot_intermediate_count = 0;
            const char* p = v;
            while (*p) {
                while (*p == ',' || *p == ' ') p++;
                if (!*p) break;
                if (opts->screenshot_intermediate_count >= OPENPETE_MAX_SCREENSHOT_FRAMES) {
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
                opts->screenshot_intermediate_frames[opts->screenshot_intermediate_count++] = (int)n;
                p = end;
            }
            opts->screenshot_native_only = true;   /* intermediate capture is vk-only */
            i++;
        } else if (strcmp(a, "--screenshot-native-only") == 0) {
            opts->screenshot_native_only = true;
        } else if (strcmp(a, "--screenshot-freeze-backdrop") == 0) {
            opts->screenshot_freeze_backdrop = true;
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
        } else if (strcmp(a, "--state-save-on-frame") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->state_save_on_frame = atoi(v); i++;
        } else if (strcmp(a, "--state-save-output") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->state_save_output = v; i++;
        } else if (strcmp(a, "--state-load-on-frame") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->state_load_on_frame = atoi(v); i++;
        } else if (strcmp(a, "--state-load-input") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->state_load_input = v; i++;
        } else if (strcmp(a, "--state-stress") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->state_stress = atoi(v); i++;
        } else if (strcmp(a, "--state-stress-rewind") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->state_stress_rewind = atoi(v); i++;
        } else if (strcmp(a, "--rewind") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->rewind_window = atoi(v); i++;
        } else if (strcmp(a, "--runahead") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->runahead = atoi(v);
            if (opts->runahead < 0) opts->runahead = 0;
            if (opts->runahead > 3) opts->runahead = 3;
            i++;
        } else if (strcmp(a, "--rewind-hold") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            if (sscanf(v, "%d:%d", &opts->rewind_hold_frame,
                       &opts->rewind_hold_iters) != 2) {
                fprintf(stderr, "openpete: --rewind-hold expects A:N, got: %s\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            i++;
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
        } else if (strcmp(a, "--log-file") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->log_file = v; i++;
        } else if (strcmp(a, "--no-log-file") == 0) {
            opts->log_file = "off";
        } else if (strcmp(a, "--disc") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->disc_path = v; i++;
        } else if (strcmp(a, "--exe-path") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->exe_path = v; i++;
        } else if (strcmp(a, "--recomp-module") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->recomp_module_path = v; i++;
        } else if (strcmp(a, "--ingest") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->ingest_path = v; i++;
        } else if (strcmp(a, "--extract-recomp-inputs") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->extract_recomp_inputs_dir = v; i++;
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
        } else if (strcmp(a, "--freecam-path") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
#ifdef _WIN32
            _putenv_s("OPENPETE_FREECAM_PATH", v);
#else
            setenv("OPENPETE_FREECAM_PATH", v, 1);
#endif
            i++;
        } else if (strcmp(a, "--freecam-pose") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
#ifdef _WIN32
            _putenv_s("OPENPETE_FREECAM_POSE", v);
#else
            setenv("OPENPETE_FREECAM_POSE", v, 1);
#endif
            i++;
        } else if (strcmp(a, "--player-pose") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
#ifdef _WIN32
            _putenv_s("OPENPETE_PLAYER_POSE", v);
#else
            setenv("OPENPETE_PLAYER_POSE", v, 1);
#endif
            notes->player_pose_flag = true;
            i++;
        } else if (strcmp(a, "--debug-triangle") == 0) {
            opts->debug_triangle = true;
        } else if (strcmp(a, "--skip-to-level") == 0) {
            /* No value (end of argv, or the next word is a flag): print the
             * target list, as --help does. Level names never start with '-'. */
            if (i + 1 >= argc || argv[i + 1][0] == '-') {
                (void)game_resolve_level_arg("");
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            v = argv[i + 1];
            opts->skip_to_level = game_resolve_level_arg(v);
            if (opts->skip_to_level < 0) {
                fprintf(stderr, "openpete: --skip-to-level: unknown level '%s'\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            i++;
        } else if (strcmp(a, "--skip-to-cutscene") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') {
                (void)game_resolve_cutscene_arg("");
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            v = argv[i + 1];
            opts->skip_to_cutscene = game_resolve_cutscene_arg(v);
            if (opts->skip_to_cutscene < 0) {
                fprintf(stderr, "openpete: --skip-to-cutscene: unknown cutscene '%s'\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            i++;
        } else if (strcmp(a, "--skip-to-area") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') {
                (void)game_resolve_area_arg("", &opts->skip_to_area);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            v = argv[i + 1];
            opts->skip_to_area_level = game_resolve_area_arg(v, &opts->skip_to_area);
            if (opts->skip_to_area_level < 0) {
                fprintf(stderr, "openpete: --skip-to-area: no sub-area '%s'\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            i++;
        } else if (strcmp(a, "--cheats") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            if (game_validate_cheats_arg(v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->cheats = v; i++;
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
            /* Optional PATH: consume the next token only when it isn't
             * another flag. With no path, the default
             * "<bk2 data dir>/<timestamp>.bk2" is resolved after the loop. */
            notes->bk2_record_requested = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                opts->bk2_record_path = argv[i + 1]; i++;
            }
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
        } else if (strcmp(a, "--devui-open") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            if      (strcmp(v, "1") == 0 || strcmp(v, "on")  == 0) opts->devui_open = 1;
            else if (strcmp(v, "0") == 0 || strcmp(v, "off") == 0) opts->devui_open = 0;
            else {
                fprintf(stderr, "openpete: --devui-open expects 0|1 (on|off also accepted), got: %s\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            i++;
        } else if (strcmp(a, "--record-fps-overlay") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            /* Accept 0|1 (preferred) plus on|off, matching --vsync. */
            if      (strcmp(v, "1") == 0 || strcmp(v, "on")  == 0) opts->record_fps_overlay = 1;
            else if (strcmp(v, "0") == 0 || strcmp(v, "off") == 0) opts->record_fps_overlay = 0;
            else {
                fprintf(stderr, "openpete: --record-fps-overlay expects 0|1 (on|off also accepted), got: %s\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            i++;
        } else if (strcmp(a, "--record-cfr") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            double rate = atof(v);
            /* Only exact multiples of the NTSC tick rate are representable as a
             * whole number of presents per sim tick; anything else would have
             * the interpolator emit 2 intermediates on some ticks and 3 on
             * others. Reject rather than round, so a typo is loud. */
            double mult = rate / OP_TICK_HZ;
            long   k    = lround(mult);
            /* Relative tolerance so the rounded spellings people actually type
             * — 30, 60, 120 — all land on their NTSC neighbour, while a genuine
             * non-multiple like 45 is refused. 60 sits 0.29 % above 2x (the
             * true tick rate is 29.913, not 30), so the window has to clear
             * that; 0.5 % does, and still rejects 45 (50 % out) and 61 (2 %). */
            if (rate <= 0.0 || k < 1 || k > OPENPETE_RECORD_CFR_MAX_MULT ||
                fabs(mult - (double)k) > 0.005 * (double)k) {
                fprintf(stderr,
                        "openpete: --record-cfr expects an integer multiple of the NTSC "
                        "tick rate 1125000/37609 (29.913, 59.826, 119.652, ... up to %dx), "
                        "got: %s\n",
                        OPENPETE_RECORD_CFR_MAX_MULT, v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            opts->record_cfr = (double)k * OP_TICK_HZ;
            if (fabs(opts->record_cfr - rate) > 0.001)
                fprintf(stderr, "openpete: --record-cfr %s -> %.6f fps (NTSC %ldx)\n",
                        v, opts->record_cfr, k);
            i++;
        } else if (strcmp(a, "--record-frames") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->record_frames = atoi(v); i++;
            if (opts->record_frames < 0) {
                fprintf(stderr, "openpete: --record-frames expects a non-negative integer\n");
                return OPENPETE_EXIT_SETUP_ERROR;
            }
        } else if (strcmp(a, "--sfxpack-dir") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->sfxpack_dir = v; i++;
        } else if (strcmp(a, "--texpack-dir") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->texpack_dir = v; i++;
        } else if (strcmp(a, "--controller-db") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->controller_db = v; i++;
        } else if (strcmp(a, "--mods-dir") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->mods_dir = v; i++;
        } else if (strcmp(a, "--mods") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->mods_enabled = v; i++;
        } else if (strcmp(a, "--compile-mods") == 0) {
            opts->compile_mods = 1;
        } else if (strcmp(a, "--dump-sfx-pack") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->dump_sfx_pack_dir = v; i++;
        } else if (strcmp(a, "--dump-sfx-wads") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->dump_sfx_wads = v; i++;
        } else if (strcmp(a, "--config") == 0) {
            /* Consumed by the config loader before this parse; accepted
             * here so it isn't an unknown flag. */
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            i++;
        } else if (strcmp(a, "--no-config") == 0) {
            /* likewise */
        } else if (strcmp(a, "--config-report") == 0) {
            opts->config_report = 1;
        } else if (strcmp(a, "--keys-report") == 0) {
            opts->keys_report = 1;
        } else if (strcmp(a, "--card-dir") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->card_dir = v; i++;
        } else if (strcmp(a, "--card1") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->card1_path = v; i++;
        } else if (strcmp(a, "--card2") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->card2_path = v; i++;
        } else if (strcmp(a, "--no-card") == 0) {
            opts->card_a_present = 0;
            opts->card_b_present = 0;
        } else if (strcmp(a, "--card-unformatted") == 0) {
            opts->card_unformatted = 1;
        } else if (strcmp(a, "--adopt-saves") == 0) {
            opts->adopt_saves = 1;
        } else if (strcmp(a, "--resume-handoff") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->resume_handoff = v; i++;
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
        } else if (strcmp(a, "--pace-log") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->pace_log_path = v; i++;
        } else if (strcmp(a, "--profile-out") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            opts->profile_out_path = v; i++;
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
        } else if (strcmp(a, "--sfx") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            if (parse_volume_arg(a, v, &opts->sfx_volume) != 0)
                return OPENPETE_EXIT_SETUP_ERROR;
            i++;
        } else if (strcmp(a, "--music") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            if (parse_volume_arg(a, v, &opts->music_volume) != 0)
                return OPENPETE_EXIT_SETUP_ERROR;
            i++;
        } else if (strcmp(a, "--vibration") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            if (parse_volume_arg(a, v, &opts->vibration) != 0)
                return OPENPETE_EXIT_SETUP_ERROR;
            i++;
        } else if (strcmp(a, "--mouse") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            if      (strcmp(v, "1") == 0 || strcmp(v, "on")  == 0) opts->mouse = 1;
            else if (strcmp(v, "0") == 0 || strcmp(v, "off") == 0) opts->mouse = 0;
            else {
                fprintf(stderr, "openpete: --mouse expects 0|1 (on|off also accepted), got: %s\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            i++;
        } else if (strcmp(a, "--input-lock") == 0) {
            /* The value is optional: the bare flag turns the lock on, and an
             * explicit 0 is how the command line overrides the env spelling. */
            opts->input_lock = 1;
            if (i + 1 < argc) {
                const char* n = argv[i + 1];
                if      (strcmp(n, "1") == 0 || strcmp(n, "on")  == 0) { opts->input_lock = 1; i++; }
                else if (strcmp(n, "0") == 0 || strcmp(n, "off") == 0) { opts->input_lock = 0; i++; }
            }
        } else if (strcmp(a, "--background") == 0) {
            /* Optional value, as --input-lock. */
            opts->background = 1;
            if (i + 1 < argc) {
                const char* n = argv[i + 1];
                if      (strcmp(n, "1") == 0 || strcmp(n, "on")  == 0) { opts->background = 1; i++; }
                else if (strcmp(n, "0") == 0 || strcmp(n, "off") == 0) { opts->background = 0; i++; }
            }
        } else if (strcmp(a, "--mouse-sensitivity") == 0) {
            if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
            char* end = NULL;
            double d = strtod(v, &end);
            if (!end || *end || !(d >= 0.0) || d > 100.0) {
                fprintf(stderr, "openpete: --mouse-sensitivity expects a number "
                        "in 0..100, got: %s\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            opts->mouse_sensitivity = (float)d;
            i++;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            print_help(opts->display_name, argv[0], opts->default_renderer);
            return OPENPETE_EXIT_SETUP_ERROR; /* treat as setup-only invocation */
        } else if (a[0] != '-' && !opts->ingest_path && !opts->disc_path
                   && !opts->exe_path && arg_is_disc_image(a)) {
            /* A bare path to a disc image — what a desktop file's %f, a
             * file manager's "Open with" and a drag onto the icon all
             * deliver. Same meaning as --ingest: the library dedups by
             * content, so re-opening an already-installed disc just boots
             * it. */
            opts->ingest_path = a;
        } else {
            /* Config-spliced tokens were never typed, so pointing at the
             * command line would send the user hunting in the wrong place. */
            const char* src = openpete_config_token_source(a);
            if (src)
                fprintf(stderr, "openpete: unknown arg: %s (from openpete.toml "
                                "[%s])\n", a, src);
            else
                fprintf(stderr, "openpete: unknown arg: %s\n", a);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
    }
    return OPENPETE_EXIT_OK;
}

/** @brief Complete a --bk2-record request: default path, then the .args sidecar text.
 *
 * Runs after the flag chain has validated the command line, so a bad
 * invocation creates no directory. A NULL sidecar (allocation failure) keeps
 * the recording without a sidecar. OPENPETE_PLAYER_POSE set without a typed
 * --player-pose fails the parse, since the sidecar carries argv only and the
 * replay would start from the spawn.
 * @param opts The options block.
 * @param orig_argc The typed argument count.
 * @param orig_argv The typed argument vector.
 * @param notes The flag chain's post-parse facts. */
static int finish_bk2_record(openpete_options_t* opts, int orig_argc,
                             char** orig_argv, const parse_notes_t* notes) {
    if (!notes->bk2_record_requested) return OPENPETE_EXIT_OK;
    if (!opts->bk2_record_path || !*opts->bk2_record_path) {
        char* def = resolve_default_bk2_record_path();
        if (!def) {
            fprintf(stderr, "openpete: --bk2-record: out of memory resolving default path\n");
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        opts->bk2_record_path = def;  /* leaked for program lifetime */
        fprintf(stderr, "openpete: --bk2-record: recording to %s\n", def);
    }
    opts->bk2_record_args = build_bk2_record_args(orig_argc, orig_argv);
    /* NULL (OOM) just means no sidecar — the recording itself still works. */
    /* The sidecar carries argv, not the environment: a placement given
     * only as OPENPETE_PLAYER_POSE would be missing from the replay, which
     * then runs the movie's input from the spawn instead. */
    const char* pp = getenv("OPENPETE_PLAYER_POSE");
    if (pp && *pp && !notes->player_pose_flag) {
        fprintf(stderr, "openpete: --bk2-record: OPENPETE_PLAYER_POSE is set but a bk2 "
                        "replays only the command line; pass --player-pose \"%s\" instead\n", pp);
        return OPENPETE_EXIT_SETUP_ERROR;
    }
    return OPENPETE_EXIT_OK;
}

/** @brief Apply the env spelling of an on/off flag when the flag was not typed (*slot < 0).
 *
 * The CLI beats the env. The variable accepts 1, 0, on or off; an empty or
 * unset variable leaves the slot alone and any other value fails the parse.
 * A harness that launches the game from several sites sets the variable once
 * at the top of the script instead of threading an argument into each site.
 * @param name The env var.
 * @param slot The tri-state option field. */
static int env_onoff_fallback(const char* name, int* slot) {
    if (*slot >= 0) return OPENPETE_EXIT_OK;
    const char* v = getenv(name);
    if (!v || !*v) return OPENPETE_EXIT_OK;
    if      (strcmp(v, "1") == 0 || strcmp(v, "on")  == 0) *slot = 1;
    else if (strcmp(v, "0") == 0 || strcmp(v, "off") == 0) *slot = 0;
    else {
        fprintf(stderr, "openpete: %s expects 0|1 (on|off also accepted), got: %s\n",
                name, v);
        return OPENPETE_EXIT_SETUP_ERROR;
    }
    return OPENPETE_EXIT_OK;
}

/** @brief Apply the env spellings of dual-spelling flags the command line left unset.
 *
 * Runs after the flag chain, so a typed flag has already claimed its slot.
 * Covers OPENPETE_VIBRATION, OPENPETE_MOUSE, OPENPETE_INPUT_LOCK and
 * OPENPETE_BACKGROUND, and refuses --input-lock with OPENPETE_INPUT_LEGACY.
 * @param opts The options block. */
static int apply_env_spellings(openpete_options_t* opts) {
    /* OPENPETE_VIBRATION: the env spelling of --vibration, applied only when
     * the flag was not given (CLI beats env, the project-wide rule).
     *
     * The env spelling exists for scripted launches. A gate or bench harness
     * needs one line it can put at the top of the file, not an argument
     * threaded into however many invocation sites it builds — and an argument
     * appended to the wrong one of those is a silent miss, which for this knob
     * means a controller buzzing on someone's desk for the length of a corpus
     * run. (Under WSL an env knob only reaches a Windows .exe if WSLENV names
     * it; scripts/fences/wslenv_guard.py is what enforces that half.) */
    if (opts->vibration < 0) {
        const char* v = getenv("OPENPETE_VIBRATION");
        if (v && *v) {
            int level = -1;
            if (parse_volume_arg("OPENPETE_VIBRATION", v, &level) != 0)
                return OPENPETE_EXIT_SETUP_ERROR;
            opts->vibration = level;
        }
    }
    /* OPENPETE_MOUSE matters to captures: a stray pointer movement over the
     * window would otherwise steer the free camera mid-run. */
    if (env_onoff_fallback("OPENPETE_MOUSE", &opts->mouse) != 0
        || env_onoff_fallback("OPENPETE_INPUT_LOCK", &opts->input_lock) != 0
        || env_onoff_fallback("OPENPETE_BACKGROUND", &opts->background) != 0)
        return OPENPETE_EXIT_SETUP_ERROR;

    /* The legacy input path hands the event pump and the pad decode to
     * PsyCross, where the lock has no filter. */
    if (opts->input_lock > 0) {
        const char* legacy = getenv("OPENPETE_INPUT_LEGACY");
        if (legacy && *legacy && *legacy != '0') {
            fprintf(stderr, "openpete: --input-lock cannot be combined with "
                    "OPENPETE_INPUT_LEGACY=1\n");
            return OPENPETE_EXIT_SETUP_ERROR;
        }
    }
    return OPENPETE_EXIT_OK;
}

int openpete_options_parse(openpete_options_t* opts, int argc, char** argv) {
    /* Snapshot the command line as typed, before the sidecar / config
     * splices below rewrite argv. --bk2-record captures this (and not the
     * machine's own config defaults) into its .args sidecar. */
    int    orig_argc = argc;
    char** orig_argv = argv;

    /* Process handoff re-execs with the ORIGINAL argv (the fresh
     * process re-splices env/opts/sidecar files itself). */
    opts->handoff_argc = orig_argc;
    opts->handoff_argv = orig_argv;

    /* Per-bk2 sidecar: <path>.bk2.args next to the movie can supply
     * scenario-specific defaults (e.g. --skip-to-level 10 for an
     * Artisans-only movie). Spliced before user argv so explicit CLI
     * flags still override. See games/spyro-1/bk2/README.md. */
    int sidecar_argc = 0;
    char** sidecar_argv = NULL;
    int sidecar_rc = expand_bk2_sidecar(argc, argv, &sidecar_argc, &sidecar_argv);
    if (sidecar_rc < 0) return OPENPETE_EXIT_SETUP_ERROR;
    if (sidecar_rc == 1) { argc = sidecar_argc; argv = sidecar_argv; }

    /* openpete.toml. Resolves every configured layer, applies the winning
     * [env] entries to the environment — which must happen before ANY
     * getenv consumer, since features lazy-cache their env reads on first
     * use — and hands back the winning flags to splice.
     *
     * Spliced after the bk2 splice and so left of it, giving the argv
     * order [config][sidecar][typed]: left is parsed first and the last
     * write wins, which is the file < sidecar < CLI half of the chain.
     * The other half (which of two config layers wins, and args-vs-env)
     * was already decided inside the resolver — see openpete_config.h. */
    /* Give the keymap its SDL-backed name lookups before the config runs:
     * openpete_config_apply seeds the registry defaults, and parsing them
     * needs the lookup live. Pure string tables in SDL, so this is safe
     * long before SDL_Init. */
    openpete_keymap_install_sdl();

    int cfg_argc = 0;
    char** cfg_argv = NULL;
    int cfg_rc = openpete_config_apply(argc, argv, orig_argc, &cfg_argc, &cfg_argv);
    if (cfg_rc < 0) return OPENPETE_EXIT_SETUP_ERROR;
    if (cfg_rc == 1) { argc = cfg_argc; argv = cfg_argv; }

    parse_notes_t notes = { false, false };
    int rc = parse_flag_chain(opts, argc, argv, &notes);
    if (rc != OPENPETE_EXIT_OK) return rc;

    /* The EXE-source requirement is deferred to
     * openpete_options_require_exe_source(): the game may resolve a
     * disc AFTER parse via the content-addressed library (--ingest, or
     * a no-argument boot of an installed entry). Without --run-recomp the
     * runtime dispatches the EXE header's entry, the crt0, once the header
     * has been read. */

    /* After validation, so a bad invocation creates no directory. */
    rc = finish_bk2_record(opts, orig_argc, orig_argv, &notes);
    if (rc != OPENPETE_EXIT_OK) return rc;
    return apply_env_spellings(opts);
}

int openpete_options_require_exe_source(const openpete_options_t* opts) {
    /* Called by game main AFTER library resolution (--ingest / installed
     * library entries can supply the disc post-parse). --test-segv never
     * touches the EXE. */
    if (opts->test_recomp_segv) return OPENPETE_EXIT_OK;
    if (opts->disc_path || opts->exe_path) return OPENPETE_EXIT_OK;
    fprintf(stderr,
            "openpete: no game found — pass --disc PATH (or --exe-path), or "
            "install a disc into the library with --ingest PATH\n");
    return OPENPETE_EXIT_SETUP_ERROR;
}
