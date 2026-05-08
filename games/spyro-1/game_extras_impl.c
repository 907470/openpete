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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* g_SonyImage / D_8006FCF4 base. Default == canonical PSX address; the
 * recompiler's lui+addiu pair substitution is then identity. --no-lod
 * (or any other env-renderer scratch-pool consumer) will swing this to
 * a relocated region in openpete-only vaddr space. Lives in this game-
 * specific TU because core/ is not allowed to encode raw Spyro
 * addresses (see openpete_core_purity_check). */
uint32_t psx_runtime_g_sony_image_base = 0x8006FCF4u;

/* Default queue-head sub-offsets — match the canonical g_SonyImage
 * struct so the substituted addi sites in r_environment.s land at
 * the same bytes as before. --no-lod boot will swing these past
 * the canonical 0x31A0 splash range. */
uint32_t psx_runtime_env_hp_queue_offset  = 0x1C00u;
uint32_t psx_runtime_env_mid_queue_offset = 0x1E00u;
uint32_t psx_runtime_env_lp_queue_offset  = 0x2000u;

/* r_moby.s LP-face-load offset. Canonical 0x1C reads m_LpFaces (low-poly
 * face list at AnimationHeader +0x1C). --no-lod swings to 0x14 so the
 * LP path reads m_Faces (high-poly) instead. Substituted at the two
 * `lw $fp, 0x1C($ra)` sites in r_moby.s (PCs 0x8001FE9C, 0x80021674). */
uint32_t psx_runtime_moby_lp_face_offset = 0x1Cu;

/* --no-lod region layout. Allocated and registered in game_apply_no_lod()
 * once the canonical splash bytes have been populated; remains live for
 * the rest of the run.
 *
 *   region base = 0x80300000       (openpete-only vaddr, above 2 MB)
 *   region size = 0x40000 (256 KB)
 *
 * Sub-offsets:
 *   +0x00000..+0x031A0  — mirror of canonical g_SonyImage / D_8006FCF4
 *                          (initialized by copy from g_ram at boot, then
 *                           every read+write to the splash struct lands
 *                           here via the value-keyed lui+addiu redirect).
 *   +0x031A0..+0x40000  — reserved/scratch (~243 KB).
 *   +0x40000            — grown HP queue head (psx_runtime_env_hp_queue_offset).
 *   +0x44000            — grown mid queue head (psx_runtime_env_mid_queue_offset).
 *   +0x48000            — grown LP queue head (psx_runtime_env_lp_queue_offset).
 *                          ...wait, 0x48000 > region size 0x40000.
 *
 * Resolution: since each queue is NULL-terminated and 4096 entries × 4 B
 * = 16 KB, three queues need 48 KB total past the 12.5 KB splash mirror.
 * Region size 0x40000 (256 KB) is plenty; queue offsets 0x40000 / 0x44000 /
 * 0x48000 fit within. But the offsets above end at 0x4C000, requiring a
 * region size of at least 0x4C000. We round to 0x80000 (512 KB) for slack.
 */
/* Queue-head offsets relative to canonical g_SonyImage base. Each addi
 * substitution computes
 *   $rt = canonical_pool_base + psx_runtime_env_{hp,mid,lp}_queue_offset
 * The composed address must land outside canonical 2 MB so a registered
 * region can claim it without overlapping g_ram. With g_SonyImage at
 * 0x8006FCF4, offsets ≥ 0x190_30C produce absolute vaddrs ≥ 0x80200000.
 * We pick offsets that put the region at a clean openpete-only base
 * (0x80300000): HP_OFFSET = 0x80300000 - 0x8006FCF4 = 0x29030C. */
#define NO_LOD_REGION_VADDR       0x80300000u
#define G_SONY_IMAGE_VADDR        0x8006FCF4u
#define NO_LOD_HP_QUEUE_OFFSET    (NO_LOD_REGION_VADDR - G_SONY_IMAGE_VADDR + 0x0000u)
#define NO_LOD_MID_QUEUE_OFFSET   (NO_LOD_REGION_VADDR - G_SONY_IMAGE_VADDR + 0x4000u)
#define NO_LOD_LP_QUEUE_OFFSET    (NO_LOD_REGION_VADDR - G_SONY_IMAGE_VADDR + 0x8000u)

/* Single backing buffer for the three grown queues (4096 entries × 4 B
 * = 16 KB per queue). 48 KB total. */
#define NO_LOD_REGION_SIZE        0xC000u
static uint8_t s_no_lod_region[NO_LOD_REGION_SIZE];

