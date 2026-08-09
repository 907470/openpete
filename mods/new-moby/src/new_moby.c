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
 * rebranded) and drives it in a circle at head height, spinning; the
 * overlay section can queue more. The render proof is structural, not
 * visual: the game's own render walk sets m_WasDrawn (+0x51) on the
 * moby when it actually draws — reading it back as 1 proves the novel
 * type went through the vanilla pipeline end to end (pool → update list
 * → render bucket → cull → prims).
 *
 * Pattern to copy: write behavior against the moby PARAMETER of your
 * update_fn, never against a cached class id — ids are level-scoped.
 */

#include <openpete_mod_api.h>
#include <openpete_sdk_symbols.h>
#include <openpete_sdk_wrappers.h>
#include <openpete_game_structs.h> /* typed Moby view over guest RAM */

#define DONOR_CLASS 16 /* MOBYCLASS_BUTTERFLY — loaded wherever fodder is */

#define ORBIT_RADIUS 600  /* world units (Spyro's jump apex is ~506) */
#define ORBIT_HEIGHT 300
#define ORBIT_STEP   24   /* angle units/tick, 4096 = full circle */

#define MAX_ORBITERS 8

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;

/* One tracked instance. `seen` is set whenever the registry dispatches
 * an update for it; a slot that misses a whole tick lost its moby (the
 * pool recycled it) and is reaped. */
typedef struct {
    uint32_t moby;      /* live orbiter guest vaddr, 0 = free slot */
    int32_t  center[3]; /* orbit center (spawn position) */
    uint32_t t;         /* orbit clock */
    int      seen;
} orbiter_t;

static orbiter_t g_orbiters[MAX_ORBITERS];
static int       g_live;      /* instances updated in the last full tick */
static int       g_seen;      /* accumulator for the tick in progress */
static int       g_spawn_req; /* pending spawns, consumed one per tick */
static int       g_last_id = -1;
static int       g_logged_draw, g_logged_tick;

/* Live orbit parameters, seeded from the defaults above and edited by
 * the overlay section; only the update below reads them, so writes from
 * the render-pure UI callback land in tick context. */
static int g_radius = ORBIT_RADIUS;
static int g_step   = ORBIT_STEP;

static void orbiter_update(CPUState* cpu, uint32_t moby_va) {
    orbiter_t* o = NULL;
    for (int i = 0; i < MAX_ORBITERS; i++) {
        if (g_orbiters[i].moby == moby_va) { o = &g_orbiters[i]; break; }
    }
    if (!o) return; /* only the instances we spawned */
    o->seen = 1;
    g_seen++;
    /* Typed view over guest RAM (openpete_game_structs.h): field names
     * instead of magic offsets; the layout is pinned by the header's
     * _Static_asserts, so drift fails THIS compile. */
    Moby* m = (Moby*)g_api->guest(moby_va);
    if (!m) return;
    o->t++;
    int32_t ang = (int32_t)((o->t * (uint32_t)g_step) & 0xFFFu);
    int32_t s = op_call_Sin(cpu, ang);                     /* q12 */
    int32_t c = op_call_Sin(cpu, (ang + 1024) & 0xFFF);    /* cos */
    m->m_Position.x = o->center[0] + ((g_radius * c) >> 12);
    m->m_Position.y = o->center[1] + ((g_radius * s) >> 12);
    m->m_Position.z = o->center[2] + ORBIT_HEIGHT;
    m->m_Rotation.y = (uint8_t)(o->t * 3);

    if (!g_logged_tick && o->t == 256) {
        g_logged_tick = 1;
        g_api->log(g_self, OP_MOD_LOG_INFO, "new-moby: ticking (t=256)");
        g_api->log(g_self, OP_MOD_LOG_INFO,
                   "new-moby: diag state=0x%02X sub=0x%02X rend=0x%02X "
                   "wasdrawn=%d radius=%d updist=%d",
                   m->m_State, m->m_Substate, m->m_Renderer.raw,
                   m->m_WasDrawn, m->m_RenderRadius, m->m_UpdateDistance);
    }
    if (!g_logged_draw && o->t > 8 && m->m_WasDrawn == 1) {
        g_logged_draw = 1;
        g_api->log(g_self, OP_MOD_LOG_INFO,
                   "new-moby: rendered (m_WasDrawn=1)");
    }
}

