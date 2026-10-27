/* fixture_core.h - shared body of the two save-record test fixtures.
 *
 * modsave-fixture-a and modsave-fixture-b each include this file after
 * defining their pad codec (FX_ID, fx_encode, fx_decode_ok, FX_TITLE_WRITES),
 * so the two fixtures differ only in the bytes they store at
 * SaveFile+0x590 (Spyro 1's 112-byte pad, outside the checksum) and in
 * what each does with a pad it cannot decode. Neither changes gameplay:
 * the save-derived state is one arena word, `value`, the count of stock
 * GamestateUpdate calls made in g_Gamestate 0 since the last restore.
 *
 * Modes (config key `mode`):
 *   auto     the save-record api when the engine and this build have it,
 *            else the legacy pad path;
 *   legacy   the pad path: SaveCreate and SaveLoad overrides and the
 *            func_8001277C (New Game) reset, no save_register;
 *   baseline neither: no save overrides, no save_register, no pad write;
 *            the card it leaves is the reference for the api legs.
 *
 * The save-record api is compiled only when the SDK header defines
 * OPENPETE_MOD_SAVE_API (the api version that added save_register); the
 * runtime takes it only when api->api_version reaches that version. A
 * build without it compiles the corrupt path of fixture A instead
 * (FX_CORRUPT_SKIPS_UPDATE), which reproduces portal-randomizer 0.1.5:
 * after a damaged pad it returns from GamestateUpdate without the base
 * while g_Gamestate is 0 or 2.
 *
 * Calibration faults (config key `seed_fault`, 0 = none):
 *   1  SaveCreate also flips record byte 0x40 (inside the checksummed
 *      body), so the card differs from the baseline outside the pads;
 *   2  GamestateUpdate skips the base once every 500 calls, so the stock
 *      update count falls behind the tick count.
 *
 * Log lines (the harness instrument), all prefixed "<FX_ID>: ":
 *   mode <name> api <n>
 *   save_create gs <g> pad <hash>        legacy path, after the encode
 *   save_load gs <g> rc <v0> pad <state> value <v>
 *   read status <S> index <i> schema <n> size <n> data <hash> value <v> gs <g>
 *                                                    api path
 *   write index <i> value <v> data <hash>             api path
 *   reset value 0                                    New Game, legacy path
 */
#ifndef MODSAVE_FIXTURE_CORE_H
#define MODSAVE_FIXTURE_CORE_H

#include <openpete_mod_api.h>
#include <openpete_sdk_symbols.h>
#include <psx_runtime.h>
#include <string.h>

#if defined(OPENPETE_MOD_SAVE_API) && OPENPETE_MOD_API_VERSION >= OPENPETE_MOD_SAVE_API
#define FX_HAVE_SAVE_API 1
#else
#define FX_HAVE_SAVE_API 0
#endif

#define FX_PAD_OFS   0x590u
#define FX_PAD_BYTES 112u
#define FX_GS_PLAYING 0u
#define FX_GS_PAUSE   2u
#define FX_GS_TITLE   13u

enum { FX_MODE_AUTO, FX_MODE_LEGACY, FX_MODE_BASELINE, FX_MODE_API };

/** @brief Arena block: everything the fixture derives from a save. */
typedef struct {
    uint32_t value;   /* stock gameplay updates since the last restore */
    uint32_t damaged; /* a load read a pad this fixture could not decode */
    uint32_t calls;   /* GamestateUpdate override entries (seed_fault 2) */
    uint8_t  payload[FX_PAD_BYTES]; /* api write buffer */
} fx_state_t;

static const openpete_mod_api_t* fx_api;
static openpete_mod_t*           fx_self;
static fx_state_t*               fx_st;
static int                       fx_mode;
static int                       fx_fault;

/* Codec, defined by the including fixture before this header. */
static void fx_encode(uint8_t* pad, uint32_t value);
static int  fx_decode_ok(const uint8_t* pad, uint32_t* value);

/** @brief FNV-1a 32 of a byte span, the payload hash the harness compares. */
static uint32_t fx_hash(const uint8_t* p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

/** @brief Little-endian 32-bit store into a byte buffer. */
static void fx_put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/** @brief Little-endian 32-bit load from a byte buffer. */
static uint32_t fx_get32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/** @brief Byte sum of pad[0..108), the checksum both codecs store at pad+108. */
static uint32_t fx_pad_sum(const uint8_t* pad) {
    uint32_t s = 0;
    for (uint32_t i = 0; i < 108u; i++) s += pad[i];
    return s;
}

/** @brief 1 when every byte of the span is zero. */
static int fx_all_zero(const uint8_t* p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) if (p[i]) return 0;
    return 1;
}

