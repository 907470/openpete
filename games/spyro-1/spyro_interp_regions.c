/* spyro_interp_regions.c — Spyro 1's render-side interpolation region
 * table. Captured per-tick into the openpete_interp ring so render
 * frames at >29.97 Hz can lerp transforms and re-emit DrawOTag.
 *
 * Registration model:
 *   This TU defines spyro_interp_register() which forwards the static
 *   table to openpete_interp_register_regions(). main.cpp calls it
 *   once at startup before openpete_run(). We avoid the weak-symbol
 *   override pattern used by spyro_state_regions.c because MinGW's
 *   PE/COFF linker has been observed to mis-resolve it, leaving the
 *   weak NULL in place at runtime and crashing the recomp boot path.
 *   Explicit registration works identically across toolchains.
 *
 * Scope (2026-05-14 architecture revision — see
 * notes/plans/openpete-renderer-foundation.md and
 * notes/plans/openpete-fps-unlock.md):
 *   - g_Camera ONLY. Captured for use by the renderer-foundation
 *     Phase 2g stretch (depth-aware async reprojection), which wants
 *     a single authoritative camera per tick rather than per-prim
 *     composite matrices.
 *   - g_Spyro / mobies / animation are NOT snapshotted here. Their
 *     lerp story lives in renderer-foundation Phases 0+1+2: the
 *     matcher reads per-vertex world-space + composite matrices
 *     directly from psxrecomp's gte_execute, pairs prims across
 *     frames by (texpage+clut+bbox+velocity), and decompose-lerp-
 *     recomposes the composite transform. That path doesn't read
 *     g_Spyro / g_LevelMobys from PSX RAM at all, so snapshotting
 *     them here is dead weight.
 *   - Particles are deferred indefinitely (no host-readable list;
 *     handled by the matcher if they're identifiable, otherwise they
 *     pop at 29.97 Hz).
 *
 * Address verification: 2026-05-14 nm of games/spyro-1/spyro-1/build/psx.elf:
 *   80076dd0 B g_Camera                   (next = 0x80076ee0 → 0x110 bytes)
 *
 * Lengths match manifest/gameplay_state_regions.toml so the same
 * boundaries the determinism gate trusts are the boundaries we
 * snapshot.
 */

#include "openpete_interp.h"
#include "openpete_interp_objs.h"
#include "openpete_psx.h"          /* openpete_psx_to_host_ptr */
#include "spyro_interp_regions.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* DIRECT singleton regions — captured whole into the interp ring each tick.
 *   g_Camera (0x110): the smooth-follow camera (re-run feeder).
 *   g_Spyro  (0x2A4): the player avatar — position/rotation/anim for the
 *                     lerp + animation-clock feeders. Singleton, so it lives
 *                     here rather than in the per-object (indirect) table. */
static const openpete_interp_region_t s_spyro_interp_regions[] = {
    { "g_Camera", 0x80076DD0, 0x110 },
    { "g_Spyro",  0x80078A58, 0x2A4 },
    /* g_SpyroFlame (0xD0 captured): the flame-breath struct. The ribbon is
     * rigidly attached to Spyro's mouth/head via m_Position (+0x00) and the
     * flame-attach matrix (+0xB8), both updated at the 30 Hz tick — captured so
     * the sub-tick spine can lerp the flame POSE to track the (also-interpolated)
     * Spyro instead of snapping each tick. Read-only; rendered only while
     * breathing fire. */
    { "g_SpyroFlame", 0x800786C8, 0xD0 },
};

/* --- per-object (indirect) capture: the g_LevelMobys array --------------- */

#define G_LEVELMOBYS   0x80075828u   /* Moby* g_LevelMobys → head of the array */
#define MOBY_STRIDE    0x58u         /* stride between live moby records */
#define MOBY_STATE_OFF 0x48u         /* +0x48: 0xFF = array sentinel, >=0x80 dead */
#define G_HUD_ICONS    0x80077FECu   /* g_Hud.m_Mobys[12] — static 3D HUD icons (stride 0x58) */
#define G_HUD_COUNT    12u           /* gem/dragon/Spyro-head/key/digit icons (they spin) */

extern unsigned openpete_frame;

static uint32_t rd32(uint32_t addr) {
    const void* p = openpete_psx_to_host_ptr(addr);
    uint32_t v = 0;
    if (p) memcpy(&v, p, 4);
    return v;
}

/* Walk g_LevelMobys, snapshotting each live moby's stride-0x58 record into
 * the keyed prev/curr table (key = the moby's vaddr — slot-stable across
 * ticks). Read-only; mirrors the moby feeder's own walk
 * (moby_native.c::append_revealed_regular). */
