/* s3-antipiracy-sentinel - Spyro 3's anti-piracy scans, shown as they run.
 *
 * Spyro 3 checks its own code for changes with CRC scans: a boot walk and
 * a title walk over the executable and the title code, a walk during each
 * level load, and in 24 levels one or two scanner mobys whose update
 * hashes a few dozen bytes per call. The engine observes every scan
 * without changing it and applies the game's own pass test at the game's
 * own comparison; this mod reads that verdict through
 * api->antipiracy_scan.
 *
 * Scanner instances. A moby is a candidate when the resident overlay's
 * MobyUpdate entry for its class (the table pointer at 0x800742D8, the
 * third word of the overlay header, which func_8003038C indexes by
 * m_Class) is one of the level's scanner functions below. Not every
 * instance of such a class scans: each row carries the scanner's own test
 * that selects the instances that reach its cursor initialisation (the
 * store of main_TEXT_START + 2 + levelIndex, or of the function start for
 * a one-function scan), read off the overlay asm. A row whose test is not
 * established marks an instance only once the engine has seen it step.
 *
 * Colours: blue an instance that passes the row's test and has not
 * started, yellow a scan in progress, green the game's compare passed,
 * red it failed. An instance outside the test is declined and draws with
 * its own shading.
 *
 * The force_fail option XORs one bit into a running scanner's CRC word in
 * its data block once per pass, so the game's own compare fails at the end
 * of the pass and the game applies its failure path (level 16: the
 * egg-collected bit of D_80070300 cleared). The walks are left alone: the
 * boot and title walks keep their CRC in registers, and the loading walk's
 * failure path (0x8002CD50) only calls rand and zeroes a word of
 * func_8002AB38, code that has already run. With the option off the mod
 * writes no guest RAM.
 */
#include <stddef.h>
#include <stdio.h>

#include <openpete_mod_api.h>
#include <openpete_mod_ui.h>
#include <openpete_sdk_symbols.h>
#include <psx_runtime.h>

#if !OPENPETE_GAME_SPYRO3
#error "s3-antipiracy-sentinel reads Spyro 3's overlay header and scanner rows"
#endif

#define S3_MOBY_UPDATE_PTR 0x800742D8u /* OvlHeader word 2: MobyUpdate */
#define S3_MOBY_ARRAY      0x8006C550u /* D_8006C550: first moby record */
#define S3_MOBY_ARRAY_END  0x8006C5ECu /* D_8006C5EC: end of the record array */
#define S3_MOBY_SIZE       0x58u
#define S3_CLASS_MAX       768
#define S3_TICK_FN         0x8001204Cu /* the camera update, once per gameplay tick */
#define S3_LOADER_FN       0x8002CA50u /* the level loader, every VBL of a load */

/* The test that picks a scanner class's scanning instances: a field of
 * the moby record or of its data block (the word at the record's offset
 * 0) compared with a constant. */
enum {
    G_ALL,          /* every instance (every placed instance stepped in the census runs) */
    G_STARTED,      /* not established: an instance counts once it has stepped */
    G_DATA_U8_EQ,   /* data block byte at off == val */
    G_DATA_S16_LT,  /* data block halfword at off < val */
    G_DATA_S32_LT0, /* data block word at off < 0 */
    G_MOBY_U8_EQ,   /* moby record byte at off == val */
    G_MOBY_U8_NE,   /* moby record byte at off != val */
    G_MOBY_S32_LT,  /* moby record word at off < val */
    G_MOBY_S32_GE,  /* moby record word at off >= val */
};

/* One scanner: its level, overlay id and entry, whether it hashes one EXE
 * function (secondary), and its instance test with the asm that makes it
 * (func_level_NN_<func>.s of the decomp). */
typedef struct {
    uint8_t  level;
    uint8_t  secondary;
    uint8_t  gate;
    uint16_t gate_off;
    int32_t  gate_val;
    uint32_t ovl;
    uint32_t func;
} scan_row_t;

