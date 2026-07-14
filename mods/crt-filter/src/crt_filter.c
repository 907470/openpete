/* crt-filter — mod-shader demo: engine-compiled GLSL postfx with
 * config-driven parameters through the mod uniform block.
 *
 * config.toml [crt] knobs -> postfx_set_uniforms (std140: four floats,
 * matching crt.frag's crt_params block). Pushed once at entry — config
 * is per-boot (dev hot reload re-runs entry with fresh values). */

#include <openpete_mod_api.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;

typedef struct {           /* std140 mirror of crt.frag crt_params */
    float curvature;
    float scan_depth;
    float mask_depth;
    float corner_fade;
} crt_params_t;

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;
    if (api->api_version != OPENPETE_MOD_API_VERSION) return -1;

    int frag = api->shader_register(self, "shaders/crt.frag");
    if (frag < 0) {
        api->log(self, OP_MOD_LOG_ERROR,
                 "crt.frag failed to compile — filter unavailable "
                 "(see .build/shaders/)");
        return 0;   /* stay loaded: a broken shader never bricks a boot */
    }

    int fx = api->postfx_register(self, frag, OP_POSTFX_COMPOSITE, 0);
    if (fx < 0) {
        api->log(self, OP_MOD_LOG_INFO,
                 "postfx chain not in this engine build — shader "
                 "compiled and ready");
        return 0;
    }

    crt_params_t p;
    p.curvature   = (float)api->config_float(self, "crt.curvature",      0.12);
    p.scan_depth  = (float)api->config_float(self, "crt.scanline_depth", 0.28);
    p.mask_depth  = (float)api->config_float(self, "crt.mask_depth",     0.18);
    p.corner_fade = (float)api->config_float(self, "crt.corner_fade",    0.02);
    api->postfx_set_uniforms(self, fx, &p, sizeof p);

    api->log(self, OP_MOD_LOG_INFO,
             "crt filter armed (curve %.2f scan %.2f mask %.2f)",
             p.curvature, p.scan_depth, p.mask_depth);
    return api->postfx_enable(self, fx, 1);
}
