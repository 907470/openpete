/*
 * Knob registry — the table behind the M-key dev config overlay.
 *
 * One row per config knob. LIVE rows write through the same public setters
 * the O-menu uses (renderer/host state only — never gameplay-readable RAM);
 * RESTART_ONLY rows are display-only until each is individually audited and
 * given a proper live channel (a single boot-latched consumer is enough to
 * keep a row read-only: cf. the fog-scale latch history in no_lod.h).
 *
 * Adding a permanent CLI flag or OPENPETE_* env var? Add its row here in the
 * same change — the overlay doubles as the living catalog of every knob.
 */

#include "openpete_knobs.h"
#include "openpete_osd.h"
#include "openpete_workpool.h"
#include "openpete_terrain_cache.h"
#include "openpete_disc_cache.h"
#include "openpete.h"
#include "openpete_config.h"
#include "config_schema.h"
#include "openpete_log.h"
#include "openpete_native_render.h"
#include "openpete_camlock.h"
#include "openpete_fillpick.h"
#include "openpete_freecam.h"
#include "openpete_record.h"
#include "openpete_rewind.h"
#include "features/no_lod.h"
#include "features/widescreen.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const openpete_options_t *g_opts;

/* ------------------------------------------------------------------ */
/* value plumbing helpers                                             */
/* ------------------------------------------------------------------ */

static double get_stored(const knob_desc_t *k) { return k->stored_num; }

static int nearest_enum_idx(const float *vals, int n, float v) {
    int best = 0;
    float bd = 1e30f;
    for (int i = 0; i < n; i++) {
        float d = fabsf(vals[i] - v);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* Rows the O menu moves through the renderer's own setters must read the
 * renderer back, not a registry-side shadow: the menu bypasses set_num, so a
 * shadowed row would still report its boot value and "save edited settings"
 * would skip the pick entirely. Until the renderer is up (its state settles
 * after the registry stamps boot values, and never at all in headless /
 * PsyCross-only runs) the stored boot value is the live value. */
#define KNOB_LIVE_OR_STORED(k) \
    do { if (!openpete_native_render_enabled()) return (k)->stored_num; } while (0)

/* ---- Display ---- */

static double fullscreen_get(const knob_desc_t *k) {
    (void)k; return openpete_native_fullscreen_active() ? 1.0 : 0.0;
}
static int fullscreen_set(knob_desc_t *k, double v) {
    (void)k;
    if ((v != 0.0) != (openpete_native_fullscreen_active() != 0))
        openpete_native_request_fullscreen_toggle();   /* SDL call runs on the window's thread */
    return 0;
}

static const char *const k_aspect_names[] = { "auto", "4:3", "16:10", "16:9", "21:9", "32:9" };
static const float       k_aspect_vals[]  = { 0.0f, 4.f/3.f, 16.f/10.f, 16.f/9.f, 21.f/9.f, 32.f/9.f };
/* Index 0 is AUTO, whose 0.0 is a sentinel rather than a ratio — the nearest
 * match runs over the fixed ratios only (the same +1 offset init uses). */
static double aspect_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    float wh = openpete_native_get_aspect();
    if (wh <= 0.0f) return 0;
    return nearest_enum_idx(k_aspect_vals + 1, 5, wh) + 1;
}
static int aspect_set(knob_desc_t *k, double v) {
    int i = (int)v;
    if (i < 0 || i >= (int)(sizeof(k_aspect_vals)/sizeof(k_aspect_vals[0]))) return -1;
    openpete_native_set_aspect(k_aspect_vals[i]);
    k->stored_num = i;
    return 0;
}

/* An explicit factor table, not a power-of-two series: the resolve pass box-
 * filters the exact NxN source block, so every integer factor is well defined
 * and 3x is a real rung (9 samples/pixel, ~44% less render-target area than
 * 4x). The ladder stops at OPENPETE_SSAA_MAX because the supersampled targets
 * outgrow an 8GB card beyond it (see openpete.h). */
static const int         k_ssaa_vals[]  = { 1, 2, 3, 4 };
static const char *const k_ssaa_names[] = { "off", "2x", "3x", "4x" };
#define K_SSAA_N ((int)(sizeof(k_ssaa_vals)/sizeof(k_ssaa_vals[0])))
/* Nearest rung to a factor (an --ssaa the ladder doesn't list still has to
 * land on a row). */
static int ssaa_idx_of(int factor) {
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < K_SSAA_N; i++) {
        int d = k_ssaa_vals[i] - factor;
        if (d < 0) d = -d;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
static double ssaa_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    return ssaa_idx_of(openpete_native_get_ssaa());
}
static int ssaa_set(knob_desc_t *k, double v) {
    int i = (int)v;
    if (i < 0 || i >= K_SSAA_N) return -1;
    openpete_native_set_ssaa(k_ssaa_vals[i]);
    k->stored_num = i;
    return 0;
}

/* -1 = the device never got an explicit preference (the env/surface picked the
 * present mode), which is what the boot value already records. */
static double vsync_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    int pref = openpete_native_get_vsync();
    return (pref < 0) ? k->stored_num : (pref ? 1 : 0);
}
static int vsync_set(knob_desc_t *k, double v) {
    openpete_native_set_vsync(v != 0.0);
    k->stored_num = (v != 0.0);
    return 0;
}

static const char *const k_anchor_names[] = { "top-left", "top-center", "top-right",
                                              "bottom-left", "bottom-center",
                                              "bottom-right" };
static double perfpos_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    return (double)openpete_native_perf_anchor();
}
static int perfpos_set(knob_desc_t *k, double v) {
    openpete_native_set_perf_anchor((int)v);
    k->stored_num = (int)v;
    return 0;
}

static double osd_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    return openpete_osd_enabled() ? 1.0 : 0.0;
}
static int osd_set(knob_desc_t *k, double v) {
    openpete_osd_set_enabled(v != 0.0);
    k->stored_num = (v != 0.0);
    return 0;
}
static double osdpos_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    return (double)openpete_osd_anchor();
}
static int osdpos_set(knob_desc_t *k, double v) {
    openpete_osd_set_anchor((int)v);
    k->stored_num = (int)v;
    return 0;
}
/* Seconds at the surface, milliseconds underneath — the same unit boundary
 * --osd-timeout draws, and the only other place it is crossed. */
static double osdtimeout_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    return (double)openpete_osd_default_timeout_ms() / 1000.0;
}
static int osdtimeout_set(knob_desc_t *k, double v) {
    if (v <= 0.0) return -1;
    openpete_osd_set_default_timeout_ms((uint32_t)(v * 1000.0 + 0.5));
    k->stored_num = v;
    return 0;
}

static const char *const k_perf_names[] = { "off", "fps", "detailed" };
static double perfstats_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    return (double)openpete_native_perf_stats_level();
}
static int perfstats_set(knob_desc_t *k, double v) {
    openpete_native_set_perf_stats((int)v);
    k->stored_num = (int)v;
    return 0;
}

static const char *const k_vprec_names[] = { "subpixel", "integer" };
static const char *const k_texmap_names[] = { "perspective", "affine" };
static double vprec_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    return openpete_native_vertex_snap_active() ? 1.0 : 0.0;
}
static int vprec_set(knob_desc_t *k, double v) {
    openpete_native_set_vertex_precision((int)v == 1);
    k->stored_num = (int)v;
    return 0;
}

/* Live getter (not stored_num): the setter only QUEUES the mode — the
 * renderer applies it (full pipeline re-bake) at the next present — and the
 * O-menu row moves it too, so the row must read the renderer's state. */
static double texmap_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    return openpete_native_get_texture_mapping();
}
static int texmap_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_native_set_texture_mapping((int)v == 1);
    return 0;
}

static double rquant_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    return openpete_native_get_raster_quantize();
}
static double exthreads_get(const knob_desc_t *k) {
    (void)k;
    return openpete_workpool_threads();   /* resolved count (auto expanded) */
}
static double disccache_get(const knob_desc_t *k) {
    (void)k;
    return openpete_disc_cache_active() ? 1 : 0;
}
static double tercache_get(const knob_desc_t *k) {
    (void)k;
    return openpete_tercache_ready() ? 1 : 0;   /* cache actually came up */
}
static int rquant_set(knob_desc_t *k, double v) {
    openpete_native_set_raster_quantize((int)v);
    k->stored_num = (int)v;
    return 0;
}

/* Live getter (queued like texmap): untextured lighting view. */
static double notex_get(const knob_desc_t *k) {
    (void)k;
    return openpete_native_get_notex();
}
static int notex_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_native_set_notex(v != 0.0);
    return 0;
}

/* Live getter: the O-menu DITHER row moves the same renderer flag. */
static double dither_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    return openpete_native_get_dither();
}
static int dither_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_native_set_dither(v != 0.0);
    return 0;
}

static const char *const k_lod_names[] = { "original", "extended" };
static double lod_get(const knob_desc_t *k) { (void)k; return openpete_no_lod ? 1.0 : 0.0; }
static int lod_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_no_lod_set(v != 0.0, -1.0f);   /* fog follows the mode unless touched */
    return 0;
}

static double fog_get(const knob_desc_t *k) { (void)k; return openpete_fog_enabled() ? 1.0 : 0.0; }
static int fog_set(knob_desc_t *k, double v) { (void)k; openpete_fog_set(v != 0.0); return 0; }

static double fogscale_get(const knob_desc_t *k) { (void)k; return openpete_fog_scale; }
static int fogscale_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_fog_scale_set((float)v);
    return 0;
}

static const char *const k_renderer_names[] = { "psycross", "vk", "ab", "diff" };
static double renderer_get(const knob_desc_t *k) { (void)k; return g_opts ? (double)g_opts->renderer : 1.0; }

