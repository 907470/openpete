/* core/runtime/config_schema.c — see config_schema.h for what this is for. */

#include "config_schema.h"

#include <string.h>

static const char *const v_perf[]     = { "off", "fps", "detailed", NULL };
static const char *const v_anchor[]   = { "top-left", "top-center", "top-right",
                                          "bottom-left", "bottom-center",
                                          "bottom-right", NULL };
static const char *const v_vprec[]    = { "subpixel", "integer", NULL };
static const char *const v_texmap[]   = { "perspective", "affine", NULL };
static const char *const v_lod[]      = { "original", "extended", NULL };
static const char *const v_renderer[] = { "psycross", "vk", NULL };
static const char *const v_logfmt[]   = { "plain", "json", NULL };

/* Ordered to mirror knob_registry.c's sections so the two read side by
 * side. `aspect`, `window`, `skip_to_level` and `skip_to_cutscene` are
 * CFG_STRING because their CLI vocabulary is a small language
 * ("auto"/"16:9", "1920x1080", a level or cutscene id or a
 * lower_snake_case name), validated by the flag parser
 * that already owns those messages — re-implementing that here would put
 * the error text in two places. */
static const cfg_knob_t g_schema[] = {
    /* -------- Display -------- */
    { "fullscreen",       "--fullscreen",       NULL,                    CFG_BOOL,   NULL,       NULL,       NULL,       0 },
    { "aspect",           "--aspect",           NULL,                    CFG_STRING, NULL,       NULL,       NULL,       0 },
    { "ssaa",             "--ssaa",             NULL,                    CFG_INT,    NULL,       NULL,       NULL,       0 },
    { "vsync",            "--vsync",            "OPENPETE_PRESENT_MODE", CFG_BOOL,   NULL,       NULL,       NULL,       0 },
    { "perf_stats",       "--perf-stats",       NULL,                    CFG_ENUM,   v_perf,     NULL,       NULL,       0 },
    { "perf_position",    "--perf-position",    NULL,                    CFG_ENUM,   v_anchor,   NULL,       NULL,       0 },
    { "osd",              "--osd",              NULL,                    CFG_BOOL,   NULL,       NULL,       NULL,       0 },
    { "osd_position",     "--osd-position",     NULL,                    CFG_ENUM,   v_anchor,   NULL,       NULL,       0 },
    { "osd_timeout",      "--osd-timeout",      NULL,                    CFG_FLOAT,  NULL,       NULL,       NULL,       0 },
    { "vertex_precision", "--vertex-precision", NULL,                    CFG_ENUM,   v_vprec,    NULL,       NULL,       0 },
    { "raster_quantize",  "--raster-quantize",  NULL,                    CFG_INT,    NULL,       NULL,       NULL,       0 },
    { "extract_threads",  "--extract-threads",  NULL,                    CFG_INT,    NULL,       NULL,       NULL,       0 },
    { "terrain_cache",    "--terrain-cache",    "OPENPETE_TERRAIN_CACHE", CFG_BOOL,  NULL,       NULL,       NULL,       0 },
    { "notex",            NULL,                 "OPENPETE_NOTEX",        CFG_BOOL,   NULL,       NULL,       NULL,       0 },
    { "dither",           "--dither",           NULL,                    CFG_BOOL,   NULL,       NULL,       NULL,       0 },
    { "texture_mapping",  "--texture-mapping",  NULL,                    CFG_ENUM,   v_texmap,   NULL,       NULL,       0 },
    { "lod",              "--lod",              NULL,                    CFG_ENUM,   v_lod,      "--no-lod", "extended", 0 },
    { "fog_scale",        "--fog-scale",        NULL,                    CFG_FLOAT,  NULL,       NULL,       NULL,       0 },
    { "renderer",         "--renderer",         NULL,                    CFG_ENUM,   v_renderer, NULL,       NULL,       0 },
    { "window",           "--window",           NULL,                    CFG_STRING, NULL,       NULL,       NULL,       0 },
    { "fov",              "--fov",              NULL,                    CFG_FLOAT,  NULL,       NULL,       NULL,       0 },

    /* -------- Pacing -------- */
    { "render_fps",       "--render-fps",       NULL,                    CFG_INT,    NULL,       NULL,       NULL,       0 },
    { "tick_scale",       "--tick-scale",       NULL,                    CFG_FLOAT,  NULL,       NULL,       NULL,       0 },
    { "runahead",         "--runahead",         NULL,                    CFG_INT,    NULL,       NULL,       NULL,       0 },
    { "rewind",           "--rewind",           NULL,                    CFG_INT,    NULL,       NULL,       NULL,       0 },

    /* -------- Audio -------- */
    { "mute",             "--mute",             NULL,                    CFG_BOOL,   NULL,       NULL,       NULL,       0 },
    { "sfx",              "--sfx",              NULL,                    CFG_INT,    NULL,       NULL,       NULL,       0 },
    { "music",            "--music",            NULL,                    CFG_INT,    NULL,       NULL,       NULL,       0 },
    { "vibration",        "--vibration",        "OPENPETE_VIBRATION",    CFG_INT,    NULL,       NULL,       NULL,       0 },
    { "mouse_sensitivity","--mouse-sensitivity", NULL,                   CFG_FLOAT,  NULL,       NULL,       NULL,       0 },

    /* -------- Paths -------- */
    { "disc",             "--disc",             NULL,                    CFG_STRING, NULL,       NULL,       NULL,       0 },
    { "disc_cache",       "--disc-cache",       "OPENPETE_DISC_CACHE",   CFG_BOOL,   NULL,       NULL,       NULL,       0 },
    { "load_delay",       "--load-delay",       "OPENPETE_LOAD_DELAY",   CFG_INT,    NULL,       NULL,       NULL,       0 },
    { "texpack_dir",      "--texpack-dir",      "OPENPETE_TEXPACK_DIR",  CFG_STRING, NULL,       NULL,       NULL,       0 },
    { "sfxpack_dir",      "--sfxpack-dir",      "OPENPETE_SFXPACK_DIR",  CFG_STRING, NULL,       NULL,       NULL,       0 },
    { "controller_db",    "--controller-db",    "OPENPETE_CONTROLLER_DB", CFG_STRING, NULL,      NULL,       NULL,       0 },
    { "mods_dir",         "--mods-dir",         "OPENPETE_MODS_DIR",     CFG_STRING, NULL,       NULL,       NULL,       0 },
    { "mods",             "--mods",             "OPENPETE_MODS",         CFG_STRING, NULL,       NULL,       NULL,       0 },
    { "card_dir",         "--card-dir",         "OPENPETE_CARD_DIR",     CFG_STRING, NULL,       NULL,       NULL,       0 },

    /* -------- Gameplay entry -------- */
    { "skip_to_level",    "--skip-to-level",    NULL,                    CFG_STRING, NULL,       NULL,       NULL,       0 },
    { "skip_to_cutscene", "--skip-to-cutscene", NULL,                    CFG_STRING, NULL,       NULL,       NULL,       0 },
    { "cheats",           "--cheats",           NULL,                    CFG_STRING, NULL,       NULL,       NULL,       0 },
    { "freecam",          NULL,                 "OPENPETE_FREECAM",      CFG_BOOL,   NULL,       NULL,       NULL,       0 },
    { "camlock",          NULL,                 "OPENPETE_CAMLOCK",      CFG_BOOL,   NULL,       NULL,       NULL,       0 },
    { "bk2",              "--bk2",              NULL,                    CFG_STRING, NULL,       NULL,       NULL,       0 },

    /* -------- Recording -------- */
    { "record",           "--record",           NULL,                    CFG_STRING, NULL,       NULL,       NULL,       0 },
    { "record_cfr",       "--record-cfr",       NULL,                    CFG_FLOAT,  NULL,       NULL,       NULL,       0 },
    { "record_frames",    "--record-frames",    NULL,                    CFG_INT,    NULL,       NULL,       NULL,       0 },
    { "record_fps_overlay", "--record-fps-overlay", "OPENPETE_FPS_IN_RECORD", CFG_BOOL, NULL,    NULL,       NULL,       0 },

    /* -------- Debug -------- */
    { "render_paths",     "--render-paths",     NULL,                    CFG_STRING, NULL,       NULL,       NULL,       0 },
    { "log_level",        "--log-level",        "OPENPETE_LOG",          CFG_STRING, NULL,       NULL,       NULL,       0 },
    { "log_format",       "--log-format",       NULL,                    CFG_ENUM,   v_logfmt,   NULL,       NULL,       0 },
    { "log_file",         "--log-file",         "OPENPETE_LOG_FILE",     CFG_STRING, NULL,       NULL,       NULL,       0 },
    { "devui_open",       "--devui-open",       "OPENPETE_DEVUI_OPEN",   CFG_BOOL,   NULL,       NULL,       NULL,       0 },
    { "devui_inject_mouse", NULL,                   "OPENPETE_DEVUI_INJECT_MOUSE", CFG_STRING, NULL, NULL,   NULL,       0 },
    { "fillpick",         NULL,                 "OPENPETE_FILLPICK",     CFG_BOOL,   NULL,       NULL,       NULL,       0 },
    { "fillpick_preview", NULL,                 "OPENPETE_FILLPICK_PREVIEW", CFG_BOOL, NULL,     NULL,       NULL,       0 },
};

