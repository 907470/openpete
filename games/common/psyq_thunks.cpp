/* games/common/psyq_thunks.cpp — PSYQ → PsyCross ABI thunks, shared by every
 * game.
 *
 * The recompiler emits PSYQ-targeted JALs as `psyq_NAME(cpu)` (with
 * --external-prefix=psyq_). This file provides hand-written bodies for the
 * named PSYQ functions a game calls, translating between the MIPS calling
 * convention (args in a0..a3 + sp-relative slots, return in v0) and PsyCross's
 * native C signatures.
 *
 * Nothing here is specific to one game. What is — the PSYQ-internal structures
 * a thunk mirrors into PSX RAM at a game's own addresses, the stack frame each
 * real PSYQ function leaves behind, and couplings to a game's own features —
 * is reached through psyq_game_hooks.h, whose every hook has an inert default.
 * A game supplies only what it has.
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
#include "openpete_ticker.h"
#include "openpete_psx.h"
#include "openpete_memcard.h"
#include "psyq_game_hooks.h"
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
    void PsyX_SPUAL_SetMasterVolume(float linear_gain);
}

/* Music WAV override (core/host/) — plays assets/music/track_NN.wav in
 * place of the XA stream when present. See
 * notes/plans/openpete-music-wav-override.md. */
#include "music_override.h"
#include "openpete_mods.h"          /* custom-level song lookup + SFX overlay */
#include "openpete_profile.h"       /* game-fiber cost split (--profile-out) */

/* PsyCross internals shared with the libcd thunks below. */
extern "C" int g_CD_com;
extern "C" int readMode;  /* 0=RM_DATA, 1=RM_XA_AUDIO */
extern "C" int g_cdXaStartLBA;   /* virtual music head anchor — LIBCD.C */
extern "C" int g_cdXaStartVBL;

/* Spyro music streaming state. CdControlB(CdlSetmode) with RT|SF arms XA;
 * CdControlB(CdlSetfilter) records file/chan; CdControlB(CdlReadS) starts
 * streaming at the requested sector. CdControlB(CdlPause/Stop/CdlReadN)
 * stops it. State is small enough to keep file-static. */
static bool s_xa_armed       = false;
static int  s_xa_filter_file = 1;
static int  s_xa_filter_chan = 0;

/* Guest-mutated thunk latches for the savestate extra payload
 * (game_extras_impl.c). CdlSetmode flips s_xa_armed (RT|SF arms the XA
 * music route, the loader's data mode disarms it) and callback
 * registrations swap guest vaddrs — a rolled-back speculative tick that
 * ran transition code leaves them on the abandoned branch otherwise,
 * and the committed timeline's next CdlReadS routes down the wrong
 * path. Declared early; definitions live after the statics they pack. */
extern "C" void spyro1_thunk_state_save(uint32_t* cd_cb, uint32_t* vs_cb,
                                        uint32_t* xa_word);
extern "C" void spyro1_thunk_state_load(uint32_t cd_cb, uint32_t vs_cb,
                                        uint32_t xa_word);
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
/* A guest NULL must stay NULL. PTR(0) resolves to the host address of PSX RAM
 * offset 0, which is a valid pointer, so a PSYQ call that writes through an
 * optional out-parameter would silently overwrite the start of guest RAM
 * instead of skipping the write. Every optional pointer argument goes through
 * this. */
#define PTR_OPT(addr) ((addr) ? PTR(addr) : nullptr)

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

/* PSYQ's interrupt-callback table: one guest handler per PSX interrupt line,
 * installed through the libetc setter the thunk below intercepts. Index 7 is
 * the controller/memory-card port, whose handler is the pump a BIOS-driven
 * card driver advances its state machine in. openpete has no interrupt
 * controller, so each installed handler is driven from the VBL the transfer
 * it services would have completed within. */
#define PSYQ_INTR_SLOTS 11
#define PSYQ_INTR_PAD_CARD 7
static uint32_t g_intr_cb[PSYQ_INTR_SLOTS];
/* Native cd.c (post-rewriter) registers a host-side callback via
 * openpete_CdReadCallback. psyq_CdRead checks both this and g_cd_read_cb;
 * one-or-the-other is set depending on whether the registrar was native
 * or recompiled code. */
typedef void (*native_cd_read_cb_t)(unsigned char intr, unsigned char* result);
static native_cd_read_cb_t g_native_cd_read_cb = nullptr;
static uint32_t g_vsync_cb       = 0;

extern "C" void spyro1_thunk_state_save(uint32_t* cd_cb, uint32_t* vs_cb,
                                        uint32_t* xa_word) {
    *cd_cb = g_cd_read_cb;
    *vs_cb = g_vsync_cb;
    *xa_word = (s_xa_armed ? 1u : 0u)
             | ((uint32_t)(s_xa_filter_file & 0xFF) << 8)
             | ((uint32_t)(s_xa_filter_chan & 0xFF) << 16);
}

