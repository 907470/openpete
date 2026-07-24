/* ps1-240p — quantize the scene to the PSX 512x240 raster (see mod.toml).
 *
 * config.toml [ps1240] knobs:
 *   point = "scene" (default) quantizes the 3D scene only, HUD stays
 *           crisp; "composite" quantizes the whole frame (HUD too).
 *   blend = 0.0  mixes the sharp frame back in (0 = fully quantized). */

#include <openpete_mod_api.h>
#include <string.h>

typedef struct {           /* std140 mirror of quantize240.frag q_params */
    float blend;
} q_params_t;

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    if (api->api_version != OPENPETE_MOD_API_VERSION) return -1;

    int frag = api->shader_register(self, "shaders/quantize240.frag");
    if (frag < 0) {
        api->log(self, OP_MOD_LOG_ERROR,
                 "quantize240.frag failed to compile (see .build/shaders/)");
        return 0;   /* stay loaded: a broken shader never bricks a boot */
    }

    char point[16];
    api->config_str(self, "ps1240.point", "scene", point, sizeof point);
    int at = (strcmp(point, "composite") == 0) ? OP_POSTFX_COMPOSITE
                                               : OP_POSTFX_SCENE;
    int fx = api->postfx_register(self, frag, at, 0);
    if (fx < 0) {
        api->log(self, OP_MOD_LOG_INFO, "postfx point unavailable");
        return 0;
    }

    q_params_t p;
    p.blend = (float)api->config_float(self, "ps1240.blend", 0.0);
    api->postfx_set_uniforms(self, fx, &p, sizeof p);

    api->log(self, OP_MOD_LOG_INFO, "240p quantize armed (%s point)", point);
    return api->postfx_enable(self, fx, 1);
}
