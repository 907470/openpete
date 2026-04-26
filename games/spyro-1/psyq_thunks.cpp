/* games/spyro-1/psyq_thunks.cpp — PSYQ → PsyCross ABI thunks.
 *
 * The recompiler emits PSYQ-targeted JALs as `psyq_NAME(cpu)` (with
 * --external-prefix=psyq_). This file provides hand-written bodies for
 * every named PSYQ function Spyro calls during Initialize() and the main
 * loop, translating between MIPS calling convention (args in a0..a3 +
 * sp-relative slots, return in v0) and PsyCross's native C signatures.
 *
 * The auto-generated abort stubs in psyq_stubs.c are weak; these strong
 * definitions override them at link time. Anything *not* thunked here
 * still aborts — that's the development gate for "did Spyro just touch a
 * PSYQ surface we don't cover yet?"
 *
 * Compiled as C++ because PsyCross's libgpu.h uses `static_assert` on
 * struct sizes (C++11 keyword). Each thunk is wrapped in `extern "C"`
 * so the recompiler-emitted dispatch table still finds them.
 */

extern "C" {
#include "openpete_fiber.h"
#include "openpete_log.h"
#include "openpete_psx.h"
}

#include <cstdio>
#include <cstring>
#include <stdint.h>

/* PsyCross PSYQ headers. Order matters: libgte.h defines SVECTOR before
 * libgpu.h references it. */
#include "psx/libgte.h"
#include "psx/libetc.h"
#include "psx/libgpu.h"
#include "psx/libcd.h"
#include "psx/libpad.h"
#include "psx/libspu.h"
#include "psx/libmcrd.h"
#include "psx/libapi.h"
#include "psx/gtereg.h"
#include "PsyX/PsyX_public.h"

/* Internal PsyCross helpers we need to reach for the OT thunk. They live
 * in src/gpu/PsyX_GPU.h, which isn't on the public include path; redeclare
 * the two we use. Both are linkable directly from the static lib. */
extern "C" {
    void ClearSplits();
    void DrawAllSplits();
}

