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
static uint32_t                  g_exits[8];   /* forced exits per fired site */
static int                       g_fired_count;

/* Site names, in mod.toml order, and whether each one attached. A site
 * only attaches when its address was baked into the binary by
 * regen-mod-sites.sh; site_handler returns -1 for the rest. */
static const char* const g_site_names[] = {
    "sony-fade-in-bound", "sony-hold-bound", "sony-fade-out-bound",
    "univ-fade-in-bound", "univ-hold-bound", "univ-fade-out-bound",
};
enum { SITE_COUNT = (int)(sizeof g_site_names / sizeof g_site_names[0]) };
static int g_site_live[SITE_COUNT];
static int g_live_count;

static void skip_loop_bound(CPUState* cpu, uint32_t site_addr) {
    cpu->v0 = 0; /* replaces `slti v0, <counter>, <bound>`: bound reached */
    for (int i = 0; i < g_fired_count; i++)
        if (g_fired[i] == site_addr) { g_exits[i]++; return; }
    if (g_fired_count < (int)(sizeof g_fired / sizeof g_fired[0])) {
        g_exits[g_fired_count] = 1;
        g_fired[g_fired_count++] = site_addr;
    }
    g_api->log(g_self, OP_MOD_LOG_INFO,
               "fade-skip: skipped loop @ 0x%08X", site_addr);
}

static void ui_draw(const openpete_mod_ui_t* ui, void* user) {
    (void)user;
    ui->text("sites: %d/%d attached", g_live_count, SITE_COUNT);
    if (g_live_count < SITE_COUNT) {
        ui->text_disabled("inert (address not baked into the binary):");
        for (int i = 0; i < SITE_COUNT; i++)
            if (!g_site_live[i]) ui->text_disabled("  %s", g_site_names[i]);
        ui->text_disabled("bake with: scripts/regen-mod-sites.sh --build");
    }
    ui->separator();
    uint32_t total = 0;
    for (int i = 0; i < g_fired_count; i++) total += g_exits[i];
    ui->text("loop exits forced: %u", total);
    for (int i = 0; i < g_fired_count; i++)
        ui->text("  0x%08X  x%u", g_fired[i], g_exits[i]);
    if (g_fired_count == 0)
        ui->text_disabled("no site has fired: the splash fades run once, "
                          "during boot");
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;
    if (api->api_version != OPENPETE_MOD_API_VERSION) return -1;
    api->register_ui(self, ui_draw, NULL);

    for (int i = 0; i < SITE_COUNT; i++)
        if (api->site_handler(self, g_site_names[i], skip_loop_bound) == 0) {
            g_site_live[i] = 1;
            g_live_count++;
        }
    g_api->log(g_self, OP_MOD_LOG_INFO, "fade-skip: %d/%d sites live",
               g_live_count, SITE_COUNT);
    /* Not baked is not a load failure: the loader already logged the
     * regen recipe; we stay resident and inert. */
    return 0;
}
