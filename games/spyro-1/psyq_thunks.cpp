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
    /* libgcc constructor-runner. Spyro's `main()` is the only direct caller;
     * with no PSX-style global ctors to chain, this is a no-op. */
    (void)cpu;
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
    if (mode == 0) {
        if (openpete_host_fiber) {
            /* openpete_yield_to_host() bumps openpete_frame and switches to
             * the host fiber. Returns when host resumes us for the next
             * frame, so cpu->v0 below sees the post-yield frame counter. */
            openpete_yield_to_host();
        } else {
            /* No fiber loop running (smoke-test path) — fall back to PsyCross's
             * busy wait so semantics still match. */
            (void)VSync(0);
        }
        /* Fire registered VSync callback (PadVSync polls pad state). */
        if (g_vsync_cb) invoke_psx_callback(cpu, g_vsync_cb, 0, 0);
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
    cpu->v0 = (uint32_t)LoadImage((RECT16*)PTR(cpu->a0),
                                  (u_long*)PTR(cpu->a1));
}

void psyq_StoreImage(CPUState* cpu) {
    cpu->v0 = (uint32_t)StoreImage((RECT16*)PTR(cpu->a0),
                                   (u_long*)PTR(cpu->a1));
}

void psyq_MoveImage(CPUState* cpu) {
    cpu->v0 = (uint32_t)MoveImage((RECT16*)PTR(cpu->a0),
                                  (int)cpu->a1, (int)cpu->a2);
}

void psyq_PutDispEnv(CPUState* cpu) {
    PutDispEnv((DISPENV*)PTR(cpu->a0));
}

void psyq_PutDrawEnv(CPUState* cpu) {
    PutDrawEnv((DRAWENV*)PTR(cpu->a0));
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

        /* Header: addr filled in below; len = payload longs; pgxp_index = none. */
        uint16_t lenField  = (uint16_t)(payload_bytes / 4u);
        uint16_t pgxpField = 0xFFFFu;
        std::memcpy(out + 8,  &lenField,  2);
        std::memcpy(out + 10, &pgxpField, 2);
        if (payload_bytes != 0u) {
            std::memcpy(out + 12, host + 4, payload_bytes);
            if (plen != 0u) openpete_primitives_drawn++;
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

    /* Submit each converted slot as a single-primitive call.
     *
     * Why not PsyCross's DrawOTag? It walks the chain via
     * ParsePrimitivesLinkedList(p, 0), whose inner loop advances by
     * `(primLength + P_LEN) * 4` bytes per sub-primitive. With P_LEN=3
     * (extended pointers on x86_64) and PSX OT entries that don't always
     * pack cleanly into that step (e.g. a primitive whose ParsePrimitive
     * returns a hardcoded primLength different from what tagLength implies),
     * the loop walks past the slot's payload into uninitialised scratch and
     * occasionally hits a 0xA?-class byte that triggers LoadImage on garbage.
     *
     * Using DrawPrim path (singlePrimitive=1) bypasses that loop entirely:
     * each slot is one primitive, parsed once. DrawAllSplits flushes the
     * accumulated batches at end. */
    if (g_ot_slot_count > 0) {
        if (PsyX_BeginScene()) {
            ClearSplits();
        }
        for (int i = 0; i < g_ot_slot_count; i++) {
            uint8_t*  s = g_ot_slot_host[i];
            uint16_t  lf;
            std::memcpy(&lf, s + 8, 2);
            if (lf == 0u) continue;  /* OT anchor / terminator entry */
            DrawPrim(s);
        }
        DrawAllSplits();
    }
}

/* --- libgte -------------------------------------------------------------- */

void psyq_InitGeom(CPUState* cpu) {
    (void)cpu;
    InitGeom();
}

void psyq_SetGeomOffset(CPUState* cpu) {
    SetGeomOffset((int)cpu->a0, (int)cpu->a1);
}

void psyq_SetGeomScreen(CPUState* cpu) {
    SetGeomScreen((int)cpu->a0);
}

void psyq_SetTransMatrix(CPUState* cpu) {
    SetTransMatrix((MATRIX*)PTR(cpu->a0));
}

void psyq_MulMatrix0(CPUState* cpu) {
    MulMatrix0((MATRIX*)PTR(cpu->a0),
               (MATRIX*)PTR(cpu->a1),
               (MATRIX*)PTR(cpu->a2));
}

void psyq_MulMatrix(CPUState* cpu) {
    MulMatrix((MATRIX*)PTR(cpu->a0), (MATRIX*)PTR(cpu->a1));
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

void psyq_CdControlF(CPUState* cpu) {
    cpu->v0 = (uint32_t)CdControlF((u_char)cpu->a0,
                                   (u_char*)PTR(cpu->a1));
}

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
     * clears. */
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
}

void psyq_CdSync(CPUState* cpu) {
    int mode = (int)cpu->a0;
    int rc = CdSync(mode, (u_char*)PTR(cpu->a1));
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

void psyq_MemCardInit(CPUState* cpu) {
    MemCardInit((int)cpu->a0);
}

void psyq_MemCardStart(CPUState* cpu) {
    (void)cpu;
    MemCardStart();
}

void psyq_MemCardStop(CPUState* cpu) {
    (void)cpu;
    MemCardStop();
}

void psyq_MemCardExist(CPUState* cpu) {
    cpu->v0 = (uint32_t)MemCardExist((int)cpu->a0);
}

void psyq_MemCardAccept(CPUState* cpu) {
    cpu->v0 = (uint32_t)MemCardAccept((int)cpu->a0);
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
    cpu->v0 = (uint32_t)MemCardSync((int)cpu->a0,
                                    (int*)PTR(cpu->a1),
                                    (int*)PTR(cpu->a2));
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