/** @brief The game's state word. */
static uint32_t fx_gamestate(void) {
    return op_read_u32(fx_api, OP_GADDR_g_Gamestate);
}

/** @brief Classify a pad copy and restore the value from it; returns the class name. */
static const char* fx_restore_from_pad(const uint8_t* pad) {
    uint32_t v = 0;
    if (fx_all_zero(pad, FX_PAD_BYTES)) { fx_st->value = 0; fx_st->damaged = 0; return "zero"; }
    if (fx_decode_ok(pad, &v)) { fx_st->value = v; fx_st->damaged = 0; return "own"; }
    fx_st->damaged = 1;
    return "damaged";
}

/** @brief SaveLoad override (legacy path): copies the pad, runs the base, then decodes the copy. */
static void fx_on_save_load(CPUState* cpu) {
    uint8_t pad[FX_PAD_BYTES];
    const uint8_t* rec = (const uint8_t*)fx_api->guest(cpu->a0);
    if (rec) memcpy(pad, rec + FX_PAD_OFS, FX_PAD_BYTES);
    else memset(pad, 0, sizeof pad);
    fx_api->base(cpu);
    const uint32_t rc = cpu->v0;
    const char* cls = rc ? fx_restore_from_pad(pad) : "unchecked";
    fx_api->log(fx_self, OP_MOD_LOG_INFO, "%s: save_load gs %u rc %u pad %s value %u",
                FX_ID, fx_gamestate(), rc, cls, fx_st->value);
    if (fx_st->damaged) FX_ON_DAMAGED();
}

/** @brief SaveCreate override (legacy path): runs the base, then stores this fixture's pad. */
static void fx_on_save_create(CPUState* cpu) {
    uint8_t* rec = (uint8_t*)fx_api->guest(cpu->a0);
    uint8_t before[FX_PAD_BYTES];
    if (rec) memcpy(before, rec + FX_PAD_OFS, FX_PAD_BYTES);
    fx_api->base(cpu);
    if (!rec) return;
    const uint32_t gs = fx_gamestate();
    if (fx_st->damaged && FX_RESTORES_WHEN_DAMAGED)
        memcpy(rec + FX_PAD_OFS, before, FX_PAD_BYTES);
    else if (gs != FX_GS_TITLE || FX_TITLE_WRITES)
        fx_encode(rec + FX_PAD_OFS, gs == FX_GS_TITLE ? 0u : fx_st->value);
    if (fx_fault == 1) rec[0x40] ^= 0x01u;
    fx_api->log(fx_self, OP_MOD_LOG_INFO, "%s: save_create gs %u pad %08x", FX_ID, gs,
                fx_hash(rec + FX_PAD_OFS, FX_PAD_BYTES));
}

/** @brief func_8001277C override (legacy path): a New Game clears the save-derived state. */
static void fx_on_new_game(CPUState* cpu) {
    fx_st->value = 0;
    fx_st->damaged = 0;
    fx_api->log(fx_self, OP_MOD_LOG_INFO, "%s: reset value 0", FX_ID);
    fx_api->base(cpu);
}

/** @brief GamestateUpdate override: counts stock gameplay updates into `value`.
 *
 * Fixture A without the save api skips the base while damaged in
 * g_Gamestate 0 or 2 (FX_CORRUPT_SKIPS_UPDATE); seed_fault 2 skips the base
 * once every 500 entries. */
static void fx_on_update(CPUState* cpu) {
    const uint32_t gs = fx_gamestate();
    fx_st->calls++;
#if FX_CORRUPT_SKIPS_UPDATE && !FX_HAVE_SAVE_API
    if (fx_st->damaged && (gs == FX_GS_PLAYING || gs == FX_GS_PAUSE)) return;
#endif
    if (fx_fault == 2 && fx_st->calls % 500u == 0u) return;
    fx_api->base(cpu);
    if (gs == FX_GS_PLAYING) fx_st->value++;
}

