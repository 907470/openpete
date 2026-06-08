// games/spyro-1/game_extras_impl.c — Spyro 1's per-game hooks.
//
// Phase 0b fills in only the identity hooks needed for the empty binary's
// window title + future disc validation. Memory layout, overlays, and
// runtime quirks are TODO until Phase 1b.
//
// Disc image: <spyro>/roms/SCUS-94228/SCUS-94228.cue
// Disc BIN SHA256: 95f03abf97c9ff0b2a64888ed7dbbb4b59a7b4363cf188cd0a562b95cfd4809f
// PSX EXE: SCUS_942.28 (CRC32 over full header+body)

#include "game_extras.h"
#include "feature_registry.h"
#include "openpete_psx.h"
#include "openpete_ui_moby.h"
#include "openpete_fiber.h"
#include "psx_runtime.h"
#include "openpete_render_cache.h"
#include "features/widescreen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Identity --- */

uint32_t game_get_entry_point(void) {
    /* From spyro-1/build/PSX.EXE header (initial_pc field). */
    return 0x8005B8E0u;
}

const char* game_get_exe_filename(void) {
    return "SCUS_942.28";
}

uint32_t game_get_expected_crc32(void) {
    /* CRC32 of spyro-1/build/PSX.EXE (full file, 2 KB header + body).
     * Re-compute if the matching decomp's binary changes:
     *   python3 -c "import zlib; print(hex(zlib.crc32(open('build/PSX.EXE','rb').read())))"
     */
    return 0xC56EB4EEu;
}

const char* game_get_display_name(void) {
    return "Spyro the Dragon";
}

/* --- Memory layout (PSX defaults; overrides come in Phase 1b) --- */

uint32_t game_get_ram_size(void)             { return 2u * 1024u * 1024u; }
uint32_t game_get_compiled_range_start(void) { return 0x80010000u; }
uint32_t game_get_compiled_range_end(void)   { return 0x80075800u; } /* load_addr + load_size */
uint32_t game_get_overlay_range_start(void)  { return 0x80100000u; } /* TODO Phase 1b */
uint32_t game_get_overlay_range_end(void)    { return 0x801F0000u; } /* TODO Phase 1b */

/* --- Overlays / quirks / per-frame --- */

int  game_on_overlay_load(uint32_t overlay_id) {
    (void)overlay_id;
    /* TODO Phase 1b — copy overlay data into g_ram, swap dispatcher table. */
    return 0;
}

void game_on_overlay_init(CPUState* cpu) {
    (void)cpu;
    /* TODO Phase 1b — runs once after WadInit. */
}

extern unsigned openpete_frame;

/* --- Optional write-watcher for channel-leak investigations -------------
 *
 * Set the following env vars to log every byte/half/word write that
 * touches [LO, HI):
 *
 *   OPENPETE_WATCH_LO=0x80070C00
 *   OPENPETE_WATCH_HI=0x80071000
 *   OPENPETE_WATCH_FRAME_MAX=20         (optional; cap noise)
 *
 * Output: stderr, one line per write:
 *   WATCH frame=N pc=0xPPPPPPPP addr=0xAAAAAAAA size=B val=0xVV
 *
 * Regen with `OPENPETE_REGEN_PC_TRACKING=1 bash scripts/regen-recomp.sh`
 * first so cpu->pc reflects the writing MIPS instruction. The recipe
 * for closing a render→gameplay channel is in
 * notes/plans/openpete-presentation-decoupling.md "Order of
 * operations for each new channel". */
static struct {
    void (*orig_w)(uint32_t, uint32_t);
    void (*orig_h)(uint32_t, uint16_t);
    void (*orig_b)(uint32_t, uint8_t);
    CPUState* cpu;
    uint32_t lo, hi;
    int frame_max;
    int active;
} g_watch = {0};

static int watch_should_log(uint32_t addr) {
    if (!g_watch.active) return 0;
    if (addr < g_watch.lo || addr >= g_watch.hi) return 0;
    if (g_watch.frame_max > 0
        && (int)openpete_frame > g_watch.frame_max) return 0;
    return 1;
}
static void watch_log(uint32_t addr, uint32_t val, int size) {
    fprintf(stderr,
            "WATCH frame=%u pc=0x%08x addr=0x%08x size=%d val=0x%0*x\n",
            openpete_frame, g_watch.cpu->pc, addr, size, size * 2, val);
}
static void watch_w(uint32_t a, uint32_t v) {
    /* A 4-byte store overlaps the watch range if any of its 4 bytes
     * fall inside; check the start byte (most common case) and then
     * the tail byte to catch stores straddling the LO boundary. */
    if (watch_should_log(a) || watch_should_log(a + 3)) watch_log(a, v, 4);
    g_watch.orig_w(a, v);
}
static void watch_h(uint32_t a, uint16_t v) {
    if (watch_should_log(a)) watch_log(a, v, 2);
    g_watch.orig_h(a, v);
}
static void watch_b(uint32_t a, uint8_t v) {
    if (watch_should_log(a)) watch_log(a, v, 1);
    g_watch.orig_b(a, v);
}

static void install_watch_from_env(CPUState* cpu) {
    const char* lo_s = getenv("OPENPETE_WATCH_LO");
    const char* hi_s = getenv("OPENPETE_WATCH_HI");
    if (!lo_s || !hi_s) return;
    g_watch.cpu       = cpu;
    g_watch.lo        = (uint32_t)strtoul(lo_s, NULL, 0);
    g_watch.hi        = (uint32_t)strtoul(hi_s, NULL, 0);
    const char* fm_s  = getenv("OPENPETE_WATCH_FRAME_MAX");
    g_watch.frame_max = fm_s ? atoi(fm_s) : 0;
    g_watch.orig_w    = cpu->write_word;
    g_watch.orig_h    = cpu->write_half;
    g_watch.orig_b    = cpu->write_byte;
    cpu->write_word   = watch_w;
    cpu->write_half   = watch_h;
    cpu->write_byte   = watch_b;
    g_watch.active    = 1;
    fprintf(stderr,
            "[watch] installed: [0x%08x, 0x%08x) frame_max=%d\n",
            g_watch.lo, g_watch.hi, g_watch.frame_max);
}

