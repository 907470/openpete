/* s3-scan-markers - shade Spyro 3's anti-piracy scanner mobys by scan state.
 *
 * Identification follows the game's own dispatch. The moby update loop
 * (func_8003038C) calls MobyUpdate[m_Class] (the table pointer at
 * 0x800742D8, the third word of the overlay header) with a0 = the moby. A
 * moby is a scanner when the resident level's table entry for its class is
 * one of that level's scanner functions below, so a level that places a
 * scanner class in several areas, or binds one scanner to two classes
 * (level 36's secondary, classes 75 and 79), is covered without a class
 * list.
 *
 * Scan state is read from the moby's data block (the word at moby + 0),
 * where each scanner keeps its cursor: 0 before the first call, the end
 * address once a pass is done. Most scanners leave it at the end; the
 * one-function scanners of the boss levels 17, 27, 37 and 47 move it back
 * and scan again, and the marker follows the cursor. Between start and
 * end the scan is in progress; it advances only on ticks the moby's update
 * runs and the scanner's own gates pass, so a cursor that moved within the
 * moby's last ADVANCE_WINDOW drawn ticks is "running" and one that did not
 * is "paused".
 *
 * Nothing here writes guest RAM: the refine callback reads the level, the
 * table, the data block and the cursor, and returns a uniform block.
 */
#include <stddef.h>

#include <openpete_mod_api.h>
#include <openpete_mod_ui.h>
#include <openpete_sdk_symbols.h>

#if !OPENPETE_GAME_SPYRO3
#error "s3-scan-markers reads Spyro 3's overlay header and scanner rows"
#endif

/* MobyUpdate, the class-indexed update table pointer of the resident
 * overlay (include/ovl_header.h of the decomp: OvlHeader word 2). */
#define S3_MOBY_UPDATE_PTR 0x800742D8u
#define S3_CLASS_MAX       768

/* One scanner: the level whose overlay holds it, its entry address, the
 * cursor's offset in the moby's data block and the cursor value that ends
 * the scan. Read off the overlay asm
 * (asm/nonmatchings/overlays/level_NN/moby/func_level_NN_<func>.s): the
 * EXE plus overlay scanners end at the overlay's data start, the
 * one-function scanners at the biased end of the EXE function they hash.
 * The same rows drive the engine's scan observer. */
typedef struct {
    uint8_t  level;
    uint8_t  secondary; /* 1 = one-function scan of an EXE function */
    uint16_t cursor_off;
    uint32_t func;
    uint32_t end;
} scan_row_t;

static const scan_row_t k_rows[] = {
    {11, 0, 0x024, 0x8007E800u, 0x80091ED4u},
    {11, 0, 0x06C, 0x80080CE0u, 0x80091ED4u},
    {12, 0, 0x038, 0x8007C2B8u, 0x8008BD18u},
    {13, 0, 0x038, 0x80080DFCu, 0x8008C5CCu},
    {14, 0, 0x030, 0x80083F9Cu, 0x800996ECu},
    {16, 0, 0x110, 0x80079AF4u, 0x800866F8u},
    {16, 1, 0x014, 0x80079654u, 0x8002ACD4u},
    {17, 0, 0x0DC, 0x8007774Cu, 0x80083EECu},
    {17, 1, 0x040, 0x8007A770u, 0x8003A584u},
    {21, 0, 0x01C, 0x8007FD94u, 0x8008F6A0u},
    {22, 0, 0x034, 0x8008D4C8u, 0x8009B090u},
    {23, 0, 0x028, 0x80080758u, 0x800963E4u},
    {24, 0, 0x058, 0x8007EF38u, 0x80096B68u},
    {26, 0, 0x0F4, 0x8007FC74u, 0x8008ADD8u},
    {26, 1, 0x010, 0x8007C6F8u, 0x80026C16u},
    {27, 0, 0x078, 0x8007797Cu, 0x80089BE0u},
    {27, 1, 0x04C, 0x8007A3C0u, 0x8003A584u},
    {31, 0, 0x05C, 0x80081920u, 0x8008FAF8u},
    {32, 0, 0x028, 0x80085480u, 0x8009D030u},
    {33, 0, 0x00C, 0x8008DF54u, 0x8009F264u},
    {34, 0, 0x014, 0x8008A88Cu, 0x8009886Cu},
    {36, 0, 0x084, 0x8007B044u, 0x800867F4u},
    {36, 1, 0x060, 0x80079D08u, 0x8002B3F7u},
    {37, 0, 0x0DC, 0x8007BD9Cu, 0x8008BA38u},
    {37, 1, 0x04C, 0x8007EFFCu, 0x8002AE00u},
    {41, 0, 0x048, 0x800806C8u, 0x80096324u},
    {42, 0, 0x034, 0x8008017Cu, 0x8008FEBCu},
    {43, 0, 0x040, 0x80085FC4u, 0x80096464u},
    {44, 0, 0x030, 0x80081188u, 0x80093E5Cu},
    {46, 0, 0x0C0, 0x8007F248u, 0x80091B40u},
    {46, 1, 0x00C, 0x8008343Cu, 0x80024F9Au},
    {47, 0, 0x0A0, 0x8007CF38u, 0x8008DF5Cu},
    {47, 1, 0x078, 0x80079AC4u, 0x8002AE00u},
};
#define N_ROWS ((int)(sizeof k_rows / sizeof k_rows[0]))

