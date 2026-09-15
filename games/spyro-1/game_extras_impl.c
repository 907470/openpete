// games/spyro-1/game_extras_impl.c — Spyro 1's per-game hooks.
//
// Identity is generated from recomp.conf (game_identity.c); this file holds
// the behavioural hooks: overlay handling, runtime quirks, the per-tick
// entries, savestate, and the CLI resolvers.
//
// Disc image: games/spyro-1/spyro-1/build/SCUS-94228.cue
// Disc BIN SHA256: fc866b2a02e010a6658f8af2de28bb3001eb33513e5924af014e35643c6dee37

#include "game_extras.h"
#include "feature_registry.h"
#include "openpete_mods.h"
#include "openpete_psx.h"
#include "openpete_ui_moby.h"
#include "openpete_fiber.h"
#include "psx_runtime_internal.h"
#include "spyro_overlay_table.h"
#include "renderer/text_moby.h"
#include "features/audio_volume.h"
#include "features/vibration.h"
#include "features/cheats.h"
#include "features/credits_skip.h"
#include "features/crash_demo_woah.h"
#include "freecam_game.h"
#include "write_census.h"
#include "openpete_fillpick.h"
#include "features/levelwarp.h"
#include "psyq/psyq_thunk_state.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t game_get_pad_read_pc(void) {
    /* PadVSync (spyro-1/src/gamepad.c) @ 0x80053C68 — the game's once-per-
     * VSync consumption of g_PadBuffer: it reads status, size_type, inputs[]
     * and sticks and folds them into g_Pad. Every other reader in the game
     * works from g_Pad, so this entry is where the host's latched sample
     * actually becomes the tick's input. PadDemoUpdate (0x800539FC) also
     * touches the buffer but only on the demo-record path, which is not a
     * play path. */
    return 0x80053C68u;
}

/* Loads are instant until this game classifies its load reads: LoadLevel's
 * CDLoadAsync reads are polled from the main loop, but the titlescreen demo
 * load spins `while (g_LoadStage < 6) LoadLevel(1);` with no VSync and
 * CDLoadSync's second loop tests m_IsReading before it calls CdSync, so a
 * deferred completion at either site would never fire. The oracle golden
 * and every recorded bk2 carry this value. */
uint32_t game_get_load_delay_default(void) { return 0u; }

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

/* mods/mod_level_ops.c — same private-decl pattern main.cpp uses. */
void spyro1_mod_level_capture_stock_counts(CPUState* cpu);

void game_on_boot(CPUState* cpu) {
    /* The target gem/dragon/egg tables are EXE .data that the custom-level
     * registry overwrites per level; snapshot the shipped values while
     * they are still the only thing in those bytes. */
    spyro1_mod_level_capture_stock_counts(cpu);
    /* The write census installs over the store hooks before any guest
     * instruction runs; inert without OPENPETE_WRITE_CENSUS. */
    write_census_install(cpu);
}

extern unsigned openpete_frame;