/* PGXP Day 4 Phase 1.3 / 2.5: Spyro-specific PC→perspective classifier.
 *
 * Prim-classes identified empirically (Phase 1.1 PC dump + Phase 0
 * function survey):
 *   tracer       : func_80017B48          0x80017B48..0x80017BFC
 *   moby_proj    : func_8001F798/F158 + func_80022A2C
 *                  0x8001F000..0x80023AC4 (covers both ranges contiguously)
 *   spyro_mesh   : func_80023AC4 and its many leaf helpers
 *                  0x80023AC4..0x80025000 (rough; helpers extend higher)
 *   terrain      : func_800258F0          0x800258F0..0x800276A0
 *   cyclorama    : func_8004EBA8          0x8004EBA8..0x8004F000
 *   particles    : func_800573C8 + func_80058D64
 *                  0x800573C8..0x80059474
 *   shadow       : func_80059F8C          0x80059F8C..0x8005A470
 *
 * Phase 2.5 bisection: each class has its own env gate. By default ALL
 * are off (= shipping Day 1c). Set OPENPETE_PGXP_<CLASS>=1 to opt in.
 * Used to isolate which class causes the level-10 missing-geo
 * regression. */
static int s_pgxp_class_init = 0;
static int s_persp_tracer = 0;
static int s_persp_moby   = 0;
static int s_persp_spyro  = 0;
static int s_persp_terr   = 0;
static int s_persp_cyc    = 0;
static int s_persp_part   = 0;
static int s_persp_shad   = 0;
static int s_persp_other  = 0;  /* PCs that don't match any known range */

static int env_on(const char* name) {
    const char* v = getenv(name);
    return (v && v[0] && v[0] != '0') ? 1 : 0;
}

static void spyro_pgxp_init_classes(void) {
    if (s_pgxp_class_init) return;
    s_persp_tracer = env_on("OPENPETE_PGXP_TRACER");
    s_persp_moby   = env_on("OPENPETE_PGXP_MOBY");
    s_persp_spyro  = env_on("OPENPETE_PGXP_SPYRO");
    s_persp_terr   = env_on("OPENPETE_PGXP_TERRAIN");
    s_persp_cyc    = env_on("OPENPETE_PGXP_CYCLORAMA");
    s_persp_part   = env_on("OPENPETE_PGXP_PARTICLES");
    s_persp_shad   = env_on("OPENPETE_PGXP_SHADOW");
    s_persp_other  = env_on("OPENPETE_PGXP_OTHER");
    s_pgxp_class_init = 1;
    fprintf(stderr, "[PGXP-CLASS] tracer=%d moby=%d spyro=%d terrain=%d cyclorama=%d particles=%d shadow=%d other=%d\n",
            s_persp_tracer, s_persp_moby, s_persp_spyro, s_persp_terr,
            s_persp_cyc, s_persp_part, s_persp_shad, s_persp_other);
}

static int spyro_pgxp_classify(uint32_t pc, int is_canonical) {
    spyro_pgxp_init_classes();
    if (!is_canonical) return 0;
    if (pc >= 0x80017B48u && pc < 0x80017BFCu)  return s_persp_tracer;
    if (pc >= 0x8001F000u && pc < 0x80023AC4u)  return s_persp_moby;
    if (pc >= 0x80023AC4u && pc < 0x80025000u)  return s_persp_spyro;
    if (pc >= 0x800258F0u && pc < 0x800276A0u)  return s_persp_terr;
    if (pc >= 0x8004EBA8u && pc < 0x8004F000u)  return s_persp_cyc;
    if (pc >= 0x800573C8u && pc < 0x80059474u)  return s_persp_part;
    if (pc >= 0x80059F8Cu && pc < 0x8005A470u)  return s_persp_shad;
    return s_persp_other;
}

void game_apply_runtime_quirks(CPUState* cpu) {
    (void)cpu;
    /* TODO Phase 1c — preamble fixes, known-bad-instruction workarounds. */
}

/* moby_pool_state diagnostic: dump per-iteration cone-test inputs at
 * 0x80022C0C for a target slot/frame range. Active iff
 * OPENPETE_DIAG_CONE_SLOT is set. Reads g_LevelMobys head pointer at
 * 0x80075828, computes slot = (fp - *g_LevelMobys) / 0x58, and only
 * logs when slot matches and frame is in [from,to]. The substitution
 * at code_generator.cpp 0x80022C0C calls openpete_diag_cone_dump after
 * computing the verdict — see render_channels.toml moby_pool_state. */
static int s_diag_cone_active = -1;
static int s_diag_cone_slot   = -1;
static int s_diag_cone_from   = 0;
static int s_diag_cone_to     = 0;

static void diag_cone_init(void) {
    if (s_diag_cone_active != -1) return;
    const char* slot_s = getenv("OPENPETE_DIAG_CONE_SLOT");
    if (!slot_s) { s_diag_cone_active = 0; return; }
    s_diag_cone_slot = atoi(slot_s);
    const char* from_s = getenv("OPENPETE_DIAG_CONE_FROM");
    const char* to_s   = getenv("OPENPETE_DIAG_CONE_TO");
    s_diag_cone_from = from_s ? atoi(from_s) : 0;
    s_diag_cone_to   = to_s   ? atoi(to_s)   : 0x7fffffff;
    s_diag_cone_active = 1;
    fprintf(stderr,
            "[diag-cone] slot=%d frames=[%d,%d]\n",
            s_diag_cone_slot, s_diag_cone_from, s_diag_cone_to);
}

/* §7.5 terrain-reproject validation probe (read-only). Per terrain
 * vertex: camera-free packed world (w0=X<<16|Y, w1=Z<<16|flags), vert
 * ptr, scaled campos (s7,t8,t9), + g_Camera.m_ViewMatrix (9 shorts
 * @0x80076DD0+0x14). Offline we compute M·(world-campos) per
 * r_environment.s and check parity vs Spyro's actual terrain output.
 * Env: OPENPETE_ENVVERT_LOG / _SKIP / _FRAMES. */