extern "C" {

/* --- Helpers ------------------------------------------------------------- */

/* PSX vaddr (held in cpu->aN) → host pointer via openpete_core's memory map. */
#define PTR(addr) ((void*)openpete_psx_to_host_ptr((uint32_t)(addr)))

/* PSYQ callbacks (CdReadCallback, VSyncCallback) take a PSX function
 * pointer that the compiled C wants invoked when the underlying event
 * fires. We can't call PSX code natively, so we save the address and
 * dispatch via the recompiled-function table when our thunk knows the
 * event has happened (e.g. right after PsyCross's synchronous CdRead).
 *
 * Each saved cb is dispatched on the *same* CPUState that registered it,
 * with its registers temporarily clobbered for the call. */
extern "C" int psx_dispatch_compiled(CPUState* cpu, uint32_t addr);

static uint32_t g_cd_read_cb     = 0;
static uint32_t g_vsync_cb       = 0;

static void invoke_psx_callback(CPUState* cpu, uint32_t cb_addr,
                                uint32_t a0, uint32_t a1) {
    if (cb_addr == 0) return;
    /* Save caller's a-regs / ra; the recompiled function will trash them. */
    uint32_t s_a0 = cpu->a0, s_a1 = cpu->a1, s_a2 = cpu->a2, s_a3 = cpu->a3;
    uint32_t s_ra = cpu->ra, s_v0 = cpu->v0, s_v1 = cpu->v1;
    cpu->a0 = a0; cpu->a1 = a1; cpu->ra = 0xDEADBEEF;
    if (!psx_dispatch_compiled(cpu, cb_addr)) {
        OPL_WARN_F("thunks", "callback 0x%08X has no compiled body", cb_addr);
    }
    cpu->a0 = s_a0; cpu->a1 = s_a1; cpu->a2 = s_a2; cpu->a3 = s_a3;
    cpu->ra = s_ra; cpu->v0 = s_v0; cpu->v1 = s_v1;
}

/* --- libgcc/crt0 --------------------------------------------------------- */

void psyq___main(CPUState* cpu) {
    /* libgcc constructor-runner. Real PSYQ __main at 0x8005B988:
     *   - load *(0x800730BC) — "already initialized" flag
     *   - if zero: write 1 to it (mark initialized)
     *   - walks __CTOR_LIST__ at 0x80010000 calling each ctor (loop count
     *     in s1 — Spyro has 0 ctors so the loop is skipped)
     *   - in addition, push ra at sp+12 of __main's own frame (sp -= 16)
     *
     * Without these byte-level effects, --seed-ram + --halt-at-pc
     * comparison against pcsx-redux shows the 5 bytes as diffs at the very
     * first halt point inside Initialize(). Replicating them keeps the
     * "first frame == pcsx-redux RAM" invariant intact for the diff loop
     * (see docs/ram-diff-vs-pcsxr.md). */
    const uint32_t INIT_FLAG_ADDR = 0x800730BCu;
    if (cpu->read_word(INIT_FLAG_ADDR) == 0u) {
        cpu->write_word(INIT_FLAG_ADDR, 1u);
    }
    /* Mirror __main's stack-frame use. PSYQ pushes 16 bytes (sp-=16) and
     * stores ra at sp+12. The recompiler's calling convention doesn't need
     * a frame for the thunk, but pcsx-redux executes the real MIPS code
     * which leaves these bytes on the stack; matching them gives a clean
     * byte-for-byte alignment at function-entry halt points. */
    uint32_t sp_before = cpu->sp;
    cpu->sp = sp_before - 16u;
    cpu->write_word(cpu->sp + 12u, cpu->ra);
    /* PSYQ restores sp before returning (lw ra, 12(sp); addiu sp, sp, 16). */
    cpu->sp = sp_before;
}

/* --- libetc -------------------------------------------------------------- */

void psyq_VSync(CPUState* cpu) {
    /* libetc semantics:
     *   mode  < 0 : query — return current vbl counter, no wait
     *   mode == 0 : wait for next vblank, return updated counter
     *   mode  > 0 : wait that many vblanks (Spyro doesn't seem to use this)
     *
     * Phase 1d: instead of busy-waiting in PsyCross's VSync, we yield to the
     * host fiber, which presents the current scene + advances openpete_frame
     * and resumes us. `openpete_frame` is the frame counter the game sees. */
    int mode = (int)cpu->a0;
    static uint32_t calls = 0u;
    if ((++calls & 0x3FFu) == 1u) {
        OPL_INFO_F("thunks", "VSync(%d) frame=%u (call #%u)",
                   mode, openpete_frame, calls);
    }
    /* Cutscene tick / DeltaTime trace — see plans/loading-text-spike-triangles.md
     * task #4. Logs g_CutsceneIdx, g_CutsceneLayout->m_CurrentTick, and
     * g_DeltaTime once per VSync(0) in the loading-text window. Addresses are
     * for the openpete-bundled spyro-1 build (psx.map @ 3fdc740). */
    if (mode == 0 && openpete_frame >= 1050u && openpete_frame <= 1700u) { /* TODO remove */
        auto safe_read32 = [](uint32_t addr) -> uint32_t {
            if (addr < 0x80000000u || addr >= 0x80200000u) return 0xFFFFFFFFu;
            void* p = PTR(addr);
            return p ? *(uint32_t*)p : 0xFFFFFFFFu;
        };
        /* Per spyro-1/build/psx.map (3fdc740): the openpete-bundled
         * games/spyro-1/spyro-1/build/psx.map is stale (.main shown as 0x32000;
         * actual EXE size in the binary header is 0x65800), so ignore it and
         * use the up-to-date addresses from the live matching-decomp tree at
         * <spyro>/spyro-1/build/. */
        const uint32_t kGamestateAddr   = 0x800757D8u; /* g_Gamestate */
        const uint32_t kTSStateBase     = 0x80078D78u; /* g_TitlescreenState */
        /* g_SonyImage @ 0x8006FCF4. Layout:
         *   [0..0x1C00]   u_char m_Buf / DrawStateSonyBuf (union)
         *   0x1C00..0x2000  WorldNearQueued[256]  (1024)
         *   0x2000..0x2400  WorldFarQueued[256]   (1024)
         *   0x2400..0x2800  ShadedMobys[256]      (1024) ← here
         *   0x2800..        unk
         */
        const uint32_t kShadedMobysAddr = 0x8006FCF4u + 0x2400u; /* = 0x800720F4 */
        int gs       = (int)safe_read32(kGamestateAddr);
        int ts_state = (int)safe_read32(kTSStateBase + 4u);   /* m_State */
        int ts_tick  = (int)safe_read32(kTSStateBase + 8u);   /* m_Tick */
        OPL_INFO_F("text-dump",
                   "f%u VSync(0) gs=%d tsState=%d tsTick=%d",
                   openpete_frame, gs, ts_state, ts_tick);
        if (openpete_frame == 1309u) {
            /* Walk g_SonyImage.m_ShadedMobys[] and dump each letter moby. */
            for (int si = 0; si < 32; si++) {
                uint32_t pMoby = safe_read32(kShadedMobysAddr + (uint32_t)si * 4u);
                if (pMoby == 0u) {
                    OPL_INFO_F("text-dump", "  ShadedMoby[%d] = NULL (end)", si);
                    break;
                }
                if (pMoby < 0x80000000u || pMoby >= 0x80200000u) {
                    OPL_INFO_F("text-dump", "  ShadedMoby[%d] = 0x%08X (bad)",
                               si, pMoby);
                    continue;
                }
                int px = (int)safe_read32(pMoby + 12u);
                int py = (int)safe_read32(pMoby + 16u);
                int pz = (int)safe_read32(pMoby + 20u);
                /* Per Moby struct: m_Class is a short at offset 54 (after
                 * m_RotationMatrix 32..51 + m_CollisionRegion at 52..53). */
                uint32_t cls_word = safe_read32(pMoby + 52u);
                int cls = (int)((int16_t)((cls_word >> 16) & 0xFFFFu));
                /* m_Rotation Vector3D8 (3 chars). After Pod (u_char) at some
                 * earlier offset; for now just dump 4 bytes near the expected
                 * region for sanity. The exact byte offset of m_Rotation can
                 * shift due to AnimationState packing — print row near 64. */
                uint32_t row64 = safe_read32(pMoby + 64u);
                uint32_t row68 = safe_read32(pMoby + 68u);
                OPL_INFO_F("text-dump",
                           "  ShadedMoby[%d]@0x%08X cls=%d pos=(%d,%d,%d) "
                           "row64=0x%08X row68=0x%08X",
                           si, pMoby, cls, px, py, pz, row64, row68);
            }
        }
    }
    /* Level-1 game-over probe (task #9): after the 2nd loading transition the
     * screen flashes and game-over fires within seconds. Confirmed via earlier
     * coarse trace: gs jumps 14 → 13 → 4 (Respawn) at f~2340 with Spyro
     * teleported to (84992,23542,-113244). Now zoom in on the transition. */
    if (mode == 0 && openpete_frame >= 1500u) {
        auto sr32 = [](uint32_t addr) -> uint32_t {
            if (addr < 0x80000000u || addr >= 0x80200000u) return 0xFFFFFFFFu;
            void* p = PTR(addr); return p ? *(uint32_t*)p : 0xFFFFFFFFu;
        };
        int gs        = (int)sr32(0x800757D8u);            /* g_Gamestate */
        int level     = (int)sr32(0x8007596Cu);            /* g_LevelId */
        int lives     = (int)sr32(0x8007582Cu);            /* g_SpyroLifeCount */
        int sx        = (int)sr32(0x80078A58u +  0u);      /* m_Position.x */
        int sy        = (int)sr32(0x80078A58u +  4u);      /* m_Position.y */
        int sz        = (int)sr32(0x80078A58u +  8u);      /* m_Position.z */
        int health    = (int)sr32(0x80078A58u + 0x164u);   /* m_health */

        /* Log transitions (gs change OR lives change) at full resolution. */
        static int s_prev_gs    = -1;
        static int s_prev_lives = -1;
        if (gs != s_prev_gs || lives != s_prev_lives) {
            OPL_INFO_F("lvl1-fall",
                       "TRANSITION f%u gs=%d->%d lvl=%d hp=%d lives=%d->%d "
                       "pos=(%d,%d,%d)",
                       openpete_frame,
                       s_prev_gs, gs, level, health,
                       s_prev_lives, lives, sx, sy, sz);
            s_prev_gs    = gs;
            s_prev_lives = lives;
        }

        /* Periodic snapshot every 30 frames as before. */
        if ((openpete_frame % 30u) == 0u) {
            OPL_INFO_F("lvl1-fall",
                       "f%u gs=%d lvl=%d hp=%d lives=%d pos=(%d,%d,%d)",
                       openpete_frame, gs, level, health, lives, sx, sy, sz);
        }
    }
    if (mode == 0) {
        if (openpete_fast_boot_until_frame > 0 &&
            (int)openpete_frame < openpete_fast_boot_until_frame) {
            /* Fast-boot: bump the counter without yielding so the game's
             * Initialize() busy-waits run as fast as possible. Nothing is
             * being rendered during these waits anyway (Sony+Universal
             * splash windows are intentional VSync(-1) spins).
             * Skip the VSync callback during fast-boot too: PadVSync would
             * be writing to g_PadBuffer based on pre-input state, which is
             * meaningless at boot and risks committing stale presses. */
            openpete_frame++;
        } else if (openpete_host_fiber) {
            /* Spyro 1 has variable iter rate based on real-PSX work_time:
             *
             *   Light scenes (loading-screen flyby = 22 letters + Spyro
             *   model; title menu) fit in <1 vblank → real PSX iter = 1
             *   vblank = 60Hz.
             *
             *   Heavy scenes (gameplay + cutscene playback) have ~1.5
             *   vblanks of work, so VSync(0) rounds up to 2 vblanks =
             *   30Hz.
             *
             * Modern CPUs do all work in <1ms regardless, so without
             * intervention every scene runs at 60Hz iter rate (= 1 host
             * frame per yield). For 60Hz scenes that's correct; for
             * 30Hz scenes it doubles per-iter counters that don't go
             * through DeltaTime (g_GameTick++, etc.).
             *
             * Approach: yield 2x by default to match the heavy-scene
             * 30Hz cadence, drop to 1x for known light scenes.
             *
             * Light-scene detection: g_Gamestate==13 (GS_TitleScreen)
             * AND g_TitlescreenState.m_Mode==3 (TSM_Demo) AND
             * m_State==2 (TSS_Active) — that's the loading-screen
             * flyby specifically. Reads the same probe addresses as
             * the diagnostic trace below (validated against the up-to-
             * date matching-decomp psx.map).
             *
             * TODO: plumb a per-game frame-rate predicate into
             * game_extras_impl.c so this isn't hardcoded in the thunk.
             */
            int yield_count = 2;
            {
                auto sr32 = [](uint32_t addr) -> uint32_t {
                    if (addr < 0x80000000u || addr >= 0x80200000u) return 0u;
                    void* p = PTR(addr); return p ? *(uint32_t*)p : 0u;
                };
                int gs       = (int)sr32(0x800757D8u);             /* g_Gamestate */
                int tsm_mode = (int)sr32(0x80078D78u +  0u);       /* m_Mode */
                int tsm_st   = (int)sr32(0x80078D78u +  4u);       /* m_State */
                if (gs == 13 && tsm_mode == 3 && tsm_st == 2) {
                    yield_count = 1;  /* loading-screen flyby — 60Hz */
                }
            }
            for (int yi = 0; yi < yield_count; yi++) {
                openpete_yield_to_host();
                if (g_vsync_cb) invoke_psx_callback(cpu, g_vsync_cb, 0, 0);
            }
        } else {
            /* No fiber loop running (smoke-test path) — fall back to PsyCross's
             * busy wait so semantics still match. */
            (void)VSync(0);
            if (g_vsync_cb) invoke_psx_callback(cpu, g_vsync_cb, 0, 0);
        }
    }
    cpu->v0 = (uint32_t)openpete_frame;
}

void psyq_VSyncCallback(CPUState* cpu) {
    /* Save the cb address; psyq_VSync invokes it once per call (Phase 1c
     * doesn't drive a real 60Hz timer — VSync()'s thunk fires it when the
     * compiled main loop yields). */
    OPL_INFO_F("thunks", "VSyncCallback registered: 0x%08X", cpu->a0);
    g_vsync_cb = cpu->a0;
    cpu->v0 = 0u;
}

void psyq_StopCallback(CPUState* cpu) {
    cpu->v0 = (uint32_t)StopCallback();
}

/* --- libgpu -------------------------------------------------------------- */

void psyq_ResetGraph(CPUState* cpu) {
    cpu->v0 = (uint32_t)ResetGraph((int)cpu->a0);
}

void psyq_SetDispMask(CPUState* cpu) {
    SetDispMask((int)cpu->a0);
}

void psyq_SetGraphDebug(CPUState* cpu) {
    cpu->v0 = (uint32_t)SetGraphDebug((int)cpu->a0);
}

void psyq_DrawSync(CPUState* cpu) {
    cpu->v0 = (uint32_t)DrawSync((int)cpu->a0);
}

void psyq_ClearImage(CPUState* cpu) {
    cpu->v0 = (uint32_t)ClearImage((RECT16*)PTR(cpu->a0),
                                   (u_char)cpu->a1,
                                   (u_char)cpu->a2,
                                   (u_char)cpu->a3);
}

void psyq_LoadImage(CPUState* cpu) {
    RECT16* r = (RECT16*)PTR(cpu->a0);
    /* Log for the loading-text window so we can correlate VRAM upload
     * timing with the fly-in animation. See plans/loading-text-spike-
     * triangles.md. */
    if (openpete_frame >= 1280 && openpete_frame <= 1315 && r) {
        OPL_INFO_F("text-dump",
                   "f%u LoadImage rect=(%d,%d %dx%d) src=0x%08X",
                   openpete_frame, r->x, r->y, r->w, r->h,
                   (unsigned)cpu->a1);
    }
    cpu->v0 = (uint32_t)LoadImage(r, (u_long*)PTR(cpu->a1));
}

void psyq_StoreImage(CPUState* cpu) {
    cpu->v0 = (uint32_t)StoreImage((RECT16*)PTR(cpu->a0),
                                   (u_long*)PTR(cpu->a1));
}

void psyq_MoveImage(CPUState* cpu) {
    RECT16* r = (RECT16*)PTR(cpu->a0);
    if (openpete_frame >= 1280 && openpete_frame <= 1315 && r) {
        OPL_INFO_F("text-dump",
                   "f%u MoveImage rect=(%d,%d %dx%d) -> dst=(%d,%d)",
                   openpete_frame, r->x, r->y, r->w, r->h,
                   (int)cpu->a1, (int)cpu->a2);
    }
    cpu->v0 = (uint32_t)MoveImage(r, (int)cpu->a1, (int)cpu->a2);
}

void psyq_PutDispEnv(CPUState* cpu) {
    DISPENV* env = (DISPENV*)PTR(cpu->a0);
    if (openpete_frame >= 1305 && openpete_frame <= 1315 && env) {
        OPL_INFO_F("text-dump",
                   "f%u PutDispEnv disp=(%d,%d %dx%d) screen=(%d,%d %dx%d) isinter=%d isrgb24=%d",
                   openpete_frame,
                   env->disp.x, env->disp.y, env->disp.w, env->disp.h,
                   env->screen.x, env->screen.y, env->screen.w, env->screen.h,
                   (int)env->isinter, (int)env->isrgb24);
    }
    PutDispEnv(env);
}

void psyq_PutDrawEnv(CPUState* cpu) {
    DRAWENV* env = (DRAWENV*)PTR(cpu->a0);
    /* Log clip rect during the loading-text frame window. If clip-bottom
     * < ~140 then PSX would have scissored the spike-quad tips and openpete
     * needs to do the same. */
    if (openpete_frame >= 1305 && openpete_frame <= 1315) {
        OPL_INFO_F("text-dump",
                   "f%u PutDrawEnv clip=(%d,%d %dx%d) ofs=(%d,%d) "
                   "tw=(%d,%d %dx%d) tpage=0x%04X dfe=%d isbg=%d "
                   "r0=%u g0=%u b0=%u",
                   openpete_frame,
                   env->clip.x, env->clip.y, env->clip.w, env->clip.h,
                   env->ofs[0], env->ofs[1],
                   env->tw.x, env->tw.y, env->tw.w, env->tw.h,
                   (unsigned)env->tpage,
                   (int)env->dfe, (int)env->isbg,
                   (unsigned)env->r0, (unsigned)env->g0, (unsigned)env->b0);
    }
    PutDrawEnv(env);
}

void psyq_SetDefDispEnv(CPUState* cpu) {
    SetDefDispEnv((DISPENV*)PTR(cpu->a0),
                  (int)cpu->a1, (int)cpu->a2, (int)cpu->a3,
                  (int)cpu->read_word(cpu->sp + 0x10));
}

void psyq_SetDefDrawEnv(CPUState* cpu) {
    SetDefDrawEnv((DRAWENV*)PTR(cpu->a0),
                  (int)cpu->a1, (int)cpu->a2, (int)cpu->a3,
                  (int)cpu->read_word(cpu->sp + 0x10));
}

void psyq_SetDrawMode(CPUState* cpu) {
    /* DR_MODE* p, int dfe, int dtd, int tpage, RECT16* tw — five args. */
    SetDrawMode((DR_MODE*)PTR(cpu->a0),
                (int)cpu->a1, (int)cpu->a2, (int)cpu->a3,
                (RECT16*)PTR(cpu->read_word(cpu->sp + 0x10)));
}

/* OT-translation scratch buffer.
 *
 * The recompiled MIPS code writes 4-byte PSX P_TAGs (`addr:24 | len:8`) and
 * 4-byte-aligned primitive payloads. PsyCross compiled with
 * USE_EXTENDED_PRIM_POINTERS=1 (the default on x86_64) expects 12-byte P_TAG
 * headers (`uintptr_t addr; uint16_t len; uint16_t pgxp_index`) followed by
 * the same payload — i.e. the payload format is identical, only the header
 * size differs. Walking the PSX chain with PsyCross macros dereferences a
 * 24-bit field as a host pointer and SEGVs.
 *
 * Each frame we rebuild the chain in host scratch memory: one PsyCross-style
 * P_TAG slot per PSX entry, payload memcpy'd verbatim, addr fields chained to
 * the next host slot, last slot pointed at PsyCross's `prim_terminator`. */
static uint8_t  g_ot_scratch[2u * 1024u * 1024u];
static size_t   g_ot_scratch_used = 0;
static uint8_t* g_ot_slot_host[16384];
static int      g_ot_slot_count = 0;

void psyq_DrawOTag(CPUState* cpu) {
    g_ot_scratch_used = 0;
    g_ot_slot_count = 0;

    static uint32_t s_calls = 0;
    ++s_calls;
    if ((s_calls & 0xFFFu) == 1u) {
        OPL_INFO_F("thunks", "DrawOTag(#%u) ot=0x%08X ra=0x%08X",
                   s_calls, cpu->a0, cpu->ra);
    }

    /* Per-OT primitive-code histogram. Indexed by (code & 0xF0) >> 4 so
     * we can see at a glance whether the chain contains polys (0x20/0x30),
     * lines (0x40/0x50), sprites/tiles (0x60/0x70), DR_LOAD (0xA0),
     * draw-env (0xE0), or unknown. Also tracks the last frame DrawOTag
     * fired in, so we can confirm whether the cutscene main loop keeps
     * issuing draws past the early-boot window. Dumped every 60 frames. */
    static uint32_t s_hist[16] = {0};
    static uint32_t s_anchor_slots = 0;
    static uint32_t s_total_slots  = 0;
    static uint32_t s_hist_calls   = 0;
    static uint32_t s_last_frame   = 0xFFFFFFFFu;
    static uint32_t s_last_dump    = 0;

    /* Diagnostic: payload dump for the loading-transition fly-in text frames
     * (~1305..1315). Captures up to 64 prims/frame as hex so we can decode
     * vertex XY and decide whether bad geometry is GPU-side or upstream. See
     * notes/plans/loading-text-spike-triangles.md. */
    static const uint32_t kTextDumpFrameMin = 1305;
    static const uint32_t kTextDumpFrameMax = 1315;
    static int            s_text_dumped_this_frame = 0;

    if (openpete_frame != s_last_frame) {
        s_last_frame = openpete_frame;
        s_text_dumped_this_frame = 0;
    }

    uint32_t addr = cpu->a0;
    while (g_ot_slot_count < (int)(sizeof(g_ot_slot_host) / sizeof(g_ot_slot_host[0]))) {
        if (addr == 0u) break;
        uint8_t* host = (uint8_t*)openpete_psx_to_host_ptr(addr);
        if (!host) break;

        uint32_t tag;
        std::memcpy(&tag, host, 4);
        uint8_t  plen = (uint8_t)((tag >> 24) & 0xFFu);
        uint32_t next = tag & 0x00FFFFFFu;

        /* Payload-size estimate. The PSX P_TAG length is 8-bit (max 1020 bytes
         * payload), but variable-length primitives like LoadImage (code 0xA0)
         * embed image data right after the struct that doesn't necessarily
         * fit in 8 bits — the original GPU reads it via the rect's w×h. We
         * peek at the code byte (offset 7 in PSX layout: tag@0..3, code@4..7
         * with the high byte being the GP0 command code) and compute extra
         * bytes from the embedded rect when needed. */
        /* Only extend payload for real primitives (plen > 0). OT anchor
         * entries have plen == 0; their post-tag bytes may contain stale
         * data left from the previous frame and must not be interpreted. */
        size_t payload_bytes = (size_t)plen * 4u;
        if (plen >= 3) {
            uint8_t code_byte = host[7];
            if ((code_byte & 0xF0u) == 0xA0u) {
                /* DR_LOAD: code[0]=cmd, code[1]=xy, code[2]=wh; image data
                 * follows. Need at least 12 (code) + w*h*2 bytes. */
                uint32_t wh;
                std::memcpy(&wh, host + 4 + 8, 4);  /* code[2] @ offset tag(4)+code[0..1](8) */
                uint32_t w = wh & 0xFFFFu;
                uint32_t h = (wh >> 16) & 0xFFFFu;
                size_t img_bytes = (size_t)w * (size_t)h * 2u;
                size_t needed    = 12u + img_bytes;
                if (needed > payload_bytes) payload_bytes = needed;
            }
        }

        size_t slot_size = 12u + payload_bytes;
        if (g_ot_scratch_used + slot_size > sizeof(g_ot_scratch)) {
            OPL_WARN_F("thunks", "DrawOTag scratch overflow at slot %d (PSX 0x%08X)",
                       g_ot_slot_count, addr);
            break;
        }
        uint8_t* out = g_ot_scratch + g_ot_scratch_used;
        g_ot_scratch_used += slot_size;
        g_ot_slot_host[g_ot_slot_count++] = out;

        /* Decide whether this PSX entry encodes a real GP0 primitive. The
         * non-singlePrimitive ParsePrimitivesLinkedList walker walks the slot
         * via `currentPacket += (primLength + P_LEN) * 4` where primLength is
         * what `ProcessXxx` returned, and stops only at `endPacket =
         * basePacket + (lenField + P_LEN) * 4`. If lenField doesn't equal the
         * expected primLength for this code, the walker either short-circuits
         * (skipping payload) or overshoots and parses garbage as a fresh
         * P_TAG — which has crashed runs by interpreting random bytes as a
         * DR_LOAD (0xA0) and SEGV'ing inside `LoadImage`.
         *
         * Spyro's PSX-side `plen` (high byte of P_TAG) sometimes diverges
         * from PsyCross's hardcoded primLength — so derive lenField from the
         * code byte itself, falling back to lenField=0 (anchor / skip) for
         * unknown codes. */
        uint8_t  code_byte = (plen != 0u) ? host[7] : 0u;
        uint16_t expected_longs = 0u;
        if (plen != 0u) {
            uint8_t hi = code_byte & 0xF0u;
            uint8_t sub = code_byte & 0x0Cu;
            switch (hi) {
                case 0x00:
                    if ((code_byte & 0x0Fu) == 0x00u) expected_longs = 3u;       /* NOP */
                    else if ((code_byte & 0x0Fu) == 0x01u) expected_longs = 5u;  /* DR_MOVE */
                    break;
                case 0x20: /* flat polys */
                    if      (sub == 0x0u) expected_longs = 4u;   /* POLY_F3 */
                    else if (sub == 0x4u) expected_longs = 7u;   /* POLY_FT3 */
                    else if (sub == 0x8u) expected_longs = 5u;   /* POLY_F4 */
                    else if (sub == 0xCu) expected_longs = 9u;   /* POLY_FT4 */
                    break;
                case 0x30: /* gouraud polys */
                    if      (sub == 0x0u) expected_longs = 6u;   /* POLY_G3 */
                    else if (sub == 0x4u) expected_longs = 9u;   /* POLY_GT3 */
                    else if (sub == 0x8u) expected_longs = 8u;   /* POLY_G4 */
                    else if (sub == 0xCu) expected_longs = 12u;  /* POLY_GT4 */
                    break;
                case 0x40: /* flat lines */
                    /* 0x40 LineF2=3, 0x48 LineF3=5, 0x4C LineF4=6 */
                    if      ((code_byte & 0x0Eu) == 0x00u) expected_longs = 3u;
                    else if ((code_byte & 0x0Eu) == 0x08u) expected_longs = 5u;
                    else if ((code_byte & 0x0Eu) == 0x0Cu) expected_longs = 6u;
                    break;
                case 0x50: /* gouraud lines */
                    if      ((code_byte & 0x0Eu) == 0x00u) expected_longs = 4u;  /* LineG2 */
                    else if ((code_byte & 0x0Eu) == 0x08u) expected_longs = 7u;  /* LineG3 */
                    else if ((code_byte & 0x0Eu) == 0x0Cu) expected_longs = 9u;  /* LineG4 */
                    break;
                case 0x60: /* TILE / SPRT free-size */
                    /* 0x60=TILE(3), 0x64=SPRT(4), 0x68=TILE_1(2), 0x6C=SPRT(4) */
                    if      ((code_byte & 0x0Cu) == 0x0u) expected_longs = 3u;
                    else if ((code_byte & 0x0Cu) == 0x4u) expected_longs = 4u;
                    else if ((code_byte & 0x0Cu) == 0x8u) expected_longs = 2u;
                    break;
                case 0x70: /* TILE_8/16, SPRT_8/16 */
                    /* 0x70=TILE_8(2), 0x74=SPRT_8(3), 0x78=TILE_16(2), 0x7C=SPRT_16(3) */
                    if      ((code_byte & 0x0Cu) == 0x0u) expected_longs = 2u;
                    else if ((code_byte & 0x0Cu) == 0x4u) expected_longs = 3u;
                    else if ((code_byte & 0x0Cu) == 0x8u) expected_longs = 2u;
                    else if ((code_byte & 0x0Cu) == 0xCu) expected_longs = 3u;
                    break;
                case 0xA0: /* DR_LOAD — variable; getlen returns slot's lenField */
                    expected_longs = (uint16_t)(payload_bytes / 4u);
                    break;
                case 0xE0: {
                    /* DR_TPAGE / DR_MODE / DR_TWIN / DR_AREA / DR_OFFSET / DR_STP
                     * — variable-length env packets. PSYQ's macros sometimes
                     * over-allocate `plen`: e.g. `setDrawMode` calls
                     * `setlen(p, 3)` while only writing `code[0]=E1xxxxx` and
                     * `code[1]=E2xxxxx` (code[2] is uninitialized memory).
                     * PsyCross's `ProcessDrawEnv` walks `polyTag->len` longs OR
                     * stops at the first subtype-0 long, whichever comes first;
                     * the OUTER walker then expects `primLength == tagLength`
                     * to land on `endPacket`.
                     *
                     * To make those match we mimic ProcessDrawEnv's stop-rule
                     * here and set `lenField` to the actual count of valid env
                     * codes (subtypes 1..6). Anything else (subtype-0,
                     * uninitialized 0xFF padding, etc.) terminates the walk.
                     *
                     * Earlier this branch dropped 0xE0 prims as anchors,
                     * which silently lost any DR_TPAGE/DR_MODE the game
                     * injects mid-frame to switch texture pages. Spyro's
                     * cutscene OT chains don't currently exercise this (the
                     * Ex column of psyq_DrawOTag's histogram stays at 0
                     * through LoadCutscene), but other game states do, so
                     * leaving env codes intact is the right default. */
                    uint32_t env_count = 0u;
                    uint32_t scan_max  = (uint32_t)plen;
                    if (scan_max > 16u) scan_max = 16u;
                    for (uint32_t i = 0; i < scan_max; ++i) {
                        uint32_t code_word;
                        std::memcpy(&code_word, host + 4 + i * 4u, 4);
                        uint32_t sub = (code_word >> 24) & 0x0Fu;
                        if (sub == 0u || sub > 6u) break;
                        ++env_count;
                    }
                    expected_longs = (uint16_t)env_count;
                    break;
                }
                default:
                    break;
            }
        }

        bool is_known_prim = (expected_longs != 0u);
        uint16_t lenField  = is_known_prim ? expected_longs : 0u;
        uint16_t pgxpField = 0xFFFFu;
        std::memcpy(out + 8,  &lenField,  2);
        std::memcpy(out + 10, &pgxpField, 2);

        /* Frame-windowed payload dump. Logs hex of the first up-to-12 longs
         * after the 4-byte PSX P_TAG, so the consumer can decode vertex XY
         * for POLY_GT3/GT4/G3/G4 (longs 1, 4, 7[, 10]). */
        if (is_known_prim &&
            openpete_frame >= kTextDumpFrameMin &&
            openpete_frame <= kTextDumpFrameMax &&
            s_text_dumped_this_frame < 6000) {
            char hex[256];
            size_t off = 0;
            uint32_t n = expected_longs < 12u ? (uint32_t)expected_longs : 12u;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t w;
                std::memcpy(&w, host + 4u + i * 4u, 4);
                int wrote = std::snprintf(hex + off, sizeof(hex) - off,
                                          "%08X ", w);
                if (wrote <= 0 || (size_t)wrote >= sizeof(hex) - off) break;
                off += (size_t)wrote;
            }
            OPL_INFO_F("text-dump",
                       "f%u slot%d psx=0x%08X code=0x%02X len=%u: %s",
                       openpete_frame, g_ot_slot_count, addr,
                       (unsigned)code_byte, (unsigned)expected_longs, hex);
            s_text_dumped_this_frame++;
        }
        if (payload_bytes != 0u) {
            std::memcpy(out + 12, host + 4, payload_bytes);
            if (is_known_prim) {
                openpete_primitives_drawn++;
                s_hist[(code_byte & 0xF0u) >> 4u]++;
                s_total_slots++;
            } else {
                s_anchor_slots++;
            }
        } else {
            s_anchor_slots++;
        }

        if (next == 0x00FFFFFFu) break;
        addr = next | 0x80000000u;
    }

    /* Patch each slot's addr to point at the next slot's host pointer; last
     * slot terminates with `prim_terminator`. */
    for (int i = 0; i < g_ot_slot_count; i++) {
        uintptr_t next_host = (i + 1 < g_ot_slot_count)
            ? (uintptr_t)g_ot_slot_host[i + 1]
            : (uintptr_t)&prim_terminator;
        std::memcpy(g_ot_slot_host[i], &next_host, sizeof(uintptr_t));
    }

    /* Walk the host-side chain in non-singlePrimitive mode and let PsyCross
     * batch splits across slots. The earlier DrawPrim-per-slot path forced
     * `g_DrawPrimMode=1` on every slot which fragmented vertex batches and
     * (empirically) prevented any geometry from reaching pixels even though
     * the chain contained tens of thousands of valid POLY_G3/POLY_GT4 prims.
     *
     * Each slot's `len` field is the primitive's PSX-side P_TAG length, which
     * matches the primLength returned by ProcessGouraudPoly/ProcessFlatPoly,
     * so the inner `(primLength + P_LEN) * 4` advance lands cleanly on the
     * next slot — no over-walk into scratch. */
    if (g_ot_slot_count > 0) {
        if (PsyX_BeginScene()) {
            ClearSplits();
        }
        extern void ParsePrimitivesLinkedList(u_long* p, int singlePrimitive);
        ParsePrimitivesLinkedList((u_long*)g_ot_slot_host[0], 0);
        DrawAllSplits();
        /* Tell the host loop this iter's GL backbuffer holds fresh game pixels.
         * Spyro is a 30fps game: it calls VSync(0) twice per logical frame, so
         * if the host loop swapped on every yield we'd flash the bare clear-
         * colour every other frame. The host now only EndScene/SwapWindow when
         * this flag is set. */
        openpete_present_pending = 1;
    }

    s_hist_calls++;
    if (openpete_frame >= s_last_dump + 60u) {
        s_last_dump = openpete_frame;
        OPL_INFO_F("thunks",
                   "DrawOTag@f%u calls=%u prims=%u anchors=%u hist "
                   "0x:%u 1x:%u 2x:%u 3x:%u 4x:%u 5x:%u 6x:%u 7x:%u "
                   "8x:%u 9x:%u Ax:%u Bx:%u Cx:%u Dx:%u Ex:%u Fx:%u",
                   openpete_frame, s_hist_calls, s_total_slots, s_anchor_slots,
                   s_hist[0],  s_hist[1],  s_hist[2],  s_hist[3],
                   s_hist[4],  s_hist[5],  s_hist[6],  s_hist[7],
                   s_hist[8],  s_hist[9],  s_hist[10], s_hist[11],
                   s_hist[12], s_hist[13], s_hist[14], s_hist[15]);
        for (int i = 0; i < 16; i++) s_hist[i] = 0;
        s_anchor_slots = 0;
        s_total_slots  = 0;
        s_hist_calls   = 0;
    }
}

