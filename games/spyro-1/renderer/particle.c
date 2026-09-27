// games/spyro-1/renderer/particle.c — the PARTICLE renderer family
// (func_800573C8 particles + func_800580F4 glows + func_800584C4 sparkles /
// r_particles.s) re-derived from gamestate in float + its numerical A/B
// harness. This is the LAST 3b actor and the most complex: 7 particle sprite
// classes (FT4 billboards, dots/tiles, F2 lines), additive glow rings, and
// twinkle sparkle quads — a mix of GPU primitive types over one polybuf.
//
// VERDICT-CRITICAL (see the format notes): the .s writes Particle.m_03
// (drawn stamp, read next frame -> conditional rand() -> RNG seed) and
// Sparkle m_Life. We NEVER reproduce those — the real .s runs headless exactly
// once per call (behaviour-identical to normal dispatch) and we only READ pool
// state + capture its emitted prims afterwards, so determinism holds by
// construction. The native path only mirrors the GEOMETRY in float.
//
// Three sub-renderers hooked, one render path each (RP_PARTICLES, RP_GLOW,
// RP_SPARKLE): the game calls them independently — a dragon-rescue cutscene
// renders particles every tick but never calls the glow or sparkle renderer,
// while their pools keep active records — so the sub-tick spine must gate each
// feeder on ITS renderer having drawn, not on the family. func_800573C8 is
// called ~9x/frame (one per occlusion/gamestate pass); glows+sparkles
// once/frame. Each call brackets the polybuf cursor and appends to the native
// scene.
//
// Gated by OPENPETE_PARTICLE_VERIFY (dump/A/B) or a native renderer mode
// (--renderer vk; scene feed); inert fall-through otherwise.

#include "feature_registry.h"
#include "renderer/reextract.h"  /* sub-tick re-extract entry */
#include "renderer/native_gte.h"    /* shared GTE float-mirror math */
#include "psx_runtime_internal.h"   /* full CPUState (gte_ctrl, read_word, a0..) */
#include "openpete_native_render.h"
#include "openpete_interp.h"       /* openpete_interp_present_now (sparkle phase) */
#include "openpete_interp_objs.h"  /* per-slot prev/curr snapshots (sparkle phase) */
#include "renderer/render_paths.h"  /* --render-paths isolation */
#include "renderer/hook_bracket.h"  /* the shared feeder hook bracket */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spyro1_symbols.gen.h"
#include "openpete_profile.h"   /* OP_PROF_ACCUM_SCOPE — fiber-hook cost split */

extern void     func_800573C8(CPUState* cpu);  /* RenderParticles  */
extern void     func_800580F4(CPUState* cpu);  /* RenderGlows      */
extern void     func_800584C4(CPUState* cpu);  /* RenderSparkles   */
extern void     func_80058864(CPUState* cpu);  /* RenderDragonStar (free-a-dragon flash) */
extern unsigned openpete_frame;

/* ---- gamestate addresses (psx.map / the format notes) --------------- */

/* ---- captured prims (ground truth) --------------------------------------- *
 * Particle FT4 quads (GPU code 0x2C/0x2E) + glow G3 rings (0x32) decode with
 * the standard PSX polygon layout (code = high byte of word[+0x04]; word[+0x00]
 * is the OT tag whose high byte is the *length*, low24 the link). Tiles (0x60),
 * lines (0x40/0x48) and the sparkle quads are NON-polygon prims: kept as raw
 * words for the dump, decoded specially in the feed once the dump confirms the
 * exact layout. */
typedef struct {
    uint8_t  code;          /* GPU prim code byte (word[1]>>24)  */
    uint8_t  taglen;        /* OT tag length (word[0]>>24)       */
    int      nv;            /* decoded polygon verts (3/4), else 0 */
    float    x[4], y[4];
    uint32_t rgb[4];
    int      u[4], v[4];
    int      clut, tpage;
    int      textured, gouraud, quad, stp;
    uint32_t raw[16];
    int      nraw;
} prim_t;

#define MAX_PRIMS 8192
static prim_t s_prim[MAX_PRIMS]; static int s_prim_n;
/* The g_Particles pool pointer is double-buffered and swapped between render
 * calls, so reading it AFTER the .s runs gets the wrong (other) buffer. The
 * hook stashes the pointer the .s actually walked at hook-entry. */
static uint32_t s_part_pool;
static uint32_t s_code_hist[256];  /* keyed on the GPU code byte (word[+4]>>24) */
static uint32_t s_tag_hist[256];   /* keyed on the OT tag length (word[+0]>>24) */

static int s16(int v) { return (int)(int16_t)(uint32_t)v; }

/** @brief Appends one walked primitive to s_prim.
 *
 * Copies the decoded fields and the raw words. Tiles and lines are not
 * polygons: they arrive with nv = 0 and only their raw words, which the dump
 * prints and the A/B compares read.
 * @param ctx Unused.
 * @param s The walked primitive.
 * @return 1 when stored, 0 when s_prim holds MAX_PRIMS entries. */
static int particle_prim_sink(void* ctx, const ngte_prim* s) {
    (void)ctx;
    if (s_prim_n >= MAX_PRIMS) return 0;
    prim_t* t = &s_prim[s_prim_n];
    memset(t, 0, sizeof(*t));
    t->code = s->code; t->taglen = s->taglen; t->nv = s->nv;
    for (int k = 0; k < 4; k++) {
        t->x[k] = s->x[k]; t->y[k] = s->y[k]; t->rgb[k] = s->rgb[k];
        t->u[k] = s->u[k]; t->v[k] = s->v[k];
    }
    t->clut = s->clut; t->tpage = s->tpage;
    t->textured = s->textured; t->gouraud = s->gouraud;
    t->quad = s->quad; t->stp = s->stp;
    memcpy(t->raw, s->raw, sizeof(t->raw));
    t->nraw = s->nraw;
    s_prim_n++;
    return 1;
}

/** @brief Walks the guest polybuf range [start, end) and appends each primitive to s_prim.
 *
 * Appends from s_prim_n, which the caller resets, up to MAX_PRIMS, and adds
 * each primitive's GPU code and tag length to the histograms the dump
 * prints. Particle tags are at most 12 words and the dump prints at most 16
 * raw words per primitive.
 * @param cpu The guest CPU state.
 * @param start The first polybuf byte the renderer wrote.
 * @param end One past the last polybuf byte the renderer wrote. */
static void capture_prims(CPUState* cpu, uint32_t start, uint32_t end) {
    OP_PROF_ACCUM_SCOPE("particle.capture");
    const ngte_walk_policy pol = {
        .tag_len_max = 12, .raw_words_max = 16, .guard = MAX_PRIMS,
        .polygons_only = 0, .code_hist = s_code_hist, .tag_hist = s_tag_hist,
        .total = NULL, .count_all = 0,
    };
    ngte_walk_prims(cpu, start, end, &pol, particle_prim_sink, NULL);
}

/* ---- one-shot dump (gamestate pools + raw polybuf + decoded prims) -------- */
/** @brief Prints the particle, sparkle and glow pools, the camera and the GTE projection registers to stderr.
 *
 * Verify-mode diagnostic, run once. Lists the first 16 live g_Particles
 * slots of the pool the renderer walked (s_part_pool, stashed at hook
 * entry) with the live total, every g_Sparkles[8] slot, the active
 * D_80078800 glow slots, the camera's first five projection words and
 * position, and GTE OFX, OFY, H, DQA and DQB.
 * @param cpu The guest CPU state. */
static void dump_pools(CPUState* cpu) {
    fprintf(stderr, "  [particles] g_Particles walk (sel/m_Class Life m03 | +4 +8):");
    uint32_t pp = s_part_pool; int npart = 0;   /* pool the .s walked (hook-entry) */
    for (int i = 0; i < 320; i++, pp += 0x20) {
        uint32_t hdr = cpu->read_word(pp);
        int sel = (int)((hdr >> 8) & 0xFF), mclass = (int)(hdr & 0xFF);
        if (sel == 0xFF) break;                        /* end of pool */
        if (sel & 0x80) continue;                      /* inactive slot (skip) */
        int life = (int)((hdr >> 16) & 0xFF), m03 = (int)((hdr >> 24) & 0xFF);
        if (npart < 16) fprintf(stderr, " [%d]s%d/c%d L%d d%d(%08X,%08X)", i, sel, mclass, life, m03,
                                cpu->read_word(pp+4), cpu->read_word(pp+8));
        npart++;
    }
    fprintf(stderr, "  (total live=%d)\n", npart);

    fprintf(stderr, "  [sparkles] g_Sparkles[8] (pos | Life Speed type | color size):\n");
    for (int i = 0; i < 8; i++) {
        uint32_t sp = G_SPARKLES + (uint32_t)i*0x18;
        int x = (int)cpu->read_word(sp+0), y = (int)cpu->read_word(sp+4), z = (int)cpu->read_word(sp+8);
        uint32_t w3 = cpu->read_word(sp+0xC), col = cpu->read_word(sp+0x10), w5 = cpu->read_word(sp+0x14);
        fprintf(stderr, "    [%d] (%d,%d,%d) life=%d spd=%d t=%d col=%06X size=%d\n", i, x, y, z,
                (int)(w3&0xFF), (int)((w3>>8)&0xFF), (int)((w3>>24)&0xFF), col&0xFFFFFF, (int)((w5>>8)&0xFF));
    }

    fprintf(stderr, "  [glows] D_80078800[16] (active | MobyPos* color size | offset bias):\n");
    for (int i = 0; i < 16; i++) {
        uint32_t g = D_GLOWS + (uint32_t)i*0x24;
        uint32_t act = cpu->read_word(g+0);
        if (act == 0) continue;
        uint32_t mp = cpu->read_word(g+0x08), col = cpu->read_word(g+0x0C), sz = cpu->read_word(g+0x10);
        int ox = (int)cpu->read_word(g+0x14), oy = (int)cpu->read_word(g+0x18), oz = (int)cpu->read_word(g+0x1C);
        int bias = (int)cpu->read_word(g+0x20);
        fprintf(stderr, "    [%d] act=%X mobypos=%08X col=%06X size=%d off=(%d,%d,%d) bias=%d\n",
                i, act, mp, col&0xFFFFFF, (int)sz, ox, oy, oz, bias);
    }
    fprintf(stderr, "  camera projMat:");
    for (int i = 0; i < 5; i++) fprintf(stderr, " %08X", cpu->read_word(G_CAMERA+(uint32_t)i*4));
    fprintf(stderr, "  pos=(%d,%d,%d)\n", (int)cpu->read_word(G_CAMERA+0x28),
            (int)cpu->read_word(G_CAMERA+0x2C), (int)cpu->read_word(G_CAMERA+0x30));
    fprintf(stderr, "  OFX=%d OFY=%d H=%d DQA=%d DQB=%d\n",
            (int)cpu->gte_ctrl[24]>>16, (int)cpu->gte_ctrl[25]>>16, (int)(cpu->gte_ctrl[26]&0xFFFF),
            (int)cpu->gte_ctrl[27], (int)cpu->gte_ctrl[28]);
}