static void window_get_str(const knob_desc_t *k, char *buf, size_t cap) {
    (void)k;
    if (g_opts && g_opts->window_w > 0)
        snprintf(buf, cap, "%dx%d", g_opts->window_w, g_opts->window_h);
    else
        snprintf(buf, cap, "(default)");
}

/* ---- Pacing ---- */

static double renderfps_get(const knob_desc_t *k) { (void)k; return openpete_get_render_fps(); }
static int renderfps_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_set_render_fps((int)v);
    return 0;
}

static double tickscale_get(const knob_desc_t *k) { (void)k; return openpete_get_tick_scale(); }
static int tickscale_set(knob_desc_t *k, double v) {
    (void)k;
    if (v <= 0.0) return -1;
    openpete_set_tick_scale(v);
    return 0;
}

static double runahead_get(const knob_desc_t *k) { (void)k; return (double)openpete_runahead_get(); }
static int runahead_set(knob_desc_t *k, double v) {
    (void)k;
    if (v < 0) return -1;
    openpete_runahead_set((int)v);
    return 0;
}

static int fov_set(knob_desc_t *k, double v) {
    if (v < 0.0) return -1;
    widescreen_set_fov((float)v);
    k->stored_num = v;
    return 0;
}

static int rewind_set(knob_desc_t *k, double v) {
    if (v < 0) return -1;
    openpete_rewind_configure((int)v);
    k->stored_num = (int)v;
    return 0;
}

/* ---- Audio ---- */

static double mute_get(const knob_desc_t *k) { (void)k; return openpete_get_mute() ? 1.0 : 0.0; }
static int mute_set(knob_desc_t *k, double v) { (void)k; openpete_set_mute(v != 0.0); return 0; }

/* The two in-game slider presets. Both setters only move a host-side request
 * — the guest write happens on the game's own tick hook — so they satisfy the
 * live-row contract despite the value ending up in gameplay-readable RAM. */
static double sfxvol_get(const knob_desc_t *k) { (void)k; return (double)openpete_get_sfx_volume(); }
static int sfxvol_set(knob_desc_t *k, double v) { (void)k; openpete_set_sfx_volume((int)v); return 0; }
static double musicvol_get(const knob_desc_t *k) { (void)k; return (double)openpete_get_music_volume(); }
static int musicvol_set(knob_desc_t *k, double v) { (void)k; openpete_set_music_volume((int)v); return 0; }

/* ---- Input ---- */

/* Vibration is the same shape: the setter moves a host request, and the guest
 * enable flag is written by the per-game feature on its tick hook. */
static double vibration_get(const knob_desc_t *k) { (void)k; return (double)openpete_get_vibration(); }
static int vibration_set(knob_desc_t *k, double v) { (void)k; openpete_set_vibration((int)v); return 0; }

/* ---- Recording ---- */

/* --record is latched at boot, so with no path this run will never encode a
 * frame and every row in the section is inert — as is all of it on a build
 * with no video encoder, where --record only warns. Hidden rather than
 * disabled: there is nothing the user could do about either from here. */
static int recording_visible(const knob_desc_t *k) {
    (void)k;
    if (!openpete_record_supported()) return 0;
    return g_opts && g_opts->record_path && g_opts->record_path[0];
}

static int recfps_set(knob_desc_t *k, double v) {
    openpete_native_set_fps_in_record(v != 0.0);
    k->stored_num = (v != 0.0);
    return 0;
}

/* ---- Paths (boot-resolved; display the same precedence the consumer used) */

static void path_get_str(const knob_desc_t *k, char *buf, size_t cap) {
    const char *cli = NULL;
    if (g_opts) {
        if      (!strcmp(k->id, "disc"))        cli = g_opts->disc_path;
        else if (!strcmp(k->id, "texpack_dir")) cli = g_opts->texpack_dir;
        else if (!strcmp(k->id, "mods_dir"))    cli = g_opts->mods_dir;
        else if (!strcmp(k->id, "mods"))        cli = g_opts->mods_enabled;
        else if (!strcmp(k->id, "card_dir"))    cli = g_opts->card_dir;
        else if (!strcmp(k->id, "record"))      cli = g_opts->record_path;
        else if (!strcmp(k->id, "bk2"))         cli = g_opts->bk2_path;
    }
    if (cli && !*cli) { /* explicit empty value, e.g. --mods "" */
        snprintf(buf, cap, "(none)");
        return;
    }
    const char *env = (k->env && !cli) ? getenv(k->env) : NULL;
    snprintf(buf, cap, "%s", cli ? cli : (env ? env : "(default)"));
}

/* ---- CLI debug rows (boot-parsed; display-only) ---- */

static void renderpaths_get_str(const knob_desc_t *k, char *buf, size_t cap) {
    (void)k;
    const char *v = (g_opts && g_opts->render_paths && *g_opts->render_paths)
                        ? g_opts->render_paths : NULL;
    snprintf(buf, cap, "%s", v ? v : "(all)");
}

static double renderstats_get(const knob_desc_t *k) {
    (void)k; return (g_opts && g_opts->render_stats) ? 1.0 : 0.0;
}

static void loglevel_get_str(const knob_desc_t *k, char *buf, size_t cap) {
    (void)k;
    const char *v = openpete_log_get_spec();
    snprintf(buf, cap, "%s", (v && *v) ? v : "(default: info)");
}

static void logfile_get_str(const knob_desc_t *k, char *buf, size_t cap) {
    (void)k;
    const char *v = openpete_log_file_path();
    snprintf(buf, cap, "%s", (v && *v) ? v : "(no log file)");
}

static int loglevel_set_str(knob_desc_t *k, const char *v) {
    (void)k;
    if (v && *v == '(') v = "";   /* "(default: info)" placeholder = clear */
    openpete_log_set_spec(v);
    return 0;
}

static const char *const k_logfmt_names[] = { "plain", "json" };
static double logfmt_get(const knob_desc_t *k) {
    (void)k; return (double)openpete_log_get_format();
}
static int logfmt_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_log_set_format((int)v == 1 ? OPENPETE_LOG_JSON : OPENPETE_LOG_PLAIN);
    return 0;
}

/* ---- Debug env rows ----
 *
 * Generic rows over raw OPENPETE_* env strings: display is the live env
 * value ("(unset)" when absent). Editability follows the consumer pattern,
 * proven by grep per var:
 *  - FRESH consumers call getenv per use, so setenv IS their live channel
 *    (editable, KNOB_LIVE);
 *  - CACHED consumers latch the value into a function-local static on first
 *    use — setenv would silently do nothing, so those rows stay display-only
 *    (KNOB_RESTART_ONLY) until the latch is converted to an accessor.
 *    Promoting one is a per-knob audited change, not an overlay tweak. */

/* Bumped on every live env edit. A consumer that re-resolves a knob "once
 * per VBL" pays a getenv per VBL on every thread that reads it; keyed on
 * this epoch it pays one per EDIT. On Windows that matters twice over: the
 * CRT's getenv is ~46x glibc's, and concurrent getenv/putenv from two
 * threads has been observed to wedge the process. */
static unsigned s_env_epoch = 1;
unsigned knob_registry_env_epoch(void) { return s_env_epoch; }

static int portable_setenv(const char *name, const char *val) {
    s_env_epoch++;
#ifdef _WIN32
    return _putenv_s(name, val ? val : "");
#else
    if (!val || !*val) return unsetenv(name);
    return setenv(name, val, 1);
#endif
}

static void env_get_str(const knob_desc_t *k, char *buf, size_t cap) {
    const char *v = k->env ? getenv(k->env) : NULL;
    if (v)
        snprintf(buf, cap, "%s", v);
    else if (k->def_str && *k->def_str)
        snprintf(buf, cap, "(unset - %s)", k->def_str);
    else
        snprintf(buf, cap, "(unset)");
}

static int env_set_str(knob_desc_t *k, const char *v) {
    if (!k->env) return -1;
    /* Editing back to the "(unset...)" placeholder (or clearing) unsets. */
    if (v && !strncmp(v, "(unset", 6)) v = "";
    return portable_setenv(k->env, v);
}

/* Compact row builders for the Debug sections. LIVE = FRESH consumer. */
#define ENVROW_LIVE(name, sect, def, helptext) \
    { name, name, sect, NULL, "OPENPETE_" name, helptext, KNOB_STRING, \
      KNOB_LIVE, NULL, 0, 0, 0, NULL, NULL, env_get_str, env_set_str, \
      KNOB_SRC_DEFAULT, 0, {0}, 0, def }
#define ENVROW_LATCHED(name, sect, def, helptext) \
    { name, name, sect, NULL, "OPENPETE_" name, helptext, KNOB_STRING, \
      KNOB_RESTART_ONLY, NULL, 0, 0, 0, NULL, NULL, env_get_str, NULL, \
      KNOB_SRC_DEFAULT, 0, {0}, 0, def }

/* ---- Gameplay / debug ---- */

static double skiplevel_get(const knob_desc_t *k) { (void)k; return g_opts ? (double)g_opts->skip_to_level : 0.0; }

static void cheats_get_str(const knob_desc_t *k, char *buf, size_t cap) {
    (void)k;
    snprintf(buf, cap, "%s",
             (g_opts && g_opts->cheats && g_opts->cheats[0]) ? g_opts->cheats
                                                             : "(none)");
}

static double freecam_get(const knob_desc_t *k) { (void)k; return openpete_freecam_active() ? 1.0 : 0.0; }
static int freecam_set(knob_desc_t *k, double v) {
    (void)k;
    if ((v != 0.0) != (openpete_freecam_active() != 0))
        openpete_freecam_toggle();
    return 0;
}