void openpete_envvert_probe(void* cpu_v, uint32_t w0, uint32_t w1,
                            uint32_t s1, uint32_t s7, uint32_t t8,
                            uint32_t t9) {
    static FILE*    s_ev_fp = NULL;
    static int      s_ev_init = 0;
    static uint32_t s_ev_skip = 0, s_ev_cap = 80, s_ev_lastf = 0xFFFFFFFFu,
                    s_ev_frames = 0;
    if (!s_ev_init) {
        s_ev_init = 1;
        const char* p = getenv("OPENPETE_ENVVERT_LOG");
        if (p && *p) {
            const char* sk = getenv("OPENPETE_ENVVERT_LOG_SKIP");
            if (sk && *sk) s_ev_skip = (uint32_t)atoi(sk);
            const char* fl = getenv("OPENPETE_ENVVERT_LOG_FRAMES");
            if (fl && *fl) s_ev_cap = (uint32_t)atoi(fl);
            s_ev_fp = fopen(p, "wb");
            if (s_ev_fp) {
                uint32_t h[8] = { 0x54525645u /*"EVRT"*/, 1u, 48u,
                                  0,0,0,0,0 };
                fwrite(h, sizeof(h), 1, s_ev_fp);
            }
        }
    }
    if (!s_ev_fp) return;
    if (openpete_frame < s_ev_skip) return;
    if (openpete_frame != s_ev_lastf) {
        if (s_ev_frames >= s_ev_cap) {
            fflush(s_ev_fp); fclose(s_ev_fp); s_ev_fp = NULL; return;
        }
        s_ev_lastf = openpete_frame;
        s_ev_frames++;
    }
    CPUState* cpu = (CPUState*)cpu_v;
    uint8_t b[48];
    memcpy(b +  0, &openpete_frame, 4);
    memcpy(b +  4, &s1, 4);
    memcpy(b +  8, &w0, 4);
    memcpy(b + 12, &w1, 4);
    memcpy(b + 16, &s7, 4);
    memcpy(b + 20, &t8, 4);
    memcpy(b + 24, &t9, 4);
    for (int k = 0; k < 9; k++) {              /* g_Camera.m_ViewMatrix */
        int16_t v = (int16_t)cpu->read_half(0x80076DD0u + 0x14u + k*2u);
        memcpy(b + 28 + k * 2, &v, 2);
    }
    int16_t pad = 0; memcpy(b + 46, &pad, 2);
    fwrite(b, 1, 48, s_ev_fp);
}

/* §7.5 re-validation callback. Read-only: emits one 80B record per
 * world (obj_tag==0) render-cache entry. Layout (v2):
 *   0  u32 frame      4  u32 sxy_key   8  u32 obj_tag
 *   12 i16 rt[9]      30 i16 v_local[3]
 *   36 i32 tr[3]      48 i32 mac[3]
 *   60 i32 campos[3]  72 u32 pad[2]    (=80) */
typedef struct { FILE* fp; uint32_t left; int32_t campos[3]; }
    openpete_wr_ud_t;

static void openpete_worldrt_dump_cb(
        const openpete_render_cache_entry_t* e, void* userdata) {
    openpete_wr_ud_t* ud = (openpete_wr_ud_t*)userdata;
    if (!ud || !ud->fp || ud->left == 0) return;
    if (e->obj_tag != 0u) return;            /* world / non-moby only */
    ud->left--;
    uint8_t r[80];
    memcpy(r +  0, &openpete_frame, 4);
    memcpy(r +  4, &e->sxy_key, 4);
    memcpy(r +  8, &e->obj_tag, 4);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            int16_t v = e->rt[i][j];
            memcpy(r + 12 + (i*3 + j) * 2, &v, 2);
        }
    memcpy(r + 30, e->v_local, 6);
    memcpy(r + 36, e->tr, 12);
    memcpy(r + 48, e->mac, 12);
    memcpy(r + 60, ud->campos, 12);
    uint32_t z[2] = {0,0}; memcpy(r + 72, z, 8);
    fwrite(r, 1, 80, ud->fp);
}

void openpete_diag_cone_dump(void* cpu_v,
                             int32_t mac1_abs,
                             int32_t mac3,
                             uint8_t native_pass) {
    diag_cone_init();
    if (!s_diag_cone_active) return;
    if ((int)openpete_frame < s_diag_cone_from
        || (int)openpete_frame > s_diag_cone_to) return;
    CPUState* cpu = (CPUState*)cpu_v;
    /* g_LevelMobys head pointer at 0x80075828 — *(uint32_t*) deref via
     * read_word. Compute slot = ($fp - head) / 0x58. */
    uint32_t head = cpu->read_word(0x80075828u);
    if (!head) return;
    uint32_t fp = cpu->fp;
    if (fp < head) return;
    uint32_t off = fp - head;
    if (off % 0x58u) return;
    int slot = (int)(off / 0x58u);
    if (s_diag_cone_slot >= 0 && slot != s_diag_cone_slot) return;
    /* Class is at offset 0x36 (lhu $s5, 0x36($fp) at 0x80022BD4). */
    uint16_t cls = cpu->read_half(fp + 0x36u);
    /* Position bytes (8-bit signed coords at 0x14/0x16/0x18 in PSX
     * convention, but Spyro mobys store world XYZ as int32 at 0x0c?
     * Just dump a few well-known fields that don't touch presentation. */
    int32_t pos_x = (int32_t)cpu->read_word(fp + 0x0Cu);
    int32_t pos_y = (int32_t)cpu->read_word(fp + 0x10u);
    int32_t pos_z = (int32_t)cpu->read_word(fp + 0x14u);
    /* MAC2 lives in gte_data[26]; useful for the Y-cone follow-up at
     * 0x80022C48..C68 which culls on `MAC3+403-3*|MAC2| <= 0`. */
    int32_t mac2 = (int32_t)cpu->gte_data[26];
    fprintf(stderr,
            "[diag-cone] f=%u slot=%d cls=0x%04x fp=0x%08x "
            "|MAC1|=%d MAC2=%d MAC3=%d 4*(|MAC1|-102)=%d 3*(MAC3+77)=%d "
            "verdict=%u pos=(%d,%d,%d)\n",
            openpete_frame, slot, cls, fp,
            mac1_abs, mac2, mac3,
            4 * (mac1_abs - 102),
            3 * (mac3 + 77),
            (unsigned)native_pass,
            pos_x, pos_y, pos_z);
}

