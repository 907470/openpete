/* crt-filter - mod-shader demo: engine-compiled GLSL postfx with
 * config-driven parameters through the mod uniform block.
 *
 * config.toml [crt] knobs -> postfx_set_uniforms (std140: four floats,
 * matching crt.frag's crt_params block). Read once at entry: the
 * [[config]] rows in mod.toml render engine-side in the Mods panel,
 * and a committed edit writes config.toml back and reloads the mod,
 * so entry re-runs with the fresh values - config.toml stays the
 * file-side source of truth. */

#include <openpete_mod_api.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;

typedef struct {           /* std140 mirror of crt.frag crt_params */
    float curvature;
    float scan_depth;
    float mask_depth;
    float corner_fade;
} crt_params_t;

static crt_params_t g_params;      /* what the shader is running on */
static int          g_fx = -1;     /* postfx handle, -1 = no pass   */
static int          g_shader_ok;   /* crt.frag compiled             */

/* Repaint the whole status block (ui_status lines persist until the
 * next repaint). Entry is the only writer of the shown state - the
 * shader/postfx outcome and the pushed params are fixed for the life
 * of the load - so it publishes once, after that state is settled. */
static void crt_status_publish(void) {
    if (!g_shader_ok) {
        g_api->ui_status(g_self,
                         "crt.frag did not compile - see .build/shaders/");
        return;
    }
    if (g_fx < 0) {
        g_api->ui_status(g_self,
                         "postfx chain unavailable in this engine build");
        return;
    }
    g_api->ui_status(g_self,
                     "composite pass active (curve %.2f scan %.2f mask %.2f)",
                     g_params.curvature, g_params.scan_depth,
                     g_params.mask_depth);
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    /* No version check: mod.toml's `api` pin is the minimum this mod
     * needs, and the loader refuses an older engine before entry. */

    int frag = api->shader_register(self, "shaders/crt.frag");
    if (frag < 0) {
        api->log(self, OP_MOD_LOG_ERROR,
                 "crt.frag failed to compile - filter unavailable "
                 "(see .build/shaders/)");
        crt_status_publish();
        return 0;   /* stay loaded: a broken shader never bricks a boot */
    }
    g_shader_ok = 1;

    int fx = api->postfx_register(self, frag, OP_POSTFX_COMPOSITE, 0);
    if (fx < 0) {
        api->log(self, OP_MOD_LOG_INFO,
                 "postfx chain not in this engine build - shader "
                 "compiled and ready");
        crt_status_publish();
        return 0;
    }
    g_fx = fx;

    crt_params_t p;
    p.curvature   = (float)api->config_float(self, "crt.curvature",      0.12);
    p.scan_depth  = (float)api->config_float(self, "crt.scanline_depth", 0.28);
    p.mask_depth  = (float)api->config_float(self, "crt.mask_depth",     0.18);
    p.corner_fade = (float)api->config_float(self, "crt.corner_fade",    0.02);
    g_params = p;
    api->postfx_set_uniforms(self, fx, &p, sizeof p);

    api->log(self, OP_MOD_LOG_INFO,
             "crt filter armed (curve %.2f scan %.2f mask %.2f)",
             p.curvature, p.scan_depth, p.mask_depth);
    crt_status_publish();
    return api->postfx_enable(self, fx, 1);
}