/* --- libgte -------------------------------------------------------------- */

/* GTE state lives in two parallel stores: psxrecomp's `cpu->gte_data[]` /
 * `cpu->gte_ctrl[]` (used by `gte_execute()` for COP2 instructions in the
 * recompiled MIPS) and PsyCross's `gteRegs.CP2D` / `gteRegs.CP2C` (used
 * by libgte funcs like SetTransMatrix). They are NOT synchronized.
 *
 * Without bridging, this manifests as TR=(0,0,0) on every RTPS — Spyro
 * calls `SetTransMatrix` (writes PsyCross gteRegs), then RTPS via the
 * recompiler (reads cpu->gte_ctrl, sees zeros). Sync at the boundary. */
static void gte_sync_cpu_to_psyx(CPUState* cpu) {
    for (int i = 0; i < 32; i++) {
        gteRegs.CP2D.p[i].d = cpu->gte_data[i];
        gteRegs.CP2C.p[i].d = cpu->gte_ctrl[i];
    }
}
static void gte_sync_psyx_to_cpu(CPUState* cpu) {
    for (int i = 0; i < 32; i++) {
        cpu->gte_data[i] = gteRegs.CP2D.p[i].d;
        cpu->gte_ctrl[i] = gteRegs.CP2C.p[i].d;
    }
}