/* --skip-to-level: load directly into a specific level instead of the
 * normal title-screen + Press-Start + save-file-picker flow.
 *
 * Strategy: don't fight the state machine — induce the same transition
 * the title's 990-tick demo timeout takes (overlays/titlescreen.c:190),
 * but seeded so it loads our target. The natural flow is:
 *   TSM_Init SubState 5 (timeout) -> m_Mode=TSM_Demo, m_State=TSS_Setup,
 *                                    m_DemoType=TSD_DemoLevel
 * TSS_Setup with TSD_DemoLevel (gamestates/update.c:636) then runs:
 *   func_8001277C();                     // Reset progress/lives/dragons
 *   levelId = g_DemoLevelIds[g_DemoIndex];
 *   g_LevelId = levelId; g_NextLevelId = levelId;
 * Followed by TSS_Loading -> LoadLevel(1) loop and TSS_Active flyby.
 *
 * So we just need to (a) overwrite g_DemoLevelIds[g_DemoIndex] with our
 * target, (b) force the same TSM_Init->TSM_Demo transition the timeout
 * path takes. The state machine handles state reset, level load, and
 * flyby intro for us — exactly what a real "Start the demo" path does.
 *
 * Hook point: TSM_Init SubState 3 (Press-Start wait, after the title's
 * own load + intro animation finish). Triggering earlier risks racing
 * the CD-load of titlescreen data; later (SubState 4/5) means the user
 * already pressed Start or the timeout already fired. */
static int s_skip_to_level    = 0;
static int s_skip_applied     = 0;

void game_set_skip_to_level(int level_id) {
    s_skip_to_level = level_id;
    s_skip_applied  = 0;
}

void game_on_hash_emit(uint32_t frame) {
    const char* df = getenv("OPENPETE_HASHTIME_CSF_FRAME");
    if (!df || frame != (uint32_t)atoi(df)) return;
    uint8_t* ram = openpete_psx_get_ram();

    const char* dp = getenv("OPENPETE_HASHTIME_CSF_DUMP");
    if (dp) {
        FILE* fp = fopen(dp, "w");
        if (fp) {
            for (uint32_t i = 0; i < 256; i++) {
                fprintf(fp, "%u,%u\n", i, ram[(0x800771C8u + i) & 0x1FFFFFFFu]);
            }
            fclose(fp);
        }
    }

    const char* rp = getenv("OPENPETE_HASHTIME_RAM_DUMP");
    if (rp) {
        FILE* fp = fopen(rp, "wb");
        if (fp) {
            fwrite(ram, 1, 2u * 1024u * 1024u, fp);
            fclose(fp);
        }
    }

    const char* mp = getenv("OPENPETE_HASHTIME_MWD_DUMP");
    if (mp) {
        FILE* fp = fopen(mp, "w");
        if (fp) {
            uint32_t lvl_ptr = *(uint32_t*)(ram + (0x80075828u & 0x1FFFFFFFu));
            uint32_t dyn_ptr = *(uint32_t*)(ram + (0x80075890u & 0x1FFFFFFFu));
            /* Per-byte pool dump: each row "pool,idx,offset,byte". Set
             * OPENPETE_HASHTIME_MWD_BYTES=1 to dump every byte; otherwise
             * dump only m_WasDrawn (offset 0x51). */
            int dump_bytes = (getenv("OPENPETE_HASHTIME_MWD_BYTES") != NULL);
            if (dump_bytes) {
                fprintf(fp, "pool,idx,offset,byte\n");
            } else {
                fprintf(fp, "pool,idx,m_WasDrawn\n");
            }
            if (lvl_ptr) {
                for (uint32_t i = 0; i < 512; i++) {
                    if (dump_bytes) {
                        for (uint32_t b = 0; b < 0x58u; b++) {
                            uint32_t off = (lvl_ptr + i * 0x58u + b) & 0x1FFFFFFFu;
                            fprintf(fp, "L,%u,%u,%u\n", i, b, ram[off]);
                        }
                    } else {
                        uint32_t off = (lvl_ptr + i * 0x58u + 0x51u) & 0x1FFFFFFFu;
                        fprintf(fp, "L,%u,%u\n", i, ram[off]);
                    }
                }
            }
            if (dyn_ptr) {
                for (uint32_t i = 0; i < 128; i++) {
                    if (dump_bytes) {
                        for (uint32_t b = 0; b < 0x70u; b++) {
                            uint32_t off = (dyn_ptr + i * 0x70u + b) & 0x1FFFFFFFu;
                            fprintf(fp, "D,%u,%u,%u\n", i, b, ram[off]);
                        }
                    } else {
                        uint32_t off = (dyn_ptr + i * 0x70u + 0x51u) & 0x1FFFFFFFu;
                        fprintf(fp, "D,%u,%u\n", i, ram[off]);
                    }
                }
            }
            fclose(fp);
        }
    }
}

