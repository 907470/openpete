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

/* PsyCross XA streaming source — forced gain from --music override.
 * ForceGain takes priority over the cd.volume → AL gain mapping that
 * psyq_SpuSetCommonAttr does, so the override stays clamped even when
 * the game's fade logic later writes cd.volume. */
extern void PsyX_XA_ForceGain(float linear_gain);
extern int  PsyX_XA_IsActive(void);
extern void PsyX_XA_Stop(void);


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

/* --sfx / --music CLI overrides. -1 = leave alone; 0..10 = forced
 * value (0=OFF). Both apply once via the SpuInitialize dispatch hook
 * below — that closes the window between SpuInitialize seeding the
 * default volumes and the title-screen intro keying its first voice
 * (sfx) / starting the title music (music). */
static int s_sfx_volume_override = -1;
static int s_sfx_applied = 0;
static int s_music_volume_override = -1;
static int s_music_applied = 0;

void game_set_sfx_volume(int value) {
    s_sfx_volume_override = value;
    s_sfx_applied = 0;
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

void game_set_music_volume(int value) {
    s_music_volume_override = value;
    s_music_applied = 0;
    /* Lock the AL gain immediately so XA streams started before the
     * first frame_pre tick (or before the next SpuSetCommonAttr
     * fade-write) honor the override. -1 releases. */
    if (value >= 0) {
        int v = value > 10 ? 10 : value;
        PsyX_XA_ForceGain((float)v / 10.0f);
    } else {
        PsyX_XA_ForceGain(-1.0f);
    }
}

/* Spyro's "SOUND EFFECTS" slider lives at:
 *   D_80075754             0..10  (0 = OFF as shown in the menu)
 *   g_Spu @ 0x80075F08
 *     +0x320  unk_0x320    intermediate: D_80075754 * 0x3FFF / 10
 *     +0x324  m_SoundVolume intermediate: unk_0x320 * 0x1000 / 0x3FFF
 * SpuInitialize (spu.c:25) writes m_SoundVolume = 0x1000 *before* any
 * sound can be queued (no SPU init = no PlaySound). That's our true
 * gate: as soon as the game has set m_SoundVolume non-zero, override
 * it. This catches the title-screen intro cutscene wind SFX, which
 * keys voices well before gamestates/init.c:549 sets D_80075754 = 10.
 * After the one-shot write the user can still adjust from the options
 * menu — that path also rewrites m_SoundVolume, which we don't fight. */
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

    /* SFX is one-shot: nothing in the rest of the game's normal flow
     * rewrites m_SoundVolume, so the SpuInitialize stamp holds. */
    if (!s_sfx_applied && s_sfx_volume_override >= 0) {
        uint32_t cur_sound_volume = cpu->read_word(0x8007622Cu);
        if (cur_sound_volume != 0) {
            int v = s_sfx_volume_override;
            if (v < 0) v = 0;
            if (v > 10) v = 10;
            uint32_t unk_0x320     = (uint32_t)((v * 0x3FFF) / 10);
            uint32_t m_SoundVolume = (unk_0x320 * 0x1000u) / 0x3FFFu;
            cpu->write_word(0x80075754u, (uint32_t)v);
            cpu->write_word(0x80076228u, unk_0x320);
            cpu->write_word(0x8007622Cu, m_SoundVolume);
            s_sfx_applied = 1;
        }
    }

    /* Music CANNOT be one-shot: gamestates/init.c:func_8002D170 (title
     * state) re-stamps D_80075748 = 10, g_Spu.m_MusicVolume = 0x5000,
     * and cd.volume = 0x5000 every time it enters the title screen,
     * which clobbers our SpuInitialize-time stamp. Re-apply each frame
     * so --music 0 stays at 0 across state transitions. The cost is
     * that the in-game music slider can't be moved while the override
     * is active — that's what the user signed up for by passing --music. */
    if (s_music_volume_override >= 0) {
        int v = s_music_volume_override;
        if (v < 0) v = 0;
        if (v > 10) v = 10;
        uint32_t mv = (uint32_t)(v << 11);
        cpu->write_word(0x80075748u, (uint32_t)v);
        cpu->write_word(0x80076224u, mv);
        cpu->write_word(0x80075F18u, (mv & 0xFFFFu) | (mv << 16));

        /* AL gain is locked via PsyX_XA_ForceGain at game_set_music_volume
         * time; nothing to do here for that path. The RAM-side stamps
         * gate FUTURE SetMusicState calls and keep music.c's fade-target
         * computations consistent with the override. */
        s_music_applied = 1;
    }
}

