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
#include "openpete.h"
#include "openpete_log.h"
#include "openpete_native_render.h"
#include "openpete_freecam.h"
#include "openpete_rewind.h"
#include "features/no_lod.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const openpete_options_t *g_opts;

/* ------------------------------------------------------------------ */
/* value plumbing helpers                                             */
/* ------------------------------------------------------------------ */

static double get_stored(const knob_desc_t *k) { return k->stored_num; }

/* ---- Display ---- */

static double fullscreen_get(const knob_desc_t *k) {
    (void)k; return openpete_native_fullscreen_active() ? 1.0 : 0.0;
}
static int fullscreen_set(knob_desc_t *k, double v) {
    (void)k;
    if ((v != 0.0) != (openpete_native_fullscreen_active() != 0))
        openpete_native_toggle_fullscreen();
    return 0;
}

static const char *const k_aspect_names[] = { "auto", "4:3", "16:10", "16:9", "21:9", "32:9" };
static const float       k_aspect_vals[]  = { 0.0f, 4.f/3.f, 16.f/10.f, 16.f/9.f, 21.f/9.f, 32.f/9.f };
static int aspect_set(knob_desc_t *k, double v) {
    int i = (int)v;
    if (i < 0 || i >= (int)(sizeof(k_aspect_vals)/sizeof(k_aspect_vals[0]))) return -1;
    openpete_native_set_aspect(k_aspect_vals[i]);
    k->stored_num = i;
    return 0;
}

static const char *const k_ssaa_names[] = { "1x", "2x", "4x", "8x", "16x" };
static int ssaa_set(knob_desc_t *k, double v) {
    int i = (int)v;
    if (i < 0 || i > 4) return -1;
    openpete_native_set_ssaa(1 << i);
    k->stored_num = i;
    return 0;
}

static int vsync_set(knob_desc_t *k, double v) {
    openpete_native_set_vsync(v != 0.0);
    k->stored_num = (v != 0.0);
    return 0;
}

static int showfps_set(knob_desc_t *k, double v) {
    openpete_native_set_fps_overlay(v != 0.0);
    k->stored_num = (v != 0.0);
    return 0;
}