void game_on_frame_pre(CPUState* cpu, uint32_t frame_counter) {
    (void)frame_counter;

    /* Install env-var write-watcher on the first frame, after the
     * runtime has wired cpu->write_byte/half/word. */
    static int s_watch_init_done = 0;
    if (!s_watch_init_done) {
        s_watch_init_done = 1;
        /* Register the PGXP per-prim-class classifier here — first
         * non-trivial frame hook that fires (game_apply_runtime_quirks
         * is declared but unwired). */
        openpete_pgxp_classify_fn = spyro_pgxp_classify;
        install_watch_from_env(cpu);
        /* Register the static UI-moby range. g_Hud.m_Mobys[12] lives
         * at PSX vaddr 0x80077FEC (g_Hud at 0x80077FA8 + offset 0x44),
         * sizeof(Moby)=0x58, so the range is
         *   [0x80077FEC, 0x80077FEC + 12*0x58 = 0x8007840C).
         * Used by openpete_ui_moby_is_current_ui to classify the
         * moby being projected by Spyro's shaded-moby renderer. */
        openpete_ui_moby_set_static_range(0x80077FECu,
                                          0x80077FECu + 12u * 0x58u);
    }


    /* --skip-to-level: induce the title's demo-timeout transition early,
     * pointed at our target via g_DemoLevelIds[g_DemoIndex], and clamp
     * g_DemoMode = DEMO_MODE_NONE so the user controls Spyro instead of
     * the AI. (TSD_DemoLevel sets g_DemoMode = PLAY at tick 384 of the
     * flyby; that's the *only* thing that distinguishes "watch demo"
     * from "actually play" — the same TSM_Demo + LoadLevel + flyby
     * sequence runs when a real player picks "New Game".) Using
     * TSD_DemoLevel (vs TSD_Level) gets us func_8001277C's free
     * progress/lives/health reset, which a freshly-booted TSD_Level
     * skip would otherwise miss.
     *
     * Addresses (spyro-1/manifest/globals.toml):
     *   g_Gamestate         = 0x800757D8  (GS_TitleScreen = 13,
     *                                      GS_Playing = 0)
     *   g_DemoMode          = 0x80075714  (clamp to 0 each frame)
     *   g_DemoLevelIds      = 0x8006EE7C  (int[] in .data)
     *   g_DemoIndex         = 0x800757B8
     *   g_TitlescreenState  = 0x80078D78
     *     +0x00 m_Mode      (TSM_Init=0, TSM_Demo=3)
     *     +0x04 m_State     (TSS_Setup=0)
     *     +0x10 m_SubState  (TSM_Init's =3 means "Press Start" wait)
     *     +0x1C m_DemoType  (TSD_DemoLevel=2)
     */
    if (s_skip_to_level > 0) {
        if (!s_skip_applied) {
            uint32_t gs       = cpu->read_word(0x800757D8u);
            uint32_t tsm_mode = cpu->read_word(0x80078D78u + 0x00u);
            uint32_t tsm_sub  = cpu->read_word(0x80078D78u + 0x10u);
            if (gs == 13u && tsm_mode == 0u /*TSM_Init*/ && tsm_sub == 3u) {
                uint32_t demo_idx = cpu->read_word(0x800757B8u);
                uint32_t slot     = 0x8006EE7Cu + (demo_idx * 4u);
                cpu->write_word(slot, (uint32_t)s_skip_to_level);
                cpu->write_word(0x80078D78u + 0x00u, 3u); /* m_Mode = TSM_Demo */
                cpu->write_word(0x80078D78u + 0x04u, 0u); /* m_State = TSS_Setup */
                cpu->write_word(0x80078D78u + 0x1Cu, 2u); /* m_DemoType = TSD_DemoLevel */
                s_skip_applied = 1;
                fprintf(stderr,
                        "[skip-to-level] induced demo-transition: level=%d "
                        "(slot=%u) at frame=%u\n",
                        s_skip_to_level, (unsigned)demo_idx,
                        (unsigned)frame_counter);
            }
        } else {
            /* After TSS_Setup runs, downgrade m_DemoType from TSD_DemoLevel
             * to TSD_Level. TSS_Setup uses TSD_DemoLevel to call
             * func_8001277C() (free progress/lives reset), but the
             * tick-384 hand-off code (gamestates/update.c:770) gates
             * `g_DemoMode = DEMO_MODE_PLAY` on TSD_DemoLevel — leaving it
             * set means LoadLevel's stage-13 pass runs the demo-input
             * playback path (loaders.c:672). Switching DemoType to
             * TSD_Level once Setup is done sidesteps both. Belt-and-
             * suspenders: also clamp g_DemoMode=0 while we're still on
             * the title-screen overlay, in case any path we missed
             * sneaks PLAY through. */
            uint32_t gs     = cpu->read_word(0x800757D8u);
            uint32_t tsm_st = cpu->read_word(0x80078D78u + 0x04u);
            if (gs == 13u /*GS_TitleScreen*/) {
                if (tsm_st != 0u /* past TSS_Setup */) {
                    cpu->write_word(0x80078D78u + 0x1Cu, 1u); /* TSD_Level */
                }
                cpu->write_word(0x80075714u, 0u); /* DEMO_MODE_NONE */
            }
        }
    }
}

/* --- G2 (transform-interp §7): moby slot/address-reuse probe ---------
 * Read-only. Each canonical game tick (this hook fires once per tick,
 * after the fiber yields with consistent RAM — run.c:1321), walk
 * g_LevelMobys and dump {tick, frame, addr, m_Class, m_State,
 * AnimationState, m_Position} per live moby. The G2 question: within
 * the 1–2-frame lag window, does a slot/address get reused by a
 * different logical object such that {addr, m_Class, liveness} reads
 * as false continuity? Analyzed offline.
 *
 * Offsets anchored from the instruction-verified m_Class@+0x36
 * (lhu $s5,0x36($fp) @ 0x80022BD4) + moby.h declaration order:
 *   +0x0C m_Position (i32 x/y/z)   +0x36 m_Class (i16)
 *   +0x3C m_AnimationState (m_Animation,_,m_Frame,m_NextFrame,
 *          m_FrameProgress @ +0x3C..+0x40)   +0x48 m_State (u8)
 * stride 0x58, terminator m_State==0xFF (decomp loaders.c:696).
 * Gate: OPENPETE_G2_LOG=path  OPENPETE_G2_LOG_SKIP=frame
 *       OPENPETE_G2_LOG_TICKS=N (self-closes => clean file under timeout)
 */
extern unsigned openpete_frame;
static FILE*    s_g2_fp        = NULL;
static int      s_g2_init      = 0;
static uint32_t s_g2_skip      = 0;
static uint32_t s_g2_tick_cap  = 3000;
static uint32_t s_g2_ticks     = 0;
static uint32_t s_g2_last_tick = 0xFFFFFFFFu;