static void spyro_interp_capture_objs(void) {
    openpete_interp_objs_begin_tick();

    uint32_t arr = rd32(G_LEVELMOBYS);
    if (arr >= 0x80000000u) {
        uint32_t m = arr;
        for (int guard = 0; guard < 4096; ++guard) {
            const void* host = openpete_psx_to_host_ptr(m);
            if (!host) break;
            uint8_t st = ((const uint8_t*)host)[MOBY_STATE_OFF];
            if (st == 0xFF) break;                       /* array sentinel */
            if (st < 0x80)                               /* alive */
                openpete_interp_objs_capture(m, host, OPENPETE_INTERP_OBJ_BLOB_BYTES);
            m += MOBY_STRIDE;
        }
    }

    /* Static 3D HUD icons (g_Hud.m_Mobys[12]) — stride-0x58 Mobys OUTSIDE
     * g_LevelMobys. They spin (m_Rotation @ +0x44); capture them so the sub-tick
     * spine can lerp the spin (else it steps at 30 Hz while the rest is smooth).
     * Always present (no sentinel) — capture all 12; inactive ones don't render. */
    for (uint32_t i = 0; i < G_HUD_COUNT; ++i) {
        uint32_t m = G_HUD_ICONS + i * MOBY_STRIDE;
        const void* host = openpete_psx_to_host_ptr(m);
        if (host) openpete_interp_objs_capture(m, host, OPENPETE_INTERP_OBJ_BLOB_BYTES);
    }

    openpete_interp_objs_end_tick();

    static int dbg = -1;
    if (dbg < 0) { const char* e = getenv("OPENPETE_INTERP_OBJDBG"); dbg = (e && *e && *e != '0'); }
    if (dbg && (openpete_frame % 120u) == 0) {
        int tracked, paired, spawned, despawned;
        openpete_interp_objs_stats(&tracked, &paired, &spawned, &despawned);
        fprintf(stderr, "[interp-objs f=%u] tracked=%d paired=%d spawned=%d despawned=%d\n",
                openpete_frame, tracked, paired, spawned, despawned);
    }

    /* Numerical LOO gate (OPENPETE_INTERP_LOO) — Gate 2 of the high-FPS plan,
     * as an instrument not eyeballs. Hold 3 consecutive real ticks; reconstruct
     * the MIDDLE by lerping the outer two (the exact α=0.5 lerp the spine uses)
     * and compare to the real middle: err = |½(p[N-1]+p[N+1]) − p[N]| — the
     * chord-vs-curve interpolation error. Small vs the per-tick motion ⇒ the
     * lerp faithfully reconstructs canonical motion (= smooth). Read-only. */
    static int loo = -1;
    if (loo < 0) { const char* e = getenv("OPENPETE_INTERP_LOO"); loo = (e && *e && *e != '0'); }
    if (loo) {
        static int32_t ch[3][3], sh[3][3]; static int ln = 0;
        for (int s = 0; s < 2; s++) for (int k = 0; k < 3; k++) { ch[s][k]=ch[s+1][k]; sh[s][k]=sh[s+1][k]; }
        for (int k = 0; k < 3; k++) { ch[2][k]=(int32_t)rd32(0x80076DD0u+0x28u+(uint32_t)k*4u);
                                      sh[2][k]=(int32_t)rd32(0x80078A58u+0x00u+(uint32_t)k*4u); }
        if (ln < 3) ln++;
        if (ln >= 3) {
            double cd=0, sd=0, cm=0, sm=0;
            for (int k = 0; k < 3; k++) {
                double c=0.5*((double)ch[0][k]+ch[2][k])-ch[1][k]; cd+=c*c;
                double s=0.5*((double)sh[0][k]+sh[2][k])-sh[1][k]; sd+=s*s;
                double cmo=(double)ch[2][k]-ch[1][k]; cm+=cmo*cmo;
                double smo=(double)sh[2][k]-sh[1][k]; sm+=smo*smo;
            }
            cd=sqrt(cd); sd=sqrt(sd); cm=sqrt(cm); sm=sqrt(sm);
            static double cs=0, cmx=0, ss=0, smx=0, cms=0, sms=0; static long lc=0;
            if (cd < 5000.0 && sd < 5000.0) {   /* exclude cuts (snapped, not lerped) */
                cs+=cd; if(cd>cmx)cmx=cd; ss+=sd; if(sd>smx)smx=sd; cms+=cm; sms+=sm; lc++;
            }
            if (lc > 0 && (openpete_frame % 600u) == 0)
                fprintf(stderr, "[interp-loo f=%u] n=%ld | CAMERA lerp-err mean=%.2f max=%.1f vs motion/tick=%.1f | "
                        "SPYRO lerp-err mean=%.2f max=%.1f vs motion/tick=%.1f (world units)\n",
                        openpete_frame, lc, cs/lc, cmx, cms/lc, ss/lc, smx, sms/lc);
        }
    }
}

void spyro_interp_register(void) {
    openpete_interp_register_regions(
        s_spyro_interp_regions,
        sizeof(s_spyro_interp_regions) / sizeof(s_spyro_interp_regions[0]));
    openpete_interp_register_obj_capture(spyro_interp_capture_objs);
}
