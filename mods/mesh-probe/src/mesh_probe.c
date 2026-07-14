#include <openpete_mod_api.h>
static const openpete_mod_api_t* g_api;
static openpete_mod_t* g_self;
static int g_pipe = -1;
static void on_present(const openpete_present_ctx_t* ctx) {
    (void)ctx;
    if (g_pipe < 0) return;
    /* clip-space magenta triangle, upper-left quadrant */
    static const openpete_mod_vertex_t v[3] = {
        { -0.9f, -0.9f, 0.0f, 0, 0, 0xFFFF00FFu },
        { -0.2f, -0.9f, 0.0f, 0, 0, 0xFFFF00FFu },
        { -0.9f, -0.2f, 0.0f, 0, 0, 0xFFFF00FFu },
    };
    g_api->draw_mesh(g_self, g_pipe, v, 3, 0);
}
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    if (api->api_version != OPENPETE_MOD_API_VERSION) return -1;
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
