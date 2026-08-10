/* hello-anchor — PoC rung 1: named-tier override + base(), log-only.
 *
 * Overrides CameraUpdate (the game's per-frame camera driver, decomp
 * name for func_80037BD4) as a pure observer: log every 300th call
 * (~10s at 29.913 sim ticks/s), then base() through to the original.
 * MUST NOT touch guest state — this mod enabled is the framework's
 * observer-neutrality gate.
 */

#include <openpete_mod_api.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static unsigned                  g_calls;

/* Runahead capability: the hook only counts calls and logs — a host side
 * effect the engine can suppress during speculative ticks. */
OPENPETE_MOD_RA_CAPS(OP_MOD_RA_HOST_FX)

static void on_camera_update(CPUState* cpu) {
    g_calls++;
    if (g_calls == 1u || (g_calls % 300u) == 0u)
        g_api->log(g_self, OP_MOD_LOG_INFO,
                   "hello from the override chain: CameraUpdate call #%u",
                   g_calls);
    g_api->base(cpu); /* pre-hook shape: our code ran first, original last */
}

/* Overlay section: read-only view of the observer's counter. Render-pure —
 * it only reads host-side state the hook publishes. */
static void ui_draw(const openpete_mod_ui_t* ui, void* user) {
    (void)user;
    ui->text("CameraUpdate calls: %u", g_calls);
    ui->text_disabled("%s", g_calls ? "observing (logs every 300th call)"
                                    : "waiting for the first call");
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;
    if (api->api_version != OPENPETE_MOD_API_VERSION) return -1;
    api->register_ui(self, ui_draw, NULL);
    return api->override_name(self, "CameraUpdate", on_camera_update);
}