void game_on_frame(CPUState* cpu, uint32_t frame_counter) {
    (void)frame_counter;

    /* --- G3: per-tick g_Camera log (independent of G2; must run
     * BEFORE any G2 early-return). g_Camera @ 0x80076DD0 (camera.h,
     * SHORTMATRIX=0x14): m_ViewMatrix @ +0x14 (9 shorts raw),
     * m_Position @ +0x28 (3 i32), m_Rotation @ +0x4C (3 i16),
     * m_State @ +0x58 (u32). Env OPENPETE_CAM_LOG / _SKIP / _TICKS. */
    {
        static FILE* s_cam_fp = NULL;
        static int   s_cam_init = 0;
        static uint32_t s_cam_skip = 0, s_cam_cap = 250, s_cam_n = 0;
        if (!s_cam_init) {
            s_cam_init = 1;
            const char* p = getenv("OPENPETE_CAM_LOG");
            if (p && *p) {
                const char* sk = getenv("OPENPETE_CAM_LOG_SKIP");
                if (sk && *sk) s_cam_skip = (uint32_t)atoi(sk);
                const char* tc = getenv("OPENPETE_CAM_LOG_TICKS");
                if (tc && *tc) s_cam_cap = (uint32_t)atoi(tc);
                s_cam_fp = fopen(p, "wb");
                if (s_cam_fp) {
                    uint32_t h[8] = { 0x4C4D4143u /*"CAML"*/, 1u, 48u,
                                      0,0,0,0,0 };
                    fwrite(h, sizeof(h), 1, s_cam_fp);
                }
            }
        }
        if (s_cam_fp && openpete_frame >= s_cam_skip) {
            if (s_cam_n >= s_cam_cap) {
                fflush(s_cam_fp); fclose(s_cam_fp); s_cam_fp = NULL;
            } else {
                s_cam_n++;
                uint32_t C = 0x80076DD0u;
                uint8_t cb[48];
                uint32_t lt = cpu->read_word(0x800758C8u);
                memcpy(cb + 0, &openpete_frame, 4);
                memcpy(cb + 4, &lt, 4);
                for (int k = 0; k < 9; k++) {
                    int16_t v = (int16_t)cpu->read_half(C + 0x14u + k*2u);
                    memcpy(cb + 8 + k * 2, &v, 2);
                }
                for (int k = 0; k < 3; k++) {
                    int32_t v = (int32_t)cpu->read_word(C + 0x28u + k*4u);
                    memcpy(cb + 26 + k * 4, &v, 4);
                }
                for (int k = 0; k < 3; k++) {
                    int16_t v = (int16_t)cpu->read_half(C + 0x4Cu + k*2u);
                    memcpy(cb + 38 + k * 2, &v, 2);
                }
                uint32_t cstate = cpu->read_word(C + 0x58u);
                memcpy(cb + 44, &cstate, 4);
                fwrite(cb, 1, 48, s_cam_fp);
            }
        }
    }

    /* --- §7.5 re-validation: world render-cache RT/TR/mac dump.
     * Read-only. Iterates the curr render-cache generation and writes
     * one 64B record per world (obj_tag==0) entry, capped per frame.
     * Offline analysis answers the decisive question: does cached `rt`
     * for world entries VARY frame-to-frame (⇒ rt = live view matrix,
     * §7.5's "static cached RT" premise is falsified ⇒ world IS
     * reprojectable at the cache layer) or is it constant (⇒ §7.5
     * stands). Env OPENPETE_WORLDRT_LOG / _SKIP / _FRAMES / _CAP. */
    {
        static FILE* s_wr_fp = NULL;
        static int   s_wr_init = 0;
        static uint32_t s_wr_skip = 0, s_wr_cap = 120,
                        s_wr_perfr = 96, s_wr_n = 0;
        if (!s_wr_init) {
            s_wr_init = 1;
            const char* p = getenv("OPENPETE_WORLDRT_LOG");
            if (p && *p) {
                const char* sk = getenv("OPENPETE_WORLDRT_LOG_SKIP");
                if (sk && *sk) s_wr_skip = (uint32_t)atoi(sk);
                const char* fl = getenv("OPENPETE_WORLDRT_LOG_FRAMES");
                if (fl && *fl) s_wr_cap = (uint32_t)atoi(fl);
                const char* pc = getenv("OPENPETE_WORLDRT_LOG_CAP");
                if (pc && *pc) s_wr_perfr = (uint32_t)atoi(pc);
                s_wr_fp = fopen(p, "wb");
                if (s_wr_fp) {
                    uint32_t h[8] = { 0x30545257u /*"WRT0"*/, 2u, 80u,
                                      0,0,0,0,0 };
                    fwrite(h, sizeof(h), 1, s_wr_fp);
                }
            }
        }
        if (s_wr_fp && openpete_frame >= s_wr_skip) {
            if (s_wr_n >= s_wr_cap) {
                fflush(s_wr_fp); fclose(s_wr_fp); s_wr_fp = NULL;
            } else {
                s_wr_n++;
                openpete_wr_ud_t ud;
                ud.fp = s_wr_fp; ud.left = s_wr_perfr;
                for (int k = 0; k < 3; k++)   /* g_Camera.m_Position */
                    ud.campos[k] =
                        (int32_t)cpu->read_word(0x80076DD0u + 0x28u + k*4u);
                openpete_render_cache_for_each_curr_entry(
                    openpete_worldrt_dump_cb, &ud);
            }
        }
    }

    if (!s_g2_init) {
        s_g2_init = 1;
        const char* p = getenv("OPENPETE_G2_LOG");
        if (p && *p) {
            const char* sk = getenv("OPENPETE_G2_LOG_SKIP");
            if (sk && *sk) s_g2_skip = (uint32_t)atoi(sk);
            const char* tc = getenv("OPENPETE_G2_LOG_TICKS");
            if (tc && *tc) s_g2_tick_cap = (uint32_t)atoi(tc);
            s_g2_fp = fopen(p, "wb");
            if (s_g2_fp) {
                uint32_t h[8] = { 0x47324C47u /*"GL2G"*/, 1u, 32u,
                                  0,0,0,0,0 };
                fwrite(h, sizeof(h), 1, s_g2_fp);
                fflush(s_g2_fp);
            }
        }
    }
    if (!s_g2_fp) return;
    if (openpete_frame < s_g2_skip) return;
    (void)s_g2_last_tick;
    /* game_on_frame is ALREADY once-per-canonical-tick (run.c:1321,
     * ticked_this_iter), so no dedup needed. */
    if (s_g2_ticks >= s_g2_tick_cap) {                     /* self-close */
        fflush(s_g2_fp); fclose(s_g2_fp); s_g2_fp = NULL; return;
    }
    s_g2_ticks++;

    uint32_t tick = cpu->read_word(0x800758C8u);           /* g_LevelTicks */
    uint32_t gs   = cpu->read_word(0x800757D8u);           /* g_Gamestate */
    uint32_t base = cpu->read_word(0x80075828u);           /* g_LevelMobys */
    if (!base) return;                                     /* no moby pool yet */

    for (uint32_t i = 0; i < 4096u; i++) {
        uint32_t m = base + i * 0x58u;
        uint8_t st = cpu->read_byte(m + 0x48u);            /* m_State */
        if (st == 0xFFu) break;                            /* list terminator */
        int16_t  cls = (int16_t)cpu->read_half(m + 0x36u);
        int32_t  px  = (int32_t)cpu->read_word(m + 0x0Cu);
        int32_t  py  = (int32_t)cpu->read_word(m + 0x10u);
        int32_t  pz  = (int32_t)cpu->read_word(m + 0x14u);
        uint8_t  an  = cpu->read_byte(m + 0x3Cu);          /* m_Animation */
        uint8_t  fr  = cpu->read_byte(m + 0x3Eu);          /* m_Frame */
        uint8_t  nf  = cpu->read_byte(m + 0x3Fu);          /* m_NextFrame */
        uint8_t  fp  = cpu->read_byte(m + 0x40u);          /* m_FrameProgress */
        uint8_t  buf[32];
        memcpy(buf +  0, &tick, 4);
        memcpy(buf +  4, &openpete_frame, 4);
        memcpy(buf +  8, &m,    4);
        memcpy(buf + 12, &px,   4);
        memcpy(buf + 16, &py,   4);
        memcpy(buf + 20, &pz,   4);
        memcpy(buf + 24, &cls,  2);
        buf[26] = st; buf[27] = an; buf[28] = fr;
        buf[29] = nf; buf[30] = fp; buf[31] = (uint8_t)gs;
        fwrite(buf, 1, 32, s_g2_fp);
    }
}