static void dump_prims(CPUState* cpu, const char* tag, uint32_t start, uint32_t end) {
    fprintf(stderr, "\n=== [%s-dump f=%u] polybuf [%08X,%08X) %u bytes, %d prims ===\n",
            tag, openpete_frame, start, end, end - start, s_prim_n);
    /* linear raw dump (4 words/row) — the only reliable view of non-polygon prims */
    fprintf(stderr, "  RAW words:\n");
    int rows = 0;
    for (uint32_t p = start; p < end && rows < 40; p += 16, rows++) {
        fprintf(stderr, "    +%04X:", p - start);
        for (int i = 0; i < 4 && p + (uint32_t)i*4 < end; i++)
            fprintf(stderr, " %08X", cpu->read_word(p + (uint32_t)i*4));
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "  code histogram (word[+4]>>24):");
    for (int c = 0; c < 256; c++) if (s_code_hist[c]) fprintf(stderr, " %02X:%u", c, s_code_hist[c]);
    fprintf(stderr, "\n  tag-len histogram (word[+0]>>24):");
    for (int c = 0; c < 256; c++) if (s_tag_hist[c]) fprintf(stderr, " %d:%u", c, s_tag_hist[c]);
    fprintf(stderr, "\n");
    int n = s_prim_n < 8 ? s_prim_n : 8;
    for (int i = 0; i < n; i++) {
        prim_t* t = &s_prim[i];
        fprintf(stderr, "  prim[%d] code=%02X taglen=%d nv=%d tex=%d gour=%d quad=%d stp=%d  raw:",
                i, t->code, t->taglen, t->nv, t->textured, t->gouraud, t->quad, t->stp);
        for (int k = 0; k < t->nraw; k++) fprintf(stderr, " %08X", t->raw[k]);
        if (t->nv) {
            fprintf(stderr, "\n      decoded:");
            for (int k = 0; k < t->nv; k++)
                fprintf(stderr, " v%d(%.0f,%.0f rgb=%06X uv=%d,%d)", k, t->x[k], t->y[k], t->rgb[k], t->u[k], t->v[k]);
            if (t->textured) fprintf(stderr, " clut=%04X tpage=%04X", t->clut, t->tpage);
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "=== end %s-dump ===\n\n", tag);
}

/* ---- native float walk (3b.3) — WORK IN PROGRESS, verify-only ------------- *
 * Re-derive particle geometry from gamestate in float. NOT YET fed to the scene
 * (no native particle rendering) and NOT YET A/B-validated — this is the probe
 * that will become the walk. It runs only under OPENPETE_PARTICLE_VERIFY (an
 * A/B diagnostic) and is inert in the feed/oracle paths, so determinism is
 * unaffected.
 *
 * ESTABLISHED so far (see the format notes "Native walk progress"):
 *   - g_Particles (0x80075824) is a POINTER to the buffer (deref before walking).
 *   - dispatch class = the HIGH byte of the lh header (m_Type/"sel"); the LOW
 *     byte is m_Class (texture/clut index). The doc had these swapped.
 *   - position layout: X=lo16(+0x04), Y=hi16(+0x04), Z=lo16(+0x08), all in
 *     world/4 units; camera-relative VX=X-camX/4, VY=camY/4-Y, VZ=camZ/4-Z.
 *     Confirmed by sel=2/3 particles decoding to small near-camera offsets.
 *   - the FT4 (code 0x2C) prims in the demo come from sel=2 + sel=3 (affine /
 *     GPF-rect billboards), NOT sel=0 — so a clean ordered A/B needs ALL FT4
 *     emitters (0/2/3) implemented before the prim sequence aligns. TODO.
 *
 * Stage 1 below = the FT4 emitters present in the attract demo: sel 3
 * (block_80057A74, depth-cued screen rect), sel 2 (block_80057C08, affine
 * sin/cos billboard) + the sel>=6 fall-through rotated billboard
 * (block_800574F8). sel 0 (TILE), sel 1/4 (lines), sel 5 (affine matrix),
 * glows + sparkles still TODO. */
/* WORLD particles project through proj2 with camera-relative coords shifted >>2
 * (world/4), so their view-Z (cz) is 1/4 the shared full-world scale that
 * scene_viewz + the terrain/actor depths use. Scale cz up by 4 at the build
 * sites (where the class is known) so they sort against terrain instead of
 * always being ~4x too near and drawing THROUGH it. HUD bursts (sel 4/5, direct
 * view-space) and glows (raw-cam, full-world) are already full-scale — not
 * touched. cz is feed-only (culls use the raw view-Z), so scaling it is safe. */
#define PART_VZ_WORLD4 4.0f

static int rd_s16(CPUState* cpu, uint32_t addr) {
    uint32_t w = cpu->read_word(addr & ~3u);
    return (int)(int16_t)((addr & 2u) ? (w >> 16) : (w & 0xFFFF));
}
static double clamp16(double v) { return v < -32768.0 ? -32768.0 : (v > 32767.0 ? 32767.0 : v); }

/* ---- sub-tick CHANNEL-PHASE interpolation (pool-wide) ---------------------- *
 * The g_Particles position bracket (reextract.c) lerps only X/Y/Z;
 * every other per-tick-animated channel — billboard SPIN (B byte3) and SIZE
 * (B byte2), sel-3 rect half-extents, sel-1/4 line FAR endpoints (B.hi + word C)
 * — was read raw-curr and stepped at 29.913 Hz while the position glided (same
 * class as the gem-sparkle twinkle phase). When the re-extract
 * pass is active (s_phase_now latched by particle_reextract) the build
 * walks lerp those channels between the slot's prev/curr interp-objs snapshots
 * at FLOAT precision. Line/tile COLOURS (+0x0C/+0x10/+0x14 rgb) are HELD at
 * curr, not lerped — colour ramps can encode palette cycling where a linear rgb
 * sweep is wrong; revisit if a stepped colour is ever visible.
 * Canonical inline feeds (hook time, s_phase_now==0) stay integer/byte-exact
 * for the A/B harnesses. Render-side only: particle RAM is never written here. */
static op_time_t s_phase_now = 0;   /* present clock; 0 = phase interp off
                                     * (latched in particle_reextract) */
/* One sim tick in WALL time — the per-slot alpha denominator cap, same global
 * as TICK_PERIOD_NS in reextract.c (--tick-scale aware: run.c
 * rescales it, else slow-motion sweeps the phase in tick_scale of a tick and
 * freezes the rest). */
#define PART_TICK_PERIOD_NS g_openpete_tick_period_ns
/* Phase-alpha floor — the SAME slightly-negative allowance the spine's
 * position/rotation alpha uses (reextract.c: alpha >= -0.35).
 * The canonical present + first intermediates of a tick fire a few ms BEFORE
 * that tick's PLL grid stamp, so their raw alpha is slightly negative;
 * clamping the phase at 0 while positions extrapolate backward freezes the
 * twinkle for those presents and delivers the clipped tail as a jump at
 * every roll — a 29.913 Hz hitch riding an otherwise smooth sweep, sized by
 * the (machine-dependent) present-clock-vs-stamp skew. Backward
 * extrapolation on the pair's slope is continuous across the roll by
 * construction. Never above 1 (the future is unsimulated). */
#define PART_ALPHA_MIN (-0.35)

/** @brief Reports whether OPENPETE_STREAK_DBG enables the long-streak log.
 *
 * When set, streak_log prints each emitted particle line, glow fan leg or
 * sparkle line longer than 300 px on screen with its family and pool slot,
 * at most five lines per openpete_frame value. The variable is read once
 * and cached.
 * @return 1 when the variable is set and not "0", else 0. */
static int streak_dbg(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_STREAK_DBG"); v = (e && *e && *e != '0'); }
    return v;
}
static void streak_log(const char* fam, uint32_t slot,
                       double ax, double ay, double bx, double by) {
    double dx = bx - ax, dy = by - ay;
    if (dx*dx + dy*dy < 300.0*300.0) return;
    static unsigned s_n, s_lastf;
    if (openpete_frame != s_lastf) { s_lastf = openpete_frame; s_n = 0; }
    if (s_n++ > 4) return;                     /* max 5 lines per VBL */
    fprintf(stderr, "[streak f=%u] %s slot=0x%X (%.0f,%.0f)->(%.0f,%.0f)\n",
            openpete_frame, fam, slot, ax, ay, bx, by);
}

/* Guarded prev-state lookup for a g_Particles slot. Returns 1 + fills prev
 * words A/B/C + the slot's own alpha iff the SAME identity guards the position
 * bracket applies all pass (reextract.c pool loop) — a slot whose
 * position SNAPPED (spawn / reuse / teleport) must not lerp its other channels,
 * or spin/size would sweep across two different particles. */
typedef struct { uint32_t A, B, C;      /* table-PREV words */
                 uint32_t cA, cB, cC;   /* table-CURR words — the forward
                                          * endpoint. NEVER live RAM: under
                                          * runahead live = committed tick ≈
                                          * table-prev, degenerating every
                                          * channel lerp back to 30 Hz (the
                                          * original sparkle complaint). */
                 double alpha; } part_prev_t;
static int part_phase_prev(uint32_t pp, part_prev_t* out) {
    if (!s_phase_now) return 0;
    const openpete_interp_obj_t* o = openpete_interp_objs_find(pp);
    if (!o || !o->have_prev) return 0;                 /* spawn → snap */
    uint32_t ph, ch;
    memcpy(&ph, o->prev + 0x00, 4);
    memcpy(&ch, o->curr + 0x00, 4);
    if ((ph & 0xFFFFu) != (ch & 0xFFFFu)) return 0;    /* m_Class/sel changed → reuse */
    uint8_t plife = (uint8_t)(ph >> 16), clife = (uint8_t)(ch >> 16);
    if (clife < plife) return 0;                       /* life reset → reuse */
    uint32_t pA, pB, pC, cA, cB, cC;
    memcpy(&pA, o->prev + 0x04, 4); memcpy(&pB, o->prev + 0x08, 4);
    memcpy(&pC, o->prev + 0x0C, 4);
    memcpy(&cA, o->curr + 0x04, 4); memcpy(&cB, o->curr + 0x08, 4);
    memcpy(&cC, o->curr + 0x0C, 4);
    double dX = (double)(int16_t)(cA & 0xFFFF) - (double)(int16_t)(pA & 0xFFFF);
    double dY = (double)(int16_t)(cA >> 16)    - (double)(int16_t)(pA >> 16);
    double dZ = (double)(int16_t)(cB & 0xFFFF) - (double)(int16_t)(pB & 0xFFFF);
    double d2 = dX*dX + dY*dY + dZ*dZ;
    /* Teleport/reuse ceiling, particle-physics scale (was 1250 == 5000
     * full-world, with a tighter 300 only for the stalled-life class):
     * under runahead the pair can span two prediction BRANCHES (the led
     * timeline re-forks whenever the held input differs from what the
     * committed tick latches), and pool churn differs between branches —
     * slot reuse across branches passes the class + life-increase tests
     * (no same-timeline ordering to reset the life byte), and anything
     * under the old ceiling lerped as a screen-crossing streak. 300
     * world/4 per tick == 1200 full-world == ~20x Spyro's run speed: a
     * REAL particle faster than that crosses the screen in a couple of
     * ticks, so snapping it loses nothing visible — beyond this, lerping
     * only ever paints streaks. */
    if (d2 > 300.0*300.0) return 0;
    op_time_t per = o->curr_ns - o->prev_ns;
    if (per > PART_TICK_PERIOD_NS) per = PART_TICK_PERIOD_NS;
    if (per <= 0) return 0;
    double a = (double)(s_phase_now - o->curr_ns) / (double)per;
    if (a < PART_ALPHA_MIN) a = PART_ALPHA_MIN; else if (a > 1.0) a = 1.0;
    out->A = pA; out->B = pB; out->C = pC;
    out->cA = cA; out->cB = cB; out->cC = cC;
    out->alpha = a;
    return 1;
}

/** @brief Interpolates a byte channel from p to c at alpha a, clamped to [0, 255].
 *
 * A negative alpha extrapolates backward past p; the clamp keeps a size or
 * colour byte channel in range.
 * @param p The previous tick's byte.
 * @param c The current tick's byte.
 * @param a The sub-tick alpha, at least PART_ALPHA_MIN.
 * @return The interpolated value. */
static double lerp_b(int p, int c, double a) {
    double v = (double)p + ((double)c - (double)p) * a;
    return v < 0.0 ? 0.0 : (v > 255.0 ? 255.0 : v);
}
/** @brief Interpolates a byte angle (256 per circle) from p to c along the shorter arc.
 *
 * The result is not wrapped back into [0, 256).
 * @param p The previous tick's angle byte.
 * @param c The current tick's angle byte.
 * @param a The sub-tick alpha.
 * @return The interpolated angle. */
static double lerp_wrap_b(int p, int c, double a) {
    int d = (c - p) & 0xFF; if (d >= 128) d -= 256;
    return (double)p + (double)d * a;
}

/** @brief Reports whether OPENPETE_PART_PHASE_DBG enables the particle phase trace.
 *
 * When set, each particle build walk prints one line for the first
 * phase-interpolated particle with its interpolated size and spin channels
 * and the raw bytes. The variable is read once and cached.
 * @return 1 when the variable is set and not "0", else 0. */
static int part_phase_dbg(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_PART_PHASE_DBG"); s = (e && *e && *e != '0'); }
    return s;
}
static int s_ppdbg_done = 0;   /* reset per build walk */
/** @brief Reports whether OPENPETE_GLOW_ANIM_DBG enables the glow animation trace.
 *
 * The trace is a census of raw glow-record changes plus a per-present
 * phase line. The variable is read once and cached.
 * @return 1 when the variable is set and not "0", else 0. */
static int glow_anim_dbg(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_GLOW_ANIM_DBG"); s = (e && *e && *e != '0'); }
    return s;
}

/* The projection view of the running build (camera, offsets, H, Hor+
 * bounds); each build entry rebuilds it, the cull helpers read it. */
static ngte_view s_pv;

/** @brief Reports whether an inline projection falls in the GTE divide-cap regime.
 *
 * The cap applies when SZ3 <= 0 or SZ3 <= H/2 at the GTE's H (Hc). The
 * verdict at the display H (He) is only counted in the NGTE_SITE_PART_CAP
 * census.
 * @param sz3 The clamped SZ3.
 * @param Hc The canonical projection distance.
 * @param He The display projection distance.
 * @return 1 when the quotient is capped, else 0. */
static int part_cap(double sz3, double Hc, double He) {
    int c = (sz3 <= 0.0 || Hc >= 2.0 * sz3);
    NGTE_LIMIT(NGTE_SITE_PART_CAP, c, sz3 <= 0.0 || He >= 2.0 * sz3);
    return c;
}

/* RTPS one view-space vertex (TR=0) through R -> screen + the view-space MAC */
typedef struct { double sx, sy, m1, m2, m3; } projres;

/* Perspective-project a VIEW-SPACE point (X,Y,Z) directly — sel 4/5 (HUD
 * particles) store view-space coords and feed them to the GTE as the TR with the
 * vertex = (0,0,0), so MAC=(X,Y,Z) and SX=OFX+X*H/Z (the camera matrix is
 * multiplied by V=0, so it drops out). No swizzle. */
static void proj_view(double ofx, double ofy, const ngte_view* v,
                      double X, double Y, double Z, projres* o);
static void proj2(const int R[3][3], double ofx, double ofy, const ngte_view* v,
                  double vx, double vy, double vz, projres* o) {
    ngte_proj r;   /* shared pipeline, NO ±1024 SXY saturation — float,
                    * widescreen reveal needs the true X */
    ngte_project_gte(R, NULL, vx, vy, vz, ofx, ofy, v, 0, &r);
    o->sx = r.sx; o->sy = r.sy; o->m1 = r.m1; o->m2 = r.m2; o->m3 = r.m3;
}

static void proj_view(double ofx, double ofy, const ngte_view* v,
                      double X, double Y, double Z, projres* o) {
    ngte_proj r;   /* view-space point: MAC=(X,Y,Z), divide-only */
    ngte_screen_gte(X, Y, Z, ofx, ofy, v, 0, &r);
    o->sx = r.sx; o->sy = r.sy; o->m1 = r.m1; o->m2 = r.m2; o->m3 = r.m3;
}

/** @brief Applies the .s screen-rect reject to a projected particle centre.
 *
 * Rounds the centre and tests the packed int32 (SY << 16) | (SX & 0xFFFF)
 * as the .s does: packed <= 0x10000 (s4) rejects at the top, packed >=
 * 0x1000000 (s5) at the bottom. The left and right edges (the .s SX <= 0
 * and the s6 0x2000000 test) are the running view's horizontal bounds
 * s_pv.xlo and s_pv.xhi, which equal [0, 512] at 4:3 and widen for a wider
 * view.
 * @param sxf The centre's screen X.
 * @param syf The centre's screen Y.
 * @return 1 to cull, 0 to keep. */
static int screen_rect_cull(double sxf, double syf) {
    int SX = (int)lrint(sxf), SY = (int)lrint(syf);
    int32_t v0  = (int32_t)(((uint32_t)(SY & 0xFFFF) << 16) | (uint32_t)(SX & 0xFFFF));
    if ((v0 - 0x10000) <= 0)    return 1;   /* above top / SY<=1   */
    if ((v0 - 0x1000000) >= 0)  return 1;   /* below bottom SY>=256 */
    /* horizontal edges widen for widescreen (Hor+): [0,512] at 4:3 (exactly the
     * .s SX<=0 / SX>=512 reject), wider when the window is. signed SX so the
     * negative left-reveal isn't mistaken for a wrapped large value. */
    float xlo = s_pv.xlo, xhi = s_pv.xhi;
    if (SX <= (int)xlo)         return 1;    /* left of the screen  */
    if (SX >= (int)xhi)         return 1;    /* right of the screen */
    return 0;
}

/** @brief Applies the default particle class's four-corner screen reject (block_8005768C..80057718).
 *
 * Culls only when all four corners lie outside the same screen edge,
 * tested on the packed int32 (SY << 16) | (SX & 0xFFFF) as the .s does:
 * top against 0x10000 (s4), bottom against 0x1000000 (s5). The left and
 * right edges (the .s SX <= 0 and s6) are the running view's horizontal
 * bounds, which widen past [0, 512] for a wider-than-4:3 view.
 * @param sx The four corners' screen X.
 * @param sy The four corners' screen Y.
 * @return 1 to cull, 0 to keep. */
static int clip4_cull(const float* sx, const float* sy) {
    /* horizontal edges widen for widescreen (Hor+); vertical stays 4:3. */
    float xlo = s_pv.xlo, xhi = s_pv.xhi;
    int top = 1, bot = 1, left = 1, right = 1;
    for (int k = 0; k < 4; k++) {
        int SX = (int)lrintf(sx[k]), SY = (int)lrintf(sy[k]);
        int32_t v   = (int32_t)(((uint32_t)(SY & 0xFFFF) << 16) | (uint32_t)(SX & 0xFFFF));
        if ((v - 0x10000) > 0)     top = 0;
        if ((v - 0x1000000) < 0)   bot = 0;
        if (SX > (int)xlo)         left = 0;
        if (SX < (int)xhi)         right = 0;
    }
    return top | bot | left | right;
}

typedef struct {
    float sx[4], sy[4], sz[4];   /* corner screen XY + per-corner view-Z */
    float cz;                     /* representative centre view-Z (depth sort) */
    int sel;
    uint32_t rgb;                 /* modulation colour (particle +0x0C low24) */
    int u[4], v[4];               /* per-corner texel coords */
    int tpx, tpy, clx, cly, bpp;  /* VRAM material */
    int textured, blend;          /* 0 opaque / 1 alpha / 2 add / 3 sub */
} fquad;
#define MAX_FQUAD 2048
static fquad s_fq[MAX_FQUAD]; static int s_fq_n;

/* sel-1 gouraud motion-streak LINE_G (code 0x50): a line between two
 * camera-relative world positions with a per-endpoint colour. */
typedef struct {
    float ax, ay, bx, by;        /* projected endpoints */
    uint32_t rgb0, rgb1;         /* per-endpoint colour (gouraud) */
    float cz;                    /* EP0 view-Z (depth) */
    int blend;
    int sel;                     /* 1 = world streak; 4 = HUD egg-burst (view-space) */
} pline;
#define MAX_PLINE 1024
static pline s_pline[MAX_PLINE]; static int s_pline_n;

/** @brief Decodes a particle's FT4 material from gamestate into a native quad.
 *
 * One idiom serves every FT4 class: the colour/command word at +0x0C (high
 * byte the GPU code, bit 1 of it STP), the texture record at
 * D_80076278[m_Class] + (byte +0x10) * 8, UV0 and the CLUT at texrec + 4,
 * UV3 and the tpage at texrec + 8, and the other two corners the (u3, v0)
 * and (u0, v3) split, corner k taking UVk in the .s vertex order. A class
 * with no texture table entry decodes as flat. The blend is the tpage's ABR
 * mode when STP is set (the quarter rate drawn as additive), else opaque.
 * @param cpu The guest CPU state.
 * @param pp The particle record's guest address.
 * @param mclass The particle's m_Class.
 * @param q Receives the colour, UVs, VRAM material and blend. */
static void decode_material(CPUState* cpu, uint32_t pp, int mclass, fquad* q) {
    uint32_t cmd = cpu->read_word(pp + 0x0C);
    q->rgb = cmd & 0xFFFFFF;
    int stp = ((cmd >> 24) & 0x02) != 0;          /* GPU code STP bit */
    uint32_t texbase = cpu->read_word(D_PARTTEX + (uint32_t)mclass * 4);
    if (!texbase) {                               /* no texture record -> flat */
        q->textured = 0; q->blend = stp ? 1 : 0;
        for (int k = 0; k < 4; k++) { q->u[k] = q->v[k] = 0; }
        q->tpx = q->tpy = q->clx = q->cly = q->bpp = 0;
        return;
    }
    uint32_t texrec = texbase + (cpu->read_word(pp + 0x10) & 0xFF) * 8u;
    uint32_t w4 = cpu->read_word(texrec + 4), w8 = cpu->read_word(texrec + 8);
    int u0 = (int)(w4 & 0xFF), v0 = (int)((w4 >> 8) & 0xFF), clut = (int)((w4 >> 16) & 0xFFFF);
    int u3 = (int)(w8 & 0xFF), v3 = (int)((w8 >> 8) & 0xFF), tpage = (int)((w8 >> 16) & 0xFFFF);
    q->u[0]=u0; q->v[0]=v0; q->u[1]=u3; q->v[1]=v0; q->u[2]=u0; q->v[2]=v3; q->u[3]=u3; q->v[3]=v3;
    ngte_tpage tp;
    ngte_decode_tpage((unsigned)tpage, (unsigned)clut, 0u, &tp);
    q->tpx = tp.tpx; q->tpy = tp.tpy;
    q->bpp = tp.bpp;
    q->clx = tp.clx; q->cly = tp.cly;
    q->textured = 1;
    /* data-driven blend (ABR + STP); rate 3 has no native pipeline */
    q->blend = stp ? ngte_abr_blend(tp.abr, NGTE_ABR_QUARTER_AS_ADD) : 0;
}

/** @brief Applies the .s SZ window and depth-cue LOD cull to a particle.
 *
 * Clamps MAC3 to [0, 65535] as SZ3, rejects SZ3 outside [128, 8192), and
 * rejects when (SZ3 >> 5) < texsub_hi.
 * @param mac3 The centre's view-space MAC3.
 * @param texsub_hi The high byte of the record's low halfword at +0x10.
 * @return 1 to cull, 0 to keep. */
static int sz_and_lod_cull(double mac3, int texsub_hi) {
    int sz3 = (int)(mac3 < 0 ? 0 : (mac3 > 65535 ? 65535 : mac3));
    if (sz3 < 128 || sz3 >= 8192) return 1;
    if (((sz3 >> 5) - texsub_hi) < 0) return 1;
    return 0;
}

static void particle_build(CPUState* cpu) {
    s_fq_n = 0; s_pline_n = 0; s_ppdbg_done = 0;
    ngte_view_build(cpu, &s_pv);
    const ngte_view* v = &s_pv;
    int R[3][3]; memcpy(R, v->cam_M, sizeof R);
    /* Camera at FRACTIONAL /4 (not >>2): the particle positions (A/B) are int16
     * world/4 gameplay storage we can't change, but flooring the CAMERA to /4
     * added a camera-correlated step on EVERY particle under panning. Carrying the
     * camera's low 2 bits (÷4.0) tracks the smooth sub-tick camera (the view
     * holds the un-rounded interpolated camera when bracketed). */
    double s7 = v->cam[0] / 4.0, t8 = v->cam[1] / 4.0, t9 = v->cam[2] / 4.0;
    double ofx = v->ofx, ofy = v->ofy;
    double H   = v->H_eff;
    ngte_view vc; ngte_view_canon(v, &vc);   /* HUD bursts (sel 4/5) project where the game put them */
    const double Hc = v->H_canon, qcap = NGTE_DIV_CAP * v->s;  /* GTE limit regime on the GTE's H; capped quotient scales with the view */

    uint32_t pp = s_part_pool;                          /* pool the .s walked (hook-entry) */
    for (int i = 0; i < 320 && s_fq_n < MAX_FQUAD; i++, pp += 0x20) {
        uint32_t hdr = cpu->read_word(pp);
        int mtype = (int)((hdr >> 8) & 0xFF);           /* dispatch byte (sel) */
        int mclass = (int)(hdr & 0xFF);                 /* texture/clut index */
        if (mtype == 0xFF) break;                       /* end of pool (gp>>8==-1) */
        if (mtype & 0x80) continue;                     /* inactive slot (skip, not stop) */
        int sel = mtype;
        uint32_t A = cpu->read_word(pp + 4), B = cpu->read_word(pp + 8);
        double VXb = (double)(int)(A & 0xFFFF) - s7;    /* GTE-Z */
        double VYb = t8 - (double)(int)(A >> 16);       /* GTE-X */
        double VZb = t9 - (double)(int)(B & 0xFFFF);    /* GTE-Y */
        int texsub_hi = (int)(cpu->read_word(pp + 0x10) & 0xFFFF) >> 8;
        fquad q; q.sel = sel;

        if (sel == 3) {                                 /* depth-cued screen rect */
            projres c; proj2(R, ofx, ofy, v, (double)VYb, (double)VZb, (double)VXb, &c);
            if (screen_rect_cull(c.sx, c.sy)) continue;
            if (sz_and_lod_cull(c.m3, texsub_hi)) continue;
            /* GTE reciprocal-Z, kept in DOUBLE for a sub-pixel billboard footprint.
             * The .s quantizes it to the integer IR0 (1.12 fixed, capped at 1.0)
             * and the half-extents to whole pixels (`(sizeB*IR0)>>12`), so the
             * sprite size jumps in pixel steps with distance even though the centre
             * (c.sx/c.sy) is sub-pixel. The native carries the float ratio so the
             * footprint grows smoothly. Display-only (native renderer consumer). */
            double sz3 = c.m3 < 0 ? 0 : (c.m3 > 65535 ? 65535 : c.m3);
            double qd = sz3 <= 0.0 ? 0.0 : (double)H / sz3;
            double ir0 = qd < 0.0 ? 0.0 : (qd > 1.0 ? 1.0 : qd);   /* GTE caps the reciprocal at 1.0 */
            int sizeB = (int)((B >> 16) & 0xFF), alphaB = (int)((B >> 24) & 0xFF);
            double szf = (double)sizeB, alf = (double)alphaB;
            part_prev_t pv;                              /* sub-tick half-extent sweep */
            if (part_phase_prev(pp, &pv)) {
                szf = lerp_b((int)((pv.B >> 16) & 0xFF), (int)((pv.cB >> 16) & 0xFF), pv.alpha);
                alf = lerp_b((int)((pv.B >> 24) & 0xFF), (int)((pv.cB >> 24) & 0xFF), pv.alpha);
                if (part_phase_dbg() && !s_ppdbg_done) { s_ppdbg_done = 1;
                    fprintf(stderr, "[part-phase f=%u] slot=%d sel=3 lerp sz=%.2f al=%.2f (raw %d/%d)\n",
                            openpete_frame, i, szf, alf, sizeB, alphaB); }
            }
            double a2 = szf * ir0, a3 = alf * ir0;       /* halfW, halfH (sub-pixel) */
            double t0 = a2 * 0.5, t1 = a3 * 0.5;
            double cx = c.sx + t0, cy = c.sy + t1;       /* rect corners (screen) */
            double X0 = cx - a2, X1 = cx, Y0 = cy - a3, Y1 = cy;
            q.sx[0]=X0; q.sy[0]=Y0; q.sx[1]=X1; q.sy[1]=Y0;
            q.sx[2]=X0; q.sy[2]=Y1; q.sx[3]=X1; q.sy[3]=Y1;
            for (int k = 0; k < 4; k++) q.sz[k] = (float)c.m3;
            q.cz = (float)c.m3 * PART_VZ_WORLD4;        /* world/4 -> full-world */
            decode_material(cpu, pp, mclass, &q);
            s_fq[s_fq_n++] = q;
        } else if (sel == 2) {                          /* affine sin/cos billboard */
            projres c; proj2(R, ofx, ofy, v, (double)VYb, (double)VZb, (double)VXb, &c);
            if (screen_rect_cull(c.sx, c.sy)) continue;
            if (sz_and_lod_cull(c.m3, texsub_hi)) continue;
            int sizeB = (int)((B >> 16) & 0xFF);
            int angIdx = (int)(((B >> 23) + 64) & 0x1FE);
            double a2, a3;                               /* corner extents (sub-pixel) */
            part_prev_t pv;                              /* sub-tick spin/size sweep */
            if (part_phase_prev(pp, &pv)) {
                /* angle channel = B byte3 (angIdx == (byte3+32 entries) — the
                 * bit-23 term is cleared by the &0x1FE); wrap-lerp it, then
                 * float sin/cos. Identical to the integer path at alpha=1. */
                double szf = lerp_b((int)((pv.B >> 16) & 0xFF), (int)((pv.cB >> 16) & 0xFF), pv.alpha);
                double ef  = lerp_wrap_b((int)(pv.B >> 24), (int)(pv.cB >> 24), pv.alpha) + 32.0;
                double sf, cf; ngte_sincos_f(cpu, ef, &sf, &cf);
                a2 = szf * sf / 1024.0; a3 = szf * cf / 1024.0;
                if (part_phase_dbg() && !s_ppdbg_done) { s_ppdbg_done = 1;
                    fprintf(stderr, "[part-phase f=%u] slot=%d sel=2 lerp ang=%.2f sz=%.2f (raw %d/%d)\n",
                            openpete_frame, i, ef, szf, (int)(B >> 24), sizeB); }
            } else {
                int sinv = rd_s16(cpu, D_SIN_TABLE + (uint32_t)angIdx);
                int cosv = rd_s16(cpu, D_SIN_TABLE + (uint32_t)angIdx + 0x80);
                a2 = (double)sizeB * sinv / 1024.0; a3 = (double)sizeB * cosv / 1024.0;  /* float (not >>10): sub-pixel corner extent */
            }
            double C[4][2] = { {-a2,-a3}, {a3,-a2}, {-a3,a2}, {a2,a3} };
            double TR0 = c.m1*4.0, TR1 = c.m2*4.0, TR2 = c.m3*4.0;
            double sz3 = TR2 < 0 ? 0 : (TR2 > 65535 ? 65535 : TR2);
            double qd = part_cap(sz3, Hc, H) ? qcap : (double)H/sz3;
            for (int k = 0; k < 4; k++) {                /* R'=diag(4096,2560,4096)+TR */
                double m1 = C[k][0] + TR0;
                double m2 = C[k][1]*2560.0/4096.0 + TR1;
                q.sx[k] = (float)(ofx + clamp16(m1)*qd);
                q.sy[k] = (float)(ofy + clamp16(m2)*qd);
                q.sz[k] = (float)TR2;
            }
            q.cz = (float)c.m3 * PART_VZ_WORLD4;          /* world/4 -> full-world (centre depth, not TR2) */
            decode_material(cpu, pp, mclass, &q);
            s_fq[s_fq_n++] = q;
        } else if (sel >= 6) {                          /* rotated-billboard FT4 default */
            int sizeB = (int)((B >> 16) & 0xFF);
            int angIdx = (int)((B >> 23) & 0x1FE);
            double eS, eC;                               /* corner extents (sub-pixel) */
            part_prev_t pv;                              /* sub-tick spin/size sweep */
            if (part_phase_prev(pp, &pv)) {
                /* angle channel = B byte3 (angIdx == byte3 entries — bit 23 is
                 * cleared by the &0x1FE); wrap-lerp + float sin/cos. */
                double szf = lerp_b((int)((pv.B >> 16) & 0xFF), (int)((pv.cB >> 16) & 0xFF), pv.alpha);
                double ef  = lerp_wrap_b((int)(pv.B >> 24), (int)(pv.cB >> 24), pv.alpha);
                double sf, cf; ngte_sincos_f(cpu, ef, &sf, &cf);
                eS = szf * sf / 4096.0; eC = szf * cf / 4096.0;
                if (part_phase_dbg() && !s_ppdbg_done) { s_ppdbg_done = 1;
                    fprintf(stderr, "[part-phase f=%u] slot=%d sel=%d lerp ang=%.2f sz=%.2f (raw %d/%d)\n",
                            openpete_frame, i, sel, ef, szf, (int)(B >> 24), sizeB); }
            } else {
                int sinv = rd_s16(cpu, D_SIN_TABLE + (uint32_t)angIdx);
                int cosv = rd_s16(cpu, D_SIN_TABLE + (uint32_t)angIdx + 0x80);
                eS = (double)sizeB * sinv / 4096.0; eC = (double)sizeB * cosv / 4096.0;  /* float (not >>12): sub-pixel corner extent */
            }
            double cx[4] = { (double)VYb-eS, (double)VYb+eC, (double)VYb-eC, (double)VYb+eS };
            double cz[4] = { (double)VXb+eC, (double)VXb+eS, (double)VXb-eS, (double)VXb-eC };
            projres pr[4]; double zsum = 0;
            for (int k = 0; k < 4; k++) {
                proj2(R, ofx, ofy, v, cx[k], (double)VZb, cz[k], &pr[k]);
                q.sx[k] = (float)pr[k].sx; q.sy[k] = (float)pr[k].sy; q.sz[k] = (float)pr[k].m3;
                zsum += pr[k].m3 < 0 ? 0 : (pr[k].m3 > 65535 ? 65535 : pr[k].m3);
            }
            if (zsum < 512.0 || zsum >= 32768.0) continue;          /* depth-sum window */
            if ((((int)zsum) >> 7) - texsub_hi < 0) continue;       /* LOD cull */
            if (clip4_cull(q.sx, q.sy)) continue;                   /* 4-corner clip */
            q.cz = (float)(zsum / 4.0) * PART_VZ_WORLD4;  /* world/4 -> full-world */
            decode_material(cpu, pp, mclass, &q);
            s_fq[s_fq_n++] = q;
        } else if (sel == 0) {                          /* TILE / 1px dot (block_80057F98) */
            projres c; proj2(R, ofx, ofy, v, (double)VYb, (double)VZb, (double)VXb, &c);
            int sz3 = (int)(c.m3 < 0 ? 0 : (c.m3 > 65535 ? 65535 : c.m3));
            if (sz3 == 0 || sz3 >= 8192) continue;      /* SZ window (SZ3!=0 .. 8192) */
            if (screen_rect_cull(c.sx, c.sy)) continue;
            int lod = (int)((B >> 16) & 0xFF);
            if (((sz3 >> 5) - lod - 2) <= 0) continue;  /* depth-cue LOD cull (s3=2) */
            /* The .s emits this as a LINE_F from (SX,SY) to (SX+1,SY) — a
             * 2px-wide x 1px-tall horizontal footprint (PSX plots both line
             * endpoints). A 1px-wide quad renders as a tall sliver because PSX
             * pixels are tall (512x224 framebuffer shown at 4:3 ~ 0.583:1 px);
             * matching the 2px line width makes it ~square like the .s. */
            q.sx[0]=(float)c.sx;       q.sy[0]=(float)c.sy;        /* 2x1 line footprint */
            q.sx[1]=(float)(c.sx+2.0); q.sy[1]=(float)c.sy;
            q.sx[2]=(float)c.sx;       q.sy[2]=(float)(c.sy+1.0);
            q.sx[3]=(float)(c.sx+2.0); q.sy[3]=(float)(c.sy+1.0);
            for (int k = 0; k < 4; k++) q.sz[k] = (float)c.m3;
            q.cz = (float)c.m3 * PART_VZ_WORLD4;         /* world/4 -> full-world */
            uint32_t cmd = cpu->read_word(pp + 0x0C);    /* flat colour, no texture */
            q.rgb = cmd & 0xFFFFFF; q.textured = 0;
            q.blend = ((cmd >> 24) & 0x02) ? 1 : 0;
            for (int k = 0; k < 4; k++) { q.u[k] = q.v[k] = 0; }
            q.tpx = q.tpy = q.clx = q.cly = q.bpp = 0;
            s_fq[s_fq_n++] = q;
        } else if (sel == 1 && s_pline_n < MAX_PLINE) {  /* gouraud motion-streak LINE_G (block_80057E7C) */
            /* EP0 = (X0=A&0xFFFF, Y0=A>>16, Z0=B&0xFFFF); the loop's VXb/VYb/VZb
             * already hold EP0's swizzle (GTE-Z/X/Y). EP1 = (X1=B>>16,
             * Y1=C&0xFFFF, Z1=C>>16). Same camera-rel /4 swizzle. */
            uint32_t C = cpu->read_word(pp + 0x0C);
            double X1 = (double)(B >> 16), Y1 = (double)(C & 0xFFFF), Z1 = (double)(C >> 16);
            part_prev_t pv;              /* sub-tick FAR-endpoint sweep (EP0 rides the
                                          * position bracket; EP1 was stepping) */
            if (part_phase_prev(pp, &pv)) {
                /* The FAR endpoint needs its own physics ceiling: the
                 * part_phase_prev teleport guard covers only the BASE
                 * position (A word), so a cross-branch pair with a stable
                 * base but divergent far endpoint still lerped — and this
                 * class decodes UNSIGNED, so a pair straddling any wrap
                 * sweeps the LONG way across the coordinate space,
                 * painting a line to a consistent screen side. Over the
                 * ceiling: snap to table-CURR, never live RAM. */
                double pX1 = (double)(pv.B >> 16),    cX1f = (double)(pv.cB >> 16);
                double pY1 = (double)(pv.C & 0xFFFF), cY1f = (double)(pv.cC & 0xFFFF);
                double pZ1 = (double)(pv.C >> 16),    cZ1f = (double)(pv.cC >> 16);
                double dx = cX1f - pX1, dy = cY1f - pY1, dz = cZ1f - pZ1;
                if (dx*dx + dy*dy + dz*dz <= 300.0*300.0) {
                    X1 = pX1 + dx * pv.alpha;
                    Y1 = pY1 + dy * pv.alpha;
                    Z1 = pZ1 + dz * pv.alpha;
                } else {
                    X1 = cX1f; Y1 = cY1f; Z1 = cZ1f;
                }
            }
            projres e0, e1;
            proj2(R, ofx, ofy, v, (double)VYb, (double)VZb, (double)VXb, &e0);   /* EP0 */
            proj2(R, ofx, ofy, v, (t8 - Y1), (t9 - Z1), (X1 - s7), &e1);         /* EP1 */
            int sz3 = (int)(e0.m3 < 0 ? 0 : (e0.m3 > 65535 ? 65535 : e0.m3));
            if (sz3 == 0 || sz3 >= 8192) continue;       /* EP0 SZ window */
            if (screen_rect_cull(e0.sx, e0.sy)) continue; /* EP0 screen-rect */
            uint32_t col0 = cpu->read_word(pp + 0x10), col1 = cpu->read_word(pp + 0x14);
            int lod = (int)(col1 >> 24);
            if (((sz3 >> 5) - lod - 2) <= 0) continue;    /* EP0 depth-cue LOD (s3=2) */
            pline* l = &s_pline[s_pline_n++];
            l->ax=(float)e0.sx; l->ay=(float)e0.sy; l->bx=(float)e1.sx; l->by=(float)e1.sy;
            l->rgb0 = col0 & 0xFFFFFF; l->rgb1 = col1 & 0xFFFFFF;
            l->cz = (float)e0.m3 * PART_VZ_WORLD4;         /* world/4 -> full-world */
            l->blend = ((col0 >> 24) & 0x02) ? 1 : 0;     /* code 0x50 → opaque */
            l->sel = 1;
            if (streak_dbg()) streak_log("line1", pp, e0.sx, e0.sy, e1.sx, e1.sy);
        } else if (sel == 4 && s_pline_n < MAX_PLINE) {  /* HUD egg-burst LINE_G (block_80057954) */
            /* VALIDATED vs egg.bk2 (egg-collection HUD burst, level_11 f3579): 32/32
             * count-exact, endpoint NN ~0.6-1.1px mean / ~2px worst. A gouraud LINE_G
             * between two VIEW-SPACE points (HUD particle: the .s sets the GTE TR to
             * the stored coords with vertex=(0,0,0), so they project directly — NO
             * camera swizzle). EP0=(s16 A.lo, A.hi, B.lo); EP1=(s16 B.hi, C.lo, C.hi). */
            uint32_t C = cpu->read_word(pp + 0x0C);
            int X0=(int)(int16_t)(A&0xFFFF), Y0=(int)(int16_t)(A>>16), Z0=(int)(int16_t)(B&0xFFFF);
            int X1=(int)(int16_t)(B>>16),    Y1=(int)(int16_t)(C&0xFFFF), Z1=(int)(int16_t)(C>>16);
            double X1f = (double)X1, Y1f = (double)Y1, Z1f = (double)Z1;
            part_prev_t pv;              /* sub-tick FAR-endpoint sweep (EP0 rides the
                                          * position bracket; EP1 was stepping) */
            if (part_phase_prev(pp, &pv)) {
                /* Same far-endpoint ceiling as sel-1 (signed decode here):
                 * the base-position guard doesn't cover these words. Over
                 * the ceiling → snap to table-curr. */
                double pX1 = (double)(int16_t)(pv.B >> 16),    cX1s = (double)(int16_t)(pv.cB >> 16);
                double pY1 = (double)(int16_t)(pv.C & 0xFFFF), cY1s = (double)(int16_t)(pv.cC & 0xFFFF);
                double pZ1 = (double)(int16_t)(pv.C >> 16),    cZ1s = (double)(int16_t)(pv.cC >> 16);
                double dx = cX1s - pX1, dy = cY1s - pY1, dz = cZ1s - pZ1;
                if (dx*dx + dy*dy + dz*dz <= 300.0*300.0) {
                    X1f = pX1 + dx * pv.alpha;
                    Y1f = pY1 + dy * pv.alpha;
                    Z1f = pZ1 + dz * pv.alpha;
                } else {
                    X1f = cX1s; Y1f = cY1s; Z1f = cZ1s;
                }
            }
            projres e0, e1;
            proj_view(ofx, ofy, &vc, (double)X0, (double)Y0, (double)Z0, &e0);
            proj_view(ofx, ofy, &vc, X1f, Y1f, Z1f, &e1);
            int sz3 = (int)(e0.m3 < 0 ? 0 : (e0.m3 > 65535 ? 65535 : e0.m3));
            uint32_t col0 = cpu->read_word(pp + 0x10), col1 = cpu->read_word(pp + 0x14);
            if (sz3 <= 64 || sz3 >= 2048) continue;      /* sel-4 SZ window 64..2048 */
            if (screen_rect_cull(e0.sx, e0.sy)) continue;
            /* (sz3>>5)-(col1>>24) is a LOD-LEVEL index (clamped to 0 when negative,
             * block_80057A30 falls through — NOT a cull); it selects a colour-table
             * entry, not visibility. The line endpoints are already fixed. */
            pline* l = &s_pline[s_pline_n++];
            l->ax=(float)e0.sx; l->ay=(float)e0.sy; l->bx=(float)e1.sx; l->by=(float)e1.sy;
            l->rgb0 = col0 & 0xFFFFFF; l->rgb1 = col1 & 0xFFFFFF;
            l->cz = (float)e0.m3;
            l->blend = ((col0 >> 24) & 0x02) ? 1 : 0;
            l->sel = 4;
            if (streak_dbg()) streak_log("line4", pp, e0.sx, e0.sy, e1.sx, e1.sy);
        } else if (sel == 5) {                           /* HUD key-burst affine FT4 (block_80057750) */
            /* VALIDATED vs key.bk2 (key-collection HUD burst, level_11 f2435): 6/6
             * count-exact, FT4 NN ~0.9-1.1px mean / ~1.8px worst, 0 over-emission
             * (pure-sel-5 frames). Same affine sin/cos corner math as sel 2, but the
             * centre TR is the DIRECT view-space packing (no centre RTPS/swizzle):
             * TRX=(s16 A.lo)*4, TRZ=(s16 B.lo)*4, TRY=(int32)A>>14 (combined packing,
             * confirmed against block_80057750 ctc2 sequence). LOD cull is real here
             * (asm jumps to the loop tail), unlike sel-4 where it clamps to 0. */
            double TR0 = (double)((int32_t)(A << 16) >> 14);  /* X*4 */
            double TR1 = (double)((int32_t)A >> 14);          /* ~Y*4 (combined) */
            double TR2 = (double)((int32_t)(B << 16) >> 14);  /* Z*4 */
            if (TR2 <= 0.0) continue;
            int sz3i = (int)(TR2 > 65535 ? 65535 : TR2);
            int lod = (int)((cpu->read_word(pp + 0x10) >> 8) & 0xFF);
            if (((sz3i >> 7) - lod) < 0) continue;       /* sel-5 cull TRZ>>7 - lod */
            int sizeB = (int)((B >> 16) & 0xFF);
            int angIdx = (int)(((B >> 23) + 64) & 0x1FE);
            double a2, a3;                               /* corner extents (sub-pixel) */
            part_prev_t pv;                              /* sub-tick spin/size sweep */
            if (part_phase_prev(pp, &pv)) {
                double szf = lerp_b((int)((pv.B >> 16) & 0xFF), (int)((pv.cB >> 16) & 0xFF), pv.alpha);
                double ef  = lerp_wrap_b((int)(pv.B >> 24), (int)(pv.cB >> 24), pv.alpha) + 32.0;
                double sf, cf; ngte_sincos_f(cpu, ef, &sf, &cf);
                a2 = szf * sf / 1024.0; a3 = szf * cf / 1024.0;
            } else {
                int sinv = rd_s16(cpu, D_SIN_TABLE + (uint32_t)angIdx);
                int cosv = rd_s16(cpu, D_SIN_TABLE + (uint32_t)angIdx + 0x80);
                a2 = (double)sizeB * sinv / 1024.0; a3 = (double)sizeB * cosv / 1024.0;  /* float (not >>10): sub-pixel corner extent */
            }
            double Cc[4][2] = { {-a2,-a3}, {a3,-a2}, {-a3,a2}, {a2,a3} };  /* same as sel 2 */
            double sz3 = TR2 > 65535 ? 65535 : TR2;
            /* HUD burst: canonical projection, the cap regime on the same H */
            double qd = part_cap(sz3, Hc, Hc) ? NGTE_DIV_CAP : (double)Hc/sz3;
            for (int k = 0; k < 4; k++) {                /* diag(4096,2560,4096) + TR */
                double m1 = Cc[k][0] + TR0;
                double m2 = Cc[k][1]*2560.0/4096.0 + TR1;
                q.sx[k] = (float)(ofx + clamp16(m1)*qd);
                q.sy[k] = (float)(ofy + clamp16(m2)*qd);
                q.sz[k] = (float)TR2;
            }
            q.cz = (float)TR2;
            decode_material(cpu, pp, mclass, &q);
            s_fq[s_fq_n++] = q;
        }
    }
}

static void particle_compare(CPUState* cpu) {
    particle_build(cpu);
    int ft4 = 0; for (int p = 0; p < s_prim_n; p++) if (s_prim[p].code == 0x2C || s_prim[p].code == 0x2E) ft4++;
    int natft4 = 0; for (int q = 0; q < s_fq_n; q++) if (s_fq[q].sel != 0) natft4++;  /* sel 0 = non-FT4 */
    int n = natft4 < ft4 ? natft4 : ft4;
    double worst = 0, sum = 0; int nv = 0, matched = 0, over4 = 0, fqi = 0;
    for (int p = 0; p < s_prim_n && matched < n; p++) {
        prim_t* pr = &s_prim[p];
        if (!(pr->code == 0x2C || pr->code == 0x2E)) continue;
        while (fqi < s_fq_n && s_fq[fqi].sel == 0) fqi++;     /* skip sel-0 tile quads */
        if (fqi >= s_fq_n) break;
        fquad* q = &s_fq[fqi++];
        double qw = 0;
        for (int k = 0; k < 4; k++) {
            double dx = q->sx[k] - pr->x[k], dy = q->sy[k] - pr->y[k];
            double d = (dx<0?-dx:dx) + (dy<0?-dy:dy);
            sum += d; nv++; if (d > qw) qw = d;
        }
        if (qw > worst) worst = qw;
        if (qw > 4.0) over4++;
        matched++;
    }
    /* order-independent NN: each native FT4 quad -> nearest prim FT4 by centroid
     * (robust to a cull-boundary count mismatch that cascades the ordered A/B). */
    double nnsum = 0; int nnn = 0;
    for (int q = 0; q < s_fq_n; q++) {
        if (s_fq[q].sel == 0) continue;
        double qcx = (s_fq[q].sx[0]+s_fq[q].sx[1]+s_fq[q].sx[2]+s_fq[q].sx[3])/4.0;
        double qcy = (s_fq[q].sy[0]+s_fq[q].sy[1]+s_fq[q].sy[2]+s_fq[q].sy[3])/4.0;
        double best = 1e9;
        for (int p = 0; p < s_prim_n; p++) {
            if (!(s_prim[p].code==0x2C||s_prim[p].code==0x2E)) continue;
            double pcx=(s_prim[p].x[0]+s_prim[p].x[1]+s_prim[p].x[2]+s_prim[p].x[3])/4.0;
            double pcy=(s_prim[p].y[0]+s_prim[p].y[1]+s_prim[p].y[2]+s_prim[p].y[3])/4.0;
            double d=(qcx-pcx<0?pcx-qcx:qcx-pcx)+(qcy-pcy<0?pcy-qcy:qcy-pcy);
            if (d<best) best=d;
        }
        if (best<1e8) { nnsum+=best; nnn++; }
    }
    int ns2=0,ns3=0,ns5=0,ns6=0; for (int q=0;q<s_fq_n;q++){if(s_fq[q].sel==2)ns2++;else if(s_fq[q].sel==3)ns3++;else if(s_fq[q].sel==5)ns5++;else if(s_fq[q].sel>=6)ns6++;}
    int npl1=0,npl4=0; for (int t=0;t<s_pline_n;t++){if(s_pline[t].sel==4)npl4++;else npl1++;}
    if (getenv("OPENPETE_PARTICLE_DEBUG")) fprintf(stderr, "   [sel] nat sel2=%d sel3=%d sel5=%d sel6=%d line1=%d line4=%d\n", ns2, ns3, ns5, ns6, npl1, npl4);
    int nat_sel0 = 0; for (int q = 0; q < s_fq_n; q++) if (s_fq[q].sel == 0) nat_sel0++;
    /* sel-0 GEOMETRY (closes the count-only blind spot that hid the 1px-sliver
     * bug): the .s emits each sel-0 as a LINE_F (code 0x40), kept raw — vertex0
     * is raw[2] packed (Y<<16)|X. NN-match each native sel-0 quad anchor
     * (sx[0],sy[0]) to the nearest .s line vertex0 and report the distance. */
    int prim_line = 0; for (int p = 0; p < s_prim_n; p++) if (s_prim[p].code == 0x40) prim_line++;
    double t0sum = 0, t0worst = 0; int t0n = 0, t0fpmis = 0;
    for (int q = 0; q < s_fq_n; q++) {
        if (s_fq[q].sel != 0) continue;
        double ax = s_fq[q].sx[0], ay = s_fq[q].sy[0], best = 1e9; int bp = -1;
        for (int p = 0; p < s_prim_n; p++) {
            if (s_prim[p].code != 0x40) continue;
            double lx = (double)(int16_t)(s_prim[p].raw[2] & 0xFFFF);
            double ly = (double)(int16_t)(s_prim[p].raw[2] >> 16);
            double d = (ax-lx<0?lx-ax:ax-lx) + (ay-ly<0?ly-ay:ay-ly);
            if (d < best) { best = d; bp = p; }
        }
        if (bp >= 0) {
            t0sum += best; if (best > t0worst) t0worst = best; t0n++;
            /* FOOTPRINT check — the 1px-sliver bug was a WIDTH error at the
             * correct position, which a position NN alone misses. Compare the
             * native quad's covered-pixel extent to the .s line's: line covers
             * pixels v0..v1 inclusive = |Δ|+1; the native quad covers sx[1]-sx[0]
             * (built as +2 to match the 2px line). */
            int v0x=(int16_t)(s_prim[bp].raw[2]&0xFFFF), v1x=(int16_t)(s_prim[bp].raw[3]&0xFFFF);
            int v0y=(int16_t)(s_prim[bp].raw[2]>>16),    v1y=(int16_t)(s_prim[bp].raw[3]>>16);
            int line_w=(v1x>v0x?v1x-v0x:v0x-v1x)+1, line_h=(v1y>v0y?v1y-v0y:v0y-v1y)+1;
            int nat_w=(int)(s_fq[q].sx[1]-s_fq[q].sx[0]+0.5), nat_h=(int)(s_fq[q].sy[2]-s_fq[q].sy[0]+0.5);
            if (nat_w != line_w || nat_h != line_h) t0fpmis++;
        }
    }
    /* sel-1 gouraud LINE_G (code 0x50) A/B: NN native streak endpoints -> nearest
     * .s 0x50 line endpoint (raw[2]=xy0, raw[4]=xy1, packed (Y<<16)|X). */
    int prim_gline = 0; for (int p = 0; p < s_prim_n; p++) if (s_prim[p].code == 0x50) prim_gline++;
    double l1sum = 0, l1worst = 0; int l1n = 0;
    for (int t = 0; t < s_pline_n; t++) {
        double e[2][2] = { {s_pline[t].ax, s_pline[t].ay}, {s_pline[t].bx, s_pline[t].by} };
        for (int k = 0; k < 2; k++) {
            double best = 1e9;
            for (int p = 0; p < s_prim_n; p++) {
                if (s_prim[p].code != 0x50) continue;
                int idx[2] = {2, 4};
                for (int m = 0; m < 2; m++) {
                    double lx = (double)(int16_t)(s_prim[p].raw[idx[m]] & 0xFFFF);
                    double ly = (double)(int16_t)(s_prim[p].raw[idx[m]] >> 16);
                    double d = (e[k][0]-lx<0?lx-e[k][0]:e[k][0]-lx) + (e[k][1]-ly<0?ly-e[k][1]:e[k][1]-ly);
                    if (d < best) best = d;
                }
            }
            if (best < 1e8) { l1sum += best; if (best > l1worst) l1worst = best; l1n++; }
        }
    }
    fprintf(stderr, "[part-ab f=%u] FT4 native=%d prim=%d cmp=%d mean|dSXY|=%.2f worst=%.1fpx over4=%d/%d nnCtr=%.1f  sel0:nat=%d line=%d nn=%.1f/%.1f fpMis=%d  sel1:nat=%d gline=%d nn=%.1f/%.1f  sel4:nat=%d sel5:nat=%d\n",
            openpete_frame, natft4, ft4, n, nv?sum/nv:0, worst, over4, n, nnn?nnsum/nnn:0,
            nat_sel0, prim_line, t0n?t0sum/t0n:0, t0worst, t0fpmis,
            s_pline_n, prim_gline, l1n?l1sum/l1n:0, l1worst, npl4, ns5);
    if (getenv("OPENPETE_PARTICLE_DEBUG")) {
        for (int p = 0, c = 0; p < s_prim_n && c < 3 && c < s_fq_n; p++) {
            if (!(s_prim[p].code==0x2C||s_prim[p].code==0x2E)) continue;
            fprintf(stderr, "   sel%d prim:(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)  nat:(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)\n",
                    s_fq[c].sel,
                    s_prim[p].x[0],s_prim[p].y[0],s_prim[p].x[1],s_prim[p].y[1],
                    s_prim[p].x[2],s_prim[p].y[2],s_prim[p].x[3],s_prim[p].y[3],
                    s_fq[c].sx[0],s_fq[c].sy[0],s_fq[c].sx[1],s_fq[c].sy[1],
                    s_fq[c].sx[2],s_fq[c].sy[2],s_fq[c].sx[3],s_fq[c].sy[3]);
            c++;
        }
    }
}

/* ---- native feed ------------------------------------------------------- */
/** @brief Builds the g_Particles pool walk and feeds its quads and lines into the native scene.
 *
 * Lines (sel 1 world streaks, sel 4 HUD egg bursts) become two-triangle
 * ribbons 1 px wide, gouraud along the line, untextured. Quads (sel 0
 * tiles, sel 2, 3 and 5 billboards) become two triangles with the decoded
 * material and blend. Each primitive takes one depth, its centre or first
 * endpoint view-Z, because particles are small billboards. Every triangle
 * carries its OPENPETE_TAG_PARTICLE tag. Glows and sparkles feed through
 * their own functions. Display-only: the .s already ran and wrote every
 * verdict byte. OPENPETE_PARTICLE_MATDBG=<n> prints each quad's material for
 * three openpete_frame values starting at n.
 * @param cpu The guest CPU state. */
static void particle_feed_native(CPUState* cpu) {
    particle_build(cpu);
    if (s_fq_n <= 0 && s_pline_n <= 0) return;
    openpete_native_scene_begin_frame(openpete_frame);   /* append after world + actors */
    int split[2][3] = { {0,1,2}, {1,3,2} };              /* FT4 -> 2 tris */
    /* sel-1 gouraud streak lines -> thin (1px-perp) gouraud quads */
    for (int i = 0; i < s_pline_n; i++) {
        pline* l = &s_pline[i];
        openpete_native_scene_set_tag(
            OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_LINE, l->sel, 0));
        float dx = l->bx - l->ax, dy = l->by - l->ay;
        float len = sqrtf(dx*dx + dy*dy);
        float px = 0.5f, py = 0.0f;
        if (len > 1e-3f) { px = -dy/len*0.5f; py = dx/len*0.5f; }
        float cx[4] = { l->ax-px, l->ax+px, l->bx-px, l->bx+px };
        float cy[4] = { l->ay-py, l->ay+py, l->by-py, l->by+py };
        uint32_t cc[4] = { l->rgb0, l->rgb0, l->rgb1, l->rgb1 };  /* gouraud along the line */
        float depth = openpete_native_scene_viewz(openpete_worldvz_full(l->cz));
        for (int s = 0; s < 2; s++) {
            openpete_scene_tri st = {0};
            for (int k = 0; k < 3; k++) {
                int j = split[s][k];
                st.v[k].sx = cx[j]; st.v[k].sy = cy[j]; st.v[k].z = depth; st.v[k].pvz = depth;
                st.v[k].u = 0.0f; st.v[k].v = 0.0f;
                st.v[k].rgba = (cc[j] & 0xFFFFFFu) | 0xFF000000u;
            }
            st.textured = 0;
            st.tpage_x = st.tpage_y = st.clut_x = st.clut_y = st.bpp = 0;
            st.blend = l->blend;
            openpete_native_scene_add_tri(&st, OPENPETE_SCENE_WORLD, 0);
        }
    }
    openpete_native_scene_set_tag(0);
    if (s_fq_n <= 0) return;
    /* OPENPETE_PARTICLE_MATDBG=<frame>: one line per emitted quad — sel/blend/
     * rgb/texture — to attribute a missing-or-wrong-blend particle (e.g. the
     * dragon-fragment smoke puffs) to its material decode. */
    { static long s_mdbg = -2;
      if (s_mdbg < -1) { const char* e = getenv("OPENPETE_PARTICLE_MATDBG");
                         s_mdbg = (e && *e) ? atol(e) : -1; }
      if (s_mdbg >= 0 && openpete_frame >= (unsigned)s_mdbg && openpete_frame < (unsigned)s_mdbg + 3) {
          for (int i = 0; i < s_fq_n; i++) {
              fquad* q = &s_fq[i];
              fprintf(stderr, "[part-mat f=%u] sel=%d blend=%d tex=%d rgb=%06X "
                      "tp=(%d,%d) clut=(%d,%d) bpp=%d ctr=(%.0f,%.0f) cz=%.0f\n",
                      openpete_frame, q->sel, q->blend, q->textured, q->rgb,
                      q->tpx, q->tpy, q->clx, q->cly, q->bpp,
                      (q->sx[0]+q->sx[1]+q->sx[2]+q->sx[3])/4.0,
                      (q->sy[0]+q->sy[1]+q->sy[2]+q->sy[3])/4.0, q->cz);
              if (q->blend == 3)
                  fprintf(stderr, "   corners (%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f) uv (%d,%d)(%d,%d)(%d,%d)(%d,%d)\n",
                          q->sx[0],q->sy[0],q->sx[1],q->sy[1],q->sx[2],q->sy[2],q->sx[3],q->sy[3],
                          q->u[0],q->v[0],q->u[1],q->v[1],q->u[2],q->v[2],q->u[3],q->v[3]);
          }
      } }
    for (int i = 0; i < s_fq_n; i++) {
        fquad* q = &s_fq[i];
        openpete_native_scene_set_tag(
            OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_QUAD, q->sel, 0));
        /* (A depth pull for the subtractive smoke was tried here and
         * FALSIFIED: the burst point is the occluder's NEAREST
         * surface from the camera, so true depth never blocked the puffs —
         * the eater was the SUBTRACT-WORLD draw ORDER, fixed renderer-side
         * by drawing that bucket after the ADD group.) */
        float depth = openpete_native_scene_viewz(openpete_worldvz_full(q->cz));
        for (int s = 0; s < 2; s++) {
            openpete_scene_tri st = {0};
            for (int k = 0; k < 3; k++) {
                int j = split[s][k];
                st.v[k].sx = q->sx[j]; st.v[k].sy = q->sy[j]; st.v[k].z = depth; st.v[k].pvz = depth;
                st.v[k].u = (float)q->u[j]; st.v[k].v = (float)q->v[j];
                st.v[k].rgba = (q->rgb & 0xFFFFFFu) | 0xFF000000u;
            }
            st.textured = q->textured;
            st.tpage_x = q->tpx; st.tpage_y = q->tpy;
            st.clut_x = q->clx; st.clut_y = q->cly; st.bpp = q->bpp;
            st.blend = q->blend;
            openpete_native_scene_add_tri(&st, OPENPETE_SCENE_WORLD, 0);
        }
    }
    openpete_native_scene_set_tag(0);
}

