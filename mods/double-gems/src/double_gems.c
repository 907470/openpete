/* double-gems — PoC rung 3: typed guest-RAM write, real divergence.
 *
 * Post-hook + state-diff (the framework's event-detection idiom): let
 * CollectItem run, diff g_GemTotal across base(), and add the delta
 * again. No need to know WHICH item was collected or re-implement any
 * game logic — the state change IS the event.
 */

#include <openpete_mod_api.h>
#include <openpete_sdk_symbols.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;

/* Session tallies, host-side only: pickups the hook doubled and the
 * gems it granted on top of the game's own award. */
static uint32_t g_doubled;
static uint32_t g_bonus_gems;

static void on_collect_item(CPUState* cpu) {
    uint32_t before = op_read_u32(g_api, OP_GADDR_g_GemTotal);
    g_api->base(cpu);
    uint32_t after = op_read_u32(g_api, OP_GADDR_g_GemTotal);
    if (after > before) {
        uint32_t delta = after - before;
        op_write_u32(g_api, OP_GADDR_g_GemTotal, after + delta);
        g_doubled++;
        g_bonus_gems += delta;
        g_api->log(g_self, OP_MOD_LOG_INFO,
                   "gem pickup doubled: +%u twice, total %u -> %u",
                   delta, after, after + delta);
    }
}

static void ui_draw(const openpete_mod_ui_t* ui, void* user) {
    (void)user;
    ui->text("pickups doubled: %u", g_doubled);
    ui->tooltip("CollectItem calls that raised g_GemTotal; the same "
                "delta is then added a second time.");
    ui->text("bonus gems granted: %u", g_bonus_gems);
    if (!g_doubled)
        ui->text_disabled("no gem pickup seen yet this session");
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;
    api->register_ui(self, ui_draw, NULL);
    return api->override_name(self, "CollectItem", on_collect_item);
}
