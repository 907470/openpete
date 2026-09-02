/* egg-hud-3d — replace the collected-egg flipbook sprite with the game's own
 * 3D dragon egg, rendered through the game's HUD billboard path so the
 * engine's sub-tick interpolation spins it smoothly. See mod.toml.
 *
 * Two hooks:
 *   func_8001919C(RECT*, Tiledef*, ColorInt*) — the only builder of the egg
 *     sprite prim. A live egg (colour NULL, Tiledef one of the nine egg
 *     cells) is recorded and NOT passed to the original: no prim, no sprite.
 *     The inventory's dark "uncollected" marks (colour given, always cell 1,
 *     static) and every other sprite pass straight through.
 *   func_80022A2C — the shaded-moby renderer, the one call every screen
 *     (gameplay, pause, inventory) makes. Before it runs, one egg Moby per
 *     recorded sprite is appended to its render list at the sprite's
 *     position, rotated by the sprite's cell. Records are consumed here, so
 *     a re-simulated tick cannot double-fill them.
 *
 * Identity: each record maps to a fixed park slot (HUD row index, inventory
 * global-row column, inventory per-level row/column), so an egg's guest
 * address is stable across ticks and the engine's per-object interpolation
 * pairs it. */

#include <openpete_mod_api.h>
#include <openpete_sdk_symbols.h>
#include <openpete_game_structs.h>
#include <psx_runtime.h>

#include <string.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;

#define EGG_CLASS          34u          /* MOBYCLASS_DRAGON_EGG */
#define EGG_CELLS          9u
#define HUD_SLOTS          12u          /* g_Hud.m_SpriteRect[0..11] */
#define INV_ROW_SLOTS      12u          /* inventory global row */
#define INV_LEVEL_ROWS     6u           /* levels per inventory page */
#define INV_LEVEL_COLS     3u           /* eggs per level */
#define SLOTS (HUD_SLOTS + INV_ROW_SLOTS + INV_LEVEL_ROWS * INV_LEVEL_COLS)

/* g_Hud layout (spyro-1/include/hud.h): m_SpriteRect[32] at +0x464 (RECT,
 * 8 bytes), m_OrbAndEggSprite[10] at +0x564 (Tiledef, 8 bytes; [0] is the
 * life orb, [1..9] the egg cells). */
#define G_HUD_SPRITERECT   (OP_GADDR_g_Hud + 0x464u)
#define G_HUD_EGGTILEDEFS  (OP_GADDR_g_Hud + 0x564u + 8u)
/* g_SonyImage.m_ShadedMobys: Moby*[256], null-terminated (sony_image.h). */
#define G_SHADED_LIST      (OP_GADDR_g_SonyImage + 0x2400u)
#define SHADED_CAP         256u

/* Billboard depth: OFX/OFY are the screen position, TRZ = z >> 1. Chosen so
 * the egg's projected height matches the sprite's (measured against the
 * -og sprite capture: 0xB00 drew ~37 px tall against the sprite's ~30). */
#define Z_EGG              0xD00

typedef struct { int16_t x, y, w, h; } rect_t;

typedef struct {
    int      live;      /* recorded this tick */
    rect_t   rc;
    unsigned cell;      /* 0..8 */
} record_t;

static record_t s_rec[SLOTS];

/* Spin. The flipbook advances one cell per tick (g_Hud+0x40, the game's own
 * frame cursor, wraps mod 9), and egg i shows cell (cursor + i) % 9 — so an
 * egg's phase is (accumulated cursor advances + i). The angle is that phase
 * times a configurable step: the nine cells span far less than a turn (the
 * cell-to-cell motion measures ~12°), so 360°/9 per tick spun visibly fast. */
static double  s_step_units;          /* rotation-byte units per tick (256 = 360°) */
static uint8_t s_cursor_prev;
static int     s_cursor_seen;
static unsigned s_cursor_total;       /* ticks of flipbook advance since load */
#define G_HUD_EGGCURSOR    (OP_GADDR_g_Hud + 0x40u)
static uint32_t s_park;                /* guest vaddr of Moby[SLOTS] */
static Moby*    s_park_host;
static unsigned s_appended, s_dropped, s_ticks_seen;
static int      s_armed_logged;

/* ── slot derivation ─────────────────────────────────────────────────── */

/* Inventory per-level rows: the emitter's y is the level row's baseline;
 * rows are assigned by rank of distinct y within the tick. */
static int16_t  s_row_y[INV_LEVEL_ROWS];
static unsigned s_row_n;

static int slot_for(uint32_t rect_va, const rect_t* rc)
{
    /* HUD row: the RECT lives inside g_Hud.m_SpriteRect[0..11]. */
    if (rect_va >= G_HUD_SPRITERECT && rect_va < G_HUD_SPRITERECT + HUD_SLOTS * 8u)
        return (int)((rect_va - G_HUD_SPRITERECT) / 8u);
    /* Inventory global row: y == 50, x = 24 + 27*i (draw.c). */
    if (rc->y == 50) {
        int i = (rc->x - 24) / 27;
        return (i >= 0 && i < (int)INV_ROW_SLOTS) ? (int)HUD_SLOTS + i : -1;
    }
    /* Inventory per-level marks: x = scroll + 232 + 24*j (+24 when the level
     * has a single egg), y = level row - 8. */
    {
        int32_t scroll = op_read_s32(g_api, OP_GADDR_D_800757CC);
        int j = (rc->x - scroll - 232) / 24;
        if (j < 0) j = 0; if (j >= (int)INV_LEVEL_COLS) j = (int)INV_LEVEL_COLS - 1;
        unsigned r;
        for (r = 0; r < s_row_n; r++) if (s_row_y[r] == rc->y) break;
        if (r == s_row_n) {
            if (s_row_n >= INV_LEVEL_ROWS) return -1;
            s_row_y[s_row_n++] = rc->y;
        }
        return (int)(HUD_SLOTS + INV_ROW_SLOTS + r * INV_LEVEL_COLS + (unsigned)j);
    }
}