/* ---- glow rings (func_800580F4) — native float walk + feed + A/B ----------- *
 * Decoded from the recompiled C (generated/PSX.EXE_full.c block_800580F4). Each
 * active glow (D_80078800[16], stride 0x24) is a gouraud TRIANGLE FAN: a bright
 * centre (the glow colour) fading to BLACK at a perspective-scaled ring, drawn
 * ADDITIVE (the .s emits a 0xE1000220 draw-mode packet = ABR mode 1, then N-1
 * G3-STP tris, code 0x32). The ring template (unk_0x04, default D_8006E308) is a
 * static 17-point ellipse (96 wide x 60 tall — pre-compensated for tall PSX
 * pixels, so ~circular on the 4:3 display); unk_0x00 is the ring point count.
 *
 * Differences from the particle/sparkle swizzle: the glow uses RAW camera world
 * coords (NO >>2 — MobyPos is full-scale world units), and a per-glow LOD shift
 * that cancels out of SX/SY and persize, so the float walk skips it (the int16
 * floor it introduces is exactly what we're improving on). All gamestate-derived
 * (pool + static ring template + colour); determinism-safe by construction (the
 * .s ran headless; glows write no verdict byte). */
typedef struct {
    float sx[3], sy[3];      /* fan tri: v0=centre, v1=prev ring, v2=this ring */
    uint32_t rgb[3];         /* v0=glow colour, v1/v2=black (the fade) */
    float cz;                /* centre view-Z (depth) */
} gtri;
#define MAX_GTRI 4096
static gtri s_gtri[MAX_GTRI]; static int s_gtri_n;