/* The screenshot pair is mutually exclusive in the parser (options.c
 * rejects both together), so a config-sourced one must stand down when
 * the other arrives from a higher tier instead of turning a valid command
 * line into a setup error. */
static const char *const g_mutex_screenshot[] = {
    "--screenshot-on-frame", "--screenshot-on-tick", NULL
};
/* --show-fps is the deprecated two-state spelling of --perf-stats. It has no
 * schema row of its own — its ON value is not fixed, which is what an alias
 * row requires — so a config file that spells the setting through [args] is
 * where the two can collide. Between tiers this group makes the higher one
 * win; a bare key and a command-line flag are already ordered by the tiers
 * themselves (config tokens are emitted ahead of the real argv). */
static const char *const g_mutex_perf_stats[] = {
    "--perf-stats", "--show-fps", NULL
};
static const char *const *const g_mutex_groups[] = {
    g_mutex_screenshot, g_mutex_perf_stats, NULL
};

const cfg_knob_t *cfg_schema(size_t *count) {
    if (count) *count = sizeof g_schema / sizeof g_schema[0];
    return g_schema;
}

int cfg_schema_find_id(const char *id) {
    if (!id) return -1;
    for (size_t i = 0; i < sizeof g_schema / sizeof g_schema[0]; i++)
        if (!strcmp(g_schema[i].id, id)) return (int)i;
    return -1;
}

int cfg_schema_find_cli(const char *flag) {
    if (!flag) return -1;
    for (size_t i = 0; i < sizeof g_schema / sizeof g_schema[0]; i++) {
        if (g_schema[i].cli && !strcmp(g_schema[i].cli, flag)) return (int)i;
        if (g_schema[i].alias && !strcmp(g_schema[i].alias, flag)) return (int)i;
    }
    return -1;
}

int cfg_schema_find_env(const char *var) {
    if (!var) return -1;
    for (size_t i = 0; i < sizeof g_schema / sizeof g_schema[0]; i++)
        if (g_schema[i].env && !strcmp(g_schema[i].env, var)) return (int)i;
    return -1;
}

const char *const *cfg_schema_mutex_group(const char *flag) {
    if (!flag) return NULL;
    for (int g = 0; g_mutex_groups[g]; g++)
        for (int i = 0; g_mutex_groups[g][i]; i++)
            if (!strcmp(g_mutex_groups[g][i], flag)) return g_mutex_groups[g];
    return NULL;
}