#define ROW(l, f, sec, g, off, val) \
    {l, sec, g, off, val, OP_OVLID_func_level_##l##_##f, 0x##f##u}
static const scan_row_t k_rows[] = {
    ROW(11, 8007E800, 0, G_DATA_U8_EQ, 0x00, 0),        /* 0x8007EEC4 lbu d0; bnez skips */
    ROW(11, 80080CE0, 0, G_ALL, 0, 0),
    ROW(12, 8007C2B8, 0, G_DATA_U8_EQ, 0x00, 1),        /* 0x8007C334..0x8007C350: d0 == 1 */
    ROW(13, 80080DFC, 0, G_DATA_U8_EQ, 0x00, 2),        /* 0x800812A4..0x800812AC: d0 == 2 */
    ROW(14, 80083F9C, 0, G_MOBY_U8_EQ, 0x4A, 0xFF),     /* 0x80084098..0x800840A0: 0x4A == 0xFF */
    ROW(16, 80079AF4, 0, G_DATA_S16_LT, 0x74, 3),       /* 0x80079BC0..0x80079BCC: data+0x74 < 3 */
    ROW(16, 80079654, 1, G_ALL, 0, 0),
    ROW(17, 8007774C, 0, G_ALL, 0, 0),
    ROW(17, 8007A770, 1, G_ALL, 0, 0),
    ROW(21, 8007FD94, 0, G_MOBY_S32_LT, 0x14, 0x2800),  /* 0x80080270..0x8008027C: z < 0x2800 */
    ROW(22, 8008D4C8, 0, G_ALL, 0, 0),
    ROW(23, 80080758, 0, G_MOBY_U8_EQ, 0x48, 0x14),     /* 0x80080788..0x800807B4: state 0x14 */
    ROW(24, 8007EF38, 0, G_DATA_U8_EQ, 0x00, 3),        /* 0x8007F9C8..0x8007F9D0: d0 == 3 */
    ROW(26, 8007FC74, 0, G_DATA_S32_LT0, 0x38, 0),      /* 0x8007FD60..0x8007FD68: bltz data+0x38 */
    ROW(26, 8007C6F8, 1, G_MOBY_U8_NE, 0x48, 4),        /* 0x8007C71C..0x8007C724: state 4 skips */
    ROW(27, 8007797C, 0, G_ALL, 0, 0),
    ROW(27, 8007A3C0, 1, G_ALL, 0, 0),
    ROW(31, 80081920, 0, G_DATA_U8_EQ, 0x00, 0),        /* 0x80081DF4..0x80081DFC, 0x8008209C: d0 == 0 */
    ROW(32, 80085480, 0, G_MOBY_S32_GE, 0x14, 0x7531),  /* 0x80085634..0x80085640: z >= 0x7531 */
    ROW(33, 8008DF54, 0, G_STARTED, 0, 0),              /* needs a placed moby with 0x4A == 0xFF: none */
    ROW(34, 8008A88C, 0, G_ALL, 0, 0),
    ROW(36, 8007B044, 0, G_ALL, 0, 0),
    ROW(36, 80079D08, 1, G_ALL, 0, 0),
    ROW(37, 8007BD9C, 0, G_ALL, 0, 0),
    ROW(37, 8007EFFC, 1, G_ALL, 0, 0),
    ROW(41, 800806C8, 0, G_DATA_U8_EQ, 0x00, 5),        /* cursor set only for d0 == 5 */
    ROW(42, 8008017C, 0, G_ALL, 0, 0),
    ROW(43, 80085FC4, 0, G_MOBY_S32_LT, 0x10, 0x7530),  /* 0x80086438..0x80086444: y < 0x7530 */
    ROW(44, 80081188, 0, G_ALL, 0, 0),
    ROW(46, 8007F248, 0, G_ALL, 0, 0),
    ROW(46, 8008343C, 1, G_ALL, 0, 0),
    ROW(47, 8007CF38, 0, G_ALL, 0, 0),
    ROW(47, 80079AC4, 1, G_ALL, 0, 0),
};
#undef ROW
#define N_ROWS ((int)(sizeof k_rows / sizeof k_rows[0]))

/* Displayed states: OP_SCAN_IDLE..OP_SCAN_FAIL, plus not-a-scanner. */
#define ST_NONE (-1)
static const char* const k_state_name[4] = {"waiting", "scanning", "PASS", "FAIL"};
static const float k_rgb[4][4] = {
    {0.10f, 0.30f, 1.00f, 1.0f}, /* waiting: blue */
    {1.00f, 0.85f, 0.05f, 1.0f}, /* scanning: yellow */
    {0.10f, 0.90f, 0.20f, 1.0f}, /* PASS: green */
    {1.00f, 0.08f, 0.05f, 1.0f}, /* FAIL: red */
};

/* One scanner instance seen this level: its moby record and data block,
 * the state last reported, the comparisons counted when it was last
 * perturbed (force_fail), and its row. */
#define TRACK_N 64
typedef struct {
    uint32_t moby, data;
    int      state;
    uint32_t perturbed_at; /* completed + 1 at the last perturbation; 0 = never */
    uint32_t cursor;
    uint32_t claims;       /* refine claims of this instance */
    const scan_row_t* row;
} track_t;

/* One walk (boot, title, loading) as last reported. */
typedef struct {
    const char* name;
    uint32_t    key;
    int         state;
    uint32_t    completed;
} walk_track_t;

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static int      g_toast_all, g_toast_failures = 1, g_force_fail;
static int      g_level = -1;
static track_t  g_track[TRACK_N];
static int      g_track_n;
static walk_track_t g_walks[3] = {
    {"boot", OP_SCAN_KEY_BOOT, ST_NONE, 0},
    {"title", OP_SCAN_KEY_TITLE, ST_NONE, 0},
    {"loading", OP_SCAN_KEY_LOADING, ST_NONE, 0},
};
static uint32_t g_transitions, g_toasts, g_perturbs, g_calls, g_claims;
static uint32_t g_claims_state[4];

/** @brief Return the resident level's scanner row whose function is a class's update.
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

/** @brief Apply a row's instance test to one moby.
 *
 * @param row The scanner row.
 * @param moby The moby record's guest address.
 * @param data Its data block.
 * @return 1 when the scanner selects this instance, 0 when it does not, -1
 *         when the row's test is not established. */
static int gate_passes(const scan_row_t* row, uint32_t moby, uint32_t data) {
    switch (row->gate) {
    case G_ALL:          return 1;
    case G_DATA_U8_EQ:   return op_read_u8(g_api, data + row->gate_off) == (uint32_t)row->gate_val;
    case G_DATA_S16_LT:  return op_read_s16(g_api, data + row->gate_off) < row->gate_val;
    case G_DATA_S32_LT0: return op_read_s32(g_api, data + row->gate_off) < 0;
    case G_MOBY_U8_EQ:   return op_read_u8(g_api, moby + row->gate_off) == (uint32_t)row->gate_val;
    case G_MOBY_U8_NE:   return op_read_u8(g_api, moby + row->gate_off) != (uint32_t)row->gate_val;
    case G_MOBY_S32_LT:  return op_read_s32(g_api, moby + row->gate_off) < row->gate_val;
    case G_MOBY_S32_GE:  return op_read_s32(g_api, moby + row->gate_off) >= row->gate_val;
    default:             return -1;
    }
}

/** @brief Return a scanner instance's displayed state.
 *
 * @param row Its scanner row.
 * @param moby The moby record.
 * @param data Its data block.
 * @param scan Receives the engine's record when it has one, else state IDLE.
 * @return OP_SCAN_* or ST_NONE for an instance the scanner does not select. */
static int instance_state(const scan_row_t* row, uint32_t moby, uint32_t data,
                          openpete_mod_scan_t* scan) {
    scan->struct_size = sizeof *scan;
    int seen = g_api->antipiracy_scan(g_self, data, scan) == 0;
    if (!seen) scan->state = OP_SCAN_IDLE;
    if (scan->state != OP_SCAN_IDLE) return scan->state;
    return gate_passes(row, moby, data) == 1 ? OP_SCAN_IDLE : ST_NONE;
}

/** @brief Report one state change: a log line always, a toast when the options ask for it.
 *
 * A failure toasts when toast_failures or toast_all is on; any other change
 * toasts only with toast_all.
 * @param key Toast coalescing key.
 * @param what Who changed, for the log line.
 * @param toast Who changed in at most 16 characters of the toast glyph set.
 * @param from The previous state, or ST_NONE for first sight.
 * @param to The new state. */
static void report(const char* key, const char* what, const char* toast, int from, int to) {
    g_transitions++;
    g_api->log(g_self, OP_MOD_LOG_INFO, "%s: %s -> %s (transitions %u toasts %u perturbs %u)",
               what, from == ST_NONE ? "unseen" : k_state_name[from], k_state_name[to],
               (unsigned)g_transitions, (unsigned)g_toasts, (unsigned)g_perturbs);
    if (from == ST_NONE && to == OP_SCAN_IDLE) return; /* first sight, nothing ran yet */
    if (!(g_toast_all || (g_toast_failures && to == OP_SCAN_FAIL))) return;
    static const char* const k_toast[4] = {"WAITING", "SCANNING", "PASS", "FAIL"};
    g_api->notify(g_self, key, 0, "%s %s", toast, k_toast[to]);
    g_toasts++;
}

/** @brief Return a moby's history slot, creating it on first sight.
 *
 * @param moby The moby record's guest address.
 * @param row Its scanner row.
 * @return The slot, or NULL when the table is full. */
static track_t* track_for(uint32_t moby, const scan_row_t* row) {
    for (int i = 0; i < g_track_n; i++)
        if (g_track[i].moby == moby) return &g_track[i];
    if (g_track_n >= TRACK_N) return NULL;
    track_t* t = &g_track[g_track_n++];
    t->moby = moby;
    t->data = 0;
    t->state = ST_NONE;
    t->perturbed_at = 0;
    t->cursor = 0;
    t->claims = 0;
    t->row = row;
    return t;
}

/** @brief XOR one bit into a running scan's CRC word, once per pass, so the game's compare fails.
 *
 * A level scan's CRC-16 sits in the word's low 16 bits, a one-function
 * scan's in its high 16 (poly 0x80050000), so the bit flipped is bit 0 or
 * bit 16. CRC steps are invertible, so a changed register before the last
 * byte leaves a changed register at the compare.
 * @param scan The engine's record of the scan.
 * @param bit The bit to flip.
 * @param perturbed_at The slot's perturbation marker.
 * @return 1 when a store was made. */
static int perturb(const openpete_mod_scan_t* scan, uint32_t bit, uint32_t* perturbed_at) {
    if (!g_force_fail || scan->state != OP_SCAN_RUNNING || !scan->crc_addr) return 0;
    if (*perturbed_at == scan->completed + 1u) return 0;
    if (scan->cursor == scan->end) return 0;
    op_write_u32(g_api, scan->crc_addr, op_read_u32(g_api, scan->crc_addr) ^ bit);
    *perturbed_at = scan->completed + 1u;
    g_perturbs++;
    g_api->log(g_self, OP_MOD_LOG_WARN, "force_fail: flipped bit 0x%X of the CRC word at 0x%08X "
               "(scan 0x%08X cursor 0x%08X end 0x%08X)", (unsigned)bit, (unsigned)scan->crc_addr,
               (unsigned)scan->func, (unsigned)scan->cursor, (unsigned)scan->end);
    return 1;
}

/** @brief Poll the boot, title and loading walks for changes. */
static void poll_walks(void) {
    for (int i = 0; i < 3; i++) {
        walk_track_t* w = &g_walks[i];
        openpete_mod_scan_t s = {0};
        s.struct_size = sizeof s;
        if (g_api->antipiracy_scan(g_self, w->key, &s) != 0) continue;
        if (s.state == w->state && s.completed == w->completed) continue;
        if (s.state == OP_SCAN_IDLE && w->state == ST_NONE) {
            w->state = s.state;
            continue;
        }
        char key[16], name[16], what[32], toast[24];
        snprintf(key, sizeof key, "walk-%s", w->name);
        snprintf(name, sizeof name, "%s", w->name);
        for (char* c = name; *c; c++)
            if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 'a' + 'A');
        snprintf(what, sizeof what, "%s WALK", name);
        snprintf(toast, sizeof toast, "%s CHECK", name);
        report(key, what, toast, w->state, s.state);
        w->state = s.state;
        w->completed = s.completed;
    }
}

