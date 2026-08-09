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
 * only the spot sizes but bind the same block). Pushed at entry and
 * again whenever the overlay's sliders move — config is per-boot (dev
 * hot reload re-runs entry with fresh values). */

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

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;

static beans_params_t g_params;
static int            g_fx[3];        /* registered pass handles */
static int            g_fx_count;     /* 0 = chain unavailable */

/* Widget-side mirrors of the params the shaders take as enums/counts;
 * the uniform block carries them as floats. */
static int g_mask_type;
static int g_subpixel_pattern;
static int g_subpixel_triad_px;
static int g_mask_triads;
static int g_output_gamma;

/* Every pass binds the same block, so all three take the same push. */
static void beans_push(void) {
    for (int i = 0; i < g_fx_count; i++)
        g_api->postfx_set_uniforms(g_self, g_fx[i], &g_params, sizeof g_params);
}

static void ui_draw(const openpete_mod_ui_t* ui, void* user) {
    (void)user;
    if (g_fx_count == 0) {
        ui->text_disabled("CRT chain unavailable — see .build/shaders/");
        return;
    }

    int dirty = 0;
    dirty |= ui->slider_float("Max spot size", &g_params.max_spot_size,
                              0.6f, 1.0f);
    ui->tooltip("Electron-beam spot size at full white, as a proportion "
                "of one scanline. Lower = deeper scanline gaps.");
    dirty |= ui->slider_float("Min spot size", &g_params.min_spot_size,
                              0.3f, 1.0f);
    ui->tooltip("Spot size floor at black, as a proportion of the max. "
                "Raising it flattens the intensity-to-width response.");

    static const char* const k_mask[3] = {
        "off", "subpixel grille", "dynamic grille"
    };
    if (ui->combo("Phosphor mask", &g_mask_type, k_mask, 3)) {
        g_params.mask_type = (float)g_mask_type;
        dirty = 1;
    }

    if (g_mask_type == 1) {
        static const char* const k_subpixel[2] = { "RGB", "BGR" };
        if (ui->combo("Subpixel order", &g_subpixel_pattern, k_subpixel, 2)) {
            g_params.subpixel_pattern = (float)g_subpixel_pattern;
            dirty = 1;
        }
        ui->tooltip("Your monitor's physical subpixel layout — the mask "
                    "lights individual subpixels, so a mismatch tints it.");
        if (ui->slider_int("Triad width (px)", &g_subpixel_triad_px, 2, 5)) {
            g_params.subpixel_triad_px = (float)g_subpixel_triad_px;
            dirty = 1;
        }
        ui->tooltip("Monitor pixels per phosphor triad.");
    } else if (g_mask_type == 2) {
        if (ui->slider_int("Triads across screen", &g_mask_triads, 400, 800)) {
            g_params.mask_triads = (float)g_mask_triads;
            dirty = 1;
        }
        ui->tooltip("Phosphor triads across the screen width; resampled "
                    "to the output resolution, so it is subpixel-agnostic.");
    }

    static const char* const k_gamma[2] = { "sRGB", "2.2 gamma" };
    if (ui->combo("Output encode", &g_output_gamma, k_gamma, 2)) {
        g_params.output_gamma = (float)g_output_gamma;
        dirty = 1;
    }

    if (dirty) beans_push();

    ui->separator();
    if (g_fx_count == 3)
        ui->text("3 passes active: virtualize -> scan H -> scan V");
    else
        ui->text_disabled("%d of 3 passes registered — chain incomplete",
                          g_fx_count);
    ui->text_disabled("Edits apply immediately and last for this run; "
                      "config.toml holds the boot values.");
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    if (api->api_version != OPENPETE_MOD_API_VERSION) return -1;

    g_api  = api;
    g_self = self;
    g_fx_count = 0;
    api->register_ui(self, ui_draw, NULL);

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

    g_mask_type         = (int)p->mask_type;
    g_subpixel_pattern  = (int)p->subpixel_pattern;
    g_subpixel_triad_px = (int)p->subpixel_triad_px;
    g_mask_triads       = (int)p->mask_triads;
    g_output_gamma      = (int)p->output_gamma;

    /* A combo has no selection for an index outside its item list, so the
     * enum mirrors fall back to the documented default; the value pushed
     * to the shaders stays the config's until a widget moves. */
    if (g_mask_type < 0 || g_mask_type > 2)                  g_mask_type = 2;
    if (g_subpixel_pattern < 0 || g_subpixel_pattern > 1)    g_subpixel_pattern = 0;
    if (g_output_gamma < 0 || g_output_gamma > 1)            g_output_gamma = 1;

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
        g_fx[g_fx_count++] = fx;
        api->postfx_set_uniforms(self, fx, p, sizeof *p);
        if (api->postfx_enable(self, fx, 1) != 0) enabled_all = 0;
    }

    api->log(self, OP_MOD_LOG_INFO,
             "crt-beans armed (spot %.2f/%.2f mask %d triads %d)",
             p->max_spot_size, p->min_spot_size,
             (int)p->mask_type, (int)p->mask_triads);
    return enabled_all ? 0 : -1;
}