/* Refine calls a cursor may stand still and still read as running. The
 * engine asks the callback once per tick per drawn instance, so this counts
 * the moby's drawn ticks. */
#define ADVANCE_WINDOW 4u

enum { ST_IDLE, ST_RUNNING, ST_PAUSED, ST_DONE, ST_COUNT };
static const char* const k_state_name[ST_COUNT] = {"idle", "running", "paused", "done"};

/* One scanner moby seen this level: its record, last cursor, the refine
 * calls since the cursor last moved (saturating past ADVANCE_WINDOW) and
 * the state last reported. */
#define TRACK_N 64
typedef struct {
    uint32_t vaddr;
    uint32_t cursor;
    uint32_t still;
    int      state;
} track_t;

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static int      g_mark_paused = 1;
static int      g_mark_done;
static int      g_level = -1;
static track_t  g_track[TRACK_N];
static int      g_track_n;
static uint32_t g_marked, g_to_running, g_to_done, g_calls, g_claims;

/** @brief Return the resident level's scanner row whose function is the update of a class.
 *
 * @param level The game's currentLevel.
 * @param cls The moby's m_Class.
 * @return The row, or NULL when the class's update is no scanner of the level. */
static const scan_row_t* row_for_class(int level, int cls) {
    if (cls < 0 || cls >= S3_CLASS_MAX) return NULL;
    uint32_t table = op_read_u32(g_api, S3_MOBY_UPDATE_PTR);
    if (!table) return NULL;
    uint32_t fn = op_read_u32(g_api, table + 4u * (uint32_t)cls);
    if (!fn) return NULL;
    for (int i = 0; i < N_ROWS; i++)
        if (k_rows[i].level == level && k_rows[i].func == fn) return &k_rows[i];
    return NULL;
}

/** @brief Return the history slot of a scanner moby, creating it on first sight.
 *
 * @param vaddr The moby record's guest address.
 * @return The slot, or NULL when the table is full. */
static track_t* track_for(uint32_t vaddr) {
    for (int i = 0; i < g_track_n; i++)
        if (g_track[i].vaddr == vaddr) return &g_track[i];
    if (g_track_n >= TRACK_N) return NULL;
    track_t* t = &g_track[g_track_n++];
    t->vaddr = vaddr;
    t->cursor = 0;
    t->still = 0;
    t->state = -1;
    return t;
}

/** @brief Classify a scanner moby's scan from its cursor and history, logging each change.
 *
 * @param t The moby's history slot.
 * @param row Its scanner row.
 * @param cls Its class, for the log.
 * @param level The level, for the log.
 * @param cursor The cursor word read this tick.
 * @return The ST_* state. */
static int classify(track_t* t, const scan_row_t* row, int cls, int level, uint32_t cursor) {
    int fresh = t->state < 0;
    if (cursor != t->cursor) {
        t->cursor = cursor;
        t->still = fresh ? ADVANCE_WINDOW + 1u : 0u;
    } else if (t->still <= ADVANCE_WINDOW) {
        t->still++;
    }
    int st;
    if (cursor == 0u) st = ST_IDLE;
    else if (cursor == row->end) st = ST_DONE;
    else st = t->still <= ADVANCE_WINDOW ? ST_RUNNING : ST_PAUSED;
    if (st == t->state) return st;
    if (fresh) g_marked++;
    if (st == ST_RUNNING && t->state != ST_RUNNING) g_to_running++;
    if (st == ST_DONE) g_to_done++;
    g_api->log(g_self, OP_MOD_LOG_INFO,
               "level %d class %d moby 0x%08X scan 0x%08X%s: %s -> %s cursor 0x%08X "
               "(marked %u running-transitions %u done %u claims %u of %u calls)",
               level, cls, (unsigned)t->vaddr, (unsigned)row->func,
               row->secondary ? " (secondary)" : "", fresh ? "unseen" : k_state_name[t->state],
               k_state_name[st], (unsigned)cursor, (unsigned)g_marked,
               (unsigned)g_to_running, (unsigned)g_to_done, (unsigned)g_claims,
               (unsigned)g_calls);
    t->state = st;
    return st;
}