/** @brief Walk the level's mobys once per tick: report each scanner instance's state changes and apply force_fail. */
static void poll_mobys(void) {
    int level = op_read_s32(g_api, OP_GADDR_currentLevel);
    if (level != g_level) {
        g_level = level;
        g_track_n = 0;
    }
    uint32_t lo = op_read_u32(g_api, S3_MOBY_ARRAY), hi = op_read_u32(g_api, S3_MOBY_ARRAY_END);
    if (!lo || hi < lo || hi - lo > S3_MOBY_SIZE * 4096u) return;
    for (uint32_t m = lo; m < hi; m += S3_MOBY_SIZE) {
        if (op_read_u8(g_api, m + 0x48) >= 0x80) continue; /* free slot */
        const scan_row_t* row = row_for_class(level, op_read_s16(g_api, m + 0x36));
        if (!row) continue;
        uint32_t data = op_read_u32(g_api, m);
        if (!data) continue;
        openpete_mod_scan_t s;
        int st = instance_state(row, m, data, &s);
        if (st == ST_NONE) continue;
        track_t* t = track_for(m, row);
        if (!t) continue;
        /* A cursor that moved back is a restarted scan (a repeating scan's
         * next pass): it owes a new perturbation. The perturbation waits for
         * a tick on which the cursor advanced, so a data block that the game
         * re-initialises while its scan stands still (level 16's houses,
         * class 154, between their first step and Spyro's approach) is
         * perturbed after the re-initialisation. */
        int advanced = t->cursor != 0u && s.cursor > t->cursor;
        if (s.cursor < t->cursor) t->perturbed_at = 0;
        t->data = data;
        t->cursor = s.cursor;
        if (advanced && perturb(&s, row->secondary ? 0x10000u : 1u, &t->perturbed_at)) continue;
        if (st == t->state) continue;
        char key[24], what[48], toast[40];
        snprintf(key, sizeof key, "scan-%08X", (unsigned)data);
        snprintf(what, sizeof what, "LEVEL %d SCAN %08X MOBY %08X", level, (unsigned)row->func,
                 (unsigned)m);
        snprintf(toast, sizeof toast, "LEVEL %d CHECK %d", level, (int)(t - g_track) + 1);
        report(key, what, toast, t->state, st);
        t->state = st;
    }
}

