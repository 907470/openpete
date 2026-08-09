/* crash-dummy — rung 6: deliberate segfault inside an override, so the
 * crash handler's attribution line can be asserted. See mod.toml. */

#include <openpete_mod_api.h>

#define AUTO_CRASH_AT 100u

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static unsigned                  g_calls;

/* Host-side controls published by the overlay section, consumed inside
 * the override — attribution only names the mod when the fault happens
 * on the chain frame, so the UI never dereferences anything itself. */
static int g_auto_armed = 1;
static int g_crash_request;

static void crash_now(const char* why) {
    g_api->log(g_self, OP_MOD_LOG_WARN,
               "dereferencing NULL on purpose — %s", why);
    *(volatile int*)0 = 1; /* boom */
}

static void on_camera_update(CPUState* cpu) {
    g_calls++;
    if (g_crash_request) {
        g_crash_request = 0;
        crash_now("attribution test (requested from the overlay)");
    }
    if (g_auto_armed && g_calls == AUTO_CRASH_AT) {
        crash_now("attribution test");
    }
    g_api->base(cpu);
}

static void ui_draw(const openpete_mod_ui_t* ui, void* user) {
    (void)user;

    ui->text("CameraUpdate calls: %u", g_calls);
    if (g_auto_armed) {
        if (g_calls < AUTO_CRASH_AT) {
            ui->text("Armed: crashing on call %u (%u to go)", AUTO_CRASH_AT,
                     AUTO_CRASH_AT - g_calls);
        } else {
            ui->text_disabled("Auto crash point already passed");
        }
    } else {
        ui->text_disabled("Auto crash disarmed");
    }

    ui->checkbox("Auto-crash on call 100", &g_auto_armed);
    ui->tooltip("Uncheck to keep playing and pick the crash moment "
                "yourself.");

    ui->separator();

    if (ui->button("CRASH THE GAME NOW (null dereference)")) {
        g_crash_request = 1;
    }
    ui->tooltip("Kills the process on the next CameraUpdate. The crash "
                "report must name this mod. Unsaved progress is lost.");
    if (g_crash_request) {
        ui->text("Crash requested — faulting on the next CameraUpdate.");
    }
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;
    api->register_ui(self, ui_draw, NULL);
    return api->override_name(self, "CameraUpdate", on_camera_update);
}