int game_apply_no_lod(void) {
    /* Strategy: leave g_SonyImage base at canonical 0x8006FCF4 — every
     * base reference (lui+addiu D_8006FCF4) resolves to canonical RAM,
     * so the splash bitmap bytes stay pristine and any path that reads
     * those bytes (including the original game's pause-menu, which
     * incidentally reads sub-offset 0x330 as moby data) gets the same
     * bytes it would on hardware.
     *
     * Only the *grown queues* are relocated. The 11 queue-head addi
     * substitutions in r_environment.s compute
     *   $rt = canonical_pool_base + psx_runtime_env_{hp|mid|lp}_queue_offset
     * Setting the offset to 0x40000 / 0x44000 / 0x48000 lands the queue
     * heads at 0x800AFCF4 + 0x{40000|44000|48000} = openpete-only vaddr
     * space. We register a single region at that base with enough size
     * to cover all three queues (each 16 KB, plus slack).
     *
     * This avoids the channel-leak that the prior "redirect base" design
     * exposed: when the base was redirected, some paths cleared the
     * relocated region's m_Buf (substituted), but other paths read
     * canonical splash bytes. The substituted reads got zeros; canonical
     * reads got splash bytes; downstream interpreted differently. With
     * the base un-relocated, that whole class of bugs vanishes. */

    /* Region at NO_LOD_REGION_VADDR holds the three grown queues:
     *   HP head  at region+0x0000 (vaddr NO_LOD_REGION_VADDR)
     *   mid head at region+0x4000
     *   LP head  at region+0x8000
     * Each queue is 16 KB (4096 entries × 4 B), NULL-terminated. */
    memset(s_no_lod_region, 0, sizeof(s_no_lod_region));

    int rc = psx_runtime_register_region(
        NO_LOD_REGION_VADDR, NO_LOD_REGION_SIZE, s_no_lod_region);
    if (rc != 0) {
        fprintf(stderr, "[no-lod] psx_runtime_register_region failed rc=%d "
                "(base=0x%08X size=0x%X)\n", rc, NO_LOD_REGION_VADDR,
                NO_LOD_REGION_SIZE);
        return 1;
    }

    psx_runtime_env_hp_queue_offset  = NO_LOD_HP_QUEUE_OFFSET;
    psx_runtime_env_mid_queue_offset = NO_LOD_MID_QUEUE_OFFSET;
    psx_runtime_env_lp_queue_offset  = NO_LOD_LP_QUEUE_OFFSET;
    /* Leave psx_runtime_g_sony_image_base at canonical (0x8006FCF4). */

    /* Phase 4: redirect r_moby.s LP-face load to read m_Faces (HP). */
    psx_runtime_moby_lp_face_offset = 0x14u;

    fprintf(stderr,
            "[no-lod] grown env queues at base=0x%08X size=%u KB "
            "(HP=+0x%X mid=+0x%X LP=+0x%X from g_SonyImage)\n",
            NO_LOD_REGION_VADDR, NO_LOD_REGION_SIZE / 1024u,
            NO_LOD_HP_QUEUE_OFFSET,
            NO_LOD_MID_QUEUE_OFFSET,
            NO_LOD_LP_QUEUE_OFFSET);
    return 0;
}

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

    /* --no-lod sector-LOD collapse. Override m_LodDistance to a value
     * the env-renderer's `dist >= m_LodDistance` predicate can never
     * trip, so every visible cell takes the high-poly path. The pool
     * relocation (game_apply_no_lod) gives the HP queue ~16 KB of
     * physical bytes (4096 entries) instead of the canonical 256, so
     * the inflated HP load doesn't overflow into adjacent gameplay
     * bytes. m_LodDistance is initialized once at level load
     * (initialization.c:411) and not re-written per frame, so a
     * per-frame override is safe and idempotent. g_Environment lives
     * at 0x800785A8; m_LodDistance is offset +0x24. */
    /* Phase 2.5 sector LOD collapse is done via PC-keyed lw substitution
     * at r_environment.s 0x80026214 (m_LodDistance read). That keeps
     * gameplay-readable RAM at canonical bytes; only the env-renderer's
     * cell-sort sees the bumped value. Avoids the widescreen+pause-menu
     * hang caused by other consumers seeing a mutated m_LodDistance.
     *
     * Phase 3 (texture LOD collapse) reverted: forcing
     * m_LQTexturePointer = m_HQTexturePointer makes far sectors index
     * HQ TIM descriptors that point to VRAM pages the LOD uploader
     * never staged for far-sector use. Result is texture corruption
     * everywhere except close-up (where HQ pages are already in VRAM).
     * A real Phase 3 needs a paired pre-render pass that uploads HQ
     * TIM pages for every sector touched by the HP-walk. Deferred. */

    /* Install env-var write-watcher on the first frame, after the
     * runtime has wired cpu->write_byte/half/word. */
    static int s_watch_init_done = 0;
    if (!s_watch_init_done) {
        s_watch_init_done = 1;
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

void game_on_frame(CPUState* cpu, uint32_t frame_counter) {
    (void)cpu; (void)frame_counter;
    /* TODO — only needed if a per-frame DMA timing hack surfaces. */
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
#include "features/widescreen.h"

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
        uint32_t lo = widescreen_polybuf_lower_vaddr();
        uint32_t hi = widescreen_polybuf_higher_vaddr();
        cpu->write_word(0x80076F50u, lo);  /* g_DB[0].m_PolyBuf           */
        cpu->write_word(0x80076FD4u, hi);  /* g_DB[1].m_PolyBuf           */
        s_in_alloc_buffers = 0;
        return 1;
    }

    return 0;
}