void psyq_InitGeom(CPUState* cpu) {
    gte_sync_cpu_to_psyx(cpu);
    InitGeom();
    gte_sync_psyx_to_cpu(cpu);
}

void psyq_SetGeomOffset(CPUState* cpu) {
    gte_sync_cpu_to_psyx(cpu);
    SetGeomOffset((int)cpu->a0, (int)cpu->a1);
    gte_sync_psyx_to_cpu(cpu);
}

void psyq_SetGeomScreen(CPUState* cpu) {
    gte_sync_cpu_to_psyx(cpu);
    SetGeomScreen((int)cpu->a0);
    gte_sync_psyx_to_cpu(cpu);
}

void psyq_SetTransMatrix(CPUState* cpu) {
    gte_sync_cpu_to_psyx(cpu);
    SetTransMatrix((MATRIX*)PTR(cpu->a0));
    gte_sync_psyx_to_cpu(cpu);
}

void psyq_MulMatrix0(CPUState* cpu) {
    gte_sync_cpu_to_psyx(cpu);
    MulMatrix0((MATRIX*)PTR(cpu->a0),
               (MATRIX*)PTR(cpu->a1),
               (MATRIX*)PTR(cpu->a2));
    gte_sync_psyx_to_cpu(cpu);
}

void psyq_MulMatrix(CPUState* cpu) {
    gte_sync_cpu_to_psyx(cpu);
    MulMatrix((MATRIX*)PTR(cpu->a0), (MATRIX*)PTR(cpu->a1));
    gte_sync_psyx_to_cpu(cpu);
}

