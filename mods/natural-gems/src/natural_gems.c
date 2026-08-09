/* natural-gems — marshalling-wrapper proof (see mod.toml).
 *
 * Everything below is natural-signature C: no cpu->a0 spelunking. The
 * typed Moby* arrives unpacked, base() re-marshals it, and op_call_Sin
 * invokes the game's own sine (PSX 4096-unit circle: Sin(1024) == 90°
 * == 4096) with argument registers saved/restored around the call.
 */

#include <openpete_mod_api.h>
#include <openpete_sdk_symbols.h>
#include <openpete_sdk_wrappers.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static int                       g_first = 1;

/* Host-side session tallies: the one-shot Sin(1024) result (-1 until the
 * first pickup makes the natural call) and the pickups this override
 * doubled, with the gems it granted on top of the game's own award. */
static int32_t                   g_sin = -1;
static uint32_t                  g_doubled;
static uint32_t                  g_bonus_gems;

OP_OVERRIDE_IMPL_CollectItem(my_collect) {
    if (g_first) {
        g_first = 0;
        int32_t s = op_call_Sin(cpu, 1024);
        g_sin = s;
        g_api->log(g_self, OP_MOD_LOG_INFO,
                   "natural call Sin(1024) -> %d", (int)s);
        g_api->log(g_self, OP_MOD_LOG_INFO,
                   "natural override: CollectItem(moby=%s)",
                   pMoby ? "non-null" : "NULL");
    }
    uint32_t before = op_read_u32(g_api, OP_GADDR_g_GemTotal);
    op_base_CollectItem(cpu, pMoby);
    uint32_t after = op_read_u32(g_api, OP_GADDR_g_GemTotal);
    if (after > before) {
        uint32_t delta = after - before;
        op_write_u32(g_api, OP_GADDR_g_GemTotal, after + delta);
        g_doubled++;
        g_bonus_gems += delta;
        g_api->log(g_self, OP_MOD_LOG_INFO,
                   "natural gem double: +%u twice, total %u -> %u",
                   delta, after, after + delta);
    }
}

static void ui_draw(const openpete_mod_ui_t* ui, void* user) {
    (void)user;
    if (g_sin < 0) {
        ui->text_disabled("natural call not made yet "
                          "(runs on the first gem pickup)");
    } else {
        ui->text("Sin(1024) -> %d%s", (int)g_sin,
                 g_sin == 4096 ? " (expected)" : " (UNEXPECTED)");
        ui->tooltip("The game's own sine, called with natural arguments; "
                    "1024 is 90 degrees on the PSX 4096-unit circle.");
    }
    ui->separator();
    ui->text("pickups doubled: %u", g_doubled);
    ui->tooltip("CollectItem calls that raised g_GemTotal; the same delta "
                "is then added a second time.");
    ui->text("bonus gems granted: %u", g_bonus_gems);
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;
    api->register_ui(self, ui_draw, NULL);
    return OP_REGISTER_OVERRIDE_CollectItem(api, self, my_collect);
}
