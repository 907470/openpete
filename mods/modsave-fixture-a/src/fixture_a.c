/* modsave-fixture-a - save-record fixture in portal-randomizer's pad format.
 *
 * Pad layout (portal-randomizer 0.1.5 `pr_encode`): "PRND" at 0, version
 * byte 1 at 4, data in bytes 5..79, bytes 80..107 zero, byte-sum checksum
 * of 0..107 at 108. Data: `value` little-endian at 5, then a pattern
 * derived from it. A pad that is non-zero and not a valid PRND record
 * sets `damaged`; without the save-record api the fixture then skips
 * GamestateUpdate's base in g_Gamestate 0 and 2, the 0.1.5 freeze. Like
 * portal-randomizer it writes no pad at the title's file create.
 */
#define FX_ID "modsave-fixture-a"
#define FX_TITLE_WRITES 0
#define FX_RESTORES_WHEN_DAMAGED 0
#define FX_CORRUPT_SKIPS_UPDATE 1
#define FX_ON_DAMAGED() \
    fx_api->log(fx_self, OP_MOD_LOG_WARN, "%s: pad damaged, corrupt", FX_ID)

#include "fixture_core.h"

/** @brief Encode `value` as a PRND record into the 112-byte pad. */
static void fx_encode(uint8_t* pad, uint32_t value) {
    memset(pad, 0, FX_PAD_BYTES);
    memcpy(pad, "PRND", 4);
    pad[4] = 1;
    fx_put32(pad + 5, value);
    for (uint32_t i = 9; i < 80u; i++) pad[i] = (uint8_t)(value * 31u + i);
    fx_put32(pad + 108, fx_pad_sum(pad));
}

/** @brief Decode a PRND record; 1 with the value on a valid record, 0 otherwise. */
static int fx_decode_ok(const uint8_t* pad, uint32_t* value) {
    if (memcmp(pad, "PRND", 4) != 0 || pad[4] != 1) return 0;
    if (fx_get32(pad + 108) != fx_pad_sum(pad)) return 0;
    *value = fx_get32(pad + 5);
    return 1;
}

/**
 * @brief Mod entry: registers the fixture in its configured mode.
 * @param api The engine table.
 * @param self This mod's handle.
 * @return 0 on success, -1 when a registration fails.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    return fx_entry(api, self);
}
