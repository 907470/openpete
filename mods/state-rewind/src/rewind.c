/* state-rewind — engine-services proof: savestate trigger +
 * participation callbacks. See mod.toml. */

#include <openpete_mod_api.h>
#include <openpete_sdk_symbols.h>

#include <string.h>

enum { STATE_SLOT = 9 };

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;

static unsigned g_ticks;        /* participates in savestates */
static uint32_t g_gems_at_save; /* participates in savestates */
static int      g_loads;        /* host-side: NOT in the blob, so the
                                 * rewound timeline doesn't re-queue the
                                 * load forever */
static int      g_verify = -1;  /* post-rewind gem check: -1 not run yet */

/* Set by the UI section's buttons, consumed by the tick hook below:
 * savestate_request is refused in the render-pure section, so the
 * widget only publishes the wish. 0 = none, 1 = save, 2 = load. */
static int g_ui_request;
/* Once the user drives the slot by hand the scripted 300/600 sequence
 * stands down, so an automatic save can't clobber a manual one. */
static int g_manual;
static int g_last_rc = 1;       /* rc of the last manual request; 1 = none */

static int on_state_save(void* buf, int cap) {
    if (cap < 8) return -1;
    memcpy(buf, &g_ticks, 4);
    memcpy((char*)buf + 4, &g_gems_at_save, 4);
    return 8;
}

static void on_state_load(const void* buf, int len) {
    if (len >= 8) {
        memcpy(&g_ticks, buf, 4);
        memcpy(&g_gems_at_save, (const char*)buf + 4, 4);
    }
    g_api->log(g_self, OP_MOD_LOG_INFO,
               "mod state restored: ticks=%u", g_ticks);
}

static void on_camera_update(CPUState* cpu) {
    g_ticks++;
    if (g_ticks == 300u && g_loads == 0 && !g_manual) {
        g_gems_at_save = op_read_u32(g_api, OP_GADDR_g_GemTotal);
        g_api->savestate_request(g_self, 1 /*save*/, STATE_SLOT);
    }
    if (g_ticks == 600u && g_loads == 0 && !g_manual) {
        g_loads = 1;
        g_api->savestate_request(g_self, 2 /*load*/, STATE_SLOT);
    }
    if (g_ticks == 301u && g_loads == 1) {
        uint32_t gems = op_read_u32(g_api, OP_GADDR_g_GemTotal);
        g_verify = (gems == g_gems_at_save);
        g_api->log(g_self, OP_MOD_LOG_INFO,
                   "post-rewind gems match=%s (%u vs %u at save)",
                   g_verify ? "yes" : "NO",
                   gems, g_gems_at_save);
    }
    if (g_ui_request) {
        int op = g_ui_request;
        g_ui_request = 0;
        if (op == 1) g_gems_at_save = op_read_u32(g_api, OP_GADDR_g_GemTotal);
        g_last_rc = g_api->savestate_request(g_self, op, STATE_SLOT);
    }
    g_api->base(cpu);
}

static void ui_draw(const openpete_mod_ui_t* ui, void* user) {
    (void)user;
    ui->text("ticks: %u", g_ticks);
    /* savestate_slot_present is a plain host-side file query, not a
     * guest/registration call, so the render-pure section may ask it. */
    ui->text("slot %d: %s", STATE_SLOT,
             g_api->savestate_slot_present(STATE_SLOT) ? "occupied" : "empty");

    ui->separator();
    if (ui->button("Save")) { g_ui_request = 1; g_manual = 1; }
    ui->tooltip("Queue an engine save of the current state on slot 9. "
                "The mod's tick counter travels in the state file.");
    ui->same_line();
    if (ui->button("Load")) { g_ui_request = 2; g_manual = 1; }
    ui->tooltip("Queue a load of slot 9: guest RAM/CPU/VRAM rewind and "
                "the tick counter above rewinds with them.");
    if (g_ui_request)
        ui->text_disabled("queueing %s at the next tick...",
                          g_ui_request == 1 ? "save" : "load");
    else if (g_last_rc != 1)
        ui->text_disabled(g_last_rc == 0 ? "request queued"
                                         : "request refused (one op at a "
                                           "time; a load is still pending)");

    ui->separator();
    if (g_manual)
        ui->text_disabled("scripted 300/600 sequence: stood down for manual "
                          "control");
    else if (g_ticks < 300u)
        ui->text_disabled("scripted save at tick 300, load at tick 600");
    else
        ui->text("rewinds performed: %d", g_loads);
    if (g_verify >= 0)
        ui->text("post-rewind gems match: %s (%u at save)",
                 g_verify ? "yes" : "NO", g_gems_at_save);
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;
    if (api->register_savestate_hooks(self, on_state_save,
                                      on_state_load) != 0)
        return -1;
    api->register_ui(self, ui_draw, NULL);
    return api->override_name(self, "CameraUpdate", on_camera_update);
}
