/* moby-instance-tint — the refine callback, made visible.
 *
 * The selector picks the audience (every moby). The callback runs once per
 * candidate instance during extraction and returns that instance's uniform
 * block, so each moby renders a distinct stable colour. That is the whole
 * middle layer of the seam in one file.
 *
 * A real mod keys the callback on GAMEPLAY state — "class 42, but only
 * while it is burning" — and scopes the selector as tightly as it can
 * first. This one hashes the guest address instead, because a hash makes
 * the per-instance-ness impossible to miss: if every moby came out the
 * same colour, the callback would not be running per instance.
 */
#include <openpete_mod_api.h>
#include <openpete_mod_ui.h>

/* Tint curve the callback evaluates: channel = floor + spread * hash.
 * Host-side only — the widgets below retune it live, and the next
 * canonical extraction pass picks the new values up. */
static float g_floor  = 0.3f;
static float g_spread = 0.6f;

/* Instances tinted. See the warning in the overlay text: this counts
 * CALLBACK INVOCATIONS, which is not the same thing as instances drawn. */
static uint32_t g_pass_hits;
static uint32_t g_last_pass;
static uint32_t g_total;

/* Runs at extraction, in tick context, per matching moby instance.
 * Returns that instance's uniform block, or -1 to decline (the instance
 * keeps stock shading). */
static int refine(const openpete_mod_material_key_t* key, void* out, uint32_t cap)
{
    if (cap < 16 || key->channel != OP_CHAN_MOBY) return -1;
    uint32_t h = key->u.moby.vaddr * 2654435761u;
    float* v = (float*)out;
    v[0] = ((float)( h        & 0xFF) / 255.0f) * g_spread + g_floor;
    v[1] = ((float)((h >> 8)  & 0xFF) / 255.0f) * g_spread + g_floor;
    v[2] = ((float)((h >> 16) & 0xFF) / 255.0f) * g_spread + g_floor;
    v[3] = 1.0f;
    g_pass_hits++;
    g_total++;
    return 16;
}

static void ui_draw(const openpete_mod_ui_t* ui) {
    if (g_pass_hits) {
        g_last_pass = g_pass_hits;
        g_pass_hits = 0;
    }
    ui->text("callback invocations: %u this pass, %u total", g_last_pass, g_total);
    /* The engine promises the VERDICT, never the CADENCE: results are
     * interned, and a channel that later banks geometry across presents
     * re-resolves from identity instead of calling back. So this number
     * measures the engine's memoization, not how many mobys are on
     * screen — and it will change meaning under the engine, silently, if
     * you read it as a scene statistic. */
    ui->tooltip("Invocations, NOT instances drawn: the engine promises the "
                "verdict your callback gave, not how often it asks for it. "
                "Do not read this as a scene count.");
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
OPENPETE_MOD_UI_SECTION(ui_draw)

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    if (api->api_version < 4u) return -1;   /* material_register_refine */
    int fs = api->shader_register(self, "shaders/tint.frag");
    if (fs < 0) {
        api->log(self, OP_MOD_LOG_ERROR, "tint.frag failed — selector inert");
        return 0;
    }
    openpete_mod_material_selector_t sel = openpete_mod_material_sel_any();
    sel.channel = OP_CHAN_MOBY;
    if (api->material_register_refine(self, &sel, fs, refine) < 0)
        api->log(self, OP_MOD_LOG_ERROR, "material refused — selector inert");
    return 0;
}
