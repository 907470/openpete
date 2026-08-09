/* crt-filter — mod-shader demo: engine-compiled GLSL postfx with
 * config-driven parameters through the mod uniform block.
 *
 * config.toml [crt] knobs -> postfx_set_uniforms (std140: four floats,
 * matching crt.frag's crt_params block). Pushed at entry and again
 * whenever the overlay sliders move — config is per-boot (dev hot
 * reload re-runs entry with fresh values) and stays the file-side
 * source of truth; slider edits live only for this run. */

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
static crt_params_t g_config;      /* values read from config.toml  */
static int          g_fx = -1;     /* postfx handle, -1 = no pass   */
static int          g_shader_ok;   /* crt.frag compiled             */

/* Uniform pushes are a host-side copy, so the overlay can drive them
 * directly: no guest state is involved and nothing is deferred. */
static void ui_draw(const openpete_mod_ui_t* ui, void* user) {
    (void)user;
    if (!g_shader_ok) {
        ui->text_disabled("crt.frag did not compile — see .build/shaders/");
        return;
    }
    if (g_fx < 0) {
        ui->text_disabled("postfx chain unavailable in this engine build");
        return;
    }

    int changed = 0;
    changed |= ui->slider_float("Curvature", &g_params.curvature, 0.0f, 0.5f);
    ui->tooltip("Barrel amount; 0 = flat");
    changed |= ui->slider_float("Scanlines", &g_params.scan_depth, 0.0f, 1.0f);
    ui->tooltip("Scanline darkening; 0 = off");
    changed |= ui->slider_float("Slot mask", &g_params.mask_depth, 0.0f, 1.0f);
    ui->tooltip("RGB slot-mask darkening; 0 = off");
    changed |= ui->slider_float("Corner fade", &g_params.corner_fade,
                                0.0f, 0.25f);
    ui->tooltip("Vignette width at the curved edge");

    if (ui->button("Reset to config.toml")) {
        g_params = g_config;
        changed = 1;
    }
    if (changed)
        g_api->postfx_set_uniforms(g_self, g_fx, &g_params, sizeof g_params);

    ui->separator();
    ui->text("composite pass active (curve %.2f scan %.2f mask %.2f)",
             g_params.curvature, g_params.scan_depth, g_params.mask_depth);
    ui->text_disabled("edits apply live; config.toml is reloaded at boot");
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;
    if (api->api_version != OPENPETE_MOD_API_VERSION) return -1;
    api->register_ui(self, ui_draw, NULL);

    int frag = api->shader_register(self, "shaders/crt.frag");
    if (frag < 0) {
        api->log(self, OP_MOD_LOG_ERROR,
                 "crt.frag failed to compile — filter unavailable "
                 "(see .build/shaders/)");
        return 0;   /* stay loaded: a broken shader never bricks a boot */
    }
    g_shader_ok = 1;

    int fx = api->postfx_register(self, frag, OP_POSTFX_COMPOSITE, 0);
    if (fx < 0) {
        api->log(self, OP_MOD_LOG_INFO,
                 "postfx chain not in this engine build — shader "
                 "compiled and ready");
        return 0;
    }
    g_fx = fx;

    crt_params_t p;
    p.curvature   = (float)api->config_float(self, "crt.curvature",      0.12);
    p.scan_depth  = (float)api->config_float(self, "crt.scanline_depth", 0.28);
    p.mask_depth  = (float)api->config_float(self, "crt.mask_depth",     0.18);
    p.corner_fade = (float)api->config_float(self, "crt.corner_fade",    0.02);
    g_config = p;
    g_params = p;
    api->postfx_set_uniforms(self, fx, &p, sizeof p);

    api->log(self, OP_MOD_LOG_INFO,
             "crt filter armed (curve %.2f scan %.2f mask %.2f)",
             p.curvature, p.scan_depth, p.mask_depth);
    return api->postfx_enable(self, fx, 1);
}