static void glow_build(CPUState* cpu) {
    s_gtri_n = 0;
    ngte_view_build(cpu, &s_pv);
    const ngte_view* v = &s_pv;
    int R[3][3]; memcpy(R, v->cam_M, sizeof R);
    /* RAW world units (no >>2); fractional interpolated camera when bracketed
     * (sub-tick floor fix), else floored RAM. */
    double camX = v->cam[0], camY = v->cam[1], camZ = v->cam[2];
    double ofx = v->ofx, ofy = v->ofy;

    int gdbg_done = 0;                                /* one glow-phase line per walk */
    for (int i = 0; i < 16 && s_gtri_n < MAX_GTRI; i++) {
        uint32_t g = D_GLOWS + (uint32_t)i*0x24;
        int count = (int)cpu->read_word(g + 0x00);    /* unk_0x00 = ring point count */
        if (count <= 0) continue;                     /* inactive slot */
        uint32_t mpp = cpu->read_word(g + 0x08);      /* MobyPos* */
        if (!mpp) continue;
        uint32_t tmpl = cpu->read_word(g + 0x04);     /* ring template (int32 x,y pairs) */
        if (!tmpl) continue;
        int offx = (int)cpu->read_word(g + 0x14);     /* PosOffset (animatable) */
        int offy = (int)cpu->read_word(g + 0x18);
        int offz = (int)cpu->read_word(g + 0x1C);
        int px = (int)cpu->read_word(mpp+0);          /* owner pos (rides the moby bracket) */
        int py = (int)cpu->read_word(mpp+4);
        int pz = (int)cpu->read_word(mpp+8);
        int size = (int)cpu->read_word(g + 0x10);
        int bias = (int)cpu->read_word(g + 0x20);
        uint32_t color = cpu->read_word(g + 0x0C) & 0xFFFFFF;

        /* Sub-tick phase: lerp size/colour/offset between the slot's prev/curr
         * interp-objs snapshots (captured in interp_regions.c). The Sparx
         * aura RAMPS size +32/tick over ~18 ticks on spawn (censused) and
         * stepped at 29.913 Hz. Guards: same owner (+0x08),
         * template (+0x04) and ring count — a slot reused for a different glow
         * snaps. Colour is additive light, so a per-byte linear lerp is safe. */
        double sizef = (double)size;
        double offxf = (double)offx, offyf = (double)offy, offzf = (double)offz;
        uint32_t colorL = color;
        if (s_phase_now) {
            const openpete_interp_obj_t* o = openpete_interp_objs_find(g);
            if (o && o->have_prev) {
                uint32_t pcnt, ptmpl, pmpp;
                memcpy(&pcnt,  o->prev + 0x00, 4);
                memcpy(&ptmpl, o->prev + 0x04, 4);
                memcpy(&pmpp,  o->prev + 0x08, 4);
                op_time_t per = o->curr_ns - o->prev_ns;
                if (per > PART_TICK_PERIOD_NS) per = PART_TICK_PERIOD_NS;
                uint32_t ccnt2, ctmpl2, cmpp2;
                memcpy(&ccnt2,  o->curr + 0x00, 4);
                memcpy(&ctmpl2, o->curr + 0x04, 4);
                memcpy(&cmpp2,  o->curr + 0x08, 4);
                /* Guards + forward endpoints from the TABLE pair (see the
                 * sparkle note): prev→table-curr, never prev→live. */
                if (pcnt == ccnt2 && (int32_t)ccnt2 > 0 && ptmpl == ctmpl2 &&
                    pmpp == cmpp2 && per > 0) {
                    double a = (double)(s_phase_now - o->curr_ns) / (double)per;
                    if (a < PART_ALPHA_MIN) a = PART_ALPHA_MIN; else if (a > 1.0) a = 1.0;
                    uint32_t pcol, psz, po14, po18, po1c;
                    uint32_t ccol, csz, co14, co18, co1c;
                    memcpy(&pcol, o->prev + 0x0C, 4);
                    memcpy(&psz,  o->prev + 0x10, 4);
                    memcpy(&po14, o->prev + 0x14, 4);
                    memcpy(&po18, o->prev + 0x18, 4);
                    memcpy(&po1c, o->prev + 0x1C, 4);
                    memcpy(&ccol, o->curr + 0x0C, 4);
                    memcpy(&csz,  o->curr + 0x10, 4);
                    memcpy(&co14, o->curr + 0x14, 4);
                    memcpy(&co18, o->curr + 0x18, 4);
                    memcpy(&co1c, o->curr + 0x1C, 4);
                    sizef = (double)(int32_t)psz  + ((double)(int32_t)csz  - (double)(int32_t)psz)  * a;
                    if (sizef < 0.0) sizef = 0.0;   /* backward extrapolation on a spawn ramp */
                    offxf = (double)(int32_t)po14 + ((double)(int32_t)co14 - (double)(int32_t)po14) * a;
                    offyf = (double)(int32_t)po18 + ((double)(int32_t)co18 - (double)(int32_t)po18) * a;
                    offzf = (double)(int32_t)po1c + ((double)(int32_t)co1c - (double)(int32_t)po1c) * a;
                    double cr = lerp_b((int)(pcol & 0xFF),         (int)(ccol & 0xFF),         a);
                    double cg = lerp_b((int)((pcol >> 8) & 0xFF),  (int)((ccol >> 8) & 0xFF),  a);
                    double cb = lerp_b((int)((pcol >> 16) & 0xFF), (int)((ccol >> 16) & 0xFF), a);
                    colorL = (uint32_t)(cr + 0.5) | ((uint32_t)(cg + 0.5) << 8)
                           | ((uint32_t)(cb + 0.5) << 16);
                    if (glow_anim_dbg() && !gdbg_done) { gdbg_done = 1;
                        fprintf(stderr, "[glow-phase f=%u] slot=%d lerp size=%.2f col=%06X (raw %d/%06X)\n",
                                openpete_frame, i, sizef, colorL, size, color); }
                }
            }
        }

        /* OPENPETE_GLOW_ANIM_DBG: census — does the game ANIMATE glow records
         * per tick (size/colour/offset pulse)? Logs RAW values on CHANGE only;
         * a change stream at ~tick rate = a stepping channel (the Sparx
         * aura size ramps +32/tick on spawn — hence the phase lerp
         * above). */
        if (glow_anim_dbg()) {
            static uint32_t s_glast[16][6]; static int s_gseen[16];
            uint32_t cur[6] = { (uint32_t)count, (uint32_t)size, color,
                                (uint32_t)offx, (uint32_t)offy, (uint32_t)offz };
            if (!s_gseen[i] || memcmp(cur, s_glast[i], sizeof cur) != 0) {
                fprintf(stderr, "[glow-anim f=%u] slot=%d cnt=%u size=%u col=%06X off=(%d,%d,%d)%s\n",
                        openpete_frame, i, cur[0], cur[1], cur[2],
                        (int32_t)cur[3], (int32_t)cur[4], (int32_t)cur[5],
                        s_gseen[i] ? "" : " (new)");
                memcpy(s_glast[i], cur, sizeof cur); s_gseen[i] = 1;
            }
        }

        /* swizzle like particles but in raw world units: GTE-X=camY-py,
         * GTE-Y=camZ-pz, GTE-Z=px-camX; TR=0 through the camera matrix. */
        projres c; proj2(R, ofx, ofy, v, camY - ((double)py + offyf),
                         camZ - ((double)pz + offzf), ((double)px + offxf) - camX, &c);
        double mac3 = c.m3;
        if (mac3 <= 0.0) continue;                    /* behind camera */
        double depthidx = mac3 / 128.0 + bias;        /* the .s OT-depth cull (a3<=0) */
        if (depthidx <= 0.0) continue;
        double persize = sizef * 4096.0 / mac3;       /* IR0 = size*4096/Z */
        /* GTE IR0 saturates at 0x1000 on hardware — without the clamp a
         * near-camera glow (mac3 → small) spikes its ring across the
         * screen where the PSX caps it at template scale. Runahead makes
         * the near pass MORE common: the committed camera trails the led
         * Sparx by N extra ticks, so he crosses the camera plane closer. */
        if (persize > 4096.0) persize = 4096.0;

        int nc = count > 64 ? 64 : count;
        double rx[64], ry[64];
        for (int j = 0; j < nc; j++) {
            int tx = (int)cpu->read_word(tmpl + (uint32_t)j*8 + 0);  /* template offset (int32) */
            int ty = (int)cpu->read_word(tmpl + (uint32_t)j*8 + 4);
            rx[j] = c.sx + persize * tx / 4096.0;     /* GPF: centre + (IR0*tmpl)>>12 */
            ry[j] = c.sy + persize * ty / 4096.0;
        }
        for (int j = 1; j < nc && s_gtri_n < MAX_GTRI; j++) {
            gtri* t = &s_gtri[s_gtri_n++];
            t->sx[0]=(float)c.sx;   t->sy[0]=(float)c.sy;   t->rgb[0]=colorL;  /* centre */
            t->sx[1]=(float)rx[j-1];t->sy[1]=(float)ry[j-1];t->rgb[1]=0;       /* prev ring */
            t->sx[2]=(float)rx[j];  t->sy[2]=(float)ry[j];  t->rgb[2]=0;       /* this ring */
            t->cz = (float)mac3;
            if (streak_dbg()) streak_log("glow", g, c.sx, c.sy, rx[j], ry[j]);
        }
    }
}