/* --- libpad -------------------------------------------------------------- */

void psyq_PadInitDirect(CPUState* cpu) {
    PadInitDirect((unsigned char*)PTR(cpu->a0),
                  (unsigned char*)PTR(cpu->a1));
}

void psyq_PadStartCom(CPUState* cpu) {
    (void)cpu;
    PadStartCom();
}

void psyq_PadStopCom(CPUState* cpu) {
    (void)cpu;
    PadStopCom();
}

void psyq_PadGetState(CPUState* cpu) {
    cpu->v0 = (uint32_t)PadGetState((int)cpu->a0);
}

void psyq_PadInfoMode(CPUState* cpu) {
    cpu->v0 = (uint32_t)PadInfoMode((int)cpu->a0, (int)cpu->a1, (int)cpu->a2);
}

void psyq_PadSetMainMode(CPUState* cpu) {
    /* PsyCross: UNIMPLEMENTED. Treat as success so GamepadInitialize moves on. */
    (void)cpu;
    cpu->v0 = 1u;
}

void psyq_PadSetActAlign(CPUState* cpu) {
    cpu->v0 = (uint32_t)PadSetActAlign((int)cpu->a0,
                                       (unsigned char*)PTR(cpu->a1));
}

void psyq_PadSetAct(CPUState* cpu) {
    PadSetAct((int)cpu->a0,
              (unsigned char*)PTR(cpu->a1),
              (int)cpu->a2);
}