/** @brief Refine callback: claim a scanner moby with its state's colour, decline every other moby.
 *
 * @param key The moby instance.
 * @param out Uniform block: one vec4 colour.
 * @param cap Bytes available in @p out.
 * @return 16, or -1 for a moby that runs no scanner. */
static int refine(const openpete_mod_material_key_t* key, void* out, uint32_t cap) {
    if (cap < 16u || key->channel != OP_CHAN_MOBY) return -1;
    g_calls++;
    int level = op_read_s32(g_api, OP_GADDR_currentLevel);
    if (level != g_level) {
        g_level = level;
        g_track_n = 0;
    }
    const scan_row_t* row = row_for_class(level, key->u.moby.moby_class);
    if (!row) return -1;
    uint32_t data = op_read_u32(g_api, key->u.moby.vaddr);
    if (!data) return -1;
    uint32_t cursor = op_read_u32(g_api, data + row->cursor_off);
    track_t* t = track_for(key->u.moby.vaddr);
    int st = t ? classify(t, row, key->u.moby.moby_class, level, cursor)
               : (cursor && cursor != row->end ? ST_RUNNING : ST_IDLE);
    if (st == ST_PAUSED && !g_mark_paused) st = ST_IDLE;
    if (st == ST_DONE && !g_mark_done) st = ST_IDLE;
    static const float k_rgb[ST_COUNT][4] = {
        {0.10f, 0.30f, 1.00f, 1.0f}, /* idle: blue */
        {1.00f, 0.08f, 0.05f, 1.0f}, /* running: red */
        {0.70f, 0.10f, 0.90f, 1.0f}, /* paused mid-scan: purple */
        {0.10f, 0.90f, 0.20f, 1.0f}, /* done: green */
    };
    float* v = (float*)out;
    for (int i = 0; i < 4; i++) v[i] = k_rgb[st][i];
    g_claims++;
    return 16;
}

/** @brief Mods-panel section: the counters and the colour legend.
 *
 * @param ui The widget table. */
static void ui_draw(const openpete_mod_ui_t* ui) {
    int running = 0;
    for (int i = 0; i < g_track_n; i++) running += g_track[i].state == ST_RUNNING;
    ui->text("level %d: %d scanner mobys seen, %d running", g_level, g_track_n, running);
    ui->text("marked %u, entered running %u, finished %u", (unsigned)g_marked,
             (unsigned)g_to_running, (unsigned)g_to_done);
    ui->text("claims %u of %u refine calls", (unsigned)g_claims, (unsigned)g_calls);
    ui->text_disabled("blue = scanner, red = scanning%s%s",
                      g_mark_paused ? ", purple = paused mid-scan" : "",
                      g_mark_done ? ", green = finished" : "");
}
OPENPETE_MOD_UI_SECTION(ui_draw)

/** @brief Read the config and register the refine material on the moby channel.
 *
 * @param api The engine's function table.
 * @param self This mod's handle.
 * @return 0 to load; -1 on an engine below api 13. */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    if (api->api_version < 13u) return -1; /* game-scoped manifest */
    g_api = api;
    g_self = self;
    g_mark_paused = api->config_bool(self, "mark_paused", 1);
    g_mark_done = api->config_bool(self, "mark_done", 0);
    int fs = api->shader_register(self, "shaders/marker.frag");
    if (fs < 0) {
        api->log(self, OP_MOD_LOG_ERROR, "marker.frag failed to compile: no markers");
        return 0;
    }
    openpete_mod_material_selector_t sel = openpete_mod_material_sel_any();
    sel.channel = OP_CHAN_MOBY;
    if (api->material_register_refine(self, &sel, fs, refine) < 0)
        api->log(self, OP_MOD_LOG_ERROR, "material refused: no markers");
    return 0;
}
