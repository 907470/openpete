/* core/runtime/config_schema.c — see config_schema.h for what this is for. */

#include "config_schema.h"

#include <string.h>

static const char *const v_vprec[]    = { "subpixel", "integer", NULL };
static const char *const v_texmap[]   = { "perspective", "affine", NULL };
static const char *const v_lod[]      = { "original", "extended", NULL };
static const char *const v_renderer[] = { "psycross", "vk", "ab", "diff", NULL };
static const char *const v_logfmt[]   = { "plain", "json", NULL };

/* Ordered to mirror knob_registry.c's sections so the two read side by
 * side. `aspect`, `window` and `skip_to_level` are CFG_STRING because
 * their CLI vocabulary is a small language ("auto"/"16:9", "1920x1080",
 * a level id or a lower_snake_case name), validated by the flag parser
 * that already owns those messages — re-implementing that here would put
 * the error text in two places. */
static const cfg_knob_t g_schema[] = {
    /* -------- Display -------- */
    { "fullscreen",       "--fullscreen",       NULL,                    CFG_BOOL,   NULL,       NULL,       NULL,       0 },
    { "aspect",           "--aspect",           NULL,                    CFG_STRING, NULL,       NULL,       NULL,       0 },
    { "ssaa",             "--ssaa",             NULL,                    CFG_INT,    NULL,       NULL,       NULL,       0 },
    { "vsync",            "--vsync",            "OPENPETE_PRESENT_MODE", CFG_BOOL,   NULL,       NULL,       NULL,       0 },
    { "show_fps",         "--show-fps",         NULL,                    CFG_BOOL,   NULL,       NULL,       NULL,       0 },
    { "vertex_precision", "--vertex-precision", NULL,                    CFG_ENUM,   v_vprec,    NULL,       NULL,       0 },
    { "raster_quantize",  "--raster-quantize",  NULL,                    CFG_INT,    NULL,       NULL,       NULL,       0 },
    { "notex",            NULL,                 "OPENPETE_NOTEX",        CFG_BOOL,   NULL,       NULL,       NULL,       0 },
    { "dither",           "--dither",           NULL,                    CFG_BOOL,   NULL,       NULL,       NULL,       0 },
    { "texture_mapping",  "--texture-mapping",  NULL,                    CFG_ENUM,   v_texmap,   NULL,       NULL,       0 },
    { "lod",              "--lod",              NULL,                    CFG_ENUM,   v_lod,      "--no-lod", "extended", 0 },
    { "fog_scale",        "--fog-scale",        NULL,                    CFG_FLOAT,  NULL,       NULL,       NULL,       0 },
    { "renderer",         "--renderer",         NULL,                    CFG_ENUM,   v_renderer, NULL,       NULL,       0 },
    { "window",           "--window",           NULL,                    CFG_STRING, NULL,       NULL,       NULL,       0 },

    /* -------- Pacing -------- */
    { "render_fps",       "--render-fps",       NULL,                    CFG_INT,    NULL,       NULL,       NULL,       0 },
    { "tick_scale",       "--tick-scale",       NULL,                    CFG_FLOAT,  NULL,       NULL,       NULL,       0 },
    { "runahead",         "--runahead",         NULL,                    CFG_INT,    NULL,       NULL,       NULL,       0 },
    { "rewind",           "--rewind",           NULL,                    CFG_INT,    NULL,       NULL,       NULL,       0 },
    { "fov",              "--fov",              NULL,                    CFG_FLOAT,  NULL,       NULL,       NULL,       0 },

    /* -------- Audio -------- */
    { "mute",             "--mute",             NULL,                    CFG_BOOL,   NULL,       NULL,       NULL,       0 },

    /* -------- Paths -------- */
    { "disc",             "--disc",             NULL,                    CFG_STRING, NULL,       NULL,       NULL,       0 },
    { "texpack_dir",      "--texpack-dir",      "OPENPETE_TEXPACK_DIR",  CFG_STRING, NULL,       NULL,       NULL,       0 },
    { "mods_dir",         "--mods-dir",         "OPENPETE_MODS_DIR",     CFG_STRING, NULL,       NULL,       NULL,       0 },
    { "mods",             "--mods",             "OPENPETE_MODS",         CFG_STRING, NULL,       NULL,       NULL,       0 },
    { "card_dir",         "--card-dir",         "OPENPETE_CARD_DIR",     CFG_STRING, NULL,       NULL,       NULL,       0 },

    /* -------- Gameplay entry -------- */
    { "skip_to_level",    "--skip-to-level",    NULL,                    CFG_STRING, NULL,       NULL,       NULL,       0 },
    { "cheats",           "--cheats",           NULL,                    CFG_STRING, NULL,       NULL,       NULL,       0 },
    { "freecam",          NULL,                 "OPENPETE_FREECAM",      CFG_BOOL,   NULL,       NULL,       NULL,       0 },
    { "camlock",          NULL,                 "OPENPETE_CAMLOCK",      CFG_BOOL,   NULL,       NULL,       NULL,       0 },

    /* -------- Recording -------- */
    { "record",           "--record",           NULL,                    CFG_STRING, NULL,       NULL,       NULL,       0 },
    { "bk2",              "--bk2",              NULL,                    CFG_STRING, NULL,       NULL,       NULL,       0 },
    { "record_cfr",       "--record-cfr",       NULL,                    CFG_FLOAT,  NULL,       NULL,       NULL,       0 },
    { "record_frames",    "--record-frames",    NULL,                    CFG_INT,    NULL,       NULL,       NULL,       0 },
    { "record_fps_overlay", "--record-fps-overlay", "OPENPETE_FPS_IN_RECORD", CFG_BOOL, NULL,    NULL,       NULL,       0 },

    /* -------- Debug -------- */
    { "render_paths",     "--render-paths",     NULL,                    CFG_STRING, NULL,       NULL,       NULL,       0 },
    { "render_stats",     "--render-stats",     NULL,                    CFG_BOOL,   NULL,       NULL,       NULL,       1 },
    { "log_level",        "--log-level",        "OPENPETE_LOG",          CFG_STRING, NULL,       NULL,       NULL,       0 },
    { "log_format",       "--log-format",       NULL,                    CFG_ENUM,   v_logfmt,   NULL,       NULL,       0 },
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
static const char *const *const g_mutex_groups[] = { g_mutex_screenshot, NULL };

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