/** @brief Builds the glow ring fans and feeds them into the native scene as additive gouraud triangles.
 *
 * Untextured, blend 2 (ABR mode 1). Display-only: the .s already ran and
 * wrote every verdict byte.
 *
 * Each triangle carries koff = centre MAC3 + 0.5, the fan's ordering-table
 * key in world units ((MAC3 >> 7) + rec[+0x20] in r_particles.s:988-990,
 * the field being 0), and the OPENPETE_SCENE_RANK_AURA sentinel, which puts
 * its fragments on the key contest's aura arm. The fan is ordered against
 * the bodies it overlaps by the table's own slots (the owner's bucket minus
 * m_DepthOffset, 4 on Sparx, the key at 8001F810..F828 and the offset
 * packed at 8001F464..F46C; the avatar's bucket minus sortDepth) while its
 * stored depth stays the true view-Z, so terrain still occludes it by
 * geometry. One stored depth cannot do both: a fan pushed behind its owner
 * in world units sinks into the ground the table draws under it.
 * @param cpu The guest CPU state. */
static void glow_feed_native(CPUState* cpu) {
    glow_build(cpu);
    if (s_gtri_n <= 0) return;
    openpete_native_scene_begin_frame(openpete_frame);
    openpete_native_scene_set_tag(
        OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_GLOW, 0, 0));
    for (int i = 0; i < s_gtri_n; i++) {
        gtri* t = &s_gtri[i];
        float depth = openpete_native_scene_viewz(openpete_worldvz_full(t->cz));
        openpete_scene_tri st = {0};
        for (int k = 0; k < 3; k++) {
            st.v[k].sx = t->sx[k]; st.v[k].sy = t->sy[k]; st.v[k].z = depth; st.v[k].pvz = depth;
            st.v[k].u = 0.0f; st.v[k].v = 0.0f;
            st.v[k].rgba = (t->rgb[k] & 0xFFFFFFu) | 0xFF000000u;
        }
        st.textured = 0;
        st.tpage_x = st.tpage_y = st.clut_x = st.clut_y = st.bpp = 0;
        st.blend = 2;                                 /* ABR mode 1 (additive) */
        st.koff = t->cz + 0.5f;                       /* the link key + sentinel */
        st.rank = OPENPETE_SCENE_RANK_AURA;
        openpete_native_scene_add_tri(&st, OPENPETE_SCENE_WORLD, 0);
    }
    openpete_native_scene_set_tag(0);
}

/** @brief Compares the native glow fan against the renderer's captured G3 semi-transparent prims (code 0x32).
 *
 * Rebuilds the fan, then matches every native vertex to its nearest .s G3
 * vertex by Manhattan distance, independent of order, which checks both the
 * centre projection and the ring corners. Prints one [glow-ab] line with
 * the triangle and prim counts, the mean and the worst distance.
 * @param cpu The guest CPU state. */