extern "C" void spyro1_thunk_state_load(uint32_t cd_cb, uint32_t vs_cb,
                                        uint32_t xa_word) {
    g_cd_read_cb     = cd_cb;
    g_vsync_cb       = vs_cb;
    s_xa_armed       = (xa_word & 1u) != 0;
    s_xa_filter_file = (int)((xa_word >> 8) & 0xFF);
    s_xa_filter_chan = (int)((xa_word >> 16) & 0xFF);
}

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

/* Run the controller/memory-card interrupt handler for one VBL. On hardware
 * the port raises this line once per transfer; the pad is polled every VBL,
 * so one call per VBL advances the driver at the rate the game expects. */
static void psyq_pump_pad_card_intr(CPUState* cpu) {
    uint32_t cb = g_intr_cb[PSYQ_INTR_PAD_CARD];
    if (cb) invoke_psx_callback(cpu, cb, 0, 0);
}

/* --- libgcc/crt0 --------------------------------------------------------- */

/* Defined below, with the libgpu thunks it is mostly used by. */
static void psyq_mirror_stack_frame(CPUState* cpu, psyq_frame_site_t site);

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
    psyq_game_mirror_main(cpu);
    psyq_mirror_stack_frame(cpu, PSYQ_FRAME_MAIN);
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
     * Wall-clock pacing lives elsewhere: the host loop (run.c) sleeps to
     * its own phase-locked deadline between game slices, so 1 yield ≈
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
        /* Query: read the counter, wait for nothing. Every Spyro game's
         * frame wait is `VSync(0); VSync(-1); while (cur - ref < 2)`, so
         * VSync(0) advances the counter and this call only observes it. */
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
        /* Fast-boot: don't yield to the host, so the boot spins run at full
         * CPU speed and openpete_frame stays anchored at 0 until the first
         * frame of real content. The game's own per-frame callbacks still
         * run: a loader that advances its state machine from the VSync
         * handler or the pad/card interrupt line makes no progress without
         * them, and would spin here forever. */
        if (g_vsync_cb) invoke_psx_callback(cpu, g_vsync_cb, 0, 0);
        psyq_pump_pad_card_intr(cpu);
    } else if (openpete_host_fiber) {
        openpete_yield_to_host();
        if (g_vsync_cb) invoke_psx_callback(cpu, g_vsync_cb, 0, 0);
        psyq_pump_pad_card_intr(cpu);
    } else {
        /* No fiber loop (smoke-test path) — fall back to PsyCross's busy
         * wait so audio/timer semantics still match. */
        if (mode == 0) (void)VSync(0);
        if (g_vsync_cb) invoke_psx_callback(cpu, g_vsync_cb, 0, 0);
        psyq_pump_pad_card_intr(cpu);
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
    psyq_game_mirror_vsync_callback(cpu, cpu->a0);
}

void psyq_StopCallback(CPUState* cpu) {
    cpu->v0 = (uint32_t)StopCallback();
}

/* Clears every registered callback and returns the previous VSync one. PSYQ
 * calls this from its own init path before installing handlers. */
void psyq_ResetCallback(CPUState* cpu) {
    cpu->v0 = (uint32_t)ResetCallback();
}

/* DMA completion handlers, by channel.
 *
 * A PSYQ transfer ends in a DMA interrupt, and libcd's read path finishes its
 * work in the channel-3 handler: it is what tells the game a read landed.
 * PsyCross performs the transfer host-side and raises no interrupt, so unless
 * the thunk layer records this handler and calls it when the transfer is done,
 * a game that waits on its own read-completion bookkeeping waits forever.
 *
 * PSX DMA has 7 channels; 3 is the CD-ROM. */
#define PSYQ_DMA_CHANNELS 7
static uint32_t g_dma_cb[PSYQ_DMA_CHANNELS];

void psyq_DMACallback(CPUState* cpu) {
    uint32_t ch = cpu->a0;
    uint32_t prev = 0u;
    if (ch < PSYQ_DMA_CHANNELS) {
        prev = g_dma_cb[ch];
        g_dma_cb[ch] = cpu->a1;
        OPL_INFO_F("thunks", "DMACallback ch=%u -> 0x%08X", ch, cpu->a1);
    }
    cpu->v0 = prev;   /* PSYQ returns the handler it replaced */
}

/* Fire a channel's completion handler, if the game registered one. */
static void psyq_dma_complete(CPUState* cpu, uint32_t ch) {
    if (ch < PSYQ_DMA_CHANNELS && g_dma_cb[ch]) {
        invoke_psx_callback(cpu, g_dma_cb[ch], ch, 0u);
    }
}