/* --- libcd --------------------------------------------------------------- */

void psyq_CdInit(CPUState* cpu) {
    int rc = CdInit();
    OPL_INFO_F("thunks", "CdInit -> %d", rc);
    cpu->v0 = (uint32_t)rc;
}

void psyq_CdControl(CPUState* cpu) {
    OPL_INFO_F("thunks", "CdControl com=0x%02X param=0x%08X result=0x%08X",
               (unsigned)cpu->a0, cpu->a1, cpu->a2);
    cpu->v0 = (uint32_t)CdControl((u_char)cpu->a0,
                                  (u_char*)PTR(cpu->a1),
                                  (u_char*)PTR(cpu->a2));
}

void psyq_CdControlB(CPUState* cpu) {
    cpu->v0 = (uint32_t)CdControlB((u_char)cpu->a0,
                                   (u_char*)PTR(cpu->a1),
                                   (u_char*)PTR(cpu->a2));
}

/* PsyCross internals we need to keep coherent across the data-read /
 * music-position split. `g_CD_com` is the "last command" int CdLastCom()
 * returns. `readMode` is the enum CdReadSync switches on (0 = RM_DATA,
 * 1 = RM_XA_AUDIO). Both are file-scope (non-static) globals in
 * third_party/PsyCross/src/psx/LIBCD.C, so we link to them by name. */
extern "C" int g_CD_com;
extern "C" int readMode;  /* enum ReadMode; 0=RM_DATA, 1=RM_XA_AUDIO */

void psyq_CdControlF(CPUState* cpu) {
    u_char com = (u_char)cpu->a0;
    /* PsyCross's CdControlF treats CdlGetlocP as "the caller is starting
     * CDDA/XA streaming" and flips readMode to RM_XA_AUDIO. Spyro's music
     * loop calls CdControlF(CdlGetlocP, NULL) just to poll the current
     * playback position — it doesn't switch mode. The bogus mode flip
     * then makes the next data CdRead read AudioSector-sized chunks
     * (2304B vs 2048B) and overrun the game's buffer. Track g_CD_com
     * ourselves for CdLastCom and skip PsyCross's switch entirely. */
    if (com == CdlGetlocP) {
        g_CD_com = com;
        cpu->v0 = 0u;  /* CdControlF returns int (0) for the GetlocP case */
        return;
    }
    cpu->v0 = (uint32_t)CdControlF(com, (u_char*)PTR(cpu->a1));
}

/* Active-overlay tracker (defined in dispatch_chain.c). Each overlay's
 * `LEVEL_BALLOON(id, …)` (overlays.ld) burns the overlay-id as the first
 * word at +0x00. After CdRead settles bytes at 0x8007AA38 we read that
 * word and update the dispatch state. Cutscene-style loads to the same
 * vaddr write LevelHeader / VRAM bytes whose first word is never one of
 * our overlay IDs, so they leave the tracker alone — but they DO clobber
 * the overlay's bytes, so an actual fix for the data corruption still
 * needs to keep level_10's bytes coherent (separate task). */