static void glow_compare(CPUState* cpu) {
    glow_build(cpu);
    int prim_g3 = 0; for (int p = 0; p < s_prim_n; p++) if (s_prim[p].code == 0x32) prim_g3++;
    double sum = 0, worst = 0; int nn = 0;
    for (int t = 0; t < s_gtri_n; t++) for (int k = 0; k < 3; k++) {
        double ax = s_gtri[t].sx[k], ay = s_gtri[t].sy[k], best = 1e9;
        for (int p = 0; p < s_prim_n; p++) {
            if (s_prim[p].code != 0x32) continue;
            for (int m = 0; m < 3; m++) {
                double dx = ax - s_prim[p].x[m], dy = ay - s_prim[p].y[m];
                double d = (dx<0?-dx:dx) + (dy<0?-dy:dy);
                if (d < best) best = d;
            }
        }
        if (best < 1e8) { sum += best; if (best > worst) worst = best; nn++; }
    }
    fprintf(stderr, "[glow-ab f=%u] native_tri=%d prim_g3=%d nn=%.2f worst=%.1fpx\n",
            openpete_frame, s_gtri_n, prim_g3, nn?sum/nn:0, worst);
}

/* ---- sparkle twinkles (func_800584C4) — native float walk + feed + A/B ----- *
 * Decoded from the recompiled C (block_800584C4). Each live sparkle (g_Sparkles[8],
 * stride 0x18) is a "twinkle cross" = TWO LINE_F prims (code 0x40, opaque white):
 * T0→T3 and T1→T2, the two diagonals. Tips (view space, VZ=0):
 *   T0=(-ec,-es) T1=(es,-ec) T2=(-es,ec) T3=(ec,es),
 *   ec=(scaledFade·cos[fade8])>>19, es=(scaledFade·sin[fade8])>>19,
 *   scaledFade=(256−fade8)·m_Fade, fade8=|m_Speed−newLife|·256/m_Speed.
 * They are transformed by a uniform matrix diag(a3,a3,a3), a3=max(centreMAC3,4096),
 * TR = centre·2 (the centre's view-space MAC), then projected.
 *
 * VERDICT-SAFE by construction: the `.s` ran headless (realfn) — it decremented
 * m_Life and killed off-screen/expired sparkles by writing m_Life=0. So we read
 * the pool AFTER the `.s` and **render exactly the sparkles with post-`.s`
 * m_Life>0** (which is `newLife` — the value the `.s` used for the fade), no
 * separate cull needed. The native lines are emitted as thin quads (the scene is
 * triangles-only), like the sel-0 line footprint. */
typedef struct {
    float ax, ay, bx, by;    /* line endpoints (centerline) */
    float cz;                /* centre view-Z (depth) */
    uint32_t rgb;            /* m_Color (flat) */
} sline;
#define MAX_SLINE 64
static sline s_sline[MAX_SLINE]; static int s_sline_n;

/* Sparkle STYLE channel: the pad byte after m_Color's rgb (+0x13) is
 * written by nothing in the game — a mod that hooks the spawner
 * (SpawnMobySparkle) can stamp a style id there per sparkle and the
 * NATIVE renderer restyles it (the PSX path still draws the stock
 * cross, same posture as mod textures). Byte layout: low nibble =
 * style, high nibble = hue index+1 (0 = no hue). Styles:
 * 0 = stock twinkle cross;
 * 1 = 4-point star flare (the Spyro 3 gem-glint look): filled concave
 * star through the SAME centre/pulse/spin pipeline, additive blend.
 * With no hue: white-hot core fading to m_Color at the tips (legacy).
 * With a hue: the full S3 two-mode glint (traced from SCUS-94467
 * func_8002F098) — S3 does NOT lerp; it switches prim style at
 * viewZ 0x4000: far = the plain cross, pale-tinted (word0, opaque);
 * near = the additive gouraud star, pale core (word0) -> deep hue at
 * the spike tips (word1) -> near-black at the waists (word2, S3's
 * ring verts). */
typedef struct {
    float sx[9], sy[9];      /* centre + 8 outline points (tip/waist x4) */
    float cz;
    uint32_t rgb;
    int hue;                 /* 0 = legacy white flare; 1..6 = k_s3_glint row+1 */
} sstar;
#define MAX_SSTAR 16
static sstar s_sstar[MAX_SSTAR]; static int s_sstar_n;

/* S3 gem-glint colour table, D_80066B64 in SCUS-94467 (bytes verified
 * against the ROM): per hue {centre + far-line, spike tip, ring}.
 * Packed 0xBBGGRR to match the sparkle m_Color/rgba convention. */
#define S3G(r, g, b) ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16))
static const uint32_t k_s3_glint[6][3] = {
    { S3G(224,160,128), S3G(64,16, 0), S3G(16, 0, 0) },  /* red   (1)  */
    { S3G(160,224,160), S3G( 0,64, 0), S3G( 0,16, 0) },  /* green (2)  */
    { S3G(160,128,224), S3G(32, 0,64), S3G( 8, 0,16) },  /* blue  (5)  */
    { S3G(208,208,128), S3G(48,48, 0), S3G(16,16, 0) },  /* gold  (10) */
    { S3G(224,128,192), S3G(64, 0,32), S3G(32, 0,16) },  /* pink  (25) */
    { S3G(128,128,128), S3G( 0, 0, 0), S3G( 0, 0, 0) },  /* misc/grey  */
};
/* S3 near/far switch: viewZ < 0x4000 world units -> star; c.m3 is
 * view-Z in world/4, so the threshold is 0x1000 in c.m3 units. */
#define S3_GLINT_NEAR_M3 4096.0

/** @brief Walks g_Sparkles and builds each live sparkle's twinkle cross lines or style-1 star.
 *
 * Reads the pool after the .s ran, so a slot with m_Life > 0 is exactly a
 * sparkle the .s drew. A cross is two lines (T0 to T3, T1 to T2) into
 * s_sline; a style-1 sparkle (low nibble of the pad byte +0x13) nearer
 * than S3_GLINT_NEAR_M3, or with no hue, is an 8-point star into s_sstar.
 *
 * A sparkle's position is static for its life; its animation phase steps
 * per tick: m_Life (pulse scale) decrements and m_0x0E (spin) advances.
 * While s_phase_now is set (the re-extract pass), life and spin are
 * interpolated between the slot's interp-objs prev and curr snapshots at
 * the slot's own alpha, and the fade math runs in float (fade8,
 * scaledFade, ngte_sincos_f); a respawn, a moved position or a changed
 * m_Speed snaps. With the latch clear the canonical inline build stays
 * integer and byte-exact for the sparkle A/B. Sparkle RAM is never
 * written.
 * @param cpu The guest CPU state. */
static void sparkle_build(CPUState* cpu) {
    s_sline_n = 0;
    s_sstar_n = 0;
    ngte_view_build(cpu, &s_pv);
    const ngte_view* v = &s_pv;
    int R[3][3]; memcpy(R, v->cam_M, sizeof R);
    /* RAW (the .s shifts the diff >>2); fractional interpolated camera when
     * bracketed (sub-tick floor fix), else floored RAM. */
    double camX = v->cam[0], camY = v->cam[1], camZ = v->cam[2];
    double ofx = v->ofx, ofy = v->ofy;
    double H   = v->H_eff;
    const double Hc = v->H_canon, qcap = NGTE_DIV_CAP * v->s;  /* GTE limit regime on the GTE's H; capped quotient scales with the view */

    for (int i = 0; i < 8 && s_sline_n + 2 <= MAX_SLINE; i++) {
        uint32_t sp = G_SPARKLES + (uint32_t)i*0x18;
        uint32_t w3 = cpu->read_word(sp + 0x0C);
        int newLife = (int)(w3 & 0xFF);              /* post-.s m_Life (== newLife) */
        if (newLife <= 0) continue;                  /* killed/expired by the .s → skip */
        int speed = (int)((w3 >> 8) & 0xFF);
        if (speed <= 0) continue;
        uint32_t w5 = cpu->read_word(sp + 0x14);
        int fade = (int)(w5 & 0xFF);                 /* m_Fade (+0x14) */
        uint32_t color = cpu->read_word(sp + 0x10) & 0xFFFFFF;
        int px = (int)cpu->read_word(sp + 0), py = (int)cpu->read_word(sp + 4), pz = (int)cpu->read_word(sp + 8);
        /* centre RTPS: swizzle (camera-relative >>2), TR=0, camera matrix */
        double VXg = (double)(camY - py) / 4.0;      /* GTE-X (÷4 fractional, not >>2) */
        double VYg = (double)(camZ - pz) / 4.0;      /* GTE-Y */
        double VZg = (double)(px - camX) / 4.0;      /* GTE-Z */
        projres c; proj2(R, ofx, ofy, v, VXg, VYg, VZg, &c);
        if (c.m3 <= 0.0) continue;

        /* SCALE: fade8 = |m_Speed − newLife|·256/m_Speed → scaledFade pulses the
         * twinkle. ANGLE: indexed by the animated m_0x0E seed (+0xE), which the
         * .s increments ~3·dt/frame for a smooth spin (NOT fade8 — that only
         * scales). Read post-.s m_0x0E (the value the .s used + wrote). */
        int angIdx = (int)((w3 >> 16) & 0xFF);       /* m_0x0E (spin angle) */
        /* Sub-tick phase: lerp life + spin between this slot's prev/curr
         * interp-objs snapshots (see s_sparkle_now above). Guards: same slot
         * identity (position + m_Speed unchanged) and life non-increasing —
         * a respawn re-arms m_Life upward → snap, never lerp across it. */
        double lifef = (double)newLife, angf = (double)angIdx;
        int phase_interp = 0;
        if (s_phase_now) {
            const openpete_interp_obj_t* o = openpete_interp_objs_find(sp);
            if (o && o->have_prev) {
                /* Endpoints + identity guards from the TABLE pair (prev AND
                 * curr) — live RAM is the committed tick under runahead
                 * (≈ table-prev): lerping prev→live degenerated the twinkle
                 * back to 30 Hz, and prev-vs-LIVE-pos identity would
                 * false-snap at N>=2. */
                uint32_t pw3, pp0, pp1, pp2, cw3, cp0, cp1, cp2;
                memcpy(&pw3, o->prev + 0x0C, 4);
                memcpy(&pp0, o->prev + 0x00, 4);
                memcpy(&pp1, o->prev + 0x04, 4);
                memcpy(&pp2, o->prev + 0x08, 4);
                memcpy(&cw3, o->curr + 0x0C, 4);
                memcpy(&cp0, o->curr + 0x00, 4);
                memcpy(&cp1, o->curr + 0x04, 4);
                memcpy(&cp2, o->curr + 0x08, 4);
                int pLife = (int)(pw3 & 0xFF), pSpeed = (int)((pw3 >> 8) & 0xFF);
                int pAng  = (int)((pw3 >> 16) & 0xFF);
                int cLife = (int)(cw3 & 0xFF), cSpeed = (int)((cw3 >> 8) & 0xFF);
                int cAng  = (int)((cw3 >> 16) & 0xFF);
                op_time_t per = o->curr_ns - o->prev_ns;
                if (per > PART_TICK_PERIOD_NS) per = PART_TICK_PERIOD_NS;
                if (pLife >= cLife && cLife > 0 && pSpeed == cSpeed && per > 0 &&
                    pp0 == cp0 && pp1 == cp1 && pp2 == cp2) {
                    double a = (double)(s_phase_now - o->curr_ns) / (double)per;
                    if (a < PART_ALPHA_MIN) a = PART_ALPHA_MIN; else if (a > 1.0) a = 1.0;
                    lifef = (double)pLife + ((double)cLife - (double)pLife) * a;
                    angf = lerp_wrap_b(pAng, cAng, a);
                    phase_interp = 1;
                }
            }
        }
        /* OPENPETE_SPARKLE_INTERP_DBG: per-present phase trace for slot 0 —
         * fractional lifef/angf between the byte grid prove the sub-tick sweep. */
        {
            static int s_sdbg = -1;
            if (s_sdbg < 0) { const char* e = getenv("OPENPETE_SPARKLE_INTERP_DBG"); s_sdbg = (e && *e && *e != '0'); }
            if (s_sdbg && i == 0)
                fprintf(stderr, "[sparkle-phase f=%u] slot0 %s life=%.3f ang=%.3f (raw %d/%d)\n",
                        openpete_frame, phase_interp ? "lerp" : "snap", lifef, angf, newLife, angIdx);
        }
        double scaledFade, sinv, cosv;
        if (phase_interp) {
            /* Float mirror of the integer math below (identical at alpha=1):
             * fractional fade8 + sin/cos lerped between adjacent table entries
             * (exact at integer angles), so the pulse/spin sweep sub-tick. */
            double dff = (double)speed - lifef; if (dff < 0.0) dff = -dff;
            double fade8f = dff * 256.0 / (double)speed;
            if (fade8f < 0.0) fade8f = 0.0; else if (fade8f > 255.0) fade8f = 255.0;
            scaledFade = (256.0 - fade8f) * (double)fade;
            ngte_sincos_f(cpu, angf, &sinv, &cosv);
        } else {
            /* canonical integer path — byte-exact for the A/B harness */
            int df = speed - newLife; if (df < 0) df = -df;
            int fade8 = speed ? (df * 256) / speed : 0;
            if (fade8 < 0) fade8 = 0; else if (fade8 > 255) fade8 = 255;
            scaledFade = (double)(256 - fade8) * fade;
            sinv = rd_s16(cpu, D_SIN_TABLE + (uint32_t)angIdx*2);
            cosv = rd_s16(cpu, D_SIN_TABLE + (uint32_t)angIdx*2 + 128);  /* cos = +0x80 */
        }
        double ec = scaledFade * cosv / 524288.0;    /* >>19 */
        double es = scaledFade * sinv / 524288.0;
        /* tips view-space (VZ=0); transform diag(a3)+TR=2*centre, then project */
        double a3 = c.m3 > 4096.0 ? c.m3 : 4096.0;
        double TR0 = c.m1*2.0, TR1 = c.m2*2.0, TR2 = c.m3*2.0;
        double sz3 = TR2 < 0 ? 0 : (TR2 > 65535 ? 65535 : TR2);
        double qd = part_cap(sz3, Hc, H) ? qcap : (double)H/sz3;
        int styleByte = (int)(cpu->read_word(sp + 0x10) >> 24);  /* pad byte +0x13 */
        int style = styleByte & 0x0F;
        int hue = (styleByte >> 4) & 0x0F;                   /* 1..6 = k_s3_glint row+1 */
        if (hue > 6) hue = 6;
        /* S3 two-mode switch: a hued glint beyond the near threshold is
         * drawn as the plain cross. S3's far path colours its lines
         * with word0 (asm: t2+0x06000000 at 0x8002F4D8), but that pale
         * tint on a 1px PSX line READS white — on our crisp native
         * lines it reads salmon/green/etc. Perceptual fidelity wins: far cross stays stock white. */
        if (style == 1 && hue > 0 && c.m3 >= S3_GLINT_NEAR_M3)
            style = 0;
        if (style == 1 && s_sstar_n < MAX_SSTAR) {
            /* 4-point star flare: tips = the stock cross's 4 diagonal
             * endpoints (same pulse+spin), waist points 45° between at
             * ~0.21R — a filled concave star instead of two lines. */
            double wc = (ec - es) * 0.2121, ws = (es + ec) * 0.2121;
            double P[8][2] = {
                { ec,  es}, { wc,  ws}, {-es,  ec}, {-ws,  wc},
                {-ec, -es}, {-wc, -ws}, { es, -ec}, { ws, -wc} };
            sstar* st = &s_sstar[s_sstar_n++];
            st->sx[0] = (float)(ofx + clamp16(TR0)*qd);   /* centre */
            st->sy[0] = (float)(ofy + clamp16(TR1)*qd);
            for (int k = 0; k < 8; k++) {
                double m1 = TR0 + a3 * P[k][0] / 4096.0;
                double m2 = TR1 + a3 * P[k][1] / 4096.0;
                st->sx[k+1] = (float)(ofx + clamp16(m1)*qd);
                st->sy[k+1] = (float)(ofy + clamp16(m2)*qd);
            }
            st->cz = (float)c.m3 * PART_VZ_WORLD4;
            st->rgb = color;
            st->hue = hue;
            continue;
        }
        double T[4][2] = { {-ec,-es}, {es,-ec}, {-es,ec}, {ec,es} };
        double sx[4], sy[4];
        for (int k = 0; k < 4; k++) {
            double m1 = TR0 + a3 * T[k][0] / 4096.0;
            double m2 = TR1 + a3 * T[k][1] / 4096.0;
            sx[k] = ofx + clamp16(m1)*qd;
            sy[k] = ofy + clamp16(m2)*qd;
        }
        /* line 1 = T0→T3, line 2 = T1→T2 */
        sline* l0 = &s_sline[s_sline_n++];
        l0->ax=(float)sx[0]; l0->ay=(float)sy[0]; l0->bx=(float)sx[3]; l0->by=(float)sy[3];
        l0->cz=(float)c.m3 * PART_VZ_WORLD4; l0->rgb=color;   /* world/4 -> full-world */
        sline* l1 = &s_sline[s_sline_n++];
        l1->ax=(float)sx[1]; l1->ay=(float)sy[1]; l1->bx=(float)sx[2]; l1->by=(float)sy[2];
        l1->cz=(float)c.m3 * PART_VZ_WORLD4; l1->rgb=color;
        if (streak_dbg()) { streak_log("sparkle", sp, sx[0], sy[0], sx[3], sy[3]);
                            streak_log("sparkle", sp, sx[1], sy[1], sx[2], sy[2]); }
    }
}

