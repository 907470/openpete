/* modsave-fixture-b - save-record fixture in spyro-japanese-extras' pad format.
 *
 * Pad layout (spyro-japanese-extras 1.2.2 `egg_save_encode`): "JPE1" at
 * 0, a profile id word at 4, a 16-bit count at 10, data in 12..107,
 * byte-sum checksum of 0..107 at 108. Data: `value` little-endian at 12,
 * then a pattern derived from it. Like spyro-japanese-extras it writes an
 * empty record (value 0) into every record the title creates, and after a
 * pad it cannot decode it locks: the load logs the lock and every later
 * SaveCreate restores the pad bytes it saw before the base ran.
 */
#define FX_ID "modsave-fixture-b"
#define FX_TITLE_WRITES 1
#define FX_RESTORES_WHEN_DAMAGED 1
#define FX_CORRUPT_SKIPS_UPDATE 0
#define FX_ON_DAMAGED() \
    fx_api->log(fx_self, OP_MOD_LOG_WARN, "%s: pad damaged, record locked", FX_ID)

#include "../../modsave-fixture-a/src/fixture_core.h"

#define FX_B_PROFILE 0x0B0B0001u

/** @brief Encode `value` as a JPE1 record into the 112-byte pad. */
static void fx_encode(uint8_t* pad, uint32_t value) {
    memset(pad, 0, FX_PAD_BYTES);
    memcpy(pad, "JPE1", 4);
    fx_put32(pad + 4, FX_B_PROFILE);
    pad[10] = (uint8_t)value;
    pad[11] = (uint8_t)(value >> 8);
    fx_put32(pad + 12, value);
    for (uint32_t i = 16; i < 108u; i++) pad[i] = (uint8_t)(value * 17u + i);
    fx_put32(pad + 108, fx_pad_sum(pad));
}

/** @brief Decode a JPE1 record; 1 with the value on a valid record, 0 otherwise. */
static int fx_decode_ok(const uint8_t* pad, uint32_t* value) {
    if (memcmp(pad, "JPE1", 4) != 0 || fx_get32(pad + 4) != FX_B_PROFILE) return 0;
    if (fx_get32(pad + 108) != fx_pad_sum(pad)) return 0;
    *value = fx_get32(pad + 12);
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