extern void spyro_set_active_overlay(int which);
enum {
    SPYRO_OVERLAY_NONE        = 0,
    SPYRO_OVERLAY_TITLESCREEN = 1,
    SPYRO_OVERLAY_LEVEL_10    = 2,
};

void psyq_CdRead(CPUState* cpu) {
    int sectors = (int)cpu->a0;
    u_long* buf = (u_long*)PTR(cpu->a1);
    int     mode = (int)cpu->a2;
    OPL_INFO_F("thunks", "CdRead sectors=%d buf=0x%08X mode=0x%02X",
               sectors, cpu->a1, mode);

    cpu->v0 = (uint32_t)CdRead(sectors, buf, mode);
    /* PsyCross's CdRead only enqueues; CdReadSync drains the queue into
     * the buffer. Spyro polls via CdSync (which doesn't drain), so we
     * drain inline here and then fire the cb so g_CdState.m_IsReading
     * clears.
     *
     * Force readMode = RM_DATA before draining: Spyro's CdRead path is
     * always data sectors (mode bits 0x80 = CdlModeSpeed). XA streaming
     * uses CdControlF(CdlReadS, ...) instead. Without this, a stale
     * RM_XA_AUDIO from prior music polling makes CdReadSync read the
     * wrong sector size and trip the assert(0) else-branch. */
    readMode = 0;  /* RM_DATA */
    extern int CdReadSync(int mode, u_char* result);
    int remaining = CdReadSync(/*mode=*/0, /*result=*/NULL);
    if (remaining > 0) {
        OPL_WARN_F("thunks", "CdReadSync left %d sectors unread", remaining);
    }
    /* Verify g_CdState.m_IsReading at 0x80076BB8 is cleared after the cb. */
    uint8_t* ram = openpete_psx_get_ram();
    uint32_t before = 0;
    memcpy(&before, ram + 0x76BB8, 4);
    invoke_psx_callback(cpu, g_cd_read_cb, /*intr=CdlComplete=*/2u, 0u);
    uint32_t after = 0;
    memcpy(&after, ram + 0x76BB8, 4);
    if (after != 0u) {
        OPL_WARN_F("thunks",
                   "CdRead: m_IsReading still set after callback (before=%u after=%u cb=0x%08X)",
                   before, after, g_cd_read_cb);
    }

    /* After the bytes have actually settled in RAM, peek the overlay-id
     * word at 0x8007AA38. The recompiler treats `LEVEL_BALLOON(id, …)` as
     * `LONG(id)`, so the id is the first word of every overlay; cutscene
     * scratch writes to the same vaddr have a different first word and
     * leave the tracker unchanged. */
    if (cpu->a1 == 0x8007AA38u) {
        uint32_t magic = 0;
        memcpy(&magic, ram + 0x7AA38, 4);
        if (magic == 0x0000000Bu) {
            spyro_set_active_overlay(SPYRO_OVERLAY_LEVEL_10);
        } else if (magic == 0x00000030u) {
            spyro_set_active_overlay(SPYRO_OVERLAY_TITLESCREEN);
        }
    }
}

void psyq_CdSync(CPUState* cpu) {
    int mode = (int)cpu->a0;
    /* PsyCross's CdSync writes into result[0..7] whenever g_CD_com ==
     * CdlGetlocP, regardless of whether the caller asked for the data.
     * Spyro's music loop sets that com via CdControlF(CdlGetlocP, NULL),
     * then non-music code calls CdSync(1, NULL) just for the return
     * value (LoadCutscene, CDLoadSync, CDLoadAsync, CDLoadTime). With
     * a NULL result PsyCross then SIGSEGVs on result[0] = ...
     *
     * Pass a stack scratch buffer when the caller didn't supply one;
     * the writes are harmless and PsyCross still returns CdlComplete. */
    u_char  scratch[8];
    u_char* result = (cpu->a1 != 0u) ? (u_char*)PTR(cpu->a1) : scratch;
    int rc = CdSync(mode, result);
    /* Don't log every CdSync — busy-loops would flood. Sample every 1024. */
    static uint32_t calls = 0;
    if ((++calls & 0x3FFFFFu) == 1u) {
        uint8_t* ram = openpete_psx_get_ram();
        uint32_t isReading = 0u;
        memcpy(&isReading, ram + 0x76BB8, 4);
        OPL_INFO_F("thunks",
                   "CdSync(%d) -> %d (call #%u) m_IsReading=%u s0=0x%08X ra=0x%08X",
                   mode, rc, calls, isReading, cpu->s0, cpu->ra);
    }
    cpu->v0 = (uint32_t)rc;
}

void psyq_CdLastCom(CPUState* cpu) {
    cpu->v0 = (uint32_t)CdLastCom();
}

void psyq_CdMix(CPUState* cpu) {
    /* PsyCross declares CdMix but never defines it. Spyro calls it from
     * CDInitialize() to set CD-XA mix volume; without real audio routing
     * yet, treat as success. Phase 1d: wire to PsyCross's CD-XA path. */
    (void)cpu;
    cpu->v0 = 1u;
}

void psyq_CdReadCallback(CPUState* cpu) {
    /* Save the recompiled cb address; our psyq_CdRead thunk fires it
     * after PsyCross finishes the (synchronous) read. */
    OPL_INFO_F("thunks", "CdReadCallback registered: 0x%08X", cpu->a0);
    uint32_t prev = g_cd_read_cb;
    g_cd_read_cb = cpu->a0;
    cpu->v0 = prev;  /* PSYQ returns the previous handler. */
}

void psyq_CdPosToInt(CPUState* cpu) {
    cpu->v0 = (uint32_t)CdPosToInt((CdlLOC*)PTR(cpu->a0));
}

void psyq_CdIntToPos(CPUState* cpu) {
    CdIntToPos((int)cpu->a0, (CdlLOC*)PTR(cpu->a1));
    cpu->v0 = cpu->a1;  /* PSYQ returns the same pointer. */
}

/* --- libspu -------------------------------------------------------------- */

void psyq_SpuInit(CPUState* cpu) {
    (void)cpu;
    SpuInit();
}

void psyq_SpuSetCommonAttr(CPUState* cpu) {
    SpuSetCommonAttr((SpuCommonAttr*)PTR(cpu->a0));
}

void psyq_SpuSetVoiceAttr(CPUState* cpu) {
    SpuSetVoiceAttr((SpuVoiceAttr*)PTR(cpu->a0));
}

void psyq_SpuSetVoiceVolume(CPUState* cpu) {
    SpuSetVoiceVolume((int)cpu->a0,
                      (short)cpu->a1, (short)cpu->a2);
}

void psyq_SpuGetVoiceVolume(CPUState* cpu) {
    SpuGetVoiceVolume((int)cpu->a0,
                      (short*)PTR(cpu->a1),
                      (short*)PTR(cpu->a2));
}

void psyq_SpuSetKey(CPUState* cpu) {
    /* PsyCross's PsyX_SPUAL_SetKey takes g_SpuMutex and walks the OpenAL
     * voice array. Calling it during Spyro's LoadLevel() transition (around
     * frame 1473 with the canonical title→level press script) faults inside
     * pthread_mutex_unlock with addr=NULL — likely OpenAL state corruption
     * from the alSourceStop/Play calls inside the mutex while voices are
     * being torn down by KillSoundsAndMusic.
     *
     * Audio is deferred to Phase 4 anyway (see plan §Phase 4); the no-op
     * here unblocks the level-1 render path. Remove once the audio rewrite
     * lands and the alSource lifetime is properly synchronized. */
    (void)cpu;
}