/* ── hook: the egg sprite builder ────────────────────────────────────── */

static void on_sprite(CPUState* cpu)
{
    uint32_t rect_va = cpu->a0, tile_va = cpu->a1, colour_va = cpu->a2;
    if (colour_va == 0 && tile_va >= G_HUD_EGGTILEDEFS &&
        tile_va < G_HUD_EGGTILEDEFS + EGG_CELLS * 8u && s_park) {
        const rect_t* rc = (const rect_t*)g_api->guest(rect_va);
        if (rc) {
            int slot = slot_for(rect_va, rc);
            if (slot >= 0) {
                s_rec[slot].live = 1;
                s_rec[slot].rc   = *rc;
                s_rec[slot].cell = (tile_va - G_HUD_EGGTILEDEFS) / 8u;
                s_dropped++;
                return;                     /* no base(): the sprite is gone */
            }
        }
    }
    g_api->base(cpu);
}

/* ── hook: the shaded renderer ───────────────────────────────────────── */

static void on_shaded_render(CPUState* cpu)
{
    if (s_park) {
        /* Advance the spin clock by the game's own cursor motion (once per tick
         * in gameplay and the inventory; unchanged when the render is called
         * twice in a tick or the game is paused). */
        uint8_t cursor = (uint8_t)(op_read_u32(g_api, G_HUD_EGGCURSOR) % EGG_CELLS);   /* int field, 0..8 */
        if (!s_cursor_seen) { s_cursor_seen = 1; s_cursor_prev = cursor; }
        if (cursor != s_cursor_prev) {
            s_cursor_total += (unsigned)((cursor + EGG_CELLS - s_cursor_prev) % EGG_CELLS);
            s_cursor_prev = cursor;
        }
        uint32_t n = 0;
        while (n < SHADED_CAP && op_read_u32(g_api, G_SHADED_LIST + n * 4u) != 0) n++;
        unsigned free_slots = (n < SHADED_CAP - 1u) ? (SHADED_CAP - 1u - n) : 0u;
        for (unsigned s = 0; s < SLOTS && free_slots; s++) {
            record_t* r = &s_rec[s];
            if (!r->live) continue;
            r->live = 0;
            Moby* m = &s_park_host[s];
            memset(m, 0, sizeof *m);
            m->m_Class            = (short)EGG_CLASS;
            m->m_Position.x       = r->rc.x + r->rc.w / 2;
            m->m_Position.y       = r->rc.y + r->rc.h / 2;
            m->m_Position.z       = Z_EGG;
            /* The model's long axis is model Y; the z rotation byte turns it
             * about that axis (verified by render: constant upright silhouette,
             * spots travelling horizontally, like the flipbook). */
            {
                unsigned i = (r->cell + EGG_CELLS - (unsigned)s_cursor_prev % EGG_CELLS) % EGG_CELLS;
                double phase = (double)(s_cursor_total + i);
                m->m_Rotation.z = (uint8_t)((long)(phase * s_step_units) & 0xFF);
            }
            m->m_DepthOffset      = 127;
            /* Specular/metal type 0, as the in-world egg moby carries it; the HUD
             * icons' type 11 (gold metal) shades the egg brown. */
            m->m_SpecularMetalType = 0;
            m->m_RenderRadius     = 0xFF;          /* bit7: HUD billboard */
            op_write_u32(g_api, G_SHADED_LIST + n * 4u, s_park + s * (uint32_t)sizeof(Moby));
            n++; free_slots--; s_appended++;
        }
        op_write_u32(g_api, G_SHADED_LIST + n * 4u, 0);
        for (unsigned s = 0; s < SLOTS; s++) s_rec[s].live = 0;   /* records consumed */
        s_row_n = 0;
        if (!s_armed_logged && s_appended) {
            s_armed_logged = 1;
            g_api->log(g_self, OP_MOD_LOG_INFO, "egg-hud-3d: armed (first egg substituted)");
        }
    }
    s_ticks_seen++;
    g_api->base(cpu);
}

/* ── entry ───────────────────────────────────────────────────────────── */

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self)
{
    g_api = api; g_self = self;
    void* host = 0;
    s_park = api->guest_alloc(self, SLOTS * (uint32_t)sizeof(Moby), 16, OP_GALLOC_LOW, &host);
    if (!s_park || !host) {
        api->log(self, OP_MOD_LOG_ERROR, "egg-hud-3d: low park allocation failed");
        return -1;
    }
    s_park_host = (Moby*)host;
    {
        double deg = api->config_float(self, "spin.degrees_per_tick", 12.0);
        if (deg < 1.0) deg = 1.0; if (deg > 60.0) deg = 60.0;
        /* Negative: the flipbook's spots travel the other way from a positive
         * z-byte step (verified against the sprite). */
        s_step_units = -deg * 256.0 / 360.0;
    }
    memset(s_park_host, 0, SLOTS * sizeof(Moby));
    if (api->override_addr(self, OP_FNADDR_func_8001919C, on_sprite) != 0) return -1;
    if (api->override_addr(self, OP_FNADDR_func_80022A2C, on_shaded_render) != 0) return -1;
    api->log(self, OP_MOD_LOG_INFO, "egg-hud-3d: park %u eggs at 0x%08x", (unsigned)SLOTS, s_park);
    return 0;
}
