/* fade-skip — PoC rung 14: the instruction-site hook tier.
 *
 * Six [[site]] anchors (mod.toml), each a `slti` loop-bound test inside
 * Initialize's blocking splash-fade sequence. The shared handler REPLACES
 * the test with "bound reached": slti writes v0 = (counter < bound), so
 * v0 = 0 makes the following `bne v0, zero, <loop>` fall through — each
 * fade loop exits after one iteration and each 210-VSync hold ends on its
 * first test. Instruction-granular by necessity: the loops are inline in
 * the boot routine; no function boundary exists to override.
 *
 * Pattern to copy for site handlers: raw cpu-context semantics — read and
 * write cpu registers directly (the SDK stages the full CPUState via
 * psx_runtime.h); one handler can serve many sites by dispatching on
 * site_addr. Handlers run mid-sim (tick rules apply).
 */

#include <openpete_mod_api.h>
#include <psx_runtime.h> /* full CPUState — site handlers poke registers */

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static uint32_t                  g_fired[8];
static int                       g_fired_count;

static void skip_loop_bound(CPUState* cpu, uint32_t site_addr) {
    cpu->v0 = 0; /* replaces `slti v0, <counter>, <bound>`: bound reached */
    for (int i = 0; i < g_fired_count; i++)
        if (g_fired[i] == site_addr) return;
    if (g_fired_count < (int)(sizeof g_fired / sizeof g_fired[0]))
        g_fired[g_fired_count++] = site_addr;
    g_api->log(g_self, OP_MOD_LOG_INFO,
               "fade-skip: skipped loop @ 0x%08X", site_addr);
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;
    if (api->api_version != OPENPETE_MOD_API_VERSION) return -1;

    static const char* const sites[] = {
        "sony-fade-in-bound", "sony-hold-bound", "sony-fade-out-bound",
        "univ-fade-in-bound", "univ-hold-bound", "univ-fade-out-bound",
    };
    const int n = (int)(sizeof sites / sizeof sites[0]);
    int live = 0;
    for (int i = 0; i < n; i++)
        if (api->site_handler(self, sites[i], skip_loop_bound) == 0)
            live++;
    g_api->log(g_self, OP_MOD_LOG_INFO, "fade-skip: %d/%d sites live",
               live, n);
    /* Not baked is not a load failure: the loader already logged the
     * regen recipe; we stay resident and inert. */
    return 0;
}