#if FX_HAVE_SAVE_API
/** @brief Status name for the log line. */
static const char* fx_status_name(openpete_mod_save_status_t s) {
    switch (s) {
    case OP_MOD_SAVE_OK:         return "OK";
    case OP_MOD_SAVE_ABSENT:     return "ABSENT";
    case OP_MOD_SAVE_GAP:        return "GAP";
    case OP_MOD_SAVE_STALE:      return "STALE";
    case OP_MOD_SAVE_NEWER:      return "NEWER";
    case OP_MOD_SAVE_LEGACY_PAD: return "LEGACY_PAD";
    default:                     return "?";
    }
}

/** @brief Save-record write callback: serializes `value` with this fixture's pad codec. */
static int fx_write(openpete_mod_t* self, const openpete_mod_save_id_t* id,
                    const void** data, uint32_t* size) {
    (void)self;
    if (fx_api->config_bool(fx_self, "write_fails", 0)) {
        fx_api->log(fx_self, OP_MOD_LOG_INFO, "%s: write index %u refused", FX_ID, id->save_index);
        return -1;
    }
    fx_encode(fx_st->payload, fx_st->value);
    *data = fx_st->payload;
    *size = FX_PAD_BYTES;
    fx_api->log(fx_self, OP_MOD_LOG_INFO, "%s: write index %u value %u data %08x", FX_ID,
                id->save_index, fx_st->value, fx_hash(fx_st->payload, FX_PAD_BYTES));
    return 0;
}

/** @brief Save-record read callback: replaces the save-derived state from the record. */
static void fx_read(openpete_mod_t* self, const openpete_mod_save_read_t* r) {
    (void)self;
    const uint8_t* d = (const uint8_t*)r->data;
    uint32_t v = 0;
    fx_st->damaged = 0;
    if (d && r->size == FX_PAD_BYTES && fx_decode_ok(d, &v)) fx_st->value = v;
    else fx_st->value = 0; /* ABSENT, NEWER, foreign or zero LEGACY_PAD */
    fx_api->log(fx_self, OP_MOD_LOG_INFO,
                "%s: read status %s index %u schema %u size %u data %08x value %u gs %u",
                FX_ID, fx_status_name(r->status), r->save_index, r->schema, r->size,
                d ? fx_hash(d, r->size) : 0u, fx_st->value, fx_gamestate());
}
#endif

/** @brief Resolve the configured mode against what the engine and this build offer. */
static int fx_pick_mode(const openpete_mod_api_t* api) {
    char m[16];
    api->config_str(fx_self, "mode", "auto", m, (int)sizeof m);
    if (!strcmp(m, "legacy")) return FX_MODE_LEGACY;
    if (!strcmp(m, "baseline")) return FX_MODE_BASELINE;
#if FX_HAVE_SAVE_API
    if (api->api_version >= OPENPETE_MOD_SAVE_API) return FX_MODE_API;
#endif
    return FX_MODE_LEGACY;
}

/** @brief Shared entry: allocates the arena block and registers the mode's hooks. */
static int fx_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    static const char* names[] = { "auto", "legacy", "baseline", "api" };
    fx_api = api; fx_self = self;
    void* view = 0;
    if (!api->guest_alloc(self, sizeof(fx_state_t), 16, OP_GALLOC_NO_DMA, &view) || !view)
        return -1;
    fx_st = (fx_state_t*)view;
    fx_mode = fx_pick_mode(api);
    fx_fault = (int)api->config_int(self, "seed_fault", 0);
    api->log(self, OP_MOD_LOG_INFO, "%s: mode %s api %u seed_fault %d", FX_ID,
             names[fx_mode], api->api_version, fx_fault);
    int rc = api->override_name(self, "GamestateUpdate", fx_on_update);
    if (fx_mode == FX_MODE_LEGACY) {
        rc |= api->override_name(self, "SaveCreate", fx_on_save_create);
        rc |= api->override_name(self, "SaveLoad", fx_on_save_load);
        rc |= api->override_name(self, "func_8001277C", fx_on_new_game);
    }
#if FX_HAVE_SAVE_API
    if (fx_mode == FX_MODE_API)
        rc |= api->save_register(self, (uint32_t)api->config_int(self, "schema", 1),
                                 fx_write, fx_read);
#endif
    return rc ? -1 : 0;
}

#endif /* MODSAVE_FIXTURE_CORE_H */
