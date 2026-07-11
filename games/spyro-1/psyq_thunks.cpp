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

#include <cstdlib>

extern "C" {
#include "openpete_fiber.h"
#include "openpete_log.h"
#include "mod_wad_overlay.h" /* rung 9: level-segment asset overlay */
#include "openpete_ticker.h"
#include "openpete_psx.h"
#include "openpete_memcard.h"
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
extern "C" void PsyX_SPUAL_TickEnvelope(int samples);

/* Saved-OT capture: each psyq_DrawOTag rebuilds the PSX OT chain into a
 * host scratch buffer (PsyCross P_TAG slots, guest->host translated)
 * and the display walk parses that. (The 1-game-frame-lag ping-pong +
 * screen-space matcher that used to live here was deleted 2026-07-06 —
 * the reference walk shows the just-built OT, bit-original.)
 *
 * Sizing (measured over a 20k-frame demo): 4:3 peaks at 2,255 slots /
 * 94 KB. 1 MB scratch + 8192 slots is ample headroom. */
#define OT_SCRATCH_SZ  (1u * 1024u * 1024u)
#define OT_MAX_SLOTS   8192

typedef struct {
    uint8_t  scratch[OT_SCRATCH_SZ];
    uint8_t* slots[OT_MAX_SLOTS];
    int      slot_count;
    size_t   scratch_used;
} saved_ot_t;

static saved_ot_t  s_ot_buf[1];
static saved_ot_t* g_ot_cur  = &s_ot_buf[0];  /* built this DrawOTag */

/* Display walk: parse the just-captured OT through PsyCross
 * (bit-original PSX presentation; the lagged/matcher path is gone).
 * Caller owns PsyX BeginScene/EndScene framing. */
extern "C" void openpete_render_walk_ot(void) {
    extern void ClearSplits(void);
    extern void ParsePrimitivesLinkedList(u_long* p, int singlePrimitive);
    extern void DrawAllSplits(void);

    if (g_ot_cur->slot_count <= 0) return;
    ClearSplits();
    ParsePrimitivesLinkedList((u_long*)g_ot_cur->slots[0], 0);
    DrawAllSplits();
}
#include "psx/libmcrd.h"
#include "psx/libapi.h"

/* XA streaming source (lives in PsyCross/audio/PsyX_XA.cpp). */
extern "C" {
    void PsyX_XA_Start(int sector, int file, int chan);
    void PsyX_XA_Stop(void);
    int  PsyX_XA_IsActive(void);
    void PsyX_XA_Pump(int maxSectors);
    int  PsyX_XA_GetCurrentSector(void);
    void PsyX_XA_SetVolume(float linear_gain);
}

/* Music WAV override (core/host/) — plays assets/music/track_NN.wav in
 * place of the XA stream when present. See
 * notes/plans/openpete-music-wav-override.md. */
#include "music_override.h"
#include "features/render_paths.h"  /* --render-paths OT-walk filter */

/* PsyCross internals shared with the libcd thunks below. */
extern "C" int g_CD_com;
extern "C" int readMode;  /* 0=RM_DATA, 1=RM_XA_AUDIO */

/* Spyro music streaming state. CdControlB(CdlSetmode) with RT|SF arms XA;
 * CdControlB(CdlSetfilter) records file/chan; CdControlB(CdlReadS) starts
 * streaming at the requested sector. CdControlB(CdlPause/Stop/CdlReadN)
 * stops it. State is small enough to keep file-static. */
static bool s_xa_armed       = false;
static int  s_xa_filter_file = 1;
static int  s_xa_filter_chan = 0;
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
#include "openpete_recomp_module.h"

static uint32_t g_cd_read_cb     = 0;
/* Native cd.c (post-rewriter) registers a host-side callback via
 * openpete_CdReadCallback. psyq_CdRead checks both this and g_cd_read_cb;
 * one-or-the-other is set depending on whether the registrar was native
 * or recompiled code. */
typedef void (*native_cd_read_cb_t)(unsigned char intr, unsigned char* result);
static native_cd_read_cb_t g_native_cd_read_cb = nullptr;
static uint32_t g_vsync_cb       = 0;

static void invoke_psx_callback(CPUState* cpu, uint32_t cb_addr,
                                uint32_t a0, uint32_t a1) {
    if (cb_addr == 0) return;
    /* Save caller's a-regs / ra; the recompiled function will trash them. */
    uint32_t s_a0 = cpu->a0, s_a1 = cpu->a1, s_a2 = cpu->a2, s_a3 = cpu->a3;
    uint32_t s_ra = cpu->ra, s_v0 = cpu->v0, s_v1 = cpu->v1;
    cpu->a0 = a0; cpu->a1 = a1; cpu->ra = 0xDEADBEEF;
    if (!openpete_recomp_module_get()->dispatch(cpu, cb_addr)) {
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

/* Forward decl: defined later, called from inside psyq_VSync below. */
static void psyq_libpad_mirror_canonical_slot2(CPUState* cpu);

void psyq_VSync(CPUState* cpu) {
    /* libetc semantics:
     *   mode  < 0 : query — return current vbl counter, no wait
     *   mode == 0 : wait for next vblank, return updated counter
     *   mode  > 0 : wait that many vblanks (Spyro doesn't seem to use this)
     *
     * vbl is yield-driven: each VSync(0) bumps the counter by exactly the
     * wait count and yields once. That keeps Spyro's wait-loop pattern
     * (`while VSync(-1)-s < 2 VSync(0)`) deterministic — the loop always
     * exits after exactly 2 yields per logical frame regardless of wall-
     * clock pacing, which is the invariant BK2 replay relies on.
     *
     * Wall-clock pacing lives elsewhere: the host loop (run.c) blocks on
     * the openpete_ticker condvar between game slices, so 1 yield ≈
     * 16.67 ms. Single-VSync(0) flyby → 1 yield/logical frame = 60 fps;
     * wait-loop gameplay → 2 yields/logical frame = 30 fps. No per-game
     * predicate needed. Tab / --headless skip the host-loop wait, which
     * fast-forwards wall clock without changing yield-per-frame counts —
     * so BK2 state at any frame is identical regardless of pacing. */
    int mode = (int)cpu->a0;
    static uint32_t calls = 0u;
    int observed = SDL_GetAtomicInt(&openpete_vbl);
    if ((++calls & 0x3FFu) == 1u) {
        OPL_INFO_F("thunks", "VSync(%d) vbl=%d frame=%u (call #%u)",
                   mode, observed, openpete_frame, calls);
    }

    if (mode < 0) {
        cpu->v0 = (uint32_t)observed;
        return;
    }

    int wait = (mode == 0) ? 1 : mode;
    SDL_AddAtomicInt(&openpete_vbl, wait);

    /* Advance PsyCross's deterministic ADSR envelope simulation in
     * lockstep with VBL. NTSC PSX: 44100 SPU samples / 60 VBL = 735
     * samples per VBL. Without this, voice key state never transitions
     * to SPU_RESET and the game's voice-pool bookkeeping leaks. The
     * envelope sim is independent of the OpenAL playback path, so this
     * is correct in both audio-on and headless runs. */
    PsyX_SPUAL_TickEnvelope(wait * 735);

    if (openpete_fast_boot_active) {
        /* Fast-boot: don't yield. Initialize()'s splash spins consume vbl
         * with no work, and we want them at full CPU speed. PadVSync would
         * be writing g_PadBuffer based on pre-input state, so skip the
         * VSync callback too. */
    } else if (openpete_host_fiber) {
        openpete_yield_to_host();
        if (g_vsync_cb) invoke_psx_callback(cpu, g_vsync_cb, 0, 0);
    } else {
        /* No fiber loop (smoke-test path) — fall back to PsyCross's busy
         * wait so audio/timer semantics still match. */
        if (mode == 0) (void)VSync(0);
        if (g_vsync_cb) invoke_psx_callback(cpu, g_vsync_cb, 0, 0);
    }

    /* PSYQ libpad parity: PsyCross's input poll (which fired during the
     * yield above, or via VSync(0) on the smoke-test path) wrote
     * "connected pad" bytes into slot 2 even though no controller is
     * present there. Mirror real PSYQ's empty-slot output before the
     * game's next read. */
    psyq_libpad_mirror_canonical_slot2(cpu);

    cpu->v0 = (uint32_t)SDL_GetAtomicInt(&openpete_vbl);
}

void psyq_VSyncCallback(CPUState* cpu) {
    /* Save the cb address; psyq_VSync invokes it once per call (Phase 1c
     * doesn't drive a real 60Hz timer — VSync()'s thunk fires it when the
     * compiled main loop yields). */
    OPL_INFO_F("thunks", "VSyncCallback registered: 0x%08X", cpu->a0);
    g_vsync_cb = cpu->a0;
    cpu->v0 = 0u;
    /* Real PSYQ VSyncCallback also stashes the cb pointer in libgpu's
     * vtable at 0x800749D0 (the slot func_8005DE58 reads on each VSync
     * IRQ). PsyCross dispatches the cb host-side, so we mirror the write
     * for byte-faithful diffs. */
    cpu->write_word(0x800749D0u, cpu->a0);
}

void psyq_StopCallback(CPUState* cpu) {
    cpu->v0 = (uint32_t)StopCallback();
}

/* --- libgpu -------------------------------------------------------------- */

/* psyq_libgpu_helpers ---------------------------------------------------- *
 * Real PSYQ libgpu MIPS code (statically linked into Spyro's EXE) writes to
 * a number of specific PSX-RAM addresses during ResetGraph / SetDef* /
 * PutDispEnv / etc. Our thunks delegate behaviour to PsyCross, which
 * manages host-side state without writing those PSX-side bytes. To keep the
 * --seed-ram + --halt-at-pc diff loop's "byte-identical with pcsx-redux"
 * invariant alive past GraphicsInitialize, we replicate the byte-level
 * writes here. Disassembly references in comments map to the real PSYQ at
 * 0x8005EA94+ in spyro-1/build/psx.elf. */

/* Mirror a function's MIPS stack-frame prologue (sp -= frame_size; sw ra at
 * sp+ra_off; optionally sw s-regs). PSYQ functions overwrite saved-register
 * slots that an empty C thunk leaves at zero — these eight bytes per call
 * accumulate fast and account for ~half the stack-region diff at MusicInit.
 *
 * `s_pushes` is a {offset, value} list of additional saved-reg writes
 * matching real PSYQ's prologue. Pass NULL/0 for ra-only. */
struct PsxStackPush { uint32_t offset; uint32_t value; };
static void psyq_mirror_stack_frame(CPUState* cpu, uint32_t frame_size,
                                    uint32_t ra_off,
                                    const PsxStackPush* pushes, size_t n) {
    uint32_t sp_b = cpu->sp;
    cpu->sp = sp_b - frame_size;
    cpu->write_word(cpu->sp + ra_off, cpu->ra);
    for (size_t i = 0; i < n; i++) {
        cpu->write_word(cpu->sp + pushes[i].offset, pushes[i].value);
    }
    cpu->sp = sp_b;
}

/* Real ResetGraph at 0x8005F2A4. For mode 0/3/5 (the init paths Spyro uses),
 * PSYQ writes a 128-byte env struct at 0x80074A64 + helper structs via
 * func_80061DEC. NTSC GPU yields gpu_type=0 (the default). */
void psyq_ResetGraph(CPUState* cpu) {
    int mode = (int)cpu->a0;
    cpu->v0 = (uint32_t)ResetGraph(mode);

    int sub = mode & 7;
    if (sub == 0 || sub == 3 || sub == 5) {
        const uint32_t ENV = 0x80074A64u;
        /* memset(ENV, 0, 128) */
        for (uint32_t a = ENV; a < ENV + 128u; a += 4u) cpu->write_word(a, 0u);
        /* sb gpu_type @ENV+0; sb 1 @ENV+1 */
        cpu->write_byte(ENV + 0u, 0u);   /* NTSC */
        cpu->write_byte(ENV + 1u, 1u);
        /* halfwords pulled from PSYQ tables at 0x80074AE4 / 0x80074AF8.
         * For NTSC (gpu_type=0) the table low-halfwords are 0x0400 and
         * 0x0200 — confirmed by xxd of pcsx-redux's RAM dump. */
        cpu->write_half(ENV + 4u, 0x0400u);
        cpu->write_half(ENV + 6u, 0x0200u);
        /* Observed pcsx-redux: byte at ENV+8 (0x80074A6C) = 1. Source not
         * yet localised in disassembly; matched by observation so the diff
         * stays clean. */
        cpu->write_byte(ENV + 8u, 1u);
        /* memset(ENV+16, 0xFF, 92) -> 0x80074A74..0x80074ACF */
        for (uint32_t a = ENV + 16u; a < ENV + 16u + 92u; a += 4u) {
            cpu->write_word(a, 0xFFFFFFFFu);
        }
        /* memset(ENV+108, 0xFF, 20) -> 0x80074AD0..0x80074AE3 */
        for (uint32_t a = ENV + 108u; a < ENV + 108u + 20u; a += 4u) {
            cpu->write_word(a, 0xFFFFFFFFu);
        }
    }

    /* func_80061DEC effects (called by ResetGraph): three word writes plus
     * the BIOS-side gpu-type cache. Hardcoded results for NTSC mode 0:
     * func_8005DF44 returns 0 ((sub==0 || sub==3 || sub==5) only). */
    cpu->write_word(0x80074B6Cu, 0u);
    cpu->write_word(0x80074B68u, 0u);

    /* Post-ResetGraph snapshot from pcsx-redux RAM at MemoryCardInit halt.
     * 60 EXE bytes scattered across libgpu's vtable / dispatch / GPU command
     * scratch areas. Real PSYQ libgpu writes these via a chain of helpers
     * (`func_8005DDC8` -> *(D_800749AC)+0xC, `func_8005DF60` walks the
     * 0x80073924 control struct, `func_80061DEC` updates the helper-table
     * trio at 0x80074B68/6C/78, and the 0x80075978 region holds queued GP0
     * env packets). openpete uses PsyCross stubs and never enters those
     * helpers, so we replay the observed post-init values directly. NTSC
     * mode 0; values verified against pcsx-redux dumps (gp=0x80075264,
     * sp=0x801FFFF8 at boot). */
    if (sub == 0 || sub == 3 || sub == 5) {
        /* libgpu callback control struct at 0x80073924+ */
        cpu->write_word(0x80073924u, 0x00000001u);
        cpu->write_word(0x80073928u, 0x8005E560u);  /* fnptr */
        cpu->write_word(0x80073934u, 0x8005E680u);  /* fnptr */
        cpu->write_word(0x80073954u, 0x00000009u);
        cpu->write_word(0x8007395Cu, 0x8005DFC8u);  /* fnptr */
        cpu->write_word(0x80073960u, 0x8007493Cu);
        cpu->write_word(0x80073964u, 0x801FFFF8u);  /* boot sp */
        cpu->write_word(0x80073968u, 0x80073924u);  /* self ref */
        cpu->write_word(0x80073970u, 0x00000001u);
        cpu->write_word(0x80073988u, 0x80075264u);  /* boot gp */
        /* libgpu vtable patch slots (indirected through D_800749AC) */
        cpu->write_word(0x80074990u, 0x8005E804u);
        cpu->write_word(0x800749A0u, 0x8005E5D8u);
        /* GP0 dispatch state at 0x80074B58+ (set by func_80061F48 in real
         * PSYQ; we cache the post-init values). */
        cpu->write_word(0x80074B58u, 0x80060FD8u);
        cpu->write_word(0x80074B5Cu, 0x801FFF78u);  /* inner sub-call sp */
        cpu->write_word(0x80074B70u, 0x00000009u);
        cpu->write_word(0x80074B78u, 0x00000009u);
        cpu->write_word(0x80074B7Cu, 0x000000F0u);
        /* Pre-built GP0 env packet block at 0x80075978 — pcsxr ends
         * GraphicsInit with these queued; openpete-PsyCross drains
         * commands a different way so the slot stays zero without help. */
        cpu->write_word(0x80075978u, 0x05FFFFFFu);
        cpu->write_word(0x8007597Cu, 0xE6000000u);
        cpu->write_word(0x80075980u, 0xE1000000u);
        cpu->write_word(0x80075984u, 0x02000000u);
        cpu->write_word(0x8007598Cu, 0x01FF0200u);
    }

    /* ResetGraph's own MIPS prologue: sp -= 32; sw s0,16; sw s1,20; sw ra,24.
     * (sw s2 at +20 was overwritten by sw s1 at the same slot.) */
    PsxStackPush rg_pushes[] = {
        {16u, cpu->s0},
        {20u, cpu->s1},
    };
    psyq_mirror_stack_frame(cpu, 32u, 24u, rg_pushes, 2);
}

void psyq_SetDispMask(CPUState* cpu) {
    SetDispMask((int)cpu->a0);
    /* Real SetDispMask @0x8005F6C8: sp -= 32; sw s0,16; sw s1,20; sw ra,24. */
    PsxStackPush p[] = {{16u, cpu->s0}, {20u, cpu->s1}};
    psyq_mirror_stack_frame(cpu, 32u, 24u, p, 2);
}

void psyq_SetGraphDebug(CPUState* cpu) {
    cpu->v0 = (uint32_t)SetGraphDebug((int)cpu->a0);
    /* Real SetGraphDebug @0x8005F53C: sp -= 24; sw s0,16; sw ra,20. */
    PsxStackPush p[] = {{16u, cpu->s0}};
    psyq_mirror_stack_frame(cpu, 24u, 20u, p, 1);
}

void psyq_DrawSync(CPUState* cpu) {
    cpu->v0 = (uint32_t)DrawSync((int)cpu->a0);
    /* Real DrawSync @0x8005F764: sp -= 24; sw s0,16; sw ra,20. */
    PsxStackPush p[] = {{16u, cpu->s0}};
    psyq_mirror_stack_frame(cpu, 24u, 20u, p, 1);
}

void psyq_ClearImage(CPUState* cpu) {
    cpu->v0 = (uint32_t)ClearImage((RECT16*)PTR(cpu->a0),
                                   (u_char)cpu->a1,
                                   (u_char)cpu->a2,
                                   (u_char)cpu->a3);
    /* Real ClearImage @0x8005F8F8: sp -= 40; sw s0,16; s1,20; s2,24; s3,28; ra,32. */
    PsxStackPush p[] = {
        {16u, cpu->s0}, {20u, cpu->s1}, {24u, cpu->s2}, {28u, cpu->s3},
    };
    psyq_mirror_stack_frame(cpu, 40u, 32u, p, 4);
}

void psyq_LoadImage(CPUState* cpu) {
    RECT16* r = (RECT16*)PTR(cpu->a0);
    cpu->v0 = (uint32_t)LoadImage(r, (u_long*)PTR(cpu->a1));
}

void psyq_StoreImage(CPUState* cpu) {
    cpu->v0 = (uint32_t)StoreImage((RECT16*)PTR(cpu->a0),
                                   (u_long*)PTR(cpu->a1));
}

void psyq_MoveImage(CPUState* cpu) {
    RECT16* r = (RECT16*)PTR(cpu->a0);
    cpu->v0 = (uint32_t)MoveImage(r, (int)cpu->a1, (int)cpu->a2);
}

/* The native renderer (--renderer vk|ab|diff) draws the scanned-out framebuffer; feed it
 * the display framebuffer (DISPENV disp) + the drawn region (DRAWENV clip, used
 * to crop the overscan). Forward-declared (extern "C", core C TUs) to avoid
 * pulling the core header into this thunk file. No-ops when disabled. */
extern "C" void openpete_native_set_display_rect(int x, int y, int w, int h);
extern "C" void openpete_native_set_draw_clip(int x, int y, int w, int h);
extern "C" void openpete_native_set_clear_color(int isbg, int r, int g, int b);

void psyq_PutDispEnv(CPUState* cpu) {
    DISPENV* env = (DISPENV*)PTR(cpu->a0);
    PutDispEnv(env);
    openpete_native_set_display_rect(env->disp.x, env->disp.y, env->disp.w, env->disp.h);
    /* Real PutDispEnv @0x80060030: sp -= 40; sw s0,16; s1,20; s2,24; s3,28; ra,32. */
    PsxStackPush p[] = {
        {16u, cpu->s0}, {20u, cpu->s1}, {24u, cpu->s2}, {28u, cpu->s3},
    };
    psyq_mirror_stack_frame(cpu, 40u, 32u, p, 4);
}

void psyq_PutDrawEnv(CPUState* cpu) {
    DRAWENV* env = (DRAWENV*)PTR(cpu->a0);
    PutDrawEnv(env);
    openpete_native_set_draw_clip(env->clip.x, env->clip.y, env->clip.w, env->clip.h);
    /* The actually-cleared framebuffer background = the gamestate draw-env clear
     * colour (r0,g0,b0) when auto-clear (isbg) is on. This is the faithful bg for
     * the native renderer in EVERY state — unlike g_Cyclorama.m_BackgroundColor,
     * which equals the clear only on frames that draw the cyclorama and goes stale
     * during cyclorama-less states (e.g. the black level-name flyby, where the
     * clear is black but g_Cyclorama still holds the previous level's sky). */
    openpete_native_set_clear_color(env->isbg, env->r0, env->g0, env->b0);
    /* Real PutDrawEnv @0x8005FDD8: sp -= 32; sw s0,16; s1,20; s2,24; ra,28. */
    PsxStackPush p[] = {
        {16u, cpu->s0}, {20u, cpu->s1}, {24u, cpu->s2},
    };
    psyq_mirror_stack_frame(cpu, 32u, 28u, p, 3);
}

/* Hor+ widescreen (--aspect) is implemented at the GTE-instruction
 * layer in psxrecomp's gte_ctc2 (third_party/psxrecomp/runner/src/gte.cpp)
 * so EVERY write to OFX picks up the widening — including Spyro's
 * renderer's inline CTC2 stores (which would bypass this thunk). The
 * libgte SetGeomOffset call eventually reaches gte_ctc2 too, so a
 * single widening point handles both routes without double-applying.
 *
 * The DISPENV / DRAWENV stay at their native PSX VRAM widths (typically
 * 512). Widening the VRAM-facing widths would make GR_StoreFrameBuffer
 * blit the GL render back into the right half of VRAM (x>=512), which
 * is where Spyro's texture pages live, causing texture corruption. The
 * GL ortho widening happens inside PsyCross via
 * g_psyx_render_aspect_factor so geometry projected to the wider
 * screen-X range is mapped across the full SDL window. */

void psyq_SetDefDispEnv(CPUState* cpu) {
    SetDefDispEnv((DISPENV*)PTR(cpu->a0),
                  (int)cpu->a1, (int)cpu->a2, (int)cpu->a3,
                  (int)cpu->read_word(cpu->sp + 0x10));
    /* Real SetDefDispEnv @0x8005EB4C is a leaf — no stack pushes. */
}

void psyq_SetDefDrawEnv(CPUState* cpu) {
    SetDefDrawEnv((DRAWENV*)PTR(cpu->a0),
                  (int)cpu->a1, (int)cpu->a2, (int)cpu->a3,
                  (int)cpu->read_word(cpu->sp + 0x10));
    /* Real SetDefDrawEnv @0x8005EA94: sp -= 40; sw s0,16; s1,20; s2,24;
     * s3,28; s4,32; ra,36. */
    PsxStackPush p[] = {
        {16u, cpu->s0}, {20u, cpu->s1}, {24u, cpu->s2},
        {28u, cpu->s3}, {32u, cpu->s4},
    };
    psyq_mirror_stack_frame(cpu, 40u, 36u, p, 5);
}

void psyq_SetDrawMode(CPUState* cpu) {
    /* DR_MODE* p, int dfe, int dtd, int tpage, RECT16* tw — five args.
     *
     * The game's polybuf primitive is native PSX layout (4-byte P_TAG:
     * addr:24 | len:8, then code[] words). PsyCross is built with
     * USE_EXTENDED_PRIM_POINTERS, so calling SetDrawMode directly on the
     * game's memory writes `len` at offset 8 and code[0] at offset 12 (after
     * the 8-byte host-pointer header) — corrupting the PSX-layout DR_MODE:
     * its tag length stays stale and the mode word lands in the wrong place.
     * The OT walk then sees a garbage-length node, and when the stale code
     * byte happens to be 0x_1 it mis-runs as DR_MOVE → out-of-bounds VRAM
     * copy (the Dream Weavers crash). g4/poly prims are unaffected because
     * the game builds those with inline setXY/setRGB/setcode macros (correct
     * PSX layout); only the *function* call SetDrawMode was layout-wrong.
     * See notes/plans/openpete-ot-walk-vram-oob-crash.md.
     *
     * Fix: run SetDrawMode into a host-local extended DR_MODE, then write the
     * computed length + code words back in native PSX 4-byte-tag layout. Only
     * the tag's len byte is touched; the addr/next 24 bits are set later by
     * the game's addPrim (recompiled inline → already PSX layout). */
    DR_MODE tmp;
    SetDrawMode(&tmp, (int)cpu->a1, (int)cpu->a2, (int)cpu->a3,
                (RECT16*)PTR(cpu->read_word(cpu->sp + 0x10)));
    uint32_t p = cpu->a0;
    cpu->write_byte(p + 3, (uint8_t)getlen(&tmp));  /* PSX tag len (high byte) */
    cpu->write_word(p + 4, (uint32_t)tmp.code[0]);  /* code[0] = draw mode */
    cpu->write_word(p + 8, (uint32_t)tmp.code[1]);  /* code[1] = tex window */
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
/* OT-walk scratch lives in the saved_ot_t buffer declared above; each
 * DrawOTag rebuilds it and the display walk parses it. */

void psyq_DrawOTag(CPUState* cpu) {
    /* First DrawOTag is the natural transition from "boot init" to "game is
     * rendering." Anchor `openpete_frame` here: from now on, every VSync(0)
     * yields to the host and ticks the host-facing counter. */
    if (openpete_fast_boot_active) {
        openpete_fast_boot_active = 0;
        OPL_INFO_F("boot",
                   "fast-boot ended at vbl=%d (first DrawOTag); openpete_frame "
                   "anchored to 0", SDL_GetAtomicInt(&openpete_vbl));
    }

    saved_ot_t* B = g_ot_cur;
    uint8_t*  g_ot_scratch      = B->scratch;
    size_t&   g_ot_scratch_used = B->scratch_used;
    uint8_t** g_ot_slot_host    = B->slots;
    int&      g_ot_slot_count   = B->slot_count;
    g_ot_scratch_used = 0;
    g_ot_slot_count = 0;

    /* OPENPETE_OT_DUMP_FRAMES=A-B → for openpete_frame in [A,B], dump every
     * DrawOTag call's slot list to OPENPETE_OT_DUMP_PATH (default
     * /tmp/ot-dump.txt). One row per slot, with frame as the leading column. */
    static int   s_dump_lo    = -2;
    static int   s_dump_hi    = -1;
    static FILE* s_dump_fp    = nullptr;
    static uint32_t s_dump_call = 0;
    if (s_dump_lo == -2) {
        const char* env = std::getenv("OPENPETE_OT_DUMP_FRAMES");
        s_dump_lo = -1;
        s_dump_hi = -1;
        if (env) {
            int a = -1, b = -1;
            if (std::sscanf(env, "%d-%d", &a, &b) >= 1) {
                s_dump_lo = a;
                s_dump_hi = (b >= a) ? b : a;
            }
        }
        if (s_dump_lo >= 0) {
            OPL_INFO_F("thunks", "OT dump armed for frames [%d,%d]",
                       s_dump_lo, s_dump_hi);
        }
    }
    FILE* dump = nullptr;
    if (s_dump_lo >= 0 &&
        (int)openpete_frame >= s_dump_lo &&
        (int)openpete_frame <= s_dump_hi) {
        if (!s_dump_fp) {
            const char* path = std::getenv("OPENPETE_OT_DUMP_PATH");
            s_dump_fp = std::fopen(path ? path : "/tmp/ot-dump.txt", "w");
            OPL_INFO_F("thunks", "OT dump opened path=%s fp=%p frame=%u",
                       path ? path : "/tmp/ot-dump.txt",
                       (void*)s_dump_fp, openpete_frame);
            if (s_dump_fp) {
                std::fprintf(s_dump_fp,
                    "# frame,slot,call,psx_addr,plen,code,payload_words(hex)\n");
            }
        }
        dump = s_dump_fp;
    }
    if (dump) ++s_dump_call;

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

    if (openpete_frame != s_last_frame) {
        s_last_frame = openpete_frame;
    }

    uint32_t addr = cpu->a0;
    /* Root-cause probe (OPENPETE_OT_GARBAGE_TRACE=1, inert otherwise): the OT
     * walk can derail into non-OT memory on dense frames (Dream Weavers moving
     * forward), where a stray tag whose code byte is 0x_1 is misparsed as
     * DR_MOVE and SEGVs the unbounded VRAM copy. Log any tag whose plen exceeds
     * the largest real Spyro primitive (POLY_GT4 = 12 longs) together with the
     * link that led here, so we can see where the chain jumps. */
    static int s_otg_trace = -1;
    if (s_otg_trace < 0) {
        const char* e = getenv("OPENPETE_OT_GARBAGE_TRACE");
        s_otg_trace = (e && *e && *e != '0') ? 1 : 0;
    }
    static int s_otg_logs = 0;
    /* OPENPETE_OT_TRUNCATE=N (portal a4 forensics, inert otherwise): stop
     * submitting the OT chain after N slots, keeping exactly OT-dump slots
     * 0..N-1 (the dump's slot column is g_ot_slot_count-1, so the indices
     * line up 1:1 with OPENPETE_OT_DUMP_FRAMES rows). Lets an -og capture
     * show what the PsyCross walk has painted "so far" at any slot cutoff —
     * the decisive probe for whether a pixel's final color comes from slots
     * before or after the cutoff. */
    static int s_ot_truncate = -2;
    if (s_ot_truncate == -2) {
        const char* e = std::getenv("OPENPETE_OT_TRUNCATE");
        s_ot_truncate = (e && *e) ? std::atoi(e) : -1;
        if (s_ot_truncate >= 0)
            OPL_INFO_F("thunks", "OT truncate armed: submitting first %d slots only",
                       s_ot_truncate);
    }
    uint32_t prev_addr = 0u, prev_tag = 0u;
    while (g_ot_slot_count < (int)OT_MAX_SLOTS) {
        if (s_ot_truncate >= 0 && g_ot_slot_count >= s_ot_truncate) break;
        if (addr == 0u) break;
        uint8_t* host = (uint8_t*)openpete_psx_to_host_ptr(addr);
        if (!host) break;

        uint32_t tag;
        std::memcpy(&tag, host, 4);
        uint8_t  plen = (uint8_t)((tag >> 24) & 0xFFu);
        uint32_t next = tag & 0x00FFFFFFu;

        if (s_otg_trace && plen > 32u && s_otg_logs < 80) {
            s_otg_logs++;
            OPL_WARN_F("thunks",
                "OT-garbage f=%u slot=%d addr=0x%08X tag=0x%08X plen=%u "
                "code=0x%02X next=0x%06X <- prev addr=0x%08X tag=0x%08X",
                openpete_frame, g_ot_slot_count, addr, tag, (unsigned)plen,
                (unsigned)host[7], next, prev_addr, prev_tag);
        }

        /* --render-paths: drop geometry prims belonging to a non-selected render
         * path. Presentation-only — we just don't submit the prim; game RAM is
         * untouched (the OT itself is never modified), so gameplay state is
         * unaffected. Control/env prims (plen==0, or code 0xE0/0xA0/0x00) always
         * pass so the draw-env / image loads still set up the framebuffer. */
        if (render_paths_active()) {
            uint8_t cb = (plen != 0u) ? host[7] : 0u;
            uint8_t hi = cb & 0xF0u;
            bool is_geom = (plen != 0u) && (hi >= 0x20u && hi <= 0x70u);
            if (is_geom && !render_path_addr_visible(addr)) {
                if (next == 0x00FFFFFFu) break;
                addr = next | 0x80000000u;
                continue;
            }
        }

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
        if (g_ot_scratch_used + slot_size > OT_SCRATCH_SZ) {
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
        /* PGXP per-primitive index disabled. Phase 2b Option 4 (content-
         * keyed (sx,sy)→index map) was tried and failed: same-pixel
         * vertex collisions caused flicker. Awaiting Option 2b
         * (a per-address PGXP memory). */
        uint16_t pgxpField = 0xFFFFu;
        std::memcpy(out + 8,  &lenField,  2);
        std::memcpy(out + 10, &pgxpField, 2);

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

        if (dump) {
            std::fprintf(dump, "%u,%d,%u,0x%08X,%u,0x%02X",
                         openpete_frame, g_ot_slot_count - 1, s_dump_call,
                         addr, (unsigned)plen, (unsigned)code_byte);
            uint32_t nwords = (uint32_t)(payload_bytes / 4u);
            if (nwords > 16u) nwords = 16u; /* cap; enough for poly_gt4 */
            for (uint32_t w = 0; w < nwords; ++w) {
                uint32_t word;
                std::memcpy(&word, host + 4 + w * 4u, 4);
                std::fprintf(dump, ",%08X", word);
            }
            std::fputc('\n', dump);
        }

        prev_addr = addr;
        prev_tag  = tag;
        if (next == 0x00FFFFFFu) break;
        addr = next | 0x80000000u;
    }
    if (dump) std::fflush(dump);

    /* --render-paths: the per-path polybuf intervals were recorded during this
     * frame's render pass and consumed by the walk above. The "canon drawn" mask
     * latch + the per-frame interval clear are NOT done here — they live in the
     * host present loop (core/runtime/run.c), once per canonical present, so they
     * survive the planned removal of this PsyCross OT submission (the feeder hooks
     * that record the intervals fire on the render functions, independent of
     * DrawOTag). The walk above has already consumed this frame's intervals; the
     * present loop snapshots + clears them before the next frame records fresh. */

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
        /* Phase 1.0 sizing probe — track high-water marks for OT memory
         * to size the saved-prev buffers correctly. */
        {
            static int      s_max_slot_count = 0;
            static size_t   s_max_scratch    = 0;
            static unsigned s_last_log_frame = 0;
            if (g_ot_slot_count > s_max_slot_count) s_max_slot_count = g_ot_slot_count;
            if (g_ot_scratch_used > s_max_scratch)  s_max_scratch    = g_ot_scratch_used;
            if (openpete_frame >= s_last_log_frame + 1000u) {
                s_last_log_frame = openpete_frame;
                OPL_INFO_F("thunks", "OT high-water: max_slots=%d max_scratch=%zu KB",
                           s_max_slot_count, s_max_scratch / 1024);
            }
        }
        /* When the native Vulkan renderer is the sole display (pure --renderer vk),
         * PsyCross does not rasterize the OT at all — the native renderer draws
         * everything from gamestate. Skip the GL walk entirely (and don't open a
         * PsyX scene, so PsyX_EndScene auto-no-ops via begin_scene_flag → no
         * GR_StoreFrameBuffer, no second vsync swap on the hidden window). */
        extern int openpete_psycross_renders;
        if (openpete_psycross_renders) {
            if (PsyX_BeginScene()) {
                ClearSplits();
            }
            /* Sim-due display walk of the OT just captured. */
            extern void openpete_render_walk_ot(void);
            openpete_render_walk_ot();
        }
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
    /* Hor+ widening / FOV override are applied inside
     * gte_rtps_internal (third_party/psxrecomp/runner/src/gte.cpp) at
     * projection time, so EVERY RTPS picks up the override regardless
     * of whether OFX/H were set via this thunk or via the renderer's
     * inline CTC2 stores. The thunk itself just forwards the args. */
}

void psyq_SetGeomScreen(CPUState* cpu) {
    gte_sync_cpu_to_psyx(cpu);
    SetGeomScreen((int)cpu->a0);
    gte_sync_psyx_to_cpu(cpu);
    /* --fov override applied inside gte_rtps_internal; see
     * psyq_SetGeomOffset for the rationale. */
}

void psyq_SetTransMatrix(CPUState* cpu) {
    gte_sync_cpu_to_psyx(cpu);
    SetTransMatrix((MATRIX*)PTR(cpu->a0));
    gte_sync_psyx_to_cpu(cpu);
}

/* Replicate PSYQ MulMatrix's final `swc2 $11, 0x10(dst)`: IR3 is a
 * signed 16-bit GTE register but `swc2` stores 32 bits. Bytes 16-17 of
 * a MATRIX hold m[2][2]; bytes 18-19 are the two-byte alignment pad
 * before the long t[3] translation triple — sometimes consumed by a
 * caller's `Memcpy(SHORTMATRIX, MATRIX, 20)`. PSYQ deposits sign-
 * extension of IR3 into that pad slot (0xFFFF when m[2][2] negative,
 * 0x0000 otherwise). PsyCross's MulMatrix only writes m[2][2] as a
 * 16-bit short and leaves the pad untouched, which appears in the
 * oracle as a 4-byte g_Camera divergence on the very first
 * non-identity rotation. Mirror the side effect explicitly. */
static inline void psyq_matrix_swc2_sign_extend_pad(MATRIX* m) {
    if (!m) return;
    int16_t* pad = ((int16_t*)m) + 9; /* word right after m[2][2] */
    *pad = (m->m[2][2] < 0) ? (int16_t)-1 : (int16_t)0;
}

void psyq_MulMatrix0(CPUState* cpu) {
    gte_sync_cpu_to_psyx(cpu);
    MATRIX* m2 = (MATRIX*)PTR(cpu->a2);
    MulMatrix0((MATRIX*)PTR(cpu->a0),
               (MATRIX*)PTR(cpu->a1),
               m2);
    psyq_matrix_swc2_sign_extend_pad(m2);
    gte_sync_psyx_to_cpu(cpu);
}

void psyq_MulMatrix(CPUState* cpu) {
    gte_sync_cpu_to_psyx(cpu);
    MATRIX* m0 = (MATRIX*)PTR(cpu->a0);
    MulMatrix(m0, (MATRIX*)PTR(cpu->a1));
    psyq_matrix_swc2_sign_extend_pad(m0);
    gte_sync_psyx_to_cpu(cpu);
}

/* --- libpad -------------------------------------------------------------- */

void psyq_PadInitDirect(CPUState* cpu) {
    uint32_t buf0 = cpu->a0;  /* &g_PadBuffer */
    uint32_t buf1 = cpu->a1;  /* &g_PadBufferSecondController */
    PadInitDirect((unsigned char*)PTR(buf0),
                  (unsigned char*)PTR(buf1));

    /* Real PSYQ PadInitDirect (0x8006A4xx region) sets up libpad's pad-state
     * machine: a 10-entry dispatch table at 0x800751BC (one fnptr per
     * PadStateXxx), plus per-slot bookkeeping at 0x80075C58+ and 0x80075D40+
     * (pointers to actuator buffers, byte masks, status caches). PsyCross's
     * libpad implementation tracks state host-side and never writes those
     * PSX-side bytes, leaving 60+ EXE/data bytes zero on our side. Replay
     * the observed values verbatim — function pointers are PSYQ code (not
     * relocated), the buffer addresses come from a0/a1. */

    /* Pad-state dispatch table @ 0x800751BC (10 PSYQ fnptrs). */
    static const struct { uint32_t off; uint32_t val; } pad_dispatch[] = {
        {0x800751BCu, 0x8006B1D8u},
        {0x800751C0u, 0x8006B170u},
        {0x800751C4u, 0x8006B2DCu},
        {0x800751C8u, 0x8006B398u},
        {0x800751CCu, 0x8006B614u},
        {0x800751D0u, 0x8006B64Cu},
        {0x800751D4u, 0x8006B6D4u},
        {0x800751D8u, 0x8006BAE8u},
        {0x800751DCu, 0x8006B7A8u},
        {0x800751E0u, 0x8006B2CCu},
        {0x800751F0u, 0x80075D18u},
        {0x800751F4u, 0x00000001u},
    };
    for (size_t i = 0; i < sizeof(pad_dispatch)/sizeof(pad_dispatch[0]); i++) {
        cpu->write_word(pad_dispatch[i].off, pad_dispatch[i].val);
    }

    /* Per-pad slot tables. Buffer pointers depend on a0/a1; the rest are
     * PSYQ data. Slot 0 ("g_PadBuffer") writes through 0x80075D40+; slot 1
     * ("g_PadBufferSecondController") through 0x80075E38+. Inner pointers
     * (0x80075CAB / 0x80075CF3 / 0x80075C88 / 0x80075CD0) are PSYQ
     * scratch-buffer locations inside the same data block. */
    cpu->write_word(0x80075C58u, 0x0000B058u);
    cpu->write_word(0x80075C5Cu, 0x8006969Cu);
    cpu->write_word(0x80075C60u, 0x80069634u);
    cpu->write_word(0x80075D28u, 0x80075D18u);
    cpu->write_word(0x80075D40u, buf0);
    cpu->write_word(0x80075D48u, buf1);
    cpu->write_word(0x80075D4Cu, 0x00000002u);
    cpu->write_word(0x80075D54u, 0x80075C88u);
    cpu->write_word(0x80075D58u, 0x80075CD0u);
    cpu->write_word(0x80075D74u, 0xFFFFFF00u);
    cpu->write_word(0x80075D78u, 0x00FFFFFFu);
    cpu->write_word(0x80075E18u, 0x80075E08u);
    cpu->write_word(0x80075E38u, buf1);
    cpu->write_word(0x80075E44u, 0x80075CABu);
    cpu->write_word(0x80075E48u, 0x80075CF3u);
    cpu->write_word(0x80075E64u, 0xFFFFFF00u);
    cpu->write_word(0x80075E68u, 0x00FFFFFFu);
}

/* PSYQ libpad slot-2 canonical-state mirror.
 *
 * PsyCross's PsyX_Pad_InitPad + PsyX_Pad_UpdateInput unconditionally
 * stamp all `g_controllers[i].padData` slots with "connected pad"
 * bytes (status=0, id=0x41/0x73, buttons, neutral analog) on every
 * poll. Real PSYQ libpad on PSX hardware, with no controller in
 * slot 2, returns the canonical empty-slot status:
 *
 *   slot-2 buffer (g_PadBufferSecondController @ 0x80078E50):
 *     FF 00 00 00 00 00 00 00      (status=0xFF "no controller", rest 0)
 *
 *   D_80078E9C @ 0x80078E9C:
 *     0x00000190                    (libpad-internal slot config word;
 *                                    constant from PadInitDirect onward —
 *                                    PsyCross leaves it 0)
 *
 * Captured both values from a real PCSX-Redux RAM dump at the first
 * GamestateDraw hit and confirmed constant across hits 1..50. Mirroring
 * per-vsync rather than one-shot at PadInitDirect because PsyCross
 * overwrites the slot-2 buffer on every input poll, so a one-shot
 * wouldn't persist past the first frame.
 *
 * Closes the last open region in the PCSX-Redux oracle gate
 * ('pad_input_buffers'); see manifest/gameplay_state_regions.toml. */
static void psyq_libpad_mirror_canonical_slot2(CPUState* cpu) {
    /* slot-2 buffer: 8 bytes packed as two LE u32 writes. */
    cpu->write_word(0x80078E50u, 0x000000FFu);  /* FF 00 00 00 */
    cpu->write_word(0x80078E54u, 0x00000000u);  /* 00 00 00 00 */
    cpu->write_word(0x80078E9Cu, 0x00000190u);
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

/* Shared body — used by both psyq_CdControl (recomp path, args from
 * cpu->aN as PSX vaddrs) and openpete_CdControl (native path, args are
 * already host pointers). Avoids the host_to_psx round-trip that breaks
 * when native callers pass a stack-local u_char/CdlLOC buffer (see plan
 * §"Pass 5e" — this is the canonical example). */

/* g_CdMusic.m_CurrentTrack (first field) — psx.map 0x800774B0. The
 * CdlSetfilter args can't identify a music track (file is always 1;
 * chan is only track-within-XA-file), so read the game's own index.
 * All play paths write m_CurrentTrack before issuing CdlReadS
 * (music.c:28, music.c:51-68, loaders.c:776). */
static int spyro_current_music_track(void) {
    int32_t track = 0;
    memcpy(&track, openpete_psx_get_ram() + 0x774B0, sizeof(track));
    return (track >= 0 && track < 48) ? (int)track : -1;
}

/* Route an armed CdlReadS to the WAV override if one exists for the
 * current track, else to the XA decoder. Exactly one of the two owns
 * playback; starting either stops the other. */
static void start_music_stream(int sector) {
    if (music_override_try_start(spyro_current_music_track(), sector)) {
        if (PsyX_XA_IsActive()) PsyX_XA_Stop();
        return;
    }
    music_override_stop();
    PsyX_XA_Start(sector, s_xa_filter_file, s_xa_filter_chan);
}

static int cdcontrol_host(u_char com, u_char* param, u_char* result) {
    if (com == CdlSetloc && param) {
        /* Track the head position for the level-segment asset overlay
         * (mod_wad_overlay.c) — CdRead has no location arg. */
        mod_wad_setloc((uint32_t)CdPosToInt((CdlLOC*)param));
    }
    if (com == CdlSetfilter) {
        CdlFILTER* f = (CdlFILTER*)param;
        if (f) {
            s_xa_filter_file = (int)f->file;
            s_xa_filter_chan = (int)f->chan;
        }
    } else if (com == CdlReadS && s_xa_armed && param) {
        CdlLOC* loc = (CdlLOC*)param;
        start_music_stream(CdPosToInt(loc));
        g_CD_com = com;
        return 1;
    } else if (com == CdlPause || com == CdlStop) {
        if (PsyX_XA_IsActive()) PsyX_XA_Stop();
        music_override_stop();
    }
    return CdControl(com, param, result);
}

void psyq_CdControl(CPUState* cpu) {
    u_char com = (u_char)cpu->a0;
    u_char* param = (u_char*)PTR(cpu->a1);
    u_char* result = (u_char*)PTR(cpu->a2);
    OPL_INFO_F("thunks", "CdControl com=0x%02X param=0x%08X result=0x%08X",
               (unsigned)com, cpu->a1, cpu->a2);
    cpu->v0 = (uint32_t)cdcontrol_host(com, param, result);
}

/* Hand-written native-side thunk (matches outbound_psyq_hand entry).
 * Bypasses the cpu->aN packing so callers can pass stack-local buffers
 * that wouldn't survive host_to_psx → 0 → NULL deref. */
extern "C" int openpete_CdControl(u_char com, u_char* param, u_char* result) {
    OPL_INFO_F("thunks", "CdControl com=0x%02X param=%p result=%p (native)",
               (unsigned)com, (void*)param, (void*)result);
    return cdcontrol_host(com, param, result);
}

void psyq_CdControlB(CPUState* cpu) {
    u_char com = (u_char)cpu->a0;
    u_char* param = (u_char*)PTR(cpu->a1);

    /* XA streaming arming. Spyro's SetMusicState issues exactly:
     *   CdControlB(CdlSetmode,   {SF|RT|Speed})
     *   CdControlB(CdlSetfilter, {file=1, chan=track%8})
     *   CdControlB(CdlReadS,     &startLoc)
     * which is the canonical "start XA streaming" sequence. */
    switch (com) {
        case CdlSetloc:
            if (param)
                mod_wad_setloc((uint32_t)CdPosToInt((CdlLOC*)param));
            break;
        case CdlSetmode: {
            uint8_t mode = param ? param[0] : 0;
            s_xa_armed = ((mode & CdlModeRT) && (mode & CdlModeSF));
            break;
        }
        case CdlSetfilter: {
            CdlFILTER* f = (CdlFILTER*)param;
            if (f) {
                s_xa_filter_file = (int)f->file;
                s_xa_filter_chan = (int)f->chan;
            }
            break;
        }
        case CdlReadS: {
            if (s_xa_armed && param) {
                CdlLOC* loc = (CdlLOC*)param;
                int sector = CdPosToInt(loc);
                start_music_stream(sector);
                /* Skip PsyCross's CdControlB body — it would flip readMode
                 * to RM_DATA and seek g_imageFile, which our XA pump owns
                 * for the duration of the stream. We still set g_CD_com so
                 * CdLastCom() reports correctly. */
                g_CD_com = com;
                cpu->v0 = 1u;
                return;
            }
            break;
        }
        case CdlPause:
        case CdlStop:
        case CdlReadN:
            if (PsyX_XA_IsActive()) PsyX_XA_Stop();
            music_override_stop();
            break;
        default: break;
    }

    cpu->v0 = (uint32_t)CdControlB(com, param, (u_char*)PTR(cpu->a2));
}

/* `g_CD_com` is the "last command" int CdLastCom() returns. `readMode`
 * is the enum CdReadSync switches on (0=RM_DATA, 1=RM_XA_AUDIO). Both
 * are file-scope non-static globals in PsyCross/src/psx/LIBCD.C; we
 * link to them by name. (Forward declared at the top of this file.) */

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
 * needs to keep the overlay's bytes coherent (separate task). */
#include "spyro_overlay_table.h"

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
    /* Level-segment asset overlay (rung 9): serve mod-supplied bytes over
     * the just-drained read BEFORE the game's loader parses them. Also
     * advances the tracked head position. */
    mod_wad_apply((uint8_t*)buf, sectors);
    /* Verify g_CdState.m_IsReading at 0x80076BB8 is cleared after the cb. */
    uint8_t* ram = openpete_psx_get_ram();
    uint32_t before = 0;
    memcpy(&before, ram + 0x76BB8, 4);
    if (g_native_cd_read_cb != nullptr) {
        /* Native registrar (post-rewriter init.c). Fire the host cb
         * directly — no cpu dispatch round-trip needed. */
        g_native_cd_read_cb(/*intr=CdlComplete=*/2u, /*result=*/nullptr);
    } else {
        invoke_psx_callback(cpu, g_cd_read_cb, /*intr=CdlComplete=*/2u, 0u);
    }
    uint32_t after = 0;
    memcpy(&after, ram + 0x76BB8, 4);
    if (after != 0u) {
        OPL_WARN_F("thunks",
                   "CdRead: m_IsReading still set after callback (before=%u after=%u cb=0x%08X native_cb=%p)",
                   before, after, g_cd_read_cb, (void*)g_native_cd_read_cb);
    }

    /* After the bytes have actually settled in RAM, peek the overlay-id
     * word at 0x8007AA38. The recompiler treats `LEVEL_BALLOON(id, …)` as
     * `LONG(id)`, so the id is the first word of every overlay; cutscene
     * scratch writes to the same vaddr have a different first word and
     * leave the tracker unchanged. */
    if (cpu->a1 == 0x8007AA38u) {
        uint32_t magic = 0;
        memcpy(&magic, ram + 0x7AA38, 4);
        spyro_set_active_overlay_by_magic(magic);
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

    /* Pump XA every CdSync — this is Spyro's per-frame music tick. */
    PsyX_XA_Pump(0);
    music_override_pump();

    int rc = CdSync(mode, result);

    /* Native-side spin guard for CDLoadSync's `while (CdSync(1, NULL) !=
     * CdlComplete)` polls. The host hang watchdog in core/runtime/dispatch.c
     * only catches recomp dispatches; a fully-native spin via this host
     * thunk → host C goes undetected. Per-frame counter resets on frame
     * advance. Default off; set OPENPETE_PSYQ_CDSYNC_LIMIT to a positive
     * count to enable. Useful when re-enabling init.c/completion.c
     * surfaces a CdSync-shaped hang. */
    const char* env = std::getenv("OPENPETE_PSYQ_CDSYNC_LIMIT");
    if (env && *env) {
        uint32_t limit = (uint32_t)std::strtoul(env, NULL, 0);
        static uint32_t cs_calls_this_frame = 0u;
        static uint32_t cs_last_frame = 0u;
        if (openpete_frame != cs_last_frame) {
            cs_last_frame = openpete_frame;
            cs_calls_this_frame = 0u;
        }
        if (limit && ++cs_calls_this_frame > limit) {
            uint8_t* ram = openpete_psx_get_ram();
            uint32_t isReading = 0u;
            memcpy(&isReading, ram + 0x76BB8, 4);
            OPL_ERROR_F("thunks",
                        "psyq_CdSync spin guard: %u calls in frame=%u "
                        "(rc=%d m_IsReading=%u s0=0x%08X ra=0x%08X) — exiting",
                        cs_calls_this_frame, openpete_frame, rc, isReading,
                        cpu->s0, cpu->ra);
            std::fflush(stdout); std::fflush(stderr);
            std::_Exit(3);
        }
    }

    /* Override PsyCross's GetlocP fake-position with the real XA (or
     * WAV-override synthetic) playhead so Spyro's track-end detection
     * (compares position to m_MusicTable.end) actually fires.
     * result[5..7] = absolute MSF. */
    if ((PsyX_XA_IsActive() || music_override_is_active())
        && g_CD_com == CdlGetlocP && mode == 1) {
        CdlLOC abs;
        CdIntToPos(music_override_is_active() ? music_override_get_sector()
                                              : PsyX_XA_GetCurrentSector(),
                   &abs);
        result[5] = abs.minute;
        result[6] = abs.second;
        result[7] = abs.sector;
    }
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

/* Native-side bridge: post-rewriter cd.c calls CdReadCallback() expecting
 * to register a host C function. We store it for psyq_CdRead to fire
 * (instead of dispatching into recomp territory). Renamed from
 * CdReadCallback in psyq_compat.h. */
extern "C" void openpete_CdReadCallback(native_cd_read_cb_t cb) {
    OPL_INFO_F("thunks", "openpete_CdReadCallback registered host cb=%p",
               (void*)cb);
    g_native_cd_read_cb = cb;
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

#include "openpete_mods.h" /* SFX overlay service (mods/mod_sfx.c) */

/* ── SFX overlay: Spyro-1 glue ─────────────────────────────────────────
 * Names for the game's SoundTable slots (spyro-1/include/sound_table.h;
 * slot value = index into the per-level SoundDefinition array). The
 * resolver maps a keyed voice's sample addr back to the first named
 * slot whose definition owns that addr; the uploader stages replacement
 * ADPCM into the free SPU tail after the level bank. Everything here is
 * host-side: guest RAM and the SPU bank image stay byte-stock.
 */
static const struct { uint8_t slot; const char* name; } k_sfx_names[] = {
    {0x00, "gem_pickup"},   {0x03, "chest_break"},
    {0x04, "spyro_charge"}, {0x05, "super_charge"},
    {0x06, "waterfall"},    {0x1A, "exit_vortex"},
    {0x1B, "spyro_stars"},  {0x1D, "water_splash"},
    {0x1E, "spyro_walk"},   {0x20, "spyro_unsquish"},
    {0x21, "flight_pling"}, {0x23, "flight_collected"},
    {0x26, "moby_poof"},    {0x28, "spyro_flame"},
    {0x2C, "menu_sound"},   {0x2D, "menu_cursor"},
    {0x2E, "menu_confirm"}, {0x2F, "pickup_ding"},
};

#define SPYRO_SPU_SOUNDDEFS_VA  0x800761D0u /* g_Spu.m_SoundDefinitions */
#define SPYRO_SPU_SOUNDTABLE_VA 0x800761D4u /* g_Spu.m_SoundTable */
#define SPYRO_LEVELHDR_VRAMSPU  0x80076C04u /* g_LevelHeader.m_VramSramSize */
#define SPYRO_SOUNDDEF_STRIDE   0x14u
#define SPYRO_SPU_BANK_BLAST    (0x80000u - 0x1010u) /* level SPU upload */
#define SPYRO_SPU_TAIL_CEIL     0x7EF00u

static const char* spyro_sfx_resolve(uint32_t spu_addr) {
    uint32_t defs_va  = *(uint32_t*)PTR(SPYRO_SPU_SOUNDDEFS_VA);
    uint32_t table_va = *(uint32_t*)PTR(SPYRO_SPU_SOUNDTABLE_VA);
    if (!defs_va || !table_va) return nullptr;
    const uint8_t* table = (const uint8_t*)PTR(table_va);
    const uint8_t* defs  = (const uint8_t*)PTR(defs_va);
    int count = *(const int32_t*)PTR(table_va + 256);
    if (!table || !defs || count <= 0 || count > 256) return nullptr;
    for (const auto& e : k_sfx_names) {
        uint8_t id = table[e.slot];
        if (id >= count) continue;
        uint32_t addr;
        memcpy(&addr, defs + id * SPYRO_SOUNDDEF_STRIDE, 4);
        if (addr == spu_addr) return e.name;
    }
    return nullptr;
}

static uint32_t s_sfx_tail; /* next free SPU byte this bank-generation */

static uint32_t spyro_sfx_upload(const void* body, uint32_t len) {
    if (!s_sfx_tail) {
        uint32_t vram = *(uint32_t*)PTR(SPYRO_LEVELHDR_VRAMSPU);
        if (vram < 0x80000u || vram > 0x100000u) return 0;
        s_sfx_tail = (0x1010u + vram - 0x80000u + 15u) & ~15u;
    }
    if (s_sfx_tail + len > SPYRO_SPU_TAIL_CEIL) return 0;
    uint32_t dest = s_sfx_tail;
    SpuSetTransferStartAddr(dest);
    SpuWrite((unsigned char*)const_cast<void*>(body), len);
    s_sfx_tail = (dest + len + 15u) & ~15u;
    return dest;
}

void psyq_SpuInit(CPUState* cpu) {
    (void)cpu;
    SpuInit();
    openpete_mods_sfx_set_fns(spyro_sfx_resolve, spyro_sfx_upload);
}

void psyq_SpuSetCommonAttr(CPUState* cpu) {
    SpuCommonAttr* attr = (SpuCommonAttr*)PTR(cpu->a0);
    SpuSetCommonAttr(attr);
    /* Spyro's music fader writes mask=SPU_COMMON_CDVOLL|SPU_COMMON_CDVOLR
     * with values in 0..0x3FFF. Route that to the XA streaming source's
     * gain (avg the two channels for a single mono gain). The WAV
     * override gets the same value so the in-game music slider and
     * track-transition fades cover both paths. */
    if (attr && (attr->mask & (SPU_COMMON_CDVOLL | SPU_COMMON_CDVOLR))) {
        int l = attr->cd.volume.left;
        int r = attr->cd.volume.right;
        if (l < 0) l = 0; if (r < 0) r = 0;
        float g = (float)((l + r) / 2) / (float)0x3FFF;
        PsyX_XA_SetVolume(g);
        music_override_set_gain(g);
    }
}

/* Per-voice SFX-overlay state: Spyro's per-tick voice updater
 * (spu.c SpuUpdate ~:470) re-applies SPU_VOICE_PITCH from the guest
 * ActiveSound (= the STOCK definition pitch) every tick with no WDSA in
 * the mask — a key-on-only substitution would be overwritten one tick
 * later (heard as the replacement racing at the stock rate). Track the
 * override per voice and keep re-applying until the voice is re-keyed. */
static uint16_t s_sfx_voice_pitch[24]; /* 0 = no override */
static uint8_t  s_sfx_voice_mute[24];

void psyq_SpuSetVoiceAttr(CPUState* cpu) {
    /* SFX overlay: substitute on a HOST COPY only — the guest attr (and
     * everything the sim can read) stays byte-stock. */
    SpuVoiceAttr attr = *(SpuVoiceAttr*)PTR(cpu->a0);
    if (attr.mask & SPU_VOICE_WDSA) {
        uint32_t n_addr = 0; uint16_t n_pitch = 0; int mute = 0;
        int sub = openpete_mods_sfx_on_key(attr.addr, attr.pitch,
                                           &n_addr, &n_pitch, &mute);
        for (int v = 0; v < 24; v++) {
            if (!(attr.voice & (1u << v))) continue;
            s_sfx_voice_pitch[v] = (sub && !mute) ? n_pitch : 0;
            s_sfx_voice_mute[v]  = (uint8_t)(sub && mute);
        }
        if (sub) {
            if (mute) {
                attr.volume.left = attr.volume.right = 0;
                attr.mask |= SPU_VOICE_VOLL | SPU_VOICE_VOLR;
            } else {
                attr.addr = n_addr;
                attr.pitch = n_pitch;
                attr.mask |= SPU_VOICE_PITCH;
                if (attr.mask & SPU_VOICE_LSAX) attr.loop_addr = n_addr;
            }
        }
    } else {
        /* pitch/volume refresh from guest state: keep overridden voices
         * overridden. (Refreshes are per-voice single-bit calls; the
         * SPU_ALLCH boot init runs before any override exists.) */
        for (int v = 0; v < 24; v++) {
            if (!(attr.voice & (1u << v))) continue;
            if (s_sfx_voice_pitch[v] && (attr.mask & SPU_VOICE_PITCH))
                attr.pitch = s_sfx_voice_pitch[v];
            if (s_sfx_voice_mute[v] &&
                (attr.mask & (SPU_VOICE_VOLL | SPU_VOICE_VOLR)))
                attr.volume.left = attr.volume.right = 0;
        }
    }
    SpuSetVoiceAttr(&attr);
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
    SpuSetKey((int)cpu->a0, (unsigned int)cpu->a1);
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
    /* The level-load bank blast overwrites all of SPU RAM including any
     * staged SFX replacements — invalidate them (re-staged lazily). */
    if ((unsigned int)cpu->a1 == SPYRO_SPU_BANK_BLAST) {
        openpete_mods_sfx_bank_reset();
        s_sfx_tail = 0;
    }
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

/* PsyCross's libmcrd is broken for our needs (MemCardAccept fopen()s
 * "0.MCD" and fread()s an unopened FILE*; it indexes memoryCards[chan]
 * with the raw port encoding 0x10, out-of-bounds). We ignore it entirely
 * and marshal into openpete's host card module (core/host/memcard.c),
 * which owns two real 128 KiB .mcr images persisted under the per-user
 * state dir. See notes/plans/openpete-memcard-host-backed.md. */

void psyq_MemCardInit(CPUState* cpu) {
    openpete_memcard_init();

    /* Real PSYQ MemCardInit (0x8006626C) clears two state words then
     * jumps into func_80067E70, which OpenEvent-registers a chain of
     * BIOS events and threads handlers through PSYQ's libgpu-style
     * control struct. The card-side struct lives at 0x800748F0+ and
     * shadows the same ABI as libgpu's at 0x80073924. PsyCross stubs
     * skip the entire chain, so 15 EXE bytes remain zero on our side.
     * Replay the observed post-init values directly. */
    cpu->write_byte(0x80074904u, 0x01u);          /* "card init done" */
    cpu->write_word(0x8007490Cu, 0x8005E124u);    /* fnptr */
    cpu->write_word(0x80074924u, 0x80073924u);    /* libgpu struct backref */
    cpu->write_byte(0x8007492Cu, 0x01u);
    cpu->write_word(0x80074938u, 0x8005DFD8u);    /* fnptr */
    cpu->write_byte(0x800749E0u, 0x05u);          /* registered handler count */

    /* MemCardInit's MIPS prologue: sp -= 0x18; sw ra,0x10(sp). */
    psyq_mirror_stack_frame(cpu, 0x18u, 0x10u, NULL, 0);
}

void psyq_MemCardStart(CPUState* cpu) {
    (void)cpu;
    openpete_memcard_start();
}

void psyq_MemCardStop(CPUState* cpu) {
    (void)cpu;
    openpete_memcard_stop();
}

/* Exist/Accept surface only v0 here; the libmcrd result code reaches the
 * game through the async latch that MemCardSync reads (the module sets it). */
void psyq_MemCardExist(CPUState* cpu) {
    cpu->v0 = (uint32_t)openpete_memcard_exist((int)cpu->a0, nullptr, nullptr);
}

void psyq_MemCardAccept(CPUState* cpu) {
    cpu->v0 = (uint32_t)openpete_memcard_accept((int)cpu->a0, nullptr, nullptr);
}

/* MemCardReadFile(chan, name, adrs, ofs, bytes): a0=chan, a1=name, a2=RAM
 * dest, a3=file offset, [sp+0x10]=bytes. */
void psyq_MemCardReadFile(CPUState* cpu) {
    cpu->v0 = (uint32_t)openpete_memcard_read((int)cpu->a0,
                                              (const char*)PTR(cpu->a1),
                                              PTR(cpu->a2),
                                              (int)cpu->a3,
                                              (int)cpu->read_word(cpu->sp + 0x10));
}

void psyq_MemCardWriteFile(CPUState* cpu) {
    cpu->v0 = (uint32_t)openpete_memcard_write((int)cpu->a0,
                                               (const char*)PTR(cpu->a1),
                                               PTR(cpu->a2),
                                               (int)cpu->a3,
                                               (int)cpu->read_word(cpu->sp + 0x10));
}

void psyq_MemCardCreateFile(CPUState* cpu) {
    cpu->v0 = (uint32_t)openpete_memcard_create((int)cpu->a0,
                                                (const char*)PTR(cpu->a1),
                                                (int)cpu->a2);
}

void psyq_MemCardFormat(CPUState* cpu) {
    cpu->v0 = (uint32_t)openpete_memcard_format((int)cpu->a0);
}

void psyq_MemCardSync(CPUState* cpu) {
    /* mode 0 = Sync, 1 = Async; host I/O completes instantly so both report
     * Finished once an op has run, NothingRegistered (-1) before. Pre-read
     * the guest out-slots so a field the module leaves unset round-trips
     * unchanged (matches the original stub's "only overwrite when set"). */
    int* cmds_out = cpu->a1 ? (int*)PTR(cpu->a1) : nullptr;
    int* rslt_out = cpu->a2 ? (int*)PTR(cpu->a2) : nullptr;
    int c = cmds_out ? *cmds_out : 0;
    int r = rslt_out ? *rslt_out : 0;
    int st = openpete_memcard_sync((int)cpu->a0, &c, &r);
    if (cmds_out) *cmds_out = c;
    if (rslt_out) *rslt_out = r;
    cpu->v0 = (uint32_t)st;
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

/* PSYQ rand/srand: byte-identical re-implementation of the LCG at
 * 0x8006272C / 0x8006275C, including the live mutation of seed slot
 * D_80075AC0 in PSX RAM. The previous implementation routed through
 * host libc `rand()` / `srand()`, which (a) doesn't match the PSYQ
 * sequence, and (b) leaves D_80075AC0 — the gameplay-state hash's
 * `psyq_rand_seed` region — uninitialized at zero forever. Result:
 * divergent rand-call counts between presentation modes were
 * completely invisible to the determinism gate, masking real
 * render→gameplay leaks (any code path that called rand() a
 * different number of times based on render-side state would drift
 * gameplay silently). Mirroring the asm restores both correctness
 * and detectability. PSYQ formula:
 *     seed = seed * 0x41C64E6D + 0x3039
 *     return (seed >> 16) & 0x7FFF */
extern "C" uint8_t* openpete_psx_get_ram(void);
static constexpr uint32_t PSYQ_RAND_SEED_VADDR = 0x80075AC0u;
static inline uint32_t psyq_seed_load(void) {
    const uint8_t* ram = openpete_psx_get_ram();
    uint32_t off = PSYQ_RAND_SEED_VADDR & 0x1FFFFFu;
    return  (uint32_t)ram[off]
         | ((uint32_t)ram[off + 1] << 8)
         | ((uint32_t)ram[off + 2] << 16)
         | ((uint32_t)ram[off + 3] << 24);
}
static inline void psyq_seed_store(uint32_t v) {
    uint8_t* ram = openpete_psx_get_ram();
    uint32_t off = PSYQ_RAND_SEED_VADDR & 0x1FFFFFu;
    ram[off    ] = (uint8_t)(v        & 0xFF);
    ram[off + 1] = (uint8_t)((v >> 8 ) & 0xFF);
    ram[off + 2] = (uint8_t)((v >> 16) & 0xFF);
    ram[off + 3] = (uint8_t)((v >> 24) & 0xFF);
}
void psyq_rand(CPUState* cpu) {
    uint32_t seed = psyq_seed_load();
    seed = seed * 0x41C64E6Du + 0x3039u;
    psyq_seed_store(seed);
    cpu->v0 = (seed >> 16) & 0x7FFFu;
}

void psyq_srand(CPUState* cpu) {
    psyq_seed_store(cpu->a0);
}

void psyq_exit(CPUState* cpu) {
    OPL_INFO_F("thunks", "exit(%u) from compiled code", cpu->a0);
    exit((int)cpu->a0);
}

/* printf / sprintf reconstruct MIPS o32 varargs: first 4 slots in
 * a0..a3, then sp+0x10, sp+0x14, ... The first slot is the format
 * string (or, for sprintf, dst+fmt occupy slots 0/1). Numeric args
 * are fetched as 32-bit words; %s dereferences a PSX vaddr.
 *
 * Supported conversions: d, i, u, x, X, o, s, c, %. Flags '-' and '0',
 * a decimal width, and a '.precision' are honored. Length modifiers
 * 'l' / 'h' / 'hh' / 'll' parse but are ignored — the original PSX ABI
 * passes everything as a 32-bit slot anyway. */

namespace {

struct MipsArgs {
    CPUState* cpu;
    int       slot;        /* next slot index (0 = a0, 4 = sp+0x10, ...) */

    uint32_t fetch() {
        uint32_t v;
        switch (slot) {
            case 0: v = cpu->a0; break;
            case 1: v = cpu->a1; break;
            case 2: v = cpu->a2; break;
            case 3: v = cpu->a3; break;
            default: v = cpu->read_word(cpu->sp + (uint32_t)(slot * 4));
                     break;
        }
        ++slot;
        return v;
    }
};

/* Render an unsigned integer in the given base. */
size_t render_uint(uint32_t v, unsigned base, bool upper, char* out) {
    if (v == 0u) { out[0] = '0'; return 1u; }
    char tmp[32];
    size_t n = 0;
    const char* digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    while (v) { tmp[n++] = digs[v % base]; v /= base; }
    for (size_t i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
    return n;
}

/* Core: format `fmt` into `dst` (capacity `cap`), pulling args from `args`.
 * Returns total length that would have been written (snprintf semantics). */
size_t format_from_mips(char* dst, size_t cap, const char* fmt, MipsArgs& args) {
    size_t pos = 0;
    if (cap == 0) return 0;
    /* Reserve one byte for NUL — emit() guards against pos >= cap-1 below. */
    size_t writable = cap - 1u;
    auto put = [&](char c) {
        if (pos < writable) dst[pos] = c;
        ++pos;
    };
    auto put_str = [&](const char* s, size_t n,
                       int width, bool left_align, bool zero_pad) {
        size_t pad = (width > 0 && (size_t)width > n) ? (size_t)width - n : 0u;
        char pc = zero_pad ? '0' : ' ';
        if (!left_align) for (size_t i = 0; i < pad; ++i) put(pc);
        for (size_t i = 0; i < n; ++i) put(s[i]);
        if (left_align) for (size_t i = 0; i < pad; ++i) put(' ');
    };

    while (*fmt) {
        char c = *fmt++;
        if (c != '%') { put(c); continue; }

        bool left_align = false, zero_pad = false;
        for (;;) {
            if (*fmt == '-') { left_align = true; ++fmt; }
            else if (*fmt == '0') { zero_pad = true; ++fmt; }
            else if (*fmt == '+' || *fmt == ' ' || *fmt == '#') { ++fmt; }
            else break;
        }
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt++ - '0'); }
        int precision = -1;
        if (*fmt == '.') {
            ++fmt;
            precision = 0;
            while (*fmt >= '0' && *fmt <= '9') { precision = precision * 10 + (*fmt++ - '0'); }
        }
        /* Skip length modifiers: h, hh, l, ll, z, j, t. PSX ABI passes
         * everything as a 32-bit slot, so they don't change behavior. */
        while (*fmt == 'h' || *fmt == 'l' || *fmt == 'z' || *fmt == 'j' || *fmt == 't') ++fmt;

        char conv = *fmt ? *fmt++ : '\0';
        char numbuf[40];
        switch (conv) {
            case '%': put('%'); break;
            case 'c': {
                char ch = (char)args.fetch();
                put_str(&ch, 1, width, left_align, false);
                break;
            }
            case 's': {
                uint32_t addr = args.fetch();
                const char* s = addr ? (const char*)PTR(addr) : "(null)";
                size_t n = 0;
                if (precision >= 0) {
                    while (n < (size_t)precision && s[n]) ++n;
                } else {
                    n = strlen(s);
                }
                put_str(s, n, width, left_align, false);
                break;
            }
            case 'd':
            case 'i': {
                int32_t sv = (int32_t)args.fetch();
                bool neg = sv < 0;
                uint32_t uv = neg ? (uint32_t)(-(int64_t)sv) : (uint32_t)sv;
                size_t n = render_uint(uv, 10, false, numbuf);
                if (neg) {
                    /* Zero-pad after sign. */
                    if (zero_pad && !left_align && width > (int)(n + 1)) {
                        put('-');
                        for (size_t i = 0; i < (size_t)width - n - 1; ++i) put('0');
                        for (size_t i = 0; i < n; ++i) put(numbuf[i]);
                    } else {
                        char tmp[41];
                        tmp[0] = '-';
                        for (size_t i = 0; i < n; ++i) tmp[i + 1] = numbuf[i];
                        put_str(tmp, n + 1, width, left_align, false);
                    }
                } else {
                    put_str(numbuf, n, width, left_align, zero_pad && !left_align);
                }
                break;
            }
            case 'u': {
                uint32_t uv = args.fetch();
                size_t n = render_uint(uv, 10, false, numbuf);
                put_str(numbuf, n, width, left_align, zero_pad && !left_align);
                break;
            }
            case 'x':
            case 'X': {
                uint32_t uv = args.fetch();
                size_t n = render_uint(uv, 16, conv == 'X', numbuf);
                put_str(numbuf, n, width, left_align, zero_pad && !left_align);
                break;
            }
            case 'o': {
                uint32_t uv = args.fetch();
                size_t n = render_uint(uv, 8, false, numbuf);
                put_str(numbuf, n, width, left_align, zero_pad && !left_align);
                break;
            }
            case 'p': {
                uint32_t uv = args.fetch();
                numbuf[0] = '0'; numbuf[1] = 'x';
                size_t n = render_uint(uv, 16, false, numbuf + 2);
                put_str(numbuf, n + 2, width, left_align, false);
                break;
            }
            default:
                /* Unknown — emit the '%' and the conversion char verbatim. */
                put('%');
                if (conv) put(conv);
                break;
        }
    }
    if (pos < cap) dst[pos] = '\0';
    else dst[cap - 1] = '\0';
    return pos;
}

}  /* namespace */

void psyq_printf(CPUState* cpu) {
    const char* fmt = (const char*)PTR(cpu->a0);
    if (!fmt) { cpu->v0 = 0u; return; }
    char buf[512];
    MipsArgs args{cpu, 1};
    size_t n = format_from_mips(buf, sizeof(buf), fmt, args);
    OPL_INFO_F("psx-printf", "%s", buf);
    cpu->v0 = (uint32_t)n;
}

void psyq_sprintf(CPUState* cpu) {
    char* dst = (char*)PTR(cpu->a0);
    const char* fmt = (const char*)PTR(cpu->a1);
    if (!dst || !fmt) { cpu->v0 = 0u; return; }
    /* No host-side bound — game allocates the buffer. Use a generous
     * cap; sprintf has no length argument on the PSX side. */
    MipsArgs args{cpu, 2};
    size_t n = format_from_mips(dst, 4096u, fmt, args);
    cpu->v0 = (uint32_t)n;
}

}  /* extern "C" */