/* Each Spyro overlay's first word at 0x8007AA38 is its id (LEVEL_BALLOON
 * burns LONG(id) at +0x00). After a save-state load, the previously-active
 * overlay tracker (host-side state in dispatch_chain.c) still points at
 * whichever overlay was live BEFORE the load — re-read the marker word
 * from restored RAM and re-sync the dispatcher. Without this, calls into
 * overlay-resident vaddrs route to the wrong dispatcher and the game
 * hangs spinning on missing functions. */
extern void spyro_set_active_overlay_by_magic(uint32_t magic);

void game_on_state_loaded(CPUState* cpu) {
    uint32_t magic = cpu->read_word(0x8007AA38u);
    spyro_set_active_overlay_by_magic(magic);
}

/* g_Spyro: 0x80078A58, first field is m_Position (Vector3D x/y/z, 3×i32).
 * The fixed addr is fine because all Spyro 1 versions share the linker
 * map; if we ever support a second exe revision this'll need a per-rev
 * lookup. Returns non-zero before the EXE's BSS init has populated the
 * struct (host-side check uses fast-boot gate, but we sanity-check the
 * address range here too in case fast-boot misfires). */
extern uint8_t* openpete_psx_get_ram(void);
int game_get_player_position(int32_t* out_x, int32_t* out_y, int32_t* out_z) {
    uint8_t* ram = openpete_psx_get_ram();
    if (!ram) return -1;
    const uint32_t base = 0x80078A58u - 0x80000000u;
    int32_t x, y, z;
    memcpy(&x, ram + base + 0, 4);
    memcpy(&y, ram + base + 4, 4);
    memcpy(&z, ram + base + 8, 4);
    *out_x = x;
    *out_y = y;
    *out_z = z;
    return 0;
}

/* --- Disc layout --- */

static const game_disc_layout_t k_disc_layout = {
    .wad_sector      = 37,            /* Spyro 1 stores WAD.WAD at sector 37 */
    .wad_header_addr = 0x80100000u,   /* TODO Phase 1c — confirm against decomp */
};

const game_disc_layout_t* game_get_disc_layout(void) {
    return &k_disc_layout;
}

/* See docs/missing-geometry-bug.md — Option B (recomp self-instrumentation).
 * On entry to func_80023AC4 (Spyro's renderer, hand-written asm), wrap
 * cpu->write_word with a frame-gated tracer to capture the prim-buffer
 * write stream and diff bad vs good frames. The depth flag prevents the
 * recursive call into the function body from re-entering this branch. */
extern void func_80023AC4(CPUState* cpu);
extern void openpete_trace_writes_begin(CPUState* cpu);
extern void openpete_trace_writes_end(CPUState* cpu);

/* AllocateBuffers (4BEF8.c) post-call hook target — relocate the poly
 * buffer to a virtualized region when widescreen has set one up. See
 * notes/plans/openpete-widescreen-fov.md item 2. */
extern void func_8005B6F8(CPUState* cpu);

/* (widescreen-related extern decls + env-renderer instrumentation
 * moved to games/spyro-1/features/widescreen.c — see Phase 2.1b in
 * notes/plans/openpete-feature-modules.md.) */
extern float    openpete_disp_w_factor;  /* still consumed by gem-trace below */

/* Phase 3/4 — native-decomp dispatch table. Generated from
 * manifest/native.toml by tools/gen_dispatch.py; see
 * generated/native_dispatch.c. Each [[dispatch]] entry there names a
 * decomp C function that takes over its PSX vaddr; the generator
 * emits the cpu-stash, psyq___main bootstrap (for main), arg unpacking
 * from a0..a3/stack, and return code. To add another entry point,
 * extend native.toml and re-run scripts/regen-manifest.sh. */
extern int spyro_native_dispatch(CPUState* cpu, uint32_t addr);

