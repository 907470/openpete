/**
 * @file natural_gems.c
 * @brief natural-gems: every gem pickup counts double, written against
 *        the typed wrapper surface instead of raw registers.
 *
 * The SDK generates a natural-signature wrapper for every prototyped game
 * function (openpete_sdk_wrappers.h). Three of them appear here:
 *
 * - OP_OVERRIDE_IMPL_CollectItem declares an override whose parameters
 *   arrive unpacked and typed (a Moby* instead of cpu->a0).
 * - op_base_CollectItem calls the next override or the original with
 *   natural arguments; the wrapper re-marshals them into registers.
 * - op_call_Sin calls an unrelated game function from inside the
 *   override, saving and restoring the argument registers around it so
 *   the pending CollectItem arguments survive.
 *
 * The wrappers exist because register marshalling is where hand-written
 * overrides go wrong: a forgotten save of cpu->a0 around a nested call
 * corrupts the outer call's argument. Prefer a wrapper whenever the
 * function is prototyped; fall back to raw cpu access only for a
 * function the symbol manifest does not prototype.
 *
 * This mod writes guest RAM (the gem total), so mod.toml declares it
 * mutative and saves played with it carry the modset tag.
 */

#include <openpete_mod_api.h>
#include <openpete_sdk_symbols.h>
#include <openpete_sdk_wrappers.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static int                       g_first = 1;  /**< First pickup not yet seen. */

/** @brief Result of the one-shot Sin(1024) call; -1 until the first pickup. */
static int32_t                   g_sin = -1;
/** @brief Pickups this override doubled. */
static uint32_t                  g_doubled;
/** @brief Gems granted on top of the game's own award. */
static uint32_t                  g_bonus_gems;

/**
 * @brief Repaint the Mods-panel status block.
 *
 * Sin is the game's own sine on its 4096-unit circle, so Sin(1024) is the
 * sine of 90 degrees and returns 4096. The line reports whether the
 * typed call produced that value, which is a check on the marshalling,
 * not on the game.
 */
static void publish_status(void) {
    if (g_sin < 0)
        g_api->ui_status(g_self, "natural call not made yet "
                         "(runs on the first gem pickup)");
    else
        g_api->ui_status(g_self, "Sin(1024) -> %d%s", (int)g_sin,
                         g_sin == 4096 ? " (expected)" : " (UNEXPECTED)");
    g_api->ui_status(g_self, "pickups doubled: %u", g_doubled);
    g_api->ui_status(g_self, "bonus gems granted: %u", g_bonus_gems);
}

/**
 * @brief Typed override of CollectItem(Moby* pMoby).
 *
 * The macro supplies `cpu` and the unpacked parameter `pMoby`. The body
 * is a post-hook: it calls the original first, then acts on the result.
 *
 * The gem award is detected by differencing g_GemTotal around the base
 * call rather than by decoding the moby, because the game's own code
 * already knows what the item was worth. Reading the delta after the
 * fact reuses that knowledge and stays correct for every item kind the
 * function handles. The doubled total is written back with op_write_u32,
 * the typed guest-RAM accessor from the SDK header.
 */
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
    publish_status();
}

/**
 * @brief Mod entry point.
 * @return The registration result; non-zero disables the mod.
 *
 * OP_REGISTER_OVERRIDE_CollectItem attaches the typed override by the
 * function's symbol name, so the mod carries no addresses of its own.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    publish_status();
    return OP_REGISTER_OVERRIDE_CollectItem(api, self, my_collect);
}
