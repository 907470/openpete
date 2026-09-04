/**
 * @file crt_beans.c
 * @brief crt-beans: CRT display simulation as a three-pass post-process
 *        chain. A port of Andrew Duffey's crt-beans, "fast" variant (MIT,
 *        see LICENSE).
 *
 * The passes, in chain order:
 *
 * 1. beans_virtualize: resample the composed frame onto the virtual
 *    512x224 PSX raster and linearise it (CRT gamma 2.4).
 * 2. beans_scan_h: convolve each scanline with the electron-beam spot
 *    profile along the horizontal axis.
 * 3. beans_scan_v: apply the vertical spot profile, the phosphor mask and
 *    the output gamma encode.
 *
 * This file only registers the passes and pushes the parameters. All of
 * the image work is in the shaders; a post-process mod's C side is
 * configuration, not rendering.
 *
 * The [crt] knobs in config.toml reach the shaders through one uniform
 * block, pushed with postfx_set_uniforms. The block is std140 and every
 * pass binds the same seven floats, even the passes that read only the
 * spot sizes: one layout shared by three shaders is one place for the
 * C struct to mirror, and a mismatch between the struct and a shader's
 * declaration is the failure mode this avoids.
 *
 * The parameters are pushed once, at entry. The knobs are [[config]]
 * rows in mod.toml, and a committed edit in the Mods panel reloads the
 * mod, so entry runs again with the new values. There is no per-frame
 * update path and none is needed.
 */

#include <openpete_mod_api.h>

/**
 * @brief Host mirror of the shaders' beans_params uniform block.
 *
 * Every member is a float, including the integer-valued knobs, because
 * std140 packs consecutive floats without padding and the shader
 * declares the block the same way. Mixing int and float members would
 * still pack, but the type of each member would then have to agree
 * between this struct and three shader files.
 */
typedef struct {
    float max_spot_size;
    float min_spot_size;
    float mask_type;
    float subpixel_pattern;
    float subpixel_triad_px;
    float mask_triads;
    float output_gamma;
} beans_params_t;

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;

static beans_params_t g_params;
static int            g_fx_count;   /**< Passes registered; 0 = chain unavailable. */

/**
 * @brief Repaint the Mods-panel status block.
 *
 * The chain state changes only during entry, so entry's exit paths are
 * the only callers.
 */
static void beans_status(void) {
    if (g_fx_count == 0) {
        g_api->ui_status(g_self, "CRT chain unavailable - see .build/shaders/");
        return;
    }
    if (g_fx_count == 3)
        g_api->ui_status(g_self, "3 passes active: virtualize -> scan H -> scan V");
    else
        g_api->ui_status(g_self, "%d of 3 passes registered - chain incomplete",
                         g_fx_count);
}

/**
 * @brief Mod entry point: read the knobs, compile the three shaders,
 *        register them as one post-process chain.
 * @return 0 when every pass is registered and enabled, or when the chain
 *         is unavailable; -1 only when a registered pass refused to
 *         enable.
 *
 * A shader that fails to compile does not fail the mod. The mod stays
 * loaded with a status line pointing at the compile log, because a mod
 * that returns non-zero from entry is unloaded and its status lines
 * vanish with it, leaving the player with nothing to read. The same
 * applies when the engine build has no post-process chain: the shaders
 * compiled, the mod is loaded, and the status says so.
 *
 * The chain point is OP_POSTFX_COMPOSITE, after the HUD is drawn, so the
 * CRT treatment covers the whole picture. The third argument is the
 * pass order within the chain point.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    g_fx_count = 0;

    static const char* const k_frag[3] = {
        "shaders/beans_virtualize.frag",
        "shaders/beans_scan_h.frag",
        "shaders/beans_scan_v.frag",
    };

    beans_params_t* p = &g_params;
    p->max_spot_size     = (float)api->config_float(self, "crt.max_spot_size",     0.90);
    p->min_spot_size     = (float)api->config_float(self, "crt.min_spot_size",     0.40);
    p->mask_type         = (float)api->config_float(self, "crt.mask_type",         2.0);
    p->subpixel_pattern  = (float)api->config_float(self, "crt.subpixel_pattern",  0.0);
    p->subpixel_triad_px = (float)api->config_float(self, "crt.subpixel_triad_px", 4.0);
    p->mask_triads       = (float)api->config_float(self, "crt.mask_triads",       550.0);
    p->output_gamma      = (float)api->config_float(self, "crt.output_gamma",      1.0);

    int enabled_all = 1;
    for (int i = 0; i < 3; i++) {
        int frag = api->shader_register(self, k_frag[i]);
        if (frag < 0) {
            api->log(self, OP_MOD_LOG_ERROR,
                     "%s failed to compile; CRT unavailable "
                     "(see .build/shaders/)", k_frag[i]);
            beans_status();
            return 0;
        }
        int fx = api->postfx_register(self, frag, OP_POSTFX_COMPOSITE, i);
        if (fx < 0) {
            api->log(self, OP_MOD_LOG_INFO,
                     "postfx chain not in this engine build; shaders "
                     "compiled and ready");
            beans_status();
            return 0;
        }
        g_fx_count++;
        api->postfx_set_uniforms(self, fx, p, sizeof *p);
        if (api->postfx_enable(self, fx, 1) != 0) enabled_all = 0;
    }

    api->log(self, OP_MOD_LOG_INFO,
             "crt-beans armed (spot %.2f/%.2f mask %d triads %d)",
             p->max_spot_size, p->min_spot_size,
             (int)p->mask_type, (int)p->mask_triads);
    beans_status();
    return enabled_all ? 0 : -1;
}