/* The remaining libetc registrations return the previous handler and are
 * otherwise inert: PsyCross services those interrupt sources host-side and
 * dispatches no guest handler for them, so recording the address would promise
 * a call that never arrives. Returning 0 says "there was no previous handler",
 * which is what a first registration sees on hardware too. */
void psyq_InterruptCallback(CPUState* cpu) { cpu->v0 = 0u; }
/* Installs a guest handler in the interrupt-callback table: a0 selects the
 * interrupt line, a1 is the handler, and the previous handler is returned. */
void psyq_VSyncCallbacks(CPUState* cpu) {
    uint32_t slot = cpu->a0;
    if (slot >= PSYQ_INTR_SLOTS) {
        OPL_WARN_F("thunks", "interrupt callback for line %u out of range",
                   slot);
        cpu->v0 = 0u;
        return;
    }
    OPL_INFO_F("thunks", "interrupt callback line %u -> 0x%08X", slot,
               cpu->a1);
    uint32_t prev = g_intr_cb[slot];
    g_intr_cb[slot] = cpu->a1;
    cpu->v0 = prev;
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
/* Replay a real PSYQ function's prologue into the guest stack.
 *
 * A PSYQ function pushes a frame and stores ra plus some saved registers; an
 * empty C thunk leaves those slots at whatever was there before, which reads
 * as a RAM difference at every call. The frame is per function and per PSYQ
 * revision, so the descriptor comes from the game; a game that declares none
 * mirrors nothing. */
static void psyq_mirror_stack_frame(CPUState* cpu, psyq_frame_site_t site) {
    psyq_game_frame_t f;
    if (!psyq_game_stack_frame(site, &f)) return;

    const uint32_t sregs[5] = { cpu->s0, cpu->s1, cpu->s2, cpu->s3, cpu->s4 };
    uint32_t sp_b = cpu->sp;
    cpu->sp = sp_b - f.frame_size;
    cpu->write_word(cpu->sp + f.ra_off, cpu->ra);
    for (uint8_t i = 0; i < f.n_sregs && i < 5u; i++) {
        cpu->write_word(cpu->sp + 16u + 4u * i, sregs[i]);
    }
    cpu->sp = sp_b;
}

/* Real ResetGraph at 0x8005F2A4. For mode 0/3/5 (the init paths Spyro uses),
 * PSYQ writes a 128-byte env struct at 0x80074A64 + helper structs via
 * func_80061DEC. NTSC GPU yields gpu_type=0 (the default). */
void psyq_ResetGraph(CPUState* cpu) {
    int mode = (int)cpu->a0;
    cpu->v0 = (uint32_t)ResetGraph(mode);

    psyq_game_mirror_resetgraph(cpu, mode);

    /* ResetGraph's own MIPS prologue: sp -= 32; sw s0,16; sw s1,20; sw ra,24.
     * (sw s2 at +20 was overwritten by sw s1 at the same slot.) */
    psyq_mirror_stack_frame(cpu, PSYQ_FRAME_RESETGRAPH);
}

void psyq_SetDispMask(CPUState* cpu) {
    SetDispMask((int)cpu->a0);
    /* Real SetDispMask @0x8005F6C8: sp -= 32; sw s0,16; sw s1,20; sw ra,24. */
    psyq_mirror_stack_frame(cpu, PSYQ_FRAME_SETDISPMASK);
}

void psyq_SetGraphDebug(CPUState* cpu) {
    cpu->v0 = (uint32_t)SetGraphDebug((int)cpu->a0);
    /* Real SetGraphDebug @0x8005F53C: sp -= 24; sw s0,16; sw ra,20. */
    psyq_mirror_stack_frame(cpu, PSYQ_FRAME_SETGRAPHDEBUG);
}

void psyq_DrawSync(CPUState* cpu) {
    cpu->v0 = (uint32_t)DrawSync((int)cpu->a0);
    /* Real DrawSync @0x8005F764: sp -= 24; sw s0,16; sw ra,20. */
    psyq_mirror_stack_frame(cpu, PSYQ_FRAME_DRAWSYNC);
}

void psyq_ClearImage(CPUState* cpu) {
    cpu->v0 = (uint32_t)ClearImage((RECT16*)PTR(cpu->a0),
                                   (u_char)cpu->a1,
                                   (u_char)cpu->a2,
                                   (u_char)cpu->a3);
    /* Real ClearImage @0x8005F8F8: sp -= 40; sw s0,16; s1,20; s2,24; s3,28; ra,32. */
    psyq_mirror_stack_frame(cpu, PSYQ_FRAME_CLEARIMAGE);
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

/* The native renderer (--renderer vk) draws the scanned-out framebuffer; feed it
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
    psyq_mirror_stack_frame(cpu, PSYQ_FRAME_PUTDISPENV);
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
    psyq_mirror_stack_frame(cpu, PSYQ_FRAME_PUTDRAWENV);
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
    psyq_mirror_stack_frame(cpu, PSYQ_FRAME_SETDEFDRAWENV);
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

/* Accumulating trace guard for the game-fiber thunks. The C header's
 * OP_PROF_ACCUM_SCOPE relies on the cleanup attribute, which is C-only; this is
 * the C++ equivalent. Inert (one load) unless a trace is open. */
namespace {
struct op_accum_scope {
    int id; uint64_t t0;
    explicit op_accum_scope(const char* name) : id(-1), t0(0) {
        if (op_prof_on()) { id = op_prof_intern(name); t0 = op_prof_now_us(); }
    }
    ~op_accum_scope() { if (id >= 0) op_prof_accum(id, op_prof_now_us() - t0); }
};
}

void psyq_DrawOTag(CPUState* cpu) {
    /* The OT rebuild is openpete's own work on the game fiber, so it lands
     * inside the sim-tick bracket rather than any render-side phase. */
    op_accum_scope prof_("psyq.drawotag");
    saved_ot_t* B = g_ot_cur;
    uint8_t*  g_ot_scratch      = B->scratch;
    size_t&   g_ot_scratch_used = B->scratch_used;
    uint8_t** g_ot_slot_host    = B->slots;
    int&      g_ot_slot_count   = B->slot_count;
    g_ot_scratch_used = 0;
    g_ot_slot_count = 0;

    /* Same-moby painter rank source: record this walk's geometry prims (walk
     * position = og paint order) as the FALLBACK rank table — the feeder's
     * primary source walks the g_WorldOT buckets directly, pose-exact. */
    psyq_game_ot_begin();

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
    /* Drawing primitives (codes 0x20-0x7F: polygons, lines, sprites) emitted
     * by THIS walk. Attribute commands (0xE0) and the terminator do not
     * count. A game that draws its loading screen emits exactly one -- the
     * background fill -- so this is what separates "rendering" from "still
     * loading" for the fast-boot anchor below. */
    unsigned draw_prims_this_call = 0;

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
        if (psyq_game_render_paths_active()) {
            uint8_t cb = (plen != 0u) ? host[7] : 0u;
            uint8_t hi = cb & 0xF0u;
            bool is_geom = (plen != 0u) && (hi >= 0x20u && hi <= 0x70u);
            if (is_geom && !psyq_game_render_path_addr_visible(addr)) {
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
                if (code_byte >= 0x20u && code_byte < 0x80u)
                    draw_prims_this_call++;
                { uint8_t hi = code_byte & 0xF0u;
                  if (hi == 0x20u || hi == 0x30u)
                      psyq_game_ot_prim_full(code_byte, host + 4,
                                                (unsigned)payload_bytes); }
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
    psyq_game_ot_end();

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

    /* The transition from "boot init" to "game is rendering" is the first OT
     * that draws a scene, not the first DrawOTag call. A game whose loading
     * screen is a single background fill issues DrawOTag every frame while it
     * reads from the disc; anchoring on that runs the whole load at wall-clock
     * pace behind a blank screen. Requiring more than one drawing primitive
     * holds fast boot until real content, and is inert for a game whose first
     * OT is already a scene. */
    if (openpete_fast_boot_active && draw_prims_this_call > 1u) {
        openpete_fast_boot_active = 0;
        OPL_INFO_F("boot",
                   "fast-boot ended at vbl=%d (first OT with %u drawing "
                   "primitives); openpete_frame anchored to 0",
                   SDL_GetAtomicInt(&openpete_vbl), draw_prims_this_call);
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

    /* The rest of what real PadInitDirect leaves in PSX RAM -- libpad's
     * pad-state dispatch table and its per-slot bookkeeping -- is PSYQ's own
     * data at this game's addresses, and the table entries are pointers into
     * PSYQ code, so both are specific to a game and a PSYQ revision. PsyCross
     * tracks pad state host-side and writes none of it. */
    psyq_game_mirror_pad_init(cpu, buf0, buf1);
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
    psyq_game_mirror_pad_slot2(cpu);
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

void psyq_PadInfoAct(CPUState* cpu) {
    cpu->v0 = (uint32_t)PadInfoAct((int)cpu->a0, (int)cpu->a1, (int)cpu->a2);
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
    /* Anchor the VIRTUAL streaming head first — guest-deterministic and
     * snapshotted, so it runs during speculation too (the game's ReadS
     * command is guest code; only the host-audio half below is
     * speculation-suppressed). The GetlocP override in psyq_CdSync
     * synthesizes the position the game reads from this anchor. */
    g_cdXaStartLBA = sector;
    g_cdXaStartVBL = SDL_GetAtomicInt(&openpete_vbl);

    const int track = spyro_current_music_track();

    /* Custom-level song ([[level]] music): resolved against the guest's
     * level id at the moment it starts the stream. Prefer g_NextLevelId
     * (same rule as the WAD overlay's latch): the level's first start
     * fires inside LoadLevelScene, where the slot's donor alias has
     * g_LevelId temporarily swapped to the DONOR — g_NextLevelId still
     * names the slot. Loop restarts fire outside the alias, where both
     * agree. */
    int32_t level_id = 0;
    memcpy(&level_id, openpete_psx_get_ram() + 0x758B4, sizeof(level_id));
    if (level_id < 10 || level_id > 65)
        memcpy(&level_id, openpete_psx_get_ram() + 0x7596C,
               sizeof(level_id));
    double mdur = 0.0;
    const char* mfile =
        openpete_mods_custom_music_for((int)level_id, &mdur);

    /* Guest-deterministic half (snapshotted; runs during speculation):
     * keep the guest's loop bookkeeping sane for a custom song. The
     * track keeps the DONOR's number (inherited at slot claim), but its
     * m_MusicTable end is stretched to the WAV's length so the guest
     * re-issues its loop ReadS when the song actually ends — 150
     * group-sectors per second (37.8 kHz stereo XA = 18.75 audio
     * sectors/s under the 8-sector interleave). Restored to stock the
     * next time the track starts without a custom song. */
    if (track >= 0 && track < 48) {
        uint8_t* ram = openpete_psx_get_ram();
        const uint32_t ent = 0x774B0u + 8u + 8u * (uint32_t)track;
        int32_t tstart = 0, tend = 0;
        memcpy(&tstart, ram + ent, 4);
        memcpy(&tend, ram + ent + 4u, 4);
        static int32_t s_stock_end[48];
        static uint8_t s_stock_known[48];
        if (!s_stock_known[track]) {
            s_stock_end[track] = tend;
            s_stock_known[track] = 1;
        }
        if (mfile && mdur > 0.0) {
            int32_t want = tstart + (int32_t)(mdur * 150.0 + 0.5);
            if (tend != want) memcpy(ram + ent + 4u, &want, 4);
        } else if (s_stock_known[track] && tend != s_stock_end[track]) {
            memcpy(ram + ent + 4u, &s_stock_end[track], 4);
        }
        if (mfile) {
            /* Pin the easter-egg swap away: the game's easter table has
             * no row for the free slot's index 35 (an OOB read into the
             * XA offset table), and a custom level shouldn't swap to
             * retail songs mid-play regardless. */
            const int32_t never = 0x7FFFFFFF;
            memcpy(ram + 0x774B0u + 540u, &never, 4);
        }
    }

    /* Runahead lookahead: starting host audio (WAV override or XA) from a
     * speculative tick would be audible and then re-fired by the committed
     * re-run. Guest-side effects of the read roll back regardless. */
    if (openpete_speculative) return;
    /* One line per track start / loop (every couple of minutes), so a
     * music report can be read off an ordinary log: the gap between two
     * of these is the loop period the guest measured. */
    OPL_INFO_F("music", "stream start: track %d LBA %d chan %d vbl %d",
               track, sector, s_xa_filter_chan, g_cdXaStartVBL);
    if (mfile)
        OPL_INFO_F("music", "custom-level song for level %d: %s (%.1fs "
                   "loop)", (int)level_id, mfile, mdur);
    if (mfile && music_override_try_start_path(mfile, sector)) {
        if (PsyX_XA_IsActive()) PsyX_XA_Stop();
        return;
    }
    if (music_override_try_start(track, sector)) {
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
        psyq_game_cd_setloc(param);
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
        if (!openpete_speculative) {           /* see start_music_stream */
            if (PsyX_XA_IsActive()) PsyX_XA_Stop();
            music_override_stop();
        }
    }
    return CdControl(com, param, result);
}

void psyq_CdControl(CPUState* cpu) {
    u_char com = (u_char)cpu->a0;
    u_char* param = (u_char*)PTR_OPT(cpu->a1);
    u_char* result = (u_char*)PTR_OPT(cpu->a2);
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
    u_char* param = (u_char*)PTR_OPT(cpu->a1);

    /* XA streaming arming. Spyro's SetMusicState issues exactly:
     *   CdControlB(CdlSetmode,   {SF|RT|Speed})
     *   CdControlB(CdlSetfilter, {file=1, chan=track%8})
     *   CdControlB(CdlReadS,     &startLoc)
     * which is the canonical "start XA streaming" sequence. */
    switch (com) {
        case CdlSetloc:
            if (param)
                psyq_game_cd_setloc(param);
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
            if (!openpete_speculative) {       /* see start_music_stream */
                if (PsyX_XA_IsActive()) PsyX_XA_Stop();
                music_override_stop();
            }
            break;
        default: break;
    }

    cpu->v0 = (uint32_t)CdControlB(com, param, (u_char*)PTR_OPT(cpu->a2));
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
    cpu->v0 = (uint32_t)CdControlF(com, (u_char*)PTR_OPT(cpu->a1));
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
    OPL_INFO_F("thunks", "CdRead sectors=%d buf=0x%08X mode=0x%02X ra=0x%08X",
               sectors, cpu->a1, mode, cpu->ra);

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
    psyq_game_cd_apply_buf((uint8_t*)buf, sectors);
    uint8_t* ram = openpete_psx_get_ram();
    if (g_native_cd_read_cb != nullptr) {
        /* Native registrar (post-rewriter init.c). Fire the host cb
         * directly — no cpu dispatch round-trip needed. */
        g_native_cd_read_cb(/*intr=CdlComplete=*/2u, /*result=*/nullptr);
    } else {
        uint32_t cb = g_cd_read_cb;
        if (cb == 0u) {
            /* libcd is translated in this game's image: CdReadCallback stored
             * the handler in guest RAM instead of reaching the thunk. */
            uint32_t slot = psyq_game_cd_read_cb_addr();
            if (slot != 0u) cb = cpu->read_word(slot);
        }
        invoke_psx_callback(cpu, cb, /*intr=CdlComplete=*/2u, 0u);
    }
    psyq_game_cd_read_done(cpu, g_cd_read_cb);

    /* The transfer is complete; on hardware that is a channel-3 DMA interrupt,
     * and libcd's read path does its bookkeeping in that handler. */
    psyq_dma_complete(cpu, 3u);

    /* Once the bytes have settled, the first word at the overlay vaddr is the
     * overlay id, so re-point the dispatcher at whichever overlay is now
     * resident. A write to that vaddr that is not an overlay carries a
     * different first word and leaves the tracker unchanged. */
    uint32_t ovl = psyq_game_overlay_vaddr();
    if (ovl != 0u && cpu->a1 == ovl) {
        uint32_t magic = 0;
        memcpy(&magic, ram + (ovl & 0x1FFFFFu), 4);
        spyro_set_active_overlay_by_magic(magic);
    }
}

/* libcd's internals, as distinct from the API above.
 *
 * PSYQ reaches the drive through the BIOS jump tables with an indirect call,
 * so these cannot be translated: the recompiler has no BIOS, and the symptom
 * is a status poll that never completes rather than a crash. They map onto
 * PsyCross's equivalents, which complete synchronously.
 *
 * The names come from the SDK's own export tables, recovered by signature
 * match; a decomp that splats the PSYQ text without symbols has none of them.
 */
void psyq_CD_sync(CPUState* cpu) {
    u_char scratch[8];
    u_char* result = (cpu->a1 != 0u) ? (u_char*)PTR(cpu->a1) : scratch;
    cpu->v0 = (uint32_t)CdSync((int)cpu->a0, result);
}

void psyq_CD_datasync(CPUState* cpu) {
    u_char scratch[8];
    u_char* result = (cpu->a1 != 0u) ? (u_char*)PTR(cpu->a1) : scratch;
    cpu->v0 = (uint32_t)CdReadSync((int)cpu->a0, result);
}

void psyq_CD_getsector(CPUState* cpu) {
    cpu->v0 = (uint32_t)CdGetSector(PTR(cpu->a0), (int)cpu->a1);
}

void psyq_CD_init(CPUState* cpu) {
    cpu->v0 = (uint32_t)CdInit();
}

/* No drive-side queue to drain or volume register to program: PsyCross owns
 * the device and applies both host-side. */
void psyq_CD_flush(CPUState* cpu)   { cpu->v0 = 0u; }
void psyq_CD_initvol(CPUState* cpu) { cpu->v0 = 0u; }

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

    /* Override PsyCross's GetlocP fake-position with the VIRTUAL music
     * head so Spyro's track-end detection (compares position to
     * m_MusicTable.end) fires at hardware rate. result[5..7] = absolute
     * MSF.
     *
     * NOT the host decoder's real playhead: that advances on the AUDIO
     * clock (up to kPumpBatch=8 sectors per pump — ~3x faster than a
     * real drive — and frozen during runahead speculation, where wall
     * time doesn't pass). Feeding it to the guest makes m_TrackPosition
     * a stealth host-state read: track ends fire early vs PSX, and
     * runahead's led/committed predictions diverge the moment music
     * starts.
     *
     * Rate = 150 sectors per wall-second of GUEST time, i.e. DOUBLE
     * speed, because that is how the music is streamed: SetMusicState
     * issues CdlModeSF|CdlModeRT|CdlModeSpeed with chan = track % 8,
     * the textbook 8-channel 37.8 kHz-stereo XA layout. Each matching
     * sector carries 53.3 ms of audio and only 1 sector in 8 matches
     * the filter, so real-time playback burns 150 LBA/s and a real
     * drive's head — which is what CdlGetlocP reports — covers the
     * same 150. At single speed the guest sees a track as twice its
     * true length: the audio runs out mid-track and the decoder walks
     * on into the next XA file's stream on the same channel (which is
     * an unrelated level's theme) until the late loop finally fires.
     * 150/59.94 = 1001/400 per VBL — deterministic and snapshotted. */
    if (g_cdXaStartLBA > 0 && g_CD_com == CdlGetlocP && mode == 1) {
        int32_t vd = SDL_GetAtomicInt(&openpete_vbl) - g_cdXaStartVBL;
        if (vd < 0) vd = 0;
        int cur = g_cdXaStartLBA + (int)(((int64_t)vd * 1001) / 400);
        CdlLOC abs;
        CdIntToPos(cur, &abs);
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
#include "sfx_pack.h"      /* drop-in WAV replacement (core/host/sfx_pack.c) */
extern "C" void PsyX_SPUAL_StopOrphans(void);

/* ── SFX overlay: Spyro-1 glue ─────────────────────────────────────────
 * Names for the game's SoundTable slots (spyro-1/include/sound_table.h;
 * slot value = index into the per-level SoundDefinition array). The
 * resolver maps a keyed voice's sample addr back to the first named
 * slot whose definition owns that addr; the uploader stages replacement
 * ADPCM into the free SPU tail after the level bank. Everything here is
 * host-side: guest RAM and the SPU bank image stay byte-stock.
 */
static uint32_t s_spu_xfer_addr; /* SpuSetTransferStartAddr, advanced by SpuWrite */

void psyq_SpuInit(CPUState* cpu) {
    (void)cpu;
    SpuInit();
    psyq_game_sfx_arm();
}

/* Music level is the product of the two hardware stages the XA stream passes
 * through: its own CD input volume, then the SPU main volume applied to the
 * summed mix. Both arrive via separate SpuSetCommonAttr masks, so they are
 * tracked apart and recombined whenever either moves. */
static float s_cdVolume      = 1.0f;
static float s_spuMainVolume = 1.0f;

static void spyro_apply_music_gain(void) {
    const float g = s_cdVolume * s_spuMainVolume;
    PsyX_XA_SetVolume(g);
    music_override_set_gain(g);
}

void psyq_SpuSetCommonAttr(CPUState* cpu) {
    SpuCommonAttr* attr = (SpuCommonAttr*)PTR(cpu->a0);
    SpuSetCommonAttr(attr);
    /* Spyro's music fader writes mask=SPU_COMMON_CDVOLL|SPU_COMMON_CDVOLR
     * with values in 0..0x3FFF. Route that to the XA streaming source's
     * gain (avg the two channels for a single mono gain). The WAV
     * override gets the same value so the in-game music slider and
     * track-transition fades cover both paths. */
    /* SPU volumes are 15-bit: hardware mixes as (sample * volume) >> 15, so
     * full scale is 32768. Main volume applies to the summed mix, and CD/XA
     * input is summed *before* it, so the music gain carries both stages. */
    if (attr && (attr->mask & (SPU_COMMON_MVOLL | SPU_COMMON_MVOLR))) {
        int l = attr->mvol.left, r = attr->mvol.right;
        if (l < 0) l = 0; if (r < 0) r = 0;
        /* Main volume lands in a sweep-capable register whose fixed mode
         * carries the level halved; the SPU doubles it on application, so
         * 0x3CCC is applied as 31128 and full scale here is 16384. CD input
         * volume below has no sweep mode and is applied as written, making
         * it full scale at 32768 — the two are not the same scale. */
        s_spuMainVolume = (float)((l + r) / 2) / 16384.0f;
        PsyX_SPUAL_SetMasterVolume(s_spuMainVolume);
        spyro_apply_music_gain();
    }
    if (attr && (attr->mask & (SPU_COMMON_CDVOLL | SPU_COMMON_CDVOLR))) {
        int l = attr->cd.volume.left;
        int r = attr->cd.volume.right;
        if (l < 0) l = 0; if (r < 0) r = 0;
        s_cdVolume = (float)((l + r) / 2) / 32768.0f;
        spyro_apply_music_gain();
    }
}

/* OPENPETE_SFX_END_LOG=1 (see psyq_SpuGetKeyStatus): the key-on side —
 * which sample (and slot name, when the level table knows it) each
 * voice was armed with. */
static int sfx_key_log_on(void) {
    static int s_log = -1;
    if (s_log < 0) { const char* e = getenv("OPENPETE_SFX_END_LOG"); s_log = (e && *e && *e != '0'); }
    return s_log;
}
static void sfx_key_log_attr(const SpuVoiceAttr* attr) {
    if (!sfx_key_log_on() || !(attr->mask & SPU_VOICE_WDSA)) return;
    const char* names[1];
    int n = psyq_game_sfx_slot_names(attr->addr, names, 1);
    for (int v = 0; v < 24; v++)
        if (attr->voice & (1u << v))
            fprintf(stderr, "[sfx-attr f=%u v=%d addr=%#x pitch=%#x name=%s]\n",
                    openpete_frame, v, attr->addr, attr->pitch, n ? names[0] : "-");
}

/* Voice attributes pass through as written. Sound replacement (the sfx
 * pack, including the mod API's sfx_replace) happens below this mirror,
 * at the voice's PCM upload inside PsyCross, so the address and pitch
 * the one-shot duration is derived from — and with them the tick on
 * which the guest sees the voice end — are always the game's own. */
void psyq_SpuSetVoiceAttr(CPUState* cpu) {
    const SpuVoiceAttr* attr = (const SpuVoiceAttr*)PTR(cpu->a0);
    sfx_key_log_attr(attr);
    SpuSetVoiceAttr((SpuVoiceAttr*)attr);
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

/* OPENPETE_SFX_END_LOG=1: one line per voice key-status transition the
 * guest observes (ON / RESET with the VBL it was read on). The RESET
 * read is the tick the game frees the sound slot and writes the
 * sound-ref byte, so two runs whose logs match played every voice to the
 * same guest-visible end — the A/B behind the sfx_replace rung of
 * check_mods.sh. */
void psyq_SpuGetKeyStatus(CPUState* cpu) {
    unsigned int mask = (unsigned int)cpu->a0;
    cpu->v0 = (uint32_t)SpuGetKeyStatus(mask);
    if (sfx_key_log_on() && mask && !(mask & (mask - 1u))) {
        static uint8_t s_last[24];
        int v = __builtin_ctz(mask);
        if (v < 24 && s_last[v] != (uint8_t)cpu->v0) {
            s_last[v] = (uint8_t)cpu->v0;
            if (cpu->v0 == SPU_ON || cpu->v0 == SPU_RESET)
                fprintf(stderr, "[sfx-key f=%u v=%d %s]\n", openpete_frame, v,
                        cpu->v0 == SPU_ON ? "on" : "reset");
        }
    }
}

void psyq_SpuGetAllKeysStatus(CPUState* cpu) {
    SpuGetAllKeysStatus((char*)PTR(cpu->a0));
}

void psyq_SpuRead(CPUState* cpu) {
    cpu->v0 = SpuRead((unsigned char*)PTR(cpu->a0), (unsigned int)cpu->a1);
}

void psyq_SpuWrite(CPUState* cpu) {
    /* The level-load bank blast overwrites all of SPU RAM including any
     * staged SFX replacements — invalidate them (re-staged lazily). The
     * sfx pack's identity cache follows the same two-tier rule: a blast
     * is a new generation, any other write (dragon speech lands over the
     * top of the bank mid-level) forgets just the addresses it covers. */
    psyq_game_sfx_arm();
    unsigned int len = (unsigned int)cpu->a1;
    if (len != 0u && len == psyq_game_spu_bank_blast_len()) {
        sfx_pack_generation_bump();
        PsyX_SPUAL_StopOrphans(); /* a new level: no tails from the old one */
    } else {
        sfx_pack_invalidate_range(s_spu_xfer_addr, len);
    }
    cpu->v0 = SpuWrite((unsigned char*)PTR(cpu->a0), len);
    s_spu_xfer_addr += len;
}

void psyq_SpuSetTransferMode(CPUState* cpu) {
    cpu->v0 = (uint32_t)SpuSetTransferMode((int)cpu->a0);
}

void psyq_SpuSetTransferStartAddr(CPUState* cpu) {
    s_spu_xfer_addr = (unsigned int)cpu->a0;
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

    psyq_game_mirror_memcard_init(cpu);

    /* MemCardInit's MIPS prologue: sp -= 0x18; sw ra,0x10(sp). */
    psyq_mirror_stack_frame(cpu, PSYQ_FRAME_MEMCARDINIT);
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
    /* One live caller: CrashDemoLaunch (title-screen L1+Triangle boots the
     * disc's S0\CRASH.EXE demo). features/crash_demo_woah.c intercepts that
     * path before its engine teardown, so this should never fire; keep a
     * warning no-op rather than abort in case another route appears. */
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

static inline uint32_t psyq_seed_load(void) {
    const uint8_t* ram = openpete_psx_get_ram();
    uint32_t off = psyq_game_rand_seed_addr() & 0x1FFFFFu;
    return  (uint32_t)ram[off]
         | ((uint32_t)ram[off + 1] << 8)
         | ((uint32_t)ram[off + 2] << 16)
         | ((uint32_t)ram[off + 3] << 24);
}
static inline void psyq_seed_store(uint32_t v) {
    uint8_t* ram = openpete_psx_get_ram();
    uint32_t off = psyq_game_rand_seed_addr() & 0x1FFFFFu;
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