static const char *const k_vprec_names[] = { "subpixel", "integer" };
static int vprec_set(knob_desc_t *k, double v) {
    openpete_native_set_vertex_precision((int)v == 1);
    k->stored_num = (int)v;
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

static int renderfps_set(knob_desc_t *k, double v) {
    openpete_set_render_fps((int)v);
    k->stored_num = (int)v;
    return 0;
}

static double tickscale_get(const knob_desc_t *k) { (void)k; return g_opts ? g_opts->tick_scale : 1.0; }
static double runahead_get(const knob_desc_t *k)  { (void)k; return g_opts ? (double)g_opts->runahead : 0.0; }
static double fov_get(const knob_desc_t *k)       { (void)k; return g_opts ? g_opts->fov_deg : 0.0; }

static int rewind_set(knob_desc_t *k, double v) {
    if (v < 0) return -1;
    openpete_rewind_configure((int)v);
    k->stored_num = (int)v;
    return 0;
}

/* ---- Audio ---- */

static double mute_get(const knob_desc_t *k) { (void)k; return (g_opts && g_opts->mute) ? 1.0 : 0.0; }

/* ---- Paths (boot-resolved; display the same precedence the consumer used) */

static void path_get_str(const knob_desc_t *k, char *buf, size_t cap) {
    const char *cli = NULL;
    if (g_opts) {
        if      (!strcmp(k->id, "disc"))        cli = g_opts->disc_path;
        else if (!strcmp(k->id, "texpack_dir")) cli = g_opts->texpack_dir;
        else if (!strcmp(k->id, "mods_dir"))    cli = g_opts->mods_dir;
        else if (!strcmp(k->id, "card_dir"))    cli = g_opts->card_dir;
        else if (!strcmp(k->id, "record"))      cli = g_opts->record_path;
        else if (!strcmp(k->id, "bk2"))         cli = g_opts->bk2_path;
    }
    const char *env = (k->env && !cli) ? getenv(k->env) : NULL;
    snprintf(buf, cap, "%s", cli ? cli : (env ? env : "(default)"));
}

/* ---- Gameplay / debug ---- */

static double skiplevel_get(const knob_desc_t *k) { (void)k; return g_opts ? (double)g_opts->skip_to_level : 0.0; }

static double freecam_get(const knob_desc_t *k) { (void)k; return openpete_freecam_active() ? 1.0 : 0.0; }
static int freecam_set(knob_desc_t *k, double v) {
    (void)k;
    if ((v != 0.0) != (openpete_freecam_active() != 0))
        openpete_freecam_toggle();
    return 0;
}

/* ------------------------------------------------------------------ */
/* the table                                                          */
/* ------------------------------------------------------------------ */

#define ENUM_ROW(vals) vals, (int)(sizeof(vals)/sizeof((vals)[0]))

static knob_desc_t g_knobs[] = {
    /* -------- Display -------- */
    { "fullscreen", "Fullscreen", "Display", "--fullscreen", NULL,
      "Borderless-desktop fullscreen (same toggle as Alt+Enter / double-click).",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      fullscreen_get, fullscreen_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "aspect", "Aspect ratio", "Display", "--aspect", NULL,
      "Hor+ render aspect; auto follows the window shape. Picking a fixed "
      "ratio windowed resizes the window to match.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_aspect_names), 0, 0,
      get_stored, aspect_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "ssaa", "Supersampling", "Display", "--ssaa", NULL,
      "Native supersampling factor. Applying reallocates the render target "
      "next present (brief hitch at large factors; GPU-max capped).",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_ssaa_names), 0, 0,
      get_stored, ssaa_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "vsync", "VSync", "Display", "--vsync", "OPENPETE_PRESENT_MODE",
      "FIFO (on) vs immediate/mailbox (off). Applying rebuilds the swapchain "
      "on the next frame.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      get_stored, vsync_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "show_fps", "FPS overlay", "Display", "--show-fps", NULL,
      "Top-left FPS counter (native renderer).",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      get_stored, showfps_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "vertex_precision", "Vertex precision", "Display", "--vertex-precision", NULL,
      "PSX integer-grid vertex snap (authentic polygon wobble) vs smooth "
      "subpixel. Display-only.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_vprec_names), 0, 0,
      get_stored, vprec_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

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
      "Live-read by the terrain every walk; editing here needs a setter that "
      "also updates the LOD/fog autopilot bookkeeping - restart-only for now.",
      KNOB_FLOAT, KNOB_RESTART_ONLY, NULL, 0, 0.25f, 128.0f,
      fogscale_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

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
      "Target present rate. -1 = display refresh, 0 = legacy match-sim "
      "pacer. Above ~30 arms sub-tick interpolation; vsync clamps to "
      "refresh.",
      KNOB_INT, KNOB_LIVE, NULL, 0, -1, 1000,
      get_stored, renderfps_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "tick_scale", "Tick scale", "Pacing", "--tick-scale", NULL,
      "Engine speed multiplier (slow-mo / speed-up; sim and presents "
      "together). Boot-latched into the pacer today.",
      KNOB_FLOAT, KNOB_RESTART_ONLY, NULL, 0, 0.05f, 8.0f,
      tickscale_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "runahead", "Runahead", "Pacing", "--runahead", NULL,
      "Present-timeline lead of N sim ticks (latency cut, ~N extra ticks of "
      "CPU). Boot-latched (snapshot buffers + mod-capability gate).",
      KNOB_INT, KNOB_RESTART_ONLY, NULL, 0, 0, 3,
      runahead_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "rewind", "Rewind window", "Pacing", "--rewind", NULL,
      "Backspace-rewind window in seconds (0 = off). APPLYING CLEARS the "
      "accumulated rewind history; ~2 MiB per second.",
      KNOB_INT, KNOB_LIVE, NULL, 0, 0, 600,
      get_stored, rewind_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "fov", "FOV override", "Pacing", "--fov", NULL,
      "Horizontal FOV in degrees (0 = game default). PsyCross reference "
      "renderer only; applied once at boot today.",
      KNOB_FLOAT, KNOB_RESTART_ONLY, NULL, 0, 0, 170,
      fov_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    /* -------- Audio -------- */
    { "mute", "Mute", "Audio", "--mute", NULL,
      "Mute all audio at the OpenAL layer (SPU SFX + XA music + WAV "
      "override). Applied once at boot today.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      mute_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    /* -------- Paths -------- */
    { "disc", "Disc image", "Paths", "--disc", NULL,
      "BIN/CUE the game boots from.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, path_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "texpack_dir", "Texture pack dir", "Paths", "--texpack-dir", "OPENPETE_TEXPACK_DIR",
      "HD texture-replacement PNG directory (scanned once at renderer init).",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, path_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "mods_dir", "Mods dir", "Paths", "--mods-dir", "OPENPETE_MODS_DIR",
      "Mods root (mod.toml subdirs; load order via enabled.txt).",
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

    /* -------- Recording -------- */
    { "record", "Record video", "Recording", "--record", NULL,
      "True-VFR gameplay capture to a video file (latched at boot).",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, path_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },

    { "bk2", "bk2 replay", "Recording", "--bk2", NULL,
      "Replay a BizHawk movie into the pad (loaded at loop start).",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, path_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0 },
};

knob_desc_t *knob_registry(size_t *count) {
    if (count) *count = sizeof(g_knobs) / sizeof(g_knobs[0]);
    return g_knobs;
}

/* ------------------------------------------------------------------ */
/* boot resolution: value + source stamping                           */
/* ------------------------------------------------------------------ */

static int nearest_enum_idx(const float *vals, int n, float v) {
    int best = 0;
    float bd = 1e30f;
    for (int i = 0; i < n; i++) {
        float d = fabsf(vals[i] - v);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

void knob_registry_init(const struct openpete_options *opts) {
    g_opts = opts;

    size_t n;
    knob_desc_t *ks = knob_registry(&n);
    for (size_t i = 0; i < n; i++) {
        knob_desc_t *k = &ks[i];
        k->boot_src = KNOB_SRC_DEFAULT;

        /* Seed stored values + source per knob. CLI beats env beats default —
         * detected via each field's documented unset sentinel, mirroring the
         * consumer-side resolution (never re-inventing it). */
        if (!strcmp(k->id, "fullscreen")) {
            k->stored_num = opts->fullscreen ? 1 : 0;
            if (opts->fullscreen) k->boot_src = KNOB_SRC_CLI;
        } else if (!strcmp(k->id, "aspect")) {
            if (opts->aspect_w_over_h > 0.0f) {
                k->stored_num = nearest_enum_idx(k_aspect_vals + 1, 5,
                                                 opts->aspect_w_over_h) + 1;
                k->boot_src = KNOB_SRC_CLI;
            } else {
                k->stored_num = 0;   /* auto (also the default) */
            }
        } else if (!strcmp(k->id, "ssaa")) {
            int f = opts->ssaa > 0 ? opts->ssaa : 1, idx = 0;
            while ((1 << idx) < f && idx < 4) idx++;
            k->stored_num = idx;
            if (opts->ssaa > 1) k->boot_src = KNOB_SRC_CLI;
        } else if (!strcmp(k->id, "vsync")) {
            if (opts->vsync != -1) {
                k->stored_num = opts->vsync ? 1 : 0;
                k->boot_src = KNOB_SRC_CLI;
            } else {
                const char *e = getenv("OPENPETE_PRESENT_MODE");
                if (e && (!strcmp(e, "immediate") || !strcmp(e, "mailbox"))) {
                    k->stored_num = 0;
                    k->boot_src = KNOB_SRC_ENV;
                } else {
                    k->stored_num = 1;   /* default FIFO */
                }
            }
        } else if (!strcmp(k->id, "show_fps")) {
            k->stored_num = opts->fps_overlay ? 1 : 0;
            if (!opts->fps_overlay) k->boot_src = KNOB_SRC_CLI; /* default ON */
        } else if (!strcmp(k->id, "vertex_precision")) {
            k->stored_num = (opts->vertex_precision == OPENPETE_VERTEX_PRECISION_INTEGER);
            if (k->stored_num != 0) k->boot_src = KNOB_SRC_CLI;
        } else if (!strcmp(k->id, "lod")) {
            k->stored_num = (opts->lod != OPENPETE_LOD_ORIGINAL);
            if (k->stored_num != 0) k->boot_src = KNOB_SRC_CLI;
        } else if (!strcmp(k->id, "fog")) {
            k->stored_num = openpete_fog_enabled() ? 1 : 0;
            if (opts->fog_scale > 0.0f) k->boot_src = KNOB_SRC_CLI;
        } else if (!strcmp(k->id, "fog_scale")) {
            k->stored_num = openpete_fog_scale;
            if (opts->fog_scale > 0.0f) k->boot_src = KNOB_SRC_CLI;
        } else if (!strcmp(k->id, "render_fps")) {
            k->stored_num = opts->render_fps;
            if (opts->render_fps != -1) k->boot_src = KNOB_SRC_CLI;
        } else if (!strcmp(k->id, "rewind")) {
            k->stored_num = opts->rewind_window;
            if (opts->rewind_window > 0) k->boot_src = KNOB_SRC_CLI;
        } else if (!strcmp(k->id, "tick_scale")) {
            k->stored_num = opts->tick_scale;
            if (opts->tick_scale != 1.0) k->boot_src = KNOB_SRC_CLI;
        } else if (!strcmp(k->id, "runahead")) {
            k->stored_num = opts->runahead;
            if (opts->runahead > 0) k->boot_src = KNOB_SRC_CLI;
        } else if (!strcmp(k->id, "fov")) {
            k->stored_num = opts->fov_deg;
            if (opts->fov_deg > 0.0f) k->boot_src = KNOB_SRC_CLI;
        } else if (!strcmp(k->id, "mute")) {
            k->stored_num = opts->mute ? 1 : 0;
            if (opts->mute) k->boot_src = KNOB_SRC_CLI;
        } else if (!strcmp(k->id, "renderer")) {
            k->stored_num = opts->renderer;
            if (opts->renderer != OPENPETE_RENDERER_VK) k->boot_src = KNOB_SRC_CLI;
        } else if (!strcmp(k->id, "skip_to_level")) {
            k->stored_num = opts->skip_to_level;
            if (opts->skip_to_level > 0) k->boot_src = KNOB_SRC_CLI;
        } else if (!strcmp(k->id, "freecam")) {
            k->stored_num = 0;
            { const char *e = getenv("OPENPETE_FREECAM");
              if (e && *e && *e != '0') k->boot_src = KNOB_SRC_ENV; }
        } else if (k->type == KNOB_STRING) {
            /* Path-style rows: CLI field non-NULL beats env beats default. */
            char buf[KNOB_STR_MAX];
            path_get_str(k, buf, sizeof buf);
            const char *cli = NULL;
            if      (!strcmp(k->id, "disc"))        cli = opts->disc_path;
            else if (!strcmp(k->id, "texpack_dir")) cli = opts->texpack_dir;
            else if (!strcmp(k->id, "mods_dir"))    cli = opts->mods_dir;
            else if (!strcmp(k->id, "card_dir"))    cli = opts->card_dir;
            else if (!strcmp(k->id, "record"))      cli = opts->record_path;
            else if (!strcmp(k->id, "bk2"))         cli = opts->bk2_path;
            if (cli)                          k->boot_src = KNOB_SRC_CLI;
            else if (k->env && getenv(k->env)) k->boot_src = KNOB_SRC_ENV;
        }

        /* Stamp the boot value for the "reset to boot" affordance. */
        if (k->type == KNOB_STRING) {
            if (k->get_str) k->get_str(k, k->boot_str, sizeof k->boot_str);
        } else {
            k->boot_num = k->get_num ? k->get_num(k) : k->stored_num;
        }
    }
}

void knob_value_str(const knob_desc_t *k, char *buf, size_t cap) {
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