/* Spawner + bookkeeping: post-hook on the master update, so it runs
 * after the tick's moby updates. Re-arms on level change (the claimed
 * id changes; the old instances died with the pool). */
static void on_gamestate_update(CPUState* cpu) {
    g_api->base(cpu);
    int id = g_api->moby_type_id(g_self, "orbiter");
    if (id != g_last_id) {
        g_last_id = id;
        for (int i = 0; i < MAX_ORBITERS; i++) g_orbiters[i].moby = 0;
        g_spawn_req = 1; /* the PoC's own orbiter */
    }
    /* Publish this tick's instance count and drop slots whose moby is
     * gone (no update dispatched for a whole tick). */
    g_live = g_seen;
    g_seen = 0;
    for (int i = 0; i < MAX_ORBITERS; i++) {
        if (!g_orbiters[i].seen) g_orbiters[i].moby = 0;
        g_orbiters[i].seen = 0;
    }

    if (id <= 0 || g_spawn_req <= 0) return;
    orbiter_t* o = NULL;
    for (int i = 0; i < MAX_ORBITERS; i++) {
        if (!g_orbiters[i].moby) { o = &g_orbiters[i]; break; }
    }
    if (!o) return; /* table full — leave the request pending */
    uint32_t mva = g_api->spawn_moby(g_self, cpu, "orbiter");
    if (!mva) return;
    g_spawn_req--;
    o->moby = mva;
    o->t = 0;
    o->seen = 1; /* survive the reap until its first update lands */
    g_logged_draw = g_logged_tick = 0;
    /* Orbit around Spyro's CURRENT position (g_Spyro.m_Position is the
     * struct's first field). Donor spawn cases that expect a parent
     * leave the moby at the origin, so place it explicitly. */
    o->center[0] = (int32_t)op_read_u32(g_api, OP_GADDR_g_Spyro + 0);
    o->center[1] = (int32_t)op_read_u32(g_api, OP_GADDR_g_Spyro + 4);
    o->center[2] = (int32_t)op_read_u32(g_api, OP_GADDR_g_Spyro + 8);
    Moby* m = (Moby*)g_api->guest(mva);
    if (m) {
        m->m_Position.x = o->center[0];
        m->m_Position.y = o->center[1];
        m->m_Position.z = o->center[2] + ORBIT_HEIGHT;
    }
    g_api->log(g_self, OP_MOD_LOG_INFO,
               "new-moby: spawned orbiter @0x%08X (class %d)", mva, id);
}

/* Overlay section. Render-pure: it only reads state the tick hook
 * published and writes statics the tick hook consumes. */
static void ui_draw(const openpete_mod_ui_t* ui, void* user) {
    (void)user;
    if (g_last_id > 0)
        ui->text("orbiter class id: %d", g_last_id);
    else
        ui->text_disabled("orbiter class id: not claimed in this level");
    ui->text("live instances: %d / %d", g_live, MAX_ORBITERS);

    ui->separator();
    if (ui->button("Spawn orbiter") && g_spawn_req < MAX_ORBITERS)
        g_spawn_req++;
    ui->tooltip("Queues a spawn at Spyro's position; the next sim tick "
                "runs it through the game's own spawner.");
    if (g_spawn_req > 0) {
        ui->same_line();
        ui->text("%d queued", g_spawn_req);
    }

    ui->slider_int("Orbit radius", &g_radius, 100, 4000);
    ui->slider_int("Orbit speed", &g_step, 1, 256);
    ui->text_disabled("Speed is angle units/tick; 4096 = full circle.");
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;
    if (api->api_version != OPENPETE_MOD_API_VERSION) return -1;
    op_sdk_bind(api);
    if (api->register_moby_type(self, "orbiter", DONOR_CLASS,
                                orbiter_update) != 0)
        return -1;
    api->register_ui(self, ui_draw, NULL);
    return api->override_name(self, "GamestateUpdate", on_gamestate_update);
}
