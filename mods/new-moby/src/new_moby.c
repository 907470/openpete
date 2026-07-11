/* new-moby — PoC rung 11: the content registry (a NOVEL moby type).
 *
 * Registers "orbiter", a new moby type the retail game has no id for.
 * The engine claims a class id that is FREE in the current level (its
 * model row is empty, so the level's compiled update switch has no case
 * for it — vanilla behavior for the id is `default`, i.e. nothing) and
 * seeds the model row from a donor class, so the new type renders and
 * collides with the donor's model. Behavior is entirely mod code: the
 * registry calls orbiter_update() once per live instance per tick,
 * after the game's own update.
 *
 * The PoC spawns ONE orbiter next to Spyro in Artisans (the spawn goes
 * through the game's own g_SpawnMoby with full donor init, then is
 * rebranded) and drives it in a circle at head height, spinning. The
 * render proof is structural, not visual: the game's own render walk
 * sets m_WasDrawn (+0x51) on the moby when it actually draws — reading
 * it back as 1 proves the novel type went through the vanilla pipeline
 * end to end (pool → update list → render bucket → cull → prims).
 *
 * Pattern to copy: write behavior against the moby PARAMETER of your
 * update_fn, never against a cached class id — ids are level-scoped.
 */

#include <openpete_mod_api.h>
#include <openpete_sdk_symbols.h>
#include <openpete_sdk_wrappers.h>

#define DONOR_CLASS 16 /* MOBYCLASS_BUTTERFLY — loaded wherever fodder is */

#define MOBY_OFF_POS      0x0Cu /* 3 x i32 world units */
#define MOBY_OFF_ROT_Y    0x45u /* Vector3D8 rotation, middle byte */
#define MOBY_OFF_WASDRAWN 0x51u

#define ORBIT_RADIUS 600  /* world units (Spyro's jump apex is ~506) */
#define ORBIT_HEIGHT 300
#define ORBIT_STEP   24   /* angle units/tick, 4096 = full circle */

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;

static uint32_t g_moby;      /* live orbiter guest vaddr, 0 = none */
static int32_t  g_center[3]; /* orbit center (spawn position) */
static uint32_t g_t;         /* orbit clock */
static int      g_last_id = -1;
static int      g_logged_draw, g_logged_tick;

static void orbiter_update(CPUState* cpu, uint32_t moby) {
    if (moby != g_moby) return; /* only the instance we spawned */
    g_t++;
    int32_t ang = (int32_t)((g_t * ORBIT_STEP) & 0xFFFu);
    int32_t s = op_call_Sin(cpu, ang);                     /* q12 */
    int32_t c = op_call_Sin(cpu, (ang + 1024) & 0xFFF);    /* cos */
    op_write_u32(g_api, moby + MOBY_OFF_POS + 0,
                 (uint32_t)(g_center[0] + ((ORBIT_RADIUS * c) >> 12)));
    op_write_u32(g_api, moby + MOBY_OFF_POS + 4,
                 (uint32_t)(g_center[1] + ((ORBIT_RADIUS * s) >> 12)));
    op_write_u32(g_api, moby + MOBY_OFF_POS + 8,
                 (uint32_t)(g_center[2] + ORBIT_HEIGHT));
    op_write_u8(g_api, moby + MOBY_OFF_ROT_Y, (uint8_t)(g_t * 3));

    if (!g_logged_tick && g_t == 256) {
        g_logged_tick = 1;
        g_api->log(g_self, OP_MOD_LOG_INFO, "new-moby: ticking (t=256)");
        g_api->log(g_self, OP_MOD_LOG_INFO,
                   "new-moby: diag state=0x%02X sub=0x%02X rend=0x%02X "
                   "wasdrawn=%d radius=%d updist=%d",
                   op_read_u8(g_api, moby + 0x48),
                   op_read_u8(g_api, moby + 0x49),
                   op_read_u8(g_api, moby + 0x4B),
                   op_read_u8(g_api, moby + MOBY_OFF_WASDRAWN),
                   op_read_u8(g_api, moby + 0x50),
                   op_read_u8(g_api, moby + 0x52));
    }
    if (!g_logged_draw && g_t > 8 &&
        op_read_u8(g_api, moby + MOBY_OFF_WASDRAWN) == 1) {
        g_logged_draw = 1;
        g_api->log(g_self, OP_MOD_LOG_INFO,
                   "new-moby: rendered (m_WasDrawn=1)");
    }
}

/* One-shot spawner: post-hook on the master update. Re-arms on level
 * change (the claimed id changes; the old instance died with the pool). */
static void on_gamestate_update(CPUState* cpu) {
    g_api->base(cpu);
    int id = g_api->moby_type_id(g_self, "orbiter");
    if (id != g_last_id) { g_last_id = id; g_moby = 0; }
    if (id <= 0 || g_moby) return;
    uint32_t m = g_api->spawn_moby(g_self, cpu, "orbiter");
    if (!m) return;
    g_moby = m;
    g_t = 0;
    g_logged_draw = g_logged_tick = 0;
    /* Orbit around Spyro's CURRENT position (g_Spyro.m_Position is the
     * struct's first field). Donor spawn cases that expect a parent
     * leave the moby at the origin, so place it explicitly. */
    g_center[0] = (int32_t)op_read_u32(g_api, OP_GADDR_g_Spyro + 0);
    g_center[1] = (int32_t)op_read_u32(g_api, OP_GADDR_g_Spyro + 4);
    g_center[2] = (int32_t)op_read_u32(g_api, OP_GADDR_g_Spyro + 8);
    op_write_u32(g_api, m + MOBY_OFF_POS + 0, (uint32_t)g_center[0]);
    op_write_u32(g_api, m + MOBY_OFF_POS + 4, (uint32_t)g_center[1]);
    op_write_u32(g_api, m + MOBY_OFF_POS + 8,
                 (uint32_t)(g_center[2] + ORBIT_HEIGHT));
    g_api->log(g_self, OP_MOD_LOG_INFO,
               "new-moby: spawned orbiter @0x%08X (class %d)", m, id);
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;
    if (api->api_version != OPENPETE_MOD_API_VERSION) return -1;
    op_sdk_bind(api);
    if (api->register_moby_type(self, "orbiter", DONOR_CLASS,
                                orbiter_update) != 0)
        return -1;
    return api->override_name(self, "GamestateUpdate", on_gamestate_update);
}