/** @brief Read the three options, so a change in the mods panel applies from the next tick.
 *
 * Logs each change of force_fail. */
static void read_options(void) {
    g_toast_all = g_api->config_bool(g_self, "toast_all", 0);
    g_toast_failures = g_api->config_bool(g_self, "toast_failures", 1);
    int ff = g_api->config_bool(g_self, "force_fail", 0);
    if (ff != g_force_fail)
        g_api->log(g_self, OP_MOD_LOG_WARN, "force_fail %s", ff ? "on: every running scan will fail"
                                                               : "off");
    g_force_fail = ff;
}

/** @brief Tick hook on the camera update: poll the walks and the scanner mobys, then run the update.
 *
 * @param cpu The guest CPU state. */
static void on_tick(CPUState* cpu) {
    read_options();
    poll_walks();
    poll_mobys();
    g_api->base(cpu);
}

/** @brief Loader hook: poll the walks while a level loads (the camera tick does not run then), then run the loader.
 *
 * @param cpu The guest CPU state. */
static void on_loader(CPUState* cpu) {
    read_options();
    poll_walks();
    g_api->base(cpu);
}

/** @brief Refine callback: colour a scanning instance by its state, decline every other moby.
 *
 * @param key The moby instance.
 * @param out Uniform block: one vec4 colour.
 * @param cap Bytes available in @p out.
 * @return 16, or -1 for a moby that is not a scanning instance. */
