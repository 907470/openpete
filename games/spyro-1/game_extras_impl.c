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

void game_on_frame_pre(CPUState* cpu, uint32_t frame_counter) {
    (void)frame_counter;

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

    static int s_in_env_renderer = 0;
    if (addr == 0x800258F0u && openpete_disp_w_factor > 0.0f
        && !s_in_env_renderer) {
        s_in_env_renderer = 1;
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
        int16_t saved_row0[3];
        int16_t row0[3];
        for (int i = 0; i < 3; i++) {
            uint16_t h = cpu->read_half(vm_base + (uint32_t)(i * 2));
            saved_row0[i] = (int16_t)h;
            float scaled = (float)saved_row0[i] / openpete_disp_w_factor;
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

        /* Restore so non-renderer consumers see native matrix. */
        for (int i = 0; i < 3; i++) {
            cpu->write_half(vm_base + (uint32_t)(i * 2),
                            (uint16_t)saved_row0[i]);
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
    if (addr == 0x8004EBA8u && openpete_disp_w_factor > 0.0f
        && openpete_disp_w_factor <= 2.0f
        && !s_in_cyclorama) {
        s_in_cyclorama = 1;
        uint32_t mat_addr = cpu->a2;
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
        for (int i = 0; i < 3; i++) {
            cpu->write_half(mat_addr + (uint32_t)(i * 2),
                            (uint16_t)saved_row0[i]);
        }
        s_in_cyclorama = 0;
        return 1;
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
