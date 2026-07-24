/* crt-beans — CRT display simulation, a port of Andrew Duffey's crt-beans
 * "fast" 3-pass chain (MIT — see LICENSE):
 *
 *   1. beans_virtualize — resample the composed frame onto the virtual
 *      512x224 PSX raster + linearize (CRT gamma 2.4)
 *   2. beans_scan_h     — horizontal beam-spot convolution per scanline
 *   3. beans_scan_v     — vertical spot profile + phosphor mask + output
 *
 * config.toml [crt] knobs -> postfx_set_uniforms (std140: seven floats,
 * matching the beans_params block in all three shaders; passes 1-2 read
 * only the spot sizes but bind the same block). Pushed once at entry —
 * config is per-boot (dev hot reload re-runs entry with fresh values). */

#include <openpete_mod_api.h>

typedef struct {           /* std140 mirror of the shaders' beans_params */
    float max_spot_size;
    float min_spot_size;
    float mask_type;
    float subpixel_pattern;
    float subpixel_triad_px;
    float mask_triads;
    float output_gamma;
} beans_params_t;

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    if (api->api_version != OPENPETE_MOD_API_VERSION) return -1;

    static const char* const k_frag[3] = {
        "shaders/beans_virtualize.frag",
        "shaders/beans_scan_h.frag",
        "shaders/beans_scan_v.frag",
    };

    beans_params_t p;
    p.max_spot_size     = (float)api->config_float(self, "crt.max_spot_size",     0.90);
    p.min_spot_size     = (float)api->config_float(self, "crt.min_spot_size",     0.40);
    p.mask_type         = (float)api->config_float(self, "crt.mask_type",         2.0);
    p.subpixel_pattern  = (float)api->config_float(self, "crt.subpixel_pattern",  0.0);
    p.subpixel_triad_px = (float)api->config_float(self, "crt.subpixel_triad_px", 4.0);
    p.mask_triads       = (float)api->config_float(self, "crt.mask_triads",       550.0);
    p.output_gamma      = (float)api->config_float(self, "crt.output_gamma",      1.0);

    int enabled_all = 1;
    for (int i = 0; i < 3; i++) {
        int frag = api->shader_register(self, k_frag[i]);
        if (frag < 0) {
            api->log(self, OP_MOD_LOG_ERROR,
                     "%s failed to compile — CRT unavailable "
                     "(see .build/shaders/)", k_frag[i]);
            return 0;   /* stay loaded: a broken shader never bricks a boot */
        }
        int fx = api->postfx_register(self, frag, OP_POSTFX_COMPOSITE, i);
        if (fx < 0) {
            api->log(self, OP_MOD_LOG_INFO,
                     "postfx chain not in this engine build — shaders "
                     "compiled and ready");
            return 0;
        }
        api->postfx_set_uniforms(self, fx, &p, sizeof p);
        if (api->postfx_enable(self, fx, 1) != 0) enabled_all = 0;
    }

    api->log(self, OP_MOD_LOG_INFO,
             "crt-beans armed (spot %.2f/%.2f mask %d triads %d)",
             p.max_spot_size, p.min_spot_size,
             (int)p.mask_type, (int)p.mask_triads);
    return enabled_all ? 0 : -1;
}