/* Feed each sparkle line as a thin (1px-perp) opaque quad — the scene is
 * triangles-only, so LINE_F becomes a 2-tri ribbon along the centerline.
 * Style-1 stars feed as an 8-tri fan (centre + consecutive outline
 * pairs), ADDITIVE, white-hot core fading to m_Color at the rim. */

/* OT source-body pull. The PSX links a moby's whole model into ONE OT
 * bucket at its centre depth and the sparkle renderer appends into that
 * same bucket AFTER the mobys (tail-append idiom at 0x80058508 ff.), so a
 * sparkle draws over its ENTIRE source moby; per-pixel Z inverts that tie
 * (the body's front polys are nearer than the centre-spawned sparkle).
 * Pull sparkle depth by a fixed distance that clears typical source
 * bodies (gems ~250, chests ~300, statue parts more). This is a
 * deliberate carve-out from the true-stored-depth rule: the og behaviour
 * is same-bucket APPEND ORDER — the sparkle beats its source regardless
 * of body size — which no sub-slot tie pull can express; the pull must
 * span the source body, not one OT slot. Residual vs PSX: geometry
 * BETWEEN sparkle and camera within this margin is overdrawn (PSX's own
 * pierce bound was its ~128-unit bucket); transient 5px glints make that
 * invisible in practice. */
#define SPARKLE_OT_PULL 1024.0f

static void sparkle_feed_native(CPUState* cpu) {
    sparkle_build(cpu);
    if (s_sline_n <= 0 && s_sstar_n <= 0) return;
    openpete_native_scene_begin_frame(openpete_frame);
    openpete_native_scene_set_tag(
        OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_FLARE, 0, 0));
    for (int i = 0; i < s_sstar_n; i++) {
        sstar* st = &s_sstar[i];
        float depth = openpete_native_scene_viewz(
            openpete_worldvz_pull(openpete_worldvz_full(st->cz), SPARKLE_OT_PULL));
        uint32_t core, tipc, waistc;
        if (st->hue > 0) {
            /* S3 near-star gouraud (func_8002F098): centre = word0
             * (pale, white-hot under additive), spike tips = word1
             * (deep hue), waists = word2 (S3's near-black ring). The
             * hue IS the gradient — additive falloff along each arm. */
            core   = k_s3_glint[st->hue - 1][0];
            tipc   = k_s3_glint[st->hue - 1][1];
            waistc = k_s3_glint[st->hue - 1][2];
        } else {
            /* legacy: rim = m_Color, core pushed toward white (x2) */
            uint32_t rim = st->rgb & 0xFFFFFFu;
            uint32_t cr = ((rim >> 0) & 0xFF) * 2; if (cr > 255) cr = 255;
            uint32_t cg = ((rim >> 8) & 0xFF) * 2; if (cg > 255) cg = 255;
            uint32_t cb = ((rim >> 16) & 0xFF) * 2; if (cb > 255) cb = 255;
            core = cr | (cg << 8) | (cb << 16);
            tipc = waistc = rim;
        }
        for (int k = 0; k < 8; k++) {
            int k2 = (k + 1) & 7;
            openpete_scene_tri t = {0};
            t.v[0].sx = st->sx[0];    t.v[0].sy = st->sy[0];
            t.v[1].sx = st->sx[k+1];  t.v[1].sy = st->sy[k+1];
            t.v[2].sx = st->sx[k2+1]; t.v[2].sy = st->sy[k2+1];
            /* outline points alternate tip (even k) / waist (odd k) */
            uint32_t oc1 = (k  & 1) ? waistc : tipc;
            uint32_t oc2 = (k2 & 1) ? waistc : tipc;
            for (int m = 0; m < 3; m++) {
                uint32_t c = m == 0 ? core : (m == 1 ? oc1 : oc2);
                t.v[m].z = depth; t.v[m].pvz = depth; t.v[m].u = 0.0f; t.v[m].v = 0.0f;
                t.v[m].rgba = (c & 0xFFFFFFu) | 0xFF000000u;
            }
            t.textured = 0;
            t.tpage_x = t.tpage_y = t.clut_x = t.clut_y = t.bpp = 0;
            t.blend = 2;                          /* additive flare */
            openpete_native_scene_add_tri(&t, OPENPETE_SCENE_WORLD, 0);
        }
    }
    openpete_native_scene_set_tag(0);
    if (s_sline_n <= 0) return;
    int split[2][3] = { {0,1,2}, {1,3,2} };
    openpete_native_scene_set_tag(
        OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_SPARK, 0, 0));
    for (int i = 0; i < s_sline_n; i++) {
        sline* l = &s_sline[i];
        float dx = l->bx - l->ax, dy = l->by - l->ay;
        float len = sqrtf(dx*dx + dy*dy);
        float px = 0.5f, py = 0.0f;                  /* perpendicular ·0.5 (1px wide) */
        if (len > 1e-3f) { px = -dy/len*0.5f; py = dx/len*0.5f; }
        float cx[4] = { l->ax-px, l->ax+px, l->bx-px, l->bx+px };
        float cy[4] = { l->ay-py, l->ay+py, l->by-py, l->by+py };
        float depth = openpete_native_scene_viewz(
            openpete_worldvz_pull(openpete_worldvz_full(l->cz), SPARKLE_OT_PULL));
        for (int s = 0; s < 2; s++) {
            openpete_scene_tri st = {0};
            for (int k = 0; k < 3; k++) {
                int j = split[s][k];
                st.v[k].sx = cx[j]; st.v[k].sy = cy[j]; st.v[k].z = depth; st.v[k].pvz = depth;
                st.v[k].u = 0.0f; st.v[k].v = 0.0f;
                st.v[k].rgba = (l->rgb & 0xFFFFFFu) | 0xFF000000u;
            }
            st.textured = 0;
            st.tpage_x = st.tpage_y = st.clut_x = st.clut_y = st.bpp = 0;
            st.blend = 0;                            /* LINE_F 0x40 = opaque */
            openpete_native_scene_add_tri(&st, OPENPETE_SCENE_WORLD, 0);
        }
    }
    openpete_native_scene_set_tag(0);
}

/** @brief Compares the native sparkle lines against the renderer's captured LINE_F prims (code 0x40).
 *
 * Rebuilds the sparkles, then matches every native line endpoint to its
 * nearest .s line endpoint (raw[2], raw[3], packed (Y << 16) | X) by
 * Manhattan distance. Prints one [sparkle-ab] line with the line and prim
 * counts, the mean and the worst distance.
 * @param cpu The guest CPU state. */
static void sparkle_compare(CPUState* cpu) {
    sparkle_build(cpu);
    int prim_line = 0; for (int p = 0; p < s_prim_n; p++) if (s_prim[p].code == 0x40) prim_line++;
    double sum = 0, worst = 0; int nn = 0;
    for (int t = 0; t < s_sline_n; t++) {
        double e[2][2] = { {s_sline[t].ax, s_sline[t].ay}, {s_sline[t].bx, s_sline[t].by} };
        for (int k = 0; k < 2; k++) {
            double best = 1e9;
            for (int p = 0; p < s_prim_n; p++) {
                if (s_prim[p].code != 0x40) continue;
                for (int m = 2; m <= 3; m++) {       /* raw[2]=v0, raw[3]=v1 packed (Y<<16)|X */
                    double lx = (double)(int16_t)(s_prim[p].raw[m] & 0xFFFF);
                    double ly = (double)(int16_t)(s_prim[p].raw[m] >> 16);
                    double d = (e[k][0]-lx<0?lx-e[k][0]:e[k][0]-lx) + (e[k][1]-ly<0?ly-e[k][1]:e[k][1]-ly);
                    if (d < best) best = d;
                }
            }
            if (best < 1e8) { sum += best; if (best > worst) worst = best; nn++; }
        }
    }
    fprintf(stderr, "[sparkle-ab f=%u] native_line=%d prim_line=%d nn=%.2f worst=%.1fpx\n",
            openpete_frame, s_sline_n, prim_line, nn?sum/nn:0, worst);
}

/* ---- dragon-rescue star flash (func_80058864) — float walk + feed + A/B ---- *
 * The "free a dragon" 8-point star sparkle (e.g. Nestor in Artisans). Unlike the
 * particle/glow/sparkle pools this is a SINGLE gamestate record D_80076248,
 * decoded from the recompiled C (block_80058864) cross-checked against the
 * captured prims:
 *   +0x00 gate (active flag; skip when 0)   +0x04 Vector3D world position
 *   +0x10 SHORTMATRIX (per-effect rotation; built each frame by
 *         RotVec8ToMatrix(+0x28 rot8,mat,g_Camera.projMat) so it already folds
 *         in the camera projection -> RTPS through it maps local spoke -> view
 *         space; dragonstar_matrix recomposes it in double from the angles)
 *   +0x24 spoke size      +0x2B centre colour byte (broadcast to (c,c,c))
 * The .s sets TR = view-space centre = camR·(camY-py,camZ-pz,px-camX) (the same
 * swizzle particles/glows use), projects the centre (V=0), then RTPS-projects 16
 * ring spokes through `matrix`+TR. Ring radius alternates: even points
 * (sin/cos·size)>>12, odd points >>10 (4x farther) -> an 8-point star. Each PSX
 * prim is a FLAT tri (code 0x22 = F3+STP) of one colour (c,c,c); a separate
 * 0xE1000620 draw-mode prim makes the whole effect ABR mode 1 ADDITIVE. Output
 * OT = g_HudOT (over the world). Display-only & determinism-safe: the .s writes
 * no gameplay-readable byte (the producer func_8002F3E4 animates D_80076248). */

static gtri s_dstri[16]; static int s_dstri_n;

/* Sub-tick euler override for the star's +0x28 angles, {Z, X, Y} byte angles
 * at float precision; s_ds_rot_on 0 reads the record's own bytes. */
static double s_ds_rot[3];
static int    s_ds_rot_on;

void dragonstar_native_set_rot_interp(const double* ang_zxy) {
    s_ds_rot_on = (ang_zxy != NULL);
    if (ang_zxy) memcpy(s_ds_rot, ang_zxy, sizeof s_ds_rot);
}

/** @brief The star's spoke matrix as gamestate 8 builds it before func_80058864, composed in double.
 *
 * The game builds it with RotVec8ToMatrix(+0x28 angles,
 * g_Camera.m_ProjectionMatrix) into +0x10 (func_8001CFDC); here it is composed
 * in double from the camera matrix the view resolves (the bracketed camera on a
 * sub-tick present) and the angles, which are the record's bytes on a canonical
 * present and the orchestrator's float lerp on a sub-tick one. The record's
 * +0x10 holds the GTE's truncated copy of the same product and is not read.
 * @param cpu guest CPU state
 * @param v   the resolved view; its cam_M is the left factor
 * @param Me  receives the spoke matrix, Q12 scale, unrounded */
static void dragonstar_matrix(CPUState* cpu, const ngte_view* v, double Me[3][3]) {
    double ang[3];
    if (s_ds_rot_on) {
        memcpy(ang, s_ds_rot, sizeof ang);
    } else {
        uint32_t w = cpu->read_word(D_DRAGONSTAR + 0x28);
        ang[0] = (double)(w & 0xFFu);
        ang[1] = (double)((w >> 8) & 0xFFu);
        ang[2] = (double)((w >> 16) & 0xFFu);
    }
    ngte_compose_euler_f(cpu, v->cam_M, ang, Me);
}

/** @brief Builds the star's 16-triangle centre fan into s_dstri.
 *
 * Projects the record's centre, then the 16 ring spokes (even points at size,
 * odd points at 4x size) through the spoke matrix and the centre's view
 * translation. Leaves s_dstri_n 0 when the record's gate is clear or the centre
 * is behind the camera.
 * @param cpu guest CPU state */
static void dragonstar_build(CPUState* cpu) {
    s_dstri_n = 0;
    if (cpu->read_word(D_DRAGONSTAR + 0x00) == 0) return;       /* inactive */

    ngte_view_build(cpu, &s_pv);
    const ngte_view* v = &s_pv;
    int Rc[3][3]; memcpy(Rc, v->cam_M, sizeof Rc);
    double camX = v->cam[0], camY = v->cam[1], camZ = v->cam[2];
    double ofx = v->ofx, ofy = v->ofy;
    double H   = v->H_eff;
    const double Hc = v->H_canon, qcap = NGTE_DIV_CAP * v->s;  /* GTE limit regime on the GTE's H; capped quotient scales with the view */

    int px = (int)cpu->read_word(D_DRAGONSTAR+0x04);
    int py = (int)cpu->read_word(D_DRAGONSTAR+0x08);
    int pz = (int)cpu->read_word(D_DRAGONSTAR+0x0C);

    /* TR = view-space centre (camera matrix · swizzle); project it (V=0). */
    projres c; proj2(Rc, ofx, ofy, v, (double)(camY-py), (double)(camZ-pz), (double)(px-camX), &c);
    double trx = c.m1, try_ = c.m2, trz = c.m3;
    if (trz <= 0.0) return;                                     /* behind camera */

    double Me[3][3]; dragonstar_matrix(cpu, v, Me);             /* per-effect rotation */
    int size = (int)cpu->read_word(D_DRAGONSTAR+0x24);
    uint32_t cc = cpu->read_word(D_DRAGONSTAR+0x28) >> 24;      /* +0x2B colour byte */
    cc &= 0xFF;
    uint32_t color = cc | (cc<<8) | (cc<<16);

    /* 16 ring points in angular order: idx = m*16; even m -> outer (>>12), odd m
     * -> inner (>>10). spoke local = ((sin·size)>>shift, 0, (cos·size)>>shift),
     * RTPS through the per-effect matrix + TR. COSINE_8 = SINE_8 + 0x80. */
    double rx[16], ry[16];
    for (int m = 0; m < 16; m++) {
        int idx   = m * 16;
        int sinv  = rd_s16(cpu, D_SIN_TABLE        + (uint32_t)idx*2);
        int cosv  = rd_s16(cpu, D_SIN_TABLE + 0x80 + (uint32_t)idx*2);
        /* spoke radius: even points >>12, odd >>10 — kept as FLOAT division (not
         * the .s integer arithmetic shift) so the spoke length carries sub-pixel
         * precision. The native renderer's whole point is no positional
         * quantization; the int floor is the .s artefact we improve on. */
        double rscale = (m & 1) ? 1024.0 : 4096.0;
        double vx = (double)sinv * (double)size / rscale;
        double vz = (double)cosv * (double)size / rscale;
        double mac1 = (Me[0][0]*vx + Me[0][2]*vz)/4096.0 + trx;
        double mac2 = (Me[1][0]*vx + Me[1][2]*vz)/4096.0 + try_;
        double mac3 = (Me[2][0]*vx + Me[2][2]*vz)/4096.0 + trz;
        double sz3 = mac3 < 0.0 ? 0.0 : (mac3 > 65535.0 ? 65535.0 : mac3);
        double q = part_cap(sz3, Hc, H) ? qcap : (H / sz3);
        rx[m] = ofx + clamp16(mac1) * q;
        ry[m] = ofy + clamp16(mac2) * q;
    }

    for (int m = 0; m < 16; m++) {                              /* centre fan */
        int n = (m + 1) & 15;
        gtri* t = &s_dstri[s_dstri_n++];
        t->sx[0]=(float)c.sx;  t->sy[0]=(float)c.sy;  t->rgb[0]=color;   /* centre */
        t->sx[1]=(float)rx[m]; t->sy[1]=(float)ry[m]; t->rgb[1]=color;
        t->sx[2]=(float)rx[n]; t->sy[2]=(float)ry[n]; t->rgb[2]=color;
        t->cz = (float)trz;
    }
}

/** @brief Builds the dragon-rescue star fan and feeds it into the native scene.
 *
 * Emits untextured flat triangles in the additive blend group (blend 2) at
 * view-Z 1, the near plane: the additive pipeline writes no depth, so the
 * star draws over the whole 3D scene as the PSX's g_HudOT link does. Emits
 * nothing when the record's gate is clear. Display-only.
 * @param cpu The guest CPU state. */
static void dragonstar_feed_native(CPUState* cpu) {
    dragonstar_build(cpu);
    if (s_dstri_n <= 0) return;
    openpete_native_scene_begin_frame(openpete_frame);
    /* g_HudOT overlay: the PSX links this into the HUD OT, drawn OVER the whole 3D
     * scene. blend=2 puts it in the native additive group (drawn last, over the
     * player); view-Z 1 (the near plane) so the depth test never rejects a
     * spike that crosses nearer ground/Spyro — the additive pipe is
     * depth_write_off, so this is an on-top additive overlay, mirroring the PSX
     * HUD-OT order. One depth for the whole overlay: W is constant. */
    float depth = 1.0f;
    openpete_native_scene_set_tag(
        OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_STAR, 0, 0));
    for (int i = 0; i < s_dstri_n; i++) {
        gtri* t = &s_dstri[i];
        openpete_scene_tri st = {0};
        for (int k = 0; k < 3; k++) {
            st.v[k].sx = t->sx[k]; st.v[k].sy = t->sy[k]; st.v[k].z = depth; st.v[k].pvz = depth;
            st.v[k].u = 0.0f; st.v[k].v = 0.0f;
            st.v[k].rgba = (t->rgb[k] & 0xFFFFFFu) | 0xFF000000u;
        }
        st.textured = 0;
        st.tpage_x = st.tpage_y = st.clut_x = st.clut_y = st.bpp = 0;
        st.blend = 2;                                  /* ABR mode 1 (additive) */
        openpete_native_scene_add_tri(&st, OPENPETE_SCENE_WORLD, 0);
    }
    openpete_native_scene_set_tag(0);
}

