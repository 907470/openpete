/* modsave-fixture-s23 - save-record fixture for Spyro 2 and 3.
 *
 * The save-derived state is one arena word, `value`: the count of the
 * game's Update (Spyro 2 0x8001B140, Spyro 3 0x80055400) calls made while
 * gameplay is live, so each save stores a different payload. The payload
 * is 16 bytes: the magic "FS23", the value, the schema and a byte sum.
 *
 * Log lines (the harness instrument), each prefixed "<tag>: ":
 *   mode <name> game <id> register rc <n>
 *   write index <i> value <v> data <fnv>
 *   read status <S> index <i> schema <n> size <n> data <fnv> value <v>
 */
#include <openpete_mod_api.h>
#include <psx_runtime.h>
#include <string.h>

#if defined(OPENPETE_MOD_SAVE_API) && OPENPETE_MOD_API_VERSION >= OPENPETE_MOD_SAVE_API
#define FX_HAVE_SAVE_API 1
#else
#define FX_HAVE_SAVE_API 0
#endif

#define FX_BYTES 16u

typedef struct {
    uint32_t value;
    uint8_t  payload[FX_BYTES];
} fx_state_t;

static const openpete_mod_api_t* fx_api;
static openpete_mod_t*           fx_self;
static fx_state_t*               fx_st;
static char                      fx_tag[64];
static uint32_t                  fx_schema;

/** @brief Update override: runs the game's update, then counts it when gameplay is live. */
static void fx_on_update(CPUState* cpu) {
    fx_api->base(cpu);
    if (fx_api->gameplay_live(fx_self, cpu)) fx_st->value++;
}

#if FX_HAVE_SAVE_API
/** @brief FNV-1a 32 of a byte span, the payload hash the harness compares. */
static uint32_t fx_hash(const uint8_t* p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

/** @brief Little-endian 32-bit store. */
static void fx_put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/** @brief Little-endian 32-bit load. */
static uint32_t fx_get32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/** @brief Status name for the log line. */
static const char* fx_status_name(openpete_mod_save_status_t s) {
    static const char* const k[] = { "OK", "ABSENT", "GAP", "STALE", "NEWER", "LEGACY_PAD" };
    return (unsigned)s < 6u ? k[s] : "?";
}

/** @brief Write callback: the payload of `value`. */
static int fx_write(openpete_mod_t* self, const openpete_mod_save_id_t* id, const void** data, uint32_t* size) {
    (void)self;
    if (fx_api->config_bool(fx_self, "write_fails", 0)) {
        fx_api->log(fx_self, OP_MOD_LOG_INFO, "%s: write index %u refused", fx_tag, id->save_index);
        return -1;
    }
    uint8_t* p = fx_st->payload;
    memcpy(p, "FS23", 4);
    fx_put32(p + 4, fx_st->value);
    fx_put32(p + 8, fx_schema);
    fx_put32(p + 12, (uint32_t)p[4] + p[5] + p[6] + p[7]);
    *data = p;
    *size = FX_BYTES;
    fx_api->log(fx_self, OP_MOD_LOG_INFO, "%s: write index %u value %u data %08x", fx_tag,
                id->save_index, fx_st->value, fx_hash(p, FX_BYTES));
    return 0;
}

/** @brief Read callback: replaces `value` from the record, 0 for any status without this fixture's payload. */
static void fx_read(openpete_mod_t* self, const openpete_mod_save_read_t* r) {
    (void)self;
    const uint8_t* d = (const uint8_t*)r->data;
    const int own = d && r->size == FX_BYTES && !memcmp(d, "FS23", 4);
    fx_st->value = own ? fx_get32(d + 4) : 0u;
    fx_api->log(fx_self, OP_MOD_LOG_INFO, "%s: read status %s index %u schema %u size %u data %08x value %u",
                fx_tag, fx_status_name(r->status), r->save_index, r->schema, r->size,
                d ? fx_hash(d, r->size) : 0u, fx_st->value);
}
#endif

/**
 * @brief Mod entry: allocates the arena word, counts Update, registers the save record.
 * @param api The engine table.
 * @param self This mod's handle.
 * @return 0 on success, -1 when the arena or the override is refused.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    char mode[16];
    void* view = 0;
    fx_api = api; fx_self = self;
    api->config_str(self, "tag", "modsave-fixture-s23", fx_tag, (int)sizeof fx_tag);
    api->config_str(self, "mode", "auto", mode, (int)sizeof mode);
    fx_schema = (uint32_t)api->config_int(self, "schema", 1);
    if (!api->guest_alloc(self, sizeof(fx_state_t), 16, OP_GALLOC_NO_DMA, &view) || !view) return -1;
    fx_st = (fx_state_t*)view;
    const char* game = api->game_id(self);
    const uint32_t update = !strcmp(game, "spyro-2") ? 0x8001B140u : 0x80055400u;
    if (api->override_addr(self, update, fx_on_update)) return -1;
    int rc = -2;
#if FX_HAVE_SAVE_API
    if (strcmp(mode, "baseline") && api->api_version >= OPENPETE_MOD_SAVE_API)
        rc = api->save_register(self, fx_schema, fx_write, fx_read);
#endif
    api->log(self, OP_MOD_LOG_INFO, "%s: mode %s game %s register rc %d", fx_tag, mode, game, rc);
    return 0;
}