int psx_override_dispatch_game(CPUState* cpu, uint32_t addr) {
    if (spyro_native_dispatch(cpu, addr)) {
        return 1;
    }

    /* Feature modules registered via core/include/feature_registry.h.
     * Phase 1 of notes/plans/openpete-feature-modules.md: registry
     * exists but no features are migrated yet, so this is a no-op
     * (openpete_feature_count == 0). Phase 2 moves the widescreen
     * dispatch arms below into core/features/widescreen.c and they
     * start firing through here instead. */
    if (feature_registry_dispatch(cpu, addr)) {
        return 1;
    }

    /* Widescreen dispatch arms (0x8001ED5C render bracket, 0x8007CD38
     * titlescreen sprite, 0x800258F0 env renderer, 0x8004EBA8 cyclorama,
     * 0x80022A2C text-moby range) moved to
     * games/spyro-1/features/widescreen.c — handled by the registry call
     * above. See Phase 2.1b in notes/plans/openpete-feature-modules.md. */
    /* Diagnostic: trace bucket-2 (gems) cull. Walks D_8006FCF4+0x2400 list,
     * computes per-moby cube + MVMVA cone test the same way the inline
     * func_80022A2C cull does, prints which gems pass / fail. Used only
     * with OPENPETE_GEM_TRACE=1 to debug widescreen gem visibility. */
    static int s_in_a2c = 0;
    if (addr == 0x80022A2Cu && !s_in_a2c
        && getenv("OPENPETE_GEM_TRACE")
        && (openpete_frame == 3000u || openpete_frame == 2999u)) {
        s_in_a2c = 1;
        const uint32_t D = 0x8006FCF4u;
        const uint32_t pm = 0x80076DD0u;
        int32_t cam_x = (int32_t)cpu->read_word(pm + 0x28u);
        int32_t cam_y = (int32_t)cpu->read_word(pm + 0x2Cu);
        int32_t cam_z = (int32_t)cpu->read_word(pm + 0x30u);
        int16_t R11 = (int16_t)cpu->read_half(pm + 0x0u);
        int16_t R12 = (int16_t)cpu->read_half(pm + 0x2u);
        int16_t R13 = (int16_t)cpu->read_half(pm + 0x4u);
        int16_t R31 = (int16_t)cpu->read_half(pm + 0xCu);
        int16_t R32 = (int16_t)cpu->read_half(pm + 0xEu);
        int16_t R33 = (int16_t)cpu->read_half(pm + 0x10u);
        fprintf(stderr, "[gem-trace f=%u cam=(%d,%d,%d) R0=(%d,%d,%d) R2=(%d,%d,%d)]\n",
                openpete_frame, cam_x, cam_y, cam_z, R11, R12, R13, R31, R32, R33);
        for (int i = 0; i < 96; i++) {
            uint32_t mp = cpu->read_word(D + 0x2400u + (uint32_t)(i * 4));
            if (mp == 0) break;
            uint16_t cls = cpu->read_half(mp + 0x36u);
            if (cls < 83 || cls > 87) continue;
            int32_t mx = (int32_t)cpu->read_word(mp + 0xCu);
            int32_t my = (int32_t)cpu->read_word(mp + 0x10u);
            int32_t mz = (int32_t)cpu->read_word(mp + 0x14u);
            int32_t dx = mx - cam_x;
            int32_t dy = my - cam_y;
            int32_t dz = mz - cam_z;
            /* Match asm: at = dx/4, v0 = -dy/4, v1 = -dz/4 */
            int32_t a_t = dx >> 2;
            int32_t v0 = -dy >> 2;  // arithmetic shift of negation
            int32_t v1 = -dz >> 2;
            /* Render distance from byte 0x50 */
            uint8_t flag_lo = (uint8_t)cpu->read_byte(mp + 0x50u);
            int32_t a0 = (int32_t)((int8_t)flag_lo) << 8;
            /* Cube test */
            int cube_pass = 1;
            if (a_t + a0 <= 0 || a_t - a0 >= 0) cube_pass = 0;
            if (v0 + a0 <= 0 || v0 - a0 >= 0) cube_pass = 0;
            if (v1 + a0 <= 0 || v1 - a0 >= 0) cube_pass = 0;
            /* MVMVA cone test inputs: IR1=v0, IR2=v1, IR3=at */
            int64_t MAC1 = (int64_t)R11 * v0 + (int64_t)R12 * v1 + (int64_t)R13 * a_t;
            int64_t MAC3 = (int64_t)R31 * v0 + (int64_t)R32 * v1 + (int64_t)R33 * a_t;
            int64_t abs_x = MAC1 < 0 ? -MAC1 : MAC1;
            int64_t cone_x = 4 * (abs_x - 102) - 3 * (MAC3 + 77);
            fprintf(stderr,
                    "  moby 0x%08X cls=%u dxyz=(%d,%d,%d) cube=%s "
                    "MAC1=%lld MAC3=%lld coneX_a1=%lld %s\n",
                    mp, cls, dx, dy, dz, cube_pass ? "PASS" : "CULL",
                    (long long)MAC1, (long long)MAC3, (long long)cone_x,
                    cone_x >= 0 ? "CULL" : "PASS");
        }
        s_in_a2c = 0;
        /* fall through to call original */
    }

    static int s_in_traced_call = 0;
    if (addr == 0x80023AC4u && !s_in_traced_call) {
        s_in_traced_call = 1;
        openpete_trace_writes_begin(cpu);
        func_80023AC4(cpu);
        openpete_trace_writes_end(cpu);
        s_in_traced_call = 0;
        return 1;
    }

    /* AllocateBuffers post-call polybuf-redirect hook (Option B).
     * AllocateBuffers (4BEF8.c, PC 0x8005B6F8) sets up the polybuf +
     * OT layout in canonical 2 MB. When widescreen has registered a
     * host-backed polybuf region via widescreen_polybuf_setup(), let
     * the original run (so the canonical anchor + OT/scene pointers
     * stay correct), then redirect only the per-frame write cursors
     * (`g_DB[0/1].m_PolyBuf`) into the host-backed region.
     *
     * `m_LowerPolyBuffer` / `m_HigherPolyBuffer` are deliberately
     * NOT overwritten — they remain canonical and serve as the layout
     * anchor for transient scratch zones (`m_LowerPolyBuffer -
     * 0x40000` titlescreen image, cyclorama dest, memcard scratch,
     * flight image stash, balloonist bounds check). Moving them was
     * the bug behind the parked Step 3c crash. The `D_800757B0 =
     * db->m_PolyBuf` assignment at draw.c:2664 propagates the
     * redirected cursor automatically every frame; the
     * `psx_runtime_polybuf_size` substitutions at the
     * `D_80075780 = ... + 0x1C000` sites cover the end-cursor.
     *
     * See notes/plans/openpete-polybuf-host-redirect.md for the
     * two-pointer separation rationale. */
    static int s_in_alloc_buffers = 0;
    if (addr == 0x8005B6F8u && widescreen_polybuf_active()
        && !s_in_alloc_buffers) {
        s_in_alloc_buffers = 1;
        func_8005B6F8(cpu);
        /* Capture the canonical per-slot poly-buffer bases AllocateBuffers
         * just wrote, before we overwrite them with the host-relocated
         * ones. psx_runtime_polybuf_to_canon() needs these to keep
         * cursor-derived gameplay pointers (g_HudMobys, D_800756FC) at
         * their hardware value. */
        widescreen_polybuf_set_canonical(cpu->read_word(0x80076F50u),
                                         cpu->read_word(0x80076FD4u));
        uint32_t lo = widescreen_polybuf_lower_vaddr();
        uint32_t hi = widescreen_polybuf_higher_vaddr();
        cpu->write_word(0x80076F50u, lo);  /* g_DB[0].m_PolyBuf           */
        cpu->write_word(0x80076FD4u, hi);  /* g_DB[1].m_PolyBuf           */
        s_in_alloc_buffers = 0;
        return 1;
    }

    return 0;
}
