#include <openpete_mod_api.h>
static const openpete_mod_api_t* g_api;
static openpete_mod_t* g_self;
static int g_pipe = -1;
/* Triangle placement/tint, clip-space; the present hook rebuilds the
 * vertices from these so the overlay can move and recolour the probe. */
#define TRI_EDGE 0.7f
static float g_org_x = -0.9f, g_org_y = -0.9f;
static int g_color_idx;
static const uint32_t k_colors[4] = {
    0xFFFF00FFu, /* magenta */
    0xFFFFFF00u, /* cyan    */
    0xFF00FFFFu, /* yellow  */
    0xFFFFFFFFu, /* white   */
};
/* Published by the present hook for the overlay section. */
static uint32_t g_draws, g_last_tick;
static void on_present(const openpete_present_ctx_t* ctx) {
    if (g_pipe < 0) return;
    /* clip-space triangle, origin corner + two edges along +x/+y */
    const uint32_t c = k_colors[g_color_idx];
    const openpete_mod_vertex_t v[3] = {
        { g_org_x,            g_org_y,            0.0f, 0, 0, c },
        { g_org_x + TRI_EDGE, g_org_y,            0.0f, 0, 0, c },
        { g_org_x,            g_org_y + TRI_EDGE, 0.0f, 0, 0, c },
    };
    g_api->draw_mesh(g_self, g_pipe, v, 3, 0);
    g_draws++;
    g_last_tick = ctx->tick;
}
static void ui_draw(const openpete_mod_ui_t* ui, void* user) {
    (void)user;
    if (g_pipe < 0) {
        ui->text_disabled("pipeline unavailable — no draw");
        return;
    }
    ui->text("pipeline %d — %u draws, last tick %u",
             g_pipe, g_draws, g_last_tick);
    ui->separator();
    static const char* const k_color_names[4] = {
        "Magenta", "Cyan", "Yellow", "White",
    };
    ui->slider_float("Origin X", &g_org_x, -1.0f, 1.0f - TRI_EDGE);
    ui->slider_float("Origin Y", &g_org_y, -1.0f, 1.0f - TRI_EDGE);
    ui->combo("Colour", &g_color_idx, k_color_names, 4);
    ui->text_disabled("clip space: y = -1 is the top edge");
}
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    if (api->api_version != OPENPETE_MOD_API_VERSION) return -1;
    api->register_ui(self, ui_draw, NULL);
    int vs = api->shader_register(self, "shaders/tri.vert");
    int fs = api->shader_register(self, "shaders/tri.frag");
    if (vs < 0 || fs < 0) return 0;
    openpete_mod_pipeline_desc_t d = {0};
    d.vert_shader = vs; d.frag_shader = fs;
    d.vertex_format = OP_VTX_POS_UV_RGBA;
    d.blend = OP_BLEND_OPAQUE; d.depth_test = 0; d.depth_write = 0;
    g_pipe = api->pipeline_register(self, &d);
    api->log(self, OP_MOD_LOG_INFO, "mesh probe pipe=%d", g_pipe);
    return api->register_present_hook(self, on_present);
}
