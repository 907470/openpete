/* games/spyro-1/decomp/openpete_cd_thunks.c — Phase 4 outbound thunks
 * for the CD subsystem.
 *
 * Bridges native C calls into the PSYQ thunk layer (psyq_thunks.cpp)
 * and the recompiled MIPS dispatcher. Two paths:
 *
 *   1. PSYQ functions (Cd*) — go through psyq_*(CPUState*) so the
 *      Spyro-specific enhancements that live in the existing thunks
 *      (XA streaming arm/start/stop in psyq_CdControl, XA pump in
 *      psyq_CdSync, etc.) keep firing. Calling PsyCross's raw
 *      `CdControl`/`CdSync` from native code would silently bypass
 *      those — that's the whole reason this layer exists.
 *
 *   2. Recompiled MIPS callees (SoundsUpdate, CDMusicUpdate) — go
 *      through psx_dispatch_compiled(cpu, vaddr) to run the still-
 *      recompiled body. Cpu state is saved/restored around each call
 *      so the caller's a0..a3/v0/v1/ra survive the trip.
 *
 * cd.c sees these via header-shim renames (`-DCdControl=openpete_CdControl`)
 * and via plain extern declarations for the spyro-internal callees. The
 * PSYQ symbols (real `CdControl`/`CdSync`/...) remain defined by
 * PsyCross — psyq_thunks.cpp still calls them — but cd.c never sees
 * those names directly. */

#include "openpete_psx.h"
#include "openpete_log.h"

#include <psx/libcd.h>  /* CdlLOC, CdlFILTER prototypes — used by callers */

#include <stdint.h>

extern "C" {
    /* Stashed by spyro_native_set_cpu when the dispatch override fires
     * for a native entry point (see decomp/native_thunks.c). Single-fiber
     * model assumed; lift to TLS if a second native entry lands. */
    CPUState* spyro_native_get_cpu(void);

    /* PSYQ thunks (psyq_thunks.cpp) — the wrappers we want to keep using. */
    void psyq_CdControl(CPUState* cpu);
    void psyq_CdRead(CPUState* cpu);
    void psyq_CdSync(CPUState* cpu);
    void psyq_CdIntToPos(CPUState* cpu);

    /* Dispatcher into recompiled MIPS bodies. */
    int psx_dispatch_compiled(CPUState* cpu, uint32_t addr);
}

/* --- Helpers ------------------------------------------------------------- */

/* Translate a host pointer back into a PSX vaddr the recompiled side can
 * read. Pointers passed in from native cd.c always either (a) sit inside
 * g_ram (because they're `&g_GlobalStruct.field` of a link-aliased global)
 * or (b) are NULL. Anything else is a programming error: PSX-side code
 * can only see PSX RAM. */
static uint32_t host_to_psx(const void* p) {
    if (!p) return 0u;
    uint8_t* base = openpete_psx_get_ram();
    intptr_t off = (const uint8_t*)p - base;
    if (off < 0 || off >= 2*1024*1024) return 0u;  /* not in RAM — best we can do is NULL */
    return 0x80000000u | (uint32_t)off;
}

/* Save / restore the eight PSYQ-ABI scratch slots around an outbound
 * call. The recompiled callee owns s0..s7/fp/gp; we don't touch those. */
typedef struct {
    uint32_t a0, a1, a2, a3, v0, v1, ra;
} cpu_scratch_t;

static void cpu_save(CPUState* cpu, cpu_scratch_t* s) {
    s->a0 = cpu->a0; s->a1 = cpu->a1; s->a2 = cpu->a2; s->a3 = cpu->a3;
    s->v0 = cpu->v0; s->v1 = cpu->v1; s->ra = cpu->ra;
}
static void cpu_restore(CPUState* cpu, const cpu_scratch_t* s) {
    cpu->a0 = s->a0; cpu->a1 = s->a1; cpu->a2 = s->a2; cpu->a3 = s->a3;
    cpu->v0 = s->v0; cpu->v1 = s->v1; cpu->ra = s->ra;
}

/* --- libcd outbound thunks ---------------------------------------------- */

extern "C" int openpete_CdControl(u_char com, u_char* param, u_char* result) {
    /* One-shot proof of life: fires the first time native cd.c routes a
     * CdControl through the outbound thunk. Subsequent calls are silent
     * — the existing psyq_CdControl trace already logs every Cd command. */
    static int s_logged = 0;
    if (!s_logged) {
        s_logged = 1;
        OPL_INFO_F("decomp", "openpete_CdControl: native cd.c → outbound thunk live (com=0x%02X)",
                   (unsigned)com);
    }
    CPUState* cpu = spyro_native_get_cpu();
    cpu_scratch_t s; cpu_save(cpu, &s);
    cpu->a0 = (uint32_t)com;
    cpu->a1 = host_to_psx(param);
    cpu->a2 = host_to_psx(result);
    cpu->ra = 0xDEADBEEFu;
    psyq_CdControl(cpu);
    int rc = (int)cpu->v0;
    cpu_restore(cpu, &s);
    return rc;
}

extern "C" int openpete_CdRead(int sectorCount, u_long* buf, int mode) {
    CPUState* cpu = spyro_native_get_cpu();
    cpu_scratch_t s; cpu_save(cpu, &s);
    cpu->a0 = (uint32_t)sectorCount;
    cpu->a1 = host_to_psx(buf);
    cpu->a2 = (uint32_t)mode;
    cpu->ra = 0xDEADBEEFu;
    psyq_CdRead(cpu);
    int rc = (int)cpu->v0;
    cpu_restore(cpu, &s);
    return rc;
}

extern "C" int openpete_CdSync(int mode, u_char* result) {
    CPUState* cpu = spyro_native_get_cpu();
    cpu_scratch_t s; cpu_save(cpu, &s);
    cpu->a0 = (uint32_t)mode;
    cpu->a1 = host_to_psx(result);
    cpu->ra = 0xDEADBEEFu;
    psyq_CdSync(cpu);
    int rc = (int)cpu->v0;
    cpu_restore(cpu, &s);
    return rc;
}

extern "C" CdlLOC* openpete_CdIntToPos(int i, CdlLOC* p) {
    CPUState* cpu = spyro_native_get_cpu();
    cpu_scratch_t s; cpu_save(cpu, &s);
    cpu->a0 = (uint32_t)i;
    cpu->a1 = host_to_psx(p);
    cpu->ra = 0xDEADBEEFu;
    psyq_CdIntToPos(cpu);
    cpu_restore(cpu, &s);
    return p;
}

/* --- Spyro-internal recompiled callees ---------------------------------- */

/* Addresses harvested from PSX.EXE_full.c by tracing CDLoadSync's jal
 * sequence against spu.h/music.h prototypes:
 *   func_8005637C → SoundsUpdate
 *   func_8002BBE0 → CDMusicUpdate */

extern "C" void SoundsUpdate(void) {
    CPUState* cpu = spyro_native_get_cpu();
    cpu_scratch_t s; cpu_save(cpu, &s);
    cpu->ra = 0xDEADBEEFu;
    psx_dispatch_compiled(cpu, 0x8005637Cu);
    cpu_restore(cpu, &s);
}

extern "C" void CDMusicUpdate(void) {
    CPUState* cpu = spyro_native_get_cpu();
    cpu_scratch_t s; cpu_save(cpu, &s);
    cpu->ra = 0xDEADBEEFu;
    psx_dispatch_compiled(cpu, 0x8002BBE0u);
    cpu_restore(cpu, &s);
}
