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
#include "openpete_psx.h"

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

void game_apply_runtime_quirks(CPUState* cpu) {
    (void)cpu;
    /* TODO Phase 1c — preamble fixes, known-bad-instruction workarounds. */
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
        install_watch_from_env(cpu);
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

/* --aspect: bypass the level's per-occlusion-group visibility list at
 * widescreen and walk every sector in g_Environment.m_SectorPointer[]
 * instead. The list was authored for 4:3, so off-axis cells aren't
 * candidates regardless of cull-bound widening. The renderer already
 * supports this — passing pOcclusionGroup=-1 skips the list and walks
 * all sectors (see r_environment.s:80-93 / src/environment.c:34-42).
 * Native callers use that path only when the camera is *outside* any
 * known group, with a shorter m_CullingDistance (0x14000) to compensate
 * for the lost filter. We restore the in-group distance (0x28000) so
 * far cells aren't dropped, then call the renderer body.
 *
 * Risk: prim-buffer overflow. The OT pool at D_8006FCF4 is sized for
 * 4:3 candidate counts; walking 2-3× more cells could exceed it. So
 * far Artisans renders fine; if a denser level breaks, fall back to
 * the cull-only path or shrink m_CullingDistance. */
extern void func_800258F0(CPUState* cpu);
extern void func_8004EBA8(CPUState* cpu);
extern void func_80022A2C(CPUState* cpu);
extern float openpete_disp_w_factor;
extern unsigned openpete_frame;

/* env-renderer instrumentation. Counts writes done by func_800258F0 to
 * help diagnose 16:9 residual: are wider-FOV cells producing more prim
 * traffic, or is the renderer doing the same work as 4:3? */
static struct {
    void (*orig_w)(uint32_t, uint32_t);
    void (*orig_h)(uint32_t, uint16_t);
    void (*orig_b)(uint32_t, uint8_t);
    CPUState* cpu;
    uint32_t writes_total;
    uint32_t writes_ram_low;     /* < 0x80020000 (scratch + early data) */
    uint32_t writes_sbss;        /* 0x80075000..0x800A0000 (g_WorldOT etc) */
    uint32_t writes_overlay;     /* >= 0x80100000 (level data) */
    uint32_t writes_dma;         /* GPU/DMA control regs (>= 0x1F000000) */
    uint32_t worldot_ptr_before;
    uint32_t worldot_ptr_after;
    int active;
} g_env_trace = {0};

static void env_trace_classify(uint32_t addr) {
    g_env_trace.writes_total++;
    if (addr >= 0x1F000000u && addr < 0x80000000u) g_env_trace.writes_dma++;
    else if (addr >= 0x80100000u)                  g_env_trace.writes_overlay++;
    else if (addr >= 0x80075000u)                  g_env_trace.writes_sbss++;
    else if (addr >= 0x80000000u)                  g_env_trace.writes_ram_low++;
}
static void env_trace_w(uint32_t a, uint32_t v) {
    env_trace_classify(a); g_env_trace.orig_w(a, v);
}
static void env_trace_h(uint32_t a, uint16_t v) {
    env_trace_classify(a); g_env_trace.orig_h(a, v);
}
static void env_trace_b(uint32_t a, uint8_t v) {
    env_trace_classify(a); g_env_trace.orig_b(a, v);
}

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

    /* GamestateDraw bracket: confine Hor+ widening to the render pass so
     * GamestateUpdate's GTE work sees native 4:3 state and stays as
     * close to bit-identical between aspect modes as possible.
     *
     * Reduction observed (20000-frame demo): without bracket 13327
     * frames diverge with peak Δ²=6.04 B; with bracket 7824 / 88.6 M
     * (~68× peak shrink). The remaining drift comes from render-side
     * mutations of moby visibility/pointer tables in 0x70000-0x71E00,
     * which feed the next GamestateUpdate. Documented in
     * notes/plans/openpete-widescreen-fov.md. */
    extern void func_8001ED5C(CPUState* cpu);
    extern int  openpete_render_pass_active;
    extern int32_t psx_runtime_moby_cone_extra_q12;
    extern int32_t openpete_moby_cone_extra_q12_render;
    static int s_in_render_pass = 0;

    if (addr == 0x8001ED5Cu && openpete_disp_w_factor != 1.0f
        && openpete_disp_w_factor > 0.0f
        && !s_in_render_pass) {
        s_in_render_pass = 1;
        openpete_render_pass_active = 1;
        psx_runtime_moby_cone_extra_q12 = openpete_moby_cone_extra_q12_render;
        uint32_t saved_gte_data[32], saved_gte_ctrl[32];
        memcpy(saved_gte_data, cpu->gte_data, sizeof(saved_gte_data));
        memcpy(saved_gte_ctrl, cpu->gte_ctrl, sizeof(saved_gte_ctrl));
        func_8001ED5C(cpu);
        memcpy(cpu->gte_data, saved_gte_data, sizeof(saved_gte_data));
        memcpy(cpu->gte_ctrl, saved_gte_ctrl, sizeof(saved_gte_ctrl));
        psx_runtime_moby_cone_extra_q12 = 0;
        openpete_render_pass_active = 0;
        s_in_render_pass = 0;
        return 1;
    }

    static int s_in_env_renderer = 0;
    if (addr == 0x800258F0u && openpete_disp_w_factor != 1.0f
        && openpete_disp_w_factor > 0.0f
        && !s_in_env_renderer) {
        s_in_env_renderer = 1;
        /* Capture caller's a0 (occlusion-group id or -1) BEFORE we force
         * the all-walk path. The canonical_mask below replays the cell-
         * list iteration the 4:3 native call would have done, which is
         * group-filtered when a0 >= 0. Without this, our mask iterates
         * the master sector table and admits cells that lie in the
         * canonical cone but aren't in the canonical occlusion group —
         * surfaced at frame 3495 of title autoplay (5 cells leaked). */
        int32_t original_a0 = (int32_t)cpu->a0;
        cpu->a0 = (uint32_t)-1;        /* walk all sectors */

        /* Boost m_CullingDistance to the in-group value (0x28000) — the
         * caller (env.c func_8002B9CC) sets a shorter 0x14000 / 0x1c000
         * when going through the all-walk path because the original
         * design only used all-walk as a fallback. With wide aspects,
         * far-off-axis cells are needed; restore the full distance. */
        const uint32_t cd_addr = 0x800785A8u + 0x28u;
        uint32_t saved_cd = cpu->read_word(cd_addr);
        if (saved_cd > 0u && saved_cd < 0x28000u) {
            cpu->write_word(cd_addr, 0x28000u);
        }

        /* Cone-cull widening. r_environment.s loads g_Camera.m_ViewMatrix
         * (offset 0x14, 5 i32s) into CP2C[R11..R33] and runs MVMVA against
         * each cell's IR-vec to derive |IR1|/IR3 for the cone test
         * (slope 0.75 ≈ atan(256/341) = native 4:3). Scaling row 0
         * (`m_ViewMatrix.m[0][0..2]`) by 1/disp_w_factor shrinks IR1 by
         * the same factor so the cone admits cells in a wider horizontal
         * arc. Saved + restored around the call so other consumers of
         * m_ViewMatrix (collision, AI) aren't affected. */
        const uint32_t mvm_addr = 0x80078A58u - 0x80000000u;  /* TODO from manifest */
        (void)mvm_addr;  /* m_ViewMatrix lives at g_Camera + 0x14 */
        const uint32_t vm_base = 0x80076DD0u + 0x14u;
        /* Save the full 20-byte SHORTMATRIX (9 shorts + pad). The
         * native renderer body may compose row 0 with rows 1/2 via
         * MVMVA chains and write back the composed matrix; restoring
         * only row 0 leaves widened residue in rows 1/2 that gameplay's
         * next CameraUpdateMatrices reads. See RAM-diff under
         * notes/plans/openpete-widescreen-fov.md. */
        uint8_t saved_view_matrix[20];
        memcpy(saved_view_matrix,
               openpete_psx_get_ram() + (vm_base & 0x1FFFFFFFu), 20);

        /* Pre-compute the canonical 4:3 per-cell visibility mask used to
         * de-leak the class_skip_flags channel. r_environment.s writes
         * D_800771C8[cell_idx] = 0xFF for every cell that passes its
         * cone-cull (PC 0x80025B50). Gameplay (func_800521C0) reads that
         * table next frame to clear m_WasDrawn=0 on mobys whose owning
         * sector wasn't rendered.
         *
         * The matrix-row-0 widening below admits more cells in 16:9 →
         * widened D_800771C8 → wrong gameplay verdict. We replicate the
         * three cull conditions in C against the canonical (un-widened)
         * matrix here, then AND the widened-renderer's output with this
         * mask after func_800258F0 returns.
         *
         * The cull conditions (per asm at 0x80025A98..0x80025B04):
         *   1. Rz + bbox_E > 0   (front face in front of camera)
         *   2. 4*(|Rx| - 25E/32) - 3*(Rz + 9E/16) < 0   (X-cone)
         *   3. 32*(|Ry| - 7E/8) - 17*(Rz + 7E/16) < 0  (Y-cone)
         * Where (Rx,Ry,Rz) = canonical_view_matrix * cell_relative_to_cam,
         * with sf=1 (>>12) and signed int16 saturation.
         *
         * The asm has additional back-corner tests at 0x80025B0C..B40, but
         * those only adjust the queue-push value $a1; visibility is
         * always written 0xFF after the front-face cone passes. */
        uint8_t canonical_mask[256];
        memset(canonical_mask, 0, sizeof(canonical_mask));
        {
            int16_t M[3][3];
            const int16_t* vm_words = (const int16_t*)saved_view_matrix;
            M[0][0] = vm_words[0]; M[0][1] = vm_words[1]; M[0][2] = vm_words[2];
            M[1][0] = vm_words[3]; M[1][1] = vm_words[4]; M[1][2] = vm_words[5];
            M[2][0] = vm_words[6]; M[2][1] = vm_words[7]; M[2][2] = vm_words[8];

            const uint32_t cam_pos_addr = 0x80076DD0u + 0x28u;  /* g_Camera.m_Position */
            int32_t cam_x4 = (int32_t)cpu->read_word(cam_pos_addr + 0) >> 4;
            int32_t cam_y4 = (int32_t)cpu->read_word(cam_pos_addr + 4) >> 4;
            int32_t cam_z4 = (int32_t)cpu->read_word(cam_pos_addr + 8) >> 4;

            const uint32_t env_addr = 0x800785A8u;
            uint32_t cell_list_base = cpu->read_word(env_addr + 0);
            uint32_t cell_count     = cpu->read_word(env_addr + 4);
            if (cell_count > 256) cell_count = 256;

            /* Build the canonical iteration set. r_environment.s branches
             * on $ra (= caller's a0): bltz → all-walk env+0x0/+0x4; else
             * walk a per-group byte-index list at env+0x8 → groups[a0],
             * terminated by 0xFF. */
            uint8_t indices[256];
            uint32_t n_indices = 0;
            if (original_a0 < 0) {
                for (uint32_t i = 0; i < cell_count; i++) {
                    indices[n_indices++] = (uint8_t)i;
                }
            } else {
                uint32_t groups_table = cpu->read_word(env_addr + 8);
                if (groups_table) {
                    uint32_t group_list = cpu->read_word(
                        groups_table + (uint32_t)original_a0 * 4);
                    if (group_list) {
                        uint8_t* gram = openpete_psx_get_ram();
                        for (uint32_t k = 0; k < 256; k++) {
                            uint8_t b = gram[(group_list + k) & 0x1FFFFFFFu];
                            if (b == 0xFF) break;
                            indices[n_indices++] = b;
                        }
                    }
                }
            }

            for (uint32_t k = 0; k < n_indices; k++) {
                uint32_t i = (uint32_t)indices[k];
                uint32_t cell_ptr = cpu->read_word(cell_list_base + i * 4);
                if (cell_ptr == 0) continue;
                uint32_t v0w = cpu->read_word(cell_ptr + 0);
                uint32_t a0w = cpu->read_word(cell_ptr + 4);
                int16_t cell_v0_lo = (int16_t)(v0w & 0xFFFF);
                int16_t cell_v0_hi = (int16_t)((v0w >> 16) & 0xFFFF);
                int16_t cell_a0_hi = (int16_t)((a0w >> 16) & 0xFFFF);
                int32_t bbox_E = (int32_t)(a0w & 0x1FFF);

                int16_t V[3];
                V[0] = (int16_t)((int32_t)cam_y4 - (int32_t)cell_v0_lo);
                V[1] = (int16_t)((int32_t)cam_z4 - (int32_t)cell_a0_hi);
                V[2] = (int16_t)((int32_t)cell_v0_hi - (int32_t)cam_x4);

                int64_t mac1 = (int64_t)M[0][0]*V[0] + (int64_t)M[0][1]*V[1] + (int64_t)M[0][2]*V[2];
                int64_t mac2 = (int64_t)M[1][0]*V[0] + (int64_t)M[1][1]*V[1] + (int64_t)M[1][2]*V[2];
                int64_t mac3 = (int64_t)M[2][0]*V[0] + (int64_t)M[2][1]*V[1] + (int64_t)M[2][2]*V[2];
                mac1 >>= 12; mac2 >>= 12; mac3 >>= 12;
                int32_t Rx = (mac1 > 32767) ? 32767 : (mac1 < -32768 ? -32768 : (int32_t)mac1);
                int32_t Ry = (mac2 > 32767) ? 32767 : (mac2 < -32768 ? -32768 : (int32_t)mac2);
                int32_t Rz = (mac3 > 32767) ? 32767 : (mac3 < -32768 ? -32768 : (int32_t)mac3);

                if (Rz + bbox_E <= 0) continue;
                int32_t abs_Rx = (Rx < 0) ? -Rx : Rx;
                int32_t abs_Ry = (Ry < 0) ? -Ry : Ry;
                /* X half-cone radius = 25E/32. */
                int32_t t3 = (bbox_E >> 1) + (bbox_E >> 2) + (bbox_E >> 5);
                /* Front-face X-cone Z adjust = 19E/32. The asm computes
                 *   t4 = E/2; t5 = E/16; t4 += t5            (9E/16)
                 *   t5 = E/32
                 *   ; delay slot of `blez at, cull` (behind-cam test)
                 *   t4 += t5                                 (19E/32)
                 * Missing the delay-slot add is what threw the C-side
                 * canonical cull off — see the original investigation
                 * in notes/plans/openpete-determinism-enforcement.md. */
                int32_t t4 = (bbox_E >> 1) + (bbox_E >> 4) + (bbox_E >> 5);
                /* Y half-cone radius = 7E/8. */
                int32_t t5 = bbox_E - (bbox_E >> 3);
                /* Front-face Y-cone Z adjust = 7E/16. */
                int32_t t6 = (bbox_E >> 1) - (bbox_E >> 4);
                if (4*(abs_Rx - t3) - 3*(Rz + t4) >= 0) continue;
                if (32*(abs_Ry - t5) - 17*(Rz + t6) >= 0) continue;
                canonical_mask[i] = 1;
            }
        }

        int16_t row0[3];
        for (int i = 0; i < 3; i++) {
            int16_t orig = (int16_t)cpu->read_half(vm_base + (uint32_t)(i * 2));
            float scaled = (float)orig / openpete_disp_w_factor;
            if (scaled >  32767.0f) scaled =  32767.0f;
            if (scaled < -32768.0f) scaled = -32768.0f;
            row0[i] = (int16_t)scaled;
            cpu->write_half(vm_base + (uint32_t)(i * 2), (uint16_t)row0[i]);
        }

        /* Instrumentation: wrap write fns to count what env emits. */
        int env_trace_on = (getenv("OPENPETE_ENV_TRACE") != NULL);
        if (env_trace_on) {
            g_env_trace.orig_w = cpu->write_word;
            g_env_trace.orig_h = cpu->write_half;
            g_env_trace.orig_b = cpu->write_byte;
            g_env_trace.cpu    = cpu;
            g_env_trace.writes_total = 0;
            g_env_trace.writes_ram_low = 0;
            g_env_trace.writes_sbss = 0;
            g_env_trace.writes_overlay = 0;
            g_env_trace.writes_dma = 0;
            g_env_trace.worldot_ptr_before = cpu->read_word(0x80075820u);
            cpu->write_word = env_trace_w;
            cpu->write_half = env_trace_h;
            cpu->write_byte = env_trace_b;
            g_env_trace.active = 1;
        }

        func_800258F0(cpu);

        if (env_trace_on) {
            cpu->write_word = g_env_trace.orig_w;
            cpu->write_half = g_env_trace.orig_h;
            cpu->write_byte = g_env_trace.orig_b;
            g_env_trace.active = 0;
            g_env_trace.worldot_ptr_after = cpu->read_word(0x80075820u);
            if ((openpete_frame % 60u) == 0u) {
                fprintf(stderr,
                        "[env-trace f=%u factor=%.3f] writes total=%u "
                        "low=%u sbss=%u ovl=%u dma=%u  WorldOT %08X->%08X\n",
                        openpete_frame, openpete_disp_w_factor,
                        g_env_trace.writes_total,
                        g_env_trace.writes_ram_low,
                        g_env_trace.writes_sbss,
                        g_env_trace.writes_overlay,
                        g_env_trace.writes_dma,
                        g_env_trace.worldot_ptr_before,
                        g_env_trace.worldot_ptr_after);
            }
        }

        /* Restore the full m_ViewMatrix so any composed-matrix
         * writebacks inside func_800258F0 don't leak into gameplay. */
        memcpy(openpete_psx_get_ram() + (vm_base & 0x1FFFFFFFu),
               saved_view_matrix, 20);

        /* AND D_800771C8 with the canonical mask: any byte the widened
         * cull set to 0xFF that wouldn't have passed the canonical 4:3
         * cone gets cleared back to 0. Result: gameplay's func_800521C0
         * reads exactly the canonical visibility verdict next frame,
         * while the render queues + emitted prims (already populated by
         * the widened pass) cover the full Hor+ visible area. */
        const uint32_t class_skip_addr = 0x800771C8u;
        uint8_t* ram = openpete_psx_get_ram();
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t a = (class_skip_addr + i) & 0x1FFFFFFFu;
            if (ram[a] != 0u && canonical_mask[i] == 0u) {
                ram[a] = 0u;
            }
        }

        cpu->write_word(cd_addr, saved_cd);
        s_in_env_renderer = 0;
        return 1;
    }

    /* --aspect: stretch the cyclorama mesh's horizontal projection so
     * its native triangles cover the full widescreen framebuffer width.
     *
     * The recompiler's OFX widening (eff_OFX = 256 * disp_w_factor)
     * shifts every projected vertex right by (eff_OFX - 256). The
     * cyclorama mesh's *angular* extent is unchanged, so its triangles
     * span [shift, shift + 512] in PSX-X — leaving framebuffer edges
     * uncovered, where DrawEnv `isbg=1` paints g_Cyclorama.m_BackgroundColor.
     *
     * Fix: pre-scale the cyclorama's projection-matrix row 0 (the X
     * row) by disp_w_factor before calling the renderer body, so post-
     * MVMVA IR1 is `factor * native_IR1` and projects across the full
     * widened framebuffer. The widened bit-3 cull threshold
     * (psx_runtime_clip_x_threshold) keeps the larger SX values from
     * being cull-rejected.
     *
     * The matrix at *a2 belongs to the caller (typically
     * &g_Camera.m_ProjectionMatrix), so we save and restore around the
     * call. Capped at factor 2.0 — beyond that (e.g. 32:9 = 2.667) the
     * scaled IR1 produces SX values that overflow PsyCross's primitive
     * split path and crash DrawSplit. */
    static int s_in_cyclorama = 0;
    if (addr == 0x8004EBA8u && openpete_disp_w_factor != 1.0f
        && openpete_disp_w_factor > 0.0f
        && openpete_disp_w_factor <= 2.0f
        && !s_in_cyclorama) {
        s_in_cyclorama = 1;
        uint32_t mat_addr = cpu->a2;
        /* Save the full 20-byte SHORTMATRIX. The native cyclorama
         * renderer composes the scaled row 0 into the rest of the
         * matrix and writes the composed values back; restoring only
         * row 0 leaves rows 1/2 contaminated and gameplay's next
         * CameraUpdateMatrices reads widened residue. */
        uint8_t saved_matrix[20];
        memcpy(saved_matrix,
               openpete_psx_get_ram() + (mat_addr & 0x1FFFFFFFu), 20);
        int16_t saved_row0[3];
        float max_abs = 1.0f;
        for (int i = 0; i < 3; i++) {
            uint16_t h = cpu->read_half(mat_addr + (uint32_t)(i * 2));
            saved_row0[i] = (int16_t)h;
            float a = (float)((saved_row0[i] < 0) ? -saved_row0[i]
                                                   :  saved_row0[i]);
            if (a > max_abs) max_abs = a;
        }
        float scale = openpete_disp_w_factor;
        float headroom = 32700.0f / max_abs;
        if (scale > headroom) scale = headroom;
        for (int i = 0; i < 3; i++) {
            float scaled = (float)saved_row0[i] * scale;
            cpu->write_half(mat_addr + (uint32_t)(i * 2),
                            (uint16_t)(int16_t)scaled);
        }
        func_8004EBA8(cpu);
        memcpy(openpete_psx_get_ram() + (mat_addr & 0x1FFFFFFFu),
               saved_matrix, 20);
        s_in_cyclorama = 0;
        return 1;
    }

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

    return 0;
}
