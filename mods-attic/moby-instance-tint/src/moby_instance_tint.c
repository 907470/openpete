#include <openpete_mod_api.h>

/* Tint curve the callback evaluates: channel = floor + spread * hash.
 * Host-side only — the widgets below retune it live, and the next
 * canonical extraction pass picks the new values up. */
static float g_floor  = 0.3f;
static float g_spread = 0.6f;

/* Instances tinted: accumulated by the callback, folded into the
 * per-pass figure when the overlay draws (extraction runs once per
 * canonical present, so a pass never straddles two overlay draws). */
static uint32_t g_pass_hits;
static uint32_t g_last_pass;
static uint32_t g_total;

/* Runs at extraction per matching moby instance (cpu available, guest
 * RAM readable). Returns a per-draw uniform block, or -1 to decline
 * (that instance keeps stock shading). Here: a stable per-vaddr hue —
 * a real mod would read gameplay state, e.g. moby FLAMED heat. */
static int per_instance(CPUState* cpu, uint32_t moby, void* out, uint32_t cap) {
    (void)cpu;
    if (cap < 16) return -1;
    uint32_t h = moby * 2654435761u;
    float* v = (float*)out;
    v[0] = ((float)( h        & 0xFF) / 255.0f) * g_spread + g_floor;
    v[1] = ((float)((h >> 8)  & 0xFF) / 255.0f) * g_spread + g_floor;
    v[2] = ((float)((h >> 16) & 0xFF) / 255.0f) * g_spread + g_floor;
    v[3] = 1.0f;
    g_pass_hits++;
    g_total++;
    return 16;
}

static void ui_draw(const openpete_mod_ui_t* ui, void* user) {
    (void)user;
    if (g_pass_hits) {
        g_last_pass = g_pass_hits;
        g_pass_hits = 0;
    }
    ui->text("instances tinted: %u this pass, %u total", g_last_pass, g_total);
    ui->tooltip("Each tinted instance carries its own uniform block, so "
                "it also costs the batcher one extra draw.");
    if (!g_total)
        ui->text_disabled("selector inert — no moby instance seen yet");
    ui->separator();
    ui->slider_float("floor", &g_floor, 0.0f, 1.0f);
    ui->tooltip("Darkest a tint channel can get — the hash rides on top.");
    ui->slider_float("spread", &g_spread, 0.0f, 1.0f);
    ui->tooltip("How far the per-vaddr hash pushes each channel above "
                "the floor.");
    if (g_floor + g_spread > 1.0f)
        ui->text_disabled("floor + spread > 1: the brightest hues clamp");
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    if (api->api_version != OPENPETE_MOD_API_VERSION) return -1;
    int fs = api->shader_register(self, "shaders/tint.frag");
    if (fs < 0) {
        api->log(self, OP_MOD_LOG_ERROR, "tint.frag failed — selector inert");
        return 0;
    }
    api->register_ui(self, ui_draw, NULL);
    openpete_mod_material_selector_t sel = { OP_RP_MOBY, -1, -1, 0 };
    return api->material_register(self, &sel, -1, fs, per_instance);
}