void psyq_SpuGetKeyStatus(CPUState* cpu) {
    cpu->v0 = (uint32_t)SpuGetKeyStatus((unsigned int)cpu->a0);
}

void psyq_SpuGetAllKeysStatus(CPUState* cpu) {
    SpuGetAllKeysStatus((char*)PTR(cpu->a0));
}

void psyq_SpuRead(CPUState* cpu) {
    cpu->v0 = SpuRead((unsigned char*)PTR(cpu->a0), (unsigned int)cpu->a1);
}

void psyq_SpuWrite(CPUState* cpu) {
    cpu->v0 = SpuWrite((unsigned char*)PTR(cpu->a0), (unsigned int)cpu->a1);
}

void psyq_SpuSetTransferMode(CPUState* cpu) {
    cpu->v0 = (uint32_t)SpuSetTransferMode((int)cpu->a0);
}

void psyq_SpuSetTransferStartAddr(CPUState* cpu) {
    cpu->v0 = SpuSetTransferStartAddr((unsigned int)cpu->a0);
}

void psyq_SpuIsTransferCompleted(CPUState* cpu) {
    cpu->v0 = (uint32_t)SpuIsTransferCompleted((int)cpu->a0);
}

/* --- libmcrd ------------------------------------------------------------- */

/* PsyCross's libmcrd is broken for our needs: MemCardAccept does
 * fopen("0.MCD","rb") and unconditionally fread()s the result, segfaulting
 * when no card file exists. It also indexes memoryCards[chan] with the raw
 * PSX channel encoding (port B = 0x10), which is out-of-bounds.
 *
 * We override the title-screen-critical thunks with a self-contained "no
 * card present" stub. Spyro's titlescreen state machine then routes
 * through TS_SubState_NoMemoryCard → TS_SubState_WarningNoSave → loading,
 * letting Start advance past the title without touching the disk. File
 * I/O thunks (Read/Write/Create/Format) are not exercised on this path,
 * so we leave them pointing at PsyCross. */

namespace {
struct McStubState {
    int cmds;    /* last issued McFunc* (or -1 if none) */
    int rslt;    /* last result (McErr*, or -1 if none) */
    int status;  /* MemCardSync return: 1=Finished, 0=Active, -1=NothingRegistered */
};
McStubState g_mc = { -1, -1, -1 };
}

void psyq_MemCardInit(CPUState* cpu) {
    (void)cpu;
    g_mc.cmds = -1;
    g_mc.rslt = -1;
    g_mc.status = -1;
}

void psyq_MemCardStart(CPUState* cpu) {
    (void)cpu;
    /* No-op: comms are always "ready" since there's no real card. */
}

void psyq_MemCardStop(CPUState* cpu) {
    (void)cpu;
    g_mc.cmds = -1;
    g_mc.rslt = -1;
    g_mc.status = -1;
}

void psyq_MemCardExist(CPUState* cpu) {
    g_mc.cmds = McFuncExist;
    g_mc.rslt = McErrCardNotExist;
    g_mc.status = 1;  /* op completed */
    cpu->v0 = 0u;     /* no card */
}

void psyq_MemCardAccept(CPUState* cpu) {
    g_mc.cmds = McFuncAccept;
    g_mc.rslt = McErrCardNotExist;
    g_mc.status = 1;
    cpu->v0 = 0u;
}

void psyq_MemCardReadFile(CPUState* cpu) {
    cpu->v0 = (uint32_t)MemCardReadFile((int)cpu->a0,
                                        (char*)PTR(cpu->a1),
                                        (unsigned int*)PTR(cpu->a2),
                                        (int)cpu->a3,
                                        (int)cpu->read_word(cpu->sp + 0x10));
}

void psyq_MemCardWriteFile(CPUState* cpu) {
    cpu->v0 = (uint32_t)MemCardWriteFile((int)cpu->a0,
                                         (char*)PTR(cpu->a1),
                                         (unsigned int*)PTR(cpu->a2),
                                         (int)cpu->a3,
                                         (int)cpu->read_word(cpu->sp + 0x10));
}

void psyq_MemCardCreateFile(CPUState* cpu) {
    cpu->v0 = (uint32_t)MemCardCreateFile((int)cpu->a0,
                                          (char*)PTR(cpu->a1),
                                          (int)cpu->a2);
}

void psyq_MemCardFormat(CPUState* cpu) {
    cpu->v0 = (uint32_t)MemCardFormat((int)cpu->a0);
}

void psyq_MemCardSync(CPUState* cpu) {
    /* mode 0 = Sync (block until done), 1 = Async (poll). Our ops always
     * complete instantly, so both return 1 (McSyncRet_Finished) once any
     * op has been issued. Before the first op, return -1 (NothingRegistered). */
    int* cmds_out = (int*)PTR(cpu->a1);
    int* rslt_out = (int*)PTR(cpu->a2);
    if (cmds_out && g_mc.cmds != -1) *cmds_out = g_mc.cmds;
    if (rslt_out && g_mc.rslt != -1) *rslt_out = g_mc.rslt;
    cpu->v0 = (uint32_t)g_mc.status;
}

/* --- libapi -------------------------------------------------------------- */

void psyq_LoadExec(CPUState* cpu) {
    /* Spyro doesn't actually call LoadExec at runtime, but the symbol is
     * exported. Stub to a no-op rather than abort. */
    (void)cpu;
    OPL_WARN_F("thunks", "%s", "LoadExec called - ignoring");
}

/* --- C runtime (printf/sprintf/exit/rand/srand/strlen/memcpy) ------------- */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* These PSYQ-name thunks shadow nothing host-side because of the psyq_
 * prefix. They translate the PSYQ ABI into libc. */

void psyq_memcpy(CPUState* cpu) {
    /* PSYQ memcpy returns dest. */
    void* dst = PTR(cpu->a0);
    const void* src = PTR(cpu->a1);
    size_t n = (size_t)cpu->a2;
    if (dst && src) memcpy(dst, src, n);
    cpu->v0 = cpu->a0;
}

void psyq_strlen(CPUState* cpu) {
    const char* s = (const char*)PTR(cpu->a0);
    cpu->v0 = s ? (uint32_t)strlen(s) : 0u;
}

void psyq_rand(CPUState* cpu) {
    cpu->v0 = (uint32_t)rand();
}

void psyq_srand(CPUState* cpu) {
    srand((unsigned int)cpu->a0);
}

void psyq_exit(CPUState* cpu) {
    OPL_INFO_F("thunks", "exit(%u) from compiled code", cpu->a0);
    exit((int)cpu->a0);
}

/* printf / sprintf are tricky because the format args are MIPS varargs
 * (a1..a3, then sp+0x10, sp+0x14, ...). We don't reconstruct the call;
 * just print the format string with no substitutions, prefixed so it's
 * obvious in the log. Phase 1c+ can teach the recompiler to lower these
 * specially if they matter. */

void psyq_printf(CPUState* cpu) {
    const char* fmt = (const char*)PTR(cpu->a0);
    OPL_INFO_F("psx-printf", "%s", fmt ? fmt : "(null)");
    cpu->v0 = 0u;
}

void psyq_sprintf(CPUState* cpu) {
    char* dst = (char*)PTR(cpu->a0);
    const char* fmt = (const char*)PTR(cpu->a1);
    if (dst && fmt) {
        /* Copy format verbatim; substitution would need MIPS varargs. */
        size_t n = strlen(fmt);
        memcpy(dst, fmt, n + 1u);
        cpu->v0 = (uint32_t)n;
    } else {
        cpu->v0 = 0u;
    }
}

}  /* extern "C" */
