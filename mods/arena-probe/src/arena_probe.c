/* arena-probe — CI proof of the guest scratch arena (op_guest_alloc).
 *
 * Allocates a block, writes a pattern through the HOST view, reads it
 * back through the GUEST path (api->guest over the region cold path),
 * and asserts vaddr<->host consistency — the property marshalled calls
 * with guest-pointer args depend on. The arena is host-backed and no
 * canonical guest RAM is touched, so the run must stay ORACLE-EXACT
 * (expect_oracle = "pass"): an active arena is sim-neutral until a mod
 * points game code at it.
 */

#include <openpete_mod_api.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;

/* Probe results, published for the overlay section below. */
static uint32_t g_va;          /* vaddr of the 256-byte block */
static uint32_t g_va2;         /* vaddr of the 64-byte, 64-aligned block */
static int      g_ok = -1;     /* -1 until the probe has run */

/* Status lines: the probe runs once at entry, so one paint suffices —
 * ui_status lines persist until repainted. */
static void publish_status(void) {
    if (g_ok < 0) {
        g_api->ui_status(g_self, "probe has not run");
        return;
    }
    g_api->ui_status(g_self, "block A: 0x%08X  (256 B, 16-aligned)", g_va);
    g_api->ui_status(g_self, "block B: 0x%08X  (64 B, 64-aligned)", g_va2);
    g_api->ui_status(g_self, "host write -> guest read: %s",
                     g_ok ? "OK" : "MISMATCH");
    g_api->ui_status(g_self, "probe runs once, at mod entry");
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;
    if (api->api_version != OPENPETE_MOD_API_VERSION) return -1;

    void* host = 0;
    uint32_t va = api->guest_alloc(self, 256, 16, &host);
    if (!va || !host) {
        g_api->log(g_self, OP_MOD_LOG_ERROR, "arena-probe: alloc FAILED");
        return -1;
    }
    for (int i = 0; i < 256; i++) ((uint8_t*)host)[i] = (uint8_t)(i * 7);
    int ok = 1;
    for (int i = 0; i < 256; i += 4) {
        uint32_t want;
        /* build expected little-endian word from the pattern */
        want = (uint32_t)(uint8_t)(i * 7)
             | ((uint32_t)(uint8_t)((i + 1) * 7) << 8)
             | ((uint32_t)(uint8_t)((i + 2) * 7) << 16)
             | ((uint32_t)(uint8_t)((i + 3) * 7) << 24);
        if (op_read_u32(g_api, va + (uint32_t)i) != want) { ok = 0; break; }
    }
    /* second allocation must not overlap the first */
    void* host2 = 0;
    uint32_t va2 = api->guest_alloc(self, 64, 64, &host2);
    if (!va2 || va2 < va + 256 || (va2 & 63u)) ok = 0;
    g_va = va;
    g_va2 = va2;
    g_ok = ok;
    g_api->log(g_self, OP_MOD_LOG_INFO,
               ok ? "arena-probe: guest/host roundtrip OK @0x%08X"
                  : "arena-probe: roundtrip MISMATCH @0x%08X", va);
    publish_status();
    return ok ? 0 : -1;
}