void game_apply_runtime_quirks(CPUState* cpu) {
    (void)cpu;
    /* TODO Phase 1c — preamble fixes, known-bad-instruction workarounds. */
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
/* Present fast-forward stop cap, set relative to arming (a boot skip arms
 * near frame 0, a runtime title-screen warp arms tens of thousands of
 * frames in — an absolute cap would clear the latter instantly). */
static uint32_t s_skip_ffwd_deadline = 0;

void game_set_skip_to_level(int level_id) {
    s_skip_to_level = level_id;
    s_skip_applied  = 0;
    /* Fast-forward presents to the level for runtime warps exactly as for
     * the boot CLI flag — the dev levelwarp's title route and the
     * custom-level menu both come through here. */
    if (level_id > 0 && openpete_skip_ffwd_windowed) {
        openpete_skip_present_until_level = 1;
        s_skip_ffwd_deadline = 0;
        fprintf(stderr, "[skip-to-level] fast-forwarding presents until "
                        "level %d loads\n", level_id);
    }
}

/* The level id/name table lives with the dev-overlay warp feature
 * (features/levelwarp.c) so the dropdown and --skip-to-level
 * resolve against exactly one list. */
/* The credits are entered from a live level (features/credits_skip.c
 * replays the "CREDITS" pad code on the first playable tick), so a credits
 * target rides a level skip: --skip-to-level's own target when armed, else
 * Artisans. The roll returns to that level. */
void game_set_skip_to_cutscene(int cutscene_id) {
    if (cutscene_id != SPYRO1_CUTSCENE_CREDITS &&
        cutscene_id != SPYRO1_CUTSCENE_CREDITS_FULL) return;
    spyro1_credits_skip_arm(cutscene_id == SPYRO1_CUTSCENE_CREDITS_FULL);
    if (s_skip_to_level <= 0) game_set_skip_to_level(10);
}
int game_resolve_cutscene_arg(const char* arg) { return spyro1_cutscene_resolve(arg); }

int game_resolve_level_arg(const char* arg) {
    return spyro1_level_resolve(arg);
}

void game_on_hash_emit(uint32_t frame) {
    (void)frame;
    /* Hook point for per-game state dumps aligned with the hash log's
     * timing. Spyro needs none. */
}

void game_on_frame_pre(CPUState* cpu, uint32_t frame_counter) {
    (void)frame_counter;

    static int s_ui_range_done = 0;
    if (!s_ui_range_done) {
        s_ui_range_done = 1;
        /* Register the static UI-moby range. g_Hud.m_Mobys[12] lives
         * at PSX vaddr 0x80077FEC (g_Hud at 0x80077FA8 + offset 0x44),
         * sizeof(Moby)=0x58, so the range is
         *   [0x80077FEC, 0x80077FEC + 12*0x58 = 0x8007840C).
         * Used by the native renderer's is_addr_ui/is_addr_static
         * HUD classification (moby.c). */
        openpete_ui_moby_set_static_range(0x80077FECu,
                                          0x80077FECu + 12u * 0x58u);
    }

    /* Optional scripted engage of the debug free camera (OPENPETE_FREECAM=1),
     * once gameplay is live. Interactive F / stick-click use is independent. */
    freecam_maybe_autoengage(cpu);

    /* The fill-tag inspector engages on the same "level is live" gate — a
     * scripted pick run sets OPENPETE_FILLPICK=1 alongside
     * OPENPETE_FREECAM_POSE and reads the [fillpick] lines off stderr. */
    {
        static int s_fp_done = 0;
        if (!s_fp_done && (int32_t)cpu->read_word(0x80075864u) < 0) {   /* g_LoadStage */
            s_fp_done = 1;
            const char* e = getenv("OPENPETE_FILLPICK");
            if (e && *e && *e != '0') openpete_fillpick_set_active(1);
            e = getenv("OPENPETE_FILLPICK_PREVIEW");
            if (e && *e && *e != '0') openpete_fillpick_set_preview(1);
        }
    }

    /* --cheats: apply the game's own built-in cheat effects once a level is
     * live, re-armed at the title screen so each new session gets them. Inert
     * without the flag. */
    spyro1_cheats_on_tick(cpu);

    /* Title-screen Crash-demo easter egg: pre-decode the woah while the
     * title screen is up, and re-arm between presses. Inert elsewhere. */
    spyro1_crash_woah_on_tick(cpu);

    /* --sfx / --music: hold the game's own volume sliders at the requested
     * levels (re-stamped over the title screen's and a save load's reseeds,
     * released when the player moves that slider), and mirror the music
     * slider onto the XA / music-WAV gain so it acts as a level rather than
     * an on/off switch. Inert without the flags apart from that mirror. */
    spyro1_audio_volume_on_tick(cpu);

    /* --vibration: hold the game's own vibration setting at the requested
     * state (re-stamped over the title screen's and a save load's reseeds,
     * released when the player toggles that row by hand). Inert without the
     * flag; the host half — motor strength, and the headless / focus gates —
     * lives in core/input/host_rumble.c. */
    spyro1_vibration_on_tick(cpu);

    /* Dev-overlay level warp (M menu): drives the game's own exit-level ->
     * load sequence toward a chosen level. Inert until the Jump button is
     * clicked. */
    spyro1_levelwarp_on_tick(cpu);


    /* Advance the gold shimmer — SpecularUpdate's travelling light, reproduced
     * host-side — once per tick, BEFORE the builders below, so the settings
     * menu and the OSD's toasts are lit by the same instant of it. */
    text_moby_native_shimmer_tick();

    /* Build the performance-stats overlay from Spyro's own 3D HUD text mobys
     * (digits 260-269, letters 426-451, punctuation) when the native renderer is
     * showing it. Pure presentation: reads model geometry from RAM, never writes
     * gameplay state. */
    text_moby_native_perf_overlay(cpu);

    /* Build the in-game settings-menu text (title + rows) from the same 3D gold
     * glyph mobys when the menu (O) is open. Pure presentation. */
    text_moby_native_menu(cpu);

    /* Any live OSD toasts, rebuilt against the shimmer phase this tick's
     * builders all share. Pure presentation. */
    text_moby_native_osd(cpu);

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
                /* Refuse targets whose guest WAD TOC entry is empty (data
                 * len 0) — loading one chases garbage and crashes. The
                 * free slot (id 65) only gains an entry once a custom
                 * level's warp claims it, so it can't be skip targeted
                 * cold; use OPENPETE_CUSTOM_WARP instead. */
                int ti = (s_skip_to_level / 10 - 1) * 6 +
                         s_skip_to_level % 10;
                if (ti < 0 || ti > 35 ||
                    !cpu->read_word(0x8007A6D0u + 72u + 16u * (uint32_t)ti +
                                    12u)) {
                    fprintf(stderr,
                            "[skip-to-level] level=%d has no WAD TOC data "
                            "(unclaimed slot?) — refusing; for a custom "
                            "level use OPENPETE_CUSTOM_WARP\n",
                            s_skip_to_level);
                    s_skip_to_level = 0;
                    /* A refused skip never induces, so the playable-edge
                     * clear below can't fire — drop the fast-forward now
                     * rather than leaving a black window to the cap. */
                    openpete_skip_present_until_level = 0;
                    s_skip_ffwd_deadline = 0;
                } else {
                    uint32_t demo_idx = cpu->read_word(0x800757B8u);
                    uint32_t slot     = 0x8006EE7Cu + (demo_idx * 4u);
                    cpu->write_word(slot, (uint32_t)s_skip_to_level);
                    cpu->write_word(0x80078D78u + 0x00u, 3u); /* m_Mode = TSM_Demo */
                    cpu->write_word(0x80078D78u + 0x04u, 0u); /* m_State = TSS_Setup */
                    cpu->write_word(0x80078D78u + 0x1Cu, 2u); /* m_DemoType = TSD_DemoLevel */
                    s_skip_applied = 1;
                    fprintf(stderr,
                            "[skip-to-level] induced demo-transition: "
                            "level=%d (slot=%u) at frame=%u\n",
                            s_skip_to_level, (unsigned)demo_idx,
                            (unsigned)frame_counter);
                }
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

    /* --skip-to-level fast-forward (openpete_skip_present_until_level): the host
     * loop skips presents + pacing until the target level is actually PLAYABLE.
     * Resume the moment g_Gamestate reaches GS_Playing (0). Note TSS_Active (the
     * LoadLevel-complete signal) is NOT the right stop — the homeworld-entrance
     * flyby that follows is a "THE ADVENTURE BEGINS" title card over black for
     * ~380 frames; the level itself only appears when the flyby hands off to
     * GS_Playing. Gated on s_skip_applied so early-boot GS_Playing (before the
     * title) can't trip it; the frame cap is a fail-safe so a never-induced skip
     * can't leave a permanently black window. */
    if (openpete_skip_present_until_level) {
        uint32_t gs = cpu->read_word(0x800757D8u); /* g_Gamestate */
        if (!s_skip_ffwd_deadline)
            s_skip_ffwd_deadline = frame_counter + 4000u;
        /* A credits skip passes through GS_Playing for one tick on its way
         * to GS_Credits (15); presents resume on the roll, not the level. */
        int playable = (gs == 0u /*GS_Playing*/ &&
                        !spyro1_credits_skip_pending()) ||
                       gs == 15u /*GS_Credits*/;
        if ((s_skip_applied && playable) ||
            frame_counter > s_skip_ffwd_deadline) {
            openpete_skip_present_until_level = 0;
            s_skip_ffwd_deadline = 0;
            fprintf(stderr,
                    "[skip-to-level] level playable — presents resume at "
                    "frame=%u\n", (unsigned)frame_counter);
        }
    }
}

void game_on_frame(CPUState* cpu, uint32_t frame_counter) {
    (void)cpu; (void)frame_counter;
    /* Fires once per canonical tick, after the fiber yields with consistent
     * RAM. Spyro needs no post-tick work. */
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

/* Gameplay-live gate for the in-memory snapshot users (savestate P3).
 * Mirrors ops_level_id's pattern: g_Gamestate == GS_Playing AND a
 * non-zero level id — g_Gamestate reads 0 during early boot too (BSS
 * zero), so the level-id check is what actually excludes boot. */
int game_is_gameplay_live(CPUState* cpu) {
    if (cpu->read_word(0x800757D8u) != 0u) return 0; /* g_Gamestate != GS_Playing */
    return cpu->read_word(0x8007596Cu) != 0u;        /* g_LevelId */
}

/* Game-side extra savestate payload. Host-side state that mirrors or
 * gates guest execution and must travel with every snapshot — including
 * the per-tick runahead/rewind mem snapshots, whose speculative ticks can
 * run load/transition code (CdlSetloc moves the WAD cursor, an overlay
 * CdRead flips the dispatcher) that the rollback must undo. Bump the
 * version word when fields are added — core treats the blob as opaque,
 * so layout discipline lives HERE.
 *   v1: WAD-overlay disc cursor (mods/mod_wad_overlay.c).
 *   v2: + active-overlay magic — restoring by saved id instead of the RAM
 *       marker word survives cutscene-style loads that clobber the marker.
 *   v3: + guest-mutated thunk latches (CD-ready / VSync callback vaddrs,
 *       XA arm + filter): CdlSetmode flips the XA route and load code
 *       re-registers callbacks, so a rolled-back tick that ran transition
 *       code otherwise leaves them on the abandoned branch.
 *   v4: + dev-overlay level-warp FSM (features/levelwarp.c). The
 *       phase gates guest writes across several ticks, so a rewind past a
 *       half-applied warp has to restore it alongside the RAM it wrote.
 *   v5: the thunk latches are the shared layer's pack
 *       (games/common/psyq/psyq_thunk_state.h: callbacks, drive mode and filter,
 *       the interrupt- and DMA-callback tables) in place of v3's three
 *       words; the warp words follow it at the length the pack's own
 *       version word gives, so a blob saved under an earlier pack version
 *       still loads. v3/v4 blobs load through the pack's legacy adapter. */
extern uint32_t spyro1_mod_wad_cursor_get(void);
extern void     spyro1_mod_wad_cursor_set(uint32_t s);

#define EXTRA_V5_PACK_OFF 12u
#define EXTRA_V5_WARP_OFF (EXTRA_V5_PACK_OFF + PSYQ_THUNK_STATE_BYTES)
#define EXTRA_V5_SIZE     (EXTRA_V5_WARP_OFF + 12u)

uint32_t game_state_extra_size(void) { return EXTRA_V5_SIZE; }

void game_state_extra_save(void* dst) {
    uint8_t* p = (uint8_t*)dst;
    uint32_t ver = 5u, cur = spyro1_mod_wad_cursor_get();
    uint32_t ovl = spyro_get_active_overlay_magic();
    uint32_t wphase, wtarget, wticks;
    spyro1_levelwarp_state_save(&wphase, &wtarget, &wticks);
    memcpy(p +  0, &ver, 4);
    memcpy(p +  4, &cur, 4);
    memcpy(p +  8, &ovl, 4);
    psyq_thunk_state_save(p + EXTRA_V5_PACK_OFF);
    memcpy(p + EXTRA_V5_WARP_OFF + 0, &wphase, 4);
    memcpy(p + EXTRA_V5_WARP_OFF + 4, &wtarget, 4);
    memcpy(p + EXTRA_V5_WARP_OFF + 8, &wticks, 4);
}

void game_state_extra_load(const void* src, uint32_t size) {
    if (size < 8u) return;   /* older/foreign blob — leave live state alone */
    const uint8_t* p = (const uint8_t*)src;
    uint32_t ver, cur;
    memcpy(&ver, p + 0, 4);
    memcpy(&cur, p + 4, 4);
    if (ver >= 1u) spyro1_mod_wad_cursor_set(cur);
    if (ver >= 2u && size >= 12u) {
        uint32_t ovl;
        memcpy(&ovl, p + 8, 4);
        if (ovl) spyro_set_active_overlay_by_magic(ovl);
    }
    if (ver >= 5u) {
        uint32_t pack_ver, pack_len;
        if (size < EXTRA_V5_PACK_OFF + 4u) return;
        memcpy(&pack_ver, p + EXTRA_V5_PACK_OFF, 4);
        pack_len = psyq_thunk_state_bytes_for(pack_ver);
        if (pack_len == 0u || size < EXTRA_V5_PACK_OFF + pack_len + 12u) return;
        (void)psyq_thunk_state_load(p + EXTRA_V5_PACK_OFF, pack_len);
        uint32_t wphase, wtarget, wticks;
        const uint8_t* w = p + EXTRA_V5_PACK_OFF + pack_len;
        memcpy(&wphase,  w + 0, 4);
        memcpy(&wtarget, w + 4, 4);
        memcpy(&wticks,  w + 8, 4);
        spyro1_levelwarp_state_load(wphase, wtarget, wticks);
        return;
    }
    if (ver >= 3u && size >= 24u) {
        uint32_t cd_cb, vs_cb, xa;
        memcpy(&cd_cb, p + 12, 4);
        memcpy(&vs_cb, p + 16, 4);
        memcpy(&xa,    p + 20, 4);
        psyq_thunk_state_load_v4(cd_cb, vs_cb, xa);
    }
    if (ver >= 4u && size >= 36u) {
        uint32_t wphase, wtarget, wticks;
        memcpy(&wphase,  p + 24, 4);
        memcpy(&wtarget, p + 28, 4);
        memcpy(&wticks,  p + 32, 4);
        spyro1_levelwarp_state_load(wphase, wtarget, wticks);
    }
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

/* Phase 3/4 — native-decomp dispatch table. Generated from
 * manifest/native.toml by tools/gen_dispatch.py; see
 * generated/native_dispatch.c. Each [[dispatch]] entry there names a
 * decomp C function that takes over its PSX vaddr; the generator
 * emits the cpu-stash, psyq___main bootstrap (for main), arg unpacking
 * from a0..a3/stack, and return code. To add another entry point,
 * extend native.toml and re-run scripts/regen-manifest.sh. */
/* g_SonyImage / D_8006FCF4 base. Default == canonical PSX address; the
 * recompiler's value-keyed lui+addiu pair substitution is then identity.
 * A generic relocation primitive for any future consumer of the
 * env-renderer scratch pool (the --no-lod producer stack that motivated
 * it was deleted 2026-07-06; it always left this at canonical anyway).
 * Declared in psx_runtime.h. */
uint32_t psx_runtime_g_sony_image_base = 0x8006FCF4u;

extern int spyro_native_dispatch(CPUState* cpu, uint32_t addr);

/* See the OPENPETE_NOOP_SREND arm in psx_override_dispatch_game below. The mask
 * is latched on first use — this sits on the per-call dispatch path and getenv
 * is expensive on Windows. */
static int srend_noop_hit(uint32_t addr) {
    static int s_mask = -1;
    if (s_mask < 0) {
        const char* e = getenv("OPENPETE_NOOP_SREND");
        s_mask = e ? (int)strtol(e, NULL, 0) : 0;
        if (s_mask) {
            fprintf(stderr, "[noop-srend] mask=%d — renderers suppressed; "
                            "geometry and gameplay state are NOT faithful\n", s_mask);
        }
    }
    if (s_mask == 0) return 0;
    switch (addr) {
    case 0x800258F0u:                        /* world/terrain renderer      */
        return (s_mask & 1) != 0;
    case 0x8001F798u:                        /* regular material renderer   */
        /* Bits 16/32 take the two material EMIT passes separately: they build
         * prims and write no gameplay-readable byte (the cull passes own
         * m_WasDrawn), so suppressing them leaves every verdict in place —
         * the "stub the moby renderer" ceiling. Bit 1 additionally takes the
         * cull passes, which the game cannot survive. */
        if (s_mask & 16) return 1;
        return (s_mask & 2) != 0;
    case 0x80020F34u:                        /* specular material renderer  */
        if (s_mask & 32) return 1;
        return (s_mask & 2) != 0;
    case 0x8001F158u: case 0x800208FCu:      /* moby cull/gather passes     */
    case 0x80022A2Cu:                        /* shaded cull + renderer      */
        return (s_mask & 2) != 0;
    case 0x8004EBA8u:                        /* cyclorama dome              */
    case 0x80050BD0u:                        /* portal previews             */
        return (s_mask & 4) != 0;
    case 0x80023AC4u:                        /* Spyro                       */
    case 0x80058D64u:                        /* flame breath                */
    case 0x80059F8Cu: case 0x80059A48u:      /* moby + Spyro shadows        */
    case 0x800573C8u: case 0x800580F4u:      /* particles, glows            */
        return (s_mask & 8) != 0;
    default:
        return 0;
    }
}

int psx_override_dispatch_game(CPUState* cpu, uint32_t addr) {
    /* Debug free camera (F): while engaged, drive the camera in place of
     * CameraFollowSpyro (on this game fiber) and suppress it + UpdateSpyro so
     * nothing overwrites the camera and Spyro holds still. Inactive → returns
     * 0 (the hot path is one bool test), so the normal build is unaffected. */
    if (freecam_should_freeze(cpu, addr)) {
        return 1;
    }

    /* Mod override chains (notes/plans/openpete-modding-framework.md).
     * One global-flag branch when no mods are loaded (inertness gate).
     * Runs before the native-decomp/feature arms: a mod that replaces a
     * function replaces it, full stop; freecam (a user debug tool) still
     * wins above so freezing behaves identically with mods loaded. */
    if (openpete_mods_active && openpete_mods_dispatch(cpu, addr)) {
        return 1;
    }

    /* NOT trace-scoped, unlike the feature-registry hooks below: these two arms
     * run on EVERY translated-function entry, so a pair of clock reads here
     * would cost more than the thing being measured. The registry's own scopes
     * start only after an address match, which is rare. */
    if (spyro_native_dispatch(cpu, addr)) {
        return 1;
    }

    /* Feature modules registered via core/include/feature_registry.h;
     * a game with none (openpete_feature_count == 0) makes this a no-op. */
    if (feature_registry_dispatch(cpu, addr)) {
        return 1;
    }

    /* Perf attribution knob (OPENPETE_NOOP_SREND=<mask>): turn the handwritten
     * PSX renderers into no-ops so the sim tick can be measured without the
     * emulated-GTE prim building whose output native mode discards. Placed
     * AFTER the feature arms on purpose: a native feeder that hooks a renderer
     * runs its own capture/feed and then calls the real renderer directly; that
     * inner call re-enters here (the feeder's re-entrancy guard has already
     * declined) and is the call this suppresses. So the feeds keep running and
     * only the guest-side prim math goes away.
     *
     * MEASUREMENT ONLY — the renderers also write gameplay-readable state
     * (sector visibility, m_WasDrawn, the moby render queue), so a run with any
     * bit set is not gameplay-faithful and its geometry will thin out.
     *   1  world/terrain (func_800258F0)
     *   2  mobys (2 cull/gather passes + 3 material renderers)
     *   4  cyclorama dome + portal previews
     *   8  Spyro, flame, shadows, particles, glows
     *  16  regular moby prim build ONLY (verdicts left intact)
     *  32  specular moby prim build ONLY (verdicts left intact)
     */
    if (srend_noop_hit(addr)) {
        return 1;
    }

    /* Widescreen dispatch arms (0x8001ED5C render bracket, 0x8007CD38
     * titlescreen sprite, 0x800258F0 env renderer, 0x8004EBA8 cyclorama,
     * 0x80022A2C text-moby range) moved to
     * core/renderer/widescreen.c — handled by the registry call above. */
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