void game_on_frame(CPUState* cpu, uint32_t frame_counter) {
    (void)cpu; (void)frame_counter;
    /* TODO — only needed if a per-frame DMA timing hack surfaces. */
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

/* SpuInitialize at 0x8005595c (spyro-1/build/psx.map). It writes
 * g_Spu.m_SoundVolume = 0x1000 inline (spu.c:25). Run it normally,
 * then stamp our --sfx override immediately on return — that closes
 * the window between m_SoundVolume becoming non-zero and the very
 * next PlaySound (which the title-screen intro fires before any host-
 * side per-frame hook gets a chance to run). */
extern void func_8005595C(CPUState* cpu);  /* SpuInitialize */

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

    static int s_in_traced_call = 0;
    if (addr == 0x80023AC4u && !s_in_traced_call) {
        s_in_traced_call = 1;
        openpete_trace_writes_begin(cpu);
        func_80023AC4(cpu);
        openpete_trace_writes_end(cpu);
        s_in_traced_call = 0;
        return 1;
    }
    /* SpuInitialize dispatch hook: applies both --sfx and --music
     * overrides immediately after the function seeds default volumes
     * (spu.c:25–31). Music slider scale: D_80075748 = N (0..10);
     * g_Spu.m_MusicVolume = N << 11; m_CommonAttr.cd.volume.{l,r} =
     * same. Sfx writes g_Spu.unk_0x320 + m_SoundVolume per the
     * existing slider math. */
    static int s_in_spuinit = 0;
    int need_sfx   = !s_sfx_applied   && s_sfx_volume_override   >= 0;
    int need_music = !s_music_applied && s_music_volume_override >= 0;
    if (addr == 0x8005595Cu && !s_in_spuinit && (need_sfx || need_music)) {
        s_in_spuinit = 1;
        func_8005595C(cpu);
        s_in_spuinit = 0;

        if (need_sfx) {
            int v = s_sfx_volume_override;
            if (v < 0) v = 0;
            if (v > 10) v = 10;
            uint32_t unk_0x320     = (uint32_t)((v * 0x3FFF) / 10);
            uint32_t m_SoundVolume = (unk_0x320 * 0x1000u) / 0x3FFFu;
            cpu->write_word(0x80075754u, (uint32_t)v);
            cpu->write_word(0x80076228u, unk_0x320);
            cpu->write_word(0x8007622Cu, m_SoundVolume);
            s_sfx_applied = 1;
        }

        if (need_music) {
            int v = s_music_volume_override;
            if (v < 0) v = 0;
            if (v > 10) v = 10;
            uint32_t mv = (uint32_t)(v << 11);          /* N * 0x800 */
            cpu->write_word(0x80075748u, (uint32_t)v);  /* D_80075748 */
            cpu->write_word(0x80076224u, mv);           /* g_Spu.m_MusicVolume */
            /* m_CommonAttr.cd.volume is at g_Spu+0x10/+0x12 (shorts).
             * Write a packed dword: low half = left, high half = right. */
            cpu->write_word(0x80075F18u, (mv & 0xFFFFu) | (mv << 16));
            s_music_applied = 1;
        }
        return 1;
    }
    return 0;
}