static double fillpick_get(const knob_desc_t *k) { (void)k; return openpete_fillpick_active() ? 1.0 : 0.0; }
static int fillpick_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_fillpick_set_active(v != 0.0);
    return 0;
}

static double fillpick_prev_get(const knob_desc_t *k) { (void)k; return openpete_fillpick_preview() ? 1.0 : 0.0; }
static int fillpick_prev_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_fillpick_set_preview(v != 0.0);
    return 0;
}

static double camlock_get(const knob_desc_t *k) { (void)k; return openpete_camlock_active() ? 1.0 : 0.0; }
static int camlock_set(knob_desc_t *k, double v) {
    (void)k;
    if ((v != 0.0) != (openpete_camlock_active() != 0)) {
        if (v != 0.0 && openpete_freecam_active())
            openpete_freecam_toggle();   /* the two share the fly axes */
        openpete_camlock_toggle();
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* the table                                                          */
/* ------------------------------------------------------------------ */

#define ENUM_ROW(vals) vals, (int)(sizeof(vals)/sizeof((vals)[0]))

/* Built-in rows. The registry hands out g_rows (this table plus whatever game
 * modules contributed through openpete_knob_add), never this array directly. */
static const knob_desc_t g_builtin[] = {
    /* -------- Display -------- */
    { "fullscreen", "Fullscreen", "Display", "--fullscreen", NULL,
      "Borderless-desktop fullscreen (same toggle as Alt+Enter / double-click).",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      fullscreen_get, fullscreen_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "aspect", "Aspect ratio", "Display", "--aspect", NULL,
      "Hor+ render aspect; auto follows the window shape. Picking a fixed "
      "ratio windowed resizes the window to match.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_aspect_names), 0, 0,
      aspect_get, aspect_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "ssaa", "Supersampling", "Display", "--ssaa", NULL,
      "Native supersampling factor. Applying reallocates the render target "
      "next present (brief hitch at large factors; GPU-max capped).",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_ssaa_names), 0, 0,
      ssaa_get, ssaa_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "vsync", "VSync", "Display", "--vsync", "OPENPETE_PRESENT_MODE",
      "FIFO (on) vs immediate/mailbox (off). Applying rebuilds the swapchain "
      "on the next frame.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      vsync_get, vsync_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "perf_stats", "Performance stats", "Display", "--perf-stats", NULL,
      "Top-left overlay (native renderer): off, the FPS counter, or the "
      "counter plus a pacing readout — VBL rate, deadline slips, late ticks "
      "per 1000, and the shed / interpolation share of presents.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_perf_names), 0, 0,
      perfstats_get, perfstats_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "perf_position", "Stats position", "Display", "--perf-position", NULL,
      "Which corner or edge the performance-stats block anchors to. Shares "
      "the anchor ledger with the OSD: whichever surface claims a corner "
      "first, the other stacks clear of it.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_anchor_names), 0, 0,
      perfpos_get, perfpos_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "osd", "Notifications", "Display", "--osd", NULL,
      "On-screen notifications when a setting changes, in the game's own gold "
      "chrome. Mods raise them too.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      osd_get, osd_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "osd_position", "Notification position", "Display", "--osd-position", NULL,
      "Which corner or edge notifications stack at. Sharing a corner with the "
      "stats block is fine: the stats block claims first and the toasts stack "
      "clear of it.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_anchor_names), 0, 0,
      osdpos_get, osdpos_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "osd_timeout", "Notification timeout", "Display", "--osd-timeout", NULL,
      "Seconds a notification stays up before it fades. A caller may ask for "
      "its own; this is what it gets when it does not.",
      KNOB_FLOAT, KNOB_LIVE, NULL, 0, 0.5, 30.0,
      osdtimeout_get, osdtimeout_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "vertex_precision", "Vertex precision", "Display", "--vertex-precision", NULL,
      "PSX integer-grid vertex snap (authentic polygon wobble) vs smooth "
      "subpixel. Display-only.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_vprec_names), 0, 0,
      vprec_get, vprec_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "raster_quantize", "Raster quantize", "Display", "--raster-quantize", NULL,
      "Point-resample the 3D scene to N x the PSX raster (1 = the authentic "
      "512x240 grid — 224 lines visible on screen, letterbands trimmed; "
      "0 = off). One sample per cell, as the hardware rasterizer produced, "
      "always aligned with the integer vertex snap. Display-only.",
      KNOB_INT, KNOB_LIVE, NULL, 0, 0, 16,
      rquant_get, rquant_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "extract_threads", "Extraction threads", "Display", "--extract-threads", NULL,
      "Worker threads for the native render extraction (terrain sector walk). "
      "Shows the resolved count (0 = auto at launch). Output is byte-identical "
      "at any count; 1 = serial. Boot-latched; display-only.",
      KNOB_INT, KNOB_RESTART_ONLY, NULL, 0, 1, 8,
      exthreads_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "terrain_cache", "Terrain GPU cache", "Display", "--terrain-cache", NULL,
      "Retained GPU terrain (default on): unclaimed sectors render from "
      "world-space vertex chunks with the per-face verdicts mirrored in the "
      "vertex stage; claimed sectors (near / blended-over-portal / safety) "
      "stay on the CPU walk. Shows whether the cache came up (needs a "
      "hardware GPU with fp64; software vk runs the CPU walk). 0 = kill "
      "switch. Boot-latched; display-only.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      tercache_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    ENVROW_LATCHED("TERRAIN_HDFOLD", "Display", "1",
      "HD texture-replacement fold (default on): with a texture pack active, "
      "pack-matched terrain faces bake replacement entries into the retained "
      "GPU cache (atlas + per-entry fog) instead of claiming their sectors "
      "back to the CPU walk; unbakeable faces (blended targets, overflow) "
      "still claim. Inert without a pack. 0 = kill switch (tier-1 per-sector "
      "claims). Boot-latched; display-only."),

    { "notex", "Untextured (lighting)", "Display", NULL, "OPENPETE_NOTEX",
      "Debug view: draw the 3D scene with textures replaced by neutral grey, "
      "showing the raw gouraud vertex lighting. Cutout silhouettes stay real. "
      "Applying re-bakes every scene pipeline (one-time hitch). Display-only.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      notex_get, notex_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "dither", "Dither (15-bit)", "Display", "--dither", NULL,
      "PS1 15-bit colour: quantize the 3D scene to RGB555 with the GPU's 4x4 "
      "ordered dither matrix on the PSX framebuffer grid (authentic banding + "
      "crosshatch). Display-only.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      dither_get, dither_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "texture_mapping", "Texture mapping", "Display", "--texture-mapping", NULL,
      "perspective = modern perspective-correct interpolation; affine = the "
      "PSX screen-linear texture swim/warp. Applying re-bakes every scene "
      "pipeline at the next present (brief hitch). Display-only.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_texmap_names), 0, 0,
      texmap_get, texmap_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "lod", "LOD", "Display", "--lod", NULL,
      "original = authentic PSX detail bands; extended = full detail at any "
      "distance (triangle-bound in dense vistas). Fog follows the mode "
      "unless explicitly set.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_lod_names), 0, 0,
      lod_get, lod_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "fog", "Fog", "Display", NULL, NULL,
      "Distance fog on/off (off = the 100x far-scale sentinel). LOD switches "
      "move this automatically until it is touched.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      fog_get, fog_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "fog_scale", "Fog scale", "Display", "--fog-scale", NULL,
      "Fog-distance multiplier (1 = authentic curve, 100 = effectively off). "
      "Terrain re-resolves it every walk; a manual value takes fog off the "
      "LOD autopilot.",
      KNOB_FLOAT, KNOB_LIVE, NULL, 0, 0.25f, 128.0f,
      fogscale_get, fogscale_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "renderer", "Renderer", "Display", "--renderer", NULL,
      "Display mode: native Vulkan (vk, default), legacy GL (psycross), "
      "side-by-side (ab), or per-pixel diff (diff). Decides which windows "
      "and devices exist at boot.",
      KNOB_ENUM, KNOB_RESTART_ONLY, ENUM_ROW(k_renderer_names), 0, 0,
      renderer_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "window", "Window size", "Display", "--window", NULL,
      "Launch window size (drag the window to resize live).",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, window_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    /* -------- Pacing -------- */
    { "render_fps", "Render FPS", "Pacing", "--render-fps", NULL,
      "Target present rate (0 = legacy match-sim pacer). Above ~30 arms "
      "sub-tick interpolation; vsync clamps to refresh. Shows the live "
      "pacer target (an unset launch value resolves to display refresh).",
      KNOB_INT, KNOB_LIVE, NULL, 0, 0, 1000,
      renderfps_get, renderfps_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "tick_scale", "Tick scale", "Pacing", "--tick-scale", NULL,
      "Engine speed multiplier (slow-mo / speed-up; sim and presents "
      "together). Applies on the next host iteration; pacing only, the sim "
      "runs identical ticks on a stretched wall clock.",
      KNOB_FLOAT, KNOB_LIVE, NULL, 0, 0.05f, 8.0f,
      tickscale_get, tickscale_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "runahead", "Runahead", "Pacing", "--runahead", NULL,
      "Present-timeline lead of N sim ticks (latency cut, ~N extra ticks of "
      "CPU). Applies next host iteration; the mod-capability gate can "
      "refuse (row falls back to 0). Presents snap for one tick on change.",
      KNOB_INT, KNOB_LIVE, NULL, 0, 0, 3,
      runahead_get, runahead_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "rewind", "Rewind window", "Pacing", "--rewind", NULL,
      "Backspace-rewind window in seconds (0 = off). APPLYING CLEARS the "
      "accumulated rewind history; ~2 MiB per second.",
      KNOB_INT, KNOB_LIVE, NULL, 0, 0, 600,
      get_stored, rewind_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "fov", "FOV override", "Pacing", "--fov", NULL,
      "Horizontal FOV in degrees (0 = game default; native 4:3 is ~50.4). "
      "The GTE re-reads the override per projection, so it applies "
      "immediately - PsyCross reference renderer only (inert in vk).",
      KNOB_FLOAT, KNOB_LIVE, NULL, 0, 0, 170,
      get_stored, fov_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    /* -------- Audio -------- */
    { "mute", "Mute", "Audio", "--mute", NULL,
      "Mute all audio at the OpenAL layer (SPU SFX + XA music + WAV "
      "override). Live; the F7 pause exit restores this baseline.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      mute_get, mute_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "sfx", "SFX volume", "Audio", "--sfx", NULL,
      "Preset the game's own SOUND EFFECTS slider, 0 (OFF) to 10 (max); "
      "-1 leaves it alone. Stamped at a tick boundary and re-stamped over "
      "the title screen / save-file load, until the player moves that "
      "slider in the game's options menu.",
      KNOB_INT, KNOB_LIVE, NULL, 0, -1, 10,
      sfxvol_get, sfxvol_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "music", "Music volume", "Audio", "--music", NULL,
      "Preset the game's own MUSIC slider, 0 (OFF) to 10 (max); -1 leaves "
      "it alone. Same stamping rules as the SFX row.",
      KNOB_INT, KNOB_LIVE, NULL, 0, -1, 10,
      musicvol_get, musicvol_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    /* -------- Input -------- */
    { "vibration", "Vibration", "Input", "--vibration", "OPENPETE_VIBRATION",
      "Controller rumble strength, 0 (off) to 10 (max); -1 leaves the game's "
      "own vibration setting alone (the default, which means on). 10 matches "
      "DuckStation's strength curve. The game's enable flag is stamped at a "
      "tick boundary and released if the player toggles vibration in the "
      "options menu; the motors are always silent headless or unfocused.",
      KNOB_INT, KNOB_LIVE, NULL, 0, -1, 10,
      vibration_get, vibration_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    /* -------- Paths -------- */
    { "disc", "Disc image", "Paths", "--disc", NULL,
      "BIN/CUE the game boots from.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, path_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "disc_cache", "Disc cache", "Paths", "--disc-cache", "OPENPETE_DISC_CACHE",
      "Disc image resident in memory (default on): guest disc reads are a "
      "memcpy from a host array a background loader fills at boot, so a "
      "level load never waits on the file (a read over a network share "
      "costs tens of ms per burst and lands as a late tick). Shows whether "
      "the cache attached. 0 = read the file per sector (A/B). Boot-latched; "
      "bytes identical either way.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      disccache_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "texpack_dir", "Texture pack dir", "Paths", "--texpack-dir", "OPENPETE_TEXPACK_DIR",
      "HD texture-replacement PNG directory (scanned once at renderer init).",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, path_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "mods_dir", "Mods dir", "Paths", "--mods-dir", "OPENPETE_MODS_DIR",
      "Mods root (mod.toml subdirs; load order via enabled.txt).",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, path_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "mods", "Enabled mods", "Paths", "--mods", "OPENPETE_MODS",
      "Enabled modset override (comma-separated ids; order = priority). "
      "Empty = no mods. Unset = the mods dir's enabled.txt.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, path_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "card_dir", "Memory card dir", "Paths", "--card-dir", "OPENPETE_CARD_DIR",
      "Directory holding the .mcr memory-card images.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, path_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    /* -------- Gameplay entry -------- */
    { "skip_to_level", "Skip to level", "Gameplay", "--skip-to-level", NULL,
      "Boot straight into a level id, fast-forwarding logos/title. One-shot "
      "at boot.",
      KNOB_INT, KNOB_RESTART_ONLY, NULL, 0, 0, 99,
      skiplevel_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "freecam", "Free camera", "Gameplay", NULL, "OPENPETE_FREECAM",
      "Debug free camera (same toggle as the F key / stick click). "
      "Intentionally divergent debug tool - the one non-presentation row.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      freecam_get, freecam_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "camlock", "Camera lock (render)", "Gameplay", NULL, "OPENPETE_CAMLOCK",
      "Lock/fly the presented view while the sim keeps running (G key). "
      "Render-only: gameplay state is untouched, so cutscenes play on "
      "underneath - the close-up inspection tool.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      camlock_get, camlock_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    /* A movie drives the pad, so it belongs with what shapes the session
     * rather than with the video-capture rows it used to sit among. */
    { "bk2", "bk2 replay", "Gameplay", "--bk2", NULL,
      "Replay a BizHawk movie into the pad (loaded at loop start).",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, path_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    /* Last row of the section on purpose: the game module contributes one
     * button row per cheat through openpete_knob_add, and those land after
     * the section's last built-in — directly under this launch summary. */
    { "cheats", "Cheats at launch", "Gameplay", "--cheats", NULL,
      "Built-in cheat codes this run starts each session with. The rows below "
      "fire the same cheats on demand; this one only shows the launch "
      "selection. Intentionally divergent gameplay state.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, cheats_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    /* -------- Recording --------
     * Every row here is hidden unless this run is recording; see
     * recording_visible. The trailing NULLs are def_str + action_label,
     * which these rows do not use. */
    { "record", "Record video", "Recording", "--record", NULL,
      "True-VFR gameplay capture to a video file (latched at boot).",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, path_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      NULL, NULL, recording_visible },

    { "record_cfr", "Constant-rate capture", "Recording", "--record-cfr", NULL,
      "Capture at a fixed frame rate (an integer multiple of the 29.913 Hz "
      "tick rate) on an offline clock, instead of true VFR. 0 = VFR. "
      "Latched at boot.",
      KNOB_FLOAT, KNOB_RESTART_ONLY, NULL, 0, 0.0f, 480.0f,
      get_stored, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      NULL, NULL, recording_visible },

    { "record_frames", "Capture frame budget", "Recording", "--record-frames", NULL,
      "Finish the recording and exit after this many encoded frames. "
      "0 = until the run ends. Latched at boot.",
      KNOB_INT, KNOB_RESTART_ONLY, NULL, 0, 0, 1000000,
      get_stored, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      NULL, NULL, recording_visible },

    { "record_fps_overlay", "FPS overlay in recording", "Recording",
      "--record-fps-overlay", "OPENPETE_FPS_IN_RECORD",
      "Burn the FPS overlay into --record video output. Live: re-resolved "
      "on the next recorded frame.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      get_stored, recfps_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      NULL, NULL, recording_visible },

#ifndef OPENPETE_HIDE_DEBUG_FLAGS
    /* -------- Debug (toggles / tunables) -------- */
    { "render_paths", "Render paths", "Debug", "--render-paths", NULL,
      "Render only the named paths in isolation (cyclorama,spyro,moby,"
      "terrain,shadow,flame,particles,tracer,transition,portal). Empty or "
      "\"(all)\" clears the selection. Live once the game module binds its "
      "setter.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, renderpaths_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "render_stats", "Render stats", "Debug", "--render-stats", NULL,
      "Per-frame colour-diff statistics (diff renderer only).",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      renderstats_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "log_level", "Log level", "Debug", "--log-level", "OPENPETE_LOG",
      "Per-subsystem stderr log levels, e.g. cd:debug,*:info. Applying "
      "REPLACES all boot rules with the typed spec (empty = *:info).",
      KNOB_STRING, KNOB_LIVE, NULL, 0, 0, 0,
      NULL, NULL, loglevel_get_str, loglevel_set_str, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "log_file", "Log file", "Debug", "--log-file", "OPENPETE_LOG_FILE",
      "Path stderr is mirrored to for bug reports; empty = disabled "
      "(--no-log-file). Opened once at startup, so read-only here.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, logfile_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "log_format", "Log format", "Debug", "--log-format", NULL,
      "stderr log shape: plain text vs one JSON object per line. Takes "
      "effect on the next line.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_logfmt_names), 0, 0,
      logfmt_get, logfmt_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    ENVROW_LATCHED("DEPTH_AFFINE", "Debug", "1",
        "0 = legacy depth curve; 1 (default) screen-affine; baked into pipelines at boot."),
    ENVROW_LATCHED("EAGER", "Pacing", "1",
      "Eager production (default on): the span of presents is extracted "
      "AHEAD of each tick and presented from a dedicated thread's scene "
      "ring, so the tick's execution leaves no hole in the display. 0 = "
      "present from the sim thread (the first A/B for any pacing "
      "regression). Windowed native-vk only; a --screenshot-on-frame "
      "target disengages it unless OPENPETE_EAGER_SHOT=1. Latched at "
      "startup."),
    ENVROW_LATCHED("THREAD_SPLIT", "Debug", "1",
      "Run the sim on a worker thread while the main thread owns the window "
      "and the event pump, so dragging or resizing the window cannot stall "
      "the game. On by default; set 0 to put the loop back on the main "
      "thread. Windowed native-vk only. Latched at startup."),
    ENVROW_LATCHED("GPU_TIMERS", "Debug", "0",
        "Per-pass GPU timestamp zones (query pools created at device init)."),
    ENVROW_LIVE("PHASE_TIMERS", "Debug", "0",
        "Once-per-second per-phase CPU us report (re-resolved per VBL)."),
    ENVROW_LIVE("MOD_TIMERS", "Debug", "0",
        "Per-mod tick/present ms report (re-read per report window)."),
    ENVROW_LIVE("GPU_TIMERS_VERBOSE", "Debug", "0",
        "GPU-timer stderr line includes nested sub-zones, not just depth-0 "
        "(re-read per report; needs GPU_TIMERS)."),
    ENVROW_LIVE("BACKDROP_DEFER", "Debug", "1",
        "Defer cyclorama/backdrop fills until after WORLD-opaque so early-Z "
        "rejects covered pixels (default 1; 0 = PSX draw-first order)."),
    ENVROW_LATCHED("MATERIAL_STATS", "Debug", "0",
        "Print new peaks of interned material slots."),
    ENVROW_LATCHED("TEXPACK_STATS", "Debug", "0",
        "HD-texpack sub-run disposition counters."),
    ENVROW_LIVE("MOBY_OCCLUSION", "Debug", "1",
        "Moby occlusion culling on/off (default 1; re-resolved per VBL)."),
    ENVROW_LATCHED("MOBY_OCCL_SLACK", "Debug", "1500",
        "Occlusion-grid depth slack before a rect counts hidden (default 1500)."),
    ENVROW_LATCHED("MOBY_RANK", "Debug", "0",
        "Same-moby painter select: og OT-chain paint ranks override per-pixel "
        "depth between one moby's own faces (default 0 = true depth only)."),
    ENVROW_LATCHED("MOBY_SHADOW_DECAL", "Debug", "1",
        "Depth-aware moby-shadow decal vs legacy subtract pipe (default 1)."),
    ENVROW_LATCHED("SHADOW_BAND", "Debug", "256",
        "Fallback view-Z band for shadow decal mode 0 (default 256)."),
    ENVROW_LATCHED("SHADOW_HW_BIAS", "Debug", "-64,-2,-0.002",
        "Hardware depth bias c,s,clamp on subtractive shadow pipes."),
    ENVROW_LIVE("SHADOW_DECAL_MODE", "Debug", "",
        "Force shadow-decal shader mode (calibration override)."),
    ENVROW_LATCHED("PORTAL_MASK_BIAS", "Debug", "0,0,0",
        "Portal-mask camera-ward depth pull (3 floats)."),
    ENVROW_LATCHED("HUD6_ZPIPE", "Debug", "0",
        "A/B: HUD glyph bucket via opaque depth-tested pipe."),
    ENVROW_LATCHED("HUD6_NOSORT", "Debug", "0",
        "A/B: keep bucket-6 submission order (skip z sort)."),
    ENVROW_LATCHED("NATIVE_SCENE_DELAY", "Debug", "0",
        "Force scene-history depth for compose (alignment sweeps)."),
    ENVROW_LATCHED("TAG_SPLIT", "Debug", "0",
        "Force batch split by mod scene-tag."),
    ENVROW_LATCHED("TAG_TINT", "Debug", "0",
        "Visual tint by scene tag."),
    ENVROW_LIVE("SHADOW_WALL_HEIGHT", "Debug", "400",
        "Decal wall-occlusion height threshold (default 400)."),
    ENVROW_LIVE("SHADOW_STEEP_NY", "Debug", "0.5",
        "Up-normal below which a surface counts as wall (default 0.5)."),
    ENVROW_LIVE("SHADOW_PLAT_EPS", "Debug", "150",
        "Raised-occluder clause epsilon (default 150; 0 disables)."),
    ENVROW_LIVE("SHADOW_PLAT_SLACK", "Debug", "48",
        "Platform-clause slack (default 48)."),
    ENVROW_LATCHED("SHADOW_BIAS_SPYRO", "Debug", "1.0",
        "Spyro-shadow depth-bias multiplier."),
    ENVROW_LATCHED("SHADOW_BIAS_MOBY", "Debug", "1.0",
        "Moby-shadow depth-bias multiplier."),
    ENVROW_LIVE("SHADOW_SCAFFOLD", "Debug", "0",
        "Feed shadows from captured .s prims instead of native walk."),
    ENVROW_LIVE("FLAME_SCAFFOLD", "Debug", "0",
        "Same scaffold fallback for the flame feeder."),
    ENVROW_LATCHED("PORTAL_SKY_EPS", "Debug", "24",
        "Portal-sky forward depth margin (default 24; two independent latches)."),
    ENVROW_LATCHED("PORTAL_SKY_DEPTH", "Debug", "plane",
        "Portal sky depth model: ot|plane|ring."),
    ENVROW_LIVE("PORTAL_SKY_OTPUSH", "Debug", "1",
        "Far-portal sky rides the .s +64-slot OT bump as an 8192-world depth push (0 = un-pushed plane)."),
    ENVROW_LATCHED("NOLOD_PORTAL_DOME", "Debug", "1",
        "Kill switch for extended-LOD always-render dome (default 1)."),
    ENVROW_LATCHED("PORTAL_GUARD", "Debug", "0",
        "Experiment: re-feed .s near-band tris as portal guard."),
    ENVROW_LATCHED("PORTAL_APERTURE", "Debug", "1",
        "Stencil the portal sky to the opening polygon (0 = old sky-footprint mask)."),
    ENVROW_LATCHED("PORTAL_APERTURE_MARGIN", "Debug", "40",
        "Outward margin on the portal aperture, in WORLD units (default 40; negative erodes)."),
    ENVROW_LATCHED("PORTAL_RIM_CULL", "Debug", "1",
        "Cull terrain lying in a portal's opening plane (the inner rim). 0 = keep."),
    ENVROW_LATCHED("PORTAL_TRANSP_COVER", "Debug", "1",
        "Drop blended terrain (waterfalls, both tiers) the portal preview covers "
        "(0 = let it blend through the preview)."),
    ENVROW_LATCHED("PORTAL_PLANE_MARGIN", "Debug", "128",
        "Plane-cover near margin M (world units): inside an opening the preview covers "
        "everything at or behind plane - M. Portal-name text floats ~192-256 in front; "
        "frame erosion begins near 512."),
    ENVROW_LATCHED("PORTAL_RIM_NEAR", "Debug", "128",
        "Rim predicate: how far the NEAREST vertex may sit from the opening plane, in "
        "world units — the rim must touch the plane, an arch's front wall must not."),
    ENVROW_LATCHED("PORTAL_RIM_EPS", "Debug", "384",
        "Rim predicate: how far the FARTHEST vertex may stray from the opening plane, "
        "in world units."),
    ENVROW_LATCHED("PORTAL_RIM_REACH", "Debug", "128",
        "Rim predicate: how far OUTSIDE the opening polygon a face may reach and still "
        "count as rim, measured in the plane, in world units."),
    ENVROW_LATCHED("PORTAL_RIM_ANGLE", "Debug", "30",
        "Rim predicate: max angle between a rim face and the opening plane, in degrees."),
    ENVROW_LATCHED("PORTAL_RIM_BROAD", "Debug", "1",
        "Portal-rim sector broadphase: skip rim classification for sectors provably "
        "clear of every opening (1 on, 0 off, 2 = classify live + audit the broadphase)."),
    ENVROW_LATCHED("PORTAL_NUDGE", "Debug", "",
        "Per-portal opening correction \"id:dh,dv[,sh[,sv]];...\" in world units."),
    ENVROW_LIVE("TERRAIN_CULL", "Debug", "3",
        "bit0 HP NCLIP cull, bit1 LP cull (default 3 = both; each mirrors its walk's exact rule)."),
    ENVROW_LIVE("TERRAIN_FLAT", "Debug", "0",
        "Draw terrain untextured."),
    ENVROW_LATCHED("TERRAIN_HQSCALE", "Debug", "4.0",
        "Near/HQ classification scale (default 4.0)."),
    ENVROW_LATCHED("TERRAIN_NEARZ", "Debug", "1.0",
        "Near clip plane in world/16 view-Z (default 1.0)."),
    ENVROW_LATCHED("TERRAIN_NEARHQ", "Debug", "1",
        "Near/HQ subdivide pass enable (default 1)."),
    ENVROW_LIVE("TERRAIN_TEXTBL", "Debug", "0",
        "0 auto; 1 force LQ; 2 force HQ tiledefs."),
    ENVROW_LATCHED("TERRAIN_HIDETAIL", "Debug", "-1",
        "Force hi-detail flag into sector decode cache."),
    ENVROW_LIVE("TERRAIN_OCCL", "Debug", "1",
        "Sector occlusion cull via occlusion grid (default 1)."),
    ENVROW_LIVE("TERRAIN_HOLEFILL", "Debug", "1",
        "Stencil-gated LP hole fill at LOD boundary (default 1)."),
    ENVROW_LATCHED("TERRAIN_LPKEEP", "Debug", "0",
        "Keep ALL near-LP faces (probe)."),
    ENVROW_LATCHED("TERRAIN_FILL_FOGSLACK", "Debug", "0",
        "ir0 slack admitting not-fully-fogged faces to fill."),
    ENVROW_LATCHED("TERRAIN_FILL_THINPX", "Debug", "16",
        "Screen-thin bbox cap for slack-admitted fill faces (default 16)."),
    ENVROW_LATCHED("TERRAIN_FILL_DILATE", "Debug", "0",
        "Fill-tri screen dilation px."),
    ENVROW_LATCHED("TERRAIN_FILL_UP", "Debug", "0.7",
        "Fill upward growth px (default 0.7)."),
    ENVROW_LATCHED("TERRAIN_FILL_QUANT", "Debug", "1",
        "Floor-quantize fill verts (default 1)."),
    ENVROW_LATCHED("TERRAIN_FILL_TIERPULL", "Debug", "1",
        "Apply LP depth pull to fill copies (default 1)."),
    ENVROW_LIVE("TERRAIN_LP_ADMIT", "Debug", "1",
        "Authored per-face LP admission (.s exact; 0 = legacy)."),
    ENVROW_LIVE("TERRAIN_LP_FARCULL", "Debug", "1",
        "LP far cull at m_CullingDistance (.s exact; original LOD only)."),
    ENVROW_LIVE("TERRAIN_HP_DROP", "Debug", "1",
        "HP LOD drop at m_LodDistance (.s exact; 0 = fog-corner legacy)."),
    ENVROW_LIVE("TERRAIN_OT_BIAS", "Debug", "1",
        "Authored .s OT-slot depth offsets, both tiers (0 = raw depth)."),
    ENVROW_LIVE("TERRAIN_GAPFILL", "Debug", "1",
        "Frozen fill-tag LP faces (default 1)."),
    ENVROW_LATCHED("TERRAIN_ARTFIX", "Debug", "1",
        "Recolour known unpainted level-data faces (0 = authentic)."),
    ENVROW_LATCHED("TERRAIN_LOD_FAITHFUL", "Debug", "1",
        "Faithful .s sector binning (original LOD; 0 = admission-only)."),
    ENVROW_LATCHED("TERRAIN_FOGSCALE", "Debug", "1.0",
        "Debug force-override of the fog curve (wins over --fog-scale)."),
    ENVROW_LATCHED("TERRAIN_FOGDPCS", "Debug", "1",
        "Exact per-corner DPCS vertex fog decode (default 1)."),
    ENVROW_LATCHED("TERRAIN_CLUTFOG", "Debug", "1",
        "CLUT-row texel fog ladder (default 1)."),
    ENVROW_LIVE("TERRAIN_LOD_HANDOFF", "Debug", "1",
        "Drop HP beyond fog saturation, substitute LP (default 1)."),
    ENVROW_LATCHED("TERRAIN_PORTAL_CONFORM", "Debug", "1",
        "Terrain-vs-portal-sky depth conformance clamp (default 1)."),
    ENVROW_LATCHED("TERRAIN_NEARVAR", "Debug", "1",
        "Retain near-plane sectors on the GPU clip-fan path (0 = CPU walk)."),
    ENVROW_LIVE("TERRAIN_VANIM", "Debug", "1",
        "Sub-tick lerp of moving-terrain verts (default 1)."),
    ENVROW_LIVE("TERRAIN_TANIM", "Debug", "1",
        "Sub-tick lerp of scrolling terrain textures (default 1)."),
    ENVROW_LATCHED("TERRAIN_ALLVIS", "Debug", "0",
        "Walk ALL sectors, bypass visibility groups (probe)."),
    ENVROW_LATCHED("TERRAIN_NEARQ", "Debug", "0",
        "Read the .s deferred near queue as debug oracle route."),
    ENVROW_LIVE("TERRAIN_NOTEX", "Debug", "0",
        "Feed terrain untextured."),
    ENVROW_LIVE("TERRAIN_TIERCOLOR", "Debug", "0",
        "Colour terrain by LOD tier."),
    ENVROW_LIVE("TERRAIN_TIERONLY", "Debug", "0",
        "Draw only LP (1) or only HP (2) tier."),

    { "fillpick", "Fill-tag inspector", "Debug", NULL, "OPENPETE_FILLPICK",
      "Pick the terrain face under the crosshair and re-judge its LP fill "
      "tag live (P key). Display-only audit tool for the frozen list in "
      "tests/freecam/fill-tags.verdicts; verdicts land in a session file.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      fillpick_get, fillpick_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "fillpick_preview", "Fill-tag preview", "Debug", NULL, "OPENPETE_FILLPICK_PREVIEW",
      "With the inspector engaged, draw every LP face - not just the tagged "
      "ones - tinting the untagged candidates magenta (H key). This is how a "
      "face that should be filling a hole is found before it is tagged.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      fillpick_prev_get, fillpick_prev_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    ENVROW_LATCHED("MOBY_OTBIAS", "Debug", "1",
        "Mirror the .s OT depth pull for moby + Spyro (two latches)."),
    ENVROW_LIVE("MOBY_TRUESCOPE", "Debug", "1",
        "Scope moby og-key yields by true depth (0 = og compound painter)."),
    ENVROW_LATCHED("SHADED_OTBIAS", "Debug", "1",
        "Same pull for the SHADED walk."),
    ENVROW_LATCHED("TRUE_W", "Debug", "7",
        "Perspective W from the true view-Z, not the OT-pulled depth "
        "(bitmask 1=player 2=moby 4=world; 0 = legacy derive-from-depth)."),
    ENVROW_LATCHED("MOBY_SNAP_STALE", "Debug", "1",
        "Snap instead of lerp across stale interp pairs (default 1)."),
    ENVROW_LATCHED("MOBY_IDLE", "Debug", "0",
        "Display-only idle animation for frozen mobys."),
    ENVROW_LATCHED("MOBY_HOLD_PX", "Debug", "10",
        "Max screen motion for tier-2 moby hold replay (default 10)."),
    ENVROW_LATCHED("SHADED_NOHL", "Debug", "0",
        "Disable shaded-walk highlight."),
    ENVROW_LATCHED("ACTOR_FRONT", "Debug", "1",
        "Dragon-rescue actor front pass (default 1)."),
    ENVROW_LATCHED("ENV_C2", "Debug", "14.5",
        "Metal env-reflection calibration constant (default 14.5)."),
    ENVROW_LATCHED("ENV_AZSIGN", "Debug", "1.0",
        "Metal env-reflection azimuth sign."),
    ENVROW_LATCHED("SPYRO_SCRATCH_MAT", "Debug", "0",
        "Force old scratch-block matrix read."),
    ENVROW_LATCHED("SPYRO_UNTEX", "Debug", "0",
        "Draw Spyro feed untextured (bisect)."),
    ENVROW_LATCHED("GEM_FLYOUT_KEY", "Debug", "1",
        "Rotation-fingerprint keying of HUD gem-flyout interp (default 1)."),
    ENVROW_LATCHED("RA_REBASE", "Debug", "1",
        "Runahead camera rebase (default 1)."),
    ENVROW_LATCHED("RA_CAM_COMMIT", "Debug", "1",
        "Committed-timeline camera regions under runahead (default 1)."),
    ENVROW_LATCHED("INTERP_LINEAR", "Debug", "0",
        "Force linear lerp instead of pred1 Hermite."),
    ENVROW_LATCHED("INTERP_PERIOD_UNCAP", "Debug", "0",
        "Diagnostic: uncap interp period."),
    ENVROW_LATCHED("CUTSCENE_PATH_SMOOTH", "Debug", "0",
        "Re-enable cutscene B-spline path smoothing (A/B)."),
    ENVROW_LATCHED("INTERP_PHASE_MS", "Debug", "0",
        "Capture-stamp phase bias vs present grid (0-16)."),
    ENVROW_LATCHED("BRACKET_CHECK", "Debug", "0",
        "Hash gameplay RAM around sub-tick brackets, log leaks."),
    ENVROW_LATCHED("FREECAM_SPIN", "Debug", "0",
        "Scripted continuous yaw sweep (units/tick)."),
    ENVROW_LATCHED("FREECAM_DRIFT", "Debug", "",
        "Scripted per-tick camera drift dx,dy,dz."),
    ENVROW_LATCHED("CAMLOCK_POSE", "Debug", "",
        "Engage the render camera lock at x,y,z,yaw,pitch (a camlock: "
        "log line pastes verbatim)."),
    ENVROW_LATCHED("CAMLOCK_FRAME", "Debug", "",
        "Engage the render camera lock (latch the live view) at VBL N."),
    ENVROW_LATCHED("S3_SCALE", "Debug", "1.0",
        "S3 model-units to S1 world-units scale (default 1.0)."),
    ENVROW_LATCHED("S3_ACTOR_TEST", "Debug", "0",
        "Draw the S3 hover-cycle test actor near the player."),
    ENVROW_LIVE("S3_ACTOR_ANIM", "Debug", "",
        "Anim id for the S3 test actor."),
    ENVROW_LATCHED("OT_TRUNCATE", "Debug", "",
        "Submit only first N OT slots (paint-order bisection)."),
    ENVROW_LATCHED("TRANSITION_PROBE", "Debug", "0",
        "One line per (func,frame) during level transitions."),

    /* -------- Debug diagnostics (stderr censuses; flip on, watch, flip off) -------- */
    ENVROW_LATCHED("REEXTRACT_DBG", "Debug diagnostics", "0",
        "Per-present alpha/bracket summary of the re-extract spine."),
    ENVROW_LATCHED("REEXTRACT_FEEDDBG", "Debug diagnostics", "0",
        "Per-frame feeder freshness/counts (spyro + moby)."),
    ENVROW_LATCHED("CANON_CAPDBG", "Debug diagnostics", "0",
        "1/200-frame canonical-capture row prints."),
    ENVROW_LATCHED("CUTPATH_DBG", "Debug diagnostics", "0",
        "1/s cutscene-path engage/reject census."),
    ENVROW_LATCHED("CAP_ROLL_DBG", "Debug diagnostics", "0",
        "One line per interp ring roll."),
    ENVROW_LATCHED("ALPHA_MAP_DBG", "Debug diagnostics", "0",
        "Per-present present-time to interp-time mapping."),
    ENVROW_LATCHED("PART_INTERP_DBG", "Debug diagnostics", "0",
        "1/s particle pool-interp verdict census."),
    ENVROW_LATCHED("ANIMDBG", "Debug diagnostics", "0",
        "Spyro anim segment/blend + sub-tick vertex probe."),
    ENVROW_LATCHED("ACTORDBG", "Debug diagnostics", "0",
        "1/100-frame actor-emit projection stats."),
    ENVROW_LATCHED("S3_DBG", "Debug diagnostics", "0",
        "1/100-frame S3 decode/projection stats."),
    ENVROW_LATCHED("MOBY_TIER_DBG", "Debug diagnostics", "0",
        "Extraction-tier census (proj/walk us, cache hits)."),
    ENVROW_LATCHED("MOBY_OCCL_DBG", "Debug diagnostics", "0",
        "1/s occlusion cull/seen breakdown."),
    ENVROW_LATCHED("MOBY_ANIMSW_DBG", "Debug diagnostics", "0",
        "Animation-switch cross/snap counters."),
    ENVROW_LATCHED("MOBY_GPF_DBG", "Debug diagnostics", "0",
        "One line per GPF re-extract."),
    ENVROW_LATCHED("MOBY_CLASSDBG", "Debug diagnostics", "0",
        "One line per drawn moby: class + vaddr + world position."),
    ENVROW_LATCHED("MOBY_CLERP_DBG", "Debug diagnostics", "0",
        "Face-0 colour-lerp ramp per present."),
    ENVROW_LATCHED("MOBY_INTERP_MISS", "Debug diagnostics", "0",
        "Record mobys with no interp snapshot."),
    ENVROW_LATCHED("SHADED_INTERP_MISS", "Debug diagnostics", "0",
        "Same for shaded mobys."),
    ENVROW_LATCHED("MOBY_TRIDBG", "Debug diagnostics", "",
        "Log moby walk tris in a screen box x0,y0,x1,y1,fA,fB."),
    ENVROW_LATCHED("SCENE_BOXDBG", "Debug diagnostics", "",
        "Log scene tris with centroid in a PSX-screen box over a frame window."),
    ENVROW_LATCHED("SCENE_CAPDBG", "Debug diagnostics", "0",
        "Print new peaks of scene-tri demand vs caps."),
    ENVROW_LATCHED("TERRAIN_CACHE_DBG", "Debug diagnostics", "0",
        "~1/s sector-cache hit/miss census."),
    ENVROW_LATCHED("TERRAIN_OCCL_DBG", "Debug diagnostics", "0",
        "Sector occlusion-cull census."),
    ENVROW_LATCHED("TERRAIN_TIMERS", "Debug diagnostics", "0",
        "~1/s per-sector walk-cost census."),
    ENVROW_LATCHED("TERRAIN_VISDBG", "Debug diagnostics", "0",
        "Visible-sector list diagnostics."),
    ENVROW_LATCHED("TERRAIN_ANIMDBG", "Debug diagnostics", "0",
        "Per-present env-anim change census."),
    ENVROW_LATCHED("TEXPACK_CATCENSUS", "Debug diagnostics", "0",
        "HD-texpack category census (which categories resolve per level)."),
    ENVROW_LATCHED("TEXPACK_ANIMPROBE", "Debug diagnostics", "",
        "Raw Tiledef words of record N each present (env-anim rewrite shape)."),
    ENVROW_LATCHED("TEXSEL_DBG", "Debug diagnostics", "0",
        "Terrain texture-id selector match census."),
    ENVROW_LATCHED("TEXSEL_DBG2", "Debug diagnostics", "0",
        "Material texture-id selector census (compose side)."),
    ENVROW_LATCHED("TEXSEL_DBG3", "Debug diagnostics", "0",
        "Mod-shader selector census."),
    ENVROW_LATCHED("TERRAIN_VANIMDBG", "Debug diagnostics", "0",
        "Vert-anim refresh census."),
    ENVROW_LIVE("TERRAIN_FOGDBG", "Debug diagnostics", "0",
        "Fog decode debug prints."),
    ENVROW_LATCHED("TERRAIN_CALDBG", "Debug diagnostics", "0",
        "Near-classification calibration stats vs .s queue."),
    ENVROW_LATCHED("TERRAIN_NEARDBG", "Debug diagnostics", "0",
        "Census of straddling foreground quads by route."),
    ENVROW_LIVE("TERRAIN_UVDBG", "Debug diagnostics", "0",
        "UV/tile A/B vs captured .s prims."),
    ENVROW_LIVE("TERRAIN_UVBOX", "Debug diagnostics", "",
        "Box filter x0,y0,x1,y1 for UVDBG."),
    ENVROW_LIVE("TERRAIN_UVALL", "Debug diagnostics", "0",
        "UVDBG print-all modifier."),
    ENVROW_LIVE("TERRAIN_POINT", "Debug diagnostics", "",
        "Frontmost native tri + .s prim under screen points x,y;x,y."),
    ENVROW_LIVE("PARTICLE_DEBUG", "Debug diagnostics", "0",
        "Native-vs-.s particle selector counts per present."),
    ENVROW_LATCHED("STREAK_DBG", "Debug diagnostics", "0",
        "Log emitted line/fan legs over 300 px."),
    ENVROW_LATCHED("PART_PHASE_DBG", "Debug diagnostics", "0",
        "Per-present phase-lerp trace of first particle."),
    ENVROW_LATCHED("GLOW_ANIM_DBG", "Debug diagnostics", "0",
        "Glow raw-change census + phase trace."),
    ENVROW_LATCHED("SPARKLE_INTERP_DBG", "Debug diagnostics", "0",
        "Per-present sparkle slot-0 phase trace."),
    ENVROW_LATCHED("PORTAL_MAT_DBG", "Debug diagnostics", "0",
        "Per-dome derived-vs-GTE matrix validation print."),
    ENVROW_LATCHED("PORTAL_REEXTRACT_DBG", "Debug diagnostics", "0",
        "1/20-frame portal replay-state print."),
    ENVROW_LIVE("POLYBUF_GUARD", "Debug diagnostics", "0",
        "polygon-buffer overflow detector: logs when the prim cursor "
        "(D_800757B0) passes the buffer end (D_800756FC) and starts "
        "overwriting g_WorldOT. 0=off (default), 1=on, "
        "verbose=per-frame headroom"),
    ENVROW_LATCHED("NOLOD_DOME_DBG", "Debug diagnostics", "0",
        "Per-event forced-dome feed print under extended LOD."),
#endif /* OPENPETE_HIDE_DEBUG_FLAGS */
};

/* Working table: the built-ins, copied once, plus room for module-contributed
 * rows. One flat array because the UI walks a single {ptr, count} and groups by
 * section; adds happen at feature apply-options time, long before the overlay
 * exists, so no row pointer the UI holds can be invalidated by an insertion. */
#define KNOB_BUILTIN_COUNT (sizeof(g_builtin) / sizeof(g_builtin[0]))
#define KNOB_EXTRA_MAX     16

static knob_desc_t g_rows[KNOB_BUILTIN_COUNT + KNOB_EXTRA_MAX];
static size_t      g_row_count;

static void rows_init(void) {
    if (g_row_count) return;
    memcpy(g_rows, g_builtin, sizeof g_builtin);
    g_row_count = KNOB_BUILTIN_COUNT;
}

knob_desc_t *knob_registry(size_t *count) {
    rows_init();
    if (count) *count = g_row_count;
    return g_rows;
}

int openpete_knob_add(const knob_desc_t *row) {
    if (!row) return -1;
    rows_init();
    if (g_row_count >= KNOB_BUILTIN_COUNT + KNOB_EXTRA_MAX) {
        OPL_WARN_F("knobs", "no room for row \"%s\" (extra capacity %d)",
                   row->id ? row->id : "?", KNOB_EXTRA_MAX);
        return -1;
    }

    /* Sections are rendered as contiguous runs, so a row joins its section by
     * landing right after that section's last row; an unknown section starts
     * its own run at the end. */
    size_t at = g_row_count;
    for (size_t i = 0; i < g_row_count; i++)
        if (row->section && !strcmp(g_rows[i].section, row->section))
            at = i + 1;

    memmove(&g_rows[at + 1], &g_rows[at],
            (g_row_count - at) * sizeof g_rows[0]);
    g_rows[at] = *row;
    g_row_count++;
    return 0;
}

/* ------------------------------------------------------------------ */
/* boot resolution: value + source stamping                           */
/* ------------------------------------------------------------------ */

/* Where this row's value came from, straight from the config resolver's
 * decision table — the one place the layer order is implemented.
 *
 * The overlay's three-way source tag predates openpete.toml, so a file
 * layer has to land in one of its buckets: an [args]-or-bare-key layer
 * reads as CLI and an [env] layer reads as ENV, matching the spelling
 * that was actually materialized. The precise layer name (and whether a
 * launch-tier override is pinning the row) is shown separately, from
 * openpete_config_knob_layer / _pinned. */
static knob_src_t knob_src_from_config(const knob_desc_t *k) {
    const char *layer = openpete_config_knob_layer(k->id);
    if (!layer || !strcmp(layer, "default")) return KNOB_SRC_DEFAULT;
    if (!strcmp(layer, "env")) return KNOB_SRC_ENV;
    size_t n = strlen(layer);
    if (n >= 4 && !strcmp(layer + n - 4, ".env")) return KNOB_SRC_ENV;
    return KNOB_SRC_CLI;
}

void knob_registry_init(const struct openpete_options *opts) {
    g_opts = opts;

    size_t n;
    knob_desc_t *ks = knob_registry(&n);
    for (size_t i = 0; i < n; i++) {
        knob_desc_t *k = &ks[i];
        k->boot_src = knob_src_from_config(k);

        /* Seed the row's displayed value from the parsed options. WHERE the
         * value came from is not decided here: config.c resolved the whole
         * layer stack at boot, and re-deriving it from each field's unset
         * sentinel would be a second implementation of the precedence rules,
         * free to drift from the first. The post-pass below asks the
         * resolver. */
        if (!strcmp(k->id, "fullscreen")) {
            k->stored_num = opts->fullscreen ? 1 : 0;
        } else if (!strcmp(k->id, "aspect")) {
            if (opts->aspect_w_over_h > 0.0f) {
                k->stored_num = nearest_enum_idx(k_aspect_vals + 1, 5,
                                                 opts->aspect_w_over_h) + 1;
            } else {
                k->stored_num = 0;   /* auto (also the default) */
            }
        } else if (!strcmp(k->id, "ssaa")) {
            k->stored_num = ssaa_idx_of(opts->ssaa > 0 ? opts->ssaa : 1);
        } else if (!strcmp(k->id, "vsync")) {
            if (opts->vsync != -1) {
                k->stored_num = opts->vsync ? 1 : 0;
            } else {
                const char *e = getenv("OPENPETE_PRESENT_MODE");
                if (e && (!strcmp(e, "immediate") || !strcmp(e, "mailbox"))) {
                    k->stored_num = 0;
                } else {
                    k->stored_num = 1;   /* default FIFO */
                }
            }
        } else if (!strcmp(k->id, "perf_stats")) {
            k->stored_num = opts->perf_stats;
        } else if (!strcmp(k->id, "perf_position")) {
            k->stored_num = opts->perf_position;
        } else if (!strcmp(k->id, "osd")) {
            k->stored_num = opts->osd ? 1 : 0;
        } else if (!strcmp(k->id, "osd_position")) {
            k->stored_num = opts->osd_position;
        } else if (!strcmp(k->id, "osd_timeout")) {
            k->stored_num = (double)opts->osd_timeout_ms / 1000.0;
        } else if (!strcmp(k->id, "vertex_precision")) {
            k->stored_num = (opts->vertex_precision == OPENPETE_VERTEX_PRECISION_INTEGER);
        } else if (!strcmp(k->id, "raster_quantize")) {
            /* mirror the renderer's auto-pair resolve (-1 = 1x iff integer snap) */
            k->stored_num = (opts->raster_quantize >= 0) ? opts->raster_quantize
                          : (opts->vertex_precision == OPENPETE_VERTEX_PRECISION_INTEGER ? 1 : 0);
        } else if (!strcmp(k->id, "dither")) {
            k->stored_num = opts->dither ? 1 : 0;
        } else if (!strcmp(k->id, "texture_mapping")) {
            k->stored_num = (opts->texture_mapping == OPENPETE_TEXTURE_MAPPING_AFFINE);
        } else if (!strcmp(k->id, "lod")) {
            k->stored_num = (opts->lod != OPENPETE_LOD_ORIGINAL);
        } else if (!strcmp(k->id, "fog")) {
            k->stored_num = openpete_fog_enabled() ? 1 : 0;
        } else if (!strcmp(k->id, "fog_scale")) {
            k->stored_num = openpete_fog_scale;
        } else if (!strcmp(k->id, "render_fps")) {
            k->stored_num = opts->render_fps;
        } else if (!strcmp(k->id, "rewind")) {
            k->stored_num = opts->rewind_window;
        } else if (!strcmp(k->id, "tick_scale")) {
            k->stored_num = opts->tick_scale;
        } else if (!strcmp(k->id, "runahead")) {
            k->stored_num = opts->runahead;
        } else if (!strcmp(k->id, "fov")) {
            k->stored_num = opts->fov_deg;
        } else if (!strcmp(k->id, "mute")) {
            k->stored_num = opts->mute ? 1 : 0;
        } else if (!strcmp(k->id, "sfx")) {
            k->stored_num = opts->sfx_volume;
        } else if (!strcmp(k->id, "music")) {
            k->stored_num = opts->music_volume;
        } else if (!strcmp(k->id, "vibration")) {
            k->stored_num = opts->vibration;
        } else if (!strcmp(k->id, "record_cfr")) {
            k->stored_num = opts->record_cfr;
        } else if (!strcmp(k->id, "record_frames")) {
            k->stored_num = opts->record_frames;
        } else if (!strcmp(k->id, "record_fps_overlay")) {
            if (opts->record_fps_overlay >= 0) {
                k->stored_num = opts->record_fps_overlay ? 1 : 0;
            } else {
                const char *e = getenv("OPENPETE_FPS_IN_RECORD");
                k->stored_num = (e && *e && *e != '0') ? 1 : 0;
            }
        } else if (!strcmp(k->id, "renderer")) {
            k->stored_num = opts->renderer;
        } else if (!strcmp(k->id, "skip_to_level")) {
            k->stored_num = opts->skip_to_level;
        } else if (!strcmp(k->id, "log_format")) {
            k->stored_num = opts->log_format;
        } else if (!strcmp(k->id, "freecam") || !strcmp(k->id, "camlock")) {
            /* Both start disengaged whatever armed them: the env var (or
             * OPENPETE_CAMLOCK_POSE) requests engagement later, at a point
             * the feature itself picks. */
            k->stored_num = 0;
        }

        /* Stamp the boot value for the "reset to boot" affordance. Actions
         * carry no value to reset to. */
        if (k->type == KNOB_ACTION) {
            continue;
        } else if (k->type == KNOB_STRING) {
            if (k->get_str) k->get_str(k, k->boot_str, sizeof k->boot_str);
        } else {
            k->boot_num = k->get_num ? k->get_num(k) : k->stored_num;
        }
    }
}

int openpete_knob_bind_str(const char *id,
                           void (*get_str)(const knob_desc_t *k,
                                           char *buf, size_t cap),
                           int (*set_str)(knob_desc_t *k, const char *v)) {
    size_t n;
    knob_desc_t *ks = knob_registry(&n);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(ks[i].id, id) != 0) continue;
        if (get_str) ks[i].get_str = get_str;
        if (set_str) { ks[i].set_str = set_str; ks[i].mut = KNOB_LIVE; }
        return 0;
    }
    return -1;   /* row absent (compiled out) — nothing to bind */
}

int knob_config_value_str(const knob_desc_t *k, char *buf, size_t cap) {
    if (!k || k->type == KNOB_ACTION) return -1;
    if (cfg_schema_find_id(k->id) < 0) return -1;   /* not a config knob */

    /* The SSAA row is an enum of "off"/"2x"/"3x"/"4x" because that is what
     * reads well in a menu; the flag takes the factor (1 = off). Every other
     * enum row already displays the CLI's own words. */
    if (!strcmp(k->id, "ssaa")) {
        int idx = (int)(k->get_num ? k->get_num(k) : k->stored_num);
        if (idx < 0 || idx >= K_SSAA_N) idx = 0;
        snprintf(buf, cap, "%d", k_ssaa_vals[idx]);
        return 0;
    }

    knob_value_str(k, buf, cap);
    /* get_str placeholders ("(default)", "(none)", "(unset - 1)") describe
     * the absence of a value; writing one back as a literal would turn it
     * into a real setting on the next boot. */
    if (buf[0] == '(') return -1;
    return 0;
}

int knob_registry_save_edited(void) {
    size_t n;
    knob_desc_t *ks = knob_registry(&n);
    int saved = 0;
    for (size_t i = 0; i < n; i++) {
        knob_desc_t *k = &ks[i];
        if (k->type == KNOB_ACTION) continue;

        /* Only what the user actually moved. Saving unchanged rows would
         * bake this launch's whole resolved state into the file, so a knob
         * they never touched would stop following its default. */
        char now[KNOB_STR_MAX];
        if (k->type == KNOB_STRING) {
            if (!k->get_str) continue;
            k->get_str(k, now, sizeof now);
            if (!strcmp(now, k->boot_str)) continue;
        } else {
            double v = k->get_num ? k->get_num(k) : k->stored_num;
            if (v == k->boot_num) continue;
        }
        if (knob_config_value_str(k, now, sizeof now) != 0) continue;
        if (openpete_config_save_knob(k->id, now) == 0) saved++;
    }
    if (openpete_config_save_flush() != 0) return -1;
    return saved;
}

void knob_registry_late_stamp(void) {
    size_t n;
    knob_desc_t *ks = knob_registry(&n);
    for (size_t i = 0; i < n; i++) {
        if (!strcmp(ks[i].id, "render_fps") ||
            !strcmp(ks[i].id, "tick_scale") ||
            !strcmp(ks[i].id, "runahead")) {
            /* The pacer seeds its live state (resolved render-fps target,
             * tick scale, gated runahead depth) at host-loop start, after
             * knob_registry_init ran; adopt the settled value as the boot
             * value so the row doesn't read as "edited" on first open. */
            ks[i].boot_num   = ks[i].get_num(&ks[i]);
            ks[i].stored_num = ks[i].boot_num;
        }
    }
}

void knob_value_str(const knob_desc_t *k, char *buf, size_t cap) {
    if (k->type == KNOB_ACTION) {
        if (k->get_str) k->get_str(k, buf, cap);
        else snprintf(buf, cap, "%s", k->action_label ? k->action_label : "Apply");
        return;
    }
    if (k->type == KNOB_STRING) {
        if (k->get_str) k->get_str(k, buf, cap);
        else snprintf(buf, cap, "?");
        return;
    }
    double v = k->get_num ? k->get_num(k) : k->stored_num;
    switch (k->type) {
    case KNOB_BOOL:  snprintf(buf, cap, "%s", v != 0.0 ? "on" : "off"); break;
    case KNOB_INT:   snprintf(buf, cap, "%d", (int)v); break;
    case KNOB_FLOAT: snprintf(buf, cap, "%.3g", v); break;
    case KNOB_ENUM: {
        int i = (int)v;
        if (k->enum_values && i >= 0 && i < k->enum_count)
            snprintf(buf, cap, "%s", k->enum_values[i]);
        else
            snprintf(buf, cap, "%d", i);
        break;
    }
    default: snprintf(buf, cap, "?"); break;
    }
}