static int refine(const openpete_mod_material_key_t* key, void* out, uint32_t cap) {
    if (cap < 16u || key->channel != OP_CHAN_MOBY) return -1;
    g_calls++;
    int level = op_read_s32(g_api, OP_GADDR_currentLevel);
    const scan_row_t* row = row_for_class(level, key->u.moby.moby_class);
    if (!row) return -1;
    uint32_t data = op_read_u32(g_api, key->u.moby.vaddr);
    if (!data) return -1;
    openpete_mod_scan_t s;
    int st = instance_state(row, key->u.moby.vaddr, data, &s);
    if (st == ST_NONE) return -1;
    float* v = (float*)out;
    for (int i = 0; i < 4; i++) v[i] = k_rgb[st][i];
    g_claims++;
    g_claims_state[st]++;
    for (int i = 0; i < g_track_n; i++)
        if (g_track[i].moby == key->u.moby.vaddr) g_track[i].claims++;
    return 16;
}

/** @brief Mods-panel section: the walks, the level's scanner instances and the counters.
 *
 * @param ui The widget table. */
static void ui_draw(const openpete_mod_ui_t* ui) {
    for (int i = 0; i < 3; i++)
        ui->text("%s walk: %s", g_walks[i].name,
                 g_walks[i].state == ST_NONE ? "not seen" : k_state_name[g_walks[i].state]);
    ui->text("level %d: %d checking objects", g_level, g_track_n);
    for (int i = 0; i < g_track_n; i++) {
        const track_t* t = &g_track[i];
        ui->text("  check %d: moby %08X scan %08X%s: %s, %u claims", i + 1, (unsigned)t->moby,
                 (unsigned)t->row->func, t->row->secondary ? " (one function)" : "",
                 t->state == ST_NONE ? "-" : k_state_name[t->state], (unsigned)t->claims);
    }
    ui->text("transitions %u, toasts %u, forced %u", (unsigned)g_transitions, (unsigned)g_toasts,
             (unsigned)g_perturbs);
    ui->text("claims %u of %u refine calls", (unsigned)g_claims, (unsigned)g_calls);
    ui->text_disabled("blue = waiting, yellow = scanning, green = PASS, red = FAIL");
}
OPENPETE_MOD_UI_SECTION(ui_draw)