/** @brief Compares the native star fan against the renderer's captured F3 semi-transparent prims (code 0x22).
 *
 * Rebuilds the fan, then matches every native vertex to its nearest .s F3
 * vertex by Manhattan distance, independent of order. Prints one [dstar-ab]
 * line with the triangle and prim counts, the mean and the worst distance.
 * @param cpu The guest CPU state. */
static void dragonstar_compare(CPUState* cpu) {
    dragonstar_build(cpu);
    int prim22 = 0; for (int p = 0; p < s_prim_n; p++) if (s_prim[p].code == 0x22) prim22++;
    double sum = 0, worst = 0; int nn = 0;
    for (int t = 0; t < s_dstri_n; t++) for (int k = 0; k < 3; k++) {
        double ax = s_dstri[t].sx[k], ay = s_dstri[t].sy[k], best = 1e9;
        for (int p = 0; p < s_prim_n; p++) {
            if (s_prim[p].code != 0x22) continue;
            for (int mv = 0; mv < 3; mv++) {
                double dx = ax - s_prim[p].x[mv], dy = ay - s_prim[p].y[mv];
                double d = (dx<0?-dx:dx) + (dy<0?-dy:dy);
                if (d < best) best = d;
            }
        }
        if (best < 1e8) { sum += best; if (best > worst) worst = best; nn++; }
    }
    fprintf(stderr, "[dstar-ab f=%u] native_tri=%d prim22=%d nn=%.2f worst=%.1fpx\n",
            openpete_frame, s_dstri_n, prim22, nn?sum/nn:0, worst);
}

/* ---- gating --------------------------------------------------------------- */
/** @brief Reports whether OPENPETE_PARTICLE_VERIFY enables the particle verify mode.
 *
 * Verify mode captures the renderers' prims, dumps the pools and prims once
 * and runs the native-versus-.s A/B compares. The variable is read once and
 * cached.
 * @return 1 when the variable is set and not "0", else 0. */
static int verify_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_PARTICLE_VERIFY"); v = (e && *e && *e != '0'); }
    return v;
}
static unsigned verify_frame(void) {
    static unsigned f = 0xFFFFFFFFu;
    if (f == 0xFFFFFFFFu) { const char* e = getenv("OPENPETE_PARTICLE_VERIFY_FRAME"); f = e ? (unsigned)strtoul(e, NULL, 0) : 0; }
    return f;
}

/* Per-tick path census (OPENPETE_PARTICLE_PATH_CENSUS=1). The three pools
 * outlive their renderers: a pool can hold active records on a tick where the
 * game never called that renderer, or called it and it emitted nothing. Under
 * the retired shared-path gating any such tick replayed the pool on every
 * intermediate present whenever any sibling renderer had drawn; the dragon
 * star still shares RP_PARTICLES, so its exposure is the live number. One
 * record per tick (openpete_frame at the hook), evaluated when the next tick's
 * first hook arrives; a totals line at exit for the corpus harness. */
typedef struct {
    unsigned frame; int ran_p, ran_g, ran_s, ran_star;
    uint32_t bytes_p, bytes_g, bytes_s, bytes_star;
    int pool_glow, pool_spark, pool_star;
} pcen_tick_t;
static pcen_tick_t s_pcen; static int s_pcen_on = -1;
static uint32_t s_pcen_ticks, s_pcen_active[3], s_pcen_ran[3], s_pcen_exposed[3], s_pcen_first[3];
/* Sparkle walk vs .s on the canonical hook: twinkles the native walk emits
 * beyond what the .s drew (extra) and short of it (missing), per tick. */
static uint32_t s_pcen_sp_ticks, s_pcen_sp_extra_ticks, s_pcen_sp_extra, s_pcen_sp_missing_ticks, s_pcen_sp_missing, s_pcen_sp_extra_first;
static void particle_path_census_exit(void) {
    fprintf(stderr, "particle path census: ticks=%u glow: active=%u ran=%u exposed=%u first=%u | "
            "sparkle: active=%u ran=%u exposed=%u first=%u | star: active=%u ran=%u exposed=%u first=%u | "
            "sparkle-walk: ticks=%u extra_ticks=%u extra=%u missing_ticks=%u missing=%u extra_first=%u\n",
            s_pcen_ticks, s_pcen_active[0], s_pcen_ran[0], s_pcen_exposed[0], s_pcen_first[0],
            s_pcen_active[1], s_pcen_ran[1], s_pcen_exposed[1], s_pcen_first[1],
            s_pcen_active[2], s_pcen_ran[2], s_pcen_exposed[2], s_pcen_first[2],
            s_pcen_sp_ticks, s_pcen_sp_extra_ticks, s_pcen_sp_extra, s_pcen_sp_missing_ticks, s_pcen_sp_missing, s_pcen_sp_extra_first);
}
static void sparkle_build(CPUState* cpu);
/** @brief Counts, for one sparkle hook call, the twinkles the native walk builds beyond and short of the .s.
 *
 * Runs after the .s drew and its prims were captured. Rebuilds the
 * sparkles and compares the native cross count (s_sline_n / 2; stars are
 * not counted) with the captured LINE_F (0x40) prims / 2, accumulating the
 * extra and missing totals, their tick counts and the first extra tick.
 * No-op unless OPENPETE_PARTICLE_PATH_CENSUS is on.
 * @param cpu The guest CPU state.
 * @param captured 1 when s_prim holds this call's prims, else 0. */
static void particle_path_census_sparkle(CPUState* cpu, int captured) {
    if (s_pcen_on != 1) return;
    sparkle_build(cpu);
    int native = s_sline_n / 2, prims = 0;
    if (captured) for (int p = 0; p < s_prim_n; p++) if (s_prim[p].code == 0x40) prims++;
    prims /= 2;
    s_pcen_sp_ticks++;
    if (native > prims) { if (s_pcen_sp_extra_ticks++ == 0) s_pcen_sp_extra_first = openpete_frame; s_pcen_sp_extra += (uint32_t)(native - prims); }
    if (native < prims) { s_pcen_sp_missing_ticks++; s_pcen_sp_missing += (uint32_t)(prims - native); }
}
static void particle_path_census_flush(void) {
    pcen_tick_t* t = &s_pcen;
    if (t->frame == 0) return;
    s_pcen_ticks++;
    /* Retired gating: RP_PARTICLES drawn by any sibling; current star gating: the
     * path the star shares (particles or the star itself) drawn. */
    const int any_drawn  = t->bytes_p > 0 || t->bytes_g > 0 || t->bytes_s > 0 || t->bytes_star > 0;
    const int part_drawn = t->bytes_p > 0 || t->bytes_star > 0;
    const int active[3] = { t->pool_glow, t->pool_spark, t->pool_star };
    const int drew[3]   = { t->ran_g && t->bytes_g > 0, t->ran_s && t->bytes_s > 0, t->ran_star && t->bytes_star > 0 };
    const int ran[3]    = { t->ran_g, t->ran_s, t->ran_star };
    const int gate[3]   = { any_drawn, any_drawn, part_drawn };
    for (int k = 0; k < 3; k++) {
        if (active[k]) s_pcen_active[k]++;
        if (ran[k]) s_pcen_ran[k]++;
        if (active[k] && !drew[k] && gate[k]) {
            if (s_pcen_exposed[k]++ == 0) s_pcen_first[k] = t->frame;
        }
    }
}
static void particle_path_census_note(CPUState* cpu, char tag, uint32_t bytes) {
    if (s_pcen_on < 0) {
        const char* e = getenv("OPENPETE_PARTICLE_PATH_CENSUS");
        s_pcen_on = (e && *e && *e != '0') ? 1 : 0;
        if (s_pcen_on) atexit(particle_path_census_exit);
    }
    if (!s_pcen_on) return;
    if (s_pcen.frame != openpete_frame) {
        particle_path_census_flush();
        memset(&s_pcen, 0, sizeof s_pcen);
        s_pcen.frame = openpete_frame;
        for (int i = 0; i < 16 && !s_pcen.pool_glow; i++)
            if (cpu->read_word(D_GLOWS + (uint32_t)i*0x24) != 0) s_pcen.pool_glow = 1;
        for (int i = 0; i < 8 && !s_pcen.pool_spark; i++)
            if ((cpu->read_word(G_SPARKLES + (uint32_t)i*0x18 + 0xC) & 0xFF) != 0) s_pcen.pool_spark = 1;
        s_pcen.pool_star = cpu->read_word(D_DRAGONSTAR) != 0;
    }
    switch (tag) {
    case 'p': s_pcen.ran_p = 1; s_pcen.bytes_p += bytes; break;
    case 'g': s_pcen.ran_g = 1; s_pcen.bytes_g += bytes; break;
    case 's': s_pcen.ran_s = 1; s_pcen.bytes_s += bytes; break;
    default:  s_pcen.ran_star = 1; s_pcen.bytes_star += bytes; break;
    }
}

/* Shared hook body: run the real renderer once, capture its prims, record the
 * renderer's polybuf range on its own path, (dump in verify mode). `tag`
 * selects the dump label + which gamestate pools to print. */
/* The three particle renderers share one latch: they are separate guest
 * entries, so the descriptor is built per call around the caller's own draw. */
static hook_bracket_state s_particle_hb;
static void (*s_particle_realfn)(CPUState*);
static void particle_run_draw(CPUState* cpu, uint32_t addr) { (void)addr; s_particle_realfn(cpu); }
/** @brief Stashes the g_Particles pool pointer before the guest particle renderer runs.
 *
 * g_Particles is a pointer the game swaps between two buffers, so the pool
 * the .s walks is read here and later walks use s_part_pool.
 * @param cpu The guest CPU state.
 * @param feed Unused.
 * @param verify Unused. */
static void particle_pre(CPUState* cpu, int feed, int verify) {
    (void)feed; (void)verify;
    s_part_pool = cpu->read_word(G_PARTICLES);
}

static int on_particle_renderer(CPUState* cpu, void (*realfn)(CPUState*), const char* tag,
                                render_path_t path, int dump_pools_too) {
    const hook_bracket_desc desc = {
        .path = path, .prof_tag = "particle.srend",
        .run = particle_run_draw, .verify_enabled = verify_enabled,
        .pre = particle_pre, .st = &s_particle_hb,
    };
    s_particle_realfn = realfn;
    uint32_t startp, endp; int feed, verify;
    if (!hook_bracket_run(cpu, 0u, &desc, &startp, &endp, &feed, &verify)) return 0;

    particle_path_census_note(cpu, tag[0], endp - startp);

    if (verify) {
        static int s_trace[3] = {0,0,0};
        int slot = (tag[0]=='p') ? 0 : (tag[0]=='g') ? 1 : 2;
        if (s_trace[slot] < 30 && endp != startp) {
            fprintf(stderr, "[%s-call f=%u] polybuf %u bytes\n", tag, openpete_frame, endp - startp);
            s_trace[slot]++;
        }
    }

    if ((feed || verify) && endp > startp) {
        s_prim_n = 0;
        capture_prims(cpu, startp, endp);
    }
    if (tag[0] == 's') particle_path_census_sparkle(cpu, (feed || verify) && endp > startp);

    /* feed the native geometry into the scene: FT4 particles (tag 'p'), glow
     * rings (tag 'g'), or sparkle twinkles (tag 's'). */
    if (feed && endp > startp && render_path_enabled(path)) {
        if (tag[0] == 'p')      particle_feed_native(cpu);
        else if (tag[0] == 'g') glow_feed_native(cpu);
        else if (tag[0] == 's') sparkle_feed_native(cpu);
    }

    if (verify && endp > startp && openpete_frame >= verify_frame()) {
        static int s_dumped[3] = {0,0,0};
        int slot = (tag[0]=='p') ? 0 : (tag[0]=='g') ? 1 : 2;
        if (!s_dumped[slot]) {
            if (dump_pools_too) dump_pools(cpu);
            dump_prims(cpu, tag, startp, endp);
            s_dumped[slot] = 1;
        }
        if (slot == 0) {                                 /* particles: FT4 A/B */
            static int s_reports = 0;
            if (s_reports < 2000) { particle_compare(cpu); s_reports++; }
        } else if (slot == 1) {                          /* glows: ring fan A/B */
            static int s_greports = 0;
            if (s_greports < 2000) { glow_compare(cpu); s_greports++; }
        } else if (slot == 2) {                          /* sparkles: line A/B */
            static int s_sreports = 0;
            if (s_sreports < 2000) { sparkle_compare(cpu); s_sreports++; }
        }
    }

    return hook_bracket_end(&desc);
}

static int on_800573C8_particles(CPUState* cpu, uint32_t addr) { (void)addr; return on_particle_renderer(cpu, func_800573C8, "particles", RP_PARTICLES, 1); }
static int on_800580F4_glows(CPUState* cpu, uint32_t addr)     { (void)addr; return on_particle_renderer(cpu, func_800580F4, "glows", RP_GLOW, 0); }
static int on_800584C4_sparkles(CPUState* cpu, uint32_t addr)  { (void)addr; return on_particle_renderer(cpu, func_800584C4, "sparkles", RP_SPARKLE, 0); }

/** @brief Runs the guest dragon-rescue star renderer (func_80058864) as the hook bracket's draw.
 *
 * The star flash is a single-record effect outside the particle pools, so
 * it has its own hook, on_80058864_dragonstar, without the path census. The
 * hook runs this draw (g_HudOT prims for the PsyCross path), captures its
 * F3 prims for the A/B, and feeds the float star. Display-only. The star
 * shares RP_PARTICLES with the pool feeders, and the sub-tick re-extract
 * replays it only while that path is canon-drawn, so the hook records its
 * polybuf interval and marks the path whenever the native feed emitted: on
 * a tick where only the star draws, nothing else marks it, and the star
 * would show on canonical presents and vanish on the intermediates.
 * @param cpu The guest CPU state.
 * @param addr The hooked address (unused). */
static void dragonstar_run_draw(CPUState* cpu, uint32_t addr) { (void)addr; func_80058864(cpu); }

static hook_bracket_state s_dragonstar_hb;
static const hook_bracket_desc k_dragonstar_bracket = {
    .path = RP_PARTICLES, .prof_tag = "particle.srend",
    .run = dragonstar_run_draw, .verify_enabled = verify_enabled,
    .st = &s_dragonstar_hb, .flags = HB_NO_RENDER_PATHS,
};

static int on_80058864_dragonstar(CPUState* cpu, uint32_t addr) {
    uint32_t startp, endp; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_dragonstar_bracket, &startp, &endp, &feed, &verify))
        return 0;
    particle_path_census_note(cpu, '*', endp - startp);

    if ((feed || verify) && endp > startp) { s_prim_n = 0; capture_prims(cpu, startp, endp); }
    if (feed) {
        dragonstar_feed_native(cpu);
        if (s_dstri_n > 0) render_path_mark_drawn(RP_PARTICLES);
    }

    if (verify && endp > startp && openpete_frame >= verify_frame()) {
        static int s_dumped = 0;
        if (!s_dumped) { dump_prims(cpu, "dstar", startp, endp); s_dumped = 1; }
        static int s_reports = 0;
        if (s_reports < 2000) { dragonstar_compare(cpu); s_reports++; }
    }
    return hook_bracket_end(&k_dragonstar_bracket);
}

/* Sub-tick re-extract (high-FPS spine): reproduce particles, glows and
 * sparkles from the current g_Particles pool — extract-only (no render_fn, no
 * verdicts). Pool walks self-limit to live particles; Sparx + ambient sparkles
 * stay present on intermediates (per-particle pos+=α·vel lerp lands in step 5). */
void particle_reextract(CPUState* cpu) {
    if (!openpete_native_scene_enabled()) return;
    /* Each feeder replays only when ITS .s renderer drew on the canonical
     * tick. The pools outlive the renderers: a level transition stops all
     * three (else the spine replays the previous level's pool over the
     * cyclorama), and a dragon-rescue cutscene keeps rendering particles while
     * the glow and sparkle renderers are never called, leaving Sparx's glow
     * active in its pool but absent from every canonical present. */
    const int part = render_path_enabled(RP_PARTICLES) && render_path_canon_drawn(RP_PARTICLES);
    const int glow = render_path_enabled(RP_GLOW)      && render_path_canon_drawn(RP_GLOW);
    const int spar = render_path_enabled(RP_SPARKLE)   && render_path_canon_drawn(RP_SPARKLE);
    if (!part && !glow && !spar) return;
    /* Channel-phase interp on for the re-extract feeds only (every present when
     * the sub-tick spine is active — canonical-lag leg + intermediates); reset
     * so the canonical inline hook feeds stay integer/byte-exact for the A/Bs.
     * Covers particle spin/size/extents + line far endpoints + sparkle
     * life/spin (see the s_phase_now block). */
    s_phase_now = openpete_interp_present_now();
    if (part) particle_feed_native(cpu);
    if (glow) glow_feed_native(cpu);
    if (spar) sparkle_feed_native(cpu);
    s_phase_now = 0;
    if (part) dragonstar_feed_native(cpu);   /* self-gates on the D_80076248 active flag; shares RP_PARTICLES */
}

static const feature_dispatch_entry_t particle_dispatch[] = {
    { 0x800573C8u, on_800573C8_particles },
    { 0x800580F4u, on_800580F4_glows },
    { 0x800584C4u, on_800584C4_sparkles },
    { 0x80058864u, on_80058864_dragonstar },
    { 0u, NULL },
};

const feature_module_t particle_module = {
    .name           = "particle_native",
    .apply_options  = NULL,
    .dispatch_table = particle_dispatch,
};