/** @brief Log the run's totals at exit, for scripted runs. */
static void on_shutdown(void) {
    for (int i = 0; i < g_track_n; i++) {
        const track_t* t = &g_track[i];
        g_api->log(g_self, OP_MOD_LOG_INFO, "level %d moby %08X scan %08X: %s, %u claims", g_level,
                   (unsigned)t->moby, (unsigned)t->row->func,
                   t->state == ST_NONE ? "-" : k_state_name[t->state], (unsigned)t->claims);
    }
    g_api->log(g_self, OP_MOD_LOG_INFO, "totals: transitions %u toasts %u perturbs %u claims %u "
               "(waiting %u scanning %u pass %u fail %u) of %u refine calls",
               (unsigned)g_transitions, (unsigned)g_toasts, (unsigned)g_perturbs,
               (unsigned)g_claims, (unsigned)g_claims_state[0], (unsigned)g_claims_state[1],
               (unsigned)g_claims_state[2], (unsigned)g_claims_state[3], (unsigned)g_calls);
}

/** @brief Read the options, bind the tick and loader hooks and register the refine material.
 *
 * @param api The engine's function table.
 * @param self This mod's handle.
 * @return 0 to load; -1 on an engine below api 13. */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    if (api->api_version < 13u) return -1; /* game-scoped manifest */
    g_api = api;
    g_self = self;
    read_options();
    if (api->override_addr(self, S3_TICK_FN, on_tick) != 0 ||
        api->override_addr(self, S3_LOADER_FN, on_loader) != 0)
        api->log(self, OP_MOD_LOG_ERROR, "tick or loader hook refused: no state tracking");
    api->register_shutdown_hook(self, on_shutdown);
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
