// games/common/renderer/terrain.c - native world/terrain renderer, shared
// across games.
//
// Every engine address the walk reads comes from game_layout (the sector
// list, the visibility table, the texture tables, the camera, the polybuf);
// the per-game decode differences (sector flag layout, the vertex Z packing
// and the flat-sector rule, the HQ UV-orientation table, the near-cell defer
// queues) are game_layout.terrain fields; the per-game data the walk consults
// (portal sky planes, the frozen LP fill tags, level names, authored-data art
// fixes) comes through terrain_game (renderer/terrain_game.h).
//
// In Spyro 1 the world renderer is func_800258F0 (-> func_800276A0
// fallthrough), dispatched per frame by func_8002B9CC
// (PrepareEnvironmentRendering). It is camera-relative (GTE rotation =
// g_Camera+0x14, TR=0), projects each visible sector's 11/11/10 packed
// vertices via RTPS, frustum-culls sectors (writing the D_800771C8 visibility
// VERDICT), walks LP (8B) / HP (16B) face lists, and emits fogged GT4/GT3
// (+ flat G3) prims for the whole environment. Spyro 3's func_80022378 has the
// same sector header and vertex packing at twice the Z offset scale, keeps
// its visibility table in the scratchpad for the duration of the draw (so the
// walk reads a copy captured at the visibility function's return), and sorts
// flat-flagged sectors into a list whose Z offset is unshifted.
//
// This module hooks the layout's world-draw site, runs the real renderer once
// (gameplay writes - sector visibility - happen identically), brackets the
// polybuf to capture the .s prims as the A/B oracle, then does the NATIVE
// walk: per visible sector it
// decodes 11/11/10 verts -> float RTPS projection, walks HP (16B) / LP (8B) faces
// into tris, and computes per-vertex RGB888 + DPCS depth-cue fog from the sector
// colour arrays (terrain_color). Each face takes its Tiledef material and
// corner UVs (m_LQTexturePointer, or the HQ record for a near face) and ABE
// blend, and a near face takes the near/HQ subdivide. The result is fed to
// the native scene; under OPENPETE_TERRAIN_VERIFY it is also numerically
// compared with the captured .s prims (geometry + colour).
//
// Determinism: display-only. The walk reads gamestate and the captured
// polybuf and writes no PSX RAM, except the OPENPETE_ENV_LODDIST measurement
// bracket, which stores m_LodDistance for the duration of the world draw and
// restores it. func_8002B9CC/func_800258F0 keep running canonical-4:3 as the
// verdict producer; the native path reads g_Camera/g_Environment read-only
// and culls a wide frustum host-side.
//
// Gated by OPENPETE_TERRAIN_VERIFY (dump/A/B) or a native renderer mode (feed).

#include "feature_registry.h"
#include "psx_runtime_internal.h"   /* full CPUState (read_word, gte_ctrl, ...) */
#include "openpete_glitch_mark.h"
#include "openpete_native_render.h"
#include "openpete_psx.h"
#include "openpete_knobs.h"          /* knob_env_epoch_stale */
#include "openpete_subtick_view.h"  /* openpete_subtick_host_span (sector colour host reads) */
#include "renderer/render_paths.h"  /* --render-paths isolation */
#include "renderer/orchestrator.h"  /* openpete_native_interp_pos */
#include "renderer/terrain.h"        /* this feeder's exports */
#include "openpete_interp.h"         /* openpete_interp_present_now (scroll-anim alpha) */
#include "openpete_interp_objs.h"    /* per-descriptor scroll snapshots */
#include "openpete.h"                /* openpete_runahead_get */
#include "renderer/native_gte.h"    /* shared GTE float-mirror math */
#include "no_lod.h"  /* openpete_no_lod + openpete_fog_scale (--fog-scale) */
#include "openpete_fillpick.h"       /* LP fill-tag inspector (P): session verdicts + pick */
#include "rhi.h"                     /* rhi_terrain_vertex (the chunk packer) */
#include "openpete_terrain_cache.h"  /* terrain sector-mesh GPU cache (retained terrain) */

#include "renderer/terrain_game.h" /* per-game data: portal sky planes, fill tags, level names, art fixes */
#include "openpete_recomp_module.h"  /* the game's own world draw, run through the module dispatch */
#include "texture_pack.h"           /* identity resolve for the record dumper */
#include "openpete_mods.h"          /* untextured-band material resolve */
#include "openpete_native_vram.h"   /* VRAM model reads for the record dumper */
#include "openpete_workpool.h"      /* parallel sector walk (--extract-threads) */
#include "openpete_profile.h"       /* ter.* sub-scopes (--profile-out) */
#include "openpete_disc_trace.h"    /* OPENPETE_DISC_TAG_TERRAIN: the HP source reads */
#include "openpete_fnv1a64.h"       /* HP source face keys */
#include "openpete_log.h"
#include "renderer/wad_subfile.h"   /* the HP source level's sub-files */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game_layout.h"          /* game_layout: the engine RAM addresses this walk reads */
#include "renderer/env_knob.h"
#include "mod_vec.h"

extern void     func_800258F0(CPUState* cpu);  /* the real world renderer */
extern unsigned openpete_frame;

/* ---- gamestate addresses (psx.map / recompiled C / r_environment.s) -------- */


/* UV-scroll offsets (see tsc_scroll_desc). The scrolling-texture animation
 * writes whole texels into the tiledef V bytes once per canonical tick, and
 * its 7-bit accumulator advances faster than those writes resolve: the LQ V is
 * accumulator>>2, so the texture holds three ticks and steps a texel on the
 * fourth. These tables carry the V offset - in texels, relative to the integer
 * decode the game committed - that puts the tile back on the exact fractional
 * accumulator, plus, on a bracketed sub-tick present, the sweep back toward the
 * previous accumulator. They are filled on every build, canonical included, so
 * both place the same texel row. */
static uint8_t s_tanim_act[128];      /* tile carries a fractional offset this build */
static float   s_tanim_dvlq[128];     /* LQ per-quad tiledef  (game writes scroll>>2) */
static float   s_tanim_dvhq[128][2];  /* HQ record entries 1-2 / 3-4 (scroll>>1) */
static float   s_tanim_dvcell[128][4]; /* HQ record 4x4 cell rows 0-3 (scroll>>1, +16 per row) */
static int     s_tanim_have;          /* any tile active - skips the per-face test */
static int     s_tanim_en;            /* OPENPETE_TERRAIN_TANIM latch (re-extract only) */
/* Tiles on the live scroller list, refreshed EVERY build (canonical included).
 * The retained-GPU path claims any sector touching one: a baked mesh can
 * neither ride the per-present fractional offset above, nor track the HQ
 * sibling record, which advances even on ticks where the 16-byte LQ entry is
 * byte-identical (scroll>>2 unchanged) - so the LQ-snapshot generations that
 * schedule rebuilds legitimately miss those advances. */
static uint8_t s_tscroll_tile[128];
static int     s_tscroll_have;

/* ---- per-game decode switches --------------------------------------------- */

/** @brief Unpacks one packed 11/11/10 sector vertex into its origin-relative offsets.
 *
 * X is bits 21..31 and Y bits 10..20 in every game. The 10-bit Z is scaled
 * by the layout: Spyro 1 uses it as it is (vz_shl 0), Spyro 2 and 3 double
 * it (vz_shl 1), and a Spyro 2 or 3 sector carrying the flat flag (header
 * word 1 bit 12) stores Z at a quarter of the unshifted value (vz_flat_shr 2;
 * the draw sorts such a sector into a list whose vertex loop reads the Z
 * offset unshifted, in quarter units).
 * @param v The packed vertex word.
 * @param flat Nonzero when the sector carries the flat flag.
 * @param x Receives the X offset.
 * @param y Receives the Y offset.
 * @param z Receives the scaled Z offset. */
static inline void tvert_unpack(uint32_t v, int flat, int* x, int* y, int* z) {
    *x = (int)((v >> 21) & 0x7FF);
    *y = (int)((v >> 10) & 0x7FF);
    int vz = (int)(v & 0x3FF);
    *z = flat ? (vz >> game_layout.terrain.vz_flat_shr) : (vz << game_layout.terrain.vz_shl);
}
/** @brief Reads an environment-header field, or returns 0 when the layout leaves it unlocated.
 *
 * A field address of 0 means the game's layout does not locate the field;
 * reading vaddr 0 would return a kernel word that passes as a pointer.
 * @param cpu The guest CPU state.
 * @param field The field's guest address, or 0.
 * @return The field's word, or 0. */
static inline uint32_t tenv_rd(CPUState* cpu, uint32_t field) {
    return field ? cpu->read_word(field) : 0u;
}
/** @brief Returns the LP/HP LOD distance.
 *
 * The environment header's field when the layout locates one, else the
 * layout's constant terrain.lod_dist_default for a game whose header has
 * none.
 * @param cpu The guest CPU state.
 * @return The LOD distance. */
static inline uint32_t tenv_lod_dist(CPUState* cpu) {
    return game_layout.env.lod_dist ? cpu->read_word(game_layout.env.lod_dist) : game_layout.terrain.lod_dist_default;
}
/** @brief Returns the bounding radius field of a sector header's word 1.
 * @param w1 The sector header's word 1.
 * @return w1 masked by the layout's terrain.radius_mask. */
static inline int thdr_radius(uint32_t w1) { return (int)(w1 & game_layout.terrain.radius_mask); }
/** @brief Returns the LP/HP list flags of a sector header's word 1.
 * @param w1 The sector header's word 1.
 * @return w1 masked by the layout's terrain.flags_mask. */
static inline int thdr_flags(uint32_t w1)  { return (int)(w1 & game_layout.terrain.flags_mask); }
/** @brief Tests the flat flag of a sector header's word 1.
 * @param w1 The sector header's word 1.
 * @return Nonzero when the layout defines terrain.flat_flag and w1 carries it. */
static inline int thdr_flat(uint32_t w1)   { return game_layout.terrain.flat_flag && (w1 & game_layout.terrain.flat_flag); }

/* The visibility verdict. A game whose draw leaves the table in RAM is read
 * there (0xFF = visible); a game whose draw computes it in the scratchpad and
 * consumes it before returning (Spyro 3) has a visibility hook that copies the
 * bytes at the visibility function's return, and the walk reads that copy
 * (nonzero = visible) until the next draw. */
#define TVIS_MAX 1024
static uint8_t s_vis_cap[TVIS_MAX];
static int     s_vis_cap_n = -1;      /* -1 = no capture this tick: read RAM */
static int     s_vis_arm;             /* the draw is running: the next world visibility call is captured */
/** @brief Tests whether the game's visibility verdict marks sector i visible.
 *
 * Reads the captured copy (nonzero byte, index below the capture length)
 * when a capture is held, else the byte at visibility.base + i of guest RAM
 * (0xFF).
 * @param cpu The guest CPU state.
 * @param i The sector index.
 * @return 1 when visible, else 0. */
static int tvis_visible(CPUState* cpu, uint32_t i) {
    if (s_vis_cap_n >= 0) return i < (uint32_t)s_vis_cap_n && s_vis_cap[i] != 0;
    return cpu->read_byte(game_layout.visibility.base + i) == 0xFF;
}

/* ---- captured prims (ground truth) --------------------------------------- */
typedef struct {
    uint8_t  code;          /* GPU prim code byte (word[1]>>24) */
    uint8_t  len;           /* OT tag length (word[0]>>24)      */
    int      nv;            /* 3 or 4                           */
    float    x[4], y[4];
    uint32_t rgb[4];
    int      u[4], v[4];
    int      clut, tpage;
    int      textured, gouraud, quad;
} prim_t;

/* Terrain emits the whole world - store a bounded sample for dumps, but the
 * histogram counts every prim. */
#define MAX_PRIMS 8192
static prim_t s_gte[MAX_PRIMS]; static int s_gte_n;
static uint32_t s_code_hist[256];
static uint32_t s_prim_total;

/** @brief Stores one walked polygon into the bounded capture sample.
 *
 * The ngte_walk_prims sink of capture_prims: copies the primitive's code,
 * tag length, corners, colours, UVs, CLUT, TPAGE and shading flags into the
 * next s_gte slot.
 * @param ctx Unused.
 * @param s The walked primitive.
 * @return 1 when stored, 0 when the sample already holds MAX_PRIMS. */
static int terrain_prim_sink(void* ctx, const ngte_prim* s) {
    (void)ctx;
    if (s_gte_n >= MAX_PRIMS) return 0;
    prim_t* t = &s_gte[s_gte_n];
    memset(t, 0, sizeof(*t));
    t->code = s->code; t->len = s->taglen; t->nv = s->nv;
    for (int k = 0; k < 4; k++) {
        t->x[k] = s->x[k]; t->y[k] = s->y[k]; t->rgb[k] = s->rgb[k];
        t->u[k] = s->u[k]; t->v[k] = s->v[k];
    }
    t->clut = s->clut; t->tpage = s->tpage;
    t->textured = s->textured; t->gouraud = s->gouraud; t->quad = s->quad;
    s_gte_n++;
    return 1;
}

/** @brief Walks the polybuf [start, end) into the bounded capture sample.
 *
 * Only polygons enter the sample. The whole world passes through here, so
 * the walk guard is eight times the sample bound, and the prim-code
 * histogram and the total count every packet walked (polygon or not), not
 * only the sampled ones.
 * @param cpu The guest CPU state.
 * @param start The first polybuf address.
 * @param end One past the last polybuf address. */
static void capture_prims(CPUState* cpu, uint32_t start, uint32_t end) {
    OP_PROF_ACCUM_SCOPE("terrain.capture");
    s_gte_n = 0;
    s_prim_total = 0;
    memset(s_code_hist, 0, sizeof(s_code_hist));
    const ngte_walk_policy pol = {
        .tag_len_max = 24, .raw_words_max = 0, .guard = MAX_PRIMS * 8,
        .polygons_only = 1, .code_hist = s_code_hist, .tag_hist = NULL,
        .total = &s_prim_total, .count_all = 1,
    };
    ngte_walk_prims(cpu, start, end, &pol, terrain_prim_sink, NULL);
}

/* ---- live gamestate decode (validates the format map vs real data) -------- */

/* Host windows above every guest address the walk reads, so the decoders
 * read HP source level data (tsimp_t, tsv) with the code that reads the
 * scene's own: the walk's sector pointer table extended with imported
 * source sectors (TSV_TABLE_BASE), the source level's terrain sub-file
 * (TSRC_TERR_BASE) and its texture table (TSRC_TEX_BASE). */
#define TSV_TABLE_BASE 0xDF000000u
#define TSRC_TERR_BASE 0xE0000000u
#define TSRC_TEX_BASE  0xF0000000u
static const uint8_t* s_tsrc_tex;      /* the source table's bytes, or NULL */
static uint32_t       s_tsrc_tex_len;
static const uint8_t* s_tsrc_terr;     /* the source terrain sub-file's bytes, or NULL */
static uint32_t       s_tsrc_terr_len;
static uint32_t       s_tsv_table[1024];   /* scene sector pointers, then imported ones */
static uint32_t       s_tsv_n;

/** @brief Reads one little-endian word of a host window.
 * @param p The window's bytes, or NULL.
 * @param len The window's byte count.
 * @param o The offset.
 * @return The word; 0 outside the window. */
static uint32_t rd_host(const uint8_t* p, uint32_t len, uint32_t o) {
    if (!p || len < 4u || o > len - 4u) return 0u;
    const uint8_t* q = p + o;
    return (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
}

/** @brief Reads one guest word, or one word of a host window at TSV_TABLE_BASE and above.
 * @param cpu The guest CPU state.
 * @param a The guest address, or a window address.
 * @return The word at a; 0 for a window address past its window. */
static uint32_t rd(CPUState* cpu, uint32_t a) {
    if (a < TSV_TABLE_BASE) return cpu->read_word(a);
    if (a < TSRC_TERR_BASE) {
        const uint32_t i = (a - TSV_TABLE_BASE) >> 2;
        return i < s_tsv_n ? s_tsv_table[i] : 0u;
    }
    if (a < TSRC_TEX_BASE) return rd_host(s_tsrc_terr, s_tsrc_terr_len, a - TSRC_TERR_BASE);
    return rd_host(s_tsrc_tex, s_tsrc_tex_len, a - TSRC_TEX_BASE);
}

/** @brief Resolves a guest or window address span as a host pointer.
 *
 * A guest address goes through openpete_subtick_host_span (the sub-tick
 * view); an address in the TSRC_TERR_BASE window resolves into the source
 * terrain sub-file's bytes.
 * @param a The address.
 * @param span The byte count that must be mapped.
 * @return The host pointer, or NULL when the span does not map. */
static const void* tn_host_span(uint32_t a, uint32_t span) {
    if (a < TSV_TABLE_BASE) return openpete_subtick_host_span(a, span);
    if (a < TSRC_TERR_BASE || a >= TSRC_TEX_BASE || !s_tsrc_terr) return NULL;
    const uint32_t o = a - TSRC_TERR_BASE;
    return (o <= s_tsrc_terr_len && span <= s_tsrc_terr_len - o) ? s_tsrc_terr + o : NULL;
}

/** @brief Tests whether an address names an imported source sector (TSRC_TERR_BASE window).
 * @param s The sector address.
 * @return 1 for an imported sector. */
static int tsv_is_virtual(uint32_t s) { return s >= TSRC_TERR_BASE && s < TSRC_TEX_BASE; }

/** @brief Prints a stderr census of the game's terrain state: header counts, a few sectors, sample prims.
 *
 * Prints the VBL, sector, occlusion-group and texture counts, the LQ and HQ
 * texture table addresses, m_LodDistance, the cull distance and the camera
 * position, then the decoded prim count with a histogram of GPU command
 * codes. The first three sectors whose visibility byte is set are decoded
 * (centre, radius, flags, origin, LP and HP vertex, colour and face counts,
 * and the first two HP vertices in the 11/11/10 form), followed by up to four
 * sample prims from s_gte. Reads guest RAM only.
 */
static void terrain_dump(CPUState* cpu) {
    uint32_t sectorPtr = tenv_rd(cpu, game_layout.env.sector_ptr);
    uint32_t sectorCnt = tenv_rd(cpu, game_layout.env.sector_count);
    uint32_t occCnt    = game_layout.env.occ_group_count ? tenv_rd(cpu, game_layout.env.occ_group_count) : 0;
    uint32_t texCnt    = tenv_rd(cpu, game_layout.env.tex_count);
    uint32_t lqTex     = tenv_rd(cpu, game_layout.env.lq_tex);
    uint32_t hqTex     = tenv_rd(cpu, game_layout.env.hq_tex);
    int lodDist        = (int)tenv_lod_dist(cpu);
    int cullDist       = (int)tenv_rd(cpu, game_layout.env.cull_dist);
    int camX = (int)rd(cpu, game_layout.cam.position);
    int camY = (int)rd(cpu, game_layout.cam.position + 4u);
    int camZ = (int)rd(cpu, game_layout.cam.position + 8u);

    fprintf(stderr,
        "[terrain] frame=%u  sectors=%u occGroups=%u texCnt=%u  LQtex=%08X HQtex=%08X\n"
        "          lodDist=%X cullDist=%X  cam=(%d,%d,%d) cam/16=(%d,%d,%d)\n"
        "          prims: total=%u decoded=%d  codes:",
        openpete_frame, sectorCnt, occCnt, texCnt, lqTex, hqTex,
        lodDist, cullDist, camX, camY, camZ, camX >> 4, camY >> 4, camZ >> 4,
        s_prim_total, s_gte_n);
    for (int i = 0; i < 256; i++)
        if (s_code_hist[i]) fprintf(stderr, " %02X:%u", i, s_code_hist[i]);
    fprintf(stderr, "\n");

    /* Decode the first few VISIBLE sectors (D_800771C8[i]==0xFF) to confirm the
     * header count-prefix + the 11/11/10 HP vertex format against live data. */
    int shown = 0;
    for (uint32_t i = 0; i < sectorCnt && shown < 3; i++) {
        if (!tvis_visible(cpu, i)) continue;
        uint32_t s = rd(cpu, sectorPtr + i * 4);
        if (!s) continue;
        uint32_t w0 = rd(cpu, s + 0x00), w1 = rd(cpu, s + 0x04);
        uint32_t og = rd(cpu, s + 0x08);     /* originX<<16 | originY */
        uint32_t oz = rd(cpu, s + 0x0C);     /* originZ<<16 | flistId */
        uint32_t lpw = rd(cpu, s + 0x10), hpw = rd(cpu, s + 0x14);
        int centreX = (int)(w0 >> 16), centreY = (int)(w0 & 0xFFFF), centreZ = (int)(w1 >> 16);
        int radius  = thdr_radius(w1), flags = thdr_flags(w1), flat = thdr_flat(w1);
        int originX = (int)(og >> 16), originY = (int)(og & 0xFFFF), originZ = (int)(oz >> 16);
        int nLpV = lpw & 0xFF, nLpC = (lpw >> 8) & 0xFF, nLpF = (lpw >> 16) & 0xFF;
        int nHpV = hpw & 0xFF, nHpC = (hpw >> 8) & 0xFF, nHpF = (hpw >> 16) & 0xFF;
        int hpVertsOff = (int)((hpw >> 24) & 0xFF);   /* ×4 = byte offset to HP verts */
        fprintf(stderr,
            "  sector[%u] @%08X  centre=(%d,%d,%d) r=%d flags=%04X  origin=(%d,%d,%d)\n"
            "            LP{v=%d c=%d f=%d}  HP{v=%d c=%d f=%d hpVertsOff=%d(*4=%d)}\n",
            i, s, centreX, centreY, centreZ, radius, flags, originX, originY, originZ,
            nLpV, nLpC, nLpF, nHpV, nHpC, nHpF, hpVertsOff, hpVertsOff * 4);
        /* Decode the first 2 HP verts: 11/11/10, world/16 = origin_field + offset. */
        uint32_t hpVerts = s + 0x1C + (uint32_t)(hpVertsOff * 4);
        for (int k = 0; k < 2 && k < nHpV; k++) {
            uint32_t v = rd(cpu, hpVerts + (uint32_t)k * 4);
            int vx, vy, vz; tvert_unpack(v, flat, &vx, &vy, &vz);
            fprintf(stderr,
                "            hpVert[%d]=%08X  off=(%d,%d,%d)  world/16=(%d,%d,%d)\n",
                k, v, vx, vy, vz, originX + vx, originY - vy, originZ - vz);
        }
        shown++;
    }

    /* A few decoded sample prims (SXY / RGB / UV / tpage). */
    int samples = s_gte_n < 4 ? s_gte_n : 4;
    for (int p = 0; p < samples; p++) {
        prim_t* t = &s_gte[p];
        fprintf(stderr, "  prim[%d] code=%02X nv=%d tex=%d gour=%d  v0=(%.0f,%.0f) rgb0=%06X",
                p, t->code, t->nv, t->textured, t->gouraud, t->x[0], t->y[0], t->rgb[0]);
        if (t->textured) fprintf(stderr, "  uv0=(%d,%d) clut=%04X tpage=%04X", t->u[0], t->v[0], t->clut, t->tpage);
        fprintf(stderr, "\n");
    }

}

/* ---- native gamestate walk (float) + geometry A/B ------------------------- */

/* Projection params (file-static so the clip/emit path can re-project clipped
 * vertices). Set per frame in terrain_build. */
static double s_ofx, s_ofy, s_H;

/** @brief Projects a camera-space point to the screen: the float mirror of RTPS with TR = 0.
 *
 * The camera-space MAC = (M*V)>>12 is ngte_mac (shared); SX/SY = OFX/OFY +
 * MAC1/2 * H / MAC3. The LP-world/16 vs HP-world/4 scale cancels in the
 * MAC1/MAC3 ratio, so V can be built in any consistent unit (world/16).
 * MAC3 is clamped to at least 1e-9, so a point at or behind the eye
 * projects to a finite, far off-screen position.
 * @param m1 Camera-space X (MAC1).
 * @param m2 Camera-space Y (MAC2).
 * @param m3 Camera-space depth (MAC3).
 * @param sx Receives the screen X.
 * @param sy Receives the screen Y. */

static void proj_screen(double m1, double m2, double m3, float* sx, float* sy) {
    double z = (m3 > 1e-9) ? m3 : 1e-9;
    *sx = (float)(s_ofx + m1 * s_H / z);
    *sy = (float)(s_ofy + m2 * s_H / z);
}

/** @brief Tests whether a point lies inside or on the edge of a screen triangle, by edge-function signs.
 *
 * Either winding passes. Used only by the OPENPETE_TERRAIN_SECTORDUMP
 * coverage-gap grid, which marks grid cells covered by native triangles and
 * by captured prims to find the cyclorama-bleed band.
 * @param px The point's X.
 * @param py The point's Y.
 * @param ax Corner A's X.
 * @param ay Corner A's Y.
 * @param bx Corner B's X.
 * @param by Corner B's Y.
 * @param cx Corner C's X.
 * @param cy Corner C's Y.
 * @return 1 when inside or on an edge, else 0. */
static int pt_in_tri(float px, float py, float ax, float ay,
                     float bx, float by, float cx, float cy) {
    float d1 = (px-bx)*(ay-by) - (ax-bx)*(py-by);
    float d2 = (px-cx)*(by-cy) - (bx-cx)*(py-cy);
    float d3 = (px-ax)*(cy-ay) - (cx-ax)*(py-ay);
    int neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    int pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(neg && pos);
}

#define MAX_TVERTS 16384
static float s_tvx[MAX_TVERTS], s_tvy[MAX_TVERTS];
static double s_tvz[MAX_TVERTS];
static int s_tv_n;
static int s_walk_sectors;

/* Built triangles for the scene feed. */
typedef struct {
    float sx[3], sy[3]; double vz[3]; uint32_t rgb[3];
    float u[3], v[3];                              /* page-relative texel coords */
    int textured, tpx, tpy, clx, cly, bpp, blend;  /* PSX material (VRAM px) */
    int fog_delta;                                 /* CLUT ladder row (see tmat) */
    int16_t texid;                                 /* Tiledef/record id, -1 untex */
    int tier;                                      /* 1=LP, 0=HP (debug tiercolor) */
    uint8_t guard;                                 /* .s-deferred face (portal-guard dup) */
    uint8_t nearband;                              /* .s near-band classified: no og key */
    uint8_t fill;                                  /* LP hole-fill backing -> WORLD_FILL */
    uint8_t fptag;                                 /* fill-tag inspector: 1 = listed FILL, 2 = preview candidate */
    int16_t sec, face;                             /* source identity (fardbg attribution) */
    uint8_t env;                                   /* env-anim tile member (provenance tag) */
    float otkey;                                   /* parent face's .s OT key (full-world mean
                                                    * vz of the 4 corners; portal conformance) */
    uint16_t otslots;                              /* .s OT-slot depth offset for the TEST depth:
                                                    * LP 0x40+8*((w1>>3)&0x1F), HP 4*((w3>>3)&0x7);
                                                    * 1 slot = 128 world units */
    float fx[3], fy[3];                            /* drain face-local coordinate per vertex */
    uint32_t kc[8];                                /* drain sub-cell key table of a face the
                                                    * drain inserts finer than this triangle
                                                    * (otkey is the least sub-cell key); all
                                                    * zero otherwise (psx_key_contest.glsl) */
    float kn[12];                                  /* the face's fold-space corners (tn_fold_corner),
                                                    * set when a quadrant of it folds */
} ttri;
static int s_emit_noHP = 0;     /* tier of the sector currently being emitted */
static int s_emit_guard = 0;    /* current face is .s-DEFERRED (near band) -> portal-guard dup */
static int s_emit_fill = 0;     /* current pass is the LP hole-fill backing */
static int s_emit_fptag = 0;    /* current LP face's fill-tag class (inspector tint) */
static int s_emit_sec = -1, s_emit_face = -1;  /* fardbg identity of the current face */
static int s_emit_env = 0;      /* current face is in the env-anim rewrite set */
static float s_emit_otkey = 0.0f;  /* current face's .s OT key (portal conformance) */
static uint16_t s_emit_otslots = 0;  /* current face's authored OT-slot offset */
static uint8_t  s_emit_nearband = 0; /* current face is .s near-band (defer/HQ classified):
                                      * NEVER an OT face, so it neither emits nor contests
                                      * the og key raster - near geometry resolves by
                                      * TRUE depth, like the mask-bit-protected defer queues */
/* The walk's triangle stream (before the scene feed), sized by the walk: a
 * dense extended-LOD vista routes every eligible face through the 4x4 cell
 * path and emits well past 100k triangles. s_tri reads the run's current
 * base; a growth moves it, so no pointer into it outlives an append. */
#define TN_TRI_INIT 65536   /* first allocation; the former fixed capacity */
static mod_fvec s_tri_v;
#define s_tri ((ttri*)s_tri_v.data)
static int s_tri_n;

static int s_dbg_faces, s_dbg_oob, s_dbg_nclip, s_dbg_behind, s_dbg_spike;  /* diagnostics */
static int s_dbg_satback, s_dbg_satemit;   /* flagged HP faces the cull drops / keeps against the projected NCLIP */
static int s_dbg_spike_tris;   /* triangles the near/HQ subdivide emitted this walk */
static int s_trisplit_faces[2], s_trisplit_subs[2], s_trisplit_scull[2], s_trisplit_matdiff[2];
                                   /* tn_wctx trisplit_* sums, [0] tier 4, [1] tier 2 */
static int s_cells4_faces[2];      /* tn_wctx cells4_faces sums */
static int s_drain_skip[3];        /* tn_wctx drain_skip sums: [0] skipKeyed, [1] skipMasked, [2] foldedQuads */
static int s_drain_skip_legacy;    /* OPENPETE_TERRAIN_DRAIN_SKIP=0: pack a skipped slot as delta 0 (A/B leg) */
static int s_fardbg = -1, s_fardbg_done = 0, s_fardbg_active = 0;  /* OPENPETE_TERRAIN_FARDBG */
static int s_fardbg_budget = -1;   /* OPENPETE_TERRAIN_FARDBG_N: walks still to dump */
static int s_fardbg_lean = -1;     /* OPENPETE_TERRAIN_FARDBG_LEAN: skip the per-prim/per-tri lines */
static int s_fardbg_walk = 0;      /* serial of the walk being dumped, 0-based */
static long s_fardbg_occl0 = 0;    /* s_tocc_culled at the dumped walk's start */
static int s_facemap = -1, s_facemap_done = 0, s_facemap_active = 0;  /* OPENPETE_TERRAIN_FACEMAP */

/* OPENPETE_TERRAIN_SECTORDUMP: per-sector coverage record for the LP<->HP gap probe */
typedef struct {
    int idx, flags, noHP, nLpV, nLpF, nHpV, nHpF, nv, nf, tris, vfront;
    int ox, oy, oz;
    double m3min, m3max;
    float bx0, by0, bx1, by1;
} secrec;
static secrec s_sd[1024]; static int s_sd_n;
static int s_sectordump = -1;
static int s_sd_active = 0;
static int s_cull = -1, s_flat = -1;              /* debug toggles (env) */

/* Env resolve for the policy toggles: the dev overlay's setenv is their live
 * channel, but the walks read them per face/sector - too hot for getenv. Each
 * site keeps a parse cache and re-resolves when the registry env epoch
 * changes (one getenv per edit). */
typedef struct { int v; unsigned ep; } tn_envvbl_t;
/** @brief Returns a policy toggle's value, re-reading the variable only when the registry env epoch changed.
 *
 * @param c The site's parse cache, zero-initialised.
 * @param name The environment variable.
 * @param def The value when the variable is unset or empty.
 * @param boolean 1 to parse as a flag (anything but "0"), 0 as an integer.
 * @return The value. */
static int tn_env_vbl(tn_envvbl_t* c, const char* name, int def, int boolean) {
    if (knob_env_epoch_stale(&c->ep)) {
        const char* e = getenv(name);
        c->v = (e && *e) ? (boolean ? (*e != '0') : atoi(e)) : def;
    }
    return c->v;
}

/** @brief Whether OPENPETE_TERRAIN_VERIFY arms the deferred-queue oracle, re-read when the registry env epoch changes. */
static int terrain_verify_env(void) {
    static tn_envvbl_t c;
    return tn_env_vbl(&c, "OPENPETE_TERRAIN_VERIFY", 0, 1);
}

/** @brief Latches the OPENPETE_TERRAIN_CULL and OPENPETE_TERRAIN_FLAT toggles into s_cull and s_flat.
 *
 * s_cull is a bit mask, default 3: bit 0 enables the NCLIP back-face cull of
 * the HP walk, bit 1 that of the LP walk, and 0 disables both. s_flat,
 * default 0, makes the feed draw every face in one flat debug colour. Each
 * value is re-read when the registry env epoch changes.
 */
static void terrain_dbg_init(void) {
    /* NCLIP back-face cull, default ON for the HP tier (bit0). A Z-buffer
     * hides *occluded* back-faces but not back-faces that sit NEARER than the
     * front geometry: back-to-back wall pairs (a room interior vs the
     * cliff/waterfall outside it) interpenetrate within a few world units, so
     * emit-all let the far surface's back-face win depth and punch through
     * (magic_crafters waterfall room). The .s NCLIP verdict - the
     * exact two-half quad rule, see the emit-site comment - is the canonical
     * visibility rule; the old "dropped ~half the faces" failure was the
     * single-triple mirror missing the second-half acceptance.
     * OPENPETE_TERRAIN_CULL is a bitmask: bit0 = HP cull, bit1 = LP cull,
     * both default ON. Each tier mirrors its own walk's exact per-half NCLIP
     * rule (HP: (i0,i1,i3)/(i2,i1,i3) with the w3 flip/two-sided bits; LP:
     * (i0,i1,i2)/(i3,i1,i2), fixed strict signs, w0 bit7 double-sided - see
     * the emit-site comment). The old "mixed winding" LP failure was the HP
     * corner order and sign applied to the LP walk. "0" disables everything. */
    static tn_envvbl_t cc, fc;
    s_cull = tn_env_vbl(&cc, "OPENPETE_TERRAIN_CULL", 3, 0);
    s_flat = tn_env_vbl(&fc, "OPENPETE_TERRAIN_FLAT", 0, 1);
}

/* Captured prim corner colours = the .s DPCS-fogged output, used as the ORACLE
 * for the native-colour A/B (terrain_compare). Not fed to the scene. */
static float s_ccx[MAX_PRIMS * 4], s_ccy[MAX_PRIMS * 4];
static uint32_t s_ccrgb[MAX_PRIMS * 4]; static int s_cc_n;
/** @brief Flattens the decoded guest prims into the corner arrays s_ccx, s_ccy and s_ccrgb.
 *
 * Appends each corner's screen x, y and colour of every prim in s_gte, in
 * order, and sets s_cc_n; it stops once fewer than four slots remain of the
 * MAX_PRIMS * 4 capacity. The arrays are the reference the colour comparison
 * in terrain_build matches native corners against; they do not reach the
 * scene.
 */
static void build_captured_colors(void) {
    s_cc_n = 0;
    for (int p = 0; p < s_gte_n && s_cc_n < MAX_PRIMS * 4 - 4; p++) {
        prim_t* t = &s_gte[p];
        for (int k = 0; k < t->nv; k++) {
            s_ccx[s_cc_n] = t->x[k]; s_ccy[s_cc_n] = t->y[k]; s_ccrgb[s_cc_n] = t->rgb[k];
            s_cc_n++;
        }
    }
}

/* Native per-vertex terrain colour: RGB888 read from the sector colour arrays +
 * the per-vertex DPCS depth-cue fog. The HP colour block is TWO 4B-stride
 * arrays [near0..][far0..]; `idx` is the pre-×4 byte offset from face word1.
 *
 * The .s decode (generated C block_80026DF0-80026E6C):
 *   ir0 = fogRef - SZ,  SZ = 4·m3 (native V is world/16; the .s HP walk feeds
 *                                  world/4, so the .s SZ3 = 4× our m3);
 *   ir0 >= 4096 -> NEAR colour raw (block_80026FB4);
 *   ir0 <= 0    -> FAR colour raw (block_80026FAC; the .s DOES fully fog);
 *   else GTE DPCS (cmd 0x10): seed RGBC = FAR word ($s0[idx]), far-colour regs
 *   = NEAR word 12-bit-expanded ($t7[idx]) -> out = far + ir0·(near-far) with
 *   the GTE's truncating >>12 >>4 per channel (ngte_dpcs_ch).
 *
 * DEFAULT: the EXACT decode. PSX terrain fog is TWO channels: the DPCS vertex
 * blend here AND a per-face CLUT-ROW ladder of pre-baked progressively-fogged
 * palettes (see the .s texture-fog select at the face loop). The exact decode
 * is paired with the ladder (s_clutfog); the DPCS blend alone leaves textured
 * faces under-fogged. OPENPETE_TERRAIN_FOGDPCS=0 restores the float lerp
 * (A/B); OPENPETE_TERRAIN_LOD_HANDOFF=0 restores HP-everywhere beyond fog
 * saturation (A/B).
 * Colour words are 0x00BBGGRR (R=byte0), matching the captured prim RGB. LP
 * faces are NEVER fogged in the .s (raw working palette at all distances);
 * noHP=1 keeps that. */
static double s_fogscale = -1.0;    /* SZ = fogscale·m3; shipped default 1.0 (see above) */
static int s_fogdpcs = -1;          /* OPENPETE_TERRAIN_FOGDPCS: GTE-exact DPCS blend */
static int s_lod_handoff = -1;      /* OPENPETE_TERRAIN_LOD_HANDOFF: HP->LP swap beyond fog saturation, default on */
static int s_gapfill_en = -1;       /* OPENPETE_TERRAIN_GAPFILL (near-LP-gap fill, default on) */
static int s_tier_faithful = -1;    /* OPENPETE_TERRAIN_LOD_FAITHFUL: .s sector binning + exact fog */
static int s_clutfog = -1;          /* OPENPETE_TERRAIN_CLUTFOG: .s per-face CLUT-row fog ladder (default ON) */
int s_fogdbg = 0;                   /* OPENPETE_TERRAIN_FOGDBG: dump fog samples */
extern int s_fogdbg;
static uint32_t terrain_fog_blend(uint32_t nearC, uint32_t farC, double m3, int fogRef);
static uint32_t terrain_fog_blend_inner(uint32_t nearC, uint32_t farC, double m3, int fogRef);
/** @brief Returns the fogged terrain vertex colour and logs the first samples when OPENPETE_TERRAIN_FOGDBG is set.
 *
 * The colour is terrain_fog_blend_inner's. With s_fogdbg set, the first 6000
 * calls print m3, fogRef and the near, far and result colours to stderr; the
 * counter is not atomic, so calls from concurrent walk workers can race on it.
 * @param nearC The near colour, 0x00BBGGRR.
 * @param farC The far colour, 0x00BBGGRR.
 * @param m3 The vertex's view-Z in world/16.
 * @param fogRef The fog reference depth.
 * @return The blended colour, 0x00BBGGRR.
 */
static uint32_t terrain_fog_blend(uint32_t nearC, uint32_t farC, double m3, int fogRef) {
    uint32_t out = terrain_fog_blend_inner(nearC, farC, m3, fogRef);
    static int s_fs_n;
    if (s_fogdbg && s_fs_n < 6000) {
        fprintf(stderr, "[fogsample] m3=%.1f fogRef=%d near=%06X far=%06X out=%06X\n",
                m3, fogRef, nearC, farC, out);
        s_fs_n++;
    }
    return out;
}
/** @brief Blends a vertex's near and far colours by its depth-cue factor.
 *
 * Under an active texture pack fog shift the far colour is first re-graded
 * per channel (scale times far plus offset, rounded, clamped to 0..255); the
 * near colour is untouched. With s_fogdpcs set, ir0 = fogRef - (int)(m3 *
 * s_fogscale) drives the GTE DPCS blend of the game's terrain fog: ir0 >=
 * 4096 returns the near colour, ir0 <= 0 the far colour, and otherwise each
 * channel is ngte_dpcs_ch(far, near << 4, ir0). With s_fogdpcs clear, a
 * float lerp far + f * (near - far) + 0.5 with f = (fogRef - m3 * s_fogscale)
 * / 4096 clamped to 0..1 replaces it.
 * @param nearC The near colour, 0x00BBGGRR.
 * @param farC The far colour, 0x00BBGGRR.
 * @param m3 The vertex's view-Z in world/16.
 * @param fogRef The fog reference depth.
 * @return The blended colour, 0x00BBGGRR.
 */
static uint32_t terrain_fog_blend_inner(uint32_t nearC, uint32_t farC, double m3, int fogRef) {
    /* Pack fog shift (texture_pack_fog_shift_set): a full-coverage re-graded
     * pack needs the DPCS vertex-fog TARGET re-graded with it - the authored
     * FAR colours are the stock ambience, and modulating dark pack texels
     * toward them re-brightens the distance the pack darkened. Near colours
     * (lighting) stay stock; only the fog destination moves. 0x00BBGGRR. */
    {
        float fs[3], fo[3];
        if (texture_pack_fog_shift_get(fs, fo)) {
            int r = (int)(fs[0] * (float)(farC & 0xFF)         + fo[0] + 0.5f);
            int g = (int)(fs[1] * (float)((farC >> 8) & 0xFF)  + fo[1] + 0.5f);
            int b = (int)(fs[2] * (float)((farC >> 16) & 0xFF) + fo[2] + 0.5f);
            if (r < 0) r = 0; if (g < 0) g = 0; if (b < 0) b = 0;
            if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
            farC = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16);
        }
    }
    if (s_fogdpcs) {                      /* GTE-exact DPCS path (default) */
        int ir0 = fogRef - (int)(m3 * s_fogscale);
        if (ir0 >= 4096) return nearC;
        if (ir0 <= 0)    return farC;
        int r = ngte_dpcs_ch((int)(farC & 0xFF),          (int)((nearC & 0xFF) << 4),         ir0);
        int g = ngte_dpcs_ch((int)((farC >> 8) & 0xFF),   (int)(((nearC >> 8) & 0xFF) << 4),  ir0);
        int b = ngte_dpcs_ch((int)((farC >> 16) & 0xFF),  (int)(((nearC >> 16) & 0xFF) << 4), ir0);
        return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16);
    }
    /* s_fogdpcs == 0: the float+0.5 lerp (OPENPETE_TERRAIN_FOGDPCS=0). It
     * under-fogs vs the .s: f pins to 1 across most of the fog band. */
    double f = ((double)fogRef - m3 * s_fogscale) / 4096.0;
    if (f < 0.0) f = 0.0; else if (f > 1.0) f = 1.0;
    int nr = (int)(nearC & 0xFF), ng = (int)((nearC >> 8) & 0xFF), nb = (int)((nearC >> 16) & 0xFF);
    int fr = (int)(farC & 0xFF),  fg = (int)((farC >> 8) & 0xFF),  fb = (int)((farC >> 16) & 0xFF);
    int r = (int)((double)fr + f * (nr - fr) + 0.5);
    int g = (int)((double)fg + f * (ng - fg) + 0.5);
    int b = (int)((double)fb + f * (nb - fb) + 0.5);
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16);
}
/** @brief Reads one vertex's terrain colour from the sector colour arrays and fogs it.
 *
 * An LP face (noHP) returns the raw LP word at lpBase + idx, never fogged, as
 * the game's LP walk draws it. An HP face reads the near word at nearBase +
 * idx and the far word at farBase + idx, masks both to 24 bits and returns
 * their terrain_fog_blend. idx is the face word 1 byte offset, already
 * multiplied by 4.
 * @param cpu The guest CPU state.
 * @param noHP 1 for an LP face.
 * @param lpBase The LP colour array's address.
 * @param nearBase The HP near colour array's address.
 * @param farBase The HP far colour array's address.
 * @param idx The vertex colour's byte offset in its array.
 * @param m3 The vertex's view-Z in world/16.
 * @param fogRef The fog reference depth.
 * @return The colour, 0x00BBGGRR.
 */
static uint32_t terrain_color(CPUState* cpu, int noHP, uint32_t lpBase,
                              uint32_t nearBase, uint32_t farBase,
                              int idx, double m3, int fogRef) {
    if (noHP) return rd(cpu, lpBase + (uint32_t)idx) & 0xFFFFFFu;
    uint32_t nearC = rd(cpu, nearBase + (uint32_t)idx) & 0xFFFFFFu;
    uint32_t farC  = rd(cpu, farBase  + (uint32_t)idx) & 0xFFFFFFu;
    return terrain_fog_blend(nearC, farC, m3, fogRef);
}

/* Per-sector local projected vertex (indexed by face vertex index). Carries the
 * camera-space (m1,m2,m3) for near-plane clipping + the projected screen sx,sy. */
typedef struct { float sx, sy; double m1, m2, m3, vz; int ok; } lvert;

/* ---- LP fill-tag inspector: the crosshair pick (openpete_fillpick.h) -------
 * "Which face am I looking at, and does authored HP cover this sightline?" -
 * answered in the walk's own projected space rather than by a world-space ray.
 * The pick point is the projection centre (OFX, OFY): that IS the optical
 * axis, so it lands on the window centre at any aspect and under the 240->224
 * overscan crop, and nothing has to reconstruct the (non-orthonormal) Spyro
 * camera basis. Every face the walk projects is tested, drawn or dropped, so
 * the LP face that WOULD cover a hole is pickable before it is tagged.
 *
 * Two winners are kept per present: the nearest LP face (the fill-tag
 * candidate) and the nearest HP face (the coverage question). Their depths
 * settle the verdict - no HP hit means nothing authored covers the sightline
 * (FILL); an HP hit BEHIND the LP means the coarse LP is punching through
 * cover that already exists (NOFILL). */
/* The pick point set. Point 0 is the crosshair, published to the overlay;
 * OPENPETE_FILLPICK_GRID="COLSxROWS" adds a grid over the visible frame so one
 * pose enumerates every fill-needed sightline in view instead of one at a
 * time. Frame extents come from the projection itself: OFX is the horizontal
 * centre (so the half-width is OFX) and the visible band is the 224 lines
 * centred on OFY, after the 240->224 overscan crop. */
#define FP_MAX_PTS 4096
typedef struct {
    double px, py;
    int    lp_sec, lp_face, lp_quad, lp_cand;
    double lp_z;
    int    hp_sec, hp_face;
    double hp_z;
} fp_point;

static int      s_fp_on;                  /* inspector engaged for this walk */
static fp_point s_fp_pt[FP_MAX_PTS];
static int      s_fp_np;
static double   s_fp_bx0, s_fp_by0, s_fp_bx1, s_fp_by1;   /* bbox of the point set */
static int      s_fp_grid_cols, s_fp_grid_rows;

/** @brief Lays out this walk's pick points and their bounding box.
 *
 * Point 0 sits at the crosshair (OFX, OFY), moved by
 * OPENPETE_FILLPICK_XY="dx,dy", offsets in [-1, 1] of the visible half-frame
 * (OFX wide, 112 lines high), so a scripted aim needs no knowledge of the
 * frame width at this aspect. OPENPETE_FILLPICK_GRID="COLSxROWS" appends one
 * point per cell centre; rows are cut so the set fits 4096 points. Every
 * point starts with no LP and no HP hit. */
static void fp_begin(void) {
    double hw = s_ofx, hh = 112.0;        /* visible half-extents, walk frame */
    s_fp_np = 0;
    fp_point* p0 = &s_fp_pt[s_fp_np++];
    memset(p0, 0, sizeof *p0);
    p0->px = s_ofx; p0->py = s_ofy;
    const char* xy = getenv("OPENPETE_FILLPICK_XY");
    if (xy && *xy) {
        double dx, dy;
        if (sscanf(xy, "%lf,%lf", &dx, &dy) == 2) {
            p0->px = s_ofx + dx * hw;
            p0->py = s_ofy + dy * hh;
        }
    }
    s_fp_grid_cols = s_fp_grid_rows = 0;
    const char* g = getenv("OPENPETE_FILLPICK_GRID");
    if (g && *g) {
        int cols = 0, rows = 0;
        if (sscanf(g, "%dx%d", &cols, &rows) == 2 && cols > 0 && rows > 0) {
            if ((long)cols * rows > FP_MAX_PTS - 1) { rows = (FP_MAX_PTS - 1) / cols; }
            s_fp_grid_cols = cols; s_fp_grid_rows = rows;
            /* Cell centres, so a sample never lands on a shared edge. */
            for (int r = 0; r < rows; r++)
                for (int c = 0; c < cols; c++) {
                    fp_point* p = &s_fp_pt[s_fp_np++];
                    memset(p, 0, sizeof *p);
                    p->px = s_ofx + (2.0 * (c + 0.5) / cols - 1.0) * hw;
                    p->py = s_ofy + (2.0 * (r + 0.5) / rows - 1.0) * hh;
                }
        }
    }
    s_fp_bx0 = s_fp_by0 = 1e30; s_fp_bx1 = s_fp_by1 = -1e30;
    for (int k = 0; k < s_fp_np; k++) {
        fp_point* p = &s_fp_pt[k];
        p->lp_sec = p->lp_face = p->hp_sec = p->hp_face = -1;
        if (p->px < s_fp_bx0) s_fp_bx0 = p->px;
        if (p->px > s_fp_bx1) s_fp_bx1 = p->px;
        if (p->py < s_fp_by0) s_fp_by0 = p->py;
        if (p->py > s_fp_by1) s_fp_by1 = p->py;
    }
}

/** @brief Keeps each pick point's nearest hit of one projected triangle.
 *
 * A triangle with a vertex at or behind the camera plane, outside the point
 * set's bounding box, or degenerate on screen is skipped. The depth is
 * perspective-correct: 1/z interpolates linearly in screen space, so the
 * reciprocal of the barycentric-weighted reciprocals is the exact surface
 * view-Z at the sample for a planar triangle. An LP hit records the face's
 * quad and fill-candidate flags; an HP hit records the sector and face only.
 * @param lp 1 for a low-poly face, 0 for a high-poly face.
 * @param sec The sector index.
 * @param face The face index within the sector.
 * @param quad 1 when the face is a quad.
 * @param cand The face's fill-candidate flag.
 * @param A The first projected vertex.
 * @param B The second projected vertex.
 * @param C The third projected vertex. */
static void fp_hit_tri(int lp, int sec, int face, int quad, int cand,
                       const lvert* A, const lvert* B, const lvert* C) {
    if (A->m3 <= 1e-6 || B->m3 <= 1e-6 || C->m3 <= 1e-6) return;  /* straddles the camera plane */
    double ax = A->sx, ay = A->sy, bx = B->sx, by = B->sy, cx = C->sx, cy = C->sy;
    /* Screen bbox reject against the whole point set first: with a grid armed
     * this is what keeps the sweep from costing points x faces. */
    double tx0 = ax < bx ? ax : bx; if (cx < tx0) tx0 = cx;
    double tx1 = ax > bx ? ax : bx; if (cx > tx1) tx1 = cx;
    double ty0 = ay < by ? ay : by; if (cy < ty0) ty0 = cy;
    double ty1 = ay > by ? ay : by; if (cy > ty1) ty1 = cy;
    if (tx1 < s_fp_bx0 || tx0 > s_fp_bx1 || ty1 < s_fp_by0 || ty0 > s_fp_by1) return;
    double d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
    if (d > -1e-9 && d < 1e-9) return;                            /* degenerate on screen */
    for (int k = 0; k < s_fp_np; k++) {
        fp_point* P = &s_fp_pt[k];
        if (P->px < tx0 || P->px > tx1 || P->py < ty0 || P->py > ty1) continue;
        double w0 = ((by - cy) * (P->px - cx) + (cx - bx) * (P->py - cy)) / d;
        double w1 = ((cy - ay) * (P->px - cx) + (ax - cx) * (P->py - cy)) / d;
        double w2 = 1.0 - w0 - w1;
        if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0) continue;
        double inv = w0 / A->m3 + w1 / B->m3 + w2 / C->m3;
        if (inv <= 1e-12) continue;
        double z = 1.0 / inv;
        if (lp) {
            if (P->lp_sec < 0 || z < P->lp_z) {
                P->lp_sec = sec; P->lp_face = face;
                P->lp_quad = quad; P->lp_cand = cand; P->lp_z = z;
            }
        } else if (P->hp_sec < 0 || z < P->hp_z) {
            P->hp_sec = sec; P->hp_face = face; P->hp_z = z;
        }
    }
}

/** @brief Tests one face's triangles against the pick points.
 *
 * A quad tests both strip triangles, matching the emit's (i0, i1, i2) +
 * (i1, i2, i3) triangulation so the picked region is exactly the drawn
 * region; a triangle tests (i0, i1, i2) only.
 * @param lp 1 for a low-poly face, 0 for a high-poly face.
 * @param sec The sector index.
 * @param face The face index within the sector.
 * @param cand The face's fill-candidate flag.
 * @param sv The sector's projected vertices.
 * @param i0 The first corner's vertex index.
 * @param i1 The second corner's vertex index.
 * @param i2 The third corner's vertex index.
 * @param i3 The fourth corner's vertex index; read only for a quad.
 * @param quad 1 when the face is a quad. */
static void fp_test_face(int lp, int sec, int face, int cand, const lvert* sv,
                         int i0, int i1, int i2, int i3, int quad) {
    fp_hit_tri(lp, sec, face, quad, cand, &sv[i0], &sv[i1], &sv[i2]);
    if (quad) fp_hit_tri(lp, sec, face, quad, cand, &sv[i1], &sv[i2], &sv[i3]);
}

/* A face corner: native colour + page-relative texel UV. */
typedef struct { uint32_t rgb; float u, v; } lcorner;
/* A face's texture material (decoded from the Tiledef; VRAM-pixel units).
 * clut_raw keeps the undecoded CLUT halfword: the .s HP walk ADDS a per-face
 * fog row offset to it per frame (Δ<<6 -> cly += Δ) before decoding. */
/* fog_delta is the ladder row offset applied to clut_raw (0 = unfogged base
 * row). Carried alongside the decoded CLUT so consumers can recover the base
 * palette the texture was authored against: the texture-replacement registry
 * keys identity on the BASE row, so one pack file covers a texture at every
 * fog distance instead of the ladder fanning it into 15 near-duplicates. */
/* bank 1 samples the auxiliary VRAM bank (openpete_native_vram_aux_set):
 * the emitted page and CLUT rows carry +512. */
typedef struct { int textured, tpx, tpy, clx, cly, bpp, blend, clut_raw, fog_delta;
                 int texid; /* Tiledef/record id (word2 & 0x7F), -1 = untextured */
                 int bank; } tmat;

/** @brief Decodes one 8-byte Tiledef entry into a material and the tile's four spatial corner UVs.
 *
 * The entry is word0 = {u8 u0, u8 v0, u16 clut}, word1 = {u8 u1, u8 v1,
 * u16 tpage}. The corners, in [TL, TR, BL, BR] order, are TL = (u0, v0),
 * TR = (u1, v1), BL = (u0, v0 + span), BR = (u0 + span, v0 + span), each
 * wrapped to a byte, matching the GT4 prim emit (0x800270AC). On a layout
 * whose word1 is the BR corner (terrain.tile_word1_br), TR's v is v1 - span.
 * The material is textured, with the tpage's page, depth and CLUT, the raw
 * CLUT halfword, and fog_delta 0 (the base row). Blend is 0 unless the face
 * is ABE, or the layout carries ABE in tpage bit 15 and that bit is set; an
 * ABE face takes the tpage's ABR mode. Serves the per-quad LQ path
 * (terrain_texmat) and the near/HQ subdivide paths (entries inside the HQ
 * cell record).
 * @param cpu The guest CPU state.
 * @param e The entry's byte address.
 * @param abe 1 when the face is semi-transparent.
 * @param m Receives the material; zeroed by the caller.
 * @param u Receives the four corner U texels.
 * @param v Receives the four corner V texels.
 * @param span The tile's texel extent minus one: 0x1F for the 32x32 LQ and
 *        2x2 quadrant tiles, 0x0F for the 16x16 4x4-cell tiles
 *        (r_environment.s 0x8002928C: BL = word0 + 0xF00, BR = word0 + 0xF0F). */
static void decode_tiledef_span(CPUState* cpu, uint32_t e, int abe, tmat* m,
                                float u[4], float v[4], int span) {
    uint32_t w0 = rd(cpu, e + 0), w1 = rd(cpu, e + 4);
    int u0 = (int)(w0 & 0xFF), v0 = (int)((w0 >> 8) & 0xFF), clut = (int)((w0 >> 16) & 0xFFFF);
    int u1 = (int)(w1 & 0xFF), v1 = (int)((w1 >> 8) & 0xFF), tpage = (int)((w1 >> 16) & 0xFFFF);
    if (game_layout.terrain.tile_word1_br) v1 = (v1 - span) & 0xFF;   /* word1 is the BR corner */
    u[0]=(float)u0;                v[0]=(float)v0;                   /* TL */
    u[1]=(float)u1;                v[1]=(float)v1;                   /* TR */
    u[2]=(float)u0;                v[2]=(float)((v0 + span) & 0xFF); /* BL */
    u[3]=(float)((u0+span)&0xFF);  v[3]=(float)((v0 + span) & 0xFF); /* BR */
    ngte_tpage tp;
    ngte_decode_tpage((unsigned)tpage, (unsigned)clut, 0u, &tp);
    m->textured = 1;
    m->tpx = tp.tpx; m->tpy = tp.tpy;
    m->bpp = tp.bpp;
    m->clx = tp.clx; m->cly = tp.cly;
    m->clut_raw = clut;
    m->fog_delta = 0;              /* base row; the HP walk ladders it per face */
    /* blend: opaque unless ABE; then the tpage ABR mode. */
    if (game_layout.terrain.abe_in_tpage && (tpage & 0x8000)) abe = 1;
    m->blend = abe ? ngte_abr_blend(tp.abr, NGTE_ABR_EXACT) : 0;
}
/** @brief Decodes a 32x32 tile's Tiledef entry with the full 0x1F texel span.
 *
 * decode_tiledef_span with span 0x1F: the LQ tiles and the 2x2 quadrant tiles
 * of the HQ record.
 * @param cpu The guest CPU state.
 * @param e The entry's byte address.
 * @param abe 1 when the face is semi-transparent.
 * @param m Receives the material.
 * @param u Receives the four corner U texels, [TL, TR, BL, BR].
 * @param v Receives the four corner V texels, [TL, TR, BL, BR].
 */
static void decode_tiledef(CPUState* cpu, uint32_t e, int abe, tmat* m, float u[4], float v[4]) {
    decode_tiledef_span(cpu, e, abe, m, u, v, 0x1F);
}

/** @brief Decodes a terrain face's texture material and its four corner UVs from the m_LQTexturePointer Tiledef table.
 *
 * texByte is the int8 of the HP face's word2: -1 (0xFF) is untextured (flat
 * gouraud, bpp 16, texid -1); otherwise the tile is texByte & 0x7F, opaque
 * when texByte >= 0 and semi-transparent (blend from the tpage ABR) when
 * negative. The entry is table + tile * terrain.tex_stride; each entry holds
 * two 8-byte tiles, [0..7] far/low-detail (typically 4-bit) and [8..15]
 * near/high-detail (typically 8-bit), and hiDetail selects the second. The
 * game picks the half per quad by depth at 0x80026F70; under --no-lod the
 * saturated m_LodDistance selects high detail everywhere. The game's GT4 emit
 * stores the last two SXY/UV slots swapped (slot2 = i3, slot3 = i2;
 * 0x80026DD0-DC), so callers map vertex i0 to TL, i1 to TR, i3 to BL and i2
 * to BR.
 * @param cpu The guest CPU state.
 * @param table The Tiledef table's address.
 * @param texByte The face's texture byte.
 * @param hiDetail 1 selects the near/high-detail half.
 * @param m Receives the material; texid is the tile index.
 * @param u Receives the four corner U texels.
 * @param v Receives the four corner V texels. */
static void terrain_texmat(CPUState* cpu, uint32_t table, int texByte, int hiDetail,
                           tmat* m, float u[4], float v[4]) {
    memset(m, 0, sizeof(*m)); m->bpp = 16; m->texid = -1;
    if (texByte == -1) { m->textured = 0; return; }      /* 0xFF: untextured */
    uint32_t e = table + (uint32_t)(texByte & 0x7F) * game_layout.terrain.tex_stride + (hiDetail ? 8u : 0u);  /* LOD half */
    decode_tiledef(cpu, e, (texByte < 0), m, u, v);      /* high bit = semi-transparent */
    m->texid = texByte & 0x7F;                           /* texture-selector key */
}

/* Native colour-verts (per emitted tri corner) for the colour A/B. An
 * instrument, not the draw: the compare runs over the first TN_CV_MAX
 * corners, and the triangles past them still draw. */
#define TN_CV_MAX (65536 * 3)
static float s_cvx[TN_CV_MAX], s_cvy[TN_CV_MAX];
static double s_cvz[TN_CV_MAX];                   /* m3 (world/16 view-Z) for fog regime */
static uint32_t s_cvrgb[TN_CV_MAX]; static int s_cv_n;
static int s_tex_faces, s_tex_tris;               /* diagnostics: textured-face count */
static int s_dbg_clipped;                          /* faces that hit the near plane */

/* ---- parallel sector walk (--extract-threads) ------------------------------
 * The face walk fans out over visible sectors on the extraction worker pool.
 * Each worker owns a tn_wctx: its output triangle buffer, the per-sector
 * projection scratch, the ambient per-face emit state the push path reads,
 * and every census counter the walk increments. Workers only read shared
 * state that the build prologue latched (matrices, env toggles, the decoded
 * sector caches - resolved serially in the prepass); everything they WRITE
 * lives here. Per-sector output runs are recorded and merged into s_tri in
 * sector order, so the merged stream is byte-identical to the serial walk. */
typedef struct {
    ttri* tri; int tri_n, tri_cap;      /* output: base and capacity of *out */
    mod_fvec* out;                       /* the run tri points into (serial: s_tri_v) */
    lvert sv[256];                       /* per-sector projected verts */
    /* ambient per-face emit state (consumed by push_tri) */
    int emit_noHP, emit_guard, emit_fill, emit_fptag, emit_sec, emit_face, emit_env;
    int emit_bank;                      /* the pass draws borrowed HP: auxiliary VRAM bank */
    float emit_otkey; uint16_t emit_otslots; uint8_t emit_nearband;
    uint8_t emit_keycells; uint32_t emit_kc[8];   /* the face's drain key table */
    uint8_t emit_fold; float emit_kn[12];         /* its folded quadrants and fold-space corners */
    /* census (summed into the s_dbg_* / s_tex_* globals after the walk) */
    int dbg_faces, dbg_oob, dbg_nclip, dbg_behind, dbg_spike, dbg_clipped;
    int dbg_satback, dbg_satemit;         /* tface_facing's verdict flips vs the projected NCLIP */
    int dbg_spike_tris;                  /* triangles emitted by the near/HQ subdivide */
    /* split triangles, [0] tier 4 and [1] tier 2: faces, sub-triangles drawn,
     * sub-triangles a straddler's facing culled, sub-triangles whose formed
     * CLUT/TPAGE words differ from the tile's own */
    int trisplit_faces[2], trisplit_subs[2], trisplit_scull[2], trisplit_matdiff[2];
    /* quads drawn as the 4x4 cells: [0] all, [1] those the drain's distance
     * test alone would draw as the 2x2 (routed by tface_cells_pin) */
    int cells4_faces[2];
    /* skipped drain-table slots packed this walk: [0] as delta 0 (keyed at
     * the face minimum), [1] as the skip sentinel; [2] folded 2x2 quadrants
     * of keyed quads (tn_quad_folds) */
    int drain_skip[3];
    int tex_faces, tex_tris;
    int near_q_agree, near_c_only;
    int walk_sectors;
    int serial;   /* 1 = single-thread walk: also fills the s_tv/s_cv A/B arrays */
} tn_wctx;

/* (tn_wcfg - the per-walk shared-input struct - is defined further down,
 * after tscache_t which it references.) */

/* Near plane (camera-space m3, world/16 view-Z). Vertices closer than this are
 * clipped so float H/m3 never blows up and behind-camera (m3<=0) verts don't
 * project to a wild position. Env-tunable. */
static double s_nearz = -1.0;
/* near/HQ subdivide pass enable (Route 2; OPENPETE_TERRAIN_NEARHQ, default ON). */
static int s_nearhq = -1;

/** @brief Ensures room for `need` triangles in the worker's output run, growing it.
 *
 * Each worker grows only its own run; the serial walk's run is s_tri_v. A
 * context with no output vector cannot grow.
 * @param W The walk context.
 * @param need The triangle count required.
 * @return 1 when the run holds `need` triangles, 0 on an allocation failure
 *         (logged by the vector), in which case the caller stores nothing. */
static int tn_room(tn_wctx* W, int need) {
    if (need <= W->tri_cap) return 1;
    if (!W->out || mod_fvec_reserve_noted(W->out, (size_t)need) != 0) return 0;
    W->tri = (ttri*)W->out->data; W->tri_cap = (int)W->out->cap;
    return 1;
}

/** @brief Pushes one near-clipped camera-space triangle into the walk's output run.
 *
 * Each corner is projected to the screen unless it carries a precomputed
 * screen position (oflag), and is floored under the integer vertex snap. The
 * triangle takes the material and the walk context's emit_* tags (tier,
 * guard, fill, sector and face, OT key and slots, key-cell table). The serial
 * walk also appends the projected corners to the colour A/B arrays.
 * @param W The walk context.
 * @param a The first corner.
 * @param b The second corner.
 * @param c The third corner.
 * @param m The triangle's material. */
static void push_tri(tn_wctx* W, ngte_cvert a, ngte_cvert b, ngte_cvert c, const tmat* m) {
    if (!tn_room(W, W->tri_n + 1)) return;
    ngte_cvert cc[3] = { a, b, c };
    ttri* t = &W->tri[W->tri_n++];
    int fsnap = openpete_native_vertex_snap_active();
    for (int k = 0; k < 3; k++) {
        if (cc[k].oflag) { t->sx[k] = cc[k].osx; t->sy[k] = cc[k].osy; }
        else {
            proj_screen(cc[k].m1, cc[k].m2, cc[k].m3, &t->sx[k], &t->sy[k]);
            /* Integer snap is applied HERE, feeder-side (the renderer skips
             * this feed - presnap contract): the near/HQ subdivide needs its
             * boundary midpoints at exact positions ON the snapped parent
             * edges (oflag above), which no per-vertex renderer snap could
             * preserve. Same floor as the renderer's (GTE MAC0>>16). */
            if (fsnap) { t->sx[k] = floorf(t->sx[k]); t->sy[k] = floorf(t->sy[k]); }
        }
        t->vz[k] = cc[k].m3; t->rgb[k] = cc[k].rgb; t->u[k] = cc[k].u; t->v[k] = cc[k].v;
        t->fx[k] = cc[k].fx; t->fy[k] = cc[k].fy;
        if (W->serial && s_cv_n < TN_CV_MAX) {       /* colour-A/B verts (verify) */
            s_cvx[s_cv_n]=t->sx[k]; s_cvy[s_cv_n]=t->sy[k]; s_cvz[s_cv_n]=cc[k].m3; s_cvrgb[s_cv_n]=cc[k].rgb; s_cv_n++;
        }
    }
    /* The auxiliary bank sits 512 rows below the PSX VRAM; its Tiledef ids
     * name the source level's table, so they key no selector. */
    const int bank = (m->bank || W->emit_bank) && m->textured;
    t->textured=m->textured; t->tpx=m->tpx; t->tpy=m->tpy + (bank ? 512 : 0);
    t->clx=m->clx; t->cly=m->cly + (bank ? 512 : 0); t->bpp=m->bpp; t->blend=m->blend;
    t->fog_delta=m->fog_delta;
    t->texid=(int16_t)(m->textured && !bank ? m->texid : -1);
    t->tier = W->emit_noHP;
    t->guard = (uint8_t)W->emit_guard;
    t->fill = (uint8_t)W->emit_fill;
    t->fptag = (uint8_t)W->emit_fptag;
    t->sec = (int16_t)W->emit_sec; t->face = (int16_t)W->emit_face;
    t->env = (uint8_t)W->emit_env;
    t->otkey = W->emit_otkey;
    t->otslots = W->emit_otslots;
    t->nearband = W->emit_nearband;
    if (W->emit_keycells) memcpy(t->kc, W->emit_kc, sizeof t->kc);
    else memset(t->kc, 0, sizeof t->kc);
    if (W->emit_keycells && W->emit_fold) memcpy(t->kn, W->emit_kn, sizeof t->kn);
    if (m->textured) W->tex_tris++;
}

/* Max distance (PSX px) a near-clip CROSSING vertex may project from the screen
 * centre. A face straddling the eye plane clipped at s_nearz (16 world units)
 * puts its crossing verts essentially AT the eye - they project 20k+ px
 * offscreen, and rasterizers (llvmpipe AND hardware) drop the resulting sliver
 * even though its on-screen wedge is large (the face disappears while the
 * camera is on its plane). Everything
 * beyond this bound is far outside any visible frame (±~600 PSX px at 32:9),
 * so re-clipping until crossing verts land inside it trims only invisible
 * geometry: the wedge's bounding EDGES are the projections of the same 3D
 * lines regardless of where the cut lands along them. The doubling loop also
 * bounds the accepted radius from BELOW (~0.4×LIMIT, the crossing slides
 * ~half the distance per doubling), so the closing cut can never enter the
 * frame: 0.4×2048 = 819 px > the widest supported visible half-extent
 * (32:9 ≈ 683 PSX px). llvmpipe still under-rasters slivers this long (a
 * software-raster artifact - hardware draws them exactly); don't judge the
 * wedge's full extent on a WSLg screenshot. */
#define CLIP_PROJ_LIMIT 2048.0f

/** @brief Near-plane clips a camera-space triangle and pushes the result.
 *
 * Sutherland-Hodgman against the plane m3 >= zc: 0 vertices behind gives 1
 * triangle, 1 gives a quad (2 triangles, fanned from out[0]), 2 give 1
 * triangle, 3 drop the face and count dbg_behind. zc starts at s_nearz; when
 * a crossing vertex still projects beyond CLIP_PROJ_LIMIT from the screen
 * centre, the whole face is re-clipped at a doubled zc, at most 16 times
 * (crossing projections shrink about 1/zc, and a face that ends fully behind
 * the raised plane lay entirely inside the invisible band). A face that never
 * crosses is pushed on round 0. A clipped push counts dbg_clipped.
 * @param W The walk context receiving the triangles.
 * @param a The first vertex.
 * @param b The second vertex.
 * @param c The third vertex.
 * @param m The face's material. */
static void clip_push_tri(tn_wctx* W, ngte_cvert a, ngte_cvert b, ngte_cvert c, const tmat* m) {
    ngte_cvert in[3] = { a, b, c };
    double zc = s_nearz;
    for (int round = 0; ; round++) {
        ngte_cvert out[4]; int nout = 0; int clipped = 0;
        for (int i = 0; i < 3; i++) {
            ngte_cvert cur = in[i], nxt = in[(i + 1) % 3];
            int curIn = cur.m3 >= zc, nxtIn = nxt.m3 >= zc;
            if (curIn) out[nout++] = cur;
            if (curIn != nxtIn && nout < 4) { out[nout++] = ngte_clip_lerp(cur, nxt, zc); clipped = 1; }
        }
        if (nout < 3) { W->dbg_behind++; return; } /* fully behind the near plane */
        if (clipped && round < 16) {
            int sane = 1;
            for (int i = 0; i < nout && sane; i++) {
                if (out[i].m3 != zc) continue;    /* only crossing verts move with zc */
                float sx, sy;
                proj_screen(out[i].m1, out[i].m2, out[i].m3, &sx, &sy);
                if (fabsf(sx - (float)s_ofx) > CLIP_PROJ_LIMIT ||
                    fabsf(sy - (float)s_ofy) > CLIP_PROJ_LIMIT) sane = 0;
            }
            if (!sane) { zc *= 2.0; continue; }
        }
        if (clipped) W->dbg_clipped++;
        push_tri(W, out[0], out[1], out[2], m);         /* fan the clipped polygon */
        if (nout == 4) push_tri(W, out[0], out[2], out[3], m);
        return;
    }
}

/** @brief Emits one triangle of a sector's projected vertices through the near-plane clip.
 *
 * @param W The walk context receiving the triangles.
 * @param sv The sector's projected vertices.
 * @param a The first vertex index.
 * @param b The second vertex index.
 * @param c The third vertex index.
 * @param ka The first corner's colour and UV.
 * @param kb The second corner's colour and UV.
 * @param kc The third corner's colour and UV.
 * @param m The face's material. */
static void emit_tri(tn_wctx* W, const lvert* sv, int a, int b, int c,
                     lcorner ka, lcorner kb, lcorner kc, const tmat* m) {
    ngte_cvert A = { sv[a].m1, sv[a].m2, sv[a].m3, ka.rgb, ka.u, ka.v };
    ngte_cvert B = { sv[b].m1, sv[b].m2, sv[b].m3, kb.rgb, kb.u, kb.v };
    ngte_cvert C = { sv[c].m1, sv[c].m2, sv[c].m3, kc.rgb, kc.u, kc.v };
    clip_push_tri(W, A, B, C, m);
}

/** @brief Combines a projected vertex with its corner's colour and UVs into a cvert.
 * @param p The projected vertex.
 * @param k The corner's colour and UV.
 * @return The cvert; the screen position, outcode flag and fx/fy are zero. */
static ngte_cvert tn_cv(const lvert* p, lcorner k) {
    ngte_cvert c = { p->m1, p->m2, p->m3, k.rgb, k.u, k.v };
    return c;
}

/** @brief Camera-space midpoint of two cverts.
 *
 * Position and the fx/fy terms are averaged, each colour channel is
 * (a + b) >> 1, and UV, the screen position and the outcode flag are zeroed
 * for the caller to fill. Mirrors the near pass's `(a+b)>>1` grid build.
 * @param a The first vertex.
 * @param b The second vertex.
 * @return The midpoint. */
static ngte_cvert cv_mid(ngte_cvert a, ngte_cvert b) {
    ngte_cvert r;
    r.osx = r.osy = 0.0f; r.oflag = 0;
    r.m1 = (a.m1 + b.m1) * 0.5; r.m2 = (a.m2 + b.m2) * 0.5; r.m3 = (a.m3 + b.m3) * 0.5;
    r.u = 0; r.v = 0;
    r.fx = (a.fx + b.fx) * 0.5f; r.fy = (a.fy + b.fy) * 0.5f;
    int ar=a.rgb&0xFF, ag=(a.rgb>>8)&0xFF, ab=(a.rgb>>16)&0xFF;
    int br=b.rgb&0xFF, bg=(b.rgb>>8)&0xFF, bb=(b.rgb>>16)&0xFF;
    r.rgb = (uint32_t)((ar+br)>>1) | ((uint32_t)((ag+bg)>>1)<<8) | ((uint32_t)((ab+bb)>>1)<<16);
    return r;
}

/* ---- drain sub-cell keys ------------------------------------------------
 * Every near drain inserts each sub-cell it draws into the ordering table at
 * that sub-cell's own key: the sum of its corner nodes' depth terms >> 7 plus
 * the face's authored bias. A quad cell sums its four corners, a
 * sub-triangle SZ A + SZ B + 2 SZ C. A node's term is its SZ3, which the GTE
 * saturates to [0, 0xFFFF] (a node at or behind the eye adds 0), masked to
 * the 12 bits the node word keeps below its outcode flags. The 2x2
 * sub-quad, the 4x4 cell and the sub-triangle inserts: Spyro 1
 * r_environment.s 0x80027DC0-0x80027EF8, 0x8002924C-0x80029460 and
 * 0x800284B0-0x80028524; Spyro 2 environment.s 0x8002647C-0x80026548,
 * 0x800277B4-0x800277D0 with 0x80027A2C-0x80027A38, and 0x80026BA8-0x80026C1C;
 * Spyro 3 environment.s 0x80024C40-0x80024D0C, 0x80025F78-0x80025F88 with
 * 0x800261F0-0x800261FC, and 0x8002536C-0x800253E0.
 * Only a face in the near band (the canonical classifier, emit_nearband) is
 * drained. A face the always-HQ pin subdivides outside the band is inserted
 * by the game whole, so no node word bounds its terms: its sub-cells sum the
 * unmasked hqscale * m3 of their nodes (`drained` 0).
 * The keys are in emit_otkey units, the whole-face key's scale (SZ3 =
 * hqscale * m3). The retained cache's vertex stage mirrors these three
 * functions (terrain.vert drain_sz, drain_key4, drain_key3). */
static double tface_hqscale(void);
/** @brief Returns one corner's depth term of a drain key.
 *
 * The term is hqscale * m3. Under drained it is floored, taken as 0 at or
 * below 0, saturated at 65535 as the GTE saturates SZ3, and masked to the
 * 12 bits the node word keeps; otherwise it is returned unrounded and
 * unmasked.
 * @param m3 The corner's view depth.
 * @param drained Nonzero when the face is in the near band.
 * @return The term. */
static double tn_drain_sz(double m3, int drained) {
    if (!drained) return m3 * tface_hqscale();
    double sz = floor(m3 * tface_hqscale());
    if (sz <= 0.0) return 0.0;
    if (sz > 65535.0) sz = 65535.0;
    return (double)((int)sz & 0xFFF);
}
/** @brief Returns a quad cell's drain key, the sum of its four corner SZ terms in emit_otkey units.
 * @param a The first corner.
 * @param b The second corner.
 * @param c The third corner.
 * @param d The fourth corner.
 * @param drained Nonzero when the face is in the near band, so each term is
 *        saturated and masked as the drain does (tn_drain_sz).
 * @return The key. */
static float tn_drain_key4(const ngte_cvert* a, const ngte_cvert* b,
                           const ngte_cvert* c, const ngte_cvert* d, int drained) {
    return (float)(tn_drain_sz(a->m3, drained) + tn_drain_sz(b->m3, drained) +
                   tn_drain_sz(c->m3, drained) + tn_drain_sz(d->m3, drained));
}
/** @brief Returns a sub-triangle's drain key SZ A + SZ B + 2 SZ C in emit_otkey units.
 * @param a The first corner.
 * @param b The second corner.
 * @param c The corner counted twice.
 * @param drained Nonzero when the face is in the near band, so each term is
 *        saturated and masked as the drain does (tn_drain_sz).
 * @return The key. */
static float tn_drain_key3(const ngte_cvert* a, const ngte_cvert* b, const ngte_cvert* c,
                           int drained) {
    return (float)(tn_drain_sz(a->m3, drained) + tn_drain_sz(b->m3, drained) +
                   2.0 * tn_drain_sz(c->m3, drained));
}

/** @brief Emits a textured camera-space quad through the near-plane clip.
 *
 * UVs come from a decoded Tiledef in [TL, TR, BL, BR] order; the GPU
 * diagonal is TR-BL, triangulated (TL, TR, BL) + (TR, BL, BR) as in the main
 * loop. The quad is one drain sub-cell and sets emit_otkey to its own key
 * (tn_drain_key4), except under emit_keycells, where the caller has set the
 * face's least sub-cell key and its table.
 * @param W The walk context receiving the triangles.
 * @param tl The top-left corner.
 * @param tr The top-right corner.
 * @param bl The bottom-left corner.
 * @param br The bottom-right corner.
 * @param m The quad's material.
 * @param u The four corner U texels, [TL, TR, BL, BR].
 * @param v The four corner V texels, [TL, TR, BL, BR]. */
static void emit_quad_cv(tn_wctx* W, ngte_cvert tl, ngte_cvert tr, ngte_cvert bl, ngte_cvert br,
                         const tmat* m, const float u[4], const float v[4]) {
    tl.u=u[0]; tl.v=v[0]; tr.u=u[1]; tr.v=v[1]; bl.u=u[2]; bl.v=v[2]; br.u=u[3]; br.v=v[3];
    if (!W->emit_keycells) W->emit_otkey = tn_drain_key4(&tl, &tr, &bl, &br, W->emit_nearband);
    clip_push_tri(W, tl, tr, bl, m);
    clip_push_tri(W, tr, bl, br, m);
}

/* Near/HQ subdivide pass - the func_800276A0 simple-emit path (.L80027D7C),
 * verified against r_environment.s. A near foreground cell is split 2×2 into a
 * 3×3 grid (corners + edge midpoints + centre, midpoints in camera space via
 * `(a+b)>>1`) and emitted as 4 sub-quads textured from the per-cell HQ Tiledef
 * record (`m_HQTexturePointer + (faceWord2 & 0x7F)*168`): sub-quad i (i=0..3 =
 * TL,TR,BL,BR quadrant) uses the 8-byte Tiledef at `hqBase + i*8` (the .s does
 * `t6 += 8` per sub-quad), with the 4 spatial corner UVs (u0,v0)/(u1,v1)/
 * (u0,v0+0x1F)/(u0+0x1F,v0+0x1F) = [TL,TR,BL,BR] (the .s t1/t2/t1+0x1F00/
 * t1+0x1F1F). These are the bpp8 hi-detail tiles. `abe` = faceWord2 & 0x80
 * (the .s `s0 & 0x80` semi-transparent gate). Colour is bilinear (no DPCS fog
 * on near cells); the corner cverts carry the original quad's raw NEAR
 * colours. The +0x1E00 queue's finer 4x4 cells and split triangles are the
 * tier-4 paths (terrain_near_emit_face, terrain_near_emit_tri). */
/** @brief Applies a Tiledef's rotation or flip to its decoded corner UVs.
 *
 * Mirrors func_800276A0 block_80027EA8. The Tiledef carries an orientation
 * byte offset in `word1 >> 25`, masked by the layout's uv_ori_mask; a nonzero
 * offset selects an 8-byte entry of the offset table (uv_ori_table: Spyro 1
 * D_8006D058, 128 bytes; Spyro 2 D_80062040 and Spyro 3 D_80065D08, 64 bytes
 * of byte-identical 32-texel entries). The entry holds per-corner packed
 * (du | dv << 8) deltas, added to word0's (u0, v0) for TL, BL and BR and to
 * word1's (u1, v1) for TR as 16-bit sums, so a carry from u into v is kept as
 * the game keeps it. Spyro 1's table has a 32-texel half and a 16-texel half
 * (read by the 4x4 cells); offsets 0 and 64 are identity entries. A layout
 * whose Tiledef word1 is the BR corner (tile_word1_br) subtracts 0x1F00 from
 * it first, as the game does.
 * @param cpu The guest CPU state.
 * @param e The Tiledef's guest address.
 * @param u The four corner U texels, [TL, TR, BL, BR]; rewritten when the
 *          offset is nonzero.
 * @param v The four corner V texels, [TL, TR, BL, BR]; rewritten likewise.
 * @return The orientation offset; 0 (identity, or no table located) leaves
 *         u and v unchanged. */
static int tiledef_apply_ori(CPUState* cpu, uint32_t e, float u[4], float v[4]) {
    if (!game_layout.terrain.uv_ori_table) return 0;   /* no table located: identity */
    uint32_t w0 = rd(cpu, e), w1 = rd(cpu, e + 4);
    /* Where word1 is the BR corner the emit decrements it by 0x1F00 before it
     * derives the index and before the TR add (Spyro 3 environment.s
     * 80024D10-80024D18), so the borrow out of the low half reaches the index
     * exactly as the game's does. */
    if (game_layout.terrain.tile_word1_br) w1 -= 0x1F00u;
    /* The index is a byte offset into the table, masked to bound it: the games
     * that carry ABE in tpage bit 15 would otherwise index past their 64-byte
     * table (Spyro 3 `srl 25; andi 0x38`, environment.s 80024D18-80024D1C). */
    uint32_t ori = (w1 >> 25) & game_layout.terrain.uv_ori_mask;
    if (!ori) return 0;
    uint32_t te = game_layout.terrain.uv_ori_table + ori;     /* per-orientation UV-offset table (Spyro 1 D_8006D058) */
    uint32_t ew0 = rd(cpu, te), ew1 = rd(cpu, te + 4);
    uint32_t tl = ((w0 & 0xFFFF) + (ew0 & 0xFFFF)) & 0xFFFF;  /* TL rel (u0,v0) */
    uint32_t tr = ((w1 & 0xFFFF) + (ew0 >> 16))    & 0xFFFF;  /* TR rel (u1,v1) */
    uint32_t bl = ((w0 & 0xFFFF) + (ew1 & 0xFFFF)) & 0xFFFF;  /* BL rel (u0,v0) */
    uint32_t br = ((w0 & 0xFFFF) + (ew1 >> 16))    & 0xFFFF;  /* BR rel (u0,v0) */
    u[0]=(float)(tl & 0xFF); v[0]=(float)((tl >> 8) & 0xFF);
    u[1]=(float)(tr & 0xFF); v[1]=(float)((tr >> 8) & 0xFF);
    u[2]=(float)(bl & 0xFF); v[2]=(float)((bl >> 8) & 0xFF);
    u[3]=(float)(br & 0xFF); v[3]=(float)((br >> 8) & 0xFF);
    return (int)ori;
}

/** @brief Returns the camera-space triple product of three terrain corners.
 *
 * Its sign is the sign of the projected NCLIP over the same triple (the
 * projection scales each axis by a positive factor), and unlike the
 * projection it stays defined for a corner behind the eye.
 * @param a The first corner.
 * @param b The second corner.
 * @param c The third corner.
 * @return det(a, b, c) over (m1, m2, m3). */
static double tface_det3(const lvert* a, const lvert* b, const lvert* c) {
    return a->m1 * (b->m2 * c->m3 - b->m3 * c->m2)
         - a->m2 * (b->m1 * c->m3 - b->m3 * c->m1)
         + a->m3 * (b->m1 * c->m2 - b->m2 * c->m1);
}

/** @brief Tests whether a straddling HP face faces the eye, by camera-space triple products.
 *
 * The .s walk does not NCLIP a straddler: a corner whose GTE FLAG bit 31 is
 * set carries vertex flag 0x10 (0x80026A8C-0x80026AA4), and the face's OR of
 * it skips both face NCLIPs (bgtz at 0x80026C5C). The near-queue drain then
 * NCLIPs every sub-quad over the same two corner triples with the same
 * flip/double-sided rule (0x80027DEC-0x80027E54, 0x800292EC-0x80029354), so
 * a back-facing straddler draws nothing. Here the first triple is (i0, i1,
 * i3) for a quad, (i0, i1, i2) for a triangle; a quad also tests (i1, i2,
 * i3), and either triple facing passes. A zero product counts as facing.
 * @param sv The sector's projected vertices.
 * @param i0 The first corner's vertex index.
 * @param i1 The second corner's vertex index.
 * @param i2 The third corner's vertex index.
 * @param i3 The fourth corner's vertex index; read only for a quad.
 * @param quad 1 when the face is a quad.
 * @param flip 1 when the face's NCLIP sign is flipped.
 * @param dside 1 when the face is double-sided; it always faces.
 * @return 1 when the face faces the eye, else 0. */
static int tface_front3d(const lvert* sv, int i0, int i1, int i2, int i3,
                         int quad, int flip, int dside) {
    if (dside) return 1;
    const double d1 = tface_det3(&sv[i0], &sv[i1], &sv[quad ? i3 : i2]);
    if (flip ? (d1 <= 0.0) : (d1 >= 0.0)) return 1;
    if (!quad) return 0;
    const double d2 = tface_det3(&sv[i1], &sv[i2], &sv[i3]);
    return flip ? (d2 <= 0.0) : (d2 >= 0.0);
}

static int face_is_near_canonical(double z0, double z1, double z2, double z3, uint32_t word3);

/** @brief Computes a face's two facing verdicts: the near/HQ subdivide gate and the back-face cull.
 *
 * *near_front gates the near/HQ subdivide (half 1 of the projected NCLIP,
 * like the .s classifier's input); *cull_front gates the back-face cull
 * (either half). A straddling face (an HP corner carrying the .s vertex flag
 * 0x10, tface_gte_flagged; an LP corner at or behind the eye) has no usable
 * projected NCLIP. HP takes tface_front3d for the near gate. Its cull
 * follows the .s path after the skipped face NCLIPs: a face the classifier
 * queues for the near drain (face_is_near_canonical, the tests at
 * 0x80026CFC-0x80026D34, tri 0x80027254-0x80027284) takes tface_front3d, the
 * drain's sub-quad NCLIP; any other face with a corner in front of the eye
 * is emitted whole with no NCLIP at .L80026DB8 (tri .L80027310), so its cull
 * verdict is 1. A face wholly at or behind the eye sums to depth 0, which the
 * near branch drops (beqz at 0x80026D70), and keeps tface_front3d. LP (whose
 * straddlers the .s path has not been traced for) waives the facing test
 * and sets both to 1.
 * @param sv The sector's projected vertices.
 * @param i0 The first corner's vertex index.
 * @param i1 The second corner's vertex index.
 * @param i2 The third corner's vertex index.
 * @param i3 The fourth corner's vertex index; read only for a quad.
 * @param quad 1 when the face is a quad.
 * @param flip 1 when the face's NCLIP sign is flipped.
 * @param dside 1 when the face is double-sided.
 * @param hp 1 for a high-poly face, 0 for a low-poly face.
 * @param straddle 1 when the face is a straddler (tface_straddle).
 * @param w3 The HP face record's word 3, read by the drain classifier.
 * @param front The projected NCLIP verdict of the first half.
 * @param front2 The projected NCLIP verdict of the second half.
 * @param near_front Receives the near/HQ subdivide verdict.
 * @param cull_front Receives the back-face cull verdict.
 * @return For an HP straddler with every corner in front of the eye, bit 0
 *         when the projected NCLIP passes it and the cull verdict drops it,
 *         bit 1 when the projected NCLIP drops it and the cull verdict
 *         passes it; 0 otherwise. */
static int tface_facing(const lvert* sv, int i0, int i1, int i2, int i3,
                        int quad, int flip, int dside, int hp, int straddle, uint32_t w3,
                        int front, int front2, int* near_front, int* cull_front) {
    if (!straddle) { *near_front = front; *cull_front = front || front2; return 0; }
    if (!hp) { *near_front = *cull_front = 1; return 0; }
    const int f3 = tface_front3d(sv, i0, i1, i2, i3, quad, flip, dside);
    const int i3q = quad ? i3 : i2;
    const int drained = face_is_near_canonical(sv[i0].m3, sv[i1].m3, sv[i2].m3, sv[i3q].m3, w3);
    const int behind = sv[i0].m3 <= 0.0 || sv[i1].m3 <= 0.0 || sv[i2].m3 <= 0.0 || sv[i3q].m3 <= 0.0;
    const int seen = sv[i0].m3 > 0.0 || sv[i1].m3 > 0.0 || sv[i2].m3 > 0.0 || sv[i3q].m3 > 0.0;
    *near_front = f3;
    *cull_front = f3 || (!drained && seen);
    const int projected = front || front2;
    if (behind || projected == *cull_front) return 0;
    return projected ? 1 : 2;
}

/** @brief Decodes quadrant i's tile of a near face's HQ record with orientation and UV scroll applied.
 *
 * The tile is the 8-byte Tiledef at hq_sub0 + i * 8 of the record. Its
 * material takes the record's texture id. A tile on the live scroll list adds
 * the fractional V offset of its group (quadrants 0-1 the first, 2-3 the
 * second) after the orientation.
 * @param cpu The guest CPU state.
 * @param hqBase The HQ record's guest address.
 * @param texid The record's texture id.
 * @param abe The face's semi-transparency gate.
 * @param i The quadrant, 0..3 = TL, TR, BL, BR.
 * @param out Receives the material.
 * @param u Receives the four corner U texels, [TL, TR, BL, BR].
 * @param v Receives the four corner V texels, [TL, TR, BL, BR]. */
static void tn_near_quad_tile(CPUState* cpu, uint32_t hqBase, int texid, int abe, int i,
                              tmat* out, float u[4], float v[4]) {
    tmat mat; memset(&mat, 0, sizeof(mat)); mat.bpp = 16;
    /* Sub-quad i reads the 8-byte tile half at terrain.hq_sub0 + i*8 of the
     * face's HQ record. Both games' emit loops bump the record pointer by 8
     * in a branch delay slot before the first Tiledef read, so the first
     * quadrant is already one half in: Spyro 1's 168-byte record starts the
     * quadrants at +8 (entry 0 is the detail-path base, unused by the 2x2),
     * Spyro 3's inline block at +0x10. Reading from offset 0 shifts every
     * sub-tile by one quadrant. */
    uint32_t e = hqBase + game_layout.terrain.hq_sub0 + (uint32_t)i * 8u;
    decode_tiledef(cpu, e, abe, &mat, u, v);
    mat.texid = texid;                /* HQ quadrants share the record's id */
    tiledef_apply_ori(cpu, e, u, v);  /* rotated/flipped HQ tiles (word1>>25) */
    /* UV scroll (env texture anim). Applied after the orientation: the
     * offset is a pure V translation in texel space and apply_ori never
     * moves V into U (its per-corner deltas only carry within the u/v byte
     * pair), so scrolling here is identical to scrolling the base entry -
     * a rotated tile scrolls along its rotated axis, exactly as the game's
     * whole-texel writes do. Sub-quads 0-1 read the first HQ group and 2-3
     * the second; the two groups sit half a window apart in the scroll
     * ring, so they carry separately-wrapped offsets. Spyro 3's scroll walker
     * writes the same two groups into its inline sub-tiles (acc>>1 into the
     * first pair, ((acc>>1)+32)&0x3F into the second), so the same pair of
     * offsets applies. */
    if (s_tanim_have && texid >= 0 && texid < 128 && s_tanim_act[texid]) {
        float dv = s_tanim_dvhq[texid][i >> 1];
        for (int k = 0; k < 4; k++) v[k] += dv;
    }
    *out = mat;
}

/** @brief Computes the normal of the plane through the eye and two camera-space points.
 *
 * The normal is the cross product a x b of the camera-space positions,
 * written out term by term as the retained cache's vertex stage computes it
 * (terrain.vert quad_side_fl).
 * @param a The first point.
 * @param b The second point.
 * @param n Receives the normal. */
static void tn_eye_normal(const ngte_cvert* a, const ngte_cvert* b, double n[3]) {
    n[0] = a->m2 * b->m3 - b->m2 * a->m3;
    n[1] = a->m3 * b->m1 - b->m3 * a->m1;
    n[2] = a->m1 * b->m2 - b->m1 * a->m2;
}
/** @brief Returns the signed side of an eye plane on which a camera-space point lies.
 *
 * The side is the dot product of n and the point (m1, m2, m3).
 * @param n The plane normal, from tn_eye_normal.
 * @param v The point, from its camera-space m1, m2, m3.
 * @return The dot product; the plane passes through the eye, so its sign
 *         is the side and its magnitude scales with the point's distance. */
static double tn_eye_side(const double n[3], const ngte_cvert* v) {
    return n[0] * v->m1 + n[1] * v->m2 + n[2] * v->m3;
}
/** @brief Sets a 2x2 quadrant's drain side coordinates fx and fy on its four corners.
 *
 * For the corners Q = TL, TR, BL, BR (psx_key_contest.glsl), fx is the dot
 * product of the corner's camera-space position with the normal of the eye
 * plane through the quadrant's column isoline (the segment joining its top
 * and bottom edge midpoints), scaled to 1 at TR, and fy the same for its row
 * isoline, scaled to 1 at BL. A zero scale is taken as 1.
 * @param Q The corners, [TL, TR, BL, BR]; fx and fy are written. */
static void tn_quad_sides(ngte_cvert Q[4]) {
    const ngte_cvert ct = cv_mid(Q[0], Q[1]), cb = cv_mid(Q[2], Q[3]);
    const ngte_cvert rl = cv_mid(Q[0], Q[2]), rr = cv_mid(Q[1], Q[3]);
    double nc[3], nr[3];
    tn_eye_normal(&ct, &cb, nc);
    tn_eye_normal(&rl, &rr, nr);
    double sc = tn_eye_side(nc, &Q[1]), sr = tn_eye_side(nr, &Q[2]);
    if (sc == 0.0) sc = 1.0;
    if (sr == 0.0) sr = 1.0;
    for (int k = 0; k < 4; k++) {
        Q[k].fx = (float)(tn_eye_side(nc, &Q[k]) / sc);
        Q[k].fy = (float)(tn_eye_side(nr, &Q[k]) / sr);
    }
}

/** @brief Emits a 2x2 near face as its four quadrant quads.
 *
 * Splits the quad at its edge and centre midpoints (cv_mid). Under the
 * integer vertex snap each corner whose m3 reaches s_nearz takes the floor of
 * its projection, each edge midpoint the point of the snapped edge nearest
 * its own projection (only when both edge corners are safe), and the centre
 * the floor of its projection when all four corners are safe. Each quadrant
 * i (TL, TR, BL, BR) takes its tile and UVs from tn_near_quad_tile, its drain
 * side coordinates from tn_quad_sides and its index in bits 30..31 of
 * emit_kc[0], and is pushed with emit_quad_cv.
 * @param W The walk context.
 * @param cpu The guest CPU state.
 * @param hqBase The face's HQ record address.
 * @param texid The face's Tiledef id.
 * @param abe 1 when the face is semi-transparent.
 * @param TL The top-left corner.
 * @param TR The top-right corner.
 * @param BL The bottom-left corner.
 * @param BR The bottom-right corner.
 */
static void terrain_near_emit(tn_wctx* W, CPUState* cpu, uint32_t hqBase, int texid, int abe,
                              ngte_cvert TL, ngte_cvert TR, ngte_cvert BL, ngte_cvert BR) {
    ngte_cvert mT = cv_mid(TL, TR), mB = cv_mid(BL, BR);
    ngte_cvert mL = cv_mid(TL, BL), mR = cv_mid(TR, BR);
    ngte_cvert C  = cv_mid(mT, mB);                 /* centre = mean of 4 corners */
    /* Integer vertex snap: pin each boundary midpoint to the SNAPPED parent
     * edge so the sub-quads stay watertight against the neighbour sharing
     * that edge (see ngte_cvert.oflag) - but at the PERSPECTIVE-CORRECT position
     * along the edge (the true camera-space midpoint's projection, dropped
     * onto the snapped segment), NOT the screen midpoint: the quadrant HQ
     * tiles are attached to these vertices, and a screen-parametric midpoint
     * re-warps near textures exactly the way the subdivide exists to
     * prevent.
     *
     * The near-plane guard is per-EDGE, not per-family: the neighbouring
     * cell sharing an edge - often another subdivided family whose OWN far
     * corners dip inside the near plane - must reach the identical verdict
     * for the SHARED midpoint, and an edge's safety is a property of its
     * two shared endpoints alone. (A family-wide bail parted two adjacent
     * families' copies of the same midpoint - one on-segment, one floored
     * fresh in push_tri - and drew a hairline across the near ground at the
     * subdivide-ring distance.) An unsafe edge gets no override: a clipping
     * child rebuilds its crossings (ngte_clip_lerp zeroes oflag) and the segment
     * projection would lie. */
    if (openpete_native_vertex_snap_active()) {
        ngte_cvert* P[4] = { &TL, &TR, &BL, &BR };
        ngte_cvert* M[4] = { &mT, &mB, &mL, &mR };
        const int E[4][2] = { {0,1}, {2,3}, {0,2}, {1,3} };  /* corner pairs */
        int safe[4];
        float px[4], py[4];
        for (int i = 0; i < 4; i++) {
            safe[i] = P[i]->m3 >= s_nearz;
            if (!safe[i]) continue;            /* mirrored projection: unusable */
            proj_screen(P[i]->m1, P[i]->m2, P[i]->m3, &px[i], &py[i]);
            px[i] = floorf(px[i]); py[i] = floorf(py[i]);
            P[i]->osx = px[i]; P[i]->osy = py[i]; P[i]->oflag = 1;
        }
        for (int i = 0; i < 4; i++) {
            if (!safe[E[i][0]] || !safe[E[i][1]]) continue;
            float mx, my;
            proj_screen(M[i]->m1, M[i]->m2, M[i]->m3, &mx, &my);
            float ax = px[E[i][0]], ay = py[E[i][0]];
            float dx = px[E[i][1]] - ax, dy = py[E[i][1]] - ay;
            float L2 = dx*dx + dy*dy;
            float tt = L2 > 0.0f ? ((mx-ax)*dx + (my-ay)*dy) / L2 : 0.5f;
            if (tt < 0.0f) tt = 0.0f; else if (tt > 1.0f) tt = 1.0f;
            M[i]->osx = ax + tt*dx; M[i]->osy = ay + tt*dy; M[i]->oflag = 1;
        }
        /* Centre: interior (shared by all 4 children only) - snapped true
         * projection keeps it wobble-consistent with the rest of the mesh. */
        if (safe[0] && safe[1] && safe[2] && safe[3]) {
            float cx2, cy2; proj_screen(C.m1, C.m2, C.m3, &cx2, &cy2);
            C.osx = floorf(cx2); C.osy = floorf(cy2); C.oflag = 1;
        }
    }
    ngte_cvert q[4][4] = {                          /* {TL,TR,BL,BR} per sub-quad */
        { TL, mT, mL, C  },                    /* SQ0: top-left quadrant   -> entry 0 */
        { mT, TR, C,  mR },                    /* SQ1: top-right quadrant  -> entry 1 */
        { mL, C,  BL, mB },                    /* SQ2: bottom-left quadrant-> entry 2 */
        { C,  mR, mB, BR },                    /* SQ3: bottom-right        -> entry 3 */
    };
    for (int i = 0; i < 4; i++) {
        tmat mat; float u[4], v[4];
        tn_near_quad_tile(cpu, hqBase, texid, abe, i, &mat, u, v);
        tn_quad_sides(q[i]);
        W->emit_kc[0] = (W->emit_kc[0] & 0x3FFFFFFFu) | ((uint32_t)i << 30);
        emit_quad_cv(W, q[i][0], q[i][1], q[i][2], q[i][3], &mat, u, v);
    }
}

/* ---- deferred near-cell queue (Route 2: the .s's own near classification) --
 *
 * func_800258F0's HP face walk classifies each HP face (the classifier at
 * r_environment.s 0x80026CFC): a NEAR foreground face is deferred into one of two
 * per-frame queues in g_SonyImage scratch, which func_800276A0 then drains +
 * subdivides:
 *   +0x2000 (tier 2): depthSum < 0x2000 && !(word3 & 0x80) && either no corner
 *                     SZ3 < 0x140 && !(word3 & 0x40) (.L80026D3C, sb 2), or a corner
 *                     SZ3 < 0x140 && depthSum != 0 && (word3 & 0x40) (.L80026D70
 *                     branches back to .L80026D3C: 0x40 caps a tier-4 face at 2x2)
 *   +0x1E00 (tier 4): depthSum < 0x2000 && !(word3 & 0x80) && a corner SZ3
 *                     < 0x140 && depthSum != 0 && !(word3 & 0x40)  (.L80026D70, sb 4)
 * (depthSum = Σ of the 4 corner SZ3; faces with m_LodDistance - depthSum <= 0 are
 * dropped entirely - too far for HP.) Both queues are 0-terminated by
 * func_800276A0's entry; each entry is: 0 = end, POSITIVE = a header
 * (sectorPtr>>2, sets up the drain's vertex/colour bases), NEGATIVE = a cell
 * (faceRecPtr | clipBit, low 2 bits are flags). func_800276A0 drains +0x2000 then
 * +0x1E00 (.L8002857C re-points $ra at +0x1E00), so BOTH are rendered.
 *
 * The walk routes faces with face_is_near_canonical and face_near_tier4, which
 * evaluate this classifier per face from the corner depths and word3. Reading
 * the queues at walk time is not a routing source: by then they hold only part
 * of the tick's deferred faces. The reader below serves the verify/debug oracle, which compares its entries
 * (faceRecPtr = faces + f*16, the same 16-byte HP face record the walk reads)
 * with the walk's verdict. Bit 1 of a cell entry marks a triangle face
 * (r_environment.s 0x800272F8). */
#define MAX_DEFER 4096
static uint32_t s_defer[MAX_DEFER]; static int s_defer_n;
static int s_defer_hdr2, s_defer_cell2, s_defer_hdr1e, s_defer_cell1e;
static uint32_t s_defer4[MAX_DEFER]; static int s_defer4_n;   /* the +0x1E00 cells alone */
/** @brief Orders two uint32_t values ascending, for qsort over the defer lists.
 *
 * @param a A pointer to the first uint32_t.
 * @param b A pointer to the second uint32_t.
 * @return Negative, zero or positive as *a is below, equal to or above *b.
 */
static int defer_cmp(const void* a, const void* b) {
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return (x > y) - (x < y);
}
/** @brief Reads one 0-terminated defer queue from guest RAM.
 *
 * A positive entry is a header and counts into *hdr; a negative entry is a
 * cell whose face record pointer (low two flag bits cleared) appends to
 * s_defer, and also to s_defer4 for the +0x1E00 queue, and counts into
 * *cell. Each list holds at most MAX_DEFER entries; the walk stops at 0,
 * after MAX_DEFER * 2 entries, or at once for a queue address of 0.
 * @param cpu The guest CPU state.
 * @param p The queue's address; 0 when the game has none.
 * @param is1e 1 for the +0x1E00 (tier 4) queue.
 * @param hdr Incremented per header.
 * @param cell Incremented per cell. */
static void defer_read_queue(CPUState* cpu, uint32_t p, int is1e, int* hdr, int* cell) {
    for (int guard = 0; p && guard < MAX_DEFER * 2; guard++) {
        uint32_t e = rd(cpu, p); p += 4;
        if (e == 0) break;
        if ((int32_t)e > 0) { (*hdr)++; continue; }
        if (s_defer_n < MAX_DEFER) s_defer[s_defer_n++] = e & ~3u;  /* cell: faceRecPtr */
        if (is1e && s_defer4_n < MAX_DEFER) s_defer4[s_defer4_n++] = e & ~3u;
        (*cell)++;
    }
}
/** @brief Reads both of the game's deferred near-cell queues into the sorted lists s_defer and s_defer4.
 *
 * Clears the lists and the header and cell counts, reads the +0x2000 queue
 * (game_layout.terrain.defer_q[0]) and the +0x1E00 queue (defer_q[1]) with
 * defer_read_queue, then sorts each list by face record pointer so
 * is_deferred and is_deferred4 can binary-search it. A queue address of 0
 * leaves that queue empty.
 * @param cpu The guest CPU state.
 */
static void terrain_read_defer(CPUState* cpu) {
    s_defer_n = s_defer4_n = 0;
    s_defer_hdr2 = s_defer_cell2 = s_defer_hdr1e = s_defer_cell1e = 0;
    /* a queue address 0: no queue located for this game */
    defer_read_queue(cpu, game_layout.terrain.defer_q[0], 0, &s_defer_hdr2, &s_defer_cell2);
    defer_read_queue(cpu, game_layout.terrain.defer_q[1], 1, &s_defer_hdr1e, &s_defer_cell1e);
    if (s_defer_n > 1) qsort(s_defer, (size_t)s_defer_n, sizeof(s_defer[0]), defer_cmp);
    if (s_defer4_n > 1) qsort(s_defer4, (size_t)s_defer4_n, sizeof(s_defer4[0]), defer_cmp);
}
/** @brief Binary-searches a sorted defer list for a face record pointer.
 *
 * @param q The list, ascending.
 * @param n The list's length.
 * @param faceRecPtr The HP face record's guest address.
 * @return 1 when the list holds the pointer, else 0.
 */
static int defer_find(const uint32_t* q, int n, uint32_t faceRecPtr) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        if (q[m] < faceRecPtr) lo = m + 1;
        else if (q[m] > faceRecPtr) hi = m - 1;
        else return 1;
    }
    return 0;
}
/** @brief Tests whether the game queued this face in either near-cell queue (s_defer).
 * @param faceRecPtr The HP face record's guest address.
 * @return 1 when queued, else 0.
 */
static int is_deferred(uint32_t faceRecPtr) { return defer_find(s_defer, s_defer_n, faceRecPtr); }
/** @brief Tests whether the game queued this face in the +0x1E00 tier-4 queue (s_defer4).
 * @param faceRecPtr The HP face record's guest address.
 * @return 1 when queued, else 0.
 */
static int is_deferred4(uint32_t faceRecPtr) { return defer_find(s_defer4, s_defer4_n, faceRecPtr); }

static double s_hqscale = -1.0;   /* SZ3 = s_hqscale * m3 (env OPENPETE_TERRAIN_HQSCALE) */
/* The two face word-3 bits the defer classifier reads, by the role the game's
 * descriptor gives them rather than by a bit position: the hard exclusion from
 * the near/HQ queues, and the refusal of the no-near-corner half of the
 * foreground band. A game whose classifier reads no face word leaves both
 * fields 0, and every consumer below then behaves as the depth-only form. */
/** @brief Tests a face's word 3 for the hard exclusion from the near/HQ queues.
 *
 * Reads the bit the layout names terrain.near_flag_excl; a layout that sets
 * it to 0 excludes no face.
 * @param w3 The face's word 3.
 * @return Nonzero when the face is excluded. */
static inline int tface_near_excl(uint32_t w3) {
    uint32_t b = game_layout.terrain.near_flag_excl;
    return b && (w3 & b);
}
/** @brief Tests a face's word 3 for the bit that refuses the no-near-corner half of the near band.
 *
 * Reads the bit the layout names terrain.near_flag_tier2; a layout that sets
 * it to 0 refuses no face.
 * @param w3 The face's word 3.
 * @return Nonzero when the face is refused. */
static inline int tface_near_tier2(uint32_t w3) {
    uint32_t b = game_layout.terrain.near_flag_tier2;
    return b && (w3 & b);
}
/** @brief The game's own near-cell classification: whether its walk defers this face to the near/HQ queues.
 *
 * Replicates func_800258F0's HP-walk defer test (the classifier at
 * r_environment.s 0x80026CFC) from game state, without the --no-lod
 * always-HQ pin: a deferred face never receives a main-OT index, so
 * consumers that model the game's OT (the authored bias gate) use this, and
 * face_is_near layers the pin on top. The game's queue itself is read only
 * as an oracle, when the verify, calibration or OPENPETE_TERRAIN_NEARQ paths
 * or the portal guard ask for it (terrain_read_defer). A face is deferred when
 *   depthSum < near_band && !(word3 & near_flag_excl)
 *     && (minCorner < near_corner || !(word3 & near_flag_tier2)),
 * the four terms from the game's terrain descriptor; a game whose classifier
 * reads no face word-3 bits leaves both flag terms 0 and defers on depth
 * alone. Depths are GTE SZ3 (world/4) and m3 is world/16, so
 * SZ3 = HQSCALE * m3, HQSCALE 4 or OPENPETE_TERRAIN_HQSCALE (latched). Each
 * corner saturates at 0 as the GTE's unsigned SZ3 does, so a corner behind
 * the camera adds 0 and the face still classifies near. The game's leading
 * "m_LodDistance - depthSum <= 0 skips" gate never fires for a near face
 * (depthSum < 0x2000, far below m_LodDistance), so it is omitted and the
 * test does not depend on m_LodDistance. depthSum > 0 stands in for the
 * tier-4 depthSum != 0 guard and drops faces fully behind the camera.
 * @param z0 The first corner's m3.
 * @param z1 The second corner's m3.
 * @param z2 The third corner's m3.
 * @param z3 The fourth corner's m3 (a triangle repeats its third).
 * @param word3 The HP face record's word 3.
 * @return 1 when the game defers the face, else 0. */
static int face_is_near_canonical(double z0, double z1, double z2, double z3, uint32_t word3) {
    if (s_hqscale < 0.0) {
        const char* e = getenv("OPENPETE_TERRAIN_HQSCALE"); s_hqscale = (e && *e) ? atof(e) : 4.0;
    }
    double s = s_hqscale;
    /* Per-corner SATURATION at 0 mirrors the GTE: SZ3 is an unsigned clamped
     * register, so a corner behind the camera contributes 0 to the .s's
     * depthSum and the face still classifies NEAR (it is the closest thing on
     * screen). The native float m3 is negative there; summing it raw would
     * drive the sum to <= 0 and classify the face "behind camera". */
    double sz0 = z0 > 0.0 ? z0*s : 0.0, sz1 = z1 > 0.0 ? z1*s : 0.0;
    double sz2 = z2 > 0.0 ? z2*s : 0.0, sz3 = z3 > 0.0 ? z3*s : 0.0;
    double sum = sz0 + sz1 + sz2 + sz3;
    if (sum <= 0.0) return 0;                          /* FULLY behind camera */
    if (tface_near_excl(word3)) return 0;              /* face hard-excluded from HQ subdivide */
    if (sum >= (double)game_layout.terrain.near_band) return 0;   /* not foreground */
    double mn = sz0; if (sz1<mn) mn=sz1; if (sz2<mn) mn=sz2; if (sz3<mn) mn=sz3;
    if (mn < (double)game_layout.terrain.near_corner) return 1;   /* a corner below the tier-4 bound */
    return !tface_near_tier2(word3);                   /* else tier 2 unless the flag refuses it */
}
/** @brief Reports whether the always-HQ pin is in force: exactly under --lod extended, on every game.
 *
 * The extended mode's contract is the highest terrain tier at
 * every distance, so every frustum-admitted HQ-eligible face takes its
 * finest near subdivide however far it is: the 4x4 cells (a quad) or the 16
 * sub-triangles (a triangle) where the game has them, word 3
 * near_flag_tier2 is clear and the record's cells carry texels its 2x2 does
 * not (tface_pin_cells; under perspective-correct texturing,
 * tface_cells_pin), the 2x2 (a quad) otherwise. One predicate behind
 * every consumer, the walk's face_is_near and face_near_tier4 and the
 * retained cache's OPTC_GF_HQPIN bake and vertex-stage select, which must
 * agree or a cached sector draws a different tier from a walked one. The
 * value is openpete_no_lod, set by --lod extended on the native renderer and
 * by the live LOD toggle.
 * @return 1 when the pin is in force, else 0. */
static int hq_pin_active(void) {
    return openpete_no_lod != 0;
}
/** @brief Tests whether a face takes the near/HQ subdivide, honouring the always-HQ pin.
 *
 * Under hq_pin_active the distance gates drop: a face qualifies when its
 * saturated depth sum (each corner max(0, m3 * HQSCALE)) is positive and its
 * word 3 does not carry near_flag_excl. Otherwise the verdict is
 * face_is_near_canonical.
 * @param z0 The first corner's m3.
 * @param z1 The second corner's m3.
 * @param z2 The third corner's m3.
 * @param z3 The fourth corner's m3 (a triangle repeats its third).
 * @param word3 The HP face record's word 3.
 * @return 1 when the face takes the near subdivide, else 0.
 */
static int face_is_near(double z0, double z1, double z2, double z3, uint32_t word3) {
    /* --no-lod: pin the bpp8 HQ near-subdivide ON for every eligible face,
     * regardless of distance, so the coarse bpp4 LQ far tier is never shown. The
     * canonical game switches LQ->HQ only inside the 0x2000 foreground band;
     * under --no-lod we drop all the distance-derived gates and keep only the
     * two that aren't distance: the behind-camera guard and the descriptor's
     * near_flag_excl hard-exclusion (those faces never carry a valid HQ
     * Tiledef). A triangle reaches this through face_near_tier4 (corners
     * A, B, C, C). Display-only; reads no gameplay state.
     *
     * Where the draw bins the HP list by distance (terrain.hp_far != 0),
     * --no-lod has already promoted the far sectors into HP-only, so dropping
     * the distance gates routes every front-or-straddling HP face through the
     * near subdivide at every distance; face_near_tier4 picks the subdivide
     * (4x4 cells or 2x2). */
    if (hq_pin_active()) {
        if (s_hqscale < 0.0) {
            const char* e = getenv("OPENPETE_TERRAIN_HQSCALE"); s_hqscale = (e && *e) ? atof(e) : 4.0;
        }
        double s = s_hqscale;
        double sz0 = z0 > 0.0 ? z0*s : 0.0, sz1 = z1 > 0.0 ? z1*s : 0.0;
        double sz2 = z2 > 0.0 ? z2*s : 0.0, sz3 = z3 > 0.0 ? z3*s : 0.0;
        if (sz0 + sz1 + sz2 + sz3 <= 0.0) return 0;    /* FULLY behind camera */
        return !tface_near_excl(word3);
    }
    return face_is_near_canonical(z0, z1, z2, z3, word3);
}
static int s_near_q_agree, s_near_c_only;   /* classifier vs queue cross-check (verify) */
static int s_caldbg = -1, s_cal_qn;         /* calibration probe (OPENPETE_TERRAIN_CALDBG) */
static int s_nearq = -2;                     /* OPENPETE_TERRAIN_NEARQ: use queue oracle (debug) */
static double s_cal_qsum_max, s_cal_qmin_max, s_cal_nqsum_min;

/* ---- 4x4 near cells (the +0x1E00 queue drain) -----------------------------
 * func_800276A0 drains its +0x1E00 queue (a corner SZ3 under near_corner,
 * word 3 near_flag_tier2 and near_flag_excl clear) finer than the +0x2000
 * queue's 2x2: the face becomes a 5x5 vertex grid and 16 cells
 * (r_environment.s 0x80028590-0x80029498), each inserted into g_WorldOT at
 * its own (sum of its four corner SZ3 >> 7) + the authored bias
 * (0x80029450-0x80029498) and textured from its own 16x16 tile. Faces with
 * word 3 flag 0x40 or no corner under the bound keep the 2x2 path. */

/* The face a near emit draws: its HQ record, identity and facing inputs. */
typedef struct {
    uint32_t hqBase;      /* hq_tex + hq_idx * hq_stride */
    uint32_t rec;         /* face record address (the queue's cell word) */
    uint32_t w3;          /* face word 3 */
    int texid, abe;       /* HQ record id; semi-transparent (word 2 & 0x80) */
    int straddle;         /* a corner at or behind the eye */
    int flip, dside;      /* NCLIP sign flip; double-sided (w3 & 0x4) */
    int rot;              /* triangle UV rotation (word 2 >> 8 & 3) */
    int bank;             /* borrowed face: source table, no texture animation */
} tn_nearface;

/** @brief Returns the SZ3 scale of the near/HQ keys (SZ3 = hqscale * m3).
 *
 * OPENPETE_TERRAIN_HQSCALE when set and nonempty, else 4.0; latched in
 * s_hqscale on first use.
 * @return The scale. */
static double tface_hqscale(void) {
    if (s_hqscale < 0.0) {
        const char* e = getenv("OPENPETE_TERRAIN_HQSCALE"); s_hqscale = (e && *e) ? atof(e) : 4.0;
    }
    return s_hqscale;
}

/* The always-HQ pin sends a face to the 4x4 cells at every distance only
 * where the cells show texels the 2x2 does not: its HQ record's cell tiles
 * are not a re-cut of the record's quadrant tiles (s_hq_differ). Where they
 * are, the cells would add geometry and colour-interpolation density only,
 * so the face keeps the pinned 2x2 and the unpinned tier-4 verdict near the
 * camera. */

/* One tile's decoded rectangle: its four corner texels [TL,TR,BL,BR] with
 * the orientation applied, and the page/CLUT/depth it samples. */
typedef struct { int u[4], v[4], tpx, tpy, bpp, clut; } thq_rect;
/** @brief Decodes one Tiledef into its oriented corner texels, page, CLUT and depth.
 *
 * Decodes the tile with decode_tiledef_span into a zeroed material, applies
 * tiledef_apply_ori, and truncates the corner texels to int.
 * @param cpu The guest CPU state.
 * @param e The Tiledef's guest address.
 * @param span The tile's texel extent minus one: 0x1F or 0x0F.
 * @param r Receives u and v in [TL, TR, BL, BR] order, the page (tpx, tpy),
 *        the depth (bpp) and the CLUT word. */
static void thq_rect_of(CPUState* cpu, uint32_t e, int span, thq_rect* r) {
    tmat m; float u[4], v[4];
    memset(&m, 0, sizeof(m)); m.bpp = 16;
    decode_tiledef_span(cpu, e, 0, &m, u, v, span);
    tiledef_apply_ori(cpu, e, u, v);
    for (int k = 0; k < 4; k++) { r->u[k] = (int)u[k]; r->v[k] = (int)v[k]; }
    r->tpx = m.tpx; r->tpy = m.tpy; r->bpp = m.bpp; r->clut = m.clut_raw;
}
/** @brief Returns the signed byte delta b - a, with UV bytes wrapping at 256.
 * @param a The first UV byte.
 * @param b The second UV byte.
 * @return b - a reduced to the range -128..127. */
static int thq_d8(int a, int b) { return (int8_t)(uint8_t)(b - a); }

/** @brief Tests whether 4x4 cell i addresses exactly the matching quarter of its parent quadrant tile.
 *
 * Cell i sits at row r = i >> 2, column c = i & 3 (D_8006CF58 order); its
 * parent quadrant is q = (r >> 1) * 2 + (c >> 1). The quadrant's 32-texel
 * edges TL->TR and TL->BL (either axis, either sign
 * after the orientation) put the quarter's corners at 0, 15, 16 and 31
 * texels along each edge; the cell's four corners must be those, byte for
 * byte, on the same page, CLUT and depth. A record on the live scroller
 * list (s_tscroll_tile) has its V bytes rewritten within a 64-texel window
 * whose content repeats (tsc_scroll_desc), so its V compares modulo 64
 * (vmask 0x3F; 0xFF otherwise).
 * @param Q The parent quadrant tile.
 * @param C The cell tile.
 * @param i The cell index, 0..15.
 * @param vmask The mask applied to the V difference.
 * @return 1 when every corner matches, else 0. */
static int thq_cell_is_quarter(const thq_rect* Q, const thq_rect* C, int i, int vmask) {
    if (Q->tpx != C->tpx || Q->tpy != C->tpy || Q->bpp != C->bpp || Q->clut != C->clut)
        return 0;
    static const int kOff[4] = { 0, 15, 16, 31 };
    const int r = i >> 2, c = i & 3;
    const int du_s = thq_d8(Q->u[0], Q->u[1]), dv_s = thq_d8(Q->v[0], Q->v[1]);   /* TL->TR */
    const int du_t = thq_d8(Q->u[0], Q->u[2]), dv_t = thq_d8(Q->v[0], Q->v[2]);   /* TL->BL */
    for (int k = 0; k < 4; k++) {             /* cell corner TL,TR,BL,BR */
        const int ks = kOff[(c & 1) * 2 + (k & 1)], kt = kOff[(r & 1) * 2 + (k >> 1)];
        const int eu = Q->u[0] + (du_s * ks + du_t * kt) / 31;
        const int ev = Q->v[0] + (dv_s * ks + dv_t * kt) / 31;
        if (((eu - C->u[k]) & 0xFF) != 0 || ((ev - C->v[k]) & vmask) != 0) return 0;
    }
    return 1;
}

#define THQ_ORI_32 0x40u
/** @brief Tests whether a 4x4 cell tile is a 32x32 tile.
 *
 * The tile's orientation index (tiledef_apply_ori) selects the 32-texel half
 * of D_8006D058 (+0x40), so the tile spans 32 texels on a quarter of its
 * quadrant: twice the 2x2's texel density. The index is word 1 >> 25 masked
 * by uv_ori_mask, after word 1 has 0x1F00 subtracted on a layout whose word 1
 * is the bottom-right corner. A layout without uv_ori_table returns 0.
 * @param cpu The guest CPU state.
 * @param e The cell Tiledef's guest address.
 * @return 1 when the index is at least THQ_ORI_32, else 0. */
static int thq_cell_hires(CPUState* cpu, uint32_t e) {
    if (!game_layout.terrain.uv_ori_table) return 0;
    uint32_t w1 = rd(cpu, e + 4);
    if (game_layout.terrain.tile_word1_br) w1 -= 0x1F00u;
    return ((w1 >> 25) & game_layout.terrain.uv_ori_mask) >= THQ_ORI_32;
}

/* s_hq_differ[i]: HQ record i has a cell tile that is not the matching
 * quarter of its quadrant tile (thq_cell_is_quarter), so its 4x4 cells show
 * texels its 2x2 does not. s_hq_hires[i]: record i has a 32x32 cell tile
 * (thq_cell_hires). Recomputed from the record table when the table moves
 * or any tile generation steps (s_tgen_epoch: a level load re-bases it, env
 * animation rewrites tiles); s_hq_differ_gen steps when a verdict moves,
 * which re-bakes the sectors whose routing it decides (tgpu_pin_key). */
static uint32_t s_tgen_epoch;         /* tentative: defined with tsc_tile_gens */
static uint8_t  s_hq_differ[128], s_hq_hires[128];
/* The same verdicts over the HP source level's table (tsimp_t), and the
 * table set the calling thread's faces index: 0 the scene's, 1 the source's
 * (a walk pass or bake loop over borrowed faces sets it, tface_verdict_bank). */
static uint8_t  s_hq_differ_src[128], s_hq_hires_src[128];
static _Thread_local int s_hq_bank;
static uint32_t s_hq_differ_gen, s_hq_differ_table, s_hq_differ_epoch;
/** @brief Computes one HQ record's s_hq_differ and s_hq_hires verdicts.
 *
 * Compares each of the record's 16 cell tiles (hq_cells4 + i*8, span 0x0F)
 * with its parent quadrant tile (hq_sub0 + q*8, span 0x1F) through
 * thq_cell_is_quarter, and tests each cell tile with thq_cell_hires.
 * @param cpu The guest CPU state.
 * @param base The HQ record's guest address.
 * @param vmask The V comparison mask passed to thq_cell_is_quarter.
 * @return Bit 0 set when any cell is not its quadrant's quarter; bit 1 set
 *         when any cell tile is 32x32. */
static unsigned thq_record_verdicts(CPUState* cpu, uint32_t base, int vmask) {
    unsigned d = 0, h = 0;
    for (int i = 0; i < 16; i++) {
        const int q = ((i >> 3) << 1) | ((i & 3) >> 1);
        const uint32_t ce = base + game_layout.terrain.hq_cells4 + (uint32_t)i * 8u;
        thq_rect Q, C;
        thq_rect_of(cpu, base + game_layout.terrain.hq_sub0 + (uint32_t)q * 8u, 0x1F, &Q);
        thq_rect_of(cpu, ce, 0x0F, &C);
        d |= !thq_cell_is_quarter(&Q, &C, i, vmask);
        h |= (unsigned)thq_cell_hires(cpu, ce);
    }
    return d | (h << 1);
}
/** @brief Prints the HQ record indices each pin verdict holds, under OPENPETE_TERRAIN_PIN_DBG=1.
 *
 * One [pincells] line on stderr lists the records with s_hq_differ set and
 * the records with s_hq_hires set. The variable is latched on the first
 * call.
 * @param hqTable The HQ record table's guest address.
 * @param texCnt The record count the verdicts were computed over. */
static void thq_verdicts_dump(uint32_t hqTable, uint32_t texCnt) {
    static int s_dbg = -1;
    if (s_dbg < 0) { const char* e = getenv("OPENPETE_TERRAIN_PIN_DBG"); s_dbg = e && *e == '1'; }
    if (!s_dbg) return;
    fprintf(stderr, "[pincells] table=%08x records=%u differ:", hqTable, texCnt);
    for (int rec = 0; rec < 128; rec++) if (s_hq_differ[rec]) fprintf(stderr, " %d", rec);
    fprintf(stderr, " hires:");
    for (int rec = 0; rec < 128; rec++) if (s_hq_hires[rec]) fprintf(stderr, " %d", rec);
    fprintf(stderr, "\n");
}
/** @brief Recomputes the always-HQ pin verdicts s_hq_differ and s_hq_hires when the HQ table or a tile generation moved.
 *
 * Returns at once on a layout without the 4x4 cells, and when hqTable and
 * s_tgen_epoch equal the values of the last refresh. Otherwise every one of
 * the 128 rows is recomputed through thq_record_verdicts; a row at or above
 * texCnt, or every row when hqTable is 0, is cleared. A record on the live
 * scroller list (s_tscroll_tile) compares its V bytes modulo 64. Steps
 * s_hq_differ_gen and prints the verdicts when any row changed.
 * @param cpu The guest CPU state.
 * @param hqTable The HQ record table's guest address, or 0.
 * @param texCnt The record count; values above 128 are clamped. */
static void tface_differ_refresh(CPUState* cpu, uint32_t hqTable, uint32_t texCnt) {
    if (!game_layout.terrain.hq_cells4) return;
    if (hqTable == s_hq_differ_table && s_tgen_epoch == s_hq_differ_epoch) return;
    if (texCnt > 128) texCnt = 128;
    int moved = 0;
    for (uint32_t rec = 0; rec < 128; rec++) {
        const unsigned v = (hqTable && rec < texCnt)
            ? thq_record_verdicts(cpu, hqTable + rec * game_layout.terrain.hq_stride,
                                  s_tscroll_tile[rec] ? 0x3F : 0xFF)
            : 0u;
        moved |= (v & 1u) != s_hq_differ[rec] || (v >> 1) != s_hq_hires[rec];
        s_hq_differ[rec] = (uint8_t)(v & 1u);
        s_hq_hires[rec] = (uint8_t)(v >> 1);
    }
    s_hq_differ_table = hqTable;
    s_hq_differ_epoch = s_tgen_epoch;
    if (moved) s_hq_differ_gen++;
    if (moved) thq_verdicts_dump(hqTable, texCnt);
}

/** @brief Returns the always-HQ pin's cell verdict for a face with HQ record hq_idx.
 *
 * Nonzero when the pin is in force on a game with the 4x4 cells and the
 * record's cell tiles differ from its quadrant tiles (s_hq_differ). The
 * affine branch of tface_cells_pin reads it; the walk and the cache bake
 * read that predicate.
 * @param hq_idx The face's HQ record index; the low 7 bits are used.
 * @return Nonzero when the pin routes the face to the cells. */
static int tface_pin_cells(int hq_idx) {
    if (!hq_pin_active() || !game_layout.terrain.hq_cells4) return 0;
    return (s_hq_bank ? s_hq_differ_src : s_hq_differ)[hq_idx & 0x7F];
}

/* The texel layouts of the near tiers. The 2x2 maps quadrant
 * tile q's 32 texels (corner texels 0 and 31) across the quadrant; the 4x4
 * maps each of the quadrant's four cell tiles across a quarter of it. A
 * 16x16 cell that is its quadrant's quarter samples two 15-texel spans
 * (0-15, 16-31) where the 2x2 samples one 31-texel span, a sub-texel shift
 * at the switch between the tiers, and adds no texels; the PSX draws those
 * cells to bound affine texture distortion near the camera, a job
 * perspective-correct texturing does not have. A triangle's tier-4 pass
 * samples the same 16x16 cell tiles against the tier-2 pass's quadrant
 * tiles. Under perspective-correct texturing a face therefore takes the
 * cells (a quad) or the 16 sub-triangles (a triangle) only where its record
 * carries 32x32 cell tiles (s_hq_hires), texels the 2x2 or the tier-2 split
 * does not show, and draws the 2x2 or the tier-2 split at every other near
 * distance: no face switches texel layout on approach. Under affine
 * texturing every face keeps the drain's routing. */
/** @brief Tests whether a face with HQ record hq_idx may take the 4x4 cells.
 *
 * 0 on a game without the 4x4 cells. Under affine texturing every face may;
 * under perspective-correct texturing only a face whose record carries
 * 32x32 cell tiles (s_hq_hires).
 * @param hq_idx The face's HQ record index; the low 7 bits are used.
 * @return Nonzero when the face may take the cells. */
static int tface_cells_ok(int hq_idx) {
    if (!game_layout.terrain.hq_cells4) return 0;
    return openpete_native_get_texture_mapping() || (s_hq_bank ? s_hq_hires_src : s_hq_hires)[hq_idx & 0x7F];
}
/** @brief Tests whether a face with HQ record hq_idx takes the cells at every near distance.
 *
 * 0 unless the always-HQ pin is in force on a game with the 4x4 cells.
 * Under affine texturing it is the pin's verdict (tface_pin_cells); under
 * perspective-correct texturing, a record with 32x32 cells (s_hq_hires).
 * The walk (terrain_near_emit_face, face_tri4_routed) and the cache bake
 * (face_flags bit 19) read this one predicate.
 * @param hq_idx The face's HQ record index; the low 7 bits are used.
 * @return Nonzero when the face is routed to the cells. */
static int tface_cells_pin(int hq_idx) {
    if (!hq_pin_active() || !game_layout.terrain.hq_cells4) return 0;
    return openpete_native_get_texture_mapping() ? tface_pin_cells(hq_idx)
                                                 : (s_hq_bank ? s_hq_hires_src : s_hq_hires)[hq_idx & 0x7F];
}

static int face_drain_tier4(double z0, double z1, double z2, double z3, uint32_t word3);
/** @brief Decides whether a near face takes the tier-4 cells (the drain's +0x1E00 route).
 *
 * The route of the classifier's .L80026D70 branch (r_environment.s
 * 0x80026D10-0x80026DB4) for a face that may take the cells (ok,
 * tface_cells_ok). Under pin (tface_cells_pin) the distance terms drop and
 * the flag term stays: every near face with near_flag_tier2 clear takes the
 * cells (a quad) or the 16 sub-triangles (a triangle, through
 * face_tri4_routed). Without pin the verdict is face_drain_tier4. A game
 * without hq_cells4 or a face with ok clear returns 0.
 * @param z0 The first corner's m3.
 * @param z1 The second corner's m3.
 * @param z2 The third corner's m3.
 * @param z3 The fourth corner's m3 (a triangle repeats its third).
 * @param word3 The HP face record's word 3.
 * @param ok 1 when the face may take the cells.
 * @param pin 1 when the face is routed to the cells at every distance.
 * @return 1 when the face takes tier 4, else 0.
 */
static int face_near_tier4(double z0, double z1, double z2, double z3, uint32_t word3,
                           int ok, int pin) {
    if (!game_layout.terrain.hq_cells4 || !ok) return 0;
    if (pin)
        return face_is_near(z0, z1, z2, z3, word3) && !tface_near_tier2(word3);
    return face_drain_tier4(z0, z1, z2, z3, word3);
}
/** @brief The drain's own tier-4 route, whatever the port draws.
 *
 * True for a canonical near face (face_is_near_canonical) with
 * near_flag_tier2 clear and a corner SZ3 (hqscale * m3) under near_corner
 * (Spyro 2 environment.s 0x80024B90-0x80024BAC, the four corner tests before
 * the second queue's append at 0x80024BE8).
 * @param z0 The first corner's m3.
 * @param z1 The second corner's m3.
 * @param z2 The third corner's m3.
 * @param z3 The fourth corner's m3 (a triangle repeats its third).
 * @param word3 The HP face record's word 3.
 * @return 1 when the drain routes the face to tier 4, else 0. */
static int face_drain_tier4(double z0, double z1, double z2, double z3, uint32_t word3) {
    if (!face_is_near_canonical(z0, z1, z2, z3, word3)) return 0;
    if (tface_near_tier2(word3)) return 0;
    double s = tface_hqscale();
    double mn = z0;
    if (z1 < mn) mn = z1;
    if (z2 < mn) mn = z2;
    if (z3 < mn) mn = z3;
    return mn * s < (double)game_layout.terrain.near_corner;
}

/** @brief The node k/4 of the way from a to b by the drain's halving order.
 *
 * The midpoint comes first, and each quarter point is the midpoint of its
 * endpoint and that midpoint (cv_mid; r_environment.s
 * 0x800286BC-0x800287B4). A grid row runs between the nodes the same rule
 * puts on the left and right edges, so node (r, c) =
 * pt(pt(TL, BL, r), pt(TR, BR, r), c).
 * @param a The k = 0 end.
 * @param b The k = 4 end.
 * @param k The quarter index, 0..4.
 * @return The node. */
static ngte_cvert nc4_pt(ngte_cvert a, ngte_cvert b, int k) {
    if (k == 0) return a;
    if (k == 4) return b;
    ngte_cvert m = cv_mid(a, b);
    if (k == 2) return m;
    return cv_mid(k == 1 ? a : b, m);
}
/** @brief Builds a quad's 5x5 node grid by the drain's halving order.
 *
 * Node G[r][c] is nc4_pt(nc4_pt(TL, BL, r), nc4_pt(TR, BR, r), c): row r
 * runs from the top edge (0) to the bottom edge (4), column c from the left
 * edge (0) to the right edge (4).
 * @param G Receives the 25 nodes.
 * @param TL The top-left corner.
 * @param TR The top-right corner.
 * @param BL The bottom-left corner.
 * @param BR The bottom-right corner.
 */
static void nc4_grid(ngte_cvert G[5][5], ngte_cvert TL, ngte_cvert TR,
                     ngte_cvert BL, ngte_cvert BR) {
    for (int r = 0; r < 5; r++) {
        ngte_cvert L = nc4_pt(TL, BL, r), R = nc4_pt(TR, BR, r);
        for (int c = 0; c < 5; c++) G[r][c] = nc4_pt(L, R, c);
    }
}

/** @brief Places an edge node on the segment between its edge's two floored corner projections.
 *
 * The parameter is the projection of (px, py) onto the segment, clamped to
 * [0, 1], so the node is collinear with the neighbour drawing the same edge.
 * A corner inside the near plane leaves the node without an override.
 * @param n The edge node; receives the screen override.
 * @param a The edge's first corner.
 * @param b The edge's second corner.
 * @param px The node's own projected X.
 * @param py The node's own projected Y. */
static void nc4_snap_edge(ngte_cvert* n, const ngte_cvert* a, const ngte_cvert* b,
                          float px, float py) {
    if (a->m3 < s_nearz || b->m3 < s_nearz) return;
    float ax, ay, bx, by;
    proj_screen(a->m1, a->m2, a->m3, &ax, &ay);
    proj_screen(b->m1, b->m2, b->m3, &bx, &by);
    ax = floorf(ax); ay = floorf(ay); bx = floorf(bx); by = floorf(by);
    float dx = bx - ax, dy = by - ay;
    float L2 = dx*dx + dy*dy;
    float tt = L2 > 0.0f ? ((px-ax)*dx + (py-ay)*dy) / L2 : 0.5f;
    if (tt < 0.0f) tt = 0.0f; else if (tt > 1.0f) tt = 1.0f;
    n->osx = ax + tt*dx; n->osy = ay + tt*dy; n->oflag = 1;
}
/** @brief Integer vertex snap of the 5x5 grid, terrain_near_emit's rule per node.
 *
 * A corner or interior node takes the floor of its own projection; a node on
 * a face edge sits on the segment between that edge's two snapped corners
 * (nc4_snap_edge). A node inside the near plane gets no override.
 * @param G The grid; receives the screen overrides. */
static void nc4_snap(ngte_cvert G[5][5]) {
    for (int i = 0; i < 25; i++) {
        int r = i / 5, c = i % 5;
        ngte_cvert* n = &G[r][c];
        if (n->m3 < s_nearz) continue;
        float px, py;
        proj_screen(n->m1, n->m2, n->m3, &px, &py);
        int er = (r == 0 || r == 4), ec = (c == 0 || c == 4);
        if (er == ec) { n->osx = floorf(px); n->osy = floorf(py); n->oflag = 1; }
        else if (er)  nc4_snap_edge(n, &G[r][0], &G[r][4], px, py);
        else          nc4_snap_edge(n, &G[0][c], &G[4][c], px, py);
    }
}

/** @brief A node's emitted screen point: its override, else its projection.
 *
 * The projection is floored under the integer snap, as push_tri does.
 * @param n The node.
 * @param snap 1 when the integer vertex snap is active.
 * @param sx Receives the screen X.
 * @param sy Receives the screen Y. */
static void nc4_scr(const ngte_cvert* n, int snap, float* sx, float* sy) {
    if (n->oflag) { *sx = n->osx; *sy = n->osy; return; }
    proj_screen(n->m1, n->m2, n->m3, sx, sy);
    if (snap) { *sx = floorf(*sx); *sy = floorf(*sy); }
}

/** @brief The drain's off-screen flag on a node's own projection (0x80028CD8-0x80028D10).
 *
 * Set when the node is at or behind the eye, or its floored screen point has
 * sy <= 0, sy >= 256, sx <= 0 or sx >= 512, the GTE's integer SXY.
 * @param n The node.
 * @return 1 when flagged off-screen, else 0. */
static int nc4_outcode(const ngte_cvert* n) {
    if (n->m3 <= 0.0) return 1;
    float px, py;
    proj_screen(n->m1, n->m2, n->m3, &px, &py);
    px = floorf(px); py = floorf(py);
    return py <= 0.0f || py >= 256.0f || px <= 0.0f || px >= 512.0f;
}

/** @brief The drain's interior screen morph of the 5x5 grid (r_environment.s 0x80028D1C-0x80028E44, node table D_8006D0D8).
 *
 * With every face corner's SZ3 above 0x40, each odd-odd node not flagged
 * off-screen moves toward the mean of its up-right and down-left neighbours,
 * weight at = (least corner SZ3) - 0x40, capped at 256:
 * s' = ((256 - at) * s + (at / 2) * (s_ur + s_dl)) / 256. At the tier-4
 * bound (at -> 0xFF) the node lies on the TR-BL diagonal the 2x2 path splits
 * its quadrants along, so the cell grid meets the 2x2 image where a face
 * crosses between the queues. The game's integer at >> 1 is kept as at / 2
 * in full precision; under the integer snap the result is floored. The
 * morph moves a node's screen point while its UV, depth and key stay. Under
 * screen-affine texturing (the PSX GPU's) it matches the cell image to the
 * 2x2 image's affine interpolation across the queue crossing; under
 * perspective-correct texturing the unmorphed node already lies at its
 * texel's projection and the moved node samples that texel at the wrong
 * screen point, so the caller applies it under affine texture mapping only.
 * @param G The grid; receives the moved nodes' screen overrides. */
static void nc4_morph(ngte_cvert G[5][5]) {
    double mn = G[0][0].m3;
    if (G[0][4].m3 < mn) mn = G[0][4].m3;
    if (G[4][0].m3 < mn) mn = G[4][0].m3;
    if (G[4][4].m3 < mn) mn = G[4][4].m3;
    double at = mn * tface_hqscale() - 64.0;
    if (at <= 0.0) return;
    /* Inside the tier-4 band a corner's SZ3 is below 0x140, so at < 256. A face
     * the --lod extended pin sends to the cells from farther away takes the
     * full weight: the grid in the 2x2 image's shape, never past it. */
    if (at > 256.0) at = 256.0;
    int snap = openpete_native_vertex_snap_active();
    static const int P[4][2] = { {1, 1}, {1, 3}, {3, 1}, {3, 3} };
    float ms[4][2];
    for (int i = 0; i < 4; i++) {           /* neighbours are even nodes: never morphed */
        int r = P[i][0], c = P[i][1];
        float sx, sy, ax, ay, bx, by;
        nc4_scr(&G[r][c], snap, &sx, &sy);
        nc4_scr(&G[r - 1][c + 1], snap, &ax, &ay);
        nc4_scr(&G[r + 1][c - 1], snap, &bx, &by);
        ms[i][0] = (float)(((256.0 - at) * sx + at * 0.5 * ((double)ax + bx)) / 256.0);
        ms[i][1] = (float)(((256.0 - at) * sy + at * 0.5 * ((double)ay + by)) / 256.0);
    }
    for (int i = 0; i < 4; i++) {
        ngte_cvert* n = &G[P[i][0]][P[i][1]];
        if (nc4_outcode(n)) continue;
        n->osx = snap ? floorf(ms[i][0]) : ms[i][0];
        n->osy = snap ? floorf(ms[i][1]) : ms[i][1];
        n->oflag = 1;
    }
}

/** @brief Camera-space triple product a · (b × c) over three cverts, tface_det3's form.
 *
 * @param a The first vertex.
 * @param b The second vertex.
 * @param c The third vertex.
 * @return The determinant. */
static double cv_det3(const ngte_cvert* a, const ngte_cvert* b, const ngte_cvert* c) {
    return a->m1 * (b->m2 * c->m3 - b->m3 * c->m2)
         - a->m2 * (b->m1 * c->m3 - b->m3 * c->m1)
         + a->m3 * (b->m1 * c->m2 - b->m2 * c->m1);
}
/** @brief A straddling face's cell facing test (r_environment.s 0x800292EC-0x80029354).
 *
 * The drain tests each cell of a face carrying the straddle bit over
 * (TL, TR, BL) and then (BR, BL, TR), flip-signed, and draws the cell when
 * either faces. Here each test is the camera-space triple product
 * (cv_det3): front is d >= 0, or d <= 0 when flipped.
 * @param tl The top-left node.
 * @param tr The top-right node.
 * @param bl The bottom-left node.
 * @param br The bottom-right node.
 * @param flip 1 when the face's winding is flipped.
 * @return 1 when either triangle faces the camera, else 0. */
static int nc4_front(const ngte_cvert* tl, const ngte_cvert* tr,
                     const ngte_cvert* bl, const ngte_cvert* br, int flip) {
    double d1 = cv_det3(tl, tr, bl);
    if (flip ? (d1 <= 0.0) : (d1 >= 0.0)) return 1;
    double d2 = cv_det3(tr, br, bl);
    return flip ? (d2 <= 0.0) : (d2 >= 0.0);
}

/** @brief Emits the 16 cells of a tier-4 quad's grid in the drain's row-major order (D_8006CF58).
 *
 * Cell i reads the 16x16 tile at hqBase + hq_cells4 + i*8 with its
 * orientation (tiledef_apply_ori) and, when the record's texture scrolls,
 * its row's V scroll. A straddling single-sided face skips a cell that faces
 * away (nc4_front). Each cell's key is emit_quad_cv's per-quad corner sum,
 * unless emit_keycells holds the face's table.
 * @param W The walk context receiving the triangles.
 * @param cpu The guest CPU state.
 * @param nf The near face.
 * @param G The 5x5 node grid. */
static void nc4_emit_cells(tn_wctx* W, CPUState* cpu, const tn_nearface* nf,
                           ngte_cvert G[5][5]) {
    int scroll = s_tanim_have && !nf->bank && nf->texid >= 0 && nf->texid < 128 && s_tanim_act[nf->texid];
    for (int i = 0; i < 16; i++) {
        int r = i >> 2, c = i & 3;
        const ngte_cvert *tl = &G[r][c], *tr = &G[r][c + 1];
        const ngte_cvert *bl = &G[r + 1][c], *br = &G[r + 1][c + 1];
        if (nf->straddle && !nf->dside && !nc4_front(tl, tr, bl, br, nf->flip)) continue;
        tmat mat; float u[4], v[4];
        memset(&mat, 0, sizeof(mat)); mat.bpp = 16;
        uint32_t e = nf->hqBase + game_layout.terrain.hq_cells4 + (uint32_t)i * 8u;
        decode_tiledef_span(cpu, e, nf->abe, &mat, u, v, 0x0F);
        mat.texid = nf->texid;
        tiledef_apply_ori(cpu, e, u, v);   /* word1 >> 25: the 16-texel half of D_8006D058 */
        for (int k = 0; scroll && k < 4; k++) v[k] += s_tanim_dvcell[nf->texid][r];
        emit_quad_cv(W, *tl, *tr, *bl, *br, &mat, u, v);
    }
}

/* ---- drain sub-cell key tables ---------------------------------------------
 * A face the drain inserts as finer sub-cells than the visual mesh draws it
 * (a tier-4 quad drawn as the 2x2, a tier-4 triangle drawn as the tier-2
 * split, a near triangle drawn whole) takes one ordering-table bucket per
 * sub-cell, and a triangle of the visual mesh spans cells of different
 * buckets. Its triangles carry the least sub-cell key as their contest key
 * (emit_otkey) and the face's sub-cell keys as a table over the drain's grid
 * (emit_kc) with a per-vertex coordinate that locates the sub-cell at each
 * pixel (ngte_cvert fx/fy: a triangle's face-local coordinate, a quad
 * quadrant's isoline sides, tn_quad_sides); every fragment keys and
 * contests at the key of the sub-cell the game draws at its pixel
 * (psx_key_contest.glsl op_drain_delta, the same function on the retained
 * cache). */
typedef struct { float key[16]; uint8_t on[16]; uint8_t slot[16]; float kmin; int n; } tn_keycells;

/** @brief Records one drain sub-cell's key, visibility and slot in a key table.
 *
 * Stores the three entries at index i. A drawn sub-cell also lowers kmin to
 * its key when it is the first or the least, and counts in n. The table
 * arrives zeroed, so n == 0 marks kmin as unset.
 * @param K The table.
 * @param i The cell index, 0..15.
 * @param on Nonzero when the drain draws the sub-cell.
 * @param key The sub-cell's key.
 * @param slot The sub-cell's slot id, stored as a byte. */
static void tn_keycells_add(tn_keycells* K, int i, int on, float key, int slot) {
    K->on[i] = (uint8_t)on;
    K->key[i] = key;
    K->slot[i] = (uint8_t)slot;
    if (!on) return;
    if (K->n == 0 || key < K->kmin) K->kmin = key;
    K->n++;
}

/* A drain-table entry the drain draws nothing at. A real entry is a key
 * sum of at most four 12-bit SZ terms above the least one (<= 0x3FFC) and
 * is clamped to 0x3FFE, so the 14-bit field never holds this value for a
 * drawn sub-cell. */
#define TN_DRAIN_SKIP 0x3FFFu

/** @brief Reads OPENPETE_TERRAIN_DRAIN_SKIP for the walk about to run.
 *
 * "0" selects the legacy packing, a skipped sub-cell keyed at the face's
 * least key and drawn; any other value or none selects the sentinel.
 * @return 1 for the legacy packing, else 0. */
static int tn_drain_skip_legacy(void) {
    const char* e = getenv("OPENPETE_TERRAIN_DRAIN_SKIP");
    return e && e[0] == '0';
}

/** @brief Packs a face's sub-cell keys into the 16-halfword key table.
 *
 * Layout per psx_key_contest.glsl: halfword slot[i] of drain sub-cell i holds
 * its key above the least one in 14 bits, clamped to 0x3FFE; a sub-cell the
 * drain skips holds TN_DRAIN_SKIP, which the key and colour passes discard
 * (0, the face's least key, under OPENPETE_TERRAIN_DRAIN_SKIP=0); bits
 * 14..15 of halfword 0 hold the grid (1 quad cells, 2 tier-4 sub-triangles,
 * 3 tier-2).
 * @param K The face's sub-cell keys.
 * @param cnt The sub-cell count.
 * @param grid The grid code.
 * @param out Receives the table, two halfwords per word.
 * @param skip Counts the skipped sub-cells: [0] packed as 0, [1] as the sentinel. */
static void tn_keycells_pack(const tn_keycells* K, int cnt, uint32_t grid, uint32_t out[8],
                             int skip[2]) {
    memset(out, 0, 8 * sizeof(uint32_t));
    for (int i = 0; i < cnt; i++) {
        const float dk = K->key[i] - K->kmin;
        uint32_t d = dk < (float)(TN_DRAIN_SKIP - 1u) ? (uint32_t)dk : TN_DRAIN_SKIP - 1u;
        if (!K->on[i]) {
            d = s_drain_skip_legacy ? 0u : TN_DRAIN_SKIP;
            skip[!s_drain_skip_legacy]++;
        }
        out[K->slot[i] >> 1] |= d << ((K->slot[i] & 1u) * 16u);
    }
    out[0] |= grid << 14;
}

/** @brief Installs a face's key table for the triangles emitted next, keyed at its least key.
 *
 * Sets emit_keycells, emit_otkey (the least key) and emit_kc on the walk
 * context; the caller clears emit_keycells after the face.
 * @param W The walk context.
 * @param K The face's sub-cell keys.
 * @param cnt The sub-cell count.
 * @param grid The grid code tn_keycells_pack stores. */
static void tn_keycells_begin(tn_wctx* W, const tn_keycells* K, int cnt, uint32_t grid) {
    W->emit_keycells = 1;
    W->emit_otkey = K->kmin;
    tn_keycells_pack(K, cnt, grid, W->emit_kc, W->drain_skip);
}

/** @brief Sets a triangle corner's face-local coordinate in the drain grid.
 *
 * Callers place A, B and C at (0,0), (4,0) and (0,4): the drain grid in
 * quarter-edge units.
 * @param v The corner; fx and fy are written.
 * @param x The face-local x.
 * @param y The face-local y. */
static void tn_fl(ngte_cvert* v, float x, float y) { v->fx = x; v->fy = y; }

/** @brief Computes the keys of a tier-4 quad's 16 cells over its 5x5 node grid.
 *
 * Cells are in the drain's row-major order (table entry = cell). A
 * straddling single-sided face's back-facing cell (the drain's per-cell
 * NCLIP, nc4_front) and a cell whose key (tn_drain_key4) is not positive are
 * skipped, as the drain skips them.
 * @param nf The face.
 * @param G The 5x5 node grid.
 * @param K Receives the keys; reset first. */
static void tn_quad_cell_keys(const tn_nearface* nf, ngte_cvert G[5][5], tn_keycells* K) {
    K->n = 0; K->kmin = 0.0f;
    for (int i = 0; i < 16; i++) {
        const int r = i >> 2, c = i & 3;
        const ngte_cvert *tl = &G[r][c], *tr = &G[r][c + 1];
        const ngte_cvert *bl = &G[r + 1][c], *br = &G[r + 1][c + 1];
        const float key = tn_drain_key4(tl, tr, bl, br, 1);
        const int front = !(nf->straddle && !nf->dside && !nc4_front(tl, tr, bl, br, nf->flip));
        tn_keycells_add(K, i, front && key > 0.0f, key, i);
    }
}

/** @brief Reports whether a drawn cell of a 2x2 quadrant reaches across an isoline into a skipped cell's slot.
 *
 * The quadrant's column and row eye planes are tn_quad_sides' planes over
 * its corners G[r0][c0], G[r0][c0 + 2], G[r0 + 2][c0], G[r0 + 2][c0 + 2]. A
 * drawn cell lies inside its own slot's wedge iff all four of its nodes do
 * (the drain draws a cell as two triangles on its nodes, each the convex
 * hull of its corners); a node strictly on the far side of an isoline puts
 * part of the cell in the slot across it, and the quadrant folds when that
 * slot is a skipped cell: the isoline lookup then names the skipped slot on
 * pixels a drawn cell covers.
 * @param G The 5x5 node grid.
 * @param K The face's cell keys (on[] the drawn cells).
 * @param qd The quadrant, 0..3 (row-major).
 * @return 1 when the quadrant folds, else 0. */
static int tn_quad_folds(ngte_cvert G[5][5], const tn_keycells* K, int qd) {
    const int r0 = (qd >> 1) * 2, c0 = (qd & 1) * 2;
    ngte_cvert Q[4] = { G[r0][c0], G[r0][c0 + 2], G[r0 + 2][c0], G[r0 + 2][c0 + 2] };
    const ngte_cvert ct = cv_mid(Q[0], Q[1]), cb = cv_mid(Q[2], Q[3]);
    const ngte_cvert rl = cv_mid(Q[0], Q[2]), rr = cv_mid(Q[1], Q[3]);
    double nc[3], nr[3];
    tn_eye_normal(&ct, &cb, nc);
    tn_eye_normal(&rl, &rr, nr);
    double sc = tn_eye_side(nc, &Q[1]), sr = tn_eye_side(nr, &Q[2]);
    if (sc == 0.0) sc = 1.0;
    if (sr == 0.0) sr = 1.0;
    const double eps = 1e-6;
    for (int k = 0; k < 4; k++) {
        const int lr = k >> 1, lc = k & 1;
        if (!K->on[(r0 + lr) * 4 + c0 + lc]) continue;
        for (int n = 0; n < 4; n++) {
            const ngte_cvert* v = &G[r0 + lr + (n >> 1)][c0 + lc + (n & 1)];
            const double fx = tn_eye_side(nc, v) / sc, fy = tn_eye_side(nr, v) / sr;
            const int xc = lc ? fx < -eps : fx > eps, xr = lr ? fy < -eps : fy > eps;
            if ((xc || xr) && !K->on[(r0 + (lr ^ xr)) * 4 + c0 + (lc ^ xc)]) return 1;
        }
    }
    return 0;
}

/** @brief Returns a keyed quad's folded quadrants as a mask and counts them.
 *
 * @param W The walk context; drain_skip[2] counts folded quadrants.
 * @param G The 5x5 node grid.
 * @param K The face's cell keys.
 * @return Bit q set when quadrant q folds (tn_quad_folds). */
static unsigned tn_quad_fold_mask(tn_wctx* W, ngte_cvert G[5][5], const tn_keycells* K) {
    unsigned m = 0;
    for (int qd = 0; qd < 4; qd++) {
        if (!tn_quad_folds(G, K, qd)) continue;
        m |= 1u << qd;
        W->drain_skip[2]++;
    }
    return m;
}

/** @brief Writes a camera-space point in the fold test's homogeneous screen space.
 *
 * (OFX z + H x, OFY z + H y, z): a linear map of camera space that sends a
 * point in front of the eye to its screen position times its depth and
 * keeps the sign of every triple product (psx_key_contest.glsl, terrain.vert
 * fold_h).
 * @param v The point.
 * @param out Receives the three floats. */
static void tn_fold_corner(const ngte_cvert* v, float out[3]) {
    out[0] = (float)(s_ofx * v->m3 + s_H * v->m1);
    out[1] = (float)(s_ofy * v->m3 + s_H * v->m2);
    out[2] = (float)v->m3;
}

/** @brief Marks a keyed quad's folded quadrants in the installed key table and stores its corners for them.
 *
 * Sets bit 14 of table entry 2 + q for each folded quadrant q
 * (psx_key_contest.glsl op_drain_folded), and emit_fold and emit_kn, which
 * push_tri copies onto the face's triangles.
 * @param W The walk context; emit_kc already packed (tn_keycells_begin).
 * @param mask The folded quadrants (tn_quad_fold_mask).
 * @param TL The top-left corner.
 * @param TR The top-right corner.
 * @param BL The bottom-left corner.
 * @param BR The bottom-right corner. */
static void tn_keycells_fold(tn_wctx* W, unsigned mask, const ngte_cvert* TL,
                             const ngte_cvert* TR, const ngte_cvert* BL, const ngte_cvert* BR) {
    W->emit_fold = (uint8_t)mask;
    if (!mask) return;
    for (int q = 0; q < 4; q++)
        if (mask & (1u << q)) W->emit_kc[(2 + q) >> 1] |= 0x4000u << ((q & 1) * 16);
    tn_fold_corner(TL, W->emit_kn);
    tn_fold_corner(TR, W->emit_kn + 3);
    tn_fold_corner(BL, W->emit_kn + 6);
    tn_fold_corner(BR, W->emit_kn + 9);
}

/** @brief Emits one routed near quad face: the 4x4 cells for a tier-4 face, the 2x2 subdivide otherwise.
 *
 * The tier-4 verdict is face_near_tier4 over the corner depths, with
 * tface_cells_ok and tface_cells_pin; OPENPETE_TERRAIN_NEARQ routes by the
 * game's filled +0x1E00 queue instead when that queue was read. A 2x2 face
 * that is opaque and that the drain would insert as tier-4 cells keys each
 * fragment by the cell containing it. A tier-4 face builds the 5x5 node
 * grid, snaps it under the integer vertex snap, applies the screen morph
 * under affine texturing, and emits its cells; cells4_faces counts tier-4
 * faces and, in its second slot, those the unpinned verdict would not have
 * routed.
 * @param W The walk context.
 * @param cpu The guest CPU state.
 * @param nf The face.
 * @param TL The top-left corner.
 * @param TR The top-right corner.
 * @param BL The bottom-left corner.
 * @param BR The bottom-right corner. */
static void terrain_near_emit_face(tn_wctx* W, CPUState* cpu, const tn_nearface* nf,
                                   ngte_cvert TL, ngte_cvert TR, ngte_cvert BL, ngte_cvert BR) {
    int t4 = (s_nearq > 0 && s_defer_n > 0)
           ? (game_layout.terrain.hq_cells4 != 0 && is_deferred4(nf->rec))
           : face_near_tier4(TL.m3, TR.m3, BR.m3, BL.m3, nf->w3,
                             tface_cells_ok(nf->texid), tface_cells_pin(nf->texid));
    if (!t4) {
        /* The drain inserts a tier-4 face as its 16 cells: the 2x2 keys
         * each fragment by the cell containing it. */
        tn_keycells K; K.n = 0;
        unsigned fold = 0;
        if (!nf->abe && face_drain_tier4(TL.m3, TR.m3, BR.m3, BL.m3, nf->w3)) {
            ngte_cvert G[5][5];
            nc4_grid(G, TL, TR, BL, BR);
            tn_quad_cell_keys(nf, G, &K);
            if (K.n) fold = tn_quad_fold_mask(W, G, &K);
        }
        if (K.n) {
            tn_keycells_begin(W, &K, 16, 1u);
            tn_keycells_fold(W, fold, &TL, &TR, &BL, &BR);
        }
        terrain_near_emit(W, cpu, nf->hqBase, nf->bank ? -1 : nf->texid, nf->abe, TL, TR, BL, BR);
        W->emit_keycells = 0;
        W->emit_fold = 0;
        return;
    }
    W->cells4_faces[0]++;
    W->cells4_faces[1] += !face_near_tier4(TL.m3, TR.m3, BR.m3, BL.m3, nf->w3, 1, 0);
    ngte_cvert G[5][5];
    nc4_grid(G, TL, TR, BL, BR);
    if (openpete_native_vertex_snap_active()) nc4_snap(G);
    int affine_texturing = openpete_native_get_texture_mapping();
    if (affine_texturing) nc4_morph(G);
    nc4_emit_cells(W, cpu, nf, G);
}

/* ---- tier-4 triangles (the +0x1E00 queue's triangle pass) -----------------
 * func_800276A0 drains the +0x1E00 queue's triangle entries (queue bit 1) in
 * a pass of their own (r_environment.s 0x80029588-0x8002A09C): corners A =
 * face word 0 byte 3, B = byte 2, C = byte 0, 15 nodes, node (r, c) at
 * A(4-r-c)/4 + Bc/4 + Cr/4 (row r toward C, column c along A->B), and 16
 * sub-triangles from the table at hq_tri4_nodes, each textured from one
 * 16x16 cell tile of the face's HQ record and inserted into g_WorldOT at
 * ((SZ A + SZ B + 2 SZ C) >> 7) + the authored bias. The pass has no screen
 * morph. Its stitch triangles (0x80029B04-0x80029E14) are omitted: all three
 * of their nodes lie on one face edge, zero area in camera space, filling the
 * slivers the GTE's integer rounding opens against a coarser neighbour. Here
 * an edge node is the camera-space point of the neighbour's edge: under the
 * integer snap it is placed on the neighbour's snapped segment, at sub-pixel
 * precision it projects onto the edge up to float rounding. */

/* Node build order with parents {node, p, q}: node = (p + q) >> 1 per axis
 * (0x800295D8-0x80029818); nodes 0, 4 and 14 are A, B and C. */
static const uint8_t k_nt4_build[12][3] = {
    { 2, 0, 4 },  { 1, 0, 2 },  { 3, 4, 2 },
    { 9, 0, 14 }, { 5, 0, 9 },  { 12, 14, 9 },
    { 11, 4, 14 }, { 8, 4, 11 }, { 13, 14, 11 },
    { 6, 0, 11 }, { 7, 4, 9 },  { 10, 14, 2 },
};
/* The face edge a node lies on: 0 none (corner or interior), 1 AB, 2 CA,
 * 3 BC; k_nt4_edge_ends names each edge's two corner nodes. */
/* Node n's place (r, c) in the grid: row r toward C, column c along A->B. */
static const uint8_t k_nt4_rc[15][2] = {
    { 0, 0 }, { 0, 1 }, { 0, 2 }, { 0, 3 }, { 0, 4 }, { 1, 0 }, { 1, 1 }, { 1, 2 },
    { 1, 3 }, { 2, 0 }, { 2, 1 }, { 2, 2 }, { 3, 0 }, { 3, 1 }, { 4, 0 },
};
static const uint8_t k_nt4_edge[15] = { 0, 1, 1, 1, 0, 2, 0, 0, 3, 2, 0, 3, 2, 3, 0 };
static const uint8_t k_nt4_edge_ends[4][2] = { { 0, 0 }, { 0, 4 }, { 0, 14 }, { 4, 14 } };

/** @brief Averages two node colours as the drain does (0x80029870-0x80029928).
 *
 * The drain masks both words with 0xFFFEFEFF, adds and shifts right once;
 * here the mask is 0xFEFEFF, which also drops the top byte. Red is the floor
 * of the mean; green and blue drop each operand's low bit first.
 * @param a The first colour (0x00BBGGRR).
 * @param b The second colour.
 * @return The averaged colour. */
static uint32_t nt4_rgb_mid(uint32_t a, uint32_t b) {
    return ((a & 0xFEFEFFu) + (b & 0xFEFEFFu)) >> 1;
}

/** @brief Builds a split triangle's 15 nodes from its three corners.
 *
 * Nodes 0, 4 and 14 are A, B and C; the other twelve follow k_nt4_build, each
 * the cv_mid of its two parents with its colour averaged as the drain does
 * (nt4_rgb_mid).
 * @param N Receives the 15 nodes.
 * @param A The first corner.
 * @param B The second corner.
 * @param C The third corner.
 */
static void nt4_grid(ngte_cvert N[15], ngte_cvert A, ngte_cvert B, ngte_cvert C) {
    N[0] = A; N[4] = B; N[14] = C;
    for (int i = 0; i < 12; i++) {
        const uint8_t* b = k_nt4_build[i];
        N[b[0]] = cv_mid(N[b[1]], N[b[2]]);
        N[b[0]].rgb = nt4_rgb_mid(N[b[1]].rgb, N[b[2]].rgb);
    }
}

/** @brief Applies the integer vertex snap to a split triangle's 15 nodes.
 *
 * nc4_snap's rule: a corner or interior node takes the floor of its own
 * projection as its screen override, an edge node the point of the segment
 * between its edge's snapped corners at the parameter of its own projection
 * (nc4_snap_edge). A node inside the near plane gets no override.
 * @param N The node grid, in the 15-node numbering. */
static void nt4_snap(ngte_cvert N[15]) {
    for (int i = 0; i < 15; i++) {
        ngte_cvert* n = &N[i];
        if (n->m3 < s_nearz) continue;
        float px, py;
        proj_screen(n->m1, n->m2, n->m3, &px, &py);
        const uint8_t* ends = k_nt4_edge_ends[k_nt4_edge[i]];
        if (k_nt4_edge[i] == 0) { n->osx = floorf(px); n->osy = floorf(py); n->oflag = 1; }
        else nc4_snap_edge(n, &N[ends[0]], &N[ends[1]], px, py);
    }
}

/** @brief Returns a tier-4 sub-triangle's cell (0..15), the 4x4 grid rotated by the face's UV rotation (0x80029E44-0x80029E58).
 *
 * The signed byte at hq_tri4_cells + selector + rot is a byte offset in
 * 8-byte tiles; the cell is (offset >> 3) & 15. The selector is the
 * sub-triangle table word's bits 2..5.
 * @param cpu The guest CPU state.
 * @param row The sub-triangle's table word.
 * @param rot The face's rotation.
 * @return The cell index. */
static int nt4_cell(CPUState* cpu, uint32_t row, int rot) {
    uint32_t a = game_layout.terrain.hq_tri4_cells + (row & 0x3Cu) + (uint32_t)rot;
    int8_t off = (int8_t)(rd(cpu, a & ~3u) >> ((a & 3u) * 8u));
    return (off >> 3) & 15;
}

/** @brief Computes a sub-triangle's corner UVs from a Tiledef and the triangle UV table.
 *
 * Mirrors r_environment.s 0x80029E88-0x80029ECC. The table entry is byte
 * offset (word1 >> 25) & hq_tri_uv_mask of orientation block (rot + o) & 3
 * (128 bytes each) of hq_tri_uv. Its deltas are added to the tile words as
 * 32-bit sums, so a u or v byte carries into the next: A = word0 + the
 * entry's low half, B = word1 + its high half (signed), C = word0 + the
 * signed halfword at entry + 4. The prim takes its CLUT from A's word and its
 * TPAGE from B's.
 * @param cpu The guest CPU state.
 * @param e The Tiledef's guest address.
 * @param rot The face's rotation.
 * @param o The sub-triangle's orientation offset.
 * @param u Receives the corner U texels, [A, B, C].
 * @param v Receives the corner V texels, [A, B, C].
 * @param matdiff Set to 1 when A's CLUT half or B's TPAGE half differs from
 *                the tile's own, else 0. */
static void tiledef_tri_uv(CPUState* cpu, uint32_t e, int rot, int o,
                           float u[3], float v[3], int* matdiff) {
    uint32_t w0 = rd(cpu, e), w1 = rd(cpu, e + 4);
    uint32_t te = game_layout.terrain.hq_tri_uv + ((uint32_t)((rot + o) & 3) << 7)
                + ((w1 >> 25) & game_layout.terrain.hq_tri_uv_mask);
    uint32_t d0 = rd(cpu, te), d1 = rd(cpu, te + 4);
    const uint32_t f[3] = {
        w0 + (d0 & 0xFFFFu),
        w1 + (uint32_t)((int32_t)d0 >> 16),
        w0 + (uint32_t)(int32_t)(int16_t)(d1 & 0xFFFFu),
    };
    for (int k = 0; k < 3; k++) {
        u[k] = (float)(f[k] & 0xFFu);
        v[k] = (float)((f[k] >> 8) & 0xFFu);
    }
    *matdiff = ((f[0] ^ w0) >> 16) != 0 || ((f[1] ^ w1) >> 16) != 0;
}

/* ---- tier-2 triangles (the +0x2000 queue's triangle pass) -----------------
 * The same drain splits a tier-2 triangle entry (queue bit 1) at
 * r_environment.s 0x80027F28-0x80028578: 6 nodes, the corners and the edge
 * midpoints by (p + q) >> 1 per axis (0x80027FF8-0x80028060), node colours
 * by the masked average (0x800280CC-0x80028100), and 4 sub-triangles from
 * the table at hq_tri2_nodes (0x800283DC), each textured from one 32x32
 * quadrant tile of the face's HQ record (hq_sub0 + the byte at hq_tri2_tiles
 * + selector + rot, 0x80028408-0x8002843C) with the tier-4 pass's UV table
 * (0x80028440-0x80028490), inserted into g_WorldOT at ((SZ A + SZ B +
 * 2 SZ C) >> 7) + the authored bias (0x800284BC-0x800284C4,
 * 0x80028518-0x80028524). Its nodes are the tier-4 grid's nodes 0, 2, 4, 9,
 * 11 and 14 (the same parents, the same averages), so both passes share the
 * 15-node grid, its snap and its emit. Its stitch triangles (table
 * D_8006D138, one per edge whose neighbour byte & 1, 0x80028240-0x800283D8)
 * are zero-area in camera space and are omitted as the tier-4 pass's are. */
static const uint8_t k_nt2_node[6] = { 0, 2, 4, 9, 11, 14 };

/* One sub-triangle of a split triangle face: its nodes in the 15-node
 * numbering, its tile, the tile's UV orientation o, the cell or quadrant it
 * reads (uv[6] of its cache record) and its V scroll. */
typedef struct { int a, b, c, o, cell; uint32_t e; float dv; } nt_sub;

/** @brief A tier-2 sub-triangle's quadrant (0..3) (0x80028408-0x8002843C).
 *
 * The signed byte at hq_tri2_tiles + selector + rot is a byte offset from
 * hq_sub0 in 8-byte tiles; the quadrant is (offset >> 3) & 3. The selector
 * is the sub-triangle table word's bits 2..3.
 * @param cpu The guest CPU state.
 * @param row The sub-triangle's table word.
 * @param rot The face's rotation.
 * @return The quadrant index. */
static int nt2_tile(CPUState* cpu, uint32_t row, int rot) {
    uint32_t a = game_layout.terrain.hq_tri2_tiles + (row & 0x0Cu) + (uint32_t)rot;
    int8_t off = (int8_t)(rd(cpu, a & ~3u) >> ((a & 3u) * 8u));
    return (off >> 3) & 3;
}

/** @brief Reads sub-triangle i's table word and its nodes A, B, C in the 15-node numbering.
 *
 * A tier-4 split has 16 sub-triangles, read from hq_tri4_nodes; a tier-2
 * split has 4, read from hq_tri2_nodes, whose node bytes index the six
 * tier-2 nodes (k_nt2_node). A is the word's byte 3, B byte 2, C byte 1,
 * each reduced modulo the node count.
 * @param cpu The guest CPU state.
 * @param tier2 1 for the tier-2 split, 0 for the tier-4 split.
 * @param i The sub-triangle index.
 * @param a Receives node A.
 * @param b Receives node B.
 * @param c Receives node C.
 * @return The table word. */
static uint32_t nt_sub_nodes(CPUState* cpu, int tier2, int i, int* a, int* b, int* c) {
    if (tier2) {
        uint32_t row = rd(cpu, game_layout.terrain.hq_tri2_nodes + (uint32_t)i * 4u);
        *a = k_nt2_node[(row >> 24) % 6u];
        *b = k_nt2_node[((row >> 16) & 0xFFu) % 6u];
        *c = k_nt2_node[((row >> 8) & 0xFFu) % 6u];
        return row;
    }
    uint32_t row = rd(cpu, game_layout.terrain.hq_tri4_nodes + (uint32_t)i * 4u);
    *a = (int)(row >> 24) % 15; *b = (int)((row >> 16) & 0xFFu) % 15;
    *c = (int)((row >> 8) & 0xFFu) % 15;
    return row;
}

/** @brief Fills the descriptor of sub-triangle i of a split face.
 *
 * Reads the nodes and orientation from the sub-triangle table (nt_sub_nodes).
 * Tier 2 takes its quadrant from nt2_tile and its Tiledef entry from
 * hq_sub0 of the face's HQ record; tier 4 takes its cell from nt4_cell and
 * its entry from hq_cells4. With scroll set, dv is the tile row's V scroll
 * for the face's texture id (s_tanim_dvhq per quadrant pair for tier 2,
 * s_tanim_dvcell per cell row for tier 4), else 0. The texid must be in
 * 0..127 when scroll is set.
 * @param cpu The guest CPU state.
 * @param nf The face.
 * @param tier2 1 for the tier-2 split, 0 for the tier-4 split.
 * @param i The sub-triangle index.
 * @param scroll 1 when the face's texture animation is active.
 * @param s Receives the descriptor: nodes a, b, c, orientation o, cell,
 *        Tiledef address e and V scroll dv.
 */
static void nt_sub_of(CPUState* cpu, const tn_nearface* nf, int tier2, int i, int scroll,
                      nt_sub* s) {
    uint32_t row = nt_sub_nodes(cpu, tier2, i, &s->a, &s->b, &s->c);
    if (tier2) {
        s->cell = nt2_tile(cpu, row, nf->rot);
        s->e = nf->hqBase + game_layout.terrain.hq_sub0 + (uint32_t)s->cell * 8u;
        s->dv = scroll ? s_tanim_dvhq[nf->texid][s->cell >> 1] : 0.0f;
    } else {
        s->cell = nt4_cell(cpu, row, nf->rot);
        s->e = nf->hqBase + game_layout.terrain.hq_cells4 + (uint32_t)s->cell * 8u;
        s->dv = scroll ? s_tanim_dvcell[nf->texid][s->cell >> 2] : 0.0f;
    }
    s->o = (int)(row & 3u);
}

/** @brief Emits a split triangle face's sub-triangles in table order, each with the parent's winding.
 *
 * A straddling face's sub-triangle draws when it faces (both drains NCLIP
 * straddler entries alone, flip-signed, skipped for double-sided faces:
 * 0x80029F24-0x80029F5C, 0x800284D0-0x80028508). Each sub-triangle takes
 * its tile's material and the tile's triangle UVs (tiledef_tri_uv), plus the
 * V scroll of its cell row or quadrant pair when the record's texture
 * animation is active. Outside a keyed-cell pass its key is its own
 * tn_drain_key3 (SZ A + SZ B + 2 SZ C) in emit_otkey units. No off-screen
 * reject and no size split: the drains' outcode rejects drop only
 * sub-triangles outside the 512x256 GTE screen, which a wider native view
 * shows. Counts culled, material-differing and emitted sub-triangles per
 * tier.
 * @param W The walk context.
 * @param cpu The guest CPU state.
 * @param nf The face.
 * @param N The node grid, in the 15-node numbering.
 * @param tier2 1 for the tier-2 split, 0 for the tier-4 split. */
static void nt_emit(tn_wctx* W, CPUState* cpu, const tn_nearface* nf, const ngte_cvert N[15],
                    int tier2) {
    int scroll = s_tanim_have && !nf->bank && nf->texid >= 0 && nf->texid < 128 && s_tanim_act[nf->texid];
    const int n = tier2 ? 4 : 16;
    for (int i = 0; i < n; i++) {
        nt_sub s;
        nt_sub_of(cpu, nf, tier2, i, scroll, &s);
        ngte_cvert A = N[s.a], B = N[s.b], C = N[s.c];
        if (nf->straddle && !nf->dside) {
            double d = cv_det3(&A, &B, &C);
            if (nf->flip ? d > 0.0 : d < 0.0) { W->trisplit_scull[tier2]++; continue; }
        }
        tmat mat; float u4[4], v4[4], u[3], v[3]; int matdiff;
        memset(&mat, 0, sizeof(mat)); mat.bpp = 16;
        decode_tiledef_span(cpu, s.e, nf->abe, &mat, u4, v4, 0x0F);
        mat.texid = nf->texid;
        tiledef_tri_uv(cpu, s.e, nf->rot, s.o, u, v, &matdiff);
        W->trisplit_matdiff[tier2] += matdiff;
        A.u = u[0]; A.v = v[0] + s.dv;
        B.u = u[1]; B.v = v[1] + s.dv;
        C.u = u[2]; C.v = v[2] + s.dv;
        if (!W->emit_keycells) W->emit_otkey = tn_drain_key3(&A, &B, &C, W->emit_nearband);
        clip_push_tri(W, A, B, C, &mat);
        W->trisplit_subs[tier2]++;
    }
}

/** @brief Returns a sub-triangle's key-table entry from its nodes' grid places.
 *
 * Per psx_key_contest.glsl, with the nodes' rows and columns from k_nt4_rc
 * and (j, i) the least row and column in grid steps: an upward sub-triangle
 * (two nodes on its top row) is entry 4j + i, a downward one
 * 4(3 - j) + 3 - i.
 * @param a The first node.
 * @param b The second node.
 * @param c The third node.
 * @param h The grid step, 1 for tier 4 and 2 for tier 2.
 * @return The table entry, 0..15. */
static int tn_sub_slot(int a, int b, int c, int h) {
    const int ra = k_nt4_rc[a][0], rb = k_nt4_rc[b][0], rc = k_nt4_rc[c][0];
    int rmin = ra < rb ? ra : rb; if (rc < rmin) rmin = rc;
    int cmin = k_nt4_rc[a][1];
    if (k_nt4_rc[b][1] < cmin) cmin = k_nt4_rc[b][1];
    if (k_nt4_rc[c][1] < cmin) cmin = k_nt4_rc[c][1];
    const int top = (ra == rmin) + (rb == rmin) + (rc == rmin);
    const int i = cmin / h, j = rmin / h;
    return top == 2 ? j * 4 + i : (3 - j) * 4 + (3 - i);
}

/** @brief Computes the keys of a triangle split's sub-triangles over its 15-node grid.
 *
 * Tier 2 has 4 sub-triangles, tier 4 has 16. A straddling single-sided
 * face's back-facing sub-triangle carries no key, as the drains skip it
 * (nt_emit's facing test), and neither does a sub-triangle whose key is not
 * positive (every node at or behind the eye). Each key goes to the entry
 * tn_sub_slot names.
 * @param cpu The guest CPU state.
 * @param nf The face.
 * @param N The 15-node grid.
 * @param tier2 1 for the tier-2 split, 0 for tier 4.
 * @param K Receives the keys; reset first. */
static void tn_tri_cell_keys(CPUState* cpu, const tn_nearface* nf, const ngte_cvert N[15],
                             int tier2, tn_keycells* K) {
    K->n = 0; K->kmin = 0.0f;
    const int n = tier2 ? 4 : 16;
    for (int i = 0; i < n; i++) {
        int a, b, c;
        nt_sub_nodes(cpu, tier2, i, &a, &b, &c);
        int front = 1;
        if (nf->straddle && !nf->dside) {
            double d = cv_det3(&N[a], &N[b], &N[c]);
            front = nf->flip ? d <= 0.0 : d >= 0.0;
        }
        const float key = tn_drain_key3(&N[a], &N[b], &N[c], 1);
        tn_keycells_add(K, i, front && key > 0.0f, key, tn_sub_slot(a, b, c, tier2 ? 2 : 1));
    }
}

/** @brief Emits a routed triangle face as its tier-4 or tier-2 split.
 *
 * The corners carry the raw near colours of word 1 bytes 3, 2 and 0 (C's
 * from byte 0, 0x80029834 and 0x8002807C; the flat GT3 emit reads byte 1).
 * The 15-node grid is built from A, B and C at face-local (0,0), (4,0) and
 * (0,4) and snapped under the integer vertex snap. A tier-4 triangle drawn
 * as the tier-2 split keys each fragment by the drain's tier-4 sub-triangle
 * containing it; trisplit_faces counts the face.
 * @param W The walk context.
 * @param cpu The guest CPU state.
 * @param nf The face.
 * @param A The first corner.
 * @param B The second corner.
 * @param C The third corner.
 * @param tier2 1 for the tier-2 split, 0 for tier 4. */
static void terrain_near_emit_tri(tn_wctx* W, CPUState* cpu, const tn_nearface* nf,
                                  ngte_cvert A, ngte_cvert B, ngte_cvert C, int tier2) {
    ngte_cvert N[15];
    tn_fl(&A, 0.0f, 0.0f); tn_fl(&B, 4.0f, 0.0f); tn_fl(&C, 0.0f, 4.0f);
    nt4_grid(N, A, B, C);
    if (openpete_native_vertex_snap_active()) nt4_snap(N);
    /* A tier-4 triangle drawn as the tier-2 split: each fragment keys by
     * the drain's tier-4 sub-triangle containing it. */
    tn_keycells K; K.n = 0;
    if (tier2 && !nf->abe && game_layout.terrain.hq_tri4_cells &&
        face_drain_tier4(A.m3, B.m3, C.m3, C.m3, nf->w3))
        tn_tri_cell_keys(cpu, nf, N, 0, &K);
    if (K.n) tn_keycells_begin(W, &K, 16, 2u);
    nt_emit(W, cpu, nf, N, tier2);
    W->emit_keycells = 0;
    W->trisplit_faces[tier2]++;
}

/** @brief Zeroes one set of split-triangle census counters.
 * @param f The face counts, per tier.
 * @param s The sub-triangle counts, per tier.
 * @param c The culled sub-triangle counts, per tier.
 * @param m The material-differ counts, per tier. */
static void trisplit_reset(int f[2], int s[2], int c[2], int m[2]) {
    memset(f, 0, 2 * sizeof(int)); memset(s, 0, 2 * sizeof(int));
    memset(c, 0, 2 * sizeof(int)); memset(m, 0, 2 * sizeof(int));
}
/** @brief Adds one walk context's split-triangle, tier-4 cell and drain-skip counters into the per-walk sums.
 * @param W The walk context. */
static void trisplit_sum(const tn_wctx* W) {
    for (int k = 0; k < 2; k++) {
        s_trisplit_faces[k] += W->trisplit_faces[k];
        s_trisplit_subs[k] += W->trisplit_subs[k];
        s_trisplit_scull[k] += W->trisplit_scull[k];
        s_trisplit_matdiff[k] += W->trisplit_matdiff[k];
        s_cells4_faces[k] += W->cells4_faces[k];
        s_drain_skip[k] += W->drain_skip[k];
    }
    s_drain_skip[2] += W->drain_skip[2];
}

/** @brief Reports whether a triangle face takes the tier-4 triangle pass.
 *
 * The verdict is face_near_tier4 over (A, B, C, C), since the triangle
 * classifier at 0x80027168 is the quad one with C counted twice; under
 * OPENPETE_TERRAIN_NEARQ, with the game's +0x1E00 queue read, it is the
 * face's presence in that queue. A layout without the tier-4 triangle
 * cells returns 0.
 * @param zA Corner A's m3.
 * @param zB Corner B's m3.
 * @param zC Corner C's m3.
 * @param w3 The HP face record's word 3.
 * @param rec The face record's guest address, the queue's cell key.
 * @param hq_idx The face's HQ record index.
 * @return 1 when the face takes the tier-4 pass, else 0. */
static int face_tri4_routed(double zA, double zB, double zC, uint32_t w3, uint32_t rec,
                            int hq_idx) {
    if (!game_layout.terrain.hq_tri4_cells) return 0;
    if (s_nearq > 0 && s_defer_n > 0) return is_deferred4(rec);
    return face_near_tier4(zA, zB, zC, zC, w3, tface_cells_ok(hq_idx), tface_cells_pin(hq_idx));
}

/* ---- sector decode cache (terrain Tier 1, mirrors the moby faceblock cache) --
 * A sector's geometry is STATIC world-space level data: packed vertex words,
 * face words (indices/colour offsets/flags) and each HP face's LQ Tiledef never
 * change while the level is loaded, so re-reading and re-unpacking all of it
 * through cpu->read_word per sector per present repeats unchanged work.
 * Decode once per sector (key: sector vaddr + a 5-word header signature, so a
 * level load re-decodes; TSC_WINDOW-slot linear probe) and iterate the
 * decoded arrays; only projection, NCLIP, near-classification, fog and the
 * near/HQ subdivide stay per-present. Colour arrays additionally resolve to
 * host pointers once per sector pass. Display-only; the output is
 * byte-identical to parsing the words inline. */
typedef struct { int16_t x, y, z; } tsvert_t;      /* unpacked local vertex */
typedef struct {
    uint8_t  i0, i1, i2, i3, quad, flip;
    uint16_t c0, c1, c2, c3;                       /* colour byte-offsets */
    uint8_t  fogbias, trirot, hq_idx, abe;         /* HP: w3&1, w2>>8&3, w2&0x7F, w2&0x80 */
    uint8_t  gapfill;                              /* LP only: frozen fill-tag face (terrain_fill_tags.h) */
    uint8_t  fillcand;                             /* LP only: gapfill set by the inspector preview, not the list */
    uint8_t  uncov;                                /* LP only: a sample point lies off the sector's HP mesh
                                                    * (tsc_tag_uncovered; distance-binning games only) */
    uint8_t  uncovX;                               /* LP only: uncov against the HP mesh plus the sector's
                                                    * borrowed HP faces (tscache_t.imp); == uncov when the
                                                    * sector borrows none. Extended LOD reads this one. */
    uint8_t  floorf;                               /* LP only: up-facing floor by winding (n_z < 0, |n_z| > 0.8) */
    uint8_t  admit;                                /* LP only: authored admission field w0 & 0x1F */
    uint8_t  dside;                                /* double-sided, emits past both NCLIP halves:
                                                    * LP w0 & 0x80 (Spyro 1) / w0 bit 31 (Spyro 3),
                                                    * HP w3 & 0x4 (Spyro 1) / w3 & 0x800 (Spyro 3) */
    uint8_t  otbias;                               /* authored OT-bias field: LP (w1>>3)&0x1F,
                                                    * HP (w3>>3)&0x7 (slot units before x8/x4) */
    int8_t   tb;                                   /* HP texByte (int8 of w2; -1 = untextured) */
    uint32_t w3;                                   /* HP word3 (face_is_near gates) */
    uint32_t tgen;                                 /* s_tgen[tile] at texmat decode (anim liveness) */
    uint32_t tgenHq;                               /* s_tgen[hq_idx] at last refresh - the scroll
                                                    * accumulator gen for the HQ sibling record
                                                    * (bake liveness; the CPU near path reads it live) */
    tmat mat; float tu[4], tv[4];                  /* tiledef half +0 (far/LQ; LP untextured) */
    tmat matN; float tuN[4], tvN[4];               /* tiledef half +8 (near tile, .s t0<=0 branch) */
} tsface_t;

/* Env texture animation liveness: the game's
 * env-anim (Spyro 1 func_8002A6FC, Spyro 3 func_80020E74) animates textures by
 * rewriting the LQ tiledef TABLE ENTRIES in place each keyframe advance - loop
 * 1 copies whole entries from an animation bank (texture-descriptor swap),
 * loop 2 rewrites the V bytes of BOTH halves (+1/+5/+9/+13, UV scroll -
 * waterfalls/water), and both also rewrite the HQ sub-tiles (Spyro 1's sibling
 * 168-byte record, Spyro 3's four tiledefs inline at +0x10).
 * The 16-byte window hashed below is exactly the two LQ halves in both games. The .s HP walk reads
 * the entry live per face per frame (block_8002709C), so PSX animates; the
 * sector cache snapshot froze it (near/HQ subdivide reads the HQ record live,
 * which is why close-up terrain still animated). Mirror = one generation
 * counter per tile, bumped when the 16-byte entry content changes (checked
 * once per present); cached faces re-decode their texmat on gen mismatch.
 * Sector verts/colours are NOT anim-rewritten storage (colour anim goes
 * through the live-read colour arrays; OPENPETE_TERRAIN_ANIMDBG reports which
 * tiles and sector vertex or colour words change between ticks). */
static uint32_t s_tgen[128];
static uint32_t s_tgen_epoch = 1;     /* bumped when ANY tile gen bumps */
static uint32_t s_lqsnap[128][4];
static uint32_t s_lqsnap_base;
/** @brief Bumps a tile's generation when its LQ table entry changed since the last call.
 *
 * Compares the first four words of each of min(texCnt, 128) entries at
 * lqTable + t * tex_stride with a snapshot. A table that moved since the last
 * call counts every entry as changed. A changed entry steps s_tgen[t] and
 * s_tgen_epoch, which makes cached faces re-decode their materials. Returns
 * at once when lqTable is 0.
 * @param cpu The guest CPU state.
 * @param lqTable The LQ texture table's guest address, or 0.
 * @param texCnt The entry count. */
static void tsc_tile_gens(CPUState* cpu, uint32_t lqTable, uint32_t texCnt) {
    if (!lqTable) return;
    if (texCnt > 128) texCnt = 128;
    int rebase = (lqTable != s_lqsnap_base);
    for (uint32_t t = 0; t < texCnt; t++) {
        uint32_t w[4]; int diff = rebase;
        for (int k = 0; k < 4; k++) {
            w[k] = rd(cpu, lqTable + t * game_layout.terrain.tex_stride + (uint32_t)k * 4u);
            diff |= (w[k] != s_lqsnap[t][k]);
        }
        if (diff) { memcpy(s_lqsnap[t], w, sizeof(w)); s_tgen[t]++; s_tgen_epoch++; }
    }
    s_lqsnap_base = lqTable;
}
typedef struct {
    uint32_t s, sig[5];
    uint32_t animEpoch;                      /* s_tgen_epoch at last texmat refresh */
    uint32_t lastUse;                        /* s_tsc_stamp at last hit - LRU eviction key */
    int nLpV, nLpF, nHpV, nHpF, flags;
    int flat;                                /* header word 1 carries the layout's flat flag */
    int nLpC, nHpC;                          /* colour-array entry counts (GPU-cache liveness hash) */
    uint32_t lpColSpan, hpColSpan;           /* colour bytes each tier's faces index (tsc_decode_faces) */
    int nGapfill;                            /* LP faces classified HP-coverage-gap fillers */
    int nUncov;                              /* LP faces tagged uncov (tsc_tag_uncovered) */
    int nUncovX;                             /* LP faces tagged uncovX */
    struct tsimp* imp;                       /* HP faces borrowed from the source level, or NULL */
    uint8_t virt;                            /* a source sector imported whole (tsv_is_virtual) */
    int lpNearSz;                            /* LP near-outcode SZ3 bound, 0 without the rule */
    unsigned fillGen;                        /* openpete_fillpick_generation() at last tagging */
    int originX, originY, originZ;
    int centreX, centreY, centreZ, radius;   /* header bounding sphere (w0/w1) - the .s LP/HP binning inputs */
    /* Vertex AABB over BOTH tiers' decoded verts (origin-relative decode
     * units, same frame the walk's per-vertex transform consumes). The
     * near-claim refinement transforms its 8 corners: view-Z is linear in
     * xyz, so the corner minimum bounds every vertex - a sector whose
     * box-min m3 stays at or beyond the claim threshold provably contains
     * no vertex the sphere test was guarding against. Invalid (vMinX >
     * vMaxX) when the sector decoded no verts. */
    int16_t vMinX, vMinY, vMinZ, vMaxX, vMaxY, vMaxZ;
    uint32_t lpColBase, nearBase, farBase, lpFaces, hpFaces;
    tsvert_t* lpV; tsface_t* lpF;
    tsvert_t* hpV; tsface_t* hpF;
    /* Poly-anim sectors only (lazy): the previous canonical tick's decoded verts
     * + the sub-tick fractional lerp banks (x,y,z triples). vrollPres/vfracPres
     * stamp which terrain_build filled them (dedupes sectors named by BOTH the
     * LP and HP descriptor lists, and invalidates lerp banks the moment a
     * sector leaves the anim registry - a stale stamp never matches). */
    tsvert_t* lpVprev; tsvert_t* hpVprev;
    float* lpVf; float* hpVf;
    uint32_t vrollPres, vfracPres;
    int polyanim;   /* named by an env poly-anim descriptor (vert banks animate) */
    uint32_t vhash; /* LP+HP packed vertex words: catches an in-place rewrite */
    /* Pack-aware LP grade (tsc_lp_grade): face-weighted mean stock->pack
     * affine of this sector's HP textures. 0 = uncomputed, 1 = fit,
     * 2 = no signal (untextured/unmatched sector - level mean applies). */
    float lpg_p[3], lpg_q[3];
    uint8_t lpg_state; unsigned lpg_gen;
    /* GPU sector-mesh cache liveness: decodeGen bumps on every (re)decode of
     * this slot (a global monotonic counter, so slot reuse for a different
     * sector always mismatches); texGen when the env texture anim re-decoded
     * any cached texmat; vertGen when the geometry env-anim actually CHANGED
     * the decoded verts (content hash, not the unconditional per-tick
     * re-decode). */
    uint32_t decodeGen, texGen, vertGen, vertHash;
    const struct canim* canim;   /* this sector's env-anim colour slot ... */
    uint32_t canimPres;          /* ... valid while this equals s_vanim_pres
                                  * (stamped per build by tsc_col_anim_refresh) */
} tscache_t;
#define TSC_SLOTS 512
/* Linear-probe window for lookup, placement, and eviction (must agree
 * everywhere a sector's slot is derived from its hash). Sized so a fully
 * GPU-owned window - the one case eviction cannot pick a victim without
 * un-drawing an owner - needs 17 owned sectors hashing together. */
#define TSC_WINDOW 16u
static tscache_t s_tsc[TSC_SLOTS];
static int tgpu_slot_hot(uint32_t idx);   /* GPU-owned this present (defined with s_tgpu) */
static uint32_t s_tsc_stamp;                 /* bumped once per terrain_build call */
static long s_tsc_hit, s_tsc_miss;

/* ---- pack-aware LP grade ---------------------------------------------------
 * LP colours are AUTHORED against the sector's stock textures: raw at every
 * distance, they read seamlessly against fully-fogged stock HP - the .s
 * LP/HP handoff is invisible purely by that colour agreement. A replacement
 * pack regrades the textured tiers and breaks it: the handoff contour shows
 * as a stepped bright-vs-dark seam. Restore the authoring relationship by
 * regrading LP the way the pack regraded THIS sector's own ground: the
 * face-weighted mean of the sector's HP textures' stock->pack affine fits
 * (texture_pack_grade_fit). Sectors with no textured/matched HP (the LP-only
 * 0x4000 hedges) take the level mean of the fitted sectors, accumulated per
 * walk (one-present lag on the very first frame). Display-only; inert
 * without an active pack. OPENPETE_TEXPACK_LP_FIT=0 disables (A/B). */
static int    s_lpg_en = -1;
static int    s_lpg_active;                  /* this walk: enabled && pack active */
static double s_lpg_acc[6];                  /* this walk's Σp,Σq (level mean) */
static int    s_lpg_acc_n;
static float  s_lpg_mean[6];                 /* last walk's mean (p,q) */
static int    s_lpg_mean_ok;

/** @brief Fits a sector's texture-pack colour grade from its HP faces, once per pack generation.
 *
 * Returns when lpg_state is set and lpg_gen equals texture_pack_generation.
 * Otherwise sets lpg_state to 2 (attempted, no fit) and, for each textured
 * HP face whose texture the pack resolves and matches, fits the per-channel
 * affine (p, q) of texture_pack_grade_fit. The mean over the fitted faces
 * goes to lpg_p and lpg_q and lpg_state becomes 1. A sector without HP
 * faces, or with no fitted face, stays at 2.
 * @param C The sector's decode cache entry; updated. */
static void tsc_lp_grade(tscache_t* C) {
    unsigned gen = texture_pack_generation();
    if (C->lpg_state && C->lpg_gen == gen) return;
    C->lpg_gen = gen; C->lpg_state = 2;
    if (!C->hpF) return;
    double sp[3] = {0}, sq[3] = {0};
    int n = 0;
    for (int f = 0; f < C->nHpF; f++) {
        const tsface_t* F = &C->hpF[f];
        const tmat* m = &F->mat;
        if (!m->textured) continue;
        float u0 = F->tu[0], v0 = F->tv[0], u1 = u0, v1 = v0;
        for (int k = 1; k < 4; k++) {
            if (F->tu[k] < u0) u0 = F->tu[k];
            if (F->tu[k] > u1) u1 = F->tu[k];
            if (F->tv[k] < v0) v0 = F->tv[k];
            if (F->tv[k] > v1) v1 = F->tv[k];
        }
        texture_pack_id id; texture_pack_repl repl;
        if (!texture_pack_resolve(m->tpx, m->tpy, m->clx, m->cly, m->bpp,
                                  (int)u0, (int)v0, (int)u1, (int)v1, &id)) continue;
        if (!texture_pack_match(&id, &repl)) continue;
        float p[3], q[3];
        if (!texture_pack_grade_fit(&id, &repl, m->tpx, m->tpy, m->clx, m->cly,
                                    m->bpp, (int)u0 - (id.box_x - repl.box_x),
                                    (int)v0 - (id.box_y - repl.box_y), p, q)) continue;
        for (int c = 0; c < 3; c++) { sp[c] += p[c]; sq[c] += q[c]; }
        n++;
    }
    if (n > 0) {
        for (int c = 0; c < 3; c++) {
            C->lpg_p[c] = (float)(sp[c] / n);
            C->lpg_q[c] = (float)(sq[c] / n);
        }
        C->lpg_state = 1;
    }
}

/** @brief Applies a sector's texture-pack grade, else the level mean, to one 0x00BBGGRR colour word.
 *
 * Each channel becomes p * c + q * 255, rounded and clamped to 0..255. A
 * sector without a fitted grade uses the level mean when one exists, and
 * the colour passes through unchanged otherwise.
 * @param C The sector's decode cache entry, carrying its grade.
 * @param rgb The authored colour.
 * @return The graded colour. */
static uint32_t lp_grade_apply(const tscache_t* C, uint32_t rgb) {
    const float* p; const float* q;
    if (C->lpg_state == 1) { p = C->lpg_p; q = C->lpg_q; }
    else if (s_lpg_mean_ok) { p = s_lpg_mean; q = s_lpg_mean + 3; }
    else return rgb;
    uint32_t out = 0;
    for (int c = 0; c < 3; c++) {
        int v = (int)(p[c] * (float)((rgb >> (8 * c)) & 0xFF) + q[c] * 255.0f + 0.5f);
        if (v < 0) v = 0; if (v > 255) v = 255;
        out |= (uint32_t)v << (8 * c);
    }
    return out;
}
static long s_tocc_culled;   /* sectors occlusion-culled this window (census) */
/* Occlusion-cull verdict latch. The occluder depth grid advances once per
 * present, while the canonical walk runs once per tick: querying it again on
 * each sub-tick re-extract lets a sector's cull verdict flip between the
 * presents of one tick, so terrain that the tick's canonical walk emits
 * disappears for the rest of the tick and returns at the next one. The
 * canonical walk's verdict is latched here and every re-extract of that tick
 * reuses its depth half (TOCC_DEPTH), so the depth-admitted set is a property
 * of the tick; the frustum half is re-tested on each walk's own rect
 * (tocc_sector_verdict), and a latched TOCC_FRUSTUM only feeds the census. A
 * sector the canonical walk did not reach is not latched and is kept
 * (conservative: the cull is a perf skip and may only ever remove provably
 * hidden work). */
static openpete_occl_camera s_occl_cam;  /* this walk's camera, for the occluder grid */
static uint8_t  s_tocc_latch[1024];      /* TOCC_* verdict the tick's canonical walk took */
static uint32_t s_tocc_latch_stamp[1024];
static uint32_t s_tocc_tick;             /* serial of the last canonical walk */
/* Per-walk census (OPENPETE_TERRAIN_FARDBG header): sectors whose canonical
 * walk rejected them off screen while this walk's own rect is on screen,
 * split by whether this walk culled them (latoff) or kept them (offadm). */
static int s_tocc_latoff, s_tocc_offadm;
/** @brief Prints and resets the decode-cache hit and miss counts on every 120th call under OPENPETE_TERRAIN_CACHE_DBG.
 *
 * The variable is latched on the first call. */
static void tsc_census(void) {   /* OPENPETE_TERRAIN_CACHE_DBG: ~1/sec hit/miss */
    static int on = -1, n;
    ENV_FLAG_LATCH(on, "OPENPETE_TERRAIN_CACHE_DBG");
    if (on && ++n >= 120) {
        fprintf(stderr, "[ter-cache] hit=%ld miss=%ld\n", s_tsc_hit, s_tsc_miss);
        s_tsc_hit = s_tsc_miss = 0; n = 0;
    }
}

/** @brief Returns a sector's LP near-outcode SZ3 bound from its header word +0x10, or 0 without the rule.
 *
 * The bound is terrain.lp_near_sz minus terrain.lp_near_sz_step times bits
 * 24-25 of the word, as the LP vertex transform derives it.
 * @param w10 The sector header's word at +0x10.
 * @return The bound, or 0 on a game whose LP walk has no near outcode. */
static int tsc_lp_near_sz(uint32_t w10) {
    if (!game_layout.terrain.lp_near_sz) return 0;
    return game_layout.terrain.lp_near_sz
           - game_layout.terrain.lp_near_sz_step * (int)((w10 >> 24) & 3u);
}

/* A sector's words come from guest RAM or from a host copy of a level's
 * terrain sub-file (the HP source level, tsimp_attach); the decoders read
 * both through one source. A host source addresses its bytes by offset. */
typedef struct {
    CPUState* cpu;          /* guest reads, and the texture tables' reads */
    const uint8_t* host;    /* host bytes, or NULL for guest RAM */
    uint32_t len;           /* host byte count */
} twsrc_t;

/** @brief Reads one word of a sector through its source.
 *
 * A guest source reads guest RAM; a host source reads the little-endian
 * word at byte offset @p a, 0 past the end.
 * @param src The source.
 * @param a The guest address or host byte offset.
 * @return The word. */
static uint32_t tw_rd(const twsrc_t* src, uint32_t a) {
    if (!src->host) return rd(src->cpu, a);
    if (src->len < 4u || a > src->len - 4u) return 0u;
    const uint8_t* p = src->host + a;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/** @brief Unpacks nv packed vertex words into local vertex coordinates.
 *
 * Reads the words at verts + 4k and unpacks each with tvert_unpack; the
 * components are truncated to int16.
 * @param src The sector's word source.
 * @param verts The first packed word's address.
 * @param nv The vertex count.
 * @param flat The sector header's flat flag.
 * @param out Receives nv vertices. */
static void tsc_decode_verts(const twsrc_t* src, uint32_t verts, int nv, int flat, tsvert_t* out) {
    for (int k = 0; k < nv; k++) {
        uint32_t v = tw_rd(src, verts + (uint32_t)k * 4);
        int x, y, z; tvert_unpack(v, flat, &x, &y, &z);
        out[k].x = (int16_t)x; out[k].y = (int16_t)y; out[k].z = (int16_t)z;
    }
}

/** @brief Decodes a sector tier's face words into out[0..nf).
 *
 * HP faces (stride 16) carry byte vertex indices, four colour byte offsets
 * in word 1, and the texture and flag fields of the game's layout; LP faces
 * (stride 8) carry packed vertex and colour indices and no texture. A host
 * source decodes no HP material: its texture indices name another level's
 * table, which the caller resolves.
 * @param src The sector's word source; its cpu reads the texture tables.
 * @param faces Address of the first face.
 * @param nf Face count.
 * @param faceStride 16 for HP faces, 8 for LP faces.
 * @param lqTable The LQ texture table's guest address.
 * @param hqTable The HQ texture table's guest address.
 * @param texTblMode 2 to take HP textures from the HQ table.
 * @param hiDetail Nonzero to decode the near texture half.
 * @param out Receives the faces.
 * @return Bytes of the colour array the faces index: the largest corner
 *         colour offset plus 4 (4 when nf is 0). */
static uint32_t tsc_decode_faces(const twsrc_t* src, uint32_t faces, int nf, int faceStride,
                                 uint32_t lqTable, uint32_t hqTable, int texTblMode,
                                 int hiDetail, tsface_t* out) {
    CPUState* cpu = src->cpu;
    uint32_t col_span = 4u;
    for (int f = 0; f < nf; f++) {
        tsface_t* F = &out[f];
        uint32_t w0 = tw_rd(src, faces + (uint32_t)f * faceStride);
        uint32_t w1 = tw_rd(src, faces + (uint32_t)f * faceStride + 4);
        memset(F, 0, sizeof(*F));
        if (faceStride == 16) {                    /* HP: byte indices; the colour indices in word1 */
            F->i0 = (uint8_t)((w0 >> 24) & 0xFF); F->i1 = (uint8_t)((w0 >> 16) & 0xFF);
            F->i2 = (uint8_t)((w0 >> 8) & 0xFF);  F->i3 = (uint8_t)(w0 & 0xFF);
            uint32_t w2 = tw_rd(src, faces + (uint32_t)f * faceStride + 8);
            uint32_t w3 = tw_rd(src, faces + (uint32_t)f * faceStride + 12);
            F->w3 = w3;
            F->c0 = (uint16_t)((w1 >> 22) & 0x3FC); F->c1 = (uint16_t)((w1 >> 14) & 0x3FC);
            F->c2 = (uint16_t)((w1 >> 6) & 0x3FC);  F->c3 = (uint16_t)((w1 << 2) & 0x3FC);
            int tb;
            if (game_layout.terrain.face_layout == TERRAIN_FACES_SPYRO3) {
                /* Spyro 3: word2 is not read by the draw; word3 carries the
                 * texture index (bits 0-6), the OT bias (7-9), flip (10) and
                 * the GT3 UV rotation (12-13). Every HP face is textured;
                 * semi-transparency comes from the tile's TPAGE. The near/HQ
                 * sub-tiles live inside that same table entry, so the HQ index
                 * is the texture index (environment.s 80025904 re-derives it
                 * from word3 for the subdivide). */
                F->flip = (w3 & 0x400) != 0; F->fogbias = 0;
                F->otbias = (uint8_t)((w3 >> 7) & 0x7);
                F->dside = (w3 & 0x800) != 0;
                F->hq_idx = (uint8_t)(w3 & 0x7F); F->abe = 0;
                F->trirot = (uint8_t)((w3 >> 12) & 3u);
                tb = (int)(w3 & 0x7F);
            } else {                               /* Spyro 1: word2 tex byte + HQ index; word3 flags */
                F->flip = (w3 & 0x2) != 0; F->fogbias = (w3 & 0x1) != 0;
                F->otbias = (uint8_t)((w3 >> 3) & 0x7);
                F->dside = (w3 & 0x4) != 0;
                F->hq_idx = (uint8_t)(w2 & 0x7F); F->abe = (w2 & 0x80) != 0;
                F->trirot = (uint8_t)((w2 >> 8) & 3u);
                tb = (int)(int8_t)(w2 & 0xFF);
            }
            uint32_t tbl = (texTblMode == 2) ? hqTable : lqTable;
            F->tb = (int8_t)tb;
            F->tgen = (tb == -1) ? 0 : s_tgen[tb & 0x7F];
            if (!src->host) {
                terrain_texmat(cpu, tbl, tb, hiDetail, &F->mat, F->tu, F->tv);
                /* Both tiledef halves are cached; the walk picks per face by the
                 * .s distance rule (t0 = key−fogRef+4096; t0<=0 -> the +8 half).
                 * Animated tiles re-decode when their gen bumps (tsc_refresh_anim). */
                terrain_texmat(cpu, tbl, tb, 1, &F->matN, F->tuN, F->tvN);
            }
        } else if (game_layout.terrain.face_layout == TERRAIN_FACES_SPYRO3) {
            /* Spyro 3 LP: 7-bit vertex indices at bits 30-24, 23-17, 16-10,
             * 9-3 of word0 (bit 31 double-sided), 7-bit colour indices at
             * 31-25, 24-18, 17-11, 10-4 of word1 with the OT bias in 3-0; no
             * admission field and never semi-transparent. The colour fields
             * are byte offsets into the colour list, as in Spyro 1. */
            F->i0 = (uint8_t)((w0 >> 24) & 0x7F); F->i1 = (uint8_t)((w0 >> 17) & 0x7F);
            F->i2 = (uint8_t)((w0 >> 10) & 0x7F); F->i3 = (uint8_t)((w0 >> 3) & 0x7F);
            F->admit = 0;
            F->dside = (w0 & 0x80000000u) != 0;
            F->otbias = (uint8_t)(w1 & 0xF);
            F->c0 = (uint16_t)(((w1 >> 25) & 0x7F) << 2); F->c1 = (uint16_t)(((w1 >> 18) & 0x7F) << 2);
            F->c2 = (uint16_t)(((w1 >> 11) & 0x7F) << 2); F->c3 = (uint16_t)(((w1 >> 4) & 0x7F) << 2);
            F->tb = -1;
            F->mat.bpp = 16;                       /* untextured flat gouraud */
        } else {                                   /* Spyro 1 LP: 6-bit indices; untextured */
            F->i0 = (uint8_t)(((w0 >> 23) & 0x1F8) >> 3); F->i1 = (uint8_t)(((w0 >> 17) & 0x1F8) >> 3);
            F->i2 = (uint8_t)(((w0 >> 11) & 0x1F8) >> 3); F->i3 = (uint8_t)(((w0 >> 5) & 0x1F8) >> 3);
            F->admit = (uint8_t)(w0 & 0x1F);       /* authored LP admission (crossover in 1024-world steps) */
            F->dside = (w0 & 0x80) != 0;           /* double-sided: draw regardless of facing */
            F->otbias = (uint8_t)((w1 >> 3) & 0x1F);
            F->c0 = (uint16_t)((w1 >> 24) & 0xFC); F->c1 = (uint16_t)((w1 >> 18) & 0xFC);
            F->c2 = (uint16_t)((w1 >> 12) & 0xFC); F->c3 = (uint16_t)((w1 >> 6) & 0xFC);
            F->tb = -1;
            F->mat.bpp = 16;                       /* untextured flat gouraud */
            /* w1 bit 2 = ABE: the .s LP emit (quad 0x80026504 / tri 0x800266D4)
             * ORs 0x02 into the G4/G3 prim code and precedes the prim with a
             * DR_TPAGE carrying ABR = w1 & 3 - untextured SEMI-TRANSPARENT
             * gouraud (the distant-waterfall veils). Same scene blend classes
             * (1..4 = ABR 0..3) as the HP tpage decode. */
            if (w1 & 0x4) F->mat.blend = (int)(w1 & 0x3) + 1;
        }
        F->quad = (F->i2 != F->i3);
        const uint16_t c[4] = { F->c0, F->c1, F->c2, F->c3 };
        for (int k = 0; k < 4; k++)
            if ((uint32_t)c[k] + 4u > col_span) col_span = (uint32_t)c[k] + 4u;
    }
    return col_span;
}

/* ---- LP fill-face tags (frozen, measurement-derived) -----------------------
 * Which LP faces may back-fill sightlines no authored HP covers is a FROZEN,
 * human-reviewed list (terrain_fill_tags.h, generated by
 * scripts/gen-fill-tags.py from tests/freecam/fill-tags.verdicts). Candidates
 * are measured per pixel from the game's own renderer - og-original coverage
 * minus pure-HP coverage (m_LodDistance doubled) - attributed to LP faces by
 * projected screen footprint, then reviewed face-by-face. FILL faces are
 * og-terrain-over-void (water planes, level-boundary channels); every
 * rejected candidate was a silhouette sliver between the coarse/alternate LP
 * shape and the HP silhouette, where filling paints LP wedges over sky.
 * Absence from the table = not a fill face: a face the sweep never measured
 * fails loud (an unfilled hole at a new camera) instead of silently shipping
 * a guess - the failure mode is visible and local, unlike the threshold
 * classifier this replaces (six tuned constants + a per-level kill list,
 * v1-v12, each version patching a newly discovered misprediction). */
/* The frozen list itself is the game's (terrain_game.fill_tag): Spyro 1's is
 * renderer/terrain_fill_tags.h. */

/** @brief Returns an LP face's effective fill tag: the inspector's override, else the frozen table's.
 *
 * An in-game inspector session (P key) overrides the frozen table
 * (terrain_game.fill_tag) for the faces it has re-judged, so a Y/N press
 * re-renders the scene with the new verdict on the next present and the
 * audit runs against the live image instead of a rebuild. With no session
 * verdict recorded for the face, the frozen table decides.
 * @param level The level id.
 * @param sec The sector index.
 * @param face The LP face index within the sector.
 * @return 1 when the face is a fill face, else 0. */
static int tft_fill(int level, int sec, int face) {
    int ov = openpete_fillpick_lookup(level, sec, face);
    if (ov != OPENPETE_FILLPICK_NONE) return ov == OPENPETE_FILLPICK_FILL;
    return terrain_game.fill_tag(level, sec, face);
}

/** @brief Prints the grid sweep report (OPENPETE_FILLPICK_GRID): the grid samples each LP face owns and how they split on cover.
 *
 * It prints once per process, on the first walk with a grid armed, one
 * [fillgrid-hdr] line and one [fillgrid] line per LP face (at most 512),
 * with the face's effective tag. A sample (point 0, the crosshair, is not
 * one) counts as:
 *   bare   = an LP face is the nearest terrain and no authored HP is on the
 *            sightline. This is the fill case: at og's LOD the LP is what
 *            covers those pixels, and without a FILL tag nothing does.
 *   behind = an HP face is on the sightline but farther than the LP: the
 *            coarse LP is in front of cover that already exists (the
 *            punch-through class a wrong FILL tag ships).
 *   cover  = HP is nearer; the LP is hidden whatever its tag says.
 * The report is a candidate list: the cells are samples, so a face with a
 * handful of `bare` cells along a silhouette is the sliver class the frozen
 * list rejects, while a face with a broad bare region is the
 * og-terrain-over-void class it accepts.
 * @param level The level id.
 * @param level_name The level's name as the verdict files spell it. */
static void fp_grid_report(int level, const char* level_name) {
    if (!s_fp_grid_cols) return;
    static int s_done;
    if (s_done) return;
    s_done = 1;
    struct { int sec, face, bare, behind, cover, tagged; } acc[512];
    int n = 0;
    for (int k = 1; k < s_fp_np; k++) {
        const fp_point* P = &s_fp_pt[k];
        if (P->lp_sec < 0) continue;
        int j = 0;
        for (; j < n; j++) if (acc[j].sec == P->lp_sec && acc[j].face == P->lp_face) break;
        if (j == n) {
            if (n >= (int)(sizeof acc / sizeof acc[0])) continue;
            acc[n].sec = P->lp_sec; acc[n].face = P->lp_face;
            acc[n].bare = acc[n].behind = acc[n].cover = 0;
            acc[n].tagged = tft_fill(level, P->lp_sec, P->lp_face);
            n++;
        }
        if (P->hp_sec < 0)          acc[j].bare++;
        else if (P->hp_z > P->lp_z) acc[j].behind++;
        else                        acc[j].cover++;
    }
    fprintf(stderr, "[fillgrid-hdr] %s grid=%dx%d faces=%d\n",
            level_name, s_fp_grid_cols, s_fp_grid_rows, n);
    for (int j = 0; j < n; j++)
        fprintf(stderr, "[fillgrid] %s %d %d tag=%s bare=%d behind=%d cover=%d\n",
                level_name, acc[j].sec, acc[j].face,
                acc[j].tagged ? "FILL" : "NOFILL",
                acc[j].bare, acc[j].behind, acc[j].cover);
}

/** @brief Publishes this walk's crosshair winners to the host and logs every change.
 *
 * The host (the overlay and the inspector's key bindings) receives point 0's
 * nearest LP face with its effective fill verdict (tft_fill) and whether an
 * inspector session overrides it, and its nearest HP face. A [fillpick]
 * line prints whenever the LP face, level or verdict changes, so a scripted
 * or headless run is a transcript; the level name is the verdict files'
 * spelling, so a logged line is one column short of a verdicts row. Then
 * the grid report runs (fp_grid_report).
 * @param cpu The guest CPU state; supplies the level id. */
static void fp_publish(CPUState* cpu) {
    openpete_fillpick_hit h;
    memset(&h, 0, sizeof h);
    h.level = (int)(int32_t)rd(cpu, game_layout.level_id);
    terrain_game.level_name(h.level, h.level_name, sizeof h.level_name);
    const fp_point* X = &s_fp_pt[0];
    if (X->lp_sec >= 0) {
        h.valid = 1;
        h.sec = X->lp_sec; h.face = X->lp_face;
        h.quad = X->lp_quad; h.vz = X->lp_z;
        /* The face's own tag bit is unreliable here: preview sets it on
         * everything. Ask the list. */
        h.verdict = tft_fill(h.level, h.sec, h.face) ? OPENPETE_FILLPICK_FILL
                                                     : OPENPETE_FILLPICK_NOFILL;
        h.overridden = openpete_fillpick_lookup(h.level, h.sec, h.face)
                       != OPENPETE_FILLPICK_NONE;
    }
    if (X->hp_sec >= 0) {
        h.hp_valid = 1;
        h.hp_sec = X->hp_sec; h.hp_face = X->hp_face; h.hp_vz = X->hp_z;
    }
    openpete_fillpick_publish(&h);
    fp_grid_report(h.level, h.level_name);

    static int last_sec = -2, last_face = -2, last_lvl = -2, last_verdict = -2;
    if (h.valid && (h.sec != last_sec || h.face != last_face ||
                    h.level != last_lvl || h.verdict != last_verdict)) {
        last_sec = h.sec; last_face = h.face;
        last_lvl = h.level; last_verdict = h.verdict;
        fprintf(stderr, "[fillpick] %s %d %d  tag=%s%s  lp_vz=%.0f  hp=%s",
                h.level_name, h.sec, h.face,
                h.verdict == OPENPETE_FILLPICK_FILL ? "FILL" : "NOFILL",
                h.overridden ? "*" : "", h.vz,
                h.hp_valid ? "" : "none");
        if (h.hp_valid)
            fprintf(stderr, "s%d/f%d vz=%.0f (%s LP by %.0f)",
                    h.hp_sec, h.hp_face, h.hp_vz,
                    h.hp_vz < h.vz ? "in front of" : "behind",
                    fabs(h.hp_vz - h.vz));
        fputc('\n', stderr);
    }
}

/* ---- authored-data art corrections (native display only) -----------------
 * Level-data authoring bugs corrected at the native colour fetch; emulated RAM
 * stays canonical, so the PSX/og legs keep the authentic data and gameplay
 * state is untouched. The table is the game's (terrain_game.lp_artfix), keyed
 * on exact (level id, sector, face). OPENPETE_TERRAIN_ARTFIX=0 restores
 * authentic. */
/** @brief Looks up the game's authored-data colour correction for one LP face.
 *
 * The level id is read once per VBL. Returns 0 at once when
 * OPENPETE_TERRAIN_ARTFIX=0 (latched).
 * @param cpu The guest CPU state; supplies the level id.
 * @param sec The sector index.
 * @param face The LP face index.
 * @param rgb Receives the corrected 0x00BBGGRR colour.
 * @return 1 when the face is corrected, else 0. */
static int lp_artfix_lookup(CPUState* cpu, int sec, int face, uint32_t* rgb) {
    static int s_on = -1;
    if (s_on < 0) { const char* e = getenv("OPENPETE_TERRAIN_ARTFIX");
                    s_on = !(e && *e == '0'); }
    if (!s_on) return 0;
    static int s_lvl = -1, s_lvl_frame = -1;
    if (s_lvl_frame != (int)openpete_frame) {   /* one RAM read per VBL */
        s_lvl = (int)(int32_t)rd(cpu, game_layout.level_id);
        s_lvl_frame = (int)openpete_frame;
    }
    return terrain_game.lp_artfix(s_lvl, sec, face, rgb);
}

/** @brief Dumps every sector's authored LOD fields and packed local vertices to stderr, once per sector table.
 *
 * Runs under OPENPETE_TERRAIN_FIELDCENSUS (any value) from tsc_tag_gapfill.
 * Walks the whole sector table in guest RAM rather than the decode cache,
 * since decode is lazy and camera-dependent. A sector table whose pointer
 * and count hash to the previous call's key is skipped. Emits [fc-hdr], then
 * per sector [fc-sec], [fc-lpv] and [fc-hpv] (at most 256 vertices each),
 * one [fc-lpf] per LP face and one [fc-hpf] per HP face; at most 1024
 * sectors. The layout mirrors tsc_get and tsc_decode_faces.
 * @param cpu The guest CPU state.
 * @param sectorPtr The sector pointer table's guest address.
 * @param sectorCnt The sector count. */
static void tsc_field_census(CPUState* cpu, uint32_t sectorPtr, uint32_t sectorCnt) {
    static uint32_t last_key;
    uint32_t ck = sectorPtr ^ (sectorCnt * 2654435761u) ^ 0x5CA1AB1Eu;
    if (ck == last_key) return;
    last_key = ck;
    int level = (int)(int32_t)rd(cpu, game_layout.level_id);
    uint32_t n = sectorCnt > 1024 ? 1024 : sectorCnt;
    fprintf(stderr, "[fc-hdr] level=%d sectors=%u\n", level, n);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t s = rd(cpu, sectorPtr + i * 4); if (!s) continue;
        uint32_t g0 = rd(cpu, s + 0x04), g3 = rd(cpu, s + 0x10), g4 = rd(cpu, s + 0x14);
        int nLpV = (int)(g3 & 0xFF), nLpC = (int)((g3 >> 8) & 0xFF), nLpF = (int)((g3 >> 16) & 0xFF);
        int nHpV = (int)(g4 & 0xFF), nHpC = (int)((g4 >> 8) & 0xFF), nHpF = (int)((g4 >> 16) & 0xFF);
        uint32_t hpVerts = s + 0x1C + ((g4 >> 24) & 0xFF) * 4;
        fprintf(stderr, "[fc-sec] s=%u flags=%04X rad=%d nLpV=%d nLpF=%d nHpV=%d nHpF=%d\n",
                i, (unsigned)thdr_flags(g0), thdr_radius(g0), nLpV, nLpF, nHpV, nHpF);
        for (int bank = 0; bank < 2; bank++) {
            uint32_t verts = bank ? hpVerts : s + 0x1C;
            int nv = bank ? nHpV : nLpV;
            if (nv <= 0) continue;
            fprintf(stderr, "[fc-%sv] s=%u", bank ? "hp" : "lp", i);
            for (int k = 0; k < nv && k < 256; k++) {
                uint32_t v = rd(cpu, verts + (uint32_t)k * 4);
                int fx, fy, fz; tvert_unpack(v, thdr_flat(g0), &fx, &fy, &fz);
                fprintf(stderr, " %d,%d,%d", fx, fy, fz);
            }
            fputc('\n', stderr);
        }
        uint32_t lpFaces = s + 0x1C + (uint32_t)(nLpV + nLpC) * 4;
        for (int f = 0; f < nLpF; f++) {
            uint32_t w0 = rd(cpu, lpFaces + (uint32_t)f * 8);
            uint32_t w1 = rd(cpu, lpFaces + (uint32_t)f * 8 + 4);
            fprintf(stderr, "[fc-lpf] s=%u f=%d a=%u b=%u ds=%u i=%u,%u,%u,%u\n",
                    i, f, (unsigned)(w0 & 0x1F), (unsigned)((w1 >> 3) & 0x1F),
                    (unsigned)((w0 >> 7) & 1),
                    (unsigned)((w0 >> 26) & 0x3F), (unsigned)((w0 >> 20) & 0x3F),
                    (unsigned)((w0 >> 14) & 0x3F), (unsigned)((w0 >> 8) & 0x3F));
        }
        uint32_t hpFaces = hpVerts + (uint32_t)(nHpV + nHpC * 2) * 4;
        for (int f = 0; f < nHpF; f++) {
            uint32_t w0 = rd(cpu, hpFaces + (uint32_t)f * 16);
            uint32_t w3 = rd(cpu, hpFaces + (uint32_t)f * 16 + 12);
            fprintf(stderr, "[fc-hpf] s=%u f=%d b=%u fp=%u fl=%u ds=%u ns=%u i=%u,%u,%u,%u\n",
                    i, f, (unsigned)((w3 >> 3) & 0x7), (unsigned)(w3 & 1),
                    (unsigned)((w3 >> 1) & 1), (unsigned)((w3 >> 2) & 1),
                    (unsigned)((w3 >> 6) & 3),
                    (unsigned)((w0 >> 24) & 0xFF), (unsigned)((w0 >> 16) & 0xFF),
                    (unsigned)((w0 >> 8) & 0xFF), (unsigned)(w0 & 0xFF));
        }
    }
}

/** @brief Tags a cached sector's LP faces with the floor bit and the gap-fill flag.
 *
 * Runs tsc_field_census first when OPENPETE_TERRAIN_FIELDCENSUS is set.
 * Returns when the sector has no LP faces or vertices. Otherwise clears
 * nGapfill and finds the sector's table index by scanning up to 1024 sector
 * pointers for C->s; the index is -1 when absent and under GS_Credits, whose
 * scenes reuse level ids. Each LP face whose four indices lie below nLpV gets
 * floorf when its winding normal points down with |nz| >= 0.8 of its length.
 * With an index, gapfill is tft_fill(level, index, face), or 1 with fillcand
 * set for an untagged face while the fill inspector previews; nGapfill counts
 * the tagged faces. A face with an index at or above nLpV keeps gapfill 0.
 * @param cpu The guest CPU state.
 * @param C The cached sector; its LP faces are updated.
 * @param sectorPtr The sector pointer table's guest address.
 * @param sectorCnt The sector count. */
static void tsc_tag_gapfill(CPUState* cpu, tscache_t* C,
                            uint32_t sectorPtr, uint32_t sectorCnt) {
    if (getenv("OPENPETE_TERRAIN_FIELDCENSUS"))
        tsc_field_census(cpu, sectorPtr, sectorCnt);
    if (!C->lpF || !C->lpV) return;
    C->nGapfill = 0;
    /* The table keys on the sector INDEX; the cache keys on the sector
     * address - recover the index from the table (decode-time only). */
    int secIdx = -1;
    uint32_t n = sectorCnt > 1024 ? 1024 : sectorCnt;
    for (uint32_t i = 0; i < n; i++)
        if (rd(cpu, sectorPtr + i * 4) == C->s) { secIdx = (int)i; break; }
    int level = (int)(int32_t)rd(cpu, game_layout.level_id);
    /* The credits overlay (func_credits_8007AA50) loads its own scene builds
     * through LoadLevelData, indexed by g_CreditsSequence, and never writes
     * g_LevelId - it keeps the id of the level the credits were entered from.
     * Those scenes are re-indexed subsets of their source levels (no sector
     * keeps its index), so a g_LevelId-keyed lookup lands the entering
     * level's tags on unrelated sectors: an Artisans fill face became the
     * near ground of the Stone Hill credits scene, drawn as untextured LP over
     * the HP. No credits scene has been swept, so under GS_Credits every face
     * is untagged (the table's absent-means-not-a-fill contract). */
    if (rd(cpu, 0x800757D8) == 15u) secIdx = -1;    /* g_Gamestate == GS_Credits */
    for (int f = 0; f < C->nLpF; f++) {
        tsface_t* F = &C->lpF[f];
        F->gapfill = 0;
        if (F->i0 >= C->nLpV || F->i1 >= C->nLpV || F->i2 >= C->nLpV || F->i3 >= C->nLpV) continue;
        {   /* up-facing-floor bit (facemap metadata; game winding: floors have
             * n_z < 0, |n_z| >= 0.8 of the normal). */
            const tsvert_t* a = &C->lpV[F->i0];
            const tsvert_t* b = &C->lpV[F->i1];
            const tsvert_t* c = &C->lpV[F->i2];
            double e1[3] = { b->x - a->x, b->y - a->y, b->z - a->z };
            double e2[3] = { c->x - a->x, c->y - a->y, c->z - a->z };
            double nz = e1[0]*e2[1] - e1[1]*e2[0];
            double nx = e1[1]*e2[2] - e1[2]*e2[1];
            double ny = e1[2]*e2[0] - e1[0]*e2[2];
            double nl = nx*nx + ny*ny + nz*nz;
            F->floorf = (nl > 0.0 && nz < 0.0 && nz*nz >= 0.64*nl);
        }
        if (secIdx >= 0) {
            F->gapfill = (uint8_t)tft_fill(level, secIdx, f);
            /* Inspector preview: force every LP face through the gapfill
             * exemptions so the UNTAGGED candidates are visible too - an
             * under-fill site is a hole the covering face does not draw into
             * until it is tagged, so it cannot be found by looking at the
             * tagged set. `fillcand` keeps the two apart for the tint (and
             * keeps the pick honest about what the shipped list says).
             * Deliberately rides `gapfill` rather than a parallel gate: every
             * exemption the tags already have - the sector-level lpGapOnly
             * arm, the authored LP admission, the handoff far-only drop - is
             * then exercised exactly as a real tag would be. */
            F->fillcand = 0;
            if (!F->gapfill && openpete_fillpick_preview()) {
                F->gapfill = 1;
                F->fillcand = 1;
            }
            C->nGapfill += F->gapfill;
        }
    }
}

/** @brief Re-derives a cached sector's fill tags after the fill inspector's list changed.
 *
 * The tags are static level data decoded once, but the inspector's session
 * verdicts and preview mode change the list while the level is live. tsc_get
 * calls this on a cache hit whose fillGen differs from
 * openpete_fillpick_generation, so an unchanged list costs one compare per
 * hit. A poly-anim sector's LP fill tags are cleared, as its LP bank is stale.
 * @param cpu The guest CPU state.
 * @param C The cached sector.
 * @param sectorPtr The sector pointer table's guest address.
 * @param sectorCnt The sector count. */
static void tsc_retag_fill(CPUState* cpu, tscache_t* C,
                           uint32_t sectorPtr, uint32_t sectorCnt) {
    if (C->virt) return;   /* an imported sector carries no guest fill tags */
    C->fillGen = openpete_fillpick_generation();
    tsc_tag_gapfill(cpu, C, sectorPtr, sectorCnt);
    if (C->polyanim && C->lpF) {          /* stale-bank guard: see tsc_sector_polyanim */
        for (int f = 0; f < C->nLpF; f++) C->lpF[f].gapfill = 0;
        C->nGapfill = 0;
    }
}

/* LP-to-HP coverage tolerance in decoded vertex units: an LP sample point
 * farther than this from every HP triangle of its sector is off the HP mesh.
 * An LP face that approximates its HP surface deviates from it by tens of
 * units; LP-only geometry over a partial HP mesh (Spyro 2's spires over a
 * ground strip) stands hundreds of units off it. At 32, coarse LP faces
 * start to draw in front of the HP surface they approximate. */
#define TSC_LPCOV_TOL 64.0

/** @brief Returns the squared distance from a point to a segment.
 *
 * The parameter of the closest point is clamped to [0, 1]; a degenerate
 * segment measures to its first end.
 * @param p The point.
 * @param a The segment's first end.
 * @param b The segment's second end.
 * @return The squared distance. */
static double tsc_seg_d2(const double p[3], const double a[3], const double b[3]) {
    double ab[3], ap[3], t = 0.0, l2 = 0.0, d2 = 0.0;
    for (int k = 0; k < 3; k++) {
        ab[k] = b[k] - a[k]; ap[k] = p[k] - a[k];
        t += ap[k] * ab[k]; l2 += ab[k] * ab[k];
    }
    t = l2 > 0.0 ? fmin(fmax(t / l2, 0.0), 1.0) : 0.0;
    for (int k = 0; k < 3; k++) { double d = ap[k] - t * ab[k]; d2 += d * d; }
    return d2;
}

/** @brief Returns the squared distance from a point to a triangle.
 *
 * A point whose projection onto the triangle's plane falls inside the
 * triangle (every edge's cross product with the corner-to-point vector on
 * the normal's side) measures to the plane; any other point, and every point
 * of a degenerate triangle, measures to the nearest of the three edges.
 * @param p The point.
 * @param v The triangle's three corners.
 * @return The squared distance. */
static double tsc_tri_d2(const double p[3], const double v[3][3]) {
    double e0[3], e1[3], n[3];
    for (int k = 0; k < 3; k++) { e0[k] = v[1][k] - v[0][k]; e1[k] = v[2][k] - v[0][k]; }
    n[0] = e0[1] * e1[2] - e0[2] * e1[1];
    n[1] = e0[2] * e1[0] - e0[0] * e1[2];
    n[2] = e0[0] * e1[1] - e0[1] * e1[0];
    double nl2 = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
    int inside = nl2 > 0.0;
    for (int e = 0; e < 3 && inside; e++) {
        const double* a = v[e]; const double* b = v[(e + 1) % 3];
        double ed[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
        double ap[3] = { p[0] - a[0], p[1] - a[1], p[2] - a[2] };
        double cx = ed[1] * ap[2] - ed[2] * ap[1];
        double cy = ed[2] * ap[0] - ed[0] * ap[2];
        double cz = ed[0] * ap[1] - ed[1] * ap[0];
        inside = (cx * n[0] + cy * n[1] + cz * n[2]) >= 0.0;
    }
    if (inside) {
        double h = (p[0] - v[0][0]) * n[0] + (p[1] - v[0][1]) * n[1] + (p[2] - v[0][2]) * n[2];
        return h * h / nl2;
    }
    double d2 = tsc_seg_d2(p, v[0], v[1]);
    d2 = fmin(d2, tsc_seg_d2(p, v[1], v[2]));
    return fmin(d2, tsc_seg_d2(p, v[2], v[0]));
}

/** @brief Copies a cached face's four corners into double triples.
 *
 * @param V The tier's decoded vertices.
 * @param F The face; its four indices must lie inside V.
 * @param q Receives the corners in i0, i1, i2, i3 order. */
static void tsc_face_corners(const tsvert_t* V, const tsface_t* F, double q[4][3]) {
    const int ci[4] = { F->i0, F->i1, F->i2, F->i3 };
    for (int c = 0; c < 4; c++) {
        q[c][0] = V[ci[c]].x; q[c][1] = V[ci[c]].y; q[c][2] = V[ci[c]].z;
    }
}

/* One HP triangle mesh the coverage tests measure against: a sector's own
 * HP tier, or the HP faces it borrows (tsimp_t), both in the sector's
 * decoded vertex frame. */
typedef struct { const tsvert_t* V; const tsface_t* F; int nV, nF; } tsmesh_t;

/** @brief Returns the squared distance from a point to a set of HP meshes, stopping early inside a bound.
 *
 * Each face whose indices lie inside its mesh's vertex bank contributes the
 * triangles the walk emits: (i0, i1, i2), and for a quad also (i1, i2, i3).
 * The meshes are scanned in order and the scan returns as soon as one
 * triangle lies within stop2.
 * @param M The meshes.
 * @param nm The mesh count.
 * @param p The point, in the sector's decoded vertex frame.
 * @param stop2 The squared distance at which the scan stops.
 * @return The smallest squared distance found, or a value at or below stop2. */
static double tsc_hp_d2(const tsmesh_t* M, int nm, const double p[3], double stop2) {
    double best = INFINITY;
    for (int m = 0; m < nm; m++) {
        for (int f = 0; f < M[m].nF && best > stop2; f++) {
            const tsface_t* H = &M[m].F[f];
            const int nV = M[m].nV;
            if (H->i0 >= nV || H->i1 >= nV || H->i2 >= nV || H->i3 >= nV) continue;
            double q[4][3];
            tsc_face_corners(M[m].V, H, q);
            best = fmin(best, tsc_tri_d2(p, (const double (*)[3])q));
            if (H->quad) best = fmin(best, tsc_tri_d2(p, (const double (*)[3])&q[1]));
        }
    }
    return best;
}

/** @brief Writes a face's coverage sample points: its corners, the midpoints of its edges and its centre.
 *
 * A quad's perimeter in strip order runs i0, i1, i3, i2.
 * @param V The face's vertex bank; the face's indices must lie inside it.
 * @param F The face.
 * @param smp Receives up to 9 points.
 * @return The point count: 9 for a quad, 7 for a triangle. */
static int tsc_face_samples(const tsvert_t* V, const tsface_t* F, double smp[9][3]) {
    double q[4][3];
    tsc_face_corners(V, F, q);
    const int nc = F->quad ? 4 : 3;
    static const int ring4[4] = { 0, 1, 3, 2 }, ring3[3] = { 0, 1, 2 };
    const int* ring = F->quad ? ring4 : ring3;
    for (int k = 0; k < 3; k++) {
        smp[2 * nc][k] = 0.0;
        for (int c = 0; c < nc; c++) {
            smp[c][k] = q[ring[c]][k];
            smp[nc + c][k] = 0.5 * (q[ring[c]][k] + q[ring[(c + 1) % nc]][k]);
            smp[2 * nc][k] += q[ring[c]][k] / nc;
        }
    }
    return 2 * nc + 1;
}

/** @brief Returns the largest distance from an LP face's sample points to a set of HP meshes, stopping early past a bound.
 *
 * The samples are tsc_face_samples'. A sample within tol of the meshes
 * counts as covered and contributes a value below tol; the scan returns at
 * the first sample farther than tol.
 * @param lpV The LP vertex bank.
 * @param F The LP face; its indices must lie inside the LP vertex bank.
 * @param M The meshes.
 * @param nm The mesh count.
 * @param tol The coverage tolerance in decoded vertex units.
 * @return The largest sample distance found, above tol for an uncovered face. */
static double tsc_lp_face_gap(const tsvert_t* lpV, const tsface_t* F,
                              const tsmesh_t* M, int nm, double tol) {
    double smp[9][3];
    const int ns = tsc_face_samples(lpV, F, smp);
    double worst = 0.0, tol2 = tol * tol;
    for (int i = 0; i < ns && worst <= tol2; i++)
        worst = fmax(worst, tsc_hp_d2(M, nm, smp[i], tol2));
    return sqrt(worst);
}

/* HP (and LP) faces a scene sector borrows from the HP source level; see
 * the borrowed-faces section below. */
typedef struct tsimp {
    int nV, nF, nC;              /* borrowed HP verts, faces; colour words copied */
    tsvert_t* V;                 /* the source sector's HP verts in this sector's frame */
    tsface_t* F;                 /* the borrowed faces, indexing V and the colour banks */
    uint8_t* nearCol;            /* the source sector's bytes from its near colour bank on */
    uint8_t* farCol;             /* points into nearCol at the far colour bank */
    int src;                     /* the source sector index */
    int srcFlags;                /* the source sector's header flags (thdr_flags) */
    int nCand, nDup;             /* candidates; dropped as duplicates */
    int nLpV, nLpF;              /* the source sector's LP verts; the borrowed LP faces */
    tsvert_t* lpV;               /* the source sector's LP verts in this sector's frame */
    tsface_t* lpF;               /* the source LP faces this sector lacks, indexing lpV */
    uint32_t lpColBase;          /* the source LP colour array in the TSRC_TERR_BASE window */
    uint32_t lpColSpan;          /* bytes of it the borrowed LP faces index */
    int centreX, centreY, centreZ, radius;   /* the source sector's bounding sphere */
    int lpNearSz;                /* the source sector's LP near-outcode bound (tscache_t.lpNearSz) */
} tsimp_t;

static void tsimp_meshes(const tscache_t* C, tsmesh_t M[2], int* nm);

/** @brief Tags a sector's borrowed LP faces (C->imp) the way tsc_tag_uncovered tags its own.
 *
 * A borrowed LP face is uncov when a sample point lies farther than
 * TSC_LPCOV_TOL from the sector's own HP mesh (every face when it has
 * none), and uncovX when one lies that far from the own and borrowed HP
 * meshes; nUncovX counts the uncovX faces, so the uncov-only ride-along
 * reaches a sector whose only uncovered LP faces are borrowed.
 * @param C The cached sector.
 * @param M The HP meshes tsimp_meshes listed.
 * @param nm Their count.
 * @param ownHP Nonzero when the sector has HP faces of its own. */
static void tsc_tag_uncovered_borrowed(tscache_t* C, const tsmesh_t* M, int nm, int ownHP) {
    tsimp_t* I = C->imp;
    for (int f = 0; I && I->lpF && f < I->nLpF; f++) {
        tsface_t* F = &I->lpF[f];
        F->uncov = (uint8_t)(!ownHP || tsc_lp_face_gap(I->lpV, F, M, 1, TSC_LPCOV_TOL) > TSC_LPCOV_TOL);
        F->uncovX = (nm > 1 && F->uncov)
                  ? (uint8_t)(tsc_lp_face_gap(I->lpV, F, M, nm, TSC_LPCOV_TOL) > TSC_LPCOV_TOL)
                  : F->uncov;
        C->nUncovX += F->uncovX;
    }
}

/** @brief Tags the LP faces a distance-binning game's sector draws apart from its HP mesh.
 *
 * Runs only where the draw bins the two lists by distance
 * (game_layout.terrain.hp_far): there a sector's LP mesh can carry
 * geometry its HP mesh lacks (a strip of ground HP under LP spires), which
 * the guest draws whenever the sector bins LP. An LP face is uncov when a
 * sample point (tsc_lp_face_gap) lies farther than TSC_LPCOV_TOL from every
 * HP triangle of the same sector, and uncovX when one lies that far from
 * the HP triangles and the sector's borrowed HP faces (C->imp); without
 * borrowed faces uncovX equals uncov. Every LP face of a sector with no HP
 * faces is uncov. A face with an out-of-bank index stays untagged. nUncov
 * and nUncovX count the tagged faces. A poly-anim sector is re-tagged whenever
 * tsc_vert_anim_refresh decodes moved vertices.
 * @param C The cached sector; its LP faces, nUncov and nUncovX are updated. */
static void tsc_tag_uncovered(tscache_t* C) {
    C->nUncov = C->nUncovX = 0;
    if (!C->lpF || !C->lpV) return;
    for (int f = 0; f < C->nLpF; f++) C->lpF[f].uncov = C->lpF[f].uncovX = 0;
    if (!game_layout.terrain.hp_far) return;
    const int ownHP = C->hpF && C->hpV && C->nHpF > 0;
    tsmesh_t M[2];
    int nm = 0;
    tsimp_meshes(C, M, &nm);
    for (int f = 0; f < C->nLpF; f++) {
        tsface_t* F = &C->lpF[f];
        if (F->i0 >= C->nLpV || F->i1 >= C->nLpV || F->i2 >= C->nLpV || F->i3 >= C->nLpV)
            continue;
        F->uncov = (uint8_t)(!ownHP || tsc_lp_face_gap(C->lpV, F, M, 1, TSC_LPCOV_TOL) > TSC_LPCOV_TOL);
        F->uncovX = (nm > 1 && F->uncov)
                  ? (uint8_t)(tsc_lp_face_gap(C->lpV, F, M, nm, TSC_LPCOV_TOL) > TSC_LPCOV_TOL)
                  : F->uncov;
        C->nUncov += F->uncov;
        C->nUncovX += F->uncovX;
    }
    tsc_tag_uncovered_borrowed(C, M, nm, ownHP);
}

/* A sector named by an env poly-anim descriptor animates its packed verts in
 * RAM - but the .s Phase-1 walk only re-packs the bank of the TIER it draws,
 * so the LP bank of a sector the .s always bins HP (near) can hold an
 * arbitrarily old pose (the Sunny-Flight wall's LP stays RAISED in RAM after
 * the puzzle lowers the HP). Static LP-vs-HP coverage classification is
 * meaningless for such sectors (it's a function of animation time - a stale
 * raised LP wall reads as "uncovered by HP" and gets admitted as a gap
 * filler), and native-only LP passes must never draw the stale bank. LP the
 * .s itself draws is safe: the walk re-packs LP verts before drawing them. */
/* The env-anim registry iterator: one walk over every animated sector the
 * game's own animation walkers would touch this tick, whatever form the game
 * stores its registry in (game_layout.env_anim.form).
 *
 * ENV_ANIM_LISTS (Spyro 1): the LP and HP geometry and colour {count,
 * descriptor-pointer array} pairs, each descriptor naming its sector in the
 * u16 at +4.
 * ENV_ANIM_ARENA (Spyro 3): the sub-lists of the block at env+0x1C, each a
 * count word followed by one-word entries whose low byte is the sector index.
 * An arena entry is live only while bit 0 of the liveness byte for that index
 * is set, which is the same test the game's walkers make before rewriting the
 * sector; skipping a dormant entry keeps the cached decode valid instead of
 * re-decoding unchanged RAM every tick.
 *
 * The scrolling-texture list stays out: its consumer needs the per-descriptor
 * accumulator record, which exists only in the LISTS form (tsc_scroll_refresh).
 *
 * A count ceiling of 1024 rejects a header read while the level data is being
 * swapped, when the pointer can be anything. */
#define ENV_ANIM_COUNT_MAX 1024u
typedef struct {
    CPUState* cpu;
    uint32_t  sectorCnt;
    uint32_t  blk;          /* ARENA: the block base, 0 when unresolved */
    int       list;         /* next list / sub-list to open */
    uint32_t  base;         /* first entry of the open list */
    uint32_t  cnt, k;
    int       kind;         /* ENV_ANIM_KIND_* of the open list */
    int       skip_top;     /* ARENA: this list skips bit-31 entries */
    uint32_t  seen;         /* entries examined, live or not (engagement) */
} env_anim_iter_t;

/** @brief Resets an environment-animation iterator to its first list.
 *
 * Zeroes the iterator and stores the CPU and the sector count. For the
 * ENV_ANIM_ARENA form with a located block, reads the block base from the
 * guest word at env_anim.block_ptr into blk.
 * @param it The iterator.
 * @param cpu The guest CPU state.
 * @param sectorCnt The level's sector count.
 */
static void env_anim_iter_init(env_anim_iter_t* it, CPUState* cpu, uint32_t sectorCnt) {
    memset(it, 0, sizeof(*it));
    it->cpu = cpu;
    it->sectorCnt = sectorCnt;
    if (game_layout.env_anim.form == ENV_ANIM_ARENA && game_layout.env_anim.block_ptr)
        it->blk = rd(cpu, game_layout.env_anim.block_ptr);
}

/** @brief ENV_ANIM_LISTS: opens the next located, non-empty {count, pointer} pair of the four the game walks.
 *
 * The four are LP and HP geometry, then LP and HP colour; the first two open
 * as ENV_ANIM_KIND_VERT, the last two as ENV_ANIM_KIND_COLOUR. A pair with
 * no layout offset, a null pointer, a zero count or a count above
 * ENV_ANIM_COUNT_MAX is skipped.
 * @param it The iterator.
 * @return 1 when a list is open, 0 when all four are spent. */
static int env_anim_lists_open(env_anim_iter_t* it) {
    const uint32_t off[4] = { game_layout.env_anim.lp_poly, game_layout.env_anim.hp_poly,
                              game_layout.env_anim.lp_col,  game_layout.env_anim.hp_col };
    while (it->list < 4) {
        int l = it->list++;
        if (!off[l]) continue;                       /* list not located */
        uint32_t cnt  = rd(it->cpu, off[l]);
        uint32_t base = rd(it->cpu, off[l] + 4);
        if (!base || cnt == 0 || cnt > ENV_ANIM_COUNT_MAX) continue;
        it->cnt = cnt; it->base = base; it->k = 0;
        it->kind = l < 2 ? ENV_ANIM_KIND_VERT : ENV_ANIM_KIND_COLOUR;
        it->skip_top = 0;
        return 1;
    }
    return 0;
}

/** @brief Opens the iterator's next non-empty list in the game's registry form.
 *
 * ENV_ANIM_LISTS delegates to env_anim_lists_open. ENV_ANIM_ARENA opens the
 * next used sub-list row of the block (indirect rows read their list offset
 * from the header word) whose count is 1..ENV_ANIM_COUNT_MAX; an unresolved
 * block (outside KSEG0) opens nothing.
 * @param it The iterator.
 * @return 1 when a list is open, 0 when the registry is exhausted. */
static int env_anim_iter_open(env_anim_iter_t* it) {
    CPUState* cpu = it->cpu;
    if (game_layout.env_anim.form == ENV_ANIM_LISTS) return env_anim_lists_open(it);
    if (game_layout.env_anim.form != ENV_ANIM_ARENA || it->blk < 0x80000000u) return 0;
    while (it->list < ENV_ANIM_SUBLISTS) {
        const env_anim_sublist_t* S = &game_layout.env_anim.sub[it->list];
        it->list++;
        if (!S->entry_off) continue;                 /* unused row */
        uint32_t hdr = it->blk + S->hdr_off;
        uint32_t lst = S->indirect ? it->blk + rd(cpu, hdr) : hdr;
        uint32_t cnt = rd(cpu, lst);
        if (cnt == 0 || cnt > ENV_ANIM_COUNT_MAX) continue;
        it->cnt = cnt; it->base = lst + S->entry_off; it->k = 0;
        it->kind = S->kind;
        it->skip_top = S->skip_top;
        return 1;
    }
    return 0;
}

/** @brief Yields the next animated sector index and what the animation rewrites.
 *
 * ENV_ANIM_LISTS entries are descriptor pointers, the sector in the u16 at
 * +4, and a null pointer is skipped. ENV_ANIM_ARENA entries carry the sector
 * in the low byte; a list that skips bit-31 entries skips them, and an
 * entry whose liveness byte has bit 0 clear is skipped. An index at or past
 * the sector count is skipped. Every examined entry counts into `seen`.
 * @param it The iterator.
 * @param sector Receives the sector index.
 * @param kind Receives the list's ENV_ANIM_KIND_*.
 * @return 1 when an entry is yielded, 0 when the registry is exhausted. */
static int env_anim_iter_next(env_anim_iter_t* it, uint32_t* sector, int* kind) {
    CPUState* cpu = it->cpu;
    for (;;) {
        if (it->k >= it->cnt) {
            if (!env_anim_iter_open(it)) return 0;
            continue;
        }
        uint32_t w = rd(cpu, it->base + it->k * 4);
        it->k++;
        it->seen++;
        uint32_t idx;
        if (game_layout.env_anim.form == ENV_ANIM_LISTS) {
            if (!w) continue;                        /* empty descriptor slot */
            idx = cpu->read_half(w + 4);
        } else {
            if (it->skip_top && (int32_t)w < 0) continue;
            idx = w & 0xFFu;
            if (game_layout.env_anim.live_table
                && !(cpu->read_byte(game_layout.env_anim.live_table + idx) & 1u))
                continue;                            /* animation not running */
        }
        if (idx >= it->sectorCnt) continue;
        *sector = idx;
        *kind = it->kind;
        return 1;
    }
}

/** @brief Tests whether an env poly-anim descriptor names the sector at address s.
 *
 * Walks the animation registry and compares the sector-table entry of each
 * vertex-kind entry with s; colour entries are read live and skipped.
 * @param cpu The guest CPU state.
 * @param s The sector's guest address.
 * @param sectorPtr The sector pointer table's guest address.
 * @param sectorCnt The sector count.
 * @return 1 when a vertex-kind entry names the sector, else 0. */
static int tsc_sector_polyanim(CPUState* cpu, uint32_t s,
                               uint32_t sectorPtr, uint32_t sectorCnt) {
    env_anim_iter_t it;
    uint32_t idx; int kind;
    env_anim_iter_init(&it, cpu, sectorCnt);
    while (env_anim_iter_next(&it, &idx, &kind)) {
        if (kind != ENV_ANIM_KIND_VERT) continue;    /* colours are read live */
        if (rd(cpu, sectorPtr + idx * 4) == s) return 1;
    }
    return 0;
}

/* Pending bytes of the deferred keyframe vertex banks (env_anim.kf_lp_poly,
 * kf_hp_poly): the world draw packs a bank only while bit 7 of its byte is
 * clear and the sector bins in that tier, then stores 0xFF there (Spyro 2
 * environment.s:1737-1743 LP, 1932-1935 HP). */
#define TSC_KF_PEND_LP 0x18u
#define TSC_KF_PEND_HP 0x1Au

/** @brief Returns the descriptor whose posted pose a sector's tier bank has not packed yet, or 0.
 *
 * Bit 7 of the tier's pending byte set means the bank holds the latest
 * posted pose. A clear byte is the descriptor's index in the tier's deferred
 * keyframe registry; a game without that registry, an index at or past the
 * registry count and a null entry return 0.
 * @param cpu The guest CPU state, read only.
 * @param s The sector's guest address.
 * @param hp 1 for the HP bank, 0 for the LP bank.
 * @return The descriptor's guest address, or 0. */
static uint32_t tsc_kf_pending(CPUState* cpu, uint32_t s, int hp) {
    uint32_t reg = hp ? game_layout.env_anim.kf_hp_poly : game_layout.env_anim.kf_lp_poly;
    if (!reg) return 0;
    uint32_t b = cpu->read_byte(s + (hp ? TSC_KF_PEND_HP : TSC_KF_PEND_LP));
    if (b & 0x80u) return 0;
    uint32_t cnt = rd(cpu, reg), arr = rd(cpu, reg + 4u);
    if (!arr || b >= cnt || cnt > ENV_ANIM_COUNT_MAX) return 0;
    return rd(cpu, arr + b * 4u);
}

/** @brief Returns a keyframe descriptor's posted keyframe word.
 *
 * The descriptor's current keyframe is the byte at +2; its 8-byte entry
 * starts at +0xC and the word at entry +4 holds the blend factor (byte 0),
 * keyframes A and B (bytes 1, 2) and the tier-hide byte (byte 3).
 * @param cpu The guest CPU state, read only.
 * @param desc The descriptor's guest address.
 * @return The keyframe word. */
static uint32_t tsc_kf_word(CPUState* cpu, uint32_t desc) {
    uint32_t kf = cpu->read_byte(desc + 2u);
    return rd(cpu, desc + 0xCu + kf * 8u + 4u);
}

/** @brief Overwrites a decoded tier bank with the pose its pending keyframe descriptor posts.
 *
 * Reproduces the world draw's pack (Spyro 2 environment.s:1744-1830 LP,
 * 1936-2042 HP) without storing to guest RAM. The keyframe block is at desc
 * plus the word at +8, one keyframe every stride bytes (u16 at +6); blend
 * factor 0 copies keyframe A's packed words, else each packed axis is GTE
 * INTPL's ((A << 12) + (B - A) * (f << 4)) >> 12. Word k lands at bank index
 * (byte at +5) + k; indices at or past nv are dropped.
 * @param cpu The guest CPU state, read only.
 * @param s The sector's guest address.
 * @param hp 1 for the HP bank, 0 for the LP bank.
 * @param nv The bank's vertex count.
 * @param flat The sector's flat flag (tvert_unpack).
 * @param out The decoded bank.
 * @return The posted tier-hide verdict (1 hidden, 0 shown), or -1 when the bank holds the posted pose. */
static int tsc_kf_overlay(CPUState* cpu, uint32_t s, int hp, int nv, int flat, tsvert_t* out) {
    uint32_t desc = tsc_kf_pending(cpu, s, hp);
    if (!desc || !out) return -1;
    uint32_t w = tsc_kf_word(cpu, desc);
    int32_t f16 = (int32_t)(w & 0xFFu) << 4;
    uint32_t stride = cpu->read_half(desc + 6u);
    uint32_t blk = desc + rd(cpu, desc + 8u);
    uint32_t kA = blk + ((w >> 8) & 0xFFu) * stride, kB = blk + ((w >> 16) & 0xFFu) * stride;
    int first = (int)cpu->read_byte(desc + 5u);
    for (uint32_t k = 0; k < stride / 4u && first + (int)k < nv; k++) {
        uint32_t v = rd(cpu, kA + k * 4u);
        if (f16) {
            uint32_t b = rd(cpu, kB + k * 4u);
            const int32_t a3[3] = { (int32_t)(v >> 21), (int32_t)((v >> 10) & 0x7FFu), (int32_t)(v & 0x3FFu) };
            const int32_t b3[3] = { (int32_t)(b >> 21), (int32_t)((b >> 10) & 0x7FFu), (int32_t)(b & 0x3FFu) };
            int32_t r[3];
            for (int j = 0; j < 3; j++) r[j] = ((a3[j] << 12) + (b3[j] - a3[j]) * f16) >> 12;
            v = ((uint32_t)r[0] << 21) + ((uint32_t)r[1] << 10) + (uint32_t)r[2];
        }
        int x, y, z; tvert_unpack(v, flat, &x, &y, &z);
        tsvert_t* o = &out[first + (int)k];
        o->x = (int16_t)x; o->y = (int16_t)y; o->z = (int16_t)z;
    }
    return (w >> 24) != 0;
}

/** @brief Brings a decoded sector's vertex banks and tier flags to the poses its deferred keyframe animations post.
 *
 * A bank the world draw has not packed since its descriptor advanced takes
 * the posted pose (tsc_kf_overlay), and the tier's no-draw flag follows the
 * posted hide byte, as the pack writes it (Spyro 2 environment.s:1756-1762,
 * 1948-1954). Either bank of a sector then shows the pose the animation is
 * at, whichever tier the guest bins. No-op on a game without the registry.
 * @param cpu The guest CPU state, read only.
 * @param C The cached sector. */
static void tsc_kf_apply(CPUState* cpu, tscache_t* C) {
    for (int hp = 0; hp < 2; hp++) {
        int hide = tsc_kf_overlay(cpu, C->s, hp, hp ? C->nHpV : C->nLpV, C->flat,
                                  hp ? C->hpV : C->lpV);
        if (hide < 0) continue;
        int bit = (int)(hp ? game_layout.terrain.no_hp_flag : game_layout.terrain.no_lp_flag);
        C->flags = hide ? (C->flags | bit) : (C->flags & ~bit);
    }
}

/** @brief Returns the FNV-1a hash of a sector's packed LP and HP vertex words.
 *
 * The decode cache's signature is the header (words +0x04..+0x14), which
 * carries the counts, the flags and the bounding sphere but not the
 * geometry: a gameplay event that rewrites a vertex block in place leaves
 * every signature word untouched, and this hash detects it. A bank with a
 * posted, unpacked keyframe (tsc_kf_pending) mixes in the descriptor and its
 * keyframe word, so an advance the draw has not packed re-decodes too. The caller
 * excludes sectors named by an env poly-anim descriptor, whose vertices are
 * rewritten every tick and which the registry path re-decodes into prev and
 * cur banks that a full miss would drop.
 * @param cpu The guest CPU state.
 * @param s The sector's guest address.
 * @param g The sector's header words +0x04..+0x14.
 * @return The hash, never 0 (0 means not computed). */
static uint32_t tsc_vhash(CPUState* cpu, uint32_t s, const uint32_t g[5]) {
    uint32_t nLpV = g[3] & 0xFFu, nHpV = g[4] & 0xFFu;
    uint32_t hpVerts = s + 0x1Cu + ((g[4] >> 24) & 0xFFu) * 4u;
    uint32_t h = 2166136261u;
    for (uint32_t k = 0; k < nLpV; k++) h = (h ^ rd(cpu, s + 0x1Cu + k * 4u)) * 16777619u;
    for (uint32_t k = 0; k < nHpV; k++) h = (h ^ rd(cpu, hpVerts + k * 4u)) * 16777619u;
    for (int hp = 0; hp < 2; hp++) {              /* a posted, unpacked keyframe */
        uint32_t d = tsc_kf_pending(cpu, s, hp);
        if (d) h = (((h ^ d) * 16777619u) ^ tsc_kf_word(cpu, d)) * 16777619u;
    }
    return h ? h : 1u;
}

/* OPENPETE_TERRAIN_VHASH_CENSUS=1: per canonical build, how many sectors were
 * hashed and which of them moved. Engagement for the vertex-rewrite miss:
 * hashed > 0 proves the compare ran, and a moved sector names the build that
 * re-decodes it. */
static int      s_vh_on = -1;
static unsigned s_vh_frame;
static int      s_vh_hashed, s_vh_moved, s_vh_listn;
static uint16_t s_vh_list[32];
/** @brief Prints and clears the vertex-hash census line under OPENPETE_TERRAIN_VHASH_CENSUS.
 *
 * Prints nothing when the census is off or no sector was hashed. The line
 * carries the frame, the hashed and moved counts and up to 32 moved sector
 * indices. */
static void tsc_vhash_census_flush(void) {
    if (s_vh_on <= 0 || !s_vh_hashed) return;
    fprintf(stderr, "[vhash] frame=%u hashed=%d moved=%d",
            s_vh_frame, s_vh_hashed, s_vh_moved);
    for (int k = 0; k < s_vh_listn; k++) fprintf(stderr, " %u", s_vh_list[k]);
    fprintf(stderr, "\n");
    s_vh_hashed = s_vh_moved = s_vh_listn = 0;
}

/** @brief Decodes a sector record's header fields, vertices and faces into a cache entry.
 *
 * The header is w0 (bounding centre X and Y) and g, the words at +0x04 to
 * +0x14: centre Z, radius and flags; origin X and Y; origin Z; the LP
 * vertex, colour and face counts; the HP vertex, colour and face counts and
 * the HP vertex block's word offset. The LP vertices start at +0x1C, then
 * the LP colours and faces; the HP block holds its vertices, the near and
 * far colours, then the faces. The entry's arrays are allocated here; an
 * allocation failure leaves that array NULL.
 * @param src The record's word source.
 * @param s The record's address in @p src.
 * @param w0 The header's word 0.
 * @param g The header's words 1-5.
 * @param C The entry, zeroed by the caller.
 * @param lqTable The LQ texture table's guest address.
 * @param hqTable The HQ texture table's guest address.
 * @param texTblMode 2 to take HP textures from the HQ table.
 * @param hiDetail Nonzero to decode the near texture half. */
static void tsc_decode_body(const twsrc_t* src, uint32_t s, uint32_t w0, const uint32_t g[5],
                            tscache_t* C, uint32_t lqTable, uint32_t hqTable,
                            int texTblMode, int hiDetail) {
    C->flags = thdr_flags(g[0]);
    C->flat  = thdr_flat(g[0]);
    /* Bounding sphere (header w0/w1) - the .s LP/HP binning inputs. w0 is
     * not in the signature (it never changes independently of w1..w4). */
    C->centreX = (int)(w0 >> 16); C->centreY = (int)(w0 & 0xFFFF);
    C->centreZ = (int)(g[0] >> 16); C->radius = thdr_radius(g[0]);
    C->originX = (int)(g[1] >> 16); C->originY = (int)(g[1] & 0xFFFF);
    C->originZ = (int)(g[2] >> 16);
    C->nLpV = g[3] & 0xFF; C->nLpF = (g[3] >> 16) & 0xFF;
    int nLpC = (g[3] >> 8) & 0xFF;
    C->nHpV = g[4] & 0xFF; C->nHpF = (g[4] >> 16) & 0xFF;
    int nHpC = (g[4] >> 8) & 0xFF, hpVertsOff = (int)((g[4] >> 24) & 0xFF);
    C->nLpC = nLpC; C->nHpC = nHpC;
    C->lpNearSz = tsc_lp_near_sz(g[3]);
    uint32_t lpVerts = s + 0x1C;
    C->lpFaces   = s + 0x1C + (uint32_t)((g[3] & 0xFF) + nLpC) * 4;
    C->lpColBase = s + 0x1C + (uint32_t)(g[3] & 0xFF) * 4;
    uint32_t hpVerts = s + 0x1C + (uint32_t)hpVertsOff * 4;
    C->hpFaces   = hpVerts + (uint32_t)((g[4] & 0xFF) + nHpC * 2) * 4;
    C->nearBase  = hpVerts + (uint32_t)(g[4] & 0xFF) * 4;
    C->farBase   = C->nearBase + (uint32_t)nHpC * 4;
    if (C->nLpV > 0) { C->lpV = (tsvert_t*)malloc(sizeof(tsvert_t) * (size_t)C->nLpV);
                       if (C->lpV) tsc_decode_verts(src, lpVerts, C->nLpV, C->flat, C->lpV); }
    if (C->nLpF > 0) { C->lpF = (tsface_t*)malloc(sizeof(tsface_t) * (size_t)C->nLpF);
                       if (C->lpF) C->lpColSpan = tsc_decode_faces(src, C->lpFaces, C->nLpF, 8,
                                                    lqTable, hqTable, texTblMode, hiDetail, C->lpF); }
    if (C->nHpV > 0) { C->hpV = (tsvert_t*)malloc(sizeof(tsvert_t) * (size_t)C->nHpV);
                       if (C->hpV) tsc_decode_verts(src, hpVerts, C->nHpV, C->flat, C->hpV); }
    if (C->nHpF > 0) { C->hpF = (tsface_t*)malloc(sizeof(tsface_t) * (size_t)C->nHpF);
                       if (C->hpF) C->hpColSpan = tsc_decode_faces(src, C->hpFaces, C->nHpF, 16,
                                                    lqTable, hqTable, texTblMode, hiDetail, C->hpF); }
}

/* ---- HP faces borrowed from a source level ---------------------------------
 * A scene can be built from another level's terrain with its meshes cut
 * back: Spyro 2's title scene carries Skelos Badlands' sectors with ground
 * strips of HP under the full LP mesh, and Spyro 3's carries Fireworks
 * Factory's sectors with both meshes cut down. The layout names that source
 * level (terrain.hp_source_wad_entry); its terrain and texture sub-files are
 * read host-side once per session. A scene sector whose LP faces all occur,
 * in world space, in one source sector borrows the source HP faces lying
 * within TSC_LPCOV_TOL of the scene LP faces its own HP leaves uncovered or
 * of the source LP faces it lacks, and those LP faces themselves, and takes
 * the source sector's header for its tier verdicts (sector_header), so in
 * either LOD mode the scene draws what the source level draws from the same
 * view. A borrowed face keeps its source vertices, colours, face words and Tiledef
 * entries: its tiles decode from the source table through the host window
 * at TSRC_TEX_BASE and sample the source level's VRAM image, loaded into the
 * renderer's auxiliary VRAM bank, with the stock texel, palette and
 * fog-ladder math. Nothing here reads or writes gameplay state or guest
 * VRAM: the source bytes come from the disc through a host read. */

/* A candidate whose every sample point lies this close to the sector's own
 * HP mesh duplicates a surface the sector already draws. */
#define TSIMP_DUP_TOL 8.0
#define HPS_KEYS 16384u          /* LP face index slots (power of two) */
#define HPS_TEX_IMAGE_W 512      /* the texture sub-file's first 512 KiB is VRAM
                                  * x 512-1023, rows 0-511 */


static struct {
    int state;                   /* 0 unread, 1 ready, -1 unavailable */
    uint8_t* terr; uint32_t terrLen;   /* terrain sub-file (1) */
    uint8_t* tex;  uint32_t texLen;    /* texture sub-file (0) */
    uint32_t texCnt;             /* texture entries at byte 8, 48 bytes each */
    int nSec;
    tscache_t* sec;              /* the decoded source sectors */
    uint64_t key[HPS_KEYS];      /* LP face key -> sector index; -2 when two sectors hold it */
    int32_t  val[HPS_KEYS];
    int32_t* votes;              /* nSec scratch tallies (serial prepass only) */
} s_hps;

/** @brief Packs a world-space corner into one ordered 63-bit value.
 * @param x The world X.
 * @param y The world Y.
 * @param z The world Z.
 * @return The packed corner. */
static uint64_t hps_corner(int x, int y, int z) {
    return ((uint64_t)(uint32_t)(x & 0x1FFFFF) << 42) | ((uint64_t)(uint32_t)(y & 0x1FFFFF) << 21)
         | (uint64_t)(uint32_t)(z & 0x1FFFFF);
}

/** @brief Returns a face's world-space identity: the hash of its distinct corners, order-free.
 *
 * World coordinates are the sector origin plus the decoded vertex. A quad
 * has four corners and a triangle (i2 == i3) three; the packed corners are
 * sorted before hashing, so the key ignores vertex order and the sector the
 * face is stored in. Never 0.
 * @param V The face's vertex bank; the indices must lie inside it.
 * @param F The face.
 * @param ox The sector origin X.
 * @param oy The sector origin Y.
 * @param oz The sector origin Z.
 * @return The key. */
static uint64_t hps_face_key(const tsvert_t* V, const tsface_t* F, int ox, int oy, int oz) {
    const int ci[4] = { F->i0, F->i1, F->i2, F->i3 };
    const int nc = F->quad ? 4 : 3;
    uint64_t c[4];
    for (int k = 0; k < nc; k++)
        c[k] = hps_corner(ox + V[ci[k]].x, oy + V[ci[k]].y, oz + V[ci[k]].z);
    for (int a = 1; a < nc; a++)
        for (int b = a; b > 0 && c[b - 1] > c[b]; b--) { uint64_t t = c[b]; c[b] = c[b - 1]; c[b - 1] = t; }
    uint64_t h = openpete_fnv1a64(c, sizeof(uint64_t) * (size_t)nc);
    return h ? h : 1u;
}

/** @brief Tests that every index of a face lies inside its vertex bank.
 * @param F The face.
 * @param nV The bank's vertex count.
 * @return 1 when every index is inside. */
static int hps_face_ok(const tsface_t* F, int nV) {
    return F->i0 < nV && F->i1 < nV && F->i2 < nV && F->i3 < nV;
}

/** @brief Inserts a source LP face key into the index; a key two sectors hold maps to -2.
 * @param key The face key.
 * @param sec The source sector index. */
static void hps_index_put(uint64_t key, int sec) {
    for (uint32_t i = (uint32_t)key & (HPS_KEYS - 1u), n = 0; n < HPS_KEYS; n++, i = (i + 1u) & (HPS_KEYS - 1u)) {
        if (s_hps.key[i] == 0) { s_hps.key[i] = key; s_hps.val[i] = sec; return; }
        if (s_hps.key[i] == key) { if (s_hps.val[i] != sec) s_hps.val[i] = -2; return; }
    }
}

/** @brief Looks a face key up in the source LP index.
 * @param key The face key.
 * @return The source sector holding it, -2 when two sectors hold it, -1 when none does. */
static int hps_index_get(uint64_t key) {
    for (uint32_t i = (uint32_t)key & (HPS_KEYS - 1u), n = 0; n < HPS_KEYS; n++, i = (i + 1u) & (HPS_KEYS - 1u)) {
        if (s_hps.key[i] == 0) return -1;
        if (s_hps.key[i] == key) return s_hps.val[i];
    }
    return -1;
}

/** @brief Reads a little-endian word of the source terrain sub-file, 0 past its end.
 * @param o The byte offset.
 * @return The word. */
static uint32_t hps_w(uint32_t o) {
    const twsrc_t src = { NULL, s_hps.terr, s_hps.terrLen };
    return tw_rd(&src, o);
}

/** @brief Decodes the source terrain sub-file's sectors and indexes their LP faces.
 *
 * The sub-file opens with the texture block, {block size, entry count,
 * 48-byte entries}; at the block size follows the sector block, {its size,
 * 4, sector count, one offset per sector}, each offset relative to the
 * sector block's second word. Every sector record is decoded with
 * tsc_decode_body from the host bytes.
 * @param cpu The guest CPU state (the word source's guest side; unused for host reads).
 * @return 1 when the sectors decoded, else 0. */
static int hps_parse(CPUState* cpu) {
    const uint32_t ts = hps_w(0), cnt = hps_w(4);
    if (cnt == 0 || cnt > 128u || ts != 8u + 48u * cnt || ts + 12u > s_hps.terrLen) return 0;
    const uint32_t nSec = hps_w(ts + 8u);
    if (nSec == 0 || nSec > 1024u || ts + 12u + 4u * nSec > s_hps.terrLen) return 0;
    s_hps.texCnt = cnt;
    s_hps.sec = (tscache_t*)calloc(nSec, sizeof(tscache_t));
    s_hps.votes = (int32_t*)calloc(nSec, sizeof(int32_t));
    if (!s_hps.sec || !s_hps.votes) return 0;
    s_hps.nSec = (int)nSec;
    const twsrc_t src = { cpu, s_hps.terr, s_hps.terrLen };
    for (uint32_t k = 0; k < nSec; k++) {
        const uint32_t o = ts + 4u + hps_w(ts + 12u + 4u * k);
        if (o + 0x1Cu > s_hps.terrLen) return 0;
        const uint32_t g[5] = { hps_w(o + 4u), hps_w(o + 8u), hps_w(o + 12u), hps_w(o + 16u), hps_w(o + 20u) };
        tscache_t* K = &s_hps.sec[k];
        tsc_decode_body(&src, o, hps_w(o), g, K, 0u, 0u, 0, 0);
        K->s = o;
        for (int f = 0; K->lpF && K->lpV && f < K->nLpF; f++)
            if (hps_face_ok(&K->lpF[f], K->nLpV))
                hps_index_put(hps_face_key(K->lpV, &K->lpF[f], K->originX, K->originY, K->originZ), (int)k);
    }
    return 1;
}

/** @brief Reads and decodes the layout's HP source level once per session.
 *
 * Reads sub-file 1 (terrain) and sub-file 0 (texture image) of the WAD
 * entry terrain.hp_source_wad_entry under the terrain disc tag, then
 * hps_parse. A game whose layout names no source, and any read or parse
 * failure, leaves the source unavailable for the session.
 * @param cpu The guest CPU state.
 * @return 1 when the source is ready. */
static int hps_ready(CPUState* cpu) {
    if (s_hps.state) return s_hps.state > 0;
    s_hps.state = -1;
    const int e = game_layout.terrain.hp_source_wad_entry;
    if (e <= 0) return 0;
    s_hps.terr = wad_subfile_load(e, 1, OPENPETE_DISC_TAG_TERRAIN, &s_hps.terrLen, "terrain");
    s_hps.tex  = wad_subfile_load(e, 0, OPENPETE_DISC_TAG_TERRAIN, &s_hps.texLen, "terrain");
    if (!s_hps.terr || !s_hps.tex || s_hps.texLen < 2u * HPS_TEX_IMAGE_W * OPENPETE_VRAM_H) return 0;
    if (!hps_parse(cpu)) {
        OPL_WARN_F("terrain", "HP source WAD entry %d: terrain sub-file not decodable", e);
        return 0;
    }
    s_hps.state = 1;
    s_tsrc_terr = s_hps.terr;
    s_tsrc_terr_len = s_hps.terrLen;
    s_tsrc_tex = s_hps.terr + 8u;                       /* the 48-byte entries */
    s_tsrc_tex_len = 48u * s_hps.texCnt;
    for (uint32_t rec = 0; rec < 128u && game_layout.terrain.hq_cells4; rec++) {
        const unsigned v = rec < s_hps.texCnt
            ? thq_record_verdicts(cpu, TSRC_TEX_BASE + rec * game_layout.terrain.hq_stride, 0xFF) : 0u;
        s_hq_differ_src[rec] = (uint8_t)(v & 1u);
        s_hq_hires_src[rec] = (uint8_t)(v >> 1);
    }
    openpete_native_vram_aux_set(s_hps.tex, 2u * HPS_TEX_IMAGE_W * OPENPETE_VRAM_H);
    OPL_INFO_F("terrain", "HP source WAD entry %d: %d sectors, %u textures", e, s_hps.nSec, s_hps.texCnt);
    return 1;
}

static const char* s_hps_why = "";   /* the census's reason for the last match miss */
/** @brief Finds the source sector holding every LP face of a scene sector, in world space.
 *
 * Each scene LP face's key (hps_face_key) votes for the source sector the
 * index maps it to; the sector with the most votes is the match when it
 * holds every scene LP face, which a scan of its own face keys confirms
 * (that scan also settles keys two source sectors share).
 * @param C The scene sector.
 * @return The source sector index, or -1 when none holds every face. */
static int hps_match(const tscache_t* C) {
    if (!C->lpF || !C->lpV || C->nLpF <= 0) return -1;
    memset(s_hps.votes, 0, sizeof(int32_t) * (size_t)s_hps.nSec);
    uint64_t keys[256];
    for (int f = 0; f < C->nLpF; f++) {
        if (!hps_face_ok(&C->lpF[f], C->nLpV)) { s_hps_why = "oob"; return -1; }
        keys[f] = hps_face_key(C->lpV, &C->lpF[f], C->originX, C->originY, C->originZ);
        const int k = hps_index_get(keys[f]);
        if (k == -1) { s_hps_why = "nokey"; return -1; }
        if (k >= 0) s_hps.votes[k]++;
    }
    int best = -1;
    for (int k = 0; k < s_hps.nSec; k++)
        if (s_hps.votes[k] > 0 && (best < 0 || s_hps.votes[k] > s_hps.votes[best])) best = k;
    if (best < 0) return -1;
    const tscache_t* K = &s_hps.sec[best];
    for (int f = 0; f < C->nLpF; f++) {
        int hit = 0;
        for (int g = 0; g < K->nLpF && !hit; g++)
            hit = hps_face_ok(&K->lpF[g], K->nLpV)
               && hps_face_key(K->lpV, &K->lpF[g], K->originX, K->originY, K->originZ) == keys[f];
        if (!hit) { s_hps_why = "verify"; return -1; }
    }
    return best;
}

/** @brief Frees a sector's borrowed faces.
 * @param I The borrowed faces, or NULL. */
static void tsimp_free(tsimp_t* I) {
    if (!I) return;
    free(I->V); free(I->F); free(I->nearCol); free(I->lpV); free(I->lpF); free(I);
}

/** @brief Lists the HP meshes a sector's coverage tests measure against: its own, then its borrowed faces.
 * @param C The sector.
 * @param M Receives the meshes.
 * @param nm Receives the mesh count, 1 or 2. */
static void tsimp_meshes(const tscache_t* C, tsmesh_t M[2], int* nm) {
    M[0] = (tsmesh_t){ C->hpV, C->hpF, C->nHpV, C->nHpF };
    *nm = 1;
    if (C->imp) { M[1] = (tsmesh_t){ C->imp->V, C->imp->F, C->imp->nV, C->imp->nF }; *nm = 2; }
}

/** @brief Tests whether a candidate HP face lies within TSC_LPCOV_TOL of a sample point of a sector's uncov LP faces.
 * @param C The sector; its LP faces carry the uncov tag.
 * @param V The candidate's vertex bank, in the sector's frame.
 * @param nV The bank's vertex count.
 * @param H The candidate face.
 * @return 1 when some uncov LP sample lies within the tolerance. */
static int tsimp_near_uncov(const tscache_t* C, const tsvert_t* V, int nV, const tsface_t* H) {
    const tsmesh_t m = { V, H, nV, 1 };
    const double tol2 = TSC_LPCOV_TOL * TSC_LPCOV_TOL;
    for (int f = 0; f < C->nLpF; f++) {
        const tsface_t* F = &C->lpF[f];
        if (!F->uncov) continue;
        double smp[9][3];
        const int ns = tsc_face_samples(C->lpV, F, smp);
        for (int i = 0; i < ns; i++)
            if (tsc_hp_d2(&m, 1, smp[i], tol2) <= tol2) return 1;
    }
    return 0;
}

/** @brief Marks the source sector's LP faces whose world key the scene sector does not hold.
 * @param C The scene sector.
 * @param K The matched source sector.
 * @param miss Receives one flag per K LP face (at least K->nLpF entries).
 * @return The marked count. */
static int tsimp_missing_lp(const tscache_t* C, const tscache_t* K, uint8_t* miss) {
    int n = 0;
    for (int g = 0; g < K->nLpF; g++) {
        miss[g] = 0;
        if (!hps_face_ok(&K->lpF[g], K->nLpV)) continue;
        const uint64_t key = hps_face_key(K->lpV, &K->lpF[g], K->originX, K->originY, K->originZ);
        int held = 0;
        for (int f = 0; f < C->nLpF && !held; f++)
            held = hps_face_ok(&C->lpF[f], C->nLpV)
                && hps_face_key(C->lpV, &C->lpF[f], C->originX, C->originY, C->originZ) == key;
        miss[g] = (uint8_t)!held;
        n += !held;
    }
    return n;
}

/** @brief Tests whether a candidate HP face lies within TSC_LPCOV_TOL of a sample point of a source LP face the scene sector lacks.
 * @param K The source sector.
 * @param miss The flags tsimp_missing_lp wrote.
 * @param d The source origin minus the scene sector origin, per axis.
 * @param V The candidate's vertex bank, in the scene sector's frame.
 * @param nV The bank's vertex count.
 * @param H The candidate face.
 * @return 1 when some such sample lies within the tolerance. */
static int tsimp_near_missing(const tscache_t* K, const uint8_t* miss, const int d[3],
                              const tsvert_t* V, int nV, const tsface_t* H) {
    const tsmesh_t m = { V, H, nV, 1 };
    const double tol2 = TSC_LPCOV_TOL * TSC_LPCOV_TOL;
    for (int g = 0; g < K->nLpF; g++) {
        if (!miss[g]) continue;
        double smp[9][3];
        const int ns = tsc_face_samples(K->lpV, &K->lpF[g], smp);
        for (int i = 0; i < ns; i++) {
            const double p[3] = { smp[i][0] + d[0], smp[i][1] + d[1], smp[i][2] + d[2] };
            if (tsc_hp_d2(&m, 1, p, tol2) <= tol2) return 1;
        }
    }
    return 0;
}

/** @brief Tests whether a source HP face is a borrowing candidate for a scene sector.
 *
 * A scene sector with no HP of its own takes the source sector's whole HP
 * mesh; one with HP takes the faces over its uncov LP (tsimp_near_uncov)
 * and over the source LP faces it lacks (tsimp_near_missing).
 * @param C The scene sector.
 * @param K The matched source sector.
 * @param miss The flags tsimp_missing_lp wrote.
 * @param nMiss Their count.
 * @param d The source origin minus the scene sector origin, per axis.
 * @param V The candidate's vertex bank, in the scene sector's frame.
 * @param nV The bank's vertex count.
 * @param H The candidate face.
 * @return 1 for a candidate. */
static int tsimp_candidate(const tscache_t* C, const tscache_t* K, const uint8_t* miss, int nMiss,
                           const int d[3], const tsvert_t* V, int nV, const tsface_t* H) {
    if (C->nHpF <= 0 || tsimp_near_uncov(C, V, nV, H)) return 1;
    return nMiss && tsimp_near_missing(K, miss, d, V, nV, H);
}

/* World-space keys (hps_face_key) of every HP face of the scene's
 * sectors, rebuilt when the sector table's address list changes: a source
 * face may duplicate a face another scene sector already draws. */
static struct {
    uint64_t sig;                /* hash of the sector table's address list */
    uint64_t* keys;              /* open addressing, 0 = empty */
    uint32_t mask;               /* slot count - 1 */
} s_scene_hp;

/** @brief Inserts a key into the scene HP key set.
 * @param key The face key; never 0. */
static void scene_hp_put(uint64_t key) {
    for (uint32_t i = (uint32_t)key & s_scene_hp.mask;; i = (i + 1u) & s_scene_hp.mask) {
        if (s_scene_hp.keys[i] == key) return;
        if (!s_scene_hp.keys[i]) { s_scene_hp.keys[i] = key; return; }
    }
}

/** @brief Tests whether the scene HP key set holds a key.
 * @param key The face key.
 * @return 1 when present. */
static int scene_hp_has(uint64_t key) {
    if (!s_scene_hp.keys) return 0;
    for (uint32_t i = (uint32_t)key & s_scene_hp.mask;; i = (i + 1u) & s_scene_hp.mask) {
        if (s_scene_hp.keys[i] == key) return 1;
        if (!s_scene_hp.keys[i]) return 0;
    }
}

/** @brief Rebuilds the scene HP key set when the scene's sector table changed.
 *
 * Each sector's geometry decodes through tsc_decode_body from guest RAM
 * (reads only; no texture tables), its HP faces' world keys go into the
 * set, and the decode is freed. The set holds at least twice as many
 * slots as faces.
 * @param cpu The guest CPU state.
 * @param sectorPtr The scene sector table's guest address.
 * @param sectorCnt The scene sector count. */
static void scene_hp_refresh(CPUState* cpu, uint32_t sectorPtr, uint32_t sectorCnt) {
    if (sectorCnt == 0u || sectorCnt > 1024u) return;
    uint32_t addr[1024];
    for (uint32_t q = 0; q < sectorCnt; q++) addr[q] = rd(cpu, sectorPtr + q * 4u);
    const uint64_t sig = openpete_fnv1a64(addr, sizeof(uint32_t) * sectorCnt) ^ sectorPtr;
    if (s_scene_hp.keys && sig == s_scene_hp.sig) return;
    free(s_scene_hp.keys);
    s_scene_hp.keys = NULL;
    s_scene_hp.sig = sig;
    uint32_t slots = 1024u;
    while (slots < 2u * 256u * sectorCnt) slots <<= 1;
    s_scene_hp.keys = (uint64_t*)calloc(slots, sizeof(uint64_t));
    if (!s_scene_hp.keys) return;
    s_scene_hp.mask = slots - 1u;
    const twsrc_t src = { cpu, NULL, 0u };
    for (uint32_t q = 0; q < sectorCnt; q++) {
        const uint32_t sa = addr[q];
        if (!sa) continue;
        const uint32_t g[5] = { rd(cpu, sa + 0x04), rd(cpu, sa + 0x08), rd(cpu, sa + 0x0C),
                                rd(cpu, sa + 0x10), rd(cpu, sa + 0x14) };
        tscache_t T;
        memset(&T, 0, sizeof(T));
        tsc_decode_body(&src, sa, rd(cpu, sa), g, &T, 0u, 0u, 0, 0);
        for (int f = 0; T.hpV && T.hpF && f < T.nHpF; f++)
            if (hps_face_ok(&T.hpF[f], T.nHpV))
                scene_hp_put(hps_face_key(T.hpV, &T.hpF[f], T.originX, T.originY, T.originZ));
        free(T.lpV); free(T.lpF); free(T.hpV); free(T.hpF);
    }
}

/** @brief Tests whether a candidate HP face duplicates a surface the sector's own HP mesh draws.
 *
 * A duplicate either has the corners of an HP face of any scene sector in
 * world space (hps_face_key, scene_hp_has), or has every sample point
 * within TSIMP_DUP_TOL of the sector's own mesh. The key test catches a copied quad whose
 * corners are not coplanar, whose centre sample lies off the triangles the
 * distance test measures.
 * @param C The sector.
 * @param V The candidate's vertex bank, in the sector's frame.
 * @param H The candidate face.
 * @return 1 when the candidate duplicates a surface the sector draws. */
static int tsimp_dup(const tscache_t* C, const tsvert_t* V, const tsface_t* H) {
    if (scene_hp_has(hps_face_key(V, H, C->originX, C->originY, C->originZ))) return 1;
    const tsmesh_t m = { C->hpV, C->hpF, C->nHpV, C->nHpF };
    const double tol2 = TSIMP_DUP_TOL * TSIMP_DUP_TOL;
    double smp[9][3];
    const int ns = tsc_face_samples(V, H, smp);
    for (int i = 0; i < ns; i++)
        if (tsc_hp_d2(&m, 1, smp[i], tol2) > tol2) return 0;
    return 1;
}

/** @brief Tests whether a scene sector holds the source sector's whole HP face list.
 *
 * Equal face counts with equal vertex indices, colour offsets and word 3 on
 * every face: the scene is the source level itself (its vertex words may
 * differ where the level animates them), and nothing is borrowed.
 * @param C The scene sector.
 * @param K The source sector.
 * @return 1 when the face lists match. */
static int tsimp_same_hp(const tscache_t* C, const tscache_t* K) {
    if (C->nHpF != K->nHpF || !C->hpF) return 0;
    for (int f = 0; f < C->nHpF; f++) {
        const tsface_t* a = &C->hpF[f];
        const tsface_t* b = &K->hpF[f];
        if (a->i0 != b->i0 || a->i1 != b->i1 || a->i2 != b->i2 || a->i3 != b->i3 ||
            a->c0 != b->c0 || a->c1 != b->c1 || a->c2 != b->c2 || a->c3 != b->c3 ||
            a->w3 != b->w3) return 0;
    }
    return 1;
}

/** @brief Decodes a borrowed face's materials from the source level's table, sampling the auxiliary VRAM bank.
 *
 * Both tile halves decode through terrain_texmat from the host window at
 * TSRC_TEX_BASE (the source table) and take bank 1, so their page and CLUT
 * coordinates address the source level's texels and palettes, fog-ladder
 * rows included, in the auxiliary bank. The face takes no texture-animation
 * generation: the source level's animation registry is not loaded.
 * @param cpu The guest CPU state.
 * @param F The face, copied from the source sector.
 * @param hiDetail Nonzero to decode the near half as the face's material. */
static void tsimp_retexture(CPUState* cpu, tsface_t* F, int hiDetail) {
    terrain_texmat(cpu, TSRC_TEX_BASE, F->tb, hiDetail, &F->mat, F->tu, F->tv);
    terrain_texmat(cpu, TSRC_TEX_BASE, F->tb, 1, &F->matN, F->tuN, F->tvN);
    F->mat.bank = F->matN.bank = 1;
    F->tgen = F->tgenHq = 0;
}

/** @brief Copies the bytes a source sector's HP colour reads reach into a sector's borrowed faces.
 *
 * The walk reads an HP corner's near colour at the near bank plus the
 * face's colour offset and its far colour at the far bank plus the same
 * offset; an offset is at most 0x3FC and may pass the bank's nHpC words, so
 * the copy runs from the near bank to 0x400 bytes past the far bank's
 * start, clipped to the sub-file, and is zero-filled past the clip.
 * @param I The borrowed faces.
 * @param K The source sector.
 * @return 1 on success, 0 when the banks lie outside the sub-file or allocation fails. */
static int tsimp_colours(tsimp_t* I, const tscache_t* K) {
    if (K->nearBase > K->farBase || K->farBase >= s_hps.terrLen) return 0;
    const uint32_t span = K->farBase - K->nearBase + 0x400u;
    I->nC = (int)(span / 4u);
    I->nearCol = (uint8_t*)calloc(span, 1);
    if (!I->nearCol) return 0;
    const uint32_t n = (K->nearBase + span <= s_hps.terrLen) ? span : s_hps.terrLen - K->nearBase;
    memcpy(I->nearCol, s_hps.terr + K->nearBase, n);
    I->farCol = I->nearCol + (K->farBase - K->nearBase);
    return 1;
}

/** @brief Moves a source sector's vertices into a scene sector's frame.
 * @param src The source vertices.
 * @param n Their count.
 * @param d The source origin minus the scene sector origin, per axis.
 * @param out Receives the moved vertices (n entries).
 * @return 1 when every moved coordinate fits the vertex type, else 0. */
static int tsimp_move_verts(const tsvert_t* src, int n, const int d[3], tsvert_t* out) {
    for (int j = 0; j < n; j++) {
        const int x = src[j].x + d[0], y = src[j].y + d[1], z = src[j].z + d[2];
        if (x < INT16_MIN || x > INT16_MAX || y < INT16_MIN || y > INT16_MAX || z < INT16_MIN || z > INT16_MAX)
            return 0;
        out[j] = (tsvert_t){ (int16_t)x, (int16_t)y, (int16_t)z };
    }
    return 1;
}

/** @brief Borrows the source sector's LP faces a cut-down scene sector lacks.
 *
 * The source LP vertex bank moves into the scene sector's frame whole; the
 * faces tsimp_missing_lp marked are copied with their colour offsets, which
 * index the source LP colour array read through the TSRC_TERR_BASE window.
 * The uncov tags are set later by tsc_tag_uncovered.
 * @param I The borrowed faces.
 * @param K The source sector.
 * @param miss The flags tsimp_missing_lp wrote.
 * @param d The source origin minus the scene sector origin, per axis.
 * @return 1 on success (also when nothing is marked), 0 on an allocation or range failure. */
static int tsimp_borrow_lp(tsimp_t* I, const tscache_t* K, const uint8_t* miss, const int d[3]) {
    if (!K->lpV || !K->lpF || K->nLpV <= 0) return 1;
    I->lpV = (tsvert_t*)malloc(sizeof(tsvert_t) * (size_t)K->nLpV);
    I->lpF = (tsface_t*)malloc(sizeof(tsface_t) * (size_t)K->nLpF);
    if (!I->lpV || !I->lpF || !tsimp_move_verts(K->lpV, K->nLpV, d, I->lpV)) return 0;
    I->nLpV = K->nLpV;
    I->lpColBase = TSRC_TERR_BASE + K->lpColBase;
    I->lpColSpan = K->lpColSpan;
    for (int g = 0; g < K->nLpF; g++)
        if (miss[g]) I->lpF[I->nLpF++] = K->lpF[g];
    return 1;
}

/** @brief Widens a sector's vertex AABB over the vertices its borrowed faces use.
 * @param C The sector; C->imp is set. */
static void tsimp_aabb(tscache_t* C) {
    const tsimp_t* I = C->imp;
    for (int f = 0; f < I->nF + I->nLpF; f++) {
        const tsface_t* F = f < I->nF ? &I->F[f] : &I->lpF[f - I->nF];
        const tsvert_t* V = f < I->nF ? I->V : I->lpV;
        const int ci[4] = { F->i0, F->i1, F->i2, F->i3 };
        for (int q = 0; q < 4; q++) {
            const tsvert_t* v = &V[ci[q]];
            if (v->x < C->vMinX) C->vMinX = v->x;
            if (v->x > C->vMaxX) C->vMaxX = v->x;
            if (v->y < C->vMinY) C->vMinY = v->y;
            if (v->y > C->vMaxY) C->vMaxY = v->y;
            if (v->z < C->vMinZ) C->vMinZ = v->z;
            if (v->z > C->vMaxZ) C->vMaxZ = v->z;
        }
    }
}

static int s_imp_census = -1;   /* OPENPETE_TERRAIN_HPIMPORT_CENSUS: one line per attach */

/** @brief Finds the source sector a scene sector borrows from.
 *
 * Runs on a distance-binning game whose layout names an HP source, outside
 * OPENPETE_TERRAIN_HPIMPORT=0, the forced texture-table modes and imported
 * sectors, for a sector with LP faces. The source must hold every LP face
 * (hps_match), carry HP faces, and differ from the sector's HP face list
 * (tsimp_same_hp). A match miss is listed under the census.
 * @param cpu The guest CPU state.
 * @param C The scene sector.
 * @param texTblMode The walk's texture-table mode.
 * @return The source sector index, or -1. */
static int tsimp_source(CPUState* cpu, const tscache_t* C, int texTblMode) {
    static int s_imp_off = -1;   /* OPENPETE_TERRAIN_HPIMPORT=0: borrow nothing (A/B) */
    if (s_imp_off < 0) { const char* e = getenv("OPENPETE_TERRAIN_HPIMPORT"); s_imp_off = e && *e == '0'; }
    if (s_imp_off || !game_layout.terrain.hp_source_wad_entry || !game_layout.terrain.hp_far) return -1;
    if (texTblMode != 0 || C->virt || !C->lpF || C->nLpF <= 0 || !hps_ready(cpu)) return -1;
    const int k = hps_match(C);
    if (k < 0) {
        ENV_FLAG_LATCH(s_imp_census, "OPENPETE_TERRAIN_HPIMPORT_CENSUS");
        if (s_imp_census) fprintf(stderr, "[hpimport-miss] sector=%08X nLpF=%d nUncov=%d why=%s\n", C->s, C->nLpF, C->nUncov, s_hps_why);
        return -1;
    }
    const tscache_t* K = &s_hps.sec[k];
    return (!K->hpV || !K->hpF || K->nHpF <= 0 || tsimp_same_hp(C, K)) ? -1 : k;
}

/** @brief Borrows the source level's HP and LP faces for a scene sector whose LP faces it holds.
 *
 * Runs on a distance-binning game whose layout names an HP source, for a
 * sector with uncov LP faces or whose matched source sector holds LP faces
 * it lacks, outside the forced texture-table debug modes. The match runs on
 * the sector's decoded LP pose; the borrowed faces keep the source
 * sub-file's pose (the source level's animation registry is not loaded),
 * with no sub-tick lerp. The matched source sector's HP vertices move into
 * the sector's frame (the origin difference). Every source HP face is a
 * candidate for a sector with no HP of its own; otherwise a face is one
 * when it lies within TSC_LPCOV_TOL of an uncov LP sample
 * (tsimp_near_uncov) or of a source LP face the sector lacks
 * (tsimp_near_missing). A candidate is borrowed unless it duplicates a
 * scene HP face (tsimp_dup); its materials come from the source table
 * (tsimp_retexture). The source LP faces the sector lacks are borrowed
 * too (tsimp_borrow_lp), and the source sector's bounding sphere and LP
 * near bound are kept for sector_header. The sector's AABB widens over the
 * borrowed vertices.
 * @param cpu The guest CPU state.
 * @param C The freshly decoded sector, uncov-tagged against its own HP.
 * @param texTblMode The walk's texture-table mode; nonzero borrows nothing.
 * @param hiDetail The hi-detail flag the sector's faces were decoded with.
 * @param sectorPtr The scene sector table's guest address.
 * @param sectorCnt The scene sector count.
 * @return 1 when HP or LP faces were borrowed (C->imp set), else 0. */
static int tsimp_attach(CPUState* cpu, tscache_t* C, int texTblMode, int hiDetail,
                        uint32_t sectorPtr, uint32_t sectorCnt) {
    const int k = tsimp_source(cpu, C, texTblMode);
    if (k < 0) return 0;
    const tscache_t* K = &s_hps.sec[k];
    const int dx = K->originX - C->originX, dy = K->originY - C->originY, dz = K->originZ - C->originZ;
    const int dvec[3] = { dx, dy, dz };
    uint8_t miss[256];
    const int nMiss = tsimp_missing_lp(C, K, miss);
    if (C->nUncov == 0 && nMiss == 0) return 0;
    scene_hp_refresh(cpu, sectorPtr, sectorCnt);
    tsimp_t* I = (tsimp_t*)calloc(1, sizeof(*I));
    if (!I) return 0;
    I->src = k; I->nV = K->nHpV; I->srcFlags = K->flags;
    I->centreX = K->centreX; I->centreY = K->centreY; I->centreZ = K->centreZ; I->radius = K->radius;
    I->lpNearSz = K->lpNearSz;
    I->V = (tsvert_t*)malloc(sizeof(tsvert_t) * (size_t)K->nHpV);
    I->F = (tsface_t*)malloc(sizeof(tsface_t) * (size_t)K->nHpF);
    int ok = I->V && I->F && tsimp_colours(I, K) && tsimp_move_verts(K->hpV, K->nHpV, dvec, I->V);
    for (int f = 0; ok && f < K->nHpF; f++) {
        const tsface_t* H = &K->hpF[f];
        if (!hps_face_ok(H, I->nV)) continue;
        if (!tsimp_candidate(C, K, miss, nMiss, dvec, I->V, I->nV, H)) continue;
        I->nCand++;
        if (tsimp_dup(C, I->V, H)) { I->nDup++; continue; }
        tsface_t* F = &I->F[I->nF];
        *F = *H;
        tsimp_retexture(cpu, F, hiDetail);
        F->uncov = F->uncovX = 0;
        I->nF++;
    }
    if (ok && nMiss) ok = tsimp_borrow_lp(I, K, miss, dvec);
    ENV_FLAG_LATCH(s_imp_census, "OPENPETE_TERRAIN_HPIMPORT_CENSUS");
    if (s_imp_census) {
        int idx = -1;
        for (uint32_t q = 0; q < sectorCnt && idx < 0; q++)
            if (rd(cpu, sectorPtr + q * 4u) == C->s) idx = (int)q;
        fprintf(stderr, "[hpimport] frame=%u sector=%d src=%d dorigin=%d,%d,%d srcHP=%d missLP=%d cand=%d dup=%d "
                        "borrowed=%d borrowedLP=%d\n", openpete_frame, idx, k, dx, dy, dz, K->nHpF, nMiss,
                I->nCand, I->nDup, I->nF, I->nLpF);
    }
    if (!ok || (I->nF == 0 && I->nLpF == 0)) { tsimp_free(I); return 0; }
    C->imp = I;
    tsimp_aabb(C);
    return 1;
}

/* The walk's sector table extended with the HP source level's sectors the
 * scene has no sector for, rebuilt when the scene's table changes. */
static struct {
    uint64_t sig;                /* hash of the scene table's address list; 0 = not built */
    uint32_t nGuest;             /* scene sectors at the head of s_tsv_table */
    uint8_t* hide;               /* per source sector, 512 flags: LP faces 0-255, HP faces 256-511 */
} s_tsv;

/** @brief Tests whether a face of a source sector lies on a scene sector's surface.
 *
 * Every sample point (tsc_face_samples), moved into the scene sector's
 * frame, lies within TSIMP_DUP_TOL of the scene sector's LP or HP mesh.
 * @param K The source sector.
 * @param V The face's vertex bank (K's LP or HP bank).
 * @param F The source face.
 * @param T The scene sector.
 * @return 1 when the face lies on the scene sector's surface. */
static int tsv_face_on(const tscache_t* K, const tsvert_t* V, const tsface_t* F, const tscache_t* T) {
    const tsmesh_t M[2] = { { T->lpV, T->lpF, T->nLpV, T->nLpF }, { T->hpV, T->hpF, T->nHpV, T->nHpF } };
    const double tol2 = TSIMP_DUP_TOL * TSIMP_DUP_TOL;
    double smp[9][3];
    const int ns = tsc_face_samples(V, F, smp);
    for (int i = 0; i < ns; i++) {
        const double p[3] = { smp[i][0] + K->originX - T->originX, smp[i][1] + K->originY - T->originY,
                              smp[i][2] + K->originZ - T->originZ };
        if (tsc_hp_d2(M, 2, p, tol2) > tol2) return 0;
    }
    return 1;
}

/** @brief Computes a sector's world-space vertex box over both tiers.
 * @param C The sector.
 * @param lo Receives the minimum corner.
 * @param hi Receives the maximum corner. */
static void tsv_world_box(const tscache_t* C, int lo[3], int hi[3]) {
    lo[0] = lo[1] = lo[2] = INT32_MAX; hi[0] = hi[1] = hi[2] = INT32_MIN;
    for (int t = 0; t < 2; t++) {
        const tsvert_t* V = t ? C->hpV : C->lpV;
        const int n = t ? C->nHpV : C->nLpV;
        for (int k = 0; V && k < n; k++) {
            const int w[3] = { C->originX + V[k].x, C->originY + V[k].y, C->originZ + V[k].z };
            for (int a = 0; a < 3; a++) { lo[a] = w[a] < lo[a] ? w[a] : lo[a]; hi[a] = w[a] > hi[a] ? w[a] : hi[a]; }
        }
    }
}

/** @brief Tests whether two world boxes meet within TSC_LPCOV_TOL.
 * @param alo The first box's minimum corner.
 * @param ahi The first box's maximum corner.
 * @param blo The second box's minimum corner.
 * @param bhi The second box's maximum corner.
 * @return 1 when they meet. */
static int tsv_boxes_meet(const int alo[3], const int ahi[3], const int blo[3], const int bhi[3]) {
    for (int a = 0; a < 3; a++)
        if (alo[a] > bhi[a] + TSC_LPCOV_TOL || ahi[a] < blo[a] - TSC_LPCOV_TOL) return 0;
    return 1;
}

/** @brief Marks one tier of a source sector's faces that lie on a scene sector's surface.
 * @param K The source sector.
 * @param hp 0 for the LP tier, 1 for the HP tier.
 * @param T The scene sector.
 * @param hid The tier's flags (256 entries); set flags stay set.
 * @return The number of faces newly marked. */
static int tsv_hide_tier(const tscache_t* K, int hp, const tscache_t* T, uint8_t* hid) {
    const tsface_t* F = hp ? K->hpF : K->lpF;
    const tsvert_t* V = hp ? K->hpV : K->lpV;
    const int nF = hp ? K->nHpF : K->nLpF, nV = hp ? K->nHpV : K->nLpV;
    int n = 0;
    for (int f = 0; F && f < nF && f < 256; f++)
        if (!hid[f] && hps_face_ok(&F[f], nV) && tsv_face_on(K, V, &F[f], T)) { hid[f] = 1; n++; }
    return n;
}

/** @brief Decodes the scene's sectors from guest RAM and marks the source sectors they match.
 * @param cpu The guest CPU state.
 * @param addr The scene sector addresses.
 * @param cnt The scene sector count.
 * @param T Receives the decodes (cnt entries, zeroed by the caller).
 * @param matched Receives one flag per source sector.
 * @return 1 when at least one scene sector has LP faces and every one that does matches. */
static int tsv_match_scene(CPUState* cpu, const uint32_t* addr, uint32_t cnt, tscache_t* T, uint8_t* matched) {
    const twsrc_t src = { cpu, NULL, 0u };
    uint32_t nLp = 0, nMatched = 0;
    for (uint32_t q = 0; q < cnt; q++) {
        if (!addr[q]) continue;
        const uint32_t g[5] = { rd(cpu, addr[q] + 0x04), rd(cpu, addr[q] + 0x08), rd(cpu, addr[q] + 0x0C),
                                rd(cpu, addr[q] + 0x10), rd(cpu, addr[q] + 0x14) };
        tsc_decode_body(&src, addr[q], rd(cpu, addr[q]), g, &T[q], 0u, 0u, 0, 0);
        nLp += T[q].nLpF > 0;
        const int k = hps_match(&T[q]);
        if (k >= 0) { matched[k] = 1; nMatched++; }
    }
    return nMatched > 0 && nMatched == nLp;
}

/** @brief Imports one unmatched source sector: marks its faces on scene surfaces and appends it to s_tsv_table.
 * @param k The source sector index.
 * @param T The scene sector decodes.
 * @param cnt The scene sector count.
 * @param n The extended table's count; advanced when the sector is appended. */
static void tsv_import(int k, const tscache_t* T, uint32_t cnt, uint32_t* n) {
    const tscache_t* K = &s_hps.sec[k];
    uint8_t* hid = s_tsv.hide + (size_t)k * 512u;
    int klo[3], khi[3], nh = 0;
    tsv_world_box(K, klo, khi);
    for (uint32_t q = 0; q < cnt; q++) {
        int tlo[3], thi[3];
        tsv_world_box(&T[q], tlo, thi);
        if (!tsv_boxes_meet(klo, khi, tlo, thi)) continue;
        nh += tsv_hide_tier(K, 0, &T[q], hid) + tsv_hide_tier(K, 1, &T[q], hid + 256);
    }
    if (s_imp_census)
        fprintf(stderr, "[hpimport-sector] src=%d nLpF=%d nHpF=%d index=%u faces on scene surfaces (not drawn)=%d\n",
                k, K->nLpF, K->nHpF, *n, nh);
    if (nh < K->nLpF + K->nHpF) s_tsv_table[(*n)++] = TSRC_TERR_BASE + K->s;
}

/** @brief Rebuilds the walk's extended sector table when the scene's sector table changed.
 *
 * Every scene sector decodes from guest RAM (reads only) and is matched to
 * the source sector holding its LP faces (tsv_match_scene). Nothing is
 * imported unless every scene sector with LP faces matches: the scene is
 * then built from the source level, and each source sector no scene sector
 * matches is imported (tsv_import: its address in the TSRC_TERR_BASE
 * window appended to s_tsv_table). Its faces lying on a scene sector's
 * surface (tsv_face_on, over scene sectors whose world box meets its own)
 * are marked hidden (s_tsv.hide), so the import never draws over the
 * scene's own geometry; OPENPETE_TERRAIN_HPIMPORT_CENSUS lists the counts.
 * @param cpu The guest CPU state.
 * @param sectorPtr The scene sector table's guest address.
 * @param sectorCnt The scene sector count.
 * @return The extended sector count; sectorCnt when nothing is imported. */
static uint32_t tsv_refresh(CPUState* cpu, uint32_t sectorPtr, uint32_t sectorCnt) {
    if (!game_layout.terrain.hp_source_wad_entry || sectorCnt == 0u || sectorCnt > 512u) return sectorCnt;
    static int s_off = -1;   /* OPENPETE_TERRAIN_HPIMPORT=0 imports nothing */
    if (s_off < 0) { const char* e = getenv("OPENPETE_TERRAIN_HPIMPORT"); s_off = e && *e == '0'; }
    if (s_off || !hps_ready(cpu)) return sectorCnt;
    uint32_t addr[512];
    for (uint32_t q = 0; q < sectorCnt; q++) addr[q] = rd(cpu, sectorPtr + q * 4u);
    const uint64_t sig = (openpete_fnv1a64(addr, sizeof(uint32_t) * sectorCnt) ^ sectorPtr) | 1u;
    if (sig == s_tsv.sig) return s_tsv_n;
    s_tsv.sig = sig;
    s_tsv.nGuest = sectorCnt;
    memcpy(s_tsv_table, addr, sizeof(uint32_t) * sectorCnt);
    free(s_tsv.hide);
    s_tsv.hide = NULL;
    tscache_t* T = (tscache_t*)calloc(sectorCnt, sizeof(tscache_t));
    uint8_t* matched = (uint8_t*)calloc((size_t)s_hps.nSec, 1);
    uint32_t n = sectorCnt;
    ENV_FLAG_LATCH(s_imp_census, "OPENPETE_TERRAIN_HPIMPORT_CENSUS");
    if (T && matched && tsv_match_scene(cpu, addr, sectorCnt, T, matched))
        s_tsv.hide = (uint8_t*)calloc((size_t)s_hps.nSec, 512);
    for (int k = 0; s_tsv.hide && k < s_hps.nSec && n < 1024u; k++)
        if (!matched[k] && (s_hps.sec[k].nLpF > 0 || s_hps.sec[k].nHpF > 0)) tsv_import(k, T, sectorCnt, &n);
    for (uint32_t q = 0; T && q < sectorCnt; q++) {
        free(T[q].lpV); free(T[q].lpF); free(T[q].hpV); free(T[q].hpF);
    }
    free(T); free(matched);
    s_tsv_n = n;
    return n;
}

/** @brief Points the walk at the extended sector table when it imports sectors, in every LOD mode.
 * @param cpu The guest CPU state.
 * @param sectorPtr The scene sector table's address; replaced by TSV_TABLE_BASE.
 * @param sectorCnt The scene sector count; replaced by the extended count. */
static void tsv_extend(CPUState* cpu, uint32_t* sectorPtr, uint32_t* sectorCnt) {
    const uint32_t ext = tsv_refresh(cpu, *sectorPtr, *sectorCnt);
    if (ext > *sectorCnt) { *sectorPtr = TSV_TABLE_BASE; *sectorCnt = ext; }
}

/** @brief Points an imported sector's face materials at the auxiliary VRAM bank.
 *
 * Both tiers' materials take bank 1 and no texture-animation generation:
 * their Tiledef ids name the source table, whose animation registry is not
 * loaded. Faces tsv_refresh found on a scene sector's surface are removed
 * from the face lists, so neither the walk nor the bake draws them.
 * @param C The imported sector, freshly decoded. */
static void tsv_mark_faces(tscache_t* C) {
    const uint8_t* hid = NULL;
    for (int k = 0; s_tsv.hide && k < s_hps.nSec && !hid; k++)
        if (TSRC_TERR_BASE + s_hps.sec[k].s == C->s) hid = s_tsv.hide + (size_t)k * 512u;
    for (int t = 0; t < 2; t++) {
        tsface_t* F = t ? C->hpF : C->lpF;
        int* n = t ? &C->nHpF : &C->nLpF;
        int kept = 0;
        for (int f = 0; F && f < *n; f++) {
            if (hid && f < 256 && hid[t * 256 + f]) continue;
            F[kept] = F[f];
            F[kept].mat.bank = F[kept].matN.bank = 1;
            F[kept].tgen = F[kept].tgenHq = 0;
            kept++;
        }
        if (F) *n = kept;
    }
}

/** @brief Decodes a cache entry's body: a scene sector from its tables, an imported one through the windows.
 *
 * An imported sector (tsv_is_virtual) reads its tiles from the source table
 * and its materials sample the auxiliary VRAM bank (tsv_mark_faces).
 * @param cpu The guest CPU state.
 * @param C The entry, cleared, with s and sig set.
 * @param s The sector address.
 * @param g The header signature words.
 * @param lqTable The scene LQ table.
 * @param hqTable The scene HQ table.
 * @param texTblMode The walk's texture-table mode.
 * @param hiDetail The hi-detail flag. */
static void tsc_decode_entry(CPUState* cpu, tscache_t* C, uint32_t s, const uint32_t g[5],
                             uint32_t lqTable, uint32_t hqTable, int texTblMode, int hiDetail) {
    C->virt = (uint8_t)tsv_is_virtual(s);
    if (C->virt) lqTable = hqTable = TSRC_TEX_BASE;
    tsc_decode_body(&(twsrc_t){ cpu, NULL, 0u }, s, rd(cpu, s + 0x00), g, C,
                    lqTable, hqTable, texTblMode, hiDetail);
    if (C->virt) tsv_mark_faces(C);
}

/** @brief Applies the guest-side tags of a freshly decoded scene sector: keyframe pose and fill tags.
 *
 * An imported sector has neither (no guest keyframe registry entry, no
 * guest fill tags).
 * @param cpu The guest CPU state.
 * @param C The entry.
 * @param sectorPtr The sector table's address.
 * @param sectorCnt The sector count. */
static void tsc_scene_tags(CPUState* cpu, tscache_t* C, uint32_t sectorPtr, uint32_t sectorCnt) {
    if (C->virt) return;
    tsc_kf_apply(cpu, C);
    tsc_tag_gapfill(cpu, C, sectorPtr, sectorCnt);
}

/** @brief Returns sector s's decoded cache entry, decoding it on a miss or a changed signature.
 *
 * The entry holds both tiers' vertices and faces, the colour-array bases and
 * the bytes each tier's faces index, keyed by the sector address and its
 * header signature words; a miss evicts the least recently used entry. */
static const tscache_t* tsc_get(CPUState* cpu, uint32_t s,
                                uint32_t lqTable, uint32_t hqTable,
                                int texTblMode, int hiDetail,
                                uint32_t sectorPtr, uint32_t sectorCnt) {
    uint32_t g[5] = { rd(cpu, s + 0x04), rd(cpu, s + 0x08), rd(cpu, s + 0x0C),
                      rd(cpu, s + 0x10), rd(cpu, s + 0x14) };
    uint32_t h = (((s >> 2) * 2654435761u) >> 14) & (TSC_SLOTS - 1);
    /* Probe window + LRU eviction. A 2-way (h, h^1) alternating eviction
     * thrashes when 3+ visible sectors share a slot pair: each miss is a full
     * re-decode + gapfill classification (ms-scale for big sectors), repeated
     * every present. The window makes thrash need TSC_WINDOW+1 hot sectors in
     * one window; LRU (stamped per terrain_build call) evicts a stale sector,
     * never this present's working set. Probes are five-word compares, so the
     * window width is negligible next to one re-decode. */
    ENV_FLAG_LATCH(s_vh_on, "OPENPETE_TERRAIN_VHASH_CENSUS");
    uint32_t vh = 0;
    tscache_t* reuse = NULL;   /* stale decode to re-fill in place */
    tscache_t* C = NULL;
    for (uint32_t k = 0; k < TSC_WINDOW; k++) {
        tscache_t* W = &s_tsc[(h + k) & (TSC_SLOTS - 1)];
        if (W->s == s && W->sig[0]==g[0] && W->sig[1]==g[1] && W->sig[2]==g[2]
            && W->sig[3]==g[3] && W->sig[4]==g[4]) {
            if (!W->polyanim) {
                if (!vh) vh = tsc_vhash(cpu, s, g);
                if (s_vh_on) {
                    if (openpete_frame != s_vh_frame) {
                        tsc_vhash_census_flush(); s_vh_frame = openpete_frame;
                    }
                    s_vh_hashed++;
                }
                if (W->vhash != vh) {      /* vertices rewritten in place: re-decode
                                            * into this same slot, so the stale
                                            * decode cannot be probed again */
                    reuse = W;
                    if (s_vh_on) {
                        s_vh_moved++;
                        for (uint32_t q = 0; q < sectorCnt && s_vh_listn < 32; q++)
                            if (rd(cpu, sectorPtr + q * 4) == s) {
                                s_vh_list[s_vh_listn++] = (uint16_t)q; break;
                            }
                    }
                    break;
                }
            }
            s_tsc_hit++; W->lastUse = s_tsc_stamp;
            if (W->fillGen != openpete_fillpick_generation())
                tsc_retag_fill(cpu, W, sectorPtr, sectorCnt);
            return W;
        }
    }
    {
        s_tsc_miss++;
        tscache_t* hot = NULL;                       /* fallback: LRU among GPU-owned */
        C = reuse;
        for (uint32_t k = 0; !reuse && k < TSC_WINDOW; k++) {  /* empty slot first, else LRU */
            uint32_t idx = (h + k) & (TSC_SLOTS - 1);
            /* OPTC_SLOT_DEAD's 9-bit id is the dead-stamp face_flags
             * sentinel (chunks baked with it are culled as dead), so the
             * decode cache never holds a sector there. */
            if (idx == OPTC_SLOT_DEAD) continue;
            tscache_t* W = &s_tsc[idx];
            if (!W->s) { C = W; break; }
            /* Never evict a slot the GPU cache validated as owned THIS
             * present: the re-resolve would free the owner's chunks and
             * republish the slot after its face walk was already skipped,
             * so the owner vanishes for a present. The incoming sector takes
             * another slot; only when every candidate is owned (TSC_WINDOW+
             * owned sectors hashing into one window) does plain LRU resume,
             * and the loser then rides tgpu_ensure's contest claim back to
             * the CPU walk. */
            if (tgpu_slot_hot(idx)) {
                if (!hot || W->lastUse < hot->lastUse) hot = W;
                continue;
            }
            if (!C || W->lastUse < C->lastUse) C = W;
        }
        if (!C) C = hot;
        free(C->lpV); free(C->lpF); free(C->hpV); free(C->hpF);
        free(C->lpVprev); free(C->hpVprev); free(C->lpVf); free(C->hpVf);
        tsimp_free(C->imp);
        memset(C, 0, sizeof(*C));
        C->s = s; memcpy(C->sig, g, sizeof(g));
        C->vhash = vh ? vh : tsc_vhash(cpu, s, g);
        C->lastUse = s_tsc_stamp;
        tsc_decode_entry(cpu, C, s, g, lqTable, hqTable, texTblMode, hiDetail);
        {   /* GPU-cache liveness: any (re)decode invalidates the slot's chunks */
            static uint32_t s_decode_ctr;
            C->decodeGen = ++s_decode_ctr;
        }
        C->lpg_state = 0;              /* re-decoded sector: re-derive the LP grade */
        C->fillGen = openpete_fillpick_generation();
        tsc_scene_tags(cpu, C, sectorPtr, sectorCnt);
        {   /* Vertex AABB (both tiers) for the near-claim refinement. */
            int16_t mnx = 32767, mny = 32767, mnz = 32767;
            int16_t mxx = -32768, mxy = -32768, mxz = -32768;
            for (int t2 = 0; t2 < 2; t2++) {
                const tsvert_t* vv = t2 ? C->hpV : C->lpV;
                int nvv = t2 ? C->nHpV : C->nLpV;
                for (int k = 0; vv && k < nvv; k++) {
                    if (vv[k].x < mnx) mnx = vv[k].x;
                    if (vv[k].x > mxx) mxx = vv[k].x;
                    if (vv[k].y < mny) mny = vv[k].y;
                    if (vv[k].y > mxy) mxy = vv[k].y;
                    if (vv[k].z < mnz) mnz = vv[k].z;
                    if (vv[k].z > mxz) mxz = vv[k].z;
                }
            }
            C->vMinX = mnx; C->vMinY = mny; C->vMinZ = mnz;
            C->vMaxX = mxx; C->vMaxY = mxy; C->vMaxZ = mxz;
        }
        C->polyanim = tsc_sector_polyanim(cpu, s, sectorPtr, sectorCnt);
        if (C->polyanim && C->lpF) {   /* stale-bank guard: see tsc_sector_polyanim */
            for (int f = 0; f < C->nLpF; f++) C->lpF[f].gapfill = 0;
            C->nGapfill = 0;
        }
        tsc_tag_uncovered(C);
        if (tsimp_attach(cpu, C, texTblMode, hiDetail, sectorPtr, sectorCnt)) {
            tsc_tag_uncovered(C);
            if (s_imp_census)
                fprintf(stderr, "[hpimport] sector@%08X uncov=%d uncovX=%d\n", C->s, C->nUncov, C->nUncovX);
        }
    }
    return C;
}

/* Geometry (vertex) env-anim liveness.
 * func_8002A6FC's poly-anim handlers (blocks 8002AA18 LP / 8002ABB8 HP) advance
 * a keyframe per descriptor and stamp it into sector+0x18/+0x1A; the .s Phase-1
 * walk then GTE-INTPL blends the descriptor's keyframe vertex banks and rewrites
 * the sector's packed vertex words IN PLACE (block_80025C08..80025D10) every
 * canonical tick while the animation runs. The cache signature only covers
 * header words +0x04..+0x14, so cached verts would stay at whatever pose the
 * sector was first decoded in.
 * Mirror the game's own registry instead of hashing vertex RAM: any sector named
 * by an LP/HP poly-anim descriptor (u16 sector index at desc+4) gets its cached
 * verts re-decoded once per canonical tick (rolling the old decode into a prev
 * bank), and on sub-tick presents lerped prev->cur at the spine's alpha so the
 * geometry moves at present rate, not 29.913 Hz. A handful of descriptors per
 * level, ≤256 words per sector - negligible next to the walk itself. Colours
 * already read live; texmats are covered by tsc_tile_gens/tsc_refresh_anim. */
/* Sub-tick vert interp state (set around terrain_build by terrain_reextract;
 * zero on the canonical path so the 30 Hz render stays byte-exact int verts). */
static int    s_vanim_sub;     /* this build is a sub-tick re-extract (never roll banks) */
static int    s_vanim_on;      /* ...with a paired camera bracket (alpha valid) */
static double s_vanim_alpha;   /* the spine's 1-tick-lag present alpha (prev->cur) */
static uint32_t s_vanim_pres;  /* per-terrain_build counter - validity stamp for the banks */

/* Teleport guard: continuous env vert anim (the Sunny-Flight wall) moves a few
 * local units per tick; a keyframe-bank reset (loop restart, level event) jumps
 * hundreds. 64 local units/tick splits the two by an order of magnitude each
 * way - above it, snap to cur instead of sweeping the sector across the map. */
#define VANIM_JUMP_LOCAL 64
/** @brief Tests whether any vertex moved more than VANIM_JUMP_LOCAL units on an axis between two banks.
 *
 * A step that large marks a keyframe reset, which the caller snaps rather
 * than sweeps.
 * @param p The previous bank.
 * @param c The current bank.
 * @param n The vertex count.
 * @return 1 when some vertex jumped, else 0. */
static int tsc_vanim_jump(const tsvert_t* p, const tsvert_t* c, int n) {
    for (int k = 0; k < n; k++) {
        if (abs((int)c[k].x - p[k].x) > VANIM_JUMP_LOCAL ||
            abs((int)c[k].y - p[k].y) > VANIM_JUMP_LOCAL ||
            abs((int)c[k].z - p[k].z) > VANIM_JUMP_LOCAL) return 1;
    }
    return 0;
}
/** @brief Writes the linear interpolation of two vertex banks to a float array.
 *
 * out[3k + axis] = p + (c - p) * a per component; a is not clamped.
 * @param p The previous bank.
 * @param c The current bank.
 * @param n The vertex count.
 * @param a The interpolation alpha.
 * @param out Receives 3 * n floats. */
static void tsc_vanim_lerp(const tsvert_t* p, const tsvert_t* c, int n,
                           double a, float* out) {
    for (int k = 0; k < n; k++) {
        out[k*3+0] = (float)(p[k].x + ((double)c[k].x - p[k].x) * a);
        out[k*3+1] = (float)(p[k].y + ((double)c[k].y - p[k].y) * a);
        out[k*3+2] = (float)(p[k].z + ((double)c[k].z - p[k].z) * a);
    }
}
/** @brief Refreshes the vertex banks of every decode-cached sector that a poly-anim descriptor names.
 *
 * Returns when sectorPtr is 0. For each vertex-kind registry entry whose
 * sector is in the decode cache, once per build (vrollPres, vfracPres): a
 * canonical build copies lpV and hpV into the previous banks (allocated on
 * the first roll), re-decodes them from RAM, and steps vertGen when the
 * hash of the decoded words changed; a sub-tick build with a camera pair
 * (s_vanim_on) fills the float banks lpVf and hpVf with the previous-to-
 * current lerp at s_vanim_alpha, except for a sector where tsc_vanim_jump
 * fires, which stays on the current bank. A failed allocation leaves the
 * bank unfilled. Prints [vanim] lines under OPENPETE_TERRAIN_VANIMDBG.
 * @param cpu The guest CPU state.
 * @param sectorPtr The sector pointer table's guest address.
 * @param sectorCnt The sector count. */
static void tsc_vert_anim_refresh(CPUState* cpu, uint32_t sectorPtr, uint32_t sectorCnt) {
    if (!sectorPtr) return;
    static int s_vdbg = -1;
    ENV_FLAG_LATCH(s_vdbg, "OPENPETE_TERRAIN_VANIMDBG");
    int dbg_desc = 0, dbg_cached = 0, dbg_prev = 0, dbg_filled = 0;
    uint32_t dbg_seen = 0;
    {
        env_anim_iter_t it;
        uint32_t idx; int kind;
        env_anim_iter_init(&it, cpu, sectorCnt);
        while (env_anim_iter_next(&it, &idx, &kind)) {
            if (kind != ENV_ANIM_KIND_VERT) continue;   /* colours are read live */
            uint32_t s = rd(cpu, sectorPtr + idx * 4);
            if (!s) continue;
            dbg_desc++;
            uint32_t h = (((s >> 2) * 2654435761u) >> 14) & (TSC_SLOTS - 1);
            tscache_t* C = NULL;
            for (uint32_t w = 0; w < TSC_WINDOW; w++) {   /* same probe window as tsc_get */
                tscache_t* W = &s_tsc[(h + w) & (TSC_SLOTS - 1)];
                if (W->s == s) { C = W; break; }
            }
            if (!C) continue;
            dbg_cached++;
            if (C->lpVprev) dbg_prev++;
            int nLp = C->lpV ? C->nLpV : 0, nHp = C->hpV ? C->nHpV : 0;
            if (!s_vanim_sub) {
                /* Canonical build (once per tick, AFTER the .s ran its INTPL vert
                 * rewrite): roll prev = last tick's decode, re-decode cur from RAM.
                 * The roll pairs cur with the tick-N snapshot the interp spine
                 * committed, so the sub-tick lerp below rides the same 1-tick-lag
                 * timeline as the camera/mobys. */
                if (C->vrollPres == s_vanim_pres) continue;   /* named by both lists */
                C->vrollPres = s_vanim_pres;
                if (nLp) {
                    if (!C->lpVprev) C->lpVprev = (tsvert_t*)malloc(sizeof(tsvert_t) * (size_t)nLp);
                    if (C->lpVprev) memcpy(C->lpVprev, C->lpV, sizeof(tsvert_t) * (size_t)nLp);
                    tsc_decode_verts(&(twsrc_t){ cpu, NULL, 0u }, C->s + 0x1C, nLp, C->flat, C->lpV);
                }
                if (nHp) {
                    if (!C->hpVprev) C->hpVprev = (tsvert_t*)malloc(sizeof(tsvert_t) * (size_t)nHp);
                    if (C->hpVprev) memcpy(C->hpVprev, C->hpV, sizeof(tsvert_t) * (size_t)nHp);
                    tsc_decode_verts(&(twsrc_t){ cpu, NULL, 0u }, C->s + 0x1C + ((C->sig[4] >> 24) & 0xFF) * 4,
                                     nHp, C->flat, C->hpV);
                }
                tsc_kf_apply(cpu, C);
                {   /* GPU-cache liveness: the re-decode is unconditional per tick,
                     * so hash the decoded verts and bump vertGen only when the
                     * geometry actually moved (else anim-listed sectors would
                     * rebuild their GPU chunks every tick for nothing). */
                    uint32_t h = 2166136261u;
                    const uint8_t* hp = (const uint8_t*)C->lpV;
                    for (size_t n = sizeof(tsvert_t) * (size_t)(C->lpV ? nLp : 0), j = 0; j < n; j++)
                        h = (h ^ hp[j]) * 16777619u;
                    hp = (const uint8_t*)C->hpV;
                    for (size_t n = sizeof(tsvert_t) * (size_t)(C->hpV ? nHp : 0), j = 0; j < n; j++)
                        h = (h ^ hp[j]) * 16777619u;
                    if (h != C->vertHash) { C->vertHash = h; C->vertGen++; tsc_tag_uncovered(C); }
                }
            } else if (s_vanim_on) {
                /* Sub-tick present: fill the fractional lerp banks prev->cur at the
                 * spine's alpha. Fractional floats on purpose - 1 LP local unit is
                 * 16 world units, so an int16 lerp would step the wall on the same
                 * 16-unit grid the fractional-camera fix removed. The walk consumes
                 * the banks only when vfracPres matches this build. */
                if (C->vfracPres == s_vanim_pres) continue;
                if ((nLp && C->lpVprev && tsc_vanim_jump(C->lpVprev, C->lpV, nLp)) ||
                    (nHp && C->hpVprev && tsc_vanim_jump(C->hpVprev, C->hpV, nHp)))
                    continue;                        /* teleport/keyframe reset: snap to cur */
                int filled = 0;
                if (nLp && C->lpVprev) {
                    if (!C->lpVf) C->lpVf = (float*)malloc(sizeof(float) * 3u * (size_t)nLp);
                    if (C->lpVf) { tsc_vanim_lerp(C->lpVprev, C->lpV, nLp, s_vanim_alpha, C->lpVf); filled = 1; }
                }
                if (nHp && C->hpVprev) {
                    if (!C->hpVf) C->hpVf = (float*)malloc(sizeof(float) * 3u * (size_t)nHp);
                    if (C->hpVf) { tsc_vanim_lerp(C->hpVprev, C->hpV, nHp, s_vanim_alpha, C->hpVf); filled = 1; }
                }
                if (filled) { C->vfracPres = s_vanim_pres; dbg_filled++; }
                if (filled && s_vdbg) {
                    /* first vert (either bank) that actually moved this tick */
                    for (int bank = 0; bank < 2; bank++) {
                        const tsvert_t* P = bank ? C->hpVprev : C->lpVprev;
                        const tsvert_t* V = bank ? C->hpV : C->lpV;
                        const float* F = bank ? C->hpVf : C->lpVf;
                        int n = bank ? nHp : nLp;
                        if (!P || !V || !F) continue;
                        for (int v = 0; v < n; v++) {
                            if (P[v].x != V[v].x || P[v].y != V[v].y || P[v].z != V[v].z) {
                                fprintf(stderr, "[vanim f=%u pres=%u] s=%u a=%.3f %sV[%d] (%d,%d,%d)->(%d,%d,%d) lerp=(%.2f,%.2f,%.2f)\n",
                                        openpete_frame, s_vanim_pres, idx, s_vanim_alpha,
                                        bank ? "hp" : "lp", v,
                                        P[v].x, P[v].y, P[v].z, V[v].x, V[v].y, V[v].z,
                                        F[v*3], F[v*3+1], F[v*3+2]);
                                bank = 2; break;
                            }
                        }
                    }
                }
            }
        }
        dbg_seen = it.seen;
    }
    if (s_vdbg && dbg_seen)
        fprintf(stderr, "[vanim-scan f=%u sub=%d on=%d entries=%u desc=%d cached=%d prev=%d filled=%d]\n",
                openpete_frame, s_vanim_sub, s_vanim_on, dbg_seen, dbg_desc, dbg_cached, dbg_prev, dbg_filled);
}

/* Colour env-anim sub-tick interpolation. The colour sub-lists (Spyro 1
 * m_LowColorAnimations / m_HighColorAnimations, the Spyro 2 and Spyro 3 arena
 * sub-list at block+0x08) rewrite a sector's vertex colour words in place once
 * per tick: Spyro 1's world draw INTPLs the keyframe pair into them
 * (r_environment.s:80025D14-80025E58 LP, 80025FD8-80026148 HP near and far),
 * Spyro 3's func_80021FF4 writes them from the update. Every colour the walk
 * and the GPU-cache bake read comes from the sector's LP colour array and its
 * HP near and far arrays, so the mirror keeps a previous-tick and a
 * current-tick copy of those arrays per animated sector, rolled on the first
 * canonical build of a VBL (after the game's rewrite).
 *
 * The present's colour is canim_lerp(prev, cur, s_canim_a) per word, before
 * the fog blend. The walk reads it from the slot's host-side `mix` copy,
 * filled once per build when the alpha is below 1; the GPU cache bakes these
 * sectors into the anim arenas with the previous words in rhi_terrain_prev
 * and terrain.vert applies the same lerp at the build's alpha (globals
 * dspare.y). Both round each channel to nearest on the RGB888 grid the
 * terrain colour path carries, so walk and cache agree bit for bit, and
 * alpha 1 selects the current words exactly. A sector whose faces can take
 * a near subdivide claims to the walk (tgpu_canim_band): the cell and
 * split-triangle records carry the current near colours alone.
 * OPENPETE_TERRAIN_CANIM: 1 (default) lerps at the spine's alpha; 0 holds the
 * previous tick's words at every alpha, the leg the sub-tick harness
 * differences the default against. */
#define CANIM_SLOTS 256
typedef struct canim {
    uint32_t s;                   /* sector pointer; 0 = free slot */
    uint32_t lpBase, hpBase;      /* LP colour array; HP near array (far follows) */
    uint32_t nLp, nHp;            /* words: nLpC; 2 * nHpC */
    uint32_t* prev; uint32_t* cur;/* nLp + nHp words each */
    uint32_t* mix;                /* the walk's present colours (alpha < 1) */
    uint32_t rollPres;            /* s_vanim_pres of the canonical build that rolled it */
    int havePrev;
} canim_t;
static canim_t  s_canim[CANIM_SLOTS];
static uint32_t s_canim_canon_pres;   /* s_vanim_pres of the last canonical roll */
static int      s_canim_en = 1;       /* OPENPETE_TERRAIN_CANIM (re-extract only) */
static double   s_canim_a = 1.0;      /* this build's colour alpha (1 = current words) */

/** @brief Locates the three colour arrays of sector `s` from its header words +0x10 and +0x14.
 *
 * The LP colour array follows the LP vertices at s + 0x1C; the HP near
 * array follows the HP vertices, and the far array follows the near one, so
 * nHp counts both (2 * nHpC). The same decode tsc_get applies.
 * @param cpu The guest CPU state.
 * @param s The sector's address.
 * @param g Receives lpBase, hpBase, nLp and nHp. */
static void canim_geom(CPUState* cpu, uint32_t s, canim_t* g) {
    uint32_t w3 = rd(cpu, s + 0x10), w4 = rd(cpu, s + 0x14);
    g->nLp = (w3 >> 8) & 0xFFu;
    g->lpBase = s + 0x1Cu + (w3 & 0xFFu) * 4u;
    g->nHp = ((w4 >> 8) & 0xFFu) * 2u;
    g->hpBase = s + 0x1Cu + ((w4 >> 24) & 0xFFu) * 4u + (w4 & 0xFFu) * 4u;
}

/** @brief Releases a colour slot's three word buffers and marks the slot free.
 * @param A The slot; zeroed on return.
 */
static void canim_free(canim_t* A) {
    free(A->prev); free(A->cur); free(A->mix);
    memset(A, 0, sizeof(*A));
}

/** @brief Finds the colour slot of a sector, or the first free slot.
 *
 * @param s The sector's address.
 * @return The slot holding s, else the first free slot (s == 0), else NULL
 *         when the table of CANIM_SLOTS is full and s has none.
 */
static canim_t* canim_slot(uint32_t s) {
    canim_t* freeSlot = NULL;
    for (int i = 0; i < CANIM_SLOTS; i++) {
        if (s_canim[i].s == s) return &s_canim[i];
        if (!s_canim[i].s && !freeSlot) freeSlot = &s_canim[i];
    }
    return freeSlot;
}

/** @brief Copies a slot's sector colour words out of guest RAM.
 *
 * Writes the nLp LP words from lpBase, then the nHp HP words from hpBase.
 * @param cpu The guest CPU state.
 * @param A The slot.
 * @param out Receives nLp + nHp words.
 */
static void canim_read(CPUState* cpu, const canim_t* A, uint32_t* out) {
    for (uint32_t k = 0; k < A->nLp; k++) out[k] = rd(cpu, A->lpBase + k * 4u);
    for (uint32_t k = 0; k < A->nHp; k++) out[A->nLp + k] = rd(cpu, A->hpBase + k * 4u);
}

/** @brief Rolls one sector's colour slot on a canonical build: prev takes the last roll's words, cur the words in RAM now.
 *
 * A slot whose header geometry moved (a new level reusing the address) is
 * reallocated and restarts without a previous tick. A full table, a slot
 * already rolled this build, or a failed allocation leaves the sector
 * without a roll.
 * @param cpu The guest CPU state.
 * @param s The sector's address. */
static void canim_roll(CPUState* cpu, uint32_t s) {
    canim_t g; canim_geom(cpu, s, &g);
    canim_t* A = canim_slot(s);
    if (!A || A->rollPres == s_vanim_pres) return;   /* table full, or named twice */
    if (A->s != s || A->lpBase != g.lpBase || A->hpBase != g.hpBase
        || A->nLp != g.nLp || A->nHp != g.nHp) {
        canim_free(A);
        A->s = s; A->lpBase = g.lpBase; A->hpBase = g.hpBase;
        A->nLp = g.nLp; A->nHp = g.nHp;
        size_t n = (size_t)(A->nLp + A->nHp);
        A->prev = (uint32_t*)malloc(sizeof(uint32_t) * (n ? n : 1u));
        A->cur  = (uint32_t*)malloc(sizeof(uint32_t) * (n ? n : 1u));
        A->mix  = (uint32_t*)malloc(sizeof(uint32_t) * (n ? n : 1u));
        if (!A->prev || !A->cur || !A->mix) { canim_free(A); return; }
    } else {
        uint32_t* t = A->prev; A->prev = A->cur; A->cur = t;
        A->havePrev = 1;
    }
    canim_read(cpu, A, A->cur);
    A->rollPres = s_vanim_pres;
}

/** @brief The widest channel step of one slot's last roll, the census's engagement figure.
 *
 * @param A The slot.
 * @return The largest per-channel |cur - prev| over the slot's words; 0
 *         without a previous tick. */
static int canim_maxd(const canim_t* A) {
    int m = 0;
    if (!A->havePrev) return 0;
    for (uint32_t k = 0; k < A->nLp + A->nHp; k++)
        for (int sh = 0; sh < 24; sh += 8) {
            int d = abs((int)((A->cur[k] >> sh) & 0xFFu) - (int)((A->prev[k] >> sh) & 0xFFu));
            if (d > m) m = d;
        }
    return m;
}

/** @brief One colour word at `a` of the interval prev to cur, per channel, rounded to nearest.
 *
 * terrain.vert canim_col mirrors it. The top byte is cur's; a == 1 returns
 * cur exactly.
 * @param p The previous tick's word.
 * @param c The current tick's word.
 * @param a The alpha, 0..1.
 * @return The interpolated word. */
static uint32_t canim_lerp(uint32_t p, uint32_t c, double a) {
    if (a == 1.0) return c;
    uint32_t out = c & 0xFF000000u;
    for (int sh = 0; sh < 24; sh += 8) {
        double pv = (double)((p >> sh) & 0xFFu), cv = (double)((c >> sh) & 0xFFu);
        out |= (uint32_t)(int)(pv + (cv - pv) * a + 0.5) << sh;
    }
    return out;
}

/** @brief Finds the decode-cache slot holding a sector.
 *
 * Probes the TSC_WINDOW slots tsc_get places a sector in, starting at its
 * address hash.
 * @param s The sector's guest address.
 * @return The slot, or NULL when the sector is not cached. */
static tscache_t* tsc_find(uint32_t s) {
    uint32_t h = (((s >> 2) * 2654435761u) >> 14) & (TSC_SLOTS - 1);
    for (uint32_t w = 0; w < TSC_WINDOW; w++) {
        tscache_t* W = &s_tsc[(h + w) & (TSC_SLOTS - 1)];
        if (W->s == s) return W;
    }
    return NULL;
}

/** @brief Returns a decoded sector's colour-anim slot for this build.
 *
 * The slot is the one tsc_col_anim_refresh stamped for this build, and only
 * while its LP and HP colour bases and counts match the decode's.
 * @param SC The decoded sector.
 * @return The slot, or NULL for every other sector. */
static const canim_t* tsc_canim(const tscache_t* SC) {
    const canim_t* A = SC->canimPres == s_vanim_pres ? SC->canim : NULL;
    if (!A || A->lpBase != SC->lpColBase || A->hpBase != SC->nearBase
        || A->nLp != (uint32_t)SC->nLpC || A->nHp != 2u * (uint32_t)SC->nHpC) return NULL;
    return A;
}

/** @brief Returns the colour-anim slot whose mix words the walk reads, or NULL to read guest RAM.
 *
 * The slot is returned while this build's colour alpha is not 1 and the
 * slot holds a previous tick.
 * @param SC The decoded sector.
 * @return The slot, or NULL. */
static const canim_t* tsc_canim_mix(const tscache_t* SC) {
    const canim_t* A = tsc_canim(SC);
    return (A && A->havePrev && s_canim_a != 1.0) ? A : NULL;
}

/** @brief Rolls every sector the colour sub-lists name on a canonical build and frees the slots whose sector left them.
 *
 * Records this build as the last canonical roll (s_canim_canon_pres).
 * @param cpu The guest CPU state.
 * @param sectorPtr The sector pointer table; 0 names nothing.
 * @param sectorCnt The sector count.
 * @param census 1 to measure the widest channel step.
 * @param maxd Raised to the widest channel step seen (census only).
 * @return The count of colour entries named. */
static int canim_roll_all(CPUState* cpu, uint32_t sectorPtr, uint32_t sectorCnt,
                          int census, int* maxd) {
    env_anim_iter_t it;
    uint32_t idx; int kind, named = 0;
    env_anim_iter_init(&it, cpu, sectorCnt);
    while (sectorPtr && env_anim_iter_next(&it, &idx, &kind)) {
        uint32_t s = kind == ENV_ANIM_KIND_COLOUR ? rd(cpu, sectorPtr + idx * 4) : 0u;
        if (!s) continue;
        canim_roll(cpu, s);
        named++;
        const canim_t* A = census ? canim_slot(s) : NULL;
        int d = (A && A->s == s) ? canim_maxd(A) : 0;
        if (d > *maxd) *maxd = d;
    }
    for (int i = 0; i < CANIM_SLOTS; i++)
        if (s_canim[i].s && s_canim[i].rollPres != s_vanim_pres) canim_free(&s_canim[i]);
    s_canim_canon_pres = s_vanim_pres;
    return named;
}

/** @brief Stamps each slot of the last canonical roll on its decode-cache slot and fills its `mix` words.
 *
 * Runs on every build. A slot whose sector is not in the decode cache is
 * not stamped. `mix` is filled only while the alpha is below 1 and the slot
 * holds a previous tick.
 * @param a This build's colour alpha.
 * @return The slots stamped. */
static int canim_stamp_all(double a) {
    int stamped = 0;
    for (int i = 0; i < CANIM_SLOTS; i++) {
        canim_t* A = &s_canim[i];
        if (!A->s || A->rollPres != s_canim_canon_pres) continue;
        tscache_t* C = tsc_find(A->s);
        if (!C) continue;
        C->canim = A; C->canimPres = s_vanim_pres;
        stamped++;
        if (a == 1.0 || !A->havePrev) continue;
        for (uint32_t k = 0; k < A->nLp + A->nHp; k++)
            A->mix[k] = canim_lerp(A->prev[k], A->cur[k], a);
    }
    return stamped;
}

/** @brief Rolls the colour env-anim slots once per tick and stamps them for this build.
 *
 * On a canonical build (s_vanim_sub clear) the first of an openpete_frame
 * calls canim_roll_all; later canonical builds of the same tick (a portal
 * preview) skip the roll so prev stays the last tick. Sets s_canim_a to
 * s_vanim_alpha on a sub-tick build with a camera pair (0 when
 * OPENPETE_TERRAIN_CANIM disables the lerp), else 1, then calls
 * canim_stamp_all. Prints a [canim] line under OPENPETE_TERRAIN_VANIMDBG.
 * @param cpu The guest CPU state.
 * @param sectorPtr The sector pointer table's guest address; 0 rolls
 *        nothing.
 * @param sectorCnt The sector count. */
static void tsc_col_anim_refresh(CPUState* cpu, uint32_t sectorPtr, uint32_t sectorCnt) {
    static int s_cdbg = -1;
    ENV_FLAG_LATCH(s_cdbg, "OPENPETE_TERRAIN_VANIMDBG");
    /* One tick can run several canonical builds (a portal preview renders the
     * environment twice); only the first rolls, so prev stays the last tick. */
    static unsigned s_roll_frame = 0xFFFFFFFFu;
    int named = 0, maxd = 0;
    if (!s_vanim_sub && openpete_frame != s_roll_frame) {
        named = canim_roll_all(cpu, sectorPtr, sectorCnt, s_cdbg, &maxd);
        s_roll_frame = openpete_frame;
    }
    s_canim_a = (s_vanim_sub && s_vanim_on) ? (s_canim_en ? s_vanim_alpha : 0.0) : 1.0;
    int stamped = canim_stamp_all(s_canim_a);
    if (s_cdbg)
        fprintf(stderr, "[canim f=%u sub=%d on=%d en=%d a=%.3f named=%d stamped=%d maxd=%d]\n",
                openpete_frame, s_vanim_sub, s_vanim_on, s_canim_en, s_canim_a, named,
                stamped, maxd);
}

/* Texture (UV-scroll) env-anim sub-tick interpolation. The scrolling-texture list (`game_layout.env_anim.scroll`: Spyro 1's
 * g_EnvironmentAnimations +0x08/+0x0C walked by func_8002A6FC, Spyro 3's
 * 0x8006E350 +0x08/+0x0C walked by func_80020E74) keeps a scroller's ENTIRE
 * animation state in a 7-bit accumulator at desc+6: on a keyframe advance it
 * adds the keyframe's signed int8 delta, wraps to 0..127, and writes the
 * accumulator into the tiledef V bytes as whole texels - scroll>>2 into the LQ
 * entry (+1/+5/+9/+13, i.e. v0/v1 of BOTH LOD halves) and scroll>>1 into the HQ
 * sub-tiles, whose two groups sit half a scroll window apart (mod 64). Both
 * games write the same two quantities into the same V-byte roles; only where
 * the HQ sub-tiles live differs (Spyro 1's separate 21-entry record, Spyro 3's
 * four inline tiledefs at +0x10 of the 48-byte entry), so the offsets computed
 * here are geometry-independent and the emit sites place them. Those writes
 * land once per canonical tick, so tsc_tile_gens sees no change in between and
 * the cached texmats hold still while everything around them interpolates.
 *
 * Mirror the game's own registry (as tsc_vert_anim_refresh does for verts)
 * rather than diffing texture RAM. The prev/cur accumulator pair comes from the
 * shared per-object snapshot table (spyro_interp_capture_objs captures the
 * (tile, accumulator) pair at desc+4 once per canonical tick, keyed by the
 * descriptor address); here each descriptor is turned into a FRACTIONAL V offset
 * relative to the committed decode, which the two emit sites add to their corner
 * Vs. Rendered position is exactly lerp(prev,cur,alpha) in accumulator units -
 * the low bits the game's >>2 / >>1 truncated away are added back, which is why
 * an interpolating tile can sit up to 0.75 texel off the canonical whole-texel
 * position (sub-perceptual, and the direction the fractional-camera fix already
 * took). Everything else about the tiledef - CLUT, tpage, U, the HQ row offsets
 * - is left exactly as the game wrote it.
 *
 * The alpha is the SLOT'S OWN (from the pair's roll timestamps), not the camera
 * bracket's: a waterfall keeps flowing while the camera holds still, and the
 * camera ring dedupes a static camera down to no pair at all (freecam, standing
 * still) - the same reason the HUD icons and gem sparkles carry per-object
 * alphas. It also means a scroller that advances every N ticks sweeps smoothly
 * across its real advance interval rather than jumping once and holding.
 *
 * NOT handled here, deliberately: the tile-SWAP list (+0x00/+0x04) memcpys whole
 * tiledef entries (and, in Spyro 1, the sibling HQ record) between texture
 * descriptors. That is a discrete frame flip (supercharge ramps, portals), not
 * a continuous scroll - snapping it is faithful, and tsc_tile_gens /
 * tsc_refresh_anim already do. */
/* A keyframe-table reset (loop restart, level event) moves the accumulator
 * arbitrarily; a real scroll steps by one keyframe's int8 delta - a few units.
 * 32 of the 128 accumulator units splits the two by an order of magnitude each
 * way: above it, snap to the committed decode rather than sweep the texture
 * across the tile. */
#define TSCROLL_JUMP 32
/* Interval ceiling. A scroller's keyframe duration is 6 bits of 1/60 s, so a
 * legitimate advance interval reaches ~31 ticks; sweeping across all of it is
 * the smoothest reading of the animation. The cap only bounds the pathological
 * case - a scroller that PAUSES (descriptor flag bit1) and resumes much later
 * would otherwise pair across the whole pause and crawl up from a stale pose. */
#define TSCROLL_PERIOD_MAX (8 * g_openpete_tick_period_ns)
/* Presents are generated slightly ahead of the tick stamp they belong to, so the
 * first present of a cycle legitimately lands just before alpha 0 (the spine's
 * neg-alpha floor). Extrapolate rather than clamp, or ~a quarter of all motion
 * arrives as a snap. */
#define TSCROLL_ALPHA_MIN (-0.35)
/* One descriptor's sub-tick sweep. */
typedef struct {
    double   off;     /* accumulator units back toward prev: 0 at the tick */
    double   a;       /* sub-tick alpha the sweep landed on */
    int      d;       /* signed accumulator advance prev -> cur */
    double   per_ms;  /* the bracketed interval */
    uint32_t prv;     /* previous accumulator */
} tscroll_sweep_t;

/** @brief Fill *S with the accumulator offset from the committed `cur` to the one a sub-tick present shows.
 *
 * The descriptor is scroller `desc` (tile `ct`); the shown accumulator is the
 * snapshot pair's lerp(prev, curr, alpha). The snapshot's curr is the tick the
 * canonical present composed: the committed tick, equal to `cur`, or under
 * --runahead the led tick captured at the speculation head while the guest RAM
 * holding `cur` and the tiledef V bytes is the committed tick; the V offset is
 * linear in the accumulator, so the lead shifts the committed decode onto the
 * led one exactly.
 * @param desc the descriptor's guest address (the objs key)
 * @param ct   the descriptor's tile
 * @param cur  the committed 7-bit accumulator in guest RAM
 * @param now  the present clock
 * @param S    receives the offset and the sweep terms
 * @return 1 when the offset is nonzero or a sweep bracketed this present, 0
 *         leaving the caller on the committed position */
static int tsc_scroll_sweep(uint32_t desc, uint32_t ct, uint32_t cur,
                            op_time_t now, tscroll_sweep_t* S) {
    const openpete_interp_obj_t* o = openpete_interp_objs_find(desc);
    if (!o) return 0;
    /* blob = the descriptor's u16 tile at +4 then u16 accumulator at +6 */
    uint16_t ctile, ptile, ocur, prv;
    memcpy(&ctile, o->curr + 0, 2); memcpy(&ocur, o->curr + 2, 2);
    memcpy(&ptile, o->prev + 0, 2); memcpy(&prv, o->prev + 2, 2);
    if (ctile != ct) return 0;
    ocur &= 0x7Fu;
    /* Lead of the snapshot's near end over the committed accumulator, the
     * shortest way around the 7-bit ring. Nonzero only under --runahead. */
    int lead = ((((int)ocur - (int)cur) + 64) & 127) - 64;
    if (lead != 0 && (openpete_runahead_get() <= 0 ||
                      lead > TSCROLL_JUMP || lead < -TSCROLL_JUMP)) return 0;
    S->off = (double)lead;
    /* curr_ns == prev_ns means the snapshot clock stood still between the
     * two distinct accumulator states - there is no interval to sweep. It
     * happens under deterministic capture with a frozen camera (the headless
     * freecam legs): the deterministic object stamp is the direct ring's
     * tick_ns, and a static camera dedupes that ring so it never advances.
     * Windowed the stamp is the PLL virtual clock, which advances per tick
     * regardless of camera motion, so a still camera still scrolls. */
    if (!o->have_prev || o->curr_ns <= o->prev_ns || ptile != ctile) return lead != 0;
    prv &= 0x7Fu;
    /* Shortest path around the 7-bit ring: the accumulator wraps 127->0 mid
     * animation, and a raw difference there would read as a full-window jump. */
    int d = ((((int)ocur - (int)prv) + 64) & 127) - 64;
    if (d == 0 || d > TSCROLL_JUMP || d < -TSCROLL_JUMP) return lead != 0;
    op_time_t per = o->curr_ns - o->prev_ns;
    if (per > TSCROLL_PERIOD_MAX) per = TSCROLL_PERIOD_MAX;
    double a = (double)(now - o->curr_ns) / (double)per;
    if (a < TSCROLL_ALPHA_MIN) a = TSCROLL_ALPHA_MIN; else if (a > 1.0) a = 1.0;
    /* Rendered accumulator = lerp(prev,ocur,a) = cur + lead + (a-1)*d. */
    S->off = (double)lead + (a - 1.0) * (double)d;
    S->a = a; S->d = d; S->per_ms = (double)per / 1e6; S->prv = prv;
    return 1;
}

/** @brief Wraps a tile group's fractional V offset so its drawn V stays inside the scroll window.
 *
 * The offset moves by one window when base + dv would leave [0, win): the
 * sampler truncates toward zero, so a negative V fetches the wrong row, and
 * the content is periodic over the window.
 * @param base The group's whole-texel V base.
 * @param dv The fractional V offset.
 * @param win The scroll window, in texels.
 * @return The wrapped offset. */
static float tscroll_wrap(double base, double dv, double win) {
    if (base + dv < 0.0)       dv += win;
    else if (base + dv >= win) dv -= win;
    return (float)dv;
}

/** @brief Publishes one scroller descriptor's tile state for this build.
 *
 * Marks the tile live for the retained-GPU claim, bumps the tile generation
 * when the 7-bit accumulator advanced on a canonical build, and publishes the
 * tile's fractional V offsets: the LQ entry, the two HQ quadrant groups and
 * the four 4x4 cell rows. A descriptor naming a tile of 128 or above is
 * ignored.
 * @param cpu The guest CPU state.
 * @param desc The descriptor's guest address.
 * @param pair 1 to apply the sub-tick sweep toward the previous accumulator.
 * @param now The present clock.
 * @param dbg 1 to print a [tanim] line.
 * @return tsc_scroll_sweep's verdict when pair is set, else 0. */
static int tsc_scroll_desc(CPUState* cpu, uint32_t desc, int pair,
                           op_time_t now, int dbg) {
    uint32_t ct = rd(cpu, desc + 4) & 0xFFFFu;
    if (ct >= 128) return 0;
    s_tscroll_tile[ct] = 1; s_tscroll_have = 1;
    uint32_t cur = cpu->read_half(desc + 6) & 0x7Fu;
    /* Accumulator-change generation: the scroller advances the HQ sibling
     * record on ticks where the 16-byte LQ entry is byte-identical
     * (scroll>>2 unchanged), so tsc_tile_gens alone would leave a stale GPU
     * bake. Track the raw 7-bit accumulator and bump the tile generation on
     * every advance (canonical builds only - it moves at tick boundaries);
     * the re-decode is a no-op for the CPU path when the LQ bytes coincide,
     * and texGen rebuilds the bake. */
    static uint16_t s_tscroll_acc[128];
    if (!s_vanim_sub && s_tscroll_acc[ct] != (uint16_t)cur) {
        s_tscroll_acc[ct] = (uint16_t)cur;
        s_tgen[ct]++; s_tgen_epoch++;
    }
    tscroll_sweep_t S = { 0.0, 1.0, 0, 0.0, cur };
    int paired = pair ? tsc_scroll_sweep(desc, ct, cur, now, &S) : 0;
    /* Render the exact fractional accumulator by adding back the low bits the
     * game's >>2 / >>1 truncated away when it wrote the V bytes: LQ cur/4, HQ
     * cur/2. Every build carries that term, so a sub-tick present and the tick
     * it brackets place the same texel row and the scroll advances at a
     * constant rate instead of holding for four (LQ) or two (HQ) ticks and
     * stepping a whole texel. `off` is the sweep back toward the previous
     * accumulator and is nonzero only on a bracketed sub-tick present. The
     * window wrap keeps base+dv inside the scroll ring: the sampler truncates
     * toward zero (psx_sample.frag), so a negative V would fetch the wrong
     * row - and the content is periodic over the window by construction, since
     * the game's own 31->0 accumulator wrap would otherwise be a visible jump
     * every cycle. */
    double dvq = S.off * 0.25 + (double)(cur & 3u) * 0.25;
    s_tanim_dvlq[ct] = tscroll_wrap((double)(cur >> 2), dvq, 32.0);
    double dvh = S.off * 0.5 + (double)(cur & 1u) * 0.5;
    s_tanim_dvhq[ct][0] = tscroll_wrap((double)(cur >> 1), dvh, 64.0);                  /* HQ entries 1-2 */
    s_tanim_dvhq[ct][1] = tscroll_wrap((double)(((cur >> 1) + 32u) & 63u), dvh, 64.0);  /* HQ entries 3-4 (+0x20) */
    /* 4x4 cell row r: ((scroll >> 1) + 16*r) & 0x3F (r_environment.s
     * 0x8002A978-0x8002AA14); rows 0 and 2 share the quadrant groups' bases. */
    for (uint32_t r = 0; r < 4u; r++)
        s_tanim_dvcell[ct][r] = tscroll_wrap((double)(((cur >> 1) + 16u * r) & 63u), dvh, 64.0);
    s_tanim_act[ct] = 1;
    s_tanim_have = 1;
    if (dbg)
        fprintf(stderr, "[tanim f=%u pres=%u] tile=%u prev=%u cur=%u d=%d per=%.1fms "
                        "a=%.3f dvlq=%.3f dvhq=%.3f/%.3f paired=%d\n",
                openpete_frame, s_vanim_pres, ct, S.prv, cur, S.d,
                S.per_ms, S.a, s_tanim_dvlq[ct], s_tanim_dvhq[ct][0], s_tanim_dvhq[ct][1], paired);
    return paired;
}

/** @brief Publishes the tile state of every UV-scroll descriptor for this build.
 *
 * Clears the previous build's s_tanim_* and s_tscroll_* state. When the
 * layout locates env_anim.scroll, reads its count and list base (a base of
 * 0, or a count of 0 or above 1024, publishes nothing) and calls
 * tsc_scroll_desc on each nonzero descriptor. The sweep toward the previous
 * accumulator is enabled on a sub-tick build with s_tanim_en and a positive
 * present clock. Prints a [tanim-scan] line under OPENPETE_TERRAIN_TANIMDBG.
 * @param cpu The guest CPU state. */
static void tsc_scroll_refresh(CPUState* cpu) {
    static int s_tdbg = -1;
    ENV_FLAG_LATCH(s_tdbg, "OPENPETE_TERRAIN_TANIMDBG");
    s_tanim_have = 0;
    memset(s_tanim_act, 0, sizeof(s_tanim_act));
    s_tscroll_have = 0;
    memset(s_tscroll_tile, 0, sizeof(s_tscroll_tile));
    int dbg_desc = 0, dbg_paired = 0;
    {
    if (!game_layout.env_anim.scroll) goto scan;     /* no scroll list located */
    uint32_t cnt  = rd(cpu, game_layout.env_anim.scroll);
    uint32_t base = rd(cpu, game_layout.env_anim.scroll + 4);
    if (!base || cnt == 0 || cnt > 1024) goto scan;
    /* The fractional position is published on every build; only the sweep
     * toward the previous accumulator needs a bracketed sub-tick present. */
    int pair = (s_vanim_sub && s_tanim_en);
    op_time_t now = pair ? openpete_interp_present_now() : 0;
    if (pair && now <= 0) pair = 0;
    for (uint32_t k = 0; k < cnt; k++) {
        uint32_t desc = rd(cpu, base + k * 4);
        if (!desc) continue;
        dbg_desc++;
        dbg_paired += tsc_scroll_desc(cpu, desc, pair, now, s_tdbg);
    }
    }
scan:
    if (s_tdbg)
        fprintf(stderr, "[tanim-scan f=%u pres=%u sub=%d en=%d] descs=%d paired=%d\n",
                openpete_frame, s_vanim_pres, s_vanim_sub, s_tanim_en, dbg_desc, dbg_paired);
}

/** @brief Re-decodes the materials of HP faces whose tile generation moved, and tracks their HQ sibling generations.
 *
 * @param cpu The guest CPU state.
 * @param F0 The faces.
 * @param n The face count.
 * @param tbl The texture table the faces decode from.
 * @param hiDetail The hi-detail flag passed to terrain_texmat.
 * @return 1 when any face's material or HQ generation changed, else 0. */
static int tsc_refresh_faces(CPUState* cpu, tsface_t* F0, int n, uint32_t tbl, int hiDetail) {
    int changed = 0;
    for (int f = 0; f < n; f++) {
        tsface_t* F = &F0[f];
        /* HQ sibling liveness: the scroll accumulator gen for the record id
         * (which can differ from tb). The near path decodes the record live
         * per emit, so nothing re-decodes here - but the GPU bake reads it
         * at build time and must go stale (texGen) when it advances. */
        if (F->quad && !tface_near_excl(F->w3)) {
            uint32_t gh = s_tgen[F->hq_idx & 0x7F];
            if (F->tgenHq != gh) { F->tgenHq = gh; changed = 1; }
        }
        if (F->tb == -1) continue;
        uint32_t g = s_tgen[F->tb & 0x7F];
        if (F->tgen == g) continue;
        F->tgen = g;
        changed = 1;
        terrain_texmat(cpu, tbl, F->tb, hiDetail, &F->mat, F->tu, F->tv);
        terrain_texmat(cpu, tbl, F->tb, 1, &F->matN, F->tuN, F->tvN);
    }
    return changed;
}

/** @brief Re-decodes a cached sector's texture materials whose tile content changed under the env texture anim.
 *
 * Returns at once when no tile generation changed since the sector's last
 * refresh (s_tgen_epoch). Each HP face whose tile generation advanced is
 * re-decoded; a quad's HQ record generation is tracked without a re-decode,
 * since the near path decodes the record per emit. LP faces are untextured
 * and skipped. Any change bumps texGen,
 * which marks the sector's GPU bake stale.
 * @param cpu The guest CPU state.
 * @param C The cached sector.
 * @param lqTable The LQ texture table's guest address.
 * @param hqTable The HQ texture table's guest address.
 * @param texTblMode 2 to decode from the HQ table, else the LQ table.
 * @param hiDetail The hi-detail flag passed to terrain_texmat. */
static void tsc_refresh_anim(CPUState* cpu, tscache_t* C,
                             uint32_t lqTable, uint32_t hqTable,
                             int texTblMode, int hiDetail) {
    if (C->virt || C->animEpoch == s_tgen_epoch) return;   /* an import carries no animation */
    C->animEpoch = s_tgen_epoch;
    if (!C->hpF) return;                     /* LP faces are untextured */
    uint32_t tbl = (texTblMode == 2) ? hqTable : lqTable;
    int changed = tsc_refresh_faces(cpu, C->hpF, C->nHpF, tbl, hiDetail);
    /* GPU-cache liveness: baked materials (incl. the HQ sub-quad tiledefs,
     * which the env anim rewrites in the sibling record) went stale. */
    if (changed) C->texGen++;
}

/** @brief Reads one unaligned 32-bit word from a host copy of guest memory, in host byte order.
 * @param p The first byte.
 * @return The word. */
static inline uint32_t t_le32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
/** @brief Returns a terrain vertex colour read through host pointers, fogged like terrain_color.
 *
 * An LP (noHP) colour is returned raw; an HP colour is the terrain_fog_blend
 * of its near and far colours. The fog debug print stays on the guest-read
 * variant terrain_color only.
 * @param noHP 1 to read the LP colour unfogged.
 * @param lp The LP colour bank.
 * @param nearp The HP near colour bank.
 * @param farp The HP far colour bank.
 * @param idx The byte offset of the vertex's colour word.
 * @param m3 The vertex's view-Z.
 * @param fogRef The fog reference depth.
 * @return The colour, 0x00BBGGRR. */
static uint32_t terrain_color_h(int noHP, const uint8_t* lp, const uint8_t* nearp,
                                const uint8_t* farp, int idx, double m3, int fogRef) {
    if (noHP) return t_le32(lp + idx) & 0xFFFFFFu;
    uint32_t nearC = t_le32(nearp + idx) & 0xFFFFFFu;
    uint32_t farC  = t_le32(farp  + idx) & 0xFFFFFFu;
    return terrain_fog_blend(nearC, farC, m3, fogRef);
}

/* Walk every VISIBLE sector (D_800771C8[i]==0xFF - the .s's own canonical
 * visible set, isolating decode+projection+faces from the cull), project its HP
 * vertices (LP for 0x4000 "no-HP" sectors), then walk the HP faces (16B) / LP
 * faces (8B) into triangles. Camera-relative GTE-vector per r_environment.s
 * 0x80026238-0x800262C0 (matrix = g_Camera+0x00; world axes = origin+field). */
/* OPENPETE_TERRAIN_VISDBG: falsify the widescreen candidate set's correctness
 * invariant. Prints ONCE (once loaded): the .s's canonical 4:3 visible count
 * (D_800771C8==0xFF) vs the occlusion candidate set, and how many visible
 * sectors are MISSING from the candidate set. The candidate set must be a
 * SUPERSET of the 4:3 visible set - if missing>0 the occlusion-group walk is
 * wrong (offset/format) and widescreen would gap even at 4:3. Display-only. */
static int s_visdbg = -1;
/** @brief Prints once, after frame 1800, how many visible sectors the candidate set misses.
 *
 * Marks cand[0..cn) in a local table, counts the sectors below
 * min(sectorCnt, 1024) that tvis_visible reports visible and that are not
 * marked, and writes one [terrain-vis] line to stderr. The first call at
 * openpete_frame >= 1800 latches the function off; earlier calls return.
 * Reads only.
 * @param group The occlusion group the game's renderer received; printed.
 * @param occCnt The occlusion group count; printed.
 * @param cand The candidate sector indices; values of 1024 or more are
 *        ignored.
 * @param cn The entry count of cand.
 * @param n The admitted sector count; printed.
 * @param ws The wide-view flag; printed. */
static void terrain_visdbg(CPUState* cpu, uint32_t sectorCnt, int group,
                           uint32_t occCnt, const uint16_t* cand, int cn, int n, int ws) {
    static int s_done = 0;
    if (s_done || openpete_frame < 1800u) return;
    s_done = 1;
    static char inset[1024]; memset(inset, 0, sizeof(inset));
    for (int j = 0; j < cn; j++) if (cand[j] < 1024) inset[cand[j]] = 1;
    int vis43 = 0, missing = 0;
    for (uint32_t i = 0; i < sectorCnt && i < 1024; i++) {
        if (tvis_visible(cpu, i)) { vis43++; if (!inset[i]) missing++; }
    }
    fprintf(stderr,
        "[terrain-vis] frame=%u group=%d occCnt=%u candidates=%d  vis4:3=%d "
        "missingFromCandidates=%d  n=%d (sectors=%u ws=%d)\n",
        openpete_frame, group, occCnt, cn, vis43, missing, n, sectorCnt, ws);
}

/* Build the set of sector indices to render this frame.
 *
 * Native 4:3: use the .s's own canonical visibility verdict D_800771C8
 * (== occlusion sublist ∩ the .s 4:3 frustum cull) directly - pixel-identical
 * to the .s visible set, zero risk.
 *
 * WIDESCREEN: the .s verdict is too narrow - its 4:3 frustum cull dropped the
 * side sectors the wider frustum now reveals (the terrain sibling of the
 * cyclorama wide-AR gap). We re-derive the candidate set
 * host-side from the SAME static, aspect/direction-INDEPENDENT occlusion data
 * the .s starts from: g_Camera.m_OcclusionGroup selects a baked PVS sublist in
 * g_Environment.m_OcclusionGroups (a conservative, direction-independent
 * superset for the camera's region), or ALL sectors when the group is invalid
 * (mirrors func_8002B9CC's group selection). The near-plane clip + the
 * full-window GPU scissor + the Z-buffer then confine and depth-sort the set.
 *
 * DISPLAY-ONLY: reads g_Camera/g_Environment read-only, never writes
 * D_800771C8 / m_WasDrawn (the canonical render->gameplay channel) - oracle-safe
 * by construction. The candidate set is a SUPERSET of the 4:3 visible set
 * (verified by OPENPETE_TERRAIN_VISDBG: every D_800771C8==0xFF sector is in the
 * sublist), so widescreen only ADDS side sectors, never drops a visible one. A
 * per-sector wide-frustum prune of this candidate set (for the 480fps /
 * --no-lod passes) is not implemented.
 *
 * `group` is the occlusion-group ARGUMENT the .s renderer (func_800258F0)
 * received this frame (captured from cpu->a0 at the hook, BEFORE the real call
 * clobbers it) - already clamped by func_8002B9CC to a valid index or -1. It is
 * the exact input the .s used to build D_800771C8, so it's the ground truth
 * even for the in-level/overlay callers that bypass func_8002B9CC (which a
 * re-derivation from g_Camera.m_OcclusionGroup would miss). */
/* The projection view of the current terrain pass (camera matrix and
 * position, GTE offsets, canonical and effective H, Hor+ bounds). Built once
 * at terrain_build entry; the feed that follows (rim frames, portal planes,
 * the conform census) reads the same view, so the plane fit and the frames
 * derived from it share one H. */
static ngte_view s_view;

/** @brief Tests a sector's bounding sphere for frustum rejection, as environment.s 800224E0-80022548 does.
 *
 * The three comparisons are behind the near plane, and outside the
 * horizontal and vertical half-angles, the second and third widened by a
 * radius-proportional slack the draw builds by shifts. The inputs are
 * saturated to 16 bits first, as the GTE saturates IR1..IR3 before the draw
 * reads them back.
 *
 * The draw's slack is sized for its own 4:3 tangents (4·t3 + 3·t4 ≈ 4.9r
 * against the exact 5r): scaled by a half-width above 1 it falls further
 * short of the sphere's true distance to the side plane (7.87r against
 * 8.94r at wx 2.6667), and rejects spheres that reach into the displayed
 * frame. An axis whose scale is not 1 therefore rejects on the exact plane
 * distance, 4|vx| − 3·wx·vz ≥ r·√(16 + 9·wx²) and
 * 32|vy| − 17·wy·vz ≥ r·√(1024 + 289·wy²), which admits every sphere the
 * displayed frustum intersects; an axis at scale 1 keeps the draw's
 * expression bit-exact, so the 4:3 verdict is the game's.
 * @param c1m The sphere centre's horizontal view coordinate.
 * @param c2m The sphere centre's vertical view coordinate.
 * @param cbin The sphere centre's view depth.
 * @param r The sphere radius.
 * @param wx The horizontal half-width scale: 1.0 is the 4:3 frame the draw
 *           tests, above 1.0 the displayed frame.
 * @param wy The vertical half-width scale, likewise.
 * @return Nonzero when the sphere is rejected. */
static int tsec_sphere_cull(double c1m, double c2m, double cbin, int r,
                            double wx, double wy) {
    double vx = ngte_ir_clamp(c1m), vy = ngte_ir_clamp(c2m), vz = ngte_ir_clamp(cbin);
    if (vz + r <= 0) return 1;
    int outX, outY;
    if (wx == 1.0) {
        double t3 = (r >> 1) + (r >> 2) + (r >> 5), t4 = (r >> 1) + (r >> 4) + (r >> 5);
        outX = 4.0 * (fabs(vx) - t3) >= 3.0 * wx * (vz + t4);
    } else {
        outX = 4.0 * fabs(vx) - 3.0 * wx * vz >= (double)r * sqrt(16.0 + 9.0 * wx * wx);
    }
    if (wy == 1.0) {
        double t5 = r - (r >> 3), t6 = (r >> 1) - (r >> 4);
        outY = 32.0 * (fabs(vy) - t5) >= 17.0 * wy * (vz + t6);
    } else {
        outY = 32.0 * fabs(vy) - 17.0 * wy * vz >= (double)r * sqrt(1024.0 + 289.0 * wy * wy);
    }
    return outX || outY;
}

/* Sector bounding-sphere frustum inputs: the sector table, the draw's
 * sector-binning matrix and camera, and the half-width scales of the displayed
 * frame (wx carries the Hor+ factor, both carry the --fov scale). NULL where
 * the game layout names no sector-header sphere (radius_mask == 0). */
typedef struct {
    uint32_t sectorPtr;
    int      M[3][3];
    double   camx, camy, camz;
    double   wx, wy;
} tvis_sphere;

/** @brief Tests whether a sector's header bounding sphere passes the frustum test at the displayed half-widths.
 *
 * Header word 0 is centreX << 16 | centreY, word 1 is
 * centreZ << 16 | flags | radius. The centre is transformed through the
 * draw's sector-binning matrix and tested by tsec_sphere_cull.
 * @param cpu The guest CPU state.
 * @param S The sphere-test inputs.
 * @param i The sector index.
 * @return 1 when the sphere passes, 0 when it is rejected or the sector
 *         pointer is null. */
static int tvis_sphere_in(CPUState* cpu, const tvis_sphere* S, uint32_t i) {
    uint32_t sp = rd(cpu, S->sectorPtr + i * 4);
    if (!sp) return 0;
    uint32_t w0 = rd(cpu, sp + 0x00), w1 = rd(cpu, sp + 0x04);
    double cx = (double)(int)(w0 >> 16), cy = (double)(int)(w0 & 0xFFFFu),
           cz = (double)(int)(w1 >> 16);
    double c1m, c2m, cbin;
    ngte_mac(S->M, S->camy - cy, S->camz - cz, cx - S->camx, &c1m, &c2m, &cbin);
    return !tsec_sphere_cull(c1m, c2m, cbin, thdr_radius(w1), S->wx, S->wy);
}

/** @brief Appends the imported sectors that pass the frustum sphere test to the walk's visible list.
 * @param cpu The guest CPU state.
 * @param S The sphere-test inputs, or NULL to admit every imported sector.
 * @param guestCnt The scene sector count (the first imported index).
 * @param sectorCnt The extended sector count.
 * @param visidx The visible list (1024 entries).
 * @param nvis The list's current count.
 * @return The new count. */
static int tsv_append_vis(CPUState* cpu, const tvis_sphere* S, uint32_t guestCnt, uint32_t sectorCnt,
                          uint16_t* visidx, int nvis) {
    for (uint32_t i = guestCnt; i < sectorCnt && nvis < 1024; i++)
        if (!S || tvis_sphere_in(cpu, S, i)) visidx[nvis++] = (uint16_t)i;
    return nvis;
}

static int s_vis_engage;   /* the BINCHECK window is open on this walk */
/** @brief Fills out with the sector indices this walk draws and returns their count.
 *
 * At most min(sectorCnt, 1024) sectors are considered, so out holds 1024
 * entries. The first mode that applies decides the set: OPENPETE_TERRAIN_ALLVIS
 * admits every sector; with S set and the terrain visibility mode frustum, the
 * sectors whose header sphere passes tvis_sphere_in; with a view wider than
 * 4:3 (s_view.extended) and a layout whose verdict has no aspect term
 * (env.vis_aspect_independent), the verdict path below; with a wider view
 * otherwise, the occlusion group's 0xFF-terminated byte list (2048 bytes at
 * most), or every sector when the group names no list; else the game's own
 * verdict through tvis_visible. Sets s_vis_engage from
 * OPENPETE_TERRAIN_BINCHECK and prints the admitted count on its window.
 * Reads guest RAM only.
 * @param cpu The guest CPU state.
 * @param sectorCnt The level's sector count.
 * @param out Receives the admitted sector indices.
 * @param group The occlusion group the game's renderer received; outside
 *        0..occ_group_count-1 names no list.
 * @param S The sphere-test inputs, or NULL to disable the frustum mode.
 * @return The number of indices written to out. */
static int terrain_vislist(CPUState* cpu, uint32_t sectorCnt, uint16_t* out, int32_t group,
                           const tvis_sphere* S) {
    if (sectorCnt > 1024) sectorCnt = 1024;
    int ws = s_view.extended;   /* Hor+ or a wider FOV: the 4:3 verdict is too narrow */
    ENV_FLAG_LATCH(s_visdbg, "OPENPETE_TERRAIN_VISDBG");
    {   /* OPENPETE_TERRAIN_BINCHECK=<frame>: engagement window, the two
         * presents from that frame. Every candidate-set mode reports the
         * sector count it admitted on those presents. */
        static int s_binchk = -2;
        ENV_INT_LATCH(s_binchk, "OPENPETE_TERRAIN_BINCHECK", 0);
        s_vis_engage = (s_binchk && openpete_frame >= (unsigned)s_binchk
                        && openpete_frame < (unsigned)s_binchk + 2u);
    }
    {   /* OPENPETE_TERRAIN_ALLVIS=1: walk ALL sectors (bypass the visibility verdict /
         * occlusion groups) - A/B probe for view-dependent sector culling. */
        static int s_allvis = -1;
        ENV_FLAG_LATCH(s_allvis, "OPENPETE_TERRAIN_ALLVIS");
        if (s_allvis) {
            int n = 0;
            for (uint32_t i = 0; i < sectorCnt && n < 1024; i++) out[n++] = (uint16_t)i;
            if (s_vis_engage) {
                fprintf(stderr, "[terrain-vis] frame=%u mode=allvis admitted=%d of %u\n",
                        openpete_frame, n, sectorCnt);
                s_vis_engage = 0;
            }
            return n;
        }
    }

    /* --terrain-visibility frustum (default): the candidate set is every
     * sector whose bounding sphere passes the frustum test at the displayed
     * half-widths, independent of the game's visibility verdict and its
     * occlusion groups, so geometry the game hides - sectors behind the walls
     * of the room the camera sits in, sectors lying only in the side wedges of
     * a frame wider than 4:3 - is drawn and the depth buffer decides what is
     * seen. A deliberate departure from the original presentation. The
     * verdict is not read in this mode; the admitted set is culled downstream
     * exactly as any other sector (per-sector tier verdicts, the retained GPU
     * cache, the native occlusion pre-pass). `--terrain-visibility game` is
     * the faithful path. */
    if (S && openpete_native_get_terrain_visibility() == OPENPETE_TERRAIN_VIS_FRUSTUM) {
        int n = 0;
        for (uint32_t i = 0; i < sectorCnt && n < 1024; i++) {
            if (tvis_sphere_in(cpu, S, i)) out[n++] = (uint16_t)i;
        }
        if (s_vis_engage) {
            fprintf(stderr, "[terrain-vis] frame=%u mode=frustum admitted=%d of %u wx=%.4f wy=%.4f\n",
                    openpete_frame, n, sectorCnt, S->wx, S->wy);
            s_vis_engage = 0;
        }
        if (s_visdbg) terrain_visdbg(cpu, sectorCnt, group, 0, out, n, n, ws);
        return n;
    }

    /* The occlusion-group candidate list (Spyro 1: a table of 0xFF-terminated
     * sector index lists, one per group, selected by the draw's argument). A
     * game whose layout names no group count keeps its occlusion data in
     * another shape, and the wide candidate set is then every sector. */
    uint32_t occCnt    = game_layout.env.occ_group_count ? tenv_rd(cpu, game_layout.env.occ_group_count) : 0;
    uint32_t occGroups = occCnt ? tenv_rd(cpu, game_layout.env.occ_groups) : 0;
    uint32_t list = (group >= 0 && (uint32_t)group < occCnt && occGroups)
                    ? rd(cpu, occGroups + (uint32_t)group * 4) : 0;

    /* A game whose verdict carries no aspect term is already the wide
     * candidate set (env.vis_aspect_independent): widening it would re-admit
     * the sectors the game's own visibility pass hides. Engagement, under
     * OPENPETE_TERRAIN_BINCHECK: the sectors the verdict drops out of the wide
     * candidate set on this walk. */
    if (ws && game_layout.env.vis_aspect_independent) {
        if (s_vis_engage) {
            int cand = 0, dropped = 0;
            if (list) {
                for (uint32_t k = 0; k < 2048; k++) {
                    uint8_t idx = cpu->read_byte(list + k);
                    if (idx == 0xFF) break;
                    if (idx >= sectorCnt) continue;
                    cand++;
                    if (!tvis_visible(cpu, idx)) dropped++;
                }
            } else {
                for (uint32_t i = 0; i < sectorCnt; i++) {
                    cand++;
                    if (!tvis_visible(cpu, i)) dropped++;
                }
            }
            fprintf(stderr, "[vis-aspect-indep] frame=%u group=%d wideCandidates=%d "
                            "dropped=%d kept=%d\n",
                    openpete_frame, group, cand, dropped, cand - dropped);
        }
        ws = 0;
    }

    if (ws) {
        int n = 0;
        if (list) {
            for (uint32_t k = 0; k < 2048 && n < 1024; k++) {
                uint8_t idx = cpu->read_byte(list + k);
                if (idx == 0xFF) break;
                if (idx < sectorCnt) out[n++] = idx;
            }
        } else {
            for (uint32_t i = 0; i < sectorCnt && n < 1024; i++) out[n++] = (uint16_t)i;
        }
        if (s_vis_engage) {
            fprintf(stderr, "[terrain-vis] frame=%u mode=game(occ-group) admitted=%d of %u\n",
                    openpete_frame, n, sectorCnt);
            s_vis_engage = 0;
        }
        if (s_visdbg) terrain_visdbg(cpu, sectorCnt, group, occCnt, out, n, n, ws);
        return n;
    }

    /* native 4:3 - the canonical .s verdict */
    int n = 0;
    for (uint32_t i = 0; i < sectorCnt; i++) {
        if (tvis_visible(cpu, i)) out[n++] = (uint16_t)i;
    }
    if (s_vis_engage) {
        fprintf(stderr, "[terrain-vis] frame=%u mode=game admitted=%d of %u\n",
                openpete_frame, n, sectorCnt);
        s_vis_engage = 0;
    }
    if (s_visdbg) {   /* validate the invariant without altering the 4:3 render set */
        static uint16_t cand[1024]; int cn = 0;
        if (list) {
            for (uint32_t k = 0; k < 2048 && cn < 1024; k++) {
                uint8_t idx = cpu->read_byte(list + k);
                if (idx == 0xFF) break;
                if (idx < sectorCnt) cand[cn++] = idx;
            }
        } else for (uint32_t i = 0; i < sectorCnt && cn < 1024; i++) cand[cn++] = (uint16_t)i;
        terrain_visdbg(cpu, sectorCnt, group, occCnt, cand, cn, n, ws);
    }
    return n;
}

/* OPENPETE_TERRAIN_ANIMDBG (keeper instrument): per-present RAM-change census
 * for env anim. Hashes each LQ tiledef entry (16B), each HQ record (168B), and
 * per sector: the +0x18 anim word, vert regions, colour regions. Prints indices
 * that changed since the previous present. Proved the waterfall freeze (only
 * lq/hq tile 23 churns in artisans; secV/secC never -> verts safe to cache). */
/* ---- texture-record dumper (HD-pack matcher input) ------------------------ */

int stbi_write_png(const char *filename, int w, int h, int comp,
                   const void *data, int stride_in_bytes);   /* impl: screenshot.c TU */

/** @brief Samples one 32x32 tile out of the native VRAM model into an RGBA8 canvas at (dx, dy).
 *
 * The CLUT is the material's base row (no fog row). Texel decode and alpha
 * encoding are identical to texture_pack_dump_draw: 4, 8 or 16 bpp, 5-bit
 * channels widened by bit replication, 0x0000 transparent, a set STP bit
 * alpha 128, else 255.
 * @param vr The VRAM model, OPENPETE_VRAM_W halfwords per row.
 * @param m The tile's material (page, CLUT, bpp).
 * @param u0 The tile's U origin within the page.
 * @param v0 The tile's V origin within the page.
 * @param dst The RGBA8 canvas.
 * @param dst_w The canvas width in pixels.
 * @param dx The destination X.
 * @param dy The destination Y. */
static void texrec_blit32(const uint16_t* vr, const tmat* m, int u0, int v0,
                          unsigned char* dst, int dst_w, int dx, int dy) {
    const uint16_t* clut = vr + (size_t)m->cly * OPENPETE_VRAM_W + m->clx;
    for (int y = 0; y < 32; y++) {
        const uint16_t* row = vr + (size_t)(m->tpy + v0 + y) * OPENPETE_VRAM_W;
        for (int x = 0; x < 32; x++) {
            int uu = u0 + x;
            uint16_t p;
            if (m->bpp == 16)     p = row[m->tpx + uu];
            else if (m->bpp == 8) p = clut[(row[m->tpx + (uu >> 1)] >> (8 * (uu & 1))) & 0xFFu];
            else                  p = clut[(row[m->tpx + (uu >> 2)] >> (4 * (uu & 3))) & 0xFu];
            unsigned char* o = dst + ((size_t)(dy + y) * dst_w + (size_t)(dx + x)) * 4;
            unsigned r5 = p & 31u, g5 = (p >> 5) & 31u, b5 = (p >> 10) & 31u;
            o[0] = (unsigned char)((r5 << 3) | (r5 >> 2));
            o[1] = (unsigned char)((g5 << 3) | (g5 >> 2));
            o[2] = (unsigned char)((b5 << 3) | (b5 >> 2));
            o[3] = (p == 0) ? 0 : ((p & 0x8000u) ? 128 : 255);
        }
    }
}

/** @brief Decodes one record Tiledef entry and reduces it to the tile's stored VRAM box origin.
 *
 * The entry is decoded as opaque, its orientation applied, and the origin
 * taken as the minimum U and V over the effective corner UVs: a rotated tile
 * still occupies one 32x32 box; the orientation deltas only permute which
 * corner each spatial vertex samples.
 * @param cpu The guest CPU state.
 * @param e The Tiledef entry's guest address.
 * @param m Receives the material.
 * @param bu Receives the box's U origin.
 * @param bv Receives the box's V origin.
 * @return The orientation index. */
static int texrec_entry(CPUState* cpu, uint32_t e, tmat* m, int* bu, int* bv) {
    float u[4], v[4];
    memset(m, 0, sizeof(*m)); m->bpp = 16;
    decode_tiledef(cpu, e, 0, m, u, v);
    int ori = tiledef_apply_ori(cpu, e, u, v);
    float mu = u[0], mv = v[0];
    for (int k = 1; k < 4; k++) {
        if (u[k] < mu) mu = u[k];
        if (v[k] < mv) mv = v[k];
    }
    *bu = (int)mu; *bv = (int)mv;
    return ori;
}

/** @brief Writes one Tiledef entry's JSON member: page and UV material plus the resolved pack identity.
 *
 * The identity is the (thash, box, phash) the replacement lookup keys on,
 * so the offline pack builder can name files without a manifest join; an
 * entry texture_pack_resolve cannot resolve writes "id":null. The member is
 * written with a leading comma.
 * @param cpu The guest CPU state.
 * @param f The output file.
 * @param key The member name.
 * @param e The Tiledef entry's guest address. */
static void texrec_json(CPUState* cpu, FILE* f, const char* key, uint32_t e) {
    tmat m; int bu, bv;
    int ori = texrec_entry(cpu, e, &m, &bu, &bv);
    fprintf(f, ",\"%s\":{\"tpx\":%d,\"tpy\":%d,\"u\":%d,\"v\":%d,\"bpp\":%d,\"clut\":%d,\"ori\":%d",
            key, m.tpx, m.tpy, bu, bv, m.bpp, m.clut_raw, ori);
    texture_pack_id id;
    if (texture_pack_resolve(m.tpx, m.tpy, m.clx, m.cly, m.bpp,
                             bu, bv, bu + 31, bv + 31, &id))
        fprintf(f, ",\"id\":{\"thash\":\"%016llx\",\"phash\":\"%016llx\","
                   "\"bx\":%d,\"by\":%d,\"bw\":%d,\"bh\":%d}",
                (unsigned long long)id.thash, (unsigned long long)id.phash,
                id.box_x, id.box_y, id.box_w, id.box_h);
    else
        fprintf(f, ",\"id\":null");
    fprintf(f, "}");
}

/** @brief Dumps the level's texture table as logical textures for the HD-pack matcher (OPENPETE_TEXPACK_RECORDS=<path>).
 *
 * Per tile index N (LQ tile index == HQ record index == a face's word 2 &
 * 0x7F, at most 128), writes its LQ far and near Tiledefs and the four
 * HQ-record quadrant Tiledefs (m_HQTexturePointer + N*168, entries
 * (i+1)*8 = TL, TR, BL, BR), each decoded to its VRAM page box, within-page
 * (u, v), bpp, CLUT and resolved replacement identity. With
 * OPENPETE_TEXPACK_RECORDS_DIR also set, each record's pixels are sampled
 * from the native VRAM model, the texels the renderer binds, as
 * recNN-hq.png (64x64, the four quadrants composited), recNN-lq.png (32x32
 * near) and recNN-lqfar.png (32x32 far). Re-dumps, overwriting, whenever a
 * hash of the table pointers and count changes: every level load (title
 * screen included) allocates fresh tables, so the last dump describes the
 * level currently loaded. Returns without writing while a table is not
 * loaded; an unset variable or an unopenable path disables the dump for the
 * process.
 * @param cpu The guest CPU state. */
static void terrain_texrecords(CPUState* cpu) {
    static int off = 0;
    static uint32_t sig = 0;
    if (off) return;
    const char* path = getenv("OPENPETE_TEXPACK_RECORDS");
    if (!path || !*path) { off = 1; return; }
    const char* dir = getenv("OPENPETE_TEXPACK_RECORDS_DIR");
    uint32_t lqTable = tenv_rd(cpu, game_layout.env.lq_tex);
    uint32_t hqTable = tenv_rd(cpu, game_layout.env.hq_tex);
    uint32_t texCnt  = tenv_rd(cpu, game_layout.env.tex_count);
    if (!lqTable || !hqTable || !texCnt) return;   /* not loaded yet - retry next frame */
    uint32_t s = lqTable ^ (hqTable * 2654435761u) ^ (texCnt << 24);
    if (s == sig) return;                          /* this level already dumped */
    sig = s;
    if (texCnt > 128) texCnt = 128;
    FILE* f = fopen(path, "w");
    if (!f) { off = 1; return; }
    const uint16_t* vr = openpete_vram_data();
    fprintf(f, "{\"texCnt\":%u,\"tiles\":[", texCnt);
    for (uint32_t N = 0; N < texCnt; N++) {
        fprintf(f, "%s{\"n\":%u", N ? "," : "", N);
        texrec_json(cpu, f, "lq_far",  lqTable + N*16u);
        texrec_json(cpu, f, "lq_near", lqTable + N*16u + 8u);
        fprintf(f, ",\"hq\":[");
        for (int i = 0; i < 4; i++) {
            fprintf(f, "%s{\"q\":%d", i ? "," : "", i);
            texrec_json(cpu, f, "t", hqTable + N*168u + (uint32_t)(i+1)*8u);
            fprintf(f, "}");
        }
        fprintf(f, "]}");
        if (dir && *dir && vr) {
            static unsigned char px[64 * 64 * 4];
            char fn[640];
            tmat m; int bu, bv;
            /* 64x64 HQ composite: quadrant i -> (i&1)*32, (i>>1)*32. */
            for (int i = 0; i < 4; i++) {
                texrec_entry(cpu, hqTable + N*168u + (uint32_t)(i+1)*8u, &m, &bu, &bv);
                texrec_blit32(vr, &m, bu, bv, px, 64, (i & 1) * 32, (i >> 1) * 32);
            }
            snprintf(fn, sizeof fn, "%s/rec%02u-hq.png", dir, N);
            stbi_write_png(fn, 64, 64, 4, px, 64 * 4);
            /* LQ near + far 32x32 tiles. */
            texrec_entry(cpu, lqTable + N*16u + 8u, &m, &bu, &bv);
            texrec_blit32(vr, &m, bu, bv, px, 32, 0, 0);
            snprintf(fn, sizeof fn, "%s/rec%02u-lq.png", dir, N);
            stbi_write_png(fn, 32, 32, 4, px, 32 * 4);
            texrec_entry(cpu, lqTable + N*16u, &m, &bu, &bv);
            texrec_blit32(vr, &m, bu, bv, px, 32, 0, 0);
            snprintf(fn, sizeof fn, "%s/rec%02u-lqfar.png", dir, N);
            stbi_write_png(fn, 32, 32, 4, px, 32 * 4);
        }
    }
    fprintf(f, "]}\n");
    fclose(f);
    fprintf(stderr, "[texpack-records] wrote %u logical tiles -> %s%s%s\n",
            texCnt, path, (dir && *dir) ? ", pixels -> " : "",
            (dir && *dir) ? dir : "");
}

/** @brief Prints the texture record and sector words that changed since the previous call.
 *
 * With OPENPETE_TEXPACK_ANIMPROBE=N, prints record N's raw LQ and HQ Tiledef
 * words every call. With OPENPETE_TERRAIN_ANIMDBG set, hashes up to 128 LQ
 * and HQ records and, for up to 512 sectors, the header word at +0x18 and
 * the vertex and colour words, and prints the indices whose hash differs
 * from the previous call (the first call only records). Stores nothing to
 * guest RAM.
 * @param cpu The guest CPU state.
 */
static void terrain_animdbg(CPUState* cpu) {
    static int on = -1;
    ENV_FLAG_LATCH(on, "OPENPETE_TERRAIN_ANIMDBG");
    /* OPENPETE_TEXPACK_ANIMPROBE=<N>: raw Tiledef words of record N each
     * present - shows HOW env anim (func_8002A6FC) rewrites an animated tile
     * (UV scroll vs CLUT swap), which decides the shape of its replacement
     * file (a box covering the full swept range). */
    static int probeN = -2;
    ENV_INT_LATCH(probeN, "OPENPETE_TEXPACK_ANIMPROBE", -1);
    if (probeN >= 0) {
        uint32_t lt = tenv_rd(cpu, game_layout.env.lq_tex), ht = tenv_rd(cpu, game_layout.env.hq_tex);
        if (lt && ht) {
            fprintf(stderr, "[animprobe f=%u] lq %08x %08x | %08x %08x | hq",
                    openpete_frame, rd(cpu, lt + (uint32_t)probeN*16u), rd(cpu, lt + (uint32_t)probeN*16u + 4),
                    rd(cpu, lt + (uint32_t)probeN*16u + 8), rd(cpu, lt + (uint32_t)probeN*16u + 12));
            for (int i = 0; i < 5; i++)
                fprintf(stderr, " %08x %08x",
                        rd(cpu, ht + (uint32_t)probeN*game_layout.terrain.hq_stride + (uint32_t)i*8u),
                        rd(cpu, ht + (uint32_t)probeN*game_layout.terrain.hq_stride + (uint32_t)i*8u + 4));
            fprintf(stderr, "\n");
        }
    }
    if (!on) return;
    uint32_t lqTable   = tenv_rd(cpu, game_layout.env.lq_tex);
    uint32_t hqTable   = tenv_rd(cpu, game_layout.env.hq_tex);
    uint32_t texCnt    = tenv_rd(cpu, game_layout.env.tex_count);
    uint32_t sectorPtr = tenv_rd(cpu, game_layout.env.sector_ptr);
    uint32_t sectorCnt = tenv_rd(cpu, game_layout.env.sector_count);
    if (texCnt > 128) texCnt = 128;
    if (sectorCnt > 512) sectorCnt = 512;
    static uint32_t lq[128], hq[128], s18[512], svh[512], sch[512];
    static int have = 0;
    char lqs[256] = "", hqs[256] = "", s18s[256] = "", svs[256] = "", scs[256] = "";
    for (uint32_t t = 0; t < texCnt; t++) {
        uint32_t h = 2166136261u;
        for (int w = 0; w < 4; w++) h = (h ^ rd(cpu, lqTable + t * game_layout.terrain.tex_stride + (uint32_t)w * 4u)) * 16777619u;
        if (have && h != lq[t] && strlen(lqs) < 240) snprintf(lqs + strlen(lqs), 12, " %u", t);
        lq[t] = h;
        h = 2166136261u;
        for (uint32_t w = 0; w < game_layout.terrain.hq_stride / 4u; w++)
            h = (h ^ rd(cpu, hqTable + t * game_layout.terrain.hq_stride + w * 4u)) * 16777619u;
        if (have && h != hq[t] && strlen(hqs) < 240) snprintf(hqs + strlen(hqs), 12, " %u", t);
        hq[t] = h;
    }
    for (uint32_t i = 0; i < sectorCnt; i++) {
        uint32_t s = rd(cpu, sectorPtr + i * 4u);
        if (!s) continue;
        uint32_t g3 = rd(cpu, s + 0x10), g4 = rd(cpu, s + 0x14);
        int nLpV = g3 & 0xFF, nLpC = (g3 >> 8) & 0xFF;
        int nHpV = g4 & 0xFF, nHpC = (g4 >> 8) & 0xFF, hpOff = (int)((g4 >> 24) & 0xFF);
        uint32_t h = rd(cpu, s + 0x18);
        if (have && h != s18[i] && strlen(s18s) < 240) snprintf(s18s + strlen(s18s), 12, " %u", i);
        s18[i] = h;
        h = 2166136261u;                                   /* verts: LP + HP */
        for (int w = 0; w < nLpV; w++) h = (h ^ rd(cpu, s + 0x1C + (uint32_t)w * 4u)) * 16777619u;
        for (int w = 0; w < nHpV; w++) h = (h ^ rd(cpu, s + 0x1C + (uint32_t)(hpOff + w) * 4u)) * 16777619u;
        if (have && h != svh[i] && strlen(svs) < 240) snprintf(svs + strlen(svs), 12, " %u", i);
        svh[i] = h;
        h = 2166136261u;                                   /* colours: LP + HP near/far pairs */
        for (int w = 0; w < nLpC; w++) h = (h ^ rd(cpu, s + 0x1C + (uint32_t)(nLpV + w) * 4u)) * 16777619u;
        for (int w = 0; w < nHpC * 2; w++) h = (h ^ rd(cpu, s + 0x1C + (uint32_t)(hpOff + nHpV + w) * 4u)) * 16777619u;
        if (have && h != sch[i] && strlen(scs) < 240) snprintf(scs + strlen(scs), 12, " %u", i);
        sch[i] = h;
    }
    if (have && (lqs[0] || hqs[0] || s18s[0] || svs[0] || scs[0]))
        fprintf(stderr, "[anim-dbg] f=%u lq:%s | hq:%s | s18:%s | secV:%s | secC:%s\n",
                openpete_frame, lqs, hqs, s18s, svs, scs);
    have = 1;
}

/* Per-sector view-space bounding sphere, stamped each walk (s_vanim_pres).
 * Shared input of the occlusion cull and the portal-rim sector broadphase
 * (rim_sector_cands): sector-table index -> centre through the projection
 * matrix (world/16 units), header radius, poly-anim membership. */
static float    s_secv[1024][4];
static uint8_t  s_secv_anim[1024];
static uint32_t s_secv_stamp[1024];
static double   s_rim_rowmax = 1.0;   /* max ||M row||/4096 - world->view scale bound */

/* ==== terrain sector-mesh GPU cache: game-side builder + claims ============
 *
 * Packs a decoded sector (tscache_t) into rhi_terrain_vertex chunks - one per
 * draw-class arena - and keeps them valid against the moving-terrain inputs:
 * decodeGen (slot re-decode / reuse), texGen (env texture anim re-decoded
 * texmats + the HQ sibling records), vertGen (geometry env anim content
 * change), a per-VBL colour-array hash (terrain colours are read live by the
 * CPU walk; gameplay can only change them at tick boundaries), and the
 * env-anim colour roll (tgpu_canim_stale). Everything
 * baked here is exactly what the CPU face walk reads per present; the
 * camera-dependent selections happen in terrain.vert.
 *
 * CLAIMED back to the CPU walk (sub-tick or per-face semantics the retained
 * mesh cannot bake): near-plane-approaching sectors (the no-clip guarantee),
 * portal-outline overlaps (rim cull / transparent-cover drop / plane-cover),
 * vert-anim sectors (the
 * fractional lerp banks), scroll-anim tiles (the 7-bit accumulator lerp),
 * fill-duty sectors (the feed's screen-space dilation), arena-full and
 * unmapped-colour sectors, sectors an HD texture pack would replace into
 * (per-run replacement rides the feed's draw_tex_runs), and sectors a mod
 * material selector could override (custom pipelines resolve off the feed's
 * provenance tags). The pack/selector claims are tier 1 of the
 * exclusion->claim->fold ladder. */

typedef struct {
    uint32_t s;                       /* sector vaddr chunks were built from (0 = none) */
    uint32_t decodeGen, texGen, vertGen, colHash;
    uint32_t lastColFrame;            /* VBL of the last colour-hash check */
    int      unbuildable;             /* claim reason latched at this decodeGen:
                                       * 0 fine, 1 arena-full, 2 scroll-anim,
                                       * 3 unmapped colours (4 = decode-slot
                                       * contested, 5 = texture pack; both
                                       * returned but never latched here).
                                       * Code 6 was the mod-material selector
                                       * claim and is retired: a material is
                                       * baked into the face as a bucket and
                                       * drawn from the cache. */
    uint32_t first[OPTC_ARENA_COUNT];
    int      count[OPTC_ARENA_COUNT];
    uint32_t bufbit;                  /* bit a = which buffer of arena a holds
                                       * this slot's chunk (0 primary, 1 the
                                       * arena's overflow buffer); each buffer
                                       * numbers its vertices from 0, so the
                                       * index goes with every first[a]       */
    uint32_t ownFrame;                /* VBL this slot last validated as GPU-owned */
    /* Pack/selector claim latches: the scan answers depend on the decoded
     * texmats (decodeGen/texGen) and on the pack registry / selector set
     * (their own generations); 0 = unscanned (both counters start at 1). */
    uint32_t scanDecodeGen, scanTexGen;
    uint32_t tpGen;                   /* texture_pack_generation at last pack scan */
    unsigned selEpoch;                /* openpete_mods_material_terrain_epoch at last scan */
    uint8_t  tpClaim;
    uint8_t  gen;                     /* chunk generation (3 bits published) */
    uint8_t  bakeVar;                 /* the last build for this sector emitted
                                       * near-variant chunks alongside the plain
                                       * ones - sticky for the sector's whole
                                       * residency, so a sector oscillating
                                       * across the near threshold re-bakes at
                                       * most once (and a variant build that
                                       * failed is not retried every present) */
    uint32_t bakePinKey;              /* tgpu_pin_key() at build: the pinned
                                       * bake puts records in the plain chunk
                                       * set and drops the 2x2 of the faces
                                       * the pin sends to the cells, so a
                                       * toggle of the pin or a moved
                                       * s_hq_differ verdict re-bakes */
    uint8_t  bakeCanim;               /* baked as an env-anim colour sector
                                       * (anim arenas, previous colours)      */
    uint32_t canimRoll;               /* its colour slot's rollPres at build */
    uint32_t animRoll;                /* poly-anim: SC->vrollPres at build - a
                                       * new canonical roll re-bakes (prev,cur)
                                       * even when the content hash is stable
                                       * (a paused anim must stop lerping the
                                       * last step) */
    uint32_t bakeTpGen;               /* HD fold: texture_pack_generation at
                                       * build - a moved registry (new files
                                       * loaded) re-bakes so faces that now
                                       * match gain their entries */
    uint32_t bakeAllocEpoch;          /* arena allocation epoch at build - an
                                       * arena-full claim (unbuildable 1)
                                       * retries when the arenas gain room   */
    uint32_t bakeHdEpoch;             /* HD fold: hdfold_epoch at build - a
                                       * fold-claimed (unbuildable=5) sector
                                       * retries when interns or an upload
                                       * drain moved it */
    unsigned bakeSelEpoch;            /* material_epoch at build - the baked
                                       * faces carry material BUCKET ids, so a
                                       * selector set that moved makes them
                                       * stale (wrong pass, or a face left in
                                       * the stock pass a material now wants) */
} tgpu_slot_t;
static tgpu_slot_t s_tgpu[TSC_SLOTS];
/** @brief Tests whether a decode slot holds a sector the GPU cache validated as owned this present.
 *
 * tsc_get's eviction skips such a slot while another candidate exists. With
 * the cache off, ownFrame is never set, so the test is always false.
 * @param idx The decode slot index.
 * @return Nonzero when the slot's GPU record holds a sector and its
 *         ownFrame equals openpete_frame. */
static int tgpu_slot_hot(uint32_t idx) {
    return s_tgpu[idx].s != 0 && s_tgpu[idx].ownFrame == openpete_frame;
}
/* census: window sums (reset each print) + per-walk counts (op_prof rows) */
static long s_tgpu_secs, s_tgpu_claims, s_tgpu_rebuilds, s_tgpu_nvsecs;
static long s_tgcl_near, s_tgcl_portal, s_tgcl_anim, s_tgcl_scroll,
            s_tgcl_fill, s_tgcl_build, s_tgcl_colour, s_tgcl_slot,
            s_tgcl_texpack, s_tgcl_nearb, s_tgcl_nodraw,
            s_tgcl_prim, s_tgcl_pcov;   /* portal sub-buckets: rim-candidate /
                                         * blended-cover (portal = their sum
                                         * plus claim-all + no-refine claims) */
static int  s_tgpu_secs_walk, s_tgpu_claims_walk;
static int  s_tgpu_canim_walk;   /* env-anim colour sectors drawn from the cache (census) */
static int  s_tgpu_canim_mv;     /* ... of them, those whose last roll moved a colour */

/* Active portal-sky planes for the per-sector portal claim. Of the feed's
 * portal semantics, only two ever DROP a CPU-fed terrain tri - the inner-rim
 * cull (bounded by the rim sector broadphase: a sector failing it cannot
 * contain a rim candidate) and the transparent cover (blended faces
 * only, discarded per fragment on both paths). Opaque plane-cover needs no claim at all: covered pixels draw at
 * true depth and the preview repaints over them in core, whoever drew them.
 * So a sector claims iff it passes the rim broadphase, or it has a blended
 * face and its projected bbox overlaps an active outline; everything else
 * near a portal keeps the GPU path and draws identically by construction.
 * s_tgpu_claim_all forces every sector to the CPU when a portal is recent
 * but its planes are momentarily unavailable (safe degenerate); with the
 * plane provider disabled (OPENPETE_TERRAIN_PORTAL_CONFORM=0) the feed
 * drops nothing, and the pre-refinement blanket bbox claim is kept. */
static openpete_portal_skyplane s_tgpu_pplane[6];
static int s_tgpu_pplane_n = 0;
static int s_tgpu_claim_all = 0;
static int s_tgpu_rim_ok = 0;         /* rim frames derived for THIS walk */
static int tgpu_portal_frames(CPUState* cpu);            /* defined post-feed-statics */
static int tgpu_rim_sphere_cand(double oc1, double oc2, double oc3, int radius);
static void tgpu_rim_globals_fill(openpete_tercache_globals* g);
static void tgpu_cover_globals_fill(openpete_tercache_globals* g);

/** @brief Tests whether a sector's decoded vertex AABB lies wholly at or beyond view-Z zt.
 *
 * View-Z is linear in xyz, so the minimum over the 8 transformed box
 * corners bounds every decoded vertex; the corners transform through the
 * walk's own per-vertex expression. With zt = 16.0, the view-Z the near
 * claim guards, a clear box means no vertex can enter the near band, clip,
 * or straddle the eye plane, the regime every sphere-passing owned sector
 * renders in, so the plain chunks are exact. Callers also pass the env-anim
 * near bound and the tier-4 corner bound as zt.
 * @param SC The decoded sector.
 * @param M The camera rotation matrix.
 * @param camx Camera X.
 * @param camy Camera Y.
 * @param camz Camera Z.
 * @param zt The view-Z bound.
 * @return 1 when every box corner's view-Z is at least zt; 0 when one is
 *         below it or no vertex was decoded. */
static int tgpu_near_box_clear(const tscache_t* SC, const int M[3][3],
                               double camx, double camy, double camz, double zt) {
    if (SC->vMinX > SC->vMaxX) return 0;             /* no verts decoded */
    const double s4 = SC->originX - camx, s5 = camy - SC->originY,
                 s6 = camz - SC->originZ;
    const double xs[2] = { (double)SC->vMinX, (double)SC->vMaxX };
    const double ys[2] = { (double)SC->vMinY, (double)SC->vMaxY };
    const double zs[2] = { (double)SC->vMinZ, (double)SC->vMaxZ };
    for (int c = 0; c < 8; c++) {
        double m1, m2, m3;
        ngte_mac(M, s5 - ys[(c >> 1) & 1], s6 - zs[(c >> 2) & 1],
                 xs[c & 1] + s4, &m1, &m2, &m3);
        if (m3 < zt) return 0;
    }
    return 1;
}
static double s_tgpu_l3 = 1.0;        /* ||M row 2||/4096 (near-claim bound) */

/** @brief Returns the key naming every cell-routing verdict a sector's bake reads.
 *
 * The bake follows the cell routing: a face tface_cells_pin admits carries
 * its cell or sub-triangle records in its sector's plain chunk set
 * (tgpu_push_cell_families), drops a quad's 2x2 (tgpu_bake_near) and is
 * marked for the vertex stage (face_flags bit 19); a face tface_cells_ok
 * refuses carries no cell or tier-4 sub-triangle records and is marked by
 * bit 20 clear. The key names everything those verdicts read, so a slot
 * baked under another key re-bakes: 0 on a game without the cells, else the
 * s_hq_differ / s_hq_hires verdict generation with the pin and the texture
 * mapping in its low bits.
 * @return 0 on a game without the 4x4 cells, else 4 + (s_hq_differ_gen << 2)
 *         + 2 when the pin is active + 1 when texture mapping is on. */
static uint32_t tgpu_pin_key(void) {
    if (!game_layout.terrain.hq_cells4) return 0;
    return 4u + (s_hq_differ_gen << 2) + (hq_pin_active() ? 2u : 0u)
         + (openpete_native_get_texture_mapping() ? 1u : 0u);
}


/** @brief Tests whether a face of an env-anim colour sector may be near this present.
 *
 * An env-anim colour sector draws from the cache only while no face of it
 * can take a near subdivide: the 2x2 carries previous colours, but the cell
 * and split-triangle records carry the current near colours alone and have
 * no vertex input left for more. A face is near only while its four corner
 * SZ3 (hqscale * m3) sum below near_band (face_is_near_canonical), so a
 * sector whose every vertex sits at or beyond near_band / (4 * hqscale)
 * view-Z has none; under the always-HQ pin every face is near. The sphere
 * bound is tested first, then the vertex AABB (tgpu_near_box_clear).
 * @param SC The decoded sector.
 * @param M The camera rotation matrix.
 * @param camx Camera X.
 * @param camy Camera Y.
 * @param camz Camera Z.
 * @param oc3 The sector centre's view-Z.
 * @return 1 when a face may be near, so the sector takes the near path. */
static int tgpu_canim_band(const tscache_t* SC, const int M[3][3], double camx,
                           double camy, double camz, double oc3) {
    if (hq_pin_active()) return 1;
    const double zb = (double)game_layout.terrain.near_band / (4.0 * tface_hqscale());
    const double nearBound = s_tgpu_l3 * ((double)SC->radius + 16.0);
    return oc3 - nearBound < zb && !tgpu_near_box_clear(SC, M, camx, camy, camz, zb);
}

/** @brief Tests whether a sector takes the near path this present.
 *
 * True for every sector whose bounding sphere, refined by its vertex AABB,
 * may bring a vertex inside the 16.0 view-Z the near claim guards
 * (poly-anim sectors on the sphere alone). On a game with the 4x4 near-cell
 * path, also every non-anim sector that may bring a vertex inside the
 * tier-4 corner bound (near_corner / hqscale view-Z): only a near-variant
 * sector carries the records of a face the pin does not send to the cells
 * (tgpu_bake_cells4), so a sector drawn from its plain chunks never holds
 * such a face the classifier can route to tier 4. An env-anim colour sector
 * answers tgpu_canim_band instead.
 * @param SC The decoded sector.
 * @param M The camera rotation matrix.
 * @param camx Camera X.
 * @param camy Camera Y.
 * @param camz Camera Z.
 * @param oc3 The sector centre's view-Z.
 * @return 1 when the sector takes the near path, else 0. */
static int tgpu_near_reach(const tscache_t* SC, const int M[3][3], double camx,
                           double camy, double camz, double oc3) {
    if (tsc_canim(SC)) return tgpu_canim_band(SC, M, camx, camy, camz, oc3);
    const double nearBound = s_tgpu_l3 * ((double)SC->radius + 16.0);
    if (oc3 - nearBound < 16.0 &&
        (SC->polyanim || !tgpu_near_box_clear(SC, M, camx, camy, camz, 16.0)))
        return 1;
    if (SC->polyanim || !game_layout.terrain.hq_cells4) return 0;
    const double zt = (double)game_layout.terrain.near_corner / tface_hqscale();
    return oc3 - nearBound < zt && !tgpu_near_box_clear(SC, M, camx, camy, camz, zt);
}

/** @brief Tests whether near-reaching sectors bake near-variant chunks instead of claiming to the CPU walk.
 *
 * Near-variant chunks carry the vertex-stage clip fan for a sector whose
 * geometry can reach or cross the eye plane. OPENPETE_TERRAIN_NEARVAR=0
 * restores the claim; the variable is latched on the first call.
 * @return 1 unless OPENPETE_TERRAIN_NEARVAR starts with '0'. */
static int tgpu_nearvar_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_TERRAIN_NEARVAR");
                 v = !(e && *e && *e == '0'); }
    return v;
}
static int s_otbias_on;               /* tentative: defined (=1) at the feed */

/** @brief Returns a slot's chunks to every arena and clears its baked-sector fields.
 *
 * For each arena with a nonzero chunk count, frees the chunk range (buffer
 * bit (bufbit >> arena) & 1, first[arena], count[arena]) through
 * openpete_tercache_chunk_free, then zeroes the counts, bufbit and the
 * slot's sector s. The other slot fields are left as they are.
 * @param T The slot. */
static void tgpu_free_slot(tgpu_slot_t* T) {
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        if (T->count[a] > 0)
            openpete_tercache_chunk_free(a, (T->bufbit >> a) & 1, T->first[a], T->count[a]);
        T->count[a] = 0;
    }
    T->bufbit = 0;
    T->s = 0;
}

/** @brief Gives every decode slot's chunks back to the arenas.
 *
 * A slot otherwise holds its chunks until another sector takes the slot, so
 * nothing else releases the outgoing level's geometry at a level load.
 * @return The number of slots that held a baked sector, which is how many
 *         sectors have to be walked and baked again. */
static long tgpu_free_all_slots(void) {
    long live = 0;
    for (int i = 0; i < TSC_SLOTS; i++) {
        if (s_tgpu[i].s) live++;
        tgpu_free_slot(&s_tgpu[i]);
    }
    return live;
}

/* Sectors given up to the second free, the one between the level edge and
 * the adoption of the fresh buffers. They were baked into the outgoing
 * buffers during the load and re-bake over the following walks - a
 * load-screen transient, and the only re-bake the replacement costs. */
static long s_tgpu_rebake;
static long s_tgpu_keycell_faces;   /* faces baked with FF_KEYCELLS (session) */

/** @brief Frees every slot's chunks at a level change and moves the arenas to the new level.
 *
 * The freed ranges cannot be reused in place: the arena's defer
 * window counts builds, no build runs during a load, and the new level's
 * first walk allocates before any range ages out. So the arenas are asked
 * for fresh buffers instead, which also resets each arena's high-water mark
 * and drops the free-list fragmentation the outgoing level left. The first
 * latch of a session has no outgoing level and init has just made the
 * buffers, so there it clears the slots and sets the level alone.
 * @param prev_level The outgoing level id, or -1 on the run's first latch.
 * @param level The incoming level id. */
static void tgpu_level_edge(int prev_level, int level) {
    tgpu_free_all_slots();
    if (prev_level >= 0) openpete_tercache_request_resize(level);
    else openpete_tercache_set_level(level);
}

/** @brief Tests whether an arena-full claimed slot should retry its bake.
 *
 * An arena-full claim is a transient: the cache asks the device-owning
 * thread for an overflow buffer, and the sector belongs on the GPU again as
 * soon as that buffer is in place. The allocation epoch steps at exactly the
 * moments a refused chunk may now fit (an overflow buffer, or a level-edge
 * replacement), so a claimed sector re-bakes then and at no other build.
 * @param T The slot.
 * @return 1 when the slot's claim code is 1 and the allocation epoch moved
 *         since its bake. */
static int tgpu_claim_retry(const tgpu_slot_t* T) {
    return T->unbuildable == 1 && T->bakeAllocEpoch != openpete_tercache_alloc_epoch();
}

/** @brief Frees every slot's chunks when the arenas' fresh buffers are ready for adoption.
 *
 * Runs before the frame_begin that adopts the buffers: the adoption zeroes
 * every arena's bookkeeping, so the sectors baked into the outgoing buffers
 * since the level edge must give up their chunk records first (a free after
 * the adoption would decrement counters the fresh buffers own). They
 * re-bake against the fresh buffers over the following walks; the freed
 * count accumulates in s_tgpu_rebake. */
static void tgpu_resize_adopt_prepare(void) {
    if (openpete_tercache_resize_ready()) s_tgpu_rebake += tgpu_free_all_slots();
}

/** @brief Tests whether a debug or A/B environment variable forces the pure-CPU terrain path.
 *
 * The listed OPENPETE_TERRAIN_* variables have semantics that live in the
 * CPU walk, or change walk math the shader mirrors only at the default;
 * any of them set and nonempty blocks the GPU cache. So do
 * OPENPETE_TERRAIN_FOGDPCS=0, OPENPETE_TERRAIN_CULL other than 3,
 * OPENPETE_PORTAL_GUARD nonzero, OPENPETE_MOBY_TRUESCOPE=0, OPENPETE_TRUE_W
 * set, and OPENPETE_NOTEX nonzero. The verdict is latched on the first
 * call.
 * @return 1 when the GPU cache is blocked, else 0. */
static int tgpu_env_block(void) {
    static int v = -1;
    if (v >= 0) return v;
    static const char* const keys[] = {
        "TIERCOLOR", "NOTEX", "FLAT", "TIERONLY", "FARDBG", "SECTORDUMP",
        "CALDBG", "NEARQ", "VERIFY", "CULLDBG", "NEARDBG", "BINDBG",
        "LPKEEP", "LPKEEPDBG", "FILL_DILATE", "FILL_FOGSLACK", "FILL_UP",
        "FILL_THINPX", "GAPFILL_EXCL", "FOGSCALE", "HQSCALE", "NEARZ",
        "HIDETAIL", "TEXTBL", "PORTAL_CONFORM_DBG",
        /* A/B levers whose non-default value changes walk math the shader
         * mirrors only at the shipped default: */
        "LP_FARCULL", "LP_ADMIT", "HP_DROP", "LOD_FAITHFUL",
        "NEARBAND_CONTEST",
    };
    v = 0;
    for (unsigned k = 0; k < sizeof(keys) / sizeof(keys[0]) && !v; k++) {
        char name[64];
        snprintf(name, sizeof name, "OPENPETE_TERRAIN_%s", keys[k]);
        const char* e = getenv(name);
        if (e && *e) v = 1;
    }
    if (!v) { const char* e = getenv("OPENPETE_TERRAIN_FOGDPCS");   /* =0: legacy lerp fog */
              if (e && *e && *e == '0') v = 1; }
    if (!v) { const char* e = getenv("OPENPETE_TERRAIN_CULL");      /* non-default cull mask */
              if (e && *e && atoi(e) != 3) v = 1; }
    if (!v) { const char* e = getenv("OPENPETE_PORTAL_GUARD");      /* guard re-feed needs CPU tris */
              if (e && *e && *e != '0') v = 1; }
    if (!v) { const char* e = getenv("OPENPETE_MOBY_TRUESCOPE");    /* terrain.frag mirrors scope ON */
              if (e && *e && *e == '0') v = 1; }
    if (!v) { const char* e = getenv("OPENPETE_TRUE_W");            /* W-mask bisect instrument */
              if (e && *e) v = 1; }
    if (!v) { const char* e = getenv("OPENPETE_NOTEX");             /* scene NOTEX spec constant */
              if (e && *e && *e != '0') v = 1; }
    return v;
}

/** @brief Packs a material's texture page, depth, blend and textured flag into vertex material word 0.
 *
 * Bits 0-9 tpx, 10-18 tpy, 19-20 depth code (4 bpp 0, 8 bpp 1, else 2),
 * 21-22 blend (a blend above 3 packs as 0), 23 textured. Bits 24-31 are left
 * clear for the caller's bucket.
 * @param m The material.
 * @return The packed word. */
static uint32_t tgpu_pack_mat0(const tmat* m) {
    int blend = (m->blend > 3) ? 0 : m->blend;   /* ABR-3 "B+F/4" clamps opaque (scene rule) */
    return (uint32_t)(m->tpx & 0x3FF)
         | ((uint32_t)(m->tpy & 0x1FF) << 10)
         | ((uint32_t)(m->bpp == 4 ? 0 : m->bpp == 8 ? 1 : 2) << 19)
         | ((uint32_t)(blend & 3) << 21)
         | ((uint32_t)(m->textured ? 1 : 0) << 23);
}
/** @brief Returns a material's draw-class arena, chosen by its blend alone.
 *
 * Blend 1 takes the alpha arena, 2 the additive, 3 the subtractive; blend 0
 * and the B+F/4 mode (above 3), which clamps opaque, take the opaque arena.
 * Untextured blended faces route to the blend arenas like the feed's
 * untextured gouraud-blend groups; the LP arena for blend-0 untextured LP
 * faces is the caller's decision.
 * @param m The material.
 * @return An OPTC_ARENA_* index. */
static int tgpu_arena_of(const tmat* m) {
    int blend = (m->blend > 3) ? 0 : m->blend;
    switch (blend) {
        case 1:  return OPTC_ARENA_ALPHA;
        case 2:  return OPTC_ARENA_ADD;
        case 3:  return OPTC_ARENA_SUB;
        default: return OPTC_ARENA_OPAQUE;
    }
}

typedef struct { rhi_terrain_vertex* v; rhi_terrain_prev* pv;
                 rhi_terrain_nearctx* cx; int n, cap;
                 int anim; /* fill pv alongside v (poly-anim sector)    */
                 int nearv; /* fill cx alongside v (near-variant chunk) */
                 uint8_t* rec;  /* cell arenas: n records of rsz bytes (v unused) */
                 size_t rsz; } tgpu_buf;
/** @brief Appends one vertex and its side records to a retained-cache buffer, growing the buffer when full.
 *
 * Capacity starts at 256 vertices and doubles. The vertex array grows with
 * the previous-tick array (when b->anim) and the near-context array (when
 * b->nearv), which receive *p and *c beside *v. A failed reallocation
 * leaves the raised capacity in place while a later array may be smaller,
 * so the caller discards the buffer after a -1.
 * @param b The buffer.
 * @param v The vertex.
 * @param p The previous-tick record; read only when b->anim is set.
 * @param c The near-variant context; read only when b->nearv is set.
 * @return 0, or -1 when a reallocation fails. */
static int tgpu_push(tgpu_buf* b, const rhi_terrain_vertex* v,
                     const rhi_terrain_prev* p, const rhi_terrain_nearctx* c) {
    if (b->n >= b->cap) {
        int nc = b->cap ? b->cap * 2 : 256;
        rhi_terrain_vertex* nv = (rhi_terrain_vertex*)realloc(b->v, sizeof(*nv) * (size_t)nc);
        if (!nv) return -1;
        b->v = nv; b->cap = nc;
        if (b->anim) {
            rhi_terrain_prev* np = (rhi_terrain_prev*)realloc(b->pv, sizeof(*np) * (size_t)nc);
            if (!np) return -1;
            b->pv = np;
        }
        if (b->nearv) {
            rhi_terrain_nearctx* ncx =
                (rhi_terrain_nearctx*)realloc(b->cx, sizeof(*ncx) * (size_t)nc);
            if (!ncx) return -1;
            b->cx = ncx;
        }
    }
    if (b->anim) b->pv[b->n] = *p;
    if (b->nearv) b->cx[b->n] = *c;
    b->v[b->n++] = *v;
    return 0;
}

/** @brief Appends one cell record to a cell arena's buffer, doubling its capacity when full.
 * @param b The cell arena's buffer; its rsz gives the record size.
 * @param r The record, rsz bytes.
 * @return 0, or -1 when the reallocation fails. */
static int tgpu_push_rec(tgpu_buf* b, const void* r) {
    if (b->n >= b->cap) {
        int nc = b->cap ? b->cap * 2 : 64;
        uint8_t* nr = (uint8_t*)realloc(b->rec, b->rsz * (size_t)nc);
        if (!nr) return -1;
        b->rec = nr; b->cap = nc;
    }
    memcpy(b->rec + b->rsz * (size_t)b->n++, r, b->rsz);
    return 0;
}

/* x4 fixed-point corner (sector-local world/16, 2 fractional bits). */
typedef struct { int16_t x, y, z; } tgpu_p4;
/** @brief Converts a sector-local vertex to an x4 fixed-point corner.
 *
 * Each coordinate is multiplied by 4 (two fractional bits) and truncated to
 * int16.
 * @param v The decoded vertex.
 * @return The corner. */
static tgpu_p4 tgpu_p4_of(const tsvert_t* v) {
    tgpu_p4 p = { (int16_t)(v->x * 4), (int16_t)(v->y * 4), (int16_t)(v->z * 4) };
    return p;
}
/** @brief Returns the per-axis midpoint of two x4 fixed-point corners.
 *
 * Each axis is (a + b) >> 1. The result is exact when the sum is even, which
 * holds for corners from tgpu_p4_of (multiples of 4) and for midpoints of
 * those.
 * @param a The first corner.
 * @param b The second corner.
 * @return The midpoint. */
static tgpu_p4 tgpu_p4_mid(tgpu_p4 a, tgpu_p4 b) {   /* exact: x4 sums are even */
    tgpu_p4 p = { (int16_t)((a.x + b.x) >> 1), (int16_t)((a.y + b.y) >> 1),
                  (int16_t)((a.z + b.z) >> 1) };
    return p;
}
/** @brief Returns the per-channel (a + b) >> 1 average of two 0x00BBGGRR colours.
 *
 * Matches cv_mid. Bits 24-31 of the result are 0.
 * @param a The first colour.
 * @param b The second colour.
 * @return The average. */
static uint32_t tgpu_rgb_mid(uint32_t a, uint32_t b) {   /* cv_mid's per-channel (a+b)>>1 */
    return (uint32_t)((((a & 0xFF) + (b & 0xFF)) >> 1))
         | (uint32_t)(((((a >> 8) & 0xFF) + ((b >> 8) & 0xFF)) >> 1) << 8)
         | (uint32_t)(((((a >> 16) & 0xFF) + ((b >> 16) & 0xFF)) >> 1) << 16);
}

/* Emit one triangle (3 corners) built from a face-level template. Each
 * corner carries its flags16 role bits (HQ: sub-quad + corner slot; the
 * vertex stage rebuilds the node from the TRANSFORMED parent corners). */
typedef struct {
    tgpu_p4  c[4];        /* parent-face corners, i0..i3 order */
    tgpu_p4  pc[4];       /* poly-anim: previous-tick corners (else unset) */
    uint32_t face_flags;
    uint32_t matf0, matf1, matn0, matn1;
    uint16_t flags16;
} tgpu_face_t;
typedef struct { tgpu_p4 p, pp; uint32_t rgb_far, rgb_near;
                 uint32_t prgb_far, prgb_near;   /* anim arenas: previous-tick colours */
                 uint8_t uf, vf, un, vn, role; } tgpu_corner_t;

/** @brief Fills one retained-cache vertex from a face template and one corner.
 *
 * Zeroes *v, then copies the corner's position, far and near UVs and colours
 * (alpha forced to 0xFF), and the face's material words, face_flags and four
 * parent corners. flags16 is the face's flags16, the corner's role shifted
 * to bit 3, bit 13 for tri2 and bit 15 for spare.
 * @param v Receives the vertex.
 * @param F The face template.
 * @param k The corner.
 * @param tri2 1 for a quad's second half or a sub-triangle.
 * @param spare 1 for the second (spare) triangle of a near-variant twin. */
static void tgpu_fill_vert(rhi_terrain_vertex* v, const tgpu_face_t* F,
                           const tgpu_corner_t* k, int tri2, int spare) {
    memset(v, 0, sizeof(*v));
    v->px = k->p.x; v->py = k->p.y; v->pz = k->p.z;
    v->flags16 = (uint16_t)(F->flags16 | ((uint16_t)k->role << 3)
                            | (tri2  ? 0x2000u : 0u)
                            | (spare ? 0x8000u : 0u));
    v->uvf_u = k->uf; v->uvf_v = k->vf;
    v->uvn_u = k->un; v->uvn_v = k->vn;
    v->rgba_far  = 0xFF000000u | (k->rgb_far  & 0xFFFFFFu);
    v->rgba_near = 0xFF000000u | (k->rgb_near & 0xFFFFFFu);
    v->mat_far0 = F->matf0; v->mat_far1 = F->matf1;
    v->mat_near0 = F->matn0; v->mat_near1 = F->matn1;
    v->face_flags = F->face_flags;
    v->c0x = F->c[0].x; v->c0y = F->c[0].y; v->c0z = F->c[0].z;
    v->c1x = F->c[1].x; v->c1y = F->c[1].y; v->c1z = F->c[1].z;
    v->c2x = F->c[2].x; v->c2y = F->c[2].y; v->c2z = F->c[2].z;
    v->c3x = F->c[3].x; v->c3y = F->c[3].y; v->c3z = F->c[3].z;
}

/** @brief Appends one triangle's three vertices to a retained-cache buffer.
 *
 * Each vertex is built by tgpu_fill_vert with the spare bit clear. In an
 * animated buffer (b->anim) the vertex's previous-tick record is filled from
 * the corner's pp and prgb words and the face's pc corners.
 * @param b The destination buffer.
 * @param F The face template.
 * @param a The first corner.
 * @param c1 The second corner.
 * @param c2 The third corner.
 * @param tri2 1 for a quad's second half or a sub-triangle (flags16 bit 13).
 * @return 0, or -1 when a push fails. */
static int tgpu_emit_tri(tgpu_buf* b, const tgpu_face_t* F,
                         tgpu_corner_t a, tgpu_corner_t c1, tgpu_corner_t c2,
                         int tri2 /* second half / sub-tri (flags16 bit13) */) {
    const tgpu_corner_t cs[3] = { a, c1, c2 };
    for (int k = 0; k < 3; k++) {
        rhi_terrain_prev pr;
        if (b->anim) {
            memset(&pr, 0, sizeof(pr));
            pr.px = cs[k].pp.x; pr.py = cs[k].pp.y; pr.pz = cs[k].pp.z;
            pr.c0x = F->pc[0].x; pr.c0y = F->pc[0].y; pr.c0z = F->pc[0].z;
            pr.c1x = F->pc[1].x; pr.c1y = F->pc[1].y; pr.c1z = F->pc[1].z;
            pr.c2x = F->pc[2].x; pr.c2y = F->pc[2].y; pr.c2z = F->pc[2].z;
            pr.c3x = F->pc[3].x; pr.c3y = F->pc[3].y; pr.c3z = F->pc[3].z;
            pr.rgb_far  = 0xFF000000u | (cs[k].prgb_far  & 0xFFFFFFu);
            pr.rgb_near = 0xFF000000u | (cs[k].prgb_near & 0xFFFFFFu);
        }
        rhi_terrain_vertex v;
        tgpu_fill_vert(&v, F, &cs[k], tri2, 0);
        if (tgpu_push(b, &v, b->anim ? &pr : NULL, NULL) != 0) return -1;
    }
    return 0;
}

/** @brief Emits the near-variant twin of one triangle: six vertices with their clip-fan context.
 *
 * The primary fan triangle comes first, then the spare (flags16 bit 15);
 * each vertex carries the other two members' UVs and colours so the vertex
 * stage can rebuild and near-plane clip the whole triangle. Context members
 * are in emit order relative to self (a = self+1, b = self+2 mod 3), the
 * order the clip fan indexes them in; both halves repeat the same three
 * corner records, so a vertex's slot in its triangle is its index mod 3.
 * @param b The near-variant buffer.
 * @param F The face template.
 * @param a The first corner.
 * @param c1 The second corner.
 * @param c2 The third corner.
 * @param tri2 1 for a quad's second half or a sub-triangle (flags16 bit 13).
 * @return 0, or -1 when a push fails. */
static int tgpu_emit_tri_near(tgpu_buf* b, const tgpu_face_t* F,
                              tgpu_corner_t a, tgpu_corner_t c1,
                              tgpu_corner_t c2, int tri2) {
    const tgpu_corner_t cs[3] = { a, c1, c2 };
    for (int spare = 0; spare < 2; spare++) {
        for (int k = 0; k < 3; k++) {
            rhi_terrain_vertex v;
            rhi_terrain_nearctx cx;
            const tgpu_corner_t* A = &cs[(k + 1) % 3];
            const tgpu_corner_t* B = &cs[(k + 2) % 3];
            tgpu_fill_vert(&v, F, &cs[k], tri2, spare);
            memset(&cx, 0, sizeof(cx));
            cx.a_uf = A->uf; cx.a_vf = A->vf; cx.a_un = A->un; cx.a_vn = A->vn;
            cx.a_far  = 0xFF000000u | (A->rgb_far  & 0xFFFFFFu);
            cx.a_near = 0xFF000000u | (A->rgb_near & 0xFFFFFFu);
            cx.b_uf = B->uf; cx.b_vf = B->vf; cx.b_un = B->un; cx.b_vn = B->vn;
            cx.b_far  = 0xFF000000u | (B->rgb_far  & 0xFFFFFFu);
            cx.b_near = 0xFF000000u | (B->rgb_near & 0xFFFFFFu);
            if (tgpu_push(b, &v, NULL, &cx) != 0) return -1;
        }
    }
    return 0;
}

/** @brief Emits one triangle into a sector's chunk buffers, with its near-variant twin while the sector carries one.
 *
 * The plain triangle goes to buf[arena + ab]; with with_var set, the twin
 * goes to buf[arena + OPTC_ARENA_NEAR_BASE] through tgpu_emit_tri_near.
 * @param buf The sector's bake buffers, indexed by arena.
 * @param arena The draw-class arena (tgpu_arena_of, or OPTC_ARENA_LP).
 * @param ab OPTC_ARENA_ANIM_BASE for an anim sector, else 0.
 * @param with_var 1 when the sector carries near-variant chunks.
 * @param F The face template.
 * @param a The first corner.
 * @param c1 The second corner.
 * @param c2 The third corner.
 * @param tri2 1 for a quad's second half or a sub-triangle (flags16 bit 13).
 * @return 0, or -1 when a push fails. */
static int tgpu_emit_face_tri(tgpu_buf* buf, int arena, int ab, int with_var,
                              const tgpu_face_t* F, tgpu_corner_t a,
                              tgpu_corner_t c1, tgpu_corner_t c2, int tri2) {
    if (tgpu_emit_tri(&buf[arena + ab], F, a, c1, c2, tri2) != 0) return -1;
    if (with_var && tgpu_emit_tri_near(&buf[arena + (int)OPTC_ARENA_NEAR_BASE],
                                       F, a, c1, c2, tri2) != 0) return -1;
    return 0;
}

/** @brief Hashes a sector's live colour arrays with FNV-1a, the liveness test for its baked colours.
 *
 * Covers the LP colours and the HP near and far colours, read through
 * openpete_subtick_host_span.
 * @param SC The decoded sector.
 * @param out Receives the hash.
 * @return 0, or -1 when a range has no host mapping (the caller claims the
 *         sector). */
static int tgpu_col_hash(const tscache_t* SC, uint32_t* out) {
    uint32_t h = 2166136261u;
    struct { uint32_t base; int bytes; } r[3] = {
        { SC->lpColBase, SC->nLpC * 4 },
        { SC->nearBase,  SC->nHpC * 4 },
        { SC->farBase,   SC->nHpC * 4 },
    };
    for (int i = 0; i < 3; i++) {
        if (r[i].bytes <= 0) continue;
        const uint8_t* p = (const uint8_t*)tn_host_span(r[i].base, (uint32_t)r[i].bytes);
        if (!p) return -1;
        for (int j = 0; j < r[i].bytes; j++) h = (h ^ p[j]) * 16777619u;
    }
    *out = h;
    return 0;
}

/** @brief Interns the HD-fold replacement entry of one textured face half for its emitted triangles.
 *
 * Evaluates the draw-time per-triangle resolve and match at bake time. An
 * unbakeable half makes the sector claim back to the CPU walk: a blended
 * target (sub-runs draw those inline, in order), triangles resolving to
 * different files, or a table or atlas overflow.
 * @param m The half's material.
 * @param ua The first triangle's corner U texels, in emit order.
 * @param va The first triangle's corner V texels.
 * @param ub The second triangle's corner U texels, or NULL for a GT3.
 * @param vb The second triangle's corner V texels, or NULL for a GT3.
 * @return 0 for no replacement (or an untextured material), the entry id
 *         when both triangles agree, or -1 when unbakeable. */
static int tgpu_hdfold_face(const tmat* m,
                            const float* ua, const float* va,
                            const float* ub, const float* vb)
{
    if (!m->textured) return 0;
    int a = openpete_native_hdfold_intern(m->tpx, m->tpy, m->clx, m->cly,
                                          m->bpp, ua, va);
    if (a < 0) return -1;
    int b = a;
    if (ub) {
        b = openpete_native_hdfold_intern(m->tpx, m->tpy, m->clx, m->cly,
                                          m->bpp, ub, vb);
        if (b < 0) return -1;
    }
    if (a != b) return -1;
    if (a > 0 && m->blend) return -1;
    return a;
}

/** @brief Decodes quadrant q's tile of a near face's HQ record.
 *
 * The 8-byte tile at hq_sub0 + q*8 with the D_8006D058 orientation applied,
 * as terrain_near_emit reads it; the V scroll rides the globals.
 * @param cpu The guest CPU state.
 * @param hqBase The HQ record's guest address.
 * @param abe The face's ABE bit.
 * @param q The quadrant: 0 TL, 1 TR, 2 BL, 3 BR.
 * @param m Receives the material.
 * @param u Receives the corner U texels, [TL,TR,BL,BR].
 * @param v Receives the corner V texels. */
static void tgpu_hq_quadrant(CPUState* cpu, uint32_t hqBase, int abe, int q,
                             tmat* m, float u[4], float v[4]) {
    memset(m, 0, sizeof(*m)); m->bpp = 16;
    uint32_t e = hqBase + game_layout.terrain.hq_sub0 + (uint32_t)q * 8u;
    decode_tiledef(cpu, e, abe, m, u, v);
    tiledef_apply_ori(cpu, e, u, v);
}

/** @brief Bakes the HQ 2x2 subdivide variant of a near quad, mirroring terrain_near_emit.
 *
 * Corners TL=i0, TR=i1, BL=i3, BR=i2 take the raw near colours; midpoint
 * positions are exact ((a+b)>>1 on even x4 sums) and midpoint colours use
 * the walk's (a+b)>>1. Sub-quad q uses the HQ tile at hq_sub0 + q*8 with
 * the D_8006D058 UV-orientation adjust (tgpu_hq_quadrant) and is
 * triangulated (TL,TR,BL) + (TR,BL,BR). Camera-space node values are
 * rebuilt in the vertex stage from the parent corners and the role bits.
 * @param cpu The guest CPU state.
 * @param buf The sector's bake buffers, indexed by arena.
 * @param ab OPTC_ARENA_ANIM_BASE for an anim sector, else 0.
 * @param with_var 1 when the sector carries near-variant chunks.
 * @param T The face template.
 * @param k The face's four corners, i0..i3 order.
 * @param nearC The raw near corner colours.
 * @param F The face.
 * @param hqBase The face's HQ record address.
 * @param hp_bucket The material bucket written into mat_far0 from bit 24.
 * @param hdfold 1 to fold each sub-quad onto its HD replacement.
 * @param fail Set to 5 when a sub-quad cannot be folded.
 * @return 0, or -1 on a failed fold or push. */
static int tgpu_bake_hq2(CPUState* cpu, tgpu_buf* buf, int ab, int with_var,
                         const tgpu_face_t* T, const tgpu_corner_t k[4],
                         const uint32_t nearC[4], const tsface_t* F,
                         uint32_t hqBase, int hp_bucket, int hdfold, int* fail) {
    tgpu_corner_t TL = k[0], TR = k[1], BL = k[3], BR = k[2];
    TL.rgb_far = TL.rgb_near = nearC[0];
    TR.rgb_far = TR.rgb_near = nearC[1];
    BL.rgb_far = BL.rgb_near = nearC[3];
    BR.rgb_far = BR.rgb_near = nearC[2];
    TL.prgb_far = TL.prgb_near = k[0].prgb_near;
    TR.prgb_far = TR.prgb_near = k[1].prgb_near;
    BL.prgb_far = BL.prgb_near = k[3].prgb_near;
    BR.prgb_far = BR.prgb_near = k[2].prgb_near;
    tgpu_corner_t mT, mB, mL, mR, Cc;
    memset(&mT, 0, sizeof(mT)); memset(&mB, 0, sizeof(mB));
    memset(&mL, 0, sizeof(mL)); memset(&mR, 0, sizeof(mR));
    memset(&Cc, 0, sizeof(Cc));
    mT.p = tgpu_p4_mid(TL.p, TR.p); mT.rgb_far = tgpu_rgb_mid(TL.rgb_far, TR.rgb_far);
    mB.p = tgpu_p4_mid(BL.p, BR.p); mB.rgb_far = tgpu_rgb_mid(BL.rgb_far, BR.rgb_far);
    mL.p = tgpu_p4_mid(TL.p, BL.p); mL.rgb_far = tgpu_rgb_mid(TL.rgb_far, BL.rgb_far);
    mR.p = tgpu_p4_mid(TR.p, BR.p); mR.rgb_far = tgpu_rgb_mid(TR.rgb_far, BR.rgb_far);
    Cc.p = tgpu_p4_mid(mT.p, mB.p); Cc.rgb_far = tgpu_rgb_mid(mT.rgb_far, mB.rgb_far);
    /* prev twin midpoints (HQ node positions derive from the lerped
     * PARENT corners in-shader; these per-vertex values are unread
     * for hqv verts but kept coherent). */
    mT.pp = tgpu_p4_mid(TL.pp, TR.pp); mB.pp = tgpu_p4_mid(BL.pp, BR.pp);
    mL.pp = tgpu_p4_mid(TL.pp, BL.pp); mR.pp = tgpu_p4_mid(TR.pp, BR.pp);
    Cc.pp = tgpu_p4_mid(mT.pp, mB.pp);
    mT.rgb_near = mT.rgb_far; mB.rgb_near = mB.rgb_far;
    mL.rgb_near = mL.rgb_far; mR.rgb_near = mR.rgb_far;
    Cc.rgb_near = Cc.rgb_far;
    mT.prgb_far = mT.prgb_near = tgpu_rgb_mid(TL.prgb_far, TR.prgb_far);
    mB.prgb_far = mB.prgb_near = tgpu_rgb_mid(BL.prgb_far, BR.prgb_far);
    mL.prgb_far = mL.prgb_near = tgpu_rgb_mid(TL.prgb_far, BL.prgb_far);
    mR.prgb_far = mR.prgb_near = tgpu_rgb_mid(TR.prgb_far, BR.prgb_far);
    Cc.prgb_far = Cc.prgb_near = tgpu_rgb_mid(mT.prgb_far, mB.prgb_far);
    tgpu_corner_t sq[4][4] = {
        { TL, mT, mL, Cc },   /* SQ0 [TL,TR,BL,BR] order */
        { mT, TR, Cc, mR },
        { mL, Cc, BL, mB },
        { Cc, mR, mB, BR },
    };
    for (int q = 0; q < 4; q++) {
        tmat m; float u[4], v[4];
        tgpu_hq_quadrant(cpu, hqBase, F->abe, q, &m, u, v);
        tgpu_face_t H = *T;
        /* HQ variant + sub-quad + the HQ RECORD's scroll tile id
         * (the near-emit's dv lookup keys on hq_idx, not tb;
         * hq_idx is w2 & 0x7F, always in range). */
        H.flags16 = (uint16_t)(1u | ((uint16_t)q << 1)
                  | (((uint32_t)F->hq_idx & 0x7Fu) << 5) | 0x1000u);
        /* HQ cells are sub-quads of this HP face: same Tiledef, so
         * the same bucket (the pack overwrites the whole word). */
        H.matf0 = tgpu_pack_mat0(&m) | ((uint32_t)hp_bucket << 24);
        H.matf1 = (uint32_t)(m.clut_raw & 0xFFFF);
        H.matn0 = 0; H.matn1 = 0;   /* near cells never re-select */
        tgpu_corner_t c0 = sq[q][0], c1 = sq[q][1], c2 = sq[q][2], c3 = sq[q][3];
        c0.uf = (uint8_t)u[0]; c0.vf = (uint8_t)v[0]; c0.role = 0;
        c1.uf = (uint8_t)u[1]; c1.vf = (uint8_t)v[1]; c1.role = 1;
        c2.uf = (uint8_t)u[2]; c2.vf = (uint8_t)v[2]; c2.role = 2;
        c3.uf = (uint8_t)u[3]; c3.vf = (uint8_t)v[3]; c3.role = 3;
        if (hdfold) {   /* HQ tiles replace like any textured half */
            float uA[3] = { u[0], u[1], u[2] }, vA[3] = { v[0], v[1], v[2] };
            float uB[3] = { u[1], u[2], u[3] }, vB[3] = { v[1], v[2], v[3] };
            int hd = tgpu_hdfold_face(&m, uA, vA, uB, vB);
            if (hd < 0) { *fail = 5; return -1; }
            H.matf1 |= (uint32_t)hd << 16;
        }
        int ha = tgpu_arena_of(&m);
        if (tgpu_emit_face_tri(buf, ha, ab, with_var, &H, c0, c1, c2, 0) != 0) return -1;
        if (tgpu_emit_face_tri(buf, ha, ab, with_var, &H, c1, c2, c3, 1) != 0) return -1;
    }
    return 0;
}

/** @brief Decodes one 4x4 cell's tile into its record entry, draw class and blend flag.
 *
 * The tile is the 16x16 Tiledef at e with the D_8006D058 orientation
 * applied; the blend flag is the cover-drop key.
 * @param cpu The guest CPU state.
 * @param e The cell tile's guest address.
 * @param abe The face's ABE bit.
 * @param hp_bucket The material bucket written into mat0 from bit 24.
 * @param hdfold 1 to fold the tile onto its HD replacement.
 * @param t Receives the record entry.
 * @param cls Receives the draw-class arena.
 * @param blended Receives 1 when the material is blended.
 * @return 0, or -1 when the HD fold cannot bake the tile. */
static int tgpu_cell_tile(CPUState* cpu, uint32_t e, int abe, int hp_bucket,
                          int hdfold, rhi_terrain_cell_tile* t, uint8_t* cls,
                          uint8_t* blended) {
    tmat m; float u[4], v[4];
    memset(&m, 0, sizeof(m)); m.bpp = 16;
    decode_tiledef_span(cpu, e, abe, &m, u, v, 0x0F);
    tiledef_apply_ori(cpu, e, u, v);
    t->mat0 = tgpu_pack_mat0(&m) | ((uint32_t)hp_bucket << 24);
    t->mat1 = (uint32_t)(m.clut_raw & 0xFFFF);
    for (int q = 0; q < 4; q++) {
        t->uv[q * 2 + 0] = (uint8_t)u[q];
        t->uv[q * 2 + 1] = (uint8_t)v[q];
    }
    if (hdfold) {
        float uA[3] = { u[0], u[1], u[2] }, vA[3] = { v[0], v[1], v[2] };
        float uB[3] = { u[1], u[2], u[3] }, vB[3] = { v[1], v[2], v[3] };
        int hd = tgpu_hdfold_face(&m, uA, vA, uB, vB);
        if (hd < 0) return -1;
        t->mat1 |= (uint32_t)hd << 16;
    }
    *cls = (uint8_t)tgpu_arena_of(&m);
    *blended = m.blend ? 1 : 0;
    return 0;
}

/** @brief Fills a cell record's face context: parent corners, previous-tick corners, face flags and raw near colours.
 * @param R The record; zeroed first.
 * @param T The face template, the source of the current and previous-tick
 *        corners and the face flags.
 * @param nearC The raw near corner colours. */
static void tgpu_cell_rec_init(rhi_terrain_cell_anim* R, const tgpu_face_t* T,
                               const uint32_t nearC[4]) {
    memset(R, 0, sizeof(*R));
    R->cur.c0x = T->c[0].x; R->cur.c0y = T->c[0].y; R->cur.c0z = T->c[0].z;
    R->cur.c1x = T->c[1].x; R->cur.c1y = T->c[1].y; R->cur.c1z = T->c[1].z;
    R->cur.c2x = T->c[2].x; R->cur.c2y = T->c[2].y; R->cur.c2z = T->c[2].z;
    R->cur.c3x = T->c[3].x; R->cur.c3y = T->c[3].y; R->cur.c3z = T->c[3].z;
    R->p0x = T->pc[0].x; R->p0y = T->pc[0].y; R->p0z = T->pc[0].z;
    R->p1x = T->pc[1].x; R->p1y = T->pc[1].y; R->p1z = T->pc[1].z;
    R->p2x = T->pc[2].x; R->p2y = T->pc[2].y; R->p2z = T->pc[2].z;
    R->p3x = T->pc[3].x; R->p3y = T->pc[3].y; R->p3z = T->pc[3].z;
    for (int q = 0; q < 4; q++) R->cur.near_rgb[q] = nearC[q] & 0xFFFFFFu;
    R->cur.face_flags = T->face_flags;
}

/** @brief Pushes a face's 16 cell-record entries as one record per half and per draw class the half uses.
 *
 * Each record carries its half's eight entries; its info word holds the HQ
 * record index (bits 0-6), the half (bit 7), the mask of entries in the
 * record's draw class (bits 8-15), the half's blended entries (bits
 * 16-23), and kind.
 * @param buf The sector's bake buffers, indexed by arena.
 * @param base The family's first arena (OPTC_ARENA_CELL_ANIM_BASE or
 *        OPTC_ARENA_CELL_NEAR_BASE).
 * @param R The record holding the face context; its tile and info are
 *        rewritten per push.
 * @param tiles The 16 entries.
 * @param cls Each entry's draw-class arena.
 * @param blended Each entry's blend flag.
 * @param hq_idx The face's HQ record index.
 * @param kind The record-kind bits (bit 24 triangle, bit 25 tier 2).
 * @return 0, or -1 when a push fails. */
static int tgpu_push_cell_halves(tgpu_buf* buf, int base, rhi_terrain_cell_anim* R,
                                 const rhi_terrain_cell_tile tiles[16], const uint8_t cls[16],
                                 const uint8_t blended[16], uint32_t hq_idx, uint32_t kind) {
    for (int h = 0; h < 2; h++) {
        uint32_t bl = 0;
        for (int j = 0; j < 8; j++) bl |= (uint32_t)blended[h * 8 + j] << j;
        memcpy(R->cur.tile, &tiles[h * 8], sizeof(R->cur.tile));
        for (int c = OPTC_ARENA_OPAQUE; c <= OPTC_ARENA_SUB; c++) {
            uint32_t mask = 0;
            for (int j = 0; j < 8; j++) mask |= (uint32_t)(cls[h * 8 + j] == c) << j;
            if (!mask) continue;
            R->cur.info = (hq_idx & 0x7Fu) | ((uint32_t)h << 7) | (mask << 8) | (bl << 16) | kind;
            if (tgpu_push_rec(&buf[base + c], R) != 0) return -1;
        }
    }
    return 0;
}

/** @brief Pushes a face's cell records into every family that draws them.
 *
 * The CA family serves the plain chunk set of a poly-anim sector (ab) or,
 * for a face routed to the cells (pin4: tface_cells_pin), of any sector (a
 * non-anim sector's previous corners equal its current ones, so the anim
 * stage's prev->cur lerp returns them exactly). The CN family serves a
 * near-variant sector's near set (with_var).
 * @param buf The sector's bake buffers, indexed by arena.
 * @param ab Nonzero for an anim sector.
 * @param with_var 1 when the sector carries near-variant chunks.
 * @param pin4 1 when the face goes to the CA family regardless of ab.
 * @param R The record holding the face context.
 * @param tiles The 16 entries.
 * @param cls Each entry's draw-class arena.
 * @param blended Each entry's blend flag.
 * @param hq_idx The face's HQ record index.
 * @param kind The record-kind bits.
 * @return 0, or -1 when a push fails. */
static int tgpu_push_cell_families(tgpu_buf* buf, int ab, int with_var, int pin4,
                                   rhi_terrain_cell_anim* R,
                                   const rhi_terrain_cell_tile tiles[16], const uint8_t cls[16],
                                   const uint8_t blended[16], uint32_t hq_idx, uint32_t kind) {
    if ((ab || pin4) &&
        tgpu_push_cell_halves(buf, (int)OPTC_ARENA_CELL_ANIM_BASE, R, tiles, cls, blended,
                              hq_idx, kind) != 0)
        return -1;
    if (with_var &&
        tgpu_push_cell_halves(buf, (int)OPTC_ARENA_CELL_NEAR_BASE, R, tiles, cls, blended,
                              hq_idx, kind) != 0)
        return -1;
    return 0;
}

/* 4x4 cell variant (nc4_emit_cells mirror) of an HQ-eligible quad the
 * classifier can route to tier 4 (near_flag_tier2 clear): 16 cells, cell i
 * textured from the 16x16 tile at hq_cells4 + i*8. The cells are not baked
 * as vertices: the face contributes cell records (rhi_terrain_cell, eight
 * cells each) to the cell arenas, and the terrain_cells vertex stage derives
 * every cell vertex from the record - node positions, the snap, the interior
 * morph, the key and the straddler facing from the transformed parent
 * corners as before, node colours from the raw near corner colours by the
 * drain's halving order, the tile UVs and material from the record.
 *
 * Records exist only where a tier-4 face can draw: a near-variant sector
 * (every non-anim sector with a vertex inside the tier-4 corner bound takes
 * one, tgpu_near_reach) writes the CN family, drawn with its near chunks; a
 * poly-anim sector (those have no near variant) writes the CA family, with
 * the previous-tick corners appended; a face routed to the cells
 * (tface_cells_pin) writes the CA family from every sector's plain chunk
 * set too (tgpu_push_cell_families). Each half of the face gets one record
 * per draw class its tiles use, masked to that class's cells. */
/** @brief Bakes the 4x4 cell records of an HQ-eligible quad into the cell arenas.
 *
 * Nothing is baked on a game without the 4x4 cells, for a face with
 * near_flag_tier2 set, or when no family draws the records (ab, with_var and
 * pin4 all clear).
 * @param cpu The guest CPU state.
 * @param buf The sector's bake buffers, indexed by arena.
 * @param ab 1 for a poly-anim sector (the CA family).
 * @param with_var 1 for a near-variant sector (the CN family).
 * @param pin4 1 for a face routed to the cells at every distance
 *        (tface_cells_pin).
 * @param T The face's transformed parent corners.
 * @param nearC The raw near corner colours.
 * @param F The face.
 * @param hqBase The face's HQ record address.
 * @param hp_bucket The value written into each tile's mat0 from bit 24.
 * @param hdfold 1 to fold each tile onto its HD replacement.
 * @param fail Set to 5 when a cell tile cannot be baked.
 * @return 0 on success, -1 on a failed tile or a failed push. */
static int tgpu_bake_cells4(CPUState* cpu, tgpu_buf* buf, int ab, int with_var, int pin4,
                            const tgpu_face_t* T, const uint32_t nearC[4],
                            const tsface_t* F, uint32_t hqBase, int hp_bucket,
                            int hdfold, int* fail) {
    if (!game_layout.terrain.hq_cells4 || tface_near_tier2(F->w3)) return 0;
    if (!ab && !with_var && !pin4) return 0;
    rhi_terrain_cell_anim R;
    tgpu_cell_rec_init(&R, T, nearC);
    rhi_terrain_cell_tile tiles[16];
    uint8_t cls[16], blended[16];
    for (int i = 0; i < 16; i++) {
        uint32_t e = hqBase + game_layout.terrain.hq_cells4 + (uint32_t)i * 8u;
        if (tgpu_cell_tile(cpu, e, F->abe, hp_bucket, hdfold, &tiles[i], &cls[i],
                           &blended[i]) != 0) {
            *fail = 5;
            return -1;
        }
    }
    return tgpu_push_cell_families(buf, ab, with_var, pin4, &R, tiles, cls, blended,
                                   F->hq_idx, 0u);
}

/** @brief Decodes one split sub-triangle's tile into its record entry, draw class and blend flag.
 *
 * The entry's material is built as in tgpu_cell_tile; uv[0..5] hold the
 * (u, v) of nodes A, B, C from tiledef_tri_uv, and uv[6] the cell or
 * quadrant (its V scroll row is cell >> 2).
 * @param cpu The guest CPU state.
 * @param e The tile's guest address.
 * @param rot The triangle's UV rotation.
 * @param o The sub-triangle's orientation index for tiledef_tri_uv.
 * @param cell The cell or quadrant index.
 * @param abe The face's ABE bit.
 * @param hp_bucket The material bucket written into mat0 from bit 24.
 * @param hdfold 1 to fold the tile onto its HD replacement.
 * @param t Receives the record entry.
 * @param cls Receives the draw-class arena.
 * @param blended Receives 1 when the material is blended.
 * @return 0, or -1 when the HD fold cannot bake the tile. */
static int tgpu_tri_tile(CPUState* cpu, uint32_t e, int rot, int o, int cell, int abe,
                         int hp_bucket, int hdfold, rhi_terrain_cell_tile* t,
                         uint8_t* cls, uint8_t* blended) {
    tmat m; float u4[4], v4[4], u[3], v[3]; int matdiff;
    memset(&m, 0, sizeof(m)); m.bpp = 16;
    decode_tiledef_span(cpu, e, abe, &m, u4, v4, 0x0F);
    tiledef_tri_uv(cpu, e, rot, o, u, v, &matdiff);
    memset(t, 0, sizeof(*t));
    t->mat0 = tgpu_pack_mat0(&m) | ((uint32_t)hp_bucket << 24);
    t->mat1 = (uint32_t)(m.clut_raw & 0xFFFF);
    for (int q = 0; q < 3; q++) {
        t->uv[q * 2 + 0] = (uint8_t)u[q];
        t->uv[q * 2 + 1] = (uint8_t)v[q];
    }
    t->uv[6] = (uint8_t)cell;
    if (hdfold) {
        int hd = tgpu_hdfold_face(&m, u, v, NULL, NULL);
        if (hd < 0) return -1;
        t->mat1 |= (uint32_t)hd << 16;
    }
    *cls = (uint8_t)tgpu_arena_of(&m);
    *blended = m.blend ? 1 : 0;
    return 0;
}

/** @brief Bakes the split-triangle records of an HQ-eligible triangle, mirroring nt_emit.
 *
 * Emits the triangle's
 * tier-4 (16) or tier-2 (4) sub-triangles as cell records (info bit 24;
 * bit 25 tier 2), entry j of half h the sub-triangle h*8 + j of its table
 * with its tile and UVs (uv[6] the cell or quadrant, which picks its V
 * scroll); the terrain_cells vertex stage builds the nodes, snap, key and
 * straddler facing from the transformed corners (c3 = c2) and the node
 * colours from near_rgb[0] (A), [1] (B) and [3] (C, word 1 byte 0).
 * Tier-4 records go where tgpu_bake_cells4 writes its cells: the CN family
 * of a near-variant sector, the CA family of a poly-anim one and, for a face
 * the pin sends to the cells, of every sector's plain chunk set. A tier-2
 * split draws wherever the 2x2 quad subdivide does, which every chunk set
 * bakes, so its record goes to the CA family of every sector and the CN
 * family of a near-variant one.
 * @param cpu The guest CPU state.
 * @param buf The sector's bake buffers, indexed by arena.
 * @param ab Nonzero for an anim sector.
 * @param with_var 1 when the sector carries near-variant chunks.
 * @param pin4 1 for a face routed to the cells at every distance.
 * @param T The face template.
 * @param nearC The raw near corner colours.
 * @param F The face.
 * @param hqBase The face's HQ record address.
 * @param hp_bucket The material bucket written into mat0 from bit 24.
 * @param hdfold 1 to fold each tile onto its HD replacement.
 * @param tier2 1 for the tier-2 split, 0 for tier 4.
 * @param fail Set to 5 when a tile cannot be folded.
 * @return 0, or -1 on a failed fold or push. */
static int tgpu_bake_tri(CPUState* cpu, tgpu_buf* buf, int ab, int with_var, int pin4,
                         const tgpu_face_t* T, const uint32_t nearC[4],
                         const tsface_t* F, uint32_t hqBase, int hp_bucket,
                         int hdfold, int tier2, int* fail) {
    rhi_terrain_cell_anim R;
    tgpu_cell_rec_init(&R, T, nearC);
    rhi_terrain_cell_tile tiles[16];
    uint8_t cls[16], blended[16];
    memset(tiles, 0, sizeof tiles); memset(cls, 0xFF, sizeof cls); memset(blended, 0, sizeof blended);
    const tn_nearface nf = { .hqBase = hqBase, .texid = F->hq_idx, .rot = F->trirot & 3 };
    const int n = tier2 ? 4 : 16;
    for (int i = 0; i < n; i++) {
        nt_sub sub;
        nt_sub_of(cpu, &nf, tier2, i, 0, &sub);
        if (tgpu_tri_tile(cpu, sub.e, nf.rot, sub.o, sub.cell, F->abe, hp_bucket, hdfold,
                          &tiles[i], &cls[i], &blended[i]) != 0) {
            *fail = 5;
            return -1;
        }
    }
    return tgpu_push_cell_families(buf, ab, with_var, pin4 || tier2, &R, tiles, cls, blended,
                                   F->hq_idx, (1u << 24) | ((uint32_t)tier2 << 25));
}

/** @brief Tests whether an HP face carries near variants.
 *
 * True when the level has an HQ table and word 3 does not exclude the face
 * (near_flag_excl): every such quad, and a triangle on a game whose tier-4
 * or tier-2 triangle split the port draws.
 * @param F The face.
 * @param hqTable The HQ record table's guest address, or 0.
 * @return 1 when eligible, else 0. */
static int tgpu_hq_elig(const tsface_t* F, uint32_t hqTable) {
    if (hqTable == 0 || tface_near_excl(F->w3)) return 0;
    return F->quad || game_layout.terrain.hq_tri4_cells != 0
        || game_layout.terrain.hq_tri2_tiles != 0;
}

/** @brief Bakes a triangle's split variants: tier-4 records where they can draw, tier-2 records where the game has that pass.
 *
 * Tier-4 records bake on a game with hq_tri4_cells when the classifier can
 * route the face to tier 4 (near_flag_tier2 clear, tface_cells_ok) and a
 * family draws them (ab, with_var or pin4); tier-2 records bake on a game
 * with hq_tri2_tiles.
 * @param cpu The guest CPU state.
 * @param buf The sector's bake buffers, indexed by arena.
 * @param ab Nonzero for an anim sector.
 * @param with_var 1 when the sector carries near-variant chunks.
 * @param pin4 1 for a face routed to the cells at every distance.
 * @param T The face template.
 * @param nearC The raw near corner colours.
 * @param F The face.
 * @param hqBase The face's HQ record address.
 * @param hp_bucket The material bucket written into mat0 from bit 24.
 * @param hdfold 1 to fold each tile onto its HD replacement.
 * @param fail Set to 5 when a tile cannot be folded.
 * @return 0, or -1 on a failed fold or push. */
static int tgpu_bake_tris(CPUState* cpu, tgpu_buf* buf, int ab, int with_var, int pin4,
                          const tgpu_face_t* T, const uint32_t nearC[4],
                          const tsface_t* F, uint32_t hqBase, int hp_bucket,
                          int hdfold, int* fail) {
    if (game_layout.terrain.hq_tri4_cells && !tface_near_tier2(F->w3) && (ab || with_var || pin4)
     && tface_cells_ok(F->hq_idx)
     && tgpu_bake_tri(cpu, buf, ab, with_var, pin4, T, nearC, F, hqBase, hp_bucket, hdfold, 0,
                      fail) != 0)
        return -1;
    if (game_layout.terrain.hq_tri2_tiles)
        return tgpu_bake_tri(cpu, buf, ab, with_var, pin4, T, nearC, F, hqBase, hp_bucket,
                             hdfold, 1, fail);
    return 0;
}

/** @brief Bakes an HQ-eligible face's near variants: a quad's 2x2 subdivide and 4x4 cells, a triangle's split records.
 *
 * A quad routed to the cells (pin4, tface_cells_pin) with near_flag_tier2
 * clear selects its cells at every distance, so its 2x2 is never drawn and
 * is not baked; a near_flag_tier2 quad keeps the 2x2 alone, and a quad
 * that may not take the cells (tface_cells_ok) bakes no cells.
 * @param cpu The guest CPU state.
 * @param buf The sector's bake buffers, indexed by arena.
 * @param ab OPTC_ARENA_ANIM_BASE for an anim sector, else 0.
 * @param with_var 1 when the sector carries near-variant chunks.
 * @param pin4 1 for a face routed to the cells at every distance.
 * @param T The face template.
 * @param k The face's four corners, i0..i3 order.
 * @param nearC The raw near corner colours.
 * @param F The face.
 * @param hqTable The HQ record table's guest address.
 * @param hp_bucket The face's material bucket.
 * @param hdfold 1 to fold each tile onto its HD replacement.
 * @param fail Set to 5 when a tile cannot be folded.
 * @return 0, or -1 on a failed fold or push. */
static int tgpu_bake_near(CPUState* cpu, tgpu_buf* buf, int ab, int with_var, int pin4,
                          const tgpu_face_t* T, const tgpu_corner_t k[4],
                          const uint32_t nearC[4], const tsface_t* F, uint32_t hqTable,
                          int hp_bucket, int hdfold, int* fail) {
    uint32_t hqBase = hqTable + (uint32_t)F->hq_idx * game_layout.terrain.hq_stride;
    if (!F->quad)
        return tgpu_bake_tris(cpu, buf, ab, with_var, pin4, T, nearC, F, hqBase, hp_bucket,
                              hdfold, fail);
    const int cells_only = pin4 && !tface_near_tier2(F->w3);
    if (!cells_only &&
        tgpu_bake_hq2(cpu, buf, ab, with_var, T, k, nearC, F, hqBase, hp_bucket, hdfold, fail) != 0)
        return -1;
    if (!tface_cells_ok(F->hq_idx)) return 0;
    return tgpu_bake_cells4(cpu, buf, ab, with_var, pin4, T, nearC, F, hqBase, hp_bucket,
                            hdfold, fail);
}

/** @brief Tests whether a face is baked with FF_KEYCELLS, keying each fragment by its own drain sub-cell.
 *
 * Keyed is a face whose drawn triangles may span drain sub-cells of different keys
 * (face_flags bit 21, FF_KEYCELLS: the vertex stage keys each fragment by
 * its own sub-cell, terrain.vert quad_cells_tab / tri_cells_tab), from the
 * variants the port can draw for it: a quad whose drawn near variant is the
 * 2x2 while the drain takes tier 4 (no 4x4 cells: none located, or the
 * record refused by tface_cells_ok; not a quad the pin sends to the cells),
 * a triangle drawn as the tier-2 split while the drain takes tier 4 (tier-4
 * cells refused), and a triangle drawn whole. near_flag_tier2 faces never
 * take tier 4, near_flag_excl faces are never near, a face whose ABE bit is
 * set keys no sub-cell (the walk's !abe gate), and an env-anim colour
 * sector bakes no near content. A game that does not locate both
 * hq_tri4_nodes and hq_tri2_nodes keys no face.
 * @param F The face.
 * @param hq_elig The face's tgpu_hq_elig verdict.
 * @param pin4 1 for a face routed to the cells at every distance.
 * @param canim_only 1 for an env-anim colour sector that is not poly-anim.
 * @return 1 when the face keys per sub-cell, else 0. */
static int tgpu_key_cells(const tsface_t* F, int hq_elig, int pin4, int canim_only) {
    /* An env-anim colour sector bakes no near content. */
    if (canim_only || F->abe || tface_near_excl(F->w3)) return 0;
    if (!game_layout.terrain.hq_tri4_nodes || !game_layout.terrain.hq_tri2_nodes) return 0;
    if (F->quad) {
        const int cells = game_layout.terrain.hq_cells4 && tface_cells_ok(F->hq_idx);
        return hq_elig && !tface_near_tier2(F->w3) && !pin4 && !cells;
    }
    if (!hq_elig || (!game_layout.terrain.hq_tri4_cells && !game_layout.terrain.hq_tri2_tiles))
        return 1;
    return game_layout.terrain.hq_tri4_cells && game_layout.terrain.hq_tri2_tiles &&
           !tface_near_tier2(F->w3) && !tface_cells_ok(F->hq_idx);
}

/** @brief Allocates and uploads one built sector's chunks, recording them in its slot.
 *
 * All or nothing: an arena that is full rolls back the chunks already
 * allocated for this sector. Cell arenas upload records; the anim and
 * near-variant arenas also upload their parallel previous-tick and context
 * records.
 * @param S The sector's slot; receives each arena's first chunk, count and
 *        buffer bit.
 * @param buf The sector's bake buffers, indexed by arena.
 * @return 0, or -1 when an arena is full (the arena-full claim). */
static int tgpu_upload_chunks(tgpu_slot_t* S, const tgpu_buf* buf) {
    uint32_t first[OPTC_ARENA_COUNT] = {0};
    int bufi[OPTC_ARENA_COUNT] = {0};
    uint32_t bits = 0;
    int a;
    for (a = 0; a < OPTC_ARENA_COUNT; a++) {
        if (buf[a].n == 0) continue;
        if (openpete_tercache_chunk_alloc(a, buf[a].n, &first[a], &bufi[a]) != 0) break;
    }
    if (a < OPTC_ARENA_COUNT) {
        for (int b = 0; b < a; b++)
            if (buf[b].n) openpete_tercache_chunk_free(b, bufi[b], first[b], buf[b].n);
        return -1;
    }
    for (a = 0; a < OPTC_ARENA_COUNT; a++) {
        if (buf[a].n == 0) { S->count[a] = 0; continue; }
        if (OPTC_ARENA_IS_CELL(a))
            openpete_tercache_chunk_write_cells(a, bufi[a], first[a], buf[a].rec, buf[a].n);
        else
            openpete_tercache_chunk_write(a, bufi[a], first[a], buf[a].v, buf[a].n);
        if (buf[a].anim)
            openpete_tercache_chunk_write_prev(a, bufi[a], first[a], buf[a].pv, buf[a].n);
        if (buf[a].nearv)
            openpete_tercache_chunk_write_ctx(a, bufi[a], first[a], buf[a].cx, buf[a].n);
        S->first[a] = first[a]; S->count[a] = buf[a].n;
        bits |= (uint32_t)bufi[a] << a;
    }
    S->bufbit = bits;
    return 0;
}

/** @brief Tests whether a sector bakes into the anim arenas: poly-anim, or on the colour lists.
 * @param SC The decoded sector.
 * @return 1 when the sector bakes into the anim arenas, else 0. */
static int tgpu_sector_anim(const tscache_t* SC) {
    return SC->polyanim != 0 || tsc_canim(SC) != NULL;
}
/** @brief Marks the anim arenas' buffers to carry the parallel previous-tick records.
 * @param buf The sector's bake buffers, indexed by arena.
 * @param anim Nonzero marks the arenas from OPTC_ARENA_ANIM_BASE up to
 *        OPTC_ARENA_NEAR_BASE; zero leaves them unmarked. */
static void tgpu_buf_anim(tgpu_buf* buf, int anim) {
    for (int a = OPTC_ARENA_ANIM_BASE; anim && a < OPTC_ARENA_NEAR_BASE; a++)
        buf[a].anim = 1;
}

/** @brief Returns the previous-tick colours of four colour indices in one of a colour-anim slot's arrays.
 *
 * Without a slot, or before its first roll, the output is the current
 * colours; an index past the array keeps its current colour.
 * @param CA The sector's colour-anim slot, or NULL.
 * @param arr The array: 0 LP, 1 HP near, 2 HP far.
 * @param c0 The first colour index, a byte offset.
 * @param c1 The second colour index.
 * @param c2 The third colour index.
 * @param c3 The fourth colour index.
 * @param cur The four current colours.
 * @param out Receives the four previous-tick colours. */
static void tgpu_prev_cols(const canim_t* CA, int arr, uint32_t c0, uint32_t c1,
                           uint32_t c2, uint32_t c3, const uint32_t cur[4],
                           uint32_t out[4]) {
    const uint32_t co[4] = { c0, c1, c2, c3 };
    if (!CA || !CA->havePrev) { memcpy(out, cur, sizeof(uint32_t) * 4u); return; }
    const uint32_t base = arr == 0 ? 0u : CA->nLp + (arr == 2 ? CA->nHp / 2u : 0u);
    const uint32_t n    = arr == 0 ? CA->nLp : CA->nHp / 2u;
    for (int q = 0; q < 4; q++)
        out[q] = (co[q] / 4u < n) ? (CA->prev[base + co[q] / 4u] & 0xFFFFFFu) : cur[q];
}

/* The per-sector state an HP face bake reads (tgpu_bake_hp_face). */
typedef struct {
    CPUState* cpu;
    tgpu_buf* buf;
    int ab;                  /* arena family base: 0 or OPTC_ARENA_ANIM_BASE */
    int with_var;            /* bake near-variant chunks */
    int prev;                /* bake previous-tick corners from the set's prev bank */
    int hdfold;              /* fold each face onto its HD replacement */
    int canim_only;          /* an env-anim colour sector: no near variants */
    uint32_t base_ff;        /* face_flags slot and generation bits */
    uint32_t hqTable;
} tgpu_hpctx;

/* One set of HP faces in a sector's frame: its own tier or its borrowed faces. */
typedef struct {
    const tsvert_t* V;       /* vertex bank */
    const tsvert_t* Vprev;   /* previous-tick bank, or NULL */
    int nV;
    const uint8_t* nearCol;  /* host near / far colour banks, or NULL to read */
    const uint8_t* farCol;   /* the guest arrays at nearBase / farBase */
    uint32_t nearBase, farBase;
    const canim_t* CA;       /* colour-anim slot, or NULL */
    uint32_t ff;             /* extra face_flags bits */
} tgpu_hpset;

/** @brief Emits an HP face's flat-variant triangles with their texel UVs and HD-fold entries.
 *
 * A quad emits (i0, i1, i3) and (i1, i3, i2) with the GT4 UV corners
 * [TL, TR, BL, BR] on i0, i1, i3, i2; a triangle emits its corners with the
 * 2-bit UV orientation (triRot). Under the fold each emitted triangle of
 * both tile halves interns its replacement entry.
 * @param X The bake context.
 * @param T The face record; the fold entries are written into its material words.
 * @param k The corners in i0..i3 order; the UVs are written here.
 * @param F The face.
 * @param da The draw-class arena.
 * @param fail Receives 5 when a fold entry cannot be interned.
 * @return 0, or -1 when the bake must stop. */
static int tgpu_bake_hp_flat(const tgpu_hpctx* X, tgpu_face_t* T, tgpu_corner_t k[4],
                             const tsface_t* F, int da, int* fail) {
    if (F->quad) {
        /* GT4: i0=TL, i1=TR, i3=BL, i2=BR; UV [TL,TR,BL,BR]; diagonal i1-i3 */
        static const int uvc[4] = { 0, 1, 3, 2 };
        for (int q = 0; q < 4; q++) {
            k[uvc[q]].uf = (uint8_t)F->tu[q];  k[uvc[q]].vf = (uint8_t)F->tv[q];
            k[uvc[q]].un = (uint8_t)F->tuN[q]; k[uvc[q]].vn = (uint8_t)F->tvN[q];
        }
        if (X->hdfold) {   /* replacement entries per emitted tri, both halves */
            float uA[3] = { k[0].uf, k[1].uf, k[3].uf }, vA[3] = { k[0].vf, k[1].vf, k[3].vf };
            float uB[3] = { k[1].uf, k[3].uf, k[2].uf }, vB[3] = { k[1].vf, k[3].vf, k[2].vf };
            float nA[3] = { k[0].un, k[1].un, k[3].un }, wA[3] = { k[0].vn, k[1].vn, k[3].vn };
            float nB[3] = { k[1].un, k[3].un, k[2].un }, wB[3] = { k[1].vn, k[3].vn, k[2].vn };
            int hdF = tgpu_hdfold_face(&F->mat,  uA, vA, uB, vB);
            int hdN = tgpu_hdfold_face(&F->matN, nA, wA, nB, wB);
            if (hdF < 0 || hdN < 0) { *fail = 5; return -1; }
            T->matf1 |= (uint32_t)hdF << 16;
            T->matn1 |= (uint32_t)hdN << 16;
        }
        if (tgpu_emit_face_tri(X->buf, da, X->ab, X->with_var, T, k[0], k[1], k[3], 0) != 0) return -1;
        return tgpu_emit_face_tri(X->buf, da, X->ab, X->with_var, T, k[1], k[3], k[2], 1) != 0 ? -1 : 0;
    }
    /* GT3 with the 2-bit UV orientation (triRot) */
    static const int triRot[4][3] = {{0,1,2},{1,3,0},{3,2,1},{2,0,3}};
    const int* tc = triRot[F->trirot & 3];
    for (int q = 0; q < 3; q++) {
        k[q].uf = (uint8_t)F->tu[tc[q]]; k[q].vf = (uint8_t)F->tv[tc[q]];
        k[q].un = (uint8_t)F->tuN[tc[q]]; k[q].vn = (uint8_t)F->tvN[tc[q]];
    }
    if (X->hdfold) {
        float uA[3] = { k[0].uf, k[1].uf, k[2].uf }, vA[3] = { k[0].vf, k[1].vf, k[2].vf };
        float nA[3] = { k[0].un, k[1].un, k[2].un }, wA[3] = { k[0].vn, k[1].vn, k[2].vn };
        int hdF = tgpu_hdfold_face(&F->mat,  uA, vA, NULL, NULL);
        int hdN = tgpu_hdfold_face(&F->matN, nA, wA, NULL, NULL);
        if (hdF < 0 || hdN < 0) { *fail = 5; return -1; }
        T->matf1 |= (uint32_t)hdF << 16;
        T->matn1 |= (uint32_t)hdN << 16;
    }
    return tgpu_emit_face_tri(X->buf, da, X->ab, X->with_var, T, k[0], k[1], k[2], 0) != 0 ? -1 : 0;
}

/** @brief Points an imported sector's own HP bake at the source table and the auxiliary bank.
 *
 * Sets the face bit 11 (the borrowed-face bit the vertex stage reads for the
 * bank), the source table, no HD fold, and the source cell verdicts
 * (s_hq_bank); a scene sector keeps its context and clears s_hq_bank.
 * @param SC The sector.
 * @param X The bake context.
 * @param own The sector's HP set. */
static void tgpu_own_bank(const tscache_t* SC, tgpu_hpctx* X, tgpu_hpset* own) {
    s_hq_bank = SC->virt;
    if (!SC->virt) return;
    X->hqTable = TSRC_TEX_BASE;
    X->hdfold = 0;
    own->ff = 1u << 11;
}

/** @brief Bakes one HP face: its flat variant and, when HQ-eligible, its near variants.
 *
 * The face record carries the face's flags (the set's extra bits among
 * them), its material bucket, both tile halves' materials and its scroll
 * tile id; each corner carries its position, previous-tick position and
 * near and far colours, read from the set's host banks or its guest arrays.
 * An env-anim colour sector bakes no near variant: it is drawn from the
 * cache only while none of its faces can be near (tgpu_canim_band), so the
 * 2x2, the cells and the split-triangle records would occupy the anim
 * arenas and never draw.
 * @param X The bake context.
 * @param S The face set.
 * @param F The face; a face with an index outside the set's bank is skipped.
 * @param fail Receives the claim code when the bake must stop.
 * @return 0, or -1 when the bake must stop. */
static int tgpu_bake_hp_face(const tgpu_hpctx* X, const tgpu_hpset* S, const tsface_t* F, int* fail) {
    if (F->i0 >= S->nV || F->i1 >= S->nV || F->i2 >= S->nV || F->i3 >= S->nV) return 0;
    int hq_elig = tgpu_hq_elig(F, X->hqTable);
    const int pin4 = hq_elig && tface_cells_pin(F->hq_idx);
    const int key_cells = tgpu_key_cells(F, hq_elig, pin4, X->canim_only);
    s_tgpu_keycell_faces += key_cells;
    tgpu_face_t T;
    memset(&T, 0, sizeof(T));
    const int vi[4] = { F->i0, F->i1, F->i2, F->i3 };
    for (int q = 0; q < 4; q++) T.c[q] = tgpu_p4_of(&S->V[vi[q]]);
    T.face_flags = X->base_ff | S->ff
                 | (F->fogbias ? (1u << 10) : 0u)
                 | (F->dside       ? (1u << 12) : 0u)   /* dside */
                 | (F->flip ? (1u << 13) : 0u)
                 | (F->quad ? (1u << 14) : 0u)
                 | (tface_near_tier2(F->w3) ? (1u << 15) : 0u)
                 | (tface_near_excl(F->w3)  ? (1u << 16) : 0u)
                 | (hq_elig ? (1u << 17) : 0u)
                 | (pin4    ? (1u << 19) : 0u)      /* routed to the cells */
                 | ((uint32_t)tface_cells_ok(F->hq_idx) << 20)   /* may take the cells */
                 | ((uint32_t)key_cells << 21)   /* keys per drain sub-cell */
                 | (((uint32_t)F->otbias & 0x1Fu) << 24);
    /* Material bucket, keyed exactly as the walk's provenance tag is:
     * Tiledef id + env-rewrite membership. Both LOD halves of a face
     * share it (the selector keys on the Tiledef, not on the page), so
     * it rides mat_far0 alone and the near half needs no copy. */
    /* A borrowed face's Tiledef id names the source table: it keys no selector. */
    const int hp_bucket = openpete_mods_material_terrain_bucket(
        (F->tb == -1 || S->ff) ? -1 : (F->tb & 0x7F), F->tgen != 0, 0);
    T.matf0 = tgpu_pack_mat0(&F->mat) | ((uint32_t)hp_bucket << 24);
    T.matf1 = (uint32_t)(F->mat.clut_raw & 0xFFFF);
    T.matn0 = tgpu_pack_mat0(&F->matN); T.matn1 = (uint32_t)(F->matN.clut_raw & 0xFFFF);
    /* Scroll tile id (flags16 bits 5-12): the vertex stage adds the
     * per-build fractional V offset for this Tiledef (zero when the
     * tile isn't scrolling). */
    if (F->tb != -1)
        T.flags16 = (uint16_t)((((uint32_t)F->tb & 0x7Fu) << 5) | 0x1000u);
    uint32_t nearC[4], farC[4], pnearC[4], pfarC[4];
    const uint16_t co[4] = { F->c0, F->c1, F->c2, F->c3 };
    for (int q = 0; q < 4; q++) {
        nearC[q] = (S->nearCol ? t_le32(S->nearCol + co[q]) : rd(X->cpu, S->nearBase + co[q])) & 0xFFFFFFu;
        farC[q]  = (S->farCol  ? t_le32(S->farCol  + co[q]) : rd(X->cpu, S->farBase  + co[q])) & 0xFFFFFFu;
    }
    tgpu_prev_cols(S->CA, 1, co[0], co[1], co[2], co[3], nearC, pnearC);
    tgpu_prev_cols(S->CA, 2, co[0], co[1], co[2], co[3], farC, pfarC);
    tgpu_corner_t k[4];
    memset(k, 0, sizeof(k));
    for (int q = 0; q < 4; q++) {
        k[q].p = tgpu_p4_of(&S->V[vi[q]]);
        k[q].pp = (X->prev && S->Vprev) ? tgpu_p4_of(&S->Vprev[vi[q]]) : k[q].p;
        T.pc[q] = k[q].pp;
        k[q].rgb_near = nearC[q]; k[q].rgb_far = farC[q];
        k[q].prgb_near = pnearC[q]; k[q].prgb_far = pfarC[q];
    }
    if (tgpu_bake_hp_flat(X, &T, k, F, tgpu_arena_of(&F->mat), fail) != 0) return -1;
    if (hq_elig && !X->canim_only &&
        tgpu_bake_near(X->cpu, X->buf, X->ab, X->with_var, pin4, &T, k, nearC,
                       F, X->hqTable, hp_bucket, X->hdfold, fail) != 0)
        return -1;
    return 0;
}

/* The per-sector state an LP face bake reads (tgpu_bake_lp_face). */
typedef struct {
    const tsvert_t* V;           /* the LP vertex bank */
    const tsvert_t* Vprev;       /* the previous-tick bank, or NULL to bake prev == cur */
    int nV;                      /* the bank's vertex count */
    uint32_t colBase;            /* the LP colour array (guest or host-window address) */
    const canim_t* CA;           /* env-anim previous-tick colours, or NULL */
    int sec_index;               /* the authored-correction key; -1 for none */
} tgpu_lpset;

/** @brief Bakes one LP face into the LP arena (or its blend arena).
 *
 * The face's corners, flags (tier LP, fill tag, uncov and uncovX,
 * double-sided, quad, floor, admission, OT bias) and colours go into one or
 * two triangles, the quad split on the i1-i2 diagonal as the walk's LP emit
 * does. A face with an out-of-bank index is skipped, mirroring the walk.
 * @param cpu The guest CPU state.
 * @param buf The per-arena buffers.
 * @param ab The arena family base (anim twins or plain).
 * @param with_var 1 when near-variant chunks are baked.
 * @param base_ff The slot and generation face-flag bits.
 * @param lp_bucket The LP material bucket.
 * @param L The face's bank, colours and correction key.
 * @param F The face.
 * @param f The face's index in its list (the correction key).
 * @return 0 on success, -1 when an arena is full. */
static int tgpu_bake_lp_face(CPUState* cpu, tgpu_buf* buf, int ab, int with_var, uint32_t base_ff,
                             int lp_bucket, const tgpu_lpset* L, const tsface_t* F, int f) {
    if (F->i0 >= L->nV || F->i1 >= L->nV || F->i2 >= L->nV || F->i3 >= L->nV)
        return 0;   /* mirrors the walk's OOB skip */
    tgpu_face_t T;
    memset(&T, 0, sizeof(T));
    T.c[0] = tgpu_p4_of(&L->V[F->i0]); T.c[1] = tgpu_p4_of(&L->V[F->i1]);
    T.c[2] = tgpu_p4_of(&L->V[F->i2]); T.c[3] = tgpu_p4_of(&L->V[F->i3]);
    T.face_flags = base_ff | (1u << 9)                       /* tier LP */
                 | (F->gapfill ? (1u << 11) : 0u)
                 | ((uint32_t)F->uncov << 17)                /* LP uncovered */
                 | ((uint32_t)F->uncovX << 16)               /* ... counting borrowed HP */
                 | (F->dside   ? (1u << 12) : 0u)            /* LP w0&0x80 */
                 | (F->quad    ? (1u << 14) : 0u)
                 | (F->floorf  ? (1u << 18) : 0u)
                 | (((uint32_t)F->admit  & 0x1Fu) << 19)
                 | (((uint32_t)F->otbias & 0x1Fu) << 24);
    T.matf0 = tgpu_pack_mat0(&F->mat)    /* untextured (blend may be set) */
            | ((uint32_t)lp_bucket << 24);
    uint32_t rc[4];
    rc[0] = rd(cpu, L->colBase + F->c0) & 0xFFFFFFu;
    rc[1] = rd(cpu, L->colBase + F->c1) & 0xFFFFFFu;
    rc[2] = rd(cpu, L->colBase + F->c2) & 0xFFFFFFu;
    rc[3] = F->quad ? (rd(cpu, L->colBase + F->c3) & 0xFFFFFFu) : 0u;
    uint32_t prc[4];
    tgpu_prev_cols(L->CA, 0, F->c0, F->c1, F->c2, F->quad ? F->c3 : F->c2, rc, prc);
    uint32_t fixrgb;   /* authored-data corrections (unpainted vertex colours) - the
                        * walk applies these per emit; bake them (static per level). */
    if (L->sec_index >= 0 && lp_artfix_lookup(cpu, L->sec_index, f, &fixrgb))
        rc[0] = rc[1] = rc[2] = rc[3] = prc[0] = prc[1] = prc[2] = prc[3] = fixrgb;
    tgpu_corner_t k[4];
    memset(k, 0, sizeof(k));
    const int vi[4] = { F->i0, F->i1, F->i2, F->i3 };
    for (int q = 0; q < 4; q++) {
        k[q].p = tgpu_p4_of(&L->V[vi[q]]);
        k[q].pp = L->Vprev ? tgpu_p4_of(&L->Vprev[vi[q]]) : k[q].p;
        T.pc[q] = k[q].pp;
        k[q].rgb_far = k[q].rgb_near = rc[q];
        k[q].prgb_far = k[q].prgb_near = prc[q];
    }
    int da = F->mat.blend ? tgpu_arena_of(&F->mat) : (int)OPTC_ARENA_LP;
    /* LP quad: strip diagonal i1-i2 (no i2/i3 swap - see the LP emit) */
    if (tgpu_emit_face_tri(buf, da, ab, with_var, &T, k[0], k[1], k[2], 0) != 0) return -1;
    if (F->quad && tgpu_emit_face_tri(buf, da, ab, with_var, &T, k[1], k[2], k[3], 1) != 0) return -1;
    return 0;
}

/** @brief Builds and uploads all of one sector's chunks.
 *
 * Bakes the LP tier (the sector's own faces, then its borrowed LP faces)
 * and the HP tier's flat variant, then each HQ-eligible HP face's near
 * variants (tgpu_bake_near), into per-arena buffers, and uploads them with
 * tgpu_upload_chunks.
 * @param cpu The guest CPU state.
 * @param slot The sector's decode slot index.
 * @param SC The decoded sector.
 * @param hqTable The HQ record table's guest address, or 0.
 * @param sec_index The level sector index, the authored-correction key.
 * @param with_var 1 to bake near-variant chunks; ignored for an anim sector.
 * @return 0 on success; 1 when an arena is full or a buffer allocation
 *         fails; 5 when a face cannot be HD-folded (tgpu_hdfold_face). */
static int tgpu_build_sector(CPUState* cpu, int slot, const tscache_t* SC,
                             uint32_t hqTable, int sec_index, int with_var) {
    /* UV-scroll is mirrored: the accumulator-change tile generations rebuild
     * the bake per whole-texel advance (incl. the HQ-sibling-only ticks, via
     * tgenHq), and sub-tick fractional offsets ride the globals' per-tile dv
     * table (flags16 carries the face's tile id). */
    tgpu_buf buf[OPTC_ARENA_COUNT];
    memset(buf, 0, sizeof(buf));
    int ok = 0;
    int fail = 1;                     /* claim code when !ok (1 = arena-full) */
    const int hdfold = openpete_native_hdfold_on();
    uint32_t base_ff = ((uint32_t)slot & 0x1FFu)
                     | (((uint32_t)s_tgpu[slot].gen & 7u) << 29);
    /* Poly-anim sectors bake into the ANIM twin arenas with a parallel
     * previous-tick record per vertex (the terrain_anim vertex stage lerps
     * prev->cur at the build's alpha). Snap rule mirror: a sector with no
     * prev bank yet, or whose banks jumped (keyframe reset - the CPU skips
     * the lerp fill and draws cur), bakes prev == cur. */
    /* Env-anim colour sectors bake there too, their previous-tick colours
     * in the same record (tsc_canim; positions carry prev == cur). */
    const canim_t* CA = tsc_canim(SC);
    const int anim = tgpu_sector_anim(SC);
    const int canim_only = CA != NULL && !SC->polyanim;
    const int ab = anim ? (int)OPTC_ARENA_ANIM_BASE : 0;
    /* Near-variant chunks are never poly-anim: those sectors keep the CPU
     * claim, so the two twin arena families are mutually exclusive (one
     * binding-1 twin per chunk - prev record OR member context). */
    if (anim) with_var = 0;
    if (with_var)
        for (int a = OPTC_ARENA_NEAR_BASE; a < OPTC_ARENA_CELL_NEAR_BASE; a++)
            buf[a].nearv = 1;
    for (int a = OPTC_ARENA_CELL_NEAR_BASE; a < OPTC_ARENA_COUNT; a++)
        buf[a].rsz = OPTC_ARENA_IS_CELL_ANIM(a) ? sizeof(rhi_terrain_cell_anim)
                                                : sizeof(rhi_terrain_cell);
    int vsnap = 1;
    tgpu_buf_anim(buf, anim);
    if (SC->polyanim) {
        const int nLp = (SC->lpV && SC->lpVprev) ? SC->nLpV : 0;
        const int nHp = (SC->hpV && SC->hpVprev) ? SC->nHpV : 0;
        const int havePrev = (!SC->lpV || SC->lpVprev) && (!SC->hpV || SC->hpVprev);
        const int jump = (nLp && tsc_vanim_jump(SC->lpVprev, SC->lpV, nLp)) ||
                         (nHp && tsc_vanim_jump(SC->hpVprev, SC->hpV, nHp));
        vsnap = !havePrev || jump;
    }

    /* ---- LP tier: untextured; drawn in WORLD and re-drawn in FILL ---- */
    /* Every LP face keys the same (untextured, LP tier), so its material
     * bucket is a per-sector constant. 0 = no material claims these faces. */
    const int lp_bucket = openpete_mods_material_terrain_bucket(-1, 0, 1);
    const tgpu_lpset own_lp = { SC->lpV, (anim && !vsnap) ? SC->lpVprev : NULL, SC->nLpV,
                                SC->lpColBase, CA, sec_index };
    for (int f = 0; SC->lpV && SC->lpF && f < SC->nLpF; f++)
        if (tgpu_bake_lp_face(cpu, buf, ab, with_var, base_ff, lp_bucket, &own_lp, &SC->lpF[f], f) != 0)
            goto done;
    if (SC->imp && SC->imp->lpV && SC->imp->lpF) {
        /* Borrowed LP faces: the source LP colours through the host window,
         * no previous-tick state, no authored correction. */
        const tsimp_t* I = SC->imp;
        const tgpu_lpset bor_lp = { I->lpV, NULL, I->nLpV, I->lpColBase, NULL, -1 };
        for (int f = 0; f < I->nLpF; f++)
            if (tgpu_bake_lp_face(cpu, buf, ab, with_var, base_ff, lp_bucket, &bor_lp, &I->lpF[f], f) != 0)
                goto done;
    }

    /* ---- HP tier: flat variant + the HQ 2x2 subdivide variant; the
     * sector's own faces, then the faces it borrows from the HP source
     * level (host colours, no previous-tick state: FF_BORROWED) ---- */
    tgpu_hpctx X = { cpu, buf, ab, with_var, anim && !vsnap, hdfold, canim_only, base_ff, hqTable };
    tgpu_hpset own = { SC->hpV, SC->hpVprev, SC->nHpV, NULL, NULL, SC->nearBase, SC->farBase, CA, 0u };
    tgpu_own_bank(SC, &X, &own);
    for (int f = 0; SC->hpV && SC->hpF && f < SC->nHpF; f++)
        if (tgpu_bake_hp_face(&X, &own, &SC->hpF[f], &fail) != 0) goto done;
    s_hq_bank = 0;
    if (SC->imp) {
        const tsimp_t* I = SC->imp;
        const tgpu_hpset bor = { I->V, NULL, I->nV, I->nearCol, I->farCol, 0u, 0u, NULL, 1u << 11 };
        /* The source table through the host window; no HD fold (pack files
         * name the scene's VRAM, not the auxiliary bank). */
        tgpu_hpctx XB = X;
        XB.hqTable = TSRC_TEX_BASE;
        XB.hdfold = 0;
        s_hq_bank = 1;
        for (int f = 0; f < I->nF; f++) {
            if (tgpu_bake_hp_face(&XB, &bor, &I->F[f], &fail) != 0) goto done;
        }
        s_hq_bank = 0;
    }

    /* ---- upload ---- */
    ok = tgpu_upload_chunks(&s_tgpu[slot], buf) == 0;
done:
    s_hq_bank = 0;
    for (int a = 0; a < OPTC_ARENA_COUNT; a++) {
        free(buf[a].v); free(buf[a].pv); free(buf[a].cx); free(buf[a].rec);
    }
    return ok ? 0 : fail;
}

/** @brief Tests whether any texture-pack file touches the texel box of either tile half of any of a set of HP faces.
 *
 * @param F0 The faces.
 * @param n The face count.
 * @return 1 when a pack file overlaps a face, else 0. */
static int tgpu_pack_scan_faces(const tsface_t* F0, int n) {
    for (int f = 0; f < n; f++) {
        const tsface_t* F = &F0[f];
        for (int half = 0; half < 2; half++) {
            const tmat*  m  = half ? &F->matN : &F->mat;
            const float* tu = half ? F->tuN : F->tu;
            const float* tv = half ? F->tvN : F->tv;
            if (!m->textured) continue;
            float u0 = tu[0], v0 = tv[0], u1 = u0, v1 = v0;
            for (int k = 1; k < 4; k++) {
                if (tu[k] < u0) u0 = tu[k];
                if (tu[k] > u1) u1 = tu[k];
                if (tv[k] < v0) v0 = tv[k];
                if (tv[k] > v1) v1 = tv[k];
            }
            texture_pack_id id;
            if (!texture_pack_resolve(m->tpx, m->tpy, m->clx, m->cly, m->bpp,
                                      (int)u0, (int)v0, (int)u1, (int)v1, &id))
                continue;
            if (texture_pack_overlaps(&id)) return 1;
        }
    }
    return 0;
}

/** @brief Tests whether any texture-pack file touches the texel box of any of a sector's HP faces.
 *
 * Both tiledef halves - the walk picks far/near per present, and draws query
 * per-tri sub-boxes, so the FACE box is tested by intersection
 * (texture_pack_overlaps), the conservative cover of every draw-time
 * containment query. LP is untextured, so HP faces only. A resolve MISS
 * (texture not yet uploaded) is a consistent non-claim: the draw path's
 * resolve misses identically, and the upload bumps the registry generation,
 * which re-runs this scan. Serial prepass only (the resolve pipeline is not
 * audited for concurrent callers, the same rule as the LP-grade pre-warm).
 * @param SC The decoded sector.
 * @return 1 when a pack file overlaps a face, else 0. */
static int tgpu_pack_scan(const tscache_t* SC) {
    return !SC->virt && SC->hpF && tgpu_pack_scan_faces(SC->hpF, SC->nHpF);
}

/** @brief Counts a GPU-owned sector on the colour lists, and whether its last roll moved a colour.
 *
 * The OPENPETE_TERRAIN_GPU_CENSUS counters s_tgpu_canim_walk and
 * s_tgpu_canim_mv; the second is the engagement of the vertex stage's
 * colour lerp.
 * @param SC The decoded sector.
 * @param on Nonzero while the census is enabled; zero counts nothing. */
static void tgpu_canim_census(const tscache_t* SC, int on) {
    const canim_t* CA = on ? tsc_canim(SC) : NULL;
    s_tgpu_canim_walk += (CA != NULL);
    s_tgpu_canim_mv += (CA != NULL && canim_maxd(CA) > 0);
}

/** @brief Records a slot's colour-list membership and colour roll at bake, the values tgpu_canim_stale compares.
 *
 * An env-anim colour sector's bake is stale when the sector joined or left
 * the colour lists since, or its colour slot rolled a new (prev, cur) pair.
 * @param T The slot.
 * @param SC The decoded sector. */
static void tgpu_canim_latch(tgpu_slot_t* T, const tscache_t* SC) {
    const canim_t* CA = tsc_canim(SC);
    T->bakeCanim = (uint8_t)(CA != NULL);
    T->canimRoll = CA ? CA->rollPres : 0u;
}
/** @brief Tests whether a slot's baked colour-anim state differs from the sector's current state.
 *
 * The bake is stale when the sector's membership in the colour lists
 * (tsc_canim non-NULL) differs from bakeCanim, or when its colour roll
 * (rollPres) differs from the canimRoll that tgpu_canim_latch stored.
 * @param T The slot.
 * @param SC The decoded sector.
 * @return 1 when the bake is stale, else 0. */
static int tgpu_canim_stale(const tgpu_slot_t* T, const tscache_t* SC) {
    const canim_t* CA = tsc_canim(SC);
    if ((CA != NULL) != (T->bakeCanim != 0)) return 1;
    return CA && T->canimRoll != CA->rollPres;
}

/** @brief Validates or rebuilds a sector's chunks and decides whether the GPU may draw it this present.
 *
 * A slot without near-variant chunks rebuilds to add them when want_near
 * is set, and keeps them for the rest of the sector's residency. A valid
 * result stamps the slot's ownFrame.
 * @param cpu The guest CPU state.
 * @param SC The decoded sector.
 * @param hqTable The HQ record table's guest address, or 0.
 * @param sec_index The level sector index.
 * @param want_near 1 when this present needs the near-variant (clip-fan)
 *        chunks.
 * @return 0 when the GPU may draw the sector; otherwise the claim reason
 *         that sends it back to the CPU walk: 1 an invalid slot or an
 *         arena-full build, 3 an unmapped colour range, 4 a slot another
 *         sector already owns this present, 5 a texture-pack claim or an
 *         HD-fold unbakeable face. */
static int tgpu_ensure(CPUState* cpu, const tscache_t* SC, uint32_t hqTable,
                       int sec_index, int want_near) {
    int slot = (int)(SC - s_tsc);
    /* Backstop only: tsc_get never places a sector in OPTC_SLOT_DEAD (its
     * 9-bit face_flags id is the dead-stamp sentinel, so baked chunks for it
     * would be killed as dead). */
    if (slot < 0 || slot >= (int)TSC_SLOTS || slot == (int)OPTC_SLOT_DEAD) return 1;
    tgpu_slot_t* T = &s_tgpu[slot];
    /* Decode-cache slot CONTESTED this build: two visible sectors share one
     * probe window, and the decode cache re-resolved this slot to a second
     * sector after the first already validated as GPU-owned. Stealing the
     * slot here would free the first sector's chunks and republish the
     * slot's origin AFTER its face walk was skipped - the first sector
     * simply vanishes (the gnorc_cove water-border wedge). The loser walks
     * CPU instead; the first keeper's retained state stays intact. */
    if (T->s != SC->s && T->s != 0 && T->ownFrame == openpete_frame) return 4;
    /* Pack/selector claims (tier 1 of the exclusion->claim->fold ladder),
     * latched per slot and re-scanned only when an input generation moves.
     * Checked BEFORE the dirty build: a claimed sector renders via the CPU
     * walk, so validating or rebuilding chunks for it would be waste - its
     * existing chunks stay allocated but unpublished (SF_DRAWN clear) until
     * the claim lifts, when the normal dirty check below takes over. */
    if (T->scanDecodeGen != SC->decodeGen || T->scanTexGen != SC->texGen) {
        T->scanDecodeGen = SC->decodeGen; T->scanTexGen = SC->texGen;
        T->tpGen = 0; T->selEpoch = 0;      /* force both rescans */
    }
    {
        uint32_t g = texture_pack_generation();
        if (openpete_native_hdfold_on()) {
            /* HD fold: matched faces bake replacement entries instead of
             * claiming (tgpu_build_sector interns per emitted tri; an
             * unbakeable face returns the tier-1 claim from the build).
             * A moved registry re-bakes so newly-loaded files take. */
            T->tpClaim = 0;
            if (T->tpGen != g) T->tpGen = g;
        } else {
            if (T->tpGen != g) {
                T->tpGen = g;
                T->tpClaim = texture_pack_replacements_active()
                           ? (uint8_t)tgpu_pack_scan(SC) : 0;
            }
            if (T->tpClaim) return 5;
        }
    }
    /* Mod materials no longer claim: a face carries its material's banked
     * bucket, and the retained draw issues a pass per bucket. The epoch is
     * still latched here because the BAKE depends on it - a selector set
     * that moved leaves the stamped buckets naming the wrong pass. */
    T->selEpoch = openpete_mods_material_terrain_epoch();
    /* Near-variant chunks: kept for as long as the slot holds this sector, so
     * a sector oscillating across the near threshold re-bakes at most once. */
    int with_var = want_near || (T->bakeVar && T->s == SC->s);
    const uint32_t pinKey = tgpu_pin_key();
    int dirty = (with_var && !T->bakeVar)
             || (T->bakePinKey != pinKey)
             || (T->s != SC->s) || (T->decodeGen != SC->decodeGen)
             || (T->texGen != SC->texGen) || (T->vertGen != SC->vertGen)
             /* poly-anim: every canonical roll re-bakes (prev,cur) - the
              * content hash alone would leave a paused anim's chunk lerping
              * its last step forever. Sub-tick builds don't roll, so this
              * stays clean between ticks. */
             || (SC->polyanim && T->animRoll != SC->vrollPres)
             /* env-anim colour: joining or leaving the colour lists moves
              * the sector between arena families, and each roll re-bakes
              * its (prev, cur) colour pair for the same reason. */
             || tgpu_canim_stale(T, SC)
             /* HD fold: the pack registry moved since this bake (files
              * uploaded/cleared) - re-intern the sector's entries; a
              * fold-claimed sector retries when the fold state moved
              * (a queue drain can unblock its overflow). */
             || (openpete_native_hdfold_on() &&
                 (T->bakeTpGen != T->tpGen ||
                  (T->unbuildable == 5 &&
                   T->bakeHdEpoch != openpete_native_hdfold_epoch())))
             /* Material buckets are baked into the faces, so a selector set
              * that moved leaves them naming the wrong pass. */
             || (T->bakeSelEpoch != T->selEpoch)
             || tgpu_claim_retry(T);
    if (!dirty && T->lastColFrame != openpete_frame) {
        /* Colour liveness once per VBL (colours can only change per tick). */
        uint32_t h;
        if (tgpu_col_hash(SC, &h) != 0) return 3;
        T->lastColFrame = openpete_frame;
        if (h != T->colHash) { dirty = 1; T->colHash = h; }
    }
    static int s_tcdbg = -1;
    ENV_FLAG_LATCH(s_tcdbg, "OPENPETE_TC_DBG");
    if (dirty) {
        if (s_tcdbg)
            fprintf(stderr, "[tcf] rebuild slot=%d sec=%u gen=%u->%u stamp=%08x var=%d->%d s=%u->%u vert=%u->%u tex=%u->%u col=%d freed=%u/%u\n",
                    slot, (unsigned)SC->s, T->gen, (T->gen + 1u) & 7u, openpete_native_scene_build_stamp(),
                    T->bakeVar, with_var, (unsigned)T->s, (unsigned)SC->s, T->vertGen, SC->vertGen, T->texGen, SC->texGen,
                    T->lastColFrame == openpete_frame, T->first[0], T->count[0]);
        tgpu_free_slot(T);
        T->gen = (uint8_t)((T->gen + 1u) & 7u);
        T->s = SC->s;
        T->decodeGen = SC->decodeGen; T->texGen = SC->texGen; T->vertGen = SC->vertGen;
        T->animRoll = SC->vrollPres;
        tgpu_canim_latch(T, SC);
        T->bakeTpGen = T->tpGen;
        T->bakeHdEpoch = openpete_native_hdfold_epoch();
        T->bakeAllocEpoch = openpete_tercache_alloc_epoch();
        T->bakeSelEpoch = T->selEpoch;
        T->lastColFrame = openpete_frame;
        if (tgpu_col_hash(SC, &T->colHash) != 0) { T->unbuildable = 3; return 3; }
        T->bakeVar = (uint8_t)(with_var != 0);
        T->bakePinKey = pinKey;
        T->unbuildable = tgpu_build_sector(cpu, slot, SC, hqTable, sec_index, with_var);
        s_tgpu_rebuilds++;
    }
    if (T->unbuildable == 0) T->ownFrame = openpete_frame;
    return T->unbuildable;
}

/** @brief Formats one per-arena census column in OPTC_ARENA_* order.
 *
 * Writes "n0,n1,..." or, with den, "n0/d0,n1/d1,..."; output that would
 * overflow is truncated to cap - 1 bytes, possibly inside an entry.
 * @param out Receives the NUL-terminated column.
 * @param cap The capacity of out in bytes.
 * @param num The per-arena numerators.
 * @param den The per-arena denominators, or NULL. */
static void tgpu_fmt_arenas(char* out, int cap, const uint32_t* num, const uint32_t* den) {
    int at = 0;
    for (int a = 0; a < OPTC_ARENA_COUNT && at < cap - 1; a++) {
        int n = den
            ? snprintf(out + at, (size_t)(cap - at), "%s%u/%u", a ? "," : "", num[a], den[a])
            : snprintf(out + at, (size_t)(cap - at), "%s%u", a ? "," : "", num[a]);
        if (n < 0 || n >= cap - at) { at = cap - 1; break; }
        at += n;
    }
    out[at] = 0;
}

/** @brief Prints one [ter-gpu] line of the retained terrain cache's arena census to stderr.
 *
 * The line carries the per-arena high-water marks against their capacities,
 * the sectors each arena claimed back for want of room, the overflow
 * buffers each arena has taken on, the device bytes the live buffers hold,
 * the level-edge replacement's engagement (service calls / requests
 * fulfilled), the retired draws, the rebake count, the instanced
 * cell-record draws with the record instances they covered, and the faces
 * baked to key per drain sub-cell (FF_KEYCELLS). A `high` equal to its
 * `cap` with no overflow behind it is a clamp, not a demand figure. */
static void tgpu_arena_census_line(void) {
    openpete_tercache_stats_t st;
    openpete_tercache_get_stats(&st);
    char hwm[768], afail[384], ovf[384];
    tgpu_fmt_arenas(hwm, (int)sizeof hwm, st.high, st.cap);
    tgpu_fmt_arenas(afail, (int)sizeof afail, st.alloc_fail, NULL);
    tgpu_fmt_arenas(ovf, (int)sizeof ovf, st.overflow_adopted, NULL);
    fprintf(stderr, "[ter-gpu f=%u] hwm=%s afail=%s ovf=%s caps=%.1fMiB svc=%u/%u "
            "draws-retired=%u rebake=%ld cell-draws=%u/%llu keycell-faces=%ld\n",
            openpete_frame, hwm, afail, ovf,
            (double)st.cap_bytes / (1024.0 * 1024.0),
            st.svc_calls, st.svc_fulfilled, st.draws_retired, s_tgpu_rebake,
            st.cell_draws, (unsigned long long)st.cell_instances, s_tgpu_keycell_faces);
}

/** @brief Registers the arena census line to print once more at process exit.
 *
 * Registers on the first call only, so a run shorter than the census
 * divider still reports its peak. */
static void tgpu_arena_census_at_exit(void) {
    static int armed;
    if (armed) return;
    armed = 1;
    atexit(tgpu_arena_census_line);
}

/** @brief Prints and resets the retained-cache claim counters on every 120th call under OPENPETE_TERRAIN_GPU_DBG.
 *
 * The variable is latched on the first call; without it the function
 * returns. Registers the arena census line to print at exit. On the 120th
 * call it prints one [ter-gpu] line (sector, claim-reason and rebuild
 * counters plus the arena statistics of openpete_tercache_get_stats), the
 * arena census line, and zeroes the counters. terrain_build calls it once
 * per build. */
static void tgpu_census(void) {   /* OPENPETE_TERRAIN_GPU_DBG: ~1/s */
    static int on = -1, n;
    ENV_FLAG_LATCH(on, "OPENPETE_TERRAIN_GPU_DBG");
    if (!on) return;
    tgpu_arena_census_at_exit();
    if (++n < 120) return;
    n = 0;
    openpete_tercache_stats_t st;
    openpete_tercache_get_stats(&st);
    fprintf(stderr, "[ter-gpu f=%u] sectors=%ld nvar=%ld claims=%ld (near=%ld portal=%ld "
            "anim=%ld scroll=%ld fill=%ld arena=%ld col=%ld slot=%ld tp=%ld "
            "nearb=%ld nodraw=%ld prim=%ld pcov=%ld) rebuilds=%ld "
            "chunks=%u verts=%u/%u draws=%u frees-pend=%u\n",
            openpete_frame, s_tgpu_secs, s_tgpu_nvsecs, s_tgpu_claims,
            s_tgcl_near, s_tgcl_portal, s_tgcl_anim, s_tgcl_scroll,
            s_tgcl_fill, s_tgcl_build, s_tgcl_colour, s_tgcl_slot,
            s_tgcl_texpack, s_tgcl_nearb, s_tgcl_nodraw,
            s_tgcl_prim, s_tgcl_pcov,
            s_tgpu_rebuilds,
            st.chunks, st.verts_used, st.verts_cap, st.draws_last_present,
            st.frees_pending);
    tgpu_arena_census_line();
    s_tgpu_secs = s_tgpu_claims = s_tgpu_rebuilds = s_tgpu_nvsecs = 0;
    s_tgcl_near = s_tgcl_portal = s_tgcl_anim = s_tgcl_scroll = 0;
    s_tgcl_fill = s_tgcl_build = s_tgcl_colour = s_tgcl_slot = 0;
    s_tgcl_texpack = s_tgcl_nearb = s_tgcl_nodraw = 0;
    s_tgcl_prim = s_tgcl_pcov = 0;
}


/* Per-walk shared inputs, latched by the build prologue BEFORE dispatch and
 * read-only during the walk (the game fiber is parked for the
 * whole build; workers read guest RAM + latched state only, and write only
 * their own tn_wctx). */
typedef struct {
    CPUState* cpu;
    int M[3][3], Mbin[3][3];
    double camx, camy, camz;
    int fogRefBase, cullRefS2, texTblMode;
    double cull_wx, cull_wy;            /* sphere-cull half-width scales (1.0 = 4:3) */
    int tier_faithful, lod_handoff;
    uint32_t hqTable;
    const uint16_t* visidx; int nvis;
    const tscache_t** SC;                /* prepass-resolved; NULL = skip */
    const uint32_t* secptr;              /* raw sector pointers (debug identity) */
    /* mid-walk env toggles, hoisted from the tn_env_vbl call sites (the
     * per-VBL re-latch is not thread-safe; the prologue latches per walk) */
    int holefill, lp_farcull, lp_admit, hp_drop;
    /* Per-sector verdicts + retained-GPU ownership, computed by the serial
     * prepass (one computation site; workers only read). Indexed by vi. */
    const struct tn_secverd_s* verd;
    const uint8_t* own;
} tn_wcfg;

/* Per-sector tier/fill verdicts (the walk head's sector-level block, hoisted
 * into the prepass so the GPU-claim decision and the face walk share ONE
 * computation). cbin stays double: the debug prints quote it raw. */
typedef struct tn_secverd_s {
    double  cbin;
    uint8_t drawHP, drawLP, lpFarOnly, lpGapOnly, lpUncovOnly, fillLP, secInLOD;
    uint16_t lpNearThr;                  /* LP near-outcode SZ3 bound this walk, 0 = no test */
} tn_secverd;
static tn_secverd s_pre_verd[1024];
static uint8_t    s_tgpu_own[1024];      /* GPU owns this sector this walk */

/* The sector header the tier verdicts read: the sector's own, or for a
 * sector that borrows from the HP source level (C->imp) the source
 * sector's, since the scene sector stands in for it whole. */
typedef struct {
    int cx, cy, cz, r;           /* bounding sphere centre and radius */
    int flags;                   /* header flags (thdr_flags) */
    int hasLP, hasHP;            /* the sector draws LP / HP faces at all */
    int lpNearSz;                /* tscache_t.lpNearSz */
} tn_sechdr;

/** @brief Fills the header the tier verdicts read for a sector.
 *
 * A sector without borrowed faces reads its own header. A sector with
 * borrowed faces reads the matched source sector's bounding sphere, flags
 * and LP near bound, and counts its borrowed faces as faces of their tier,
 * so it bins, culls and selects its tiers as the source level does the
 * sector it was cut from, in every LOD mode.
 * @param SC The sector's cache entry.
 * @param H Receives the header. */
static void sector_header(const tscache_t* SC, tn_sechdr* H) {
    const tsimp_t* I = SC->imp;
    H->cx = I ? I->centreX : SC->centreX;
    H->cy = I ? I->centreY : SC->centreY;
    H->cz = I ? I->centreZ : SC->centreZ;
    H->r = I ? I->radius : SC->radius;
    H->flags = I ? I->srcFlags : SC->flags;
    H->lpNearSz = I ? I->lpNearSz : SC->lpNearSz;
    H->hasLP = SC->nLpF > 0 || (I && I->nLpF > 0);
    H->hasHP = SC->nHpF > 0 || (I && I->nF > 0);
}

/** @brief Computes a distance-binning game's own HP and LP list verdicts for a sector.
 *
 * The draw's list binning, environment.s:800225A0-80022640: vz is the
 * bounding centre's view-Z through the binning matrix, clamped like IR3. A
 * sector lists LP while vz + radius - lp_near (plus lp_near_shift on a
 * sector with lp_near_shift_flag) is positive, and HP while vz - radius is
 * below hp_far (hp_far_tight on a sector with tight_flags); either list
 * also needs faces and its no-tier flag clear. A sector can land in both
 * lists where the two distance bands overlap.
 * @param H The sector header (sector_header).
 * @param cbin The bounding centre's view-Z through the binning matrix.
 * @param binHP Receives the HP list verdict.
 * @param binLP Receives the LP list verdict. */
static void sector_bin_lists(const tn_sechdr* H, double cbin, int* binHP, int* binLP) {
    const int flags = H->flags;
    double vz = ngte_ir_clamp(cbin);
    int r = H->r;
    int shift = (flags & (int)game_layout.terrain.lp_near_shift_flag)
                ? game_layout.terrain.lp_near_shift : 0;
    int hpFar = (flags & (int)game_layout.terrain.tight_flags)
                ? game_layout.terrain.hp_far_tight : game_layout.terrain.hp_far;
    *binLP = !(flags & (int)game_layout.terrain.no_lp_flag) && H->hasLP
             && (vz + r - game_layout.terrain.lp_near + shift > 0);
    *binHP = !(flags & (int)game_layout.terrain.no_hp_flag) && H->hasHP
             && (vz - r < hpFar);
}

/* The LP ride-along a sector the list verdict leaves LP-unlisted takes. */
enum { TN_RIDE_NONE, TN_RIDE_FARONLY, TN_RIDE_GAPONLY, TN_RIDE_UNCOVONLY };

/** @brief Picks the LP ride-along for a sector whose list verdict leaves its LP tier undrawn.
 *
 * A game whose draw does not bin by distance (terrain.hp_far 0) takes its
 * LOD ladder: the LOD handoff substitutes a drawn-HP sector's LP for its
 * far-dropped HP (far-only, outside the tier-faithful bin), else a sector
 * with frozen fill tags draws those faces (gap-only). A game whose draw
 * bins by distance keeps its list verdict, except under extended LOD
 * (openpete_no_lod), where the HP list holds every sector and the LP list
 * none: a drawn-HP sector the draw's own binning lists LP (binLP) and with
 * LP faces off its HP mesh and its borrowed HP faces (nUncovX) draws those
 * faces (uncov-only), the
 * geometry the guest draws only from the LP list. The binning gate keeps
 * the guest's near bound and, on a poly-anim sector, draws the LP bank only
 * while the guest re-packs it. A sector with no HP of its own, whose HP is
 * all borrowed, takes uncov-only without the binning gate: the guest draws
 * its whole LP bank wherever it draws the sector.
 * @param SC The sector's cache entry.
 * @param G The latched walk config.
 * @param drawHP The sector's HP verdict.
 * @param binLP The draw's own LP list verdict (sector_bin_lists; 0 on a
 *        game that bins by flags).
 * @return One of TN_RIDE_NONE, TN_RIDE_FARONLY, TN_RIDE_GAPONLY, TN_RIDE_UNCOVONLY. */
static int sector_lp_ride(const tscache_t* SC, const tn_wcfg* G, int drawHP, int binLP) {
    if (game_layout.terrain.hp_far)
        return (openpete_no_lod && drawHP && (binLP || SC->nHpF == 0) && SC->nUncovX > 0)
               ? TN_RIDE_UNCOVONLY : TN_RIDE_NONE;
    if (G->lod_handoff && !G->tier_faithful && drawHP) return TN_RIDE_FARONLY;
    if (s_gapfill_en && SC->nGapfill > 0) return TN_RIDE_GAPONLY;
    return TN_RIDE_NONE;
}

/** @brief Returns the SZ3 bound under which the LP walk tags a sector's vertices near, or 0.
 *
 * The binning sets the near-band entry bit on a sector whose clamped
 * bounding-centre view-Z minus radius is at most terrain.lp_near_band; the
 * LP vertex transform of such a sector tags each vertex whose SZ3 is below
 * the sector's bound (tscache_t.lpNearSz), and the face walk skips a face
 * whose corners are all tagged. A game without the outcode and a sector out
 * of the band return 0.
 * @param H The sector header (sector_header).
 * @param cbin The bounding centre's view-Z through the binning matrix.
 * @return The bound, or 0 when the walk tags no vertex of the sector. */
static int sector_lp_near_thr(const tn_sechdr* H, double cbin) {
    if (!H->lpNearSz) return 0;
    return (ngte_ir_clamp(cbin) - H->r - game_layout.terrain.lp_near_band <= 0)
           ? H->lpNearSz : 0;
}

/** @brief Computes one sector's tier and fill verdicts in the serial prepass.
 *
 * A pure function of the sector cache entry and the latched walk config,
 * computed once per sector so the GPU-claim decision and the face walk read
 * one result. The bounding centre of the sector's header (sector_header: the
 * source sector's for a sector with borrowed faces) goes through the
 * binning matrix (cbin its view-Z). Under the sphere cull a sector outside the displayed
 * half-widths draws neither list. A game whose draw bins by distance
 * (terrain.hp_far, LOD on) takes the draw's own list binning, where a sector
 * can land in both lists; otherwise the flags decide, with the tier-faithful
 * distance tests when enabled. A sector left LP-unlisted takes the LP
 * ride-along sector_lp_ride picks. fillLP marks an LP list kept for hole fill;
 * lpNearThr is the LP near-outcode bound (sector_lp_near_thr), kept only
 * while the sector's HP draws, since the dropped LP faces owe that cover. The rule
 * comments sit beside each decision in the code and in walk_sector.
 * @param SC The sector's cache entry.
 * @param G The latched walk config.
 * @param V Receives the verdicts. */
static void sector_verdicts(const tscache_t* SC, const tn_wcfg* G, tn_secverd* V) {
    const int noLP  = (int)game_layout.terrain.no_lp_flag;
    const int noHPf = (int)game_layout.terrain.no_hp_flag;
    const int forceLP = (int)game_layout.terrain.force_lp_flag;
    tn_sechdr H;
    sector_header(SC, &H);
    int flags = SC->flags;
    int nLpF = SC->nLpF, nHpF = SC->nHpF;
    double cbin, c1m, c2m;
    ngte_mac(G->Mbin, G->camy - H.cy, G->camz - H.cz, H.cx - G->camx, &c1m, &c2m, &cbin);
    int lodThr = G->fogRefBase >> 2;
    int drawHP, drawLP;
    int culled = 0;
    if (game_layout.terrain.sector_cull == TERRAIN_SECTOR_CULL_SPHERE) {
        /* The sphere test runs at the displayed half-widths: at the canonical
         * frame cull_wx/cull_wy are 1.0 and this is the draw's own verdict,
         * and where the wedge fill is active the side wedges keep the sectors
         * the 4:3 half-width would reject. */
        culled = tsec_sphere_cull(c1m, c2m, cbin, H.r, G->cull_wx, G->cull_wy);
    }
    int binHP = 0, binLP = 0;
    if (game_layout.terrain.hp_far) sector_bin_lists(&H, cbin, &binHP, &binLP);
    if (game_layout.terrain.hp_far && !openpete_no_lod) {
        drawHP = binHP; drawLP = binLP;
    } else {
        /* A sector's borrowed HP faces count as its HP; the no-HP flag that
         * gates them is the source sector's, since a scene sector cut down
         * to LP carries the flag for its emptied bank. */
        drawHP = (!(flags & noHPf) && nHpF > 0) || (SC->imp && !(SC->imp->srcFlags & noHPf));
        drawLP = !(flags & noLP) && nLpF > 0 && ((flags & forceLP) || !drawHP);
        if (G->tier_faithful) {
            drawHP = drawHP && ((double)lodThr > cbin - H.r);
            drawLP = !(flags & noLP) && nLpF > 0 &&
                     ((flags & forceLP) || (double)lodThr < cbin + H.r + 0x100);
        }
    }
    if (culled) { drawHP = 0; drawLP = 0; }
    int secInLOD = ((double)lodThr * (4.0 / s_fogscale) > cbin - H.r);
    const int ride = (!culled && !drawLP && !(flags & noLP) && nLpF > 0)
                     ? sector_lp_ride(SC, G, drawHP, binLP) : TN_RIDE_NONE;
    drawLP = drawLP || ride != TN_RIDE_NONE;
    int fillLP = G->holefill && !culled && !(flags & noLP) && nLpF > 0
                 && ((double)lodThr < cbin + H.r + 0x100)
                 && !(openpete_no_lod && drawHP);
    V->cbin = cbin;
    V->drawHP = (uint8_t)drawHP; V->drawLP = (uint8_t)drawLP;
    V->lpFarOnly = (uint8_t)(ride == TN_RIDE_FARONLY);
    V->lpGapOnly = (uint8_t)(ride == TN_RIDE_GAPONLY);
    V->lpUncovOnly = (uint8_t)(ride == TN_RIDE_UNCOVONLY);
    V->fillLP = (uint8_t)fillLP; V->secInLOD = (uint8_t)secInLOD;
    V->lpNearThr = (uint16_t)(drawHP ? sector_lp_near_thr(&H, cbin) : 0);
}

/** @brief Returns 1 when an LP face draws in the world pass under its sector's ride-along verdict.
 *
 * A gap-only sector draws its fill-tagged faces and an uncov-only sector
 * its faces off the HP mesh and the borrowed HP faces (uncovX; uncov-only
 * is an extended-LOD verdict); every other sector draws every LP face.
 * @param SV The sector's verdicts.
 * @param F The LP face.
 * @return 1 when the face draws, else 0. */
static int tn_lp_face_rides(const tn_secverd* SV, const tsface_t* F) {
    if (SV->lpGapOnly) return F->gapfill != 0;
    if (SV->lpUncovOnly) return F->uncovX != 0;
    return 1;
}

/** @brief Applies the draw's near rule for an LP face: the LP near outcode where it applies, else the authored admission.
 *
 * On a game with terrain.lp_near_sz the near outcode replaces the
 * admission for a face on its sector's HP mesh and borrowed HP faces
 * (uncovX clear; uncovX equals uncov without borrowed faces) while the
 * sector's HP draws (a nonzero bound, sector_verdicts): the face drops when
 * every corner's SZ3 is under the bound, so a dropped LP face always has its
 * HP surface drawn in its place. Every other face takes the authored
 * admission: with the admission on, slot = (four corner SZ3s summed, a triangle counting its
 * shared corner twice) >> 5 and s3 = (fogRefBase >> 5) - 0x20: the face
 * draws iff slot + 8*admit >= s3; a deficit of -31..-1 falls back to "draw
 * if any corner SZ3 >= (s3 - 8*admit)*8"; a deeper deficit drops.
 * @param SV The sector's verdicts.
 * @param F The LP face.
 * @param cz The corners' view-Z (SZ3 scale) in i0..i3 order.
 * @param quad Nonzero for a quad.
 * @param admit Nonzero when the authored admission is on.
 * @param fogRefBase The walk's fog reference.
 * @return The drop reason ("nearout", "admit"), or NULL when the face draws. */
static const char* tn_lp_face_near_drop(const tn_secverd* SV, const tsface_t* F,
                                        const double cz[4], int quad, int admit,
                                        int fogRefBase) {
    const int nc = quad ? 4 : 3;
    if (game_layout.terrain.lp_near_sz && SV->lpNearThr && !F->uncovX) {
        const double thr = (double)SV->lpNearThr;
        int under = 0;
        for (int k = 0; k < nc; k++) under += cz[k] < thr;
        return under == nc ? "nearout" : NULL;
    }
    if (!admit) return NULL;
    const double sz3 = quad ? cz[3] : cz[2];          /* .s tris sum the shared corner twice */
    const double slot = (cz[0] + cz[1] + cz[2] + sz3) / 32.0;
    const double s3v = (double)((fogRefBase >> 5) - 0x20);
    const double a1 = slot + 8.0 * (double)F->admit - s3v;
    if (a1 >= 0.0) return NULL;
    if (a1 <= -32.0) return "admit";
    const double thr = (s3v - 8.0 * (double)F->admit) * 8.0;
    for (int k = 0; k < nc; k++) if (cz[k] >= thr) return NULL;
    return "admit";
}

#define TN_MAX_WORKERS 8
/* Walked-face count below which the face walk runs inline instead of fanning
 * out - see the dispatch break-even derivation at the workpool call site. */
#define TN_FANOUT_MIN_FACES 400
static tn_wctx s_wctx[TN_MAX_WORKERS];
static mod_fvec s_wbuf[TN_MAX_WORKERS];  /* parallel-mode output runs (lazy) */
static const tscache_t* s_pre_SC[1024];
static uint32_t s_pre_sec[1024];
static struct { uint8_t worker; int start, count; } s_pre_rec[1024];

/** @brief Emits a triangle face the port draws whole.
 *
 * A, B and C are the face's corners i0, i1 and i2, placed at face-local
 * (0,0), (4,0) and (0,4). An opaque, unblended near HP triangle the drain
 * inserts as its tier-2 or tier-4 sub-triangles keys each fragment by the
 * sub-triangle containing it (tn_keycells_begin); this applies on a layout
 * with both split node tables.
 * @param W The walk context.
 * @param cpu The guest CPU state.
 * @param F The face.
 * @param hp 1 for a high-poly face.
 * @param straddle 1 when a corner is at or behind the eye.
 * @param m The face's material.
 * @param A The first corner.
 * @param B The second corner.
 * @param C The third corner. */
static void tn_flat_tri_emit(tn_wctx* W, CPUState* cpu, const tsface_t* F, int hp, int straddle,
                             const tmat* m, ngte_cvert A, ngte_cvert B, ngte_cvert C) {
    tn_fl(&A, 0.0f, 0.0f); tn_fl(&B, 4.0f, 0.0f); tn_fl(&C, 0.0f, 4.0f);
    tn_keycells K; K.n = 0;
    int tier2 = 0;
    if (hp && game_layout.terrain.hq_tri4_nodes && game_layout.terrain.hq_tri2_nodes &&
        !F->abe && !m->blend && face_is_near_canonical(A.m3, B.m3, C.m3, C.m3, F->w3)) {
        const tn_nearface nf = { .w3 = F->w3, .abe = F->abe, .straddle = straddle,
                                 .flip = F->flip, .dside = F->dside };
        tier2 = !face_drain_tier4(A.m3, B.m3, C.m3, C.m3, nf.w3);
        ngte_cvert N[15];
        nt4_grid(N, A, B, C);
        tn_tri_cell_keys(cpu, &nf, N, tier2, &K);
    }
    if (K.n) tn_keycells_begin(W, &K, tier2 ? 4 : 16, tier2 ? 3u : 2u);
    clip_push_tri(W, A, B, C, m);
    W->emit_keycells = 0;
}

/** @brief Returns the walk's near-path verdict for an HP face.
 *
 * The caller holds the facing and table gates. A quad is decided by
 * face_is_near. A triangle, on a game with the tier-2 triangle pass, is
 * decided by face_is_near over (A, B, C, C): the triangle classifier at
 * 0x80027168 is the quad one with C counted twice, so a near triangle takes
 * tier 4 or tier 2 exactly where a quad would. Otherwise face_tri4_routed
 * decides alone, and a near triangle outside tier 4 keeps the flat GT3 emit.
 * @param sv The sector's projected vertices.
 * @param F The face.
 * @param rec The face record's guest address, the key of the game's near queues.
 * @return Nonzero when the face takes the near path. */
static int tn_face_near(const lvert* sv, const tsface_t* F, uint32_t rec) {
    if (F->quad) return face_is_near(sv[F->i0].m3, sv[F->i1].m3, sv[F->i2].m3, sv[F->i3].m3, F->w3);
    if (game_layout.terrain.hq_tri2_tiles)
        return face_is_near(sv[F->i0].m3, sv[F->i1].m3, sv[F->i2].m3, sv[F->i2].m3, F->w3);
    return face_tri4_routed(sv[F->i0].m3, sv[F->i1].m3, sv[F->i2].m3, F->w3, rec, F->hq_idx);
}
/** @brief Returns the near-path verdict read off the game's filled queues, for OPENPETE_TERRAIN_NEARQ.
 *
 * A quad is near when it is in either queue; a triangle when it is in either
 * queue on a game with the tier-2 pass, or in the +0x1E00 queue on a game
 * without it that locates the tier-4 triangle cells.
 * @param quad 1 for a quad face.
 * @param rec The face record's guest address, the key of the queues.
 * @return Nonzero when the face is near. */
static int tn_queue_near(int quad, uint32_t rec) {
    if (quad || game_layout.terrain.hq_tri2_tiles) return is_deferred(rec);
    return game_layout.terrain.hq_tri4_cells != 0 && is_deferred4(rec);
}

/** @brief Emits one routed HP face on the near path.
 *
 * Reads the raw near colours of its corners (no DPCS fog on near cells),
 * through host pointers when host_col is set, then emits the quad's 2x2 or
 * 4x4 subdivide or the triangle's tier-4 or tier-2 split. Corners are in the
 * drain's grid order: TL = i0, TR = i1, BL = i3, BR = i2; a triangle's C is
 * i2 = i3 with the colour of word 1 byte 0 (c3). Counts near_q_agree or
 * near_c_only by the face's presence in the game's queue, and the dbg_spike
 * counters.
 * @param W The walk context.
 * @param cpu The guest CPU state.
 * @param sv The sector's projected vertices.
 * @param F The face.
 * @param host_col 1 to read colours through h_near.
 * @param h_near The near colour bank as a host pointer.
 * @param nearBase The near colour bank's guest address.
 * @param hqTable The HQ record table's guest address.
 * @param rec The face record's guest address, the key of the game's near queues.
 * @param straddle 1 when a corner is at or behind the eye. */
static void tn_near_face(tn_wctx* W, CPUState* cpu, const lvert* sv, const tsface_t* F,
                         int host_col, const uint8_t* h_near, uint32_t nearBase,
                         uint32_t hqTable, uint32_t rec, int straddle) {
    if (is_deferred(rec)) W->near_q_agree++; else W->near_c_only++;
    const uint16_t co[4] = { F->c0, F->c1, F->c2, F->c3 };
    uint32_t r[4];
    for (int q = 0; q < 4; q++)
        r[q] = (host_col ? t_le32(h_near + co[q]) : rd(cpu, nearBase + (uint32_t)co[q])) & 0xFFFFFFu;
    ngte_cvert TL = { sv[F->i0].m1, sv[F->i0].m2, sv[F->i0].m3, r[0], 0, 0 };
    ngte_cvert TR = { sv[F->i1].m1, sv[F->i1].m2, sv[F->i1].m3, r[1], 0, 0 };
    ngte_cvert BL = { sv[F->i3].m1, sv[F->i3].m2, sv[F->i3].m3, r[3], 0, 0 };
    ngte_cvert BR = { sv[F->i2].m1, sv[F->i2].m2, sv[F->i2].m3, r[2], 0, 0 };
    int spikeTri0 = W->tri_n;
    const tn_nearface nf = {
        .hqBase = hqTable + (uint32_t)F->hq_idx * game_layout.terrain.hq_stride,
        .rec = rec, .w3 = F->w3,
        .texid = F->hq_idx, .abe = F->abe, .bank = W->emit_bank,
        .straddle = straddle, .flip = F->flip, .dside = F->dside,
        .rot = F->trirot,
    };
    if (F->quad) terrain_near_emit_face(W, cpu, &nf, TL, TR, BL, BR);
    else         terrain_near_emit_tri(W, cpu, &nf, TL, TR, BL,
                                       !face_tri4_routed(TL.m3, TR.m3, BL.m3, F->w3, rec, F->hq_idx));
    W->dbg_spike_tris += W->tri_n - spikeTri0;
    W->dbg_spike++;   /* near/HQ cells subdivided */
}

/** @brief Resolves a pass's colour arrays as host pointers.
 *
 * The source is guest RAM through the sub-tick view, or an env-anim colour
 * sector's lerped words between ticks (CA, from tsc_canim_mix). The LP pass
 * reads one array, the HP pass the near and far pair; the other pointers are
 * NULL, and a guest array outside the sub-tick view resolves to NULL.
 * @param CA The colour-anim slot, or NULL to read guest RAM.
 * @param passLP 1 for an LP pass.
 * @param lpColBase The LP colour array's guest address.
 * @param nearBase The HP near colour array's guest address.
 * @param farBase The HP far colour array's guest address.
 * @param lpSpan Bytes of the LP array the LP faces index.
 * @param hpSpan Bytes of each HP array the HP faces index. An array that
 *        does not map for its span resolves to NULL.
 * @param h_lp Receives the LP colours.
 * @param h_near Receives the HP near colours.
 * @param h_far Receives the HP far colours. */
static void tn_col_ptrs(const canim_t* CA, int passLP, uint32_t lpColBase,
                        uint32_t nearBase, uint32_t farBase, uint32_t lpSpan,
                        uint32_t hpSpan, const uint8_t** h_lp,
                        const uint8_t** h_near, const uint8_t** h_far) {
    *h_lp = *h_near = *h_far = NULL;
    if (passLP) {
        *h_lp = CA ? (const uint8_t*)CA->mix : (const uint8_t*)tn_host_span(lpColBase, lpSpan);
        return;
    }
    if (CA) {
        *h_near = (const uint8_t*)(CA->mix + CA->nLp);
        *h_far  = (const uint8_t*)(CA->mix + CA->nLp + CA->nHp / 2u);
        return;
    }
    *h_near = (const uint8_t*)tn_host_span(nearBase, hpSpan);
    *h_far  = (const uint8_t*)tn_host_span(farBase, hpSpan);
}

/** @brief Tests whether any corner of a face carries the .s HP walk's vertex flag 0x10.
 *
 * The walk reads GTE FLAG after each vertex's RTPS when SZ3 < 0x600 and adds
 * flag 0x10 when bit 31 is set (r_environment.s 0x80026A8C-0x80026AA4); a
 * vertex with SZ3 >= 0x600 takes .L80026AF8, which never sets it. Bit 31 ORs
 * FLAG bits 30-23 and 18-13, of which the terrain walk reaches three: SX2 or
 * SY2 saturation outside [-1024, 1023] (bits 14, 13), the UNR divide
 * overflow (bit 17, SZ3 <= H/2, so m3 below H_canon / 8, the GTE's own H)
 * and SZ3 saturation at 0 (bit 18, m3 <= 0). SZ3 is 4 * m3, so the SZ3 bound
 * is m3 < 0x600 / 4. The face ORs its corners' flags (0x80026BE0-0x80026C04).
 * @param sv The sector's projected vertices.
 * @param i0 The first corner's vertex index.
 * @param i1 The second corner's vertex index.
 * @param i2 The third corner's vertex index.
 * @param i3 The fourth corner's vertex index; read only for a quad.
 * @param quad 1 when the face is a quad.
 * @return 1 when a corner is flagged, else 0. */
static int tface_gte_flagged(const lvert* sv, int i0, int i1, int i2, int i3, int quad) {
    const int ci[4] = { i0, i1, i2, i3 };
    const int nc = quad ? 4 : 3;
    for (int k = 0; k < nc; k++) {
        const lvert* V = &sv[ci[k]];
        if (V->m3 <= 0.0) return 1;
        if (V->m3 >= 0x600 / 4.0) continue;
        if (V->sx < -1024.0f || V->sx > 1023.0f || V->sy < -1024.0f || V->sy > 1023.0f) return 1;
        if (V->m3 < s_view.H_canon / 8.0) return 1;
    }
    return 0;
}

/** @brief Tests whether a face is a straddler, whose facing tface_facing decides off the projected NCLIP.
 *
 * An HP face straddles when a corner carries the .s vertex flag 0x10
 * (tface_gte_flagged): the .s skips both face NCLIPs for it (bgtz at
 * 0x80026C5C). An LP face straddles when a corner is at or behind the eye.
 * @param sv The sector's projected vertices.
 * @param i0 The first corner's vertex index.
 * @param i1 The second corner's vertex index.
 * @param i2 The third corner's vertex index.
 * @param i3 The fourth corner's vertex index; read by the HP test only for a quad.
 * @param quad 1 when the face is a quad.
 * @param hp 1 for a high-poly face, 0 for a low-poly face.
 * @return 1 when the face straddles, else 0. */
static int tface_straddle(const lvert* sv, int i0, int i1, int i2, int i3, int quad, int hp) {
    if (hp) return tface_gte_flagged(sv, i0, i1, i2, i3, quad);
    return sv[i0].m3 <= 0.0 || sv[i1].m3 <= 0.0 || sv[i2].m3 <= 0.0 || sv[i3].m3 <= 0.0;
}

/** @brief Selects a walk pass's texture source: the scene's, or the HP source level's.
 *
 * Borrowed HP (the borrow pass) and every pass of an imported sector draw
 * through the auxiliary VRAM bank (emit_bank), read the source table and
 * take the source table's cell verdicts (s_hq_bank).
 * @param W The walk context.
 * @param passBorrow 1 on the borrow pass.
 * @param SC The sector.
 * @param hqTableScene The scene's HQ table.
 * @return The HQ table the pass reads. */
static uint32_t tn_pass_bank(tn_wctx* W, int passBorrow, const tscache_t* SC, uint32_t hqTableScene) {
    W->emit_bank = passBorrow || SC->virt;
    s_hq_bank = W->emit_bank;
    return W->emit_bank ? TSRC_TEX_BASE : hqTableScene;
}

/* One walk pass's face source: vertices, faces, guest face records and
 * colour arrays (tn_pass_source). */
typedef struct {
    const tsvert_t* V; const tsface_t* F;
    int nv, nf;
    uint32_t faces;              /* guest face records; 1 = none (no queue entry matches it) */
    const float* vF;             /* sub-tick fractional verts, or NULL */
    uint32_t lpCol, nearB, farB; /* colour arrays for the cpu read path */
    const uint8_t *h_lp, *h_near, *h_far;   /* the same as host pointers, or NULL */
} tn_passsrc;

/** @brief Selects the faces one walk pass draws for a sector.
 *
 * Passes 0 and 2 walk the sector's LP faces and pass 1 its HP faces, with
 * the sector's colour arrays (env-anim mixed through CAm). Pass 3 walks the
 * HP faces borrowed from the HP source level and passes 4 and 5 the
 * borrowed LP faces (C->imp): host vertices in the sector's frame, no
 * guest face records, and colours read from the source level's bytes (the
 * borrowed HP colour copy, the source LP array through the TSRC_TERR_BASE
 * window), with no env-anim mix and no sub-tick vertices.
 * @param SC The sector.
 * @param pass The pass, 0-5.
 * @param CAm The sector's env-anim colour mix, or NULL.
 * @param P Receives the source.
 * @return 1 when the pass has faces to walk, else 0. */
static int tn_pass_source(const tscache_t* SC, int pass, const canim_t* CAm, tn_passsrc* P) {
    memset(P, 0, sizeof(*P));
    const tsimp_t* I = SC->imp;
    if (pass >= 3) {
        if (!I) return 0;
        P->faces = 1u;
        if (pass == 3) {
            P->V = I->V; P->F = I->F; P->nv = I->nV; P->nf = I->nF;
            P->h_near = I->nearCol; P->h_far = I->farCol;
        } else {
            P->V = I->lpV; P->F = I->lpF; P->nv = I->nLpV; P->nf = I->nLpF;
            P->lpCol = I->lpColBase;
            P->h_lp = (const uint8_t*)tn_host_span(I->lpColBase, I->lpColSpan);
        }
        return P->V && P->F && P->nf > 0;
    }
    const int lp = (pass != 1);
    if (lp) {
        P->V = SC->lpV; P->F = SC->lpF; P->nv = SC->nLpV; P->nf = SC->nLpF;
        P->faces = SC->lpFaces; P->lpCol = SC->lpColBase;
        if (SC->vfracPres == s_vanim_pres) P->vF = SC->lpVf;
    } else {
        P->V = SC->hpV; P->F = SC->hpF; P->nv = SC->nHpV; P->nf = SC->nHpF;
        P->faces = SC->hpFaces; P->nearB = SC->nearBase; P->farB = SC->farBase;
        if (SC->vfracPres == s_vanim_pres) P->vF = SC->hpVf;
    }
    tn_col_ptrs(CAm, lp, P->lpCol, P->nearB, P->farB, SC->lpColSpan, SC->hpColSpan,
                &P->h_lp, &P->h_near, &P->h_far);
    return P->V && P->F;   /* alloc failure: skip the pass */
}

/** @brief Walks one visible sector's faces into W's triangle run.
 *
 * Reads the sector's precomputed verdicts (drawHP, drawLP, the fill and
 * far-only flags). A sector the retained GPU cache owns publishes those
 * verdicts as sector parameters and skips the face walk; an owned sector
 * wholly behind the near threshold publishes nothing. Otherwise the walk
 * runs six passes: 0 the LP tier drawn behind HP, 1 the HP tier, 2 the LP
 * hole-fill backing, then the faces borrowed from the HP source level: 3
 * HP, 4 LP and 5 LP hole-fill (tn_pass_source), each skipped when its
 * verdict is off.
 * @param W The walk context receiving the triangles.
 * @param G The walk's shared inputs.
 * @param vi The sector's index in the visible list. */
static void walk_sector(tn_wctx* W, tn_wcfg* G, int vi) {
    CPUState* cpu = G->cpu;
    int (*M)[3] = G->M; int (*Mbin)[3] = G->Mbin;
    double camx = G->camx, camy = G->camy, camz = G->camz;
    int fogRefBase = G->fogRefBase, cullRefS2 = G->cullRefS2, texTblMode = G->texTblMode;
    int tier_faithful = G->tier_faithful, lod_handoff = G->lod_handoff;
    const uint32_t hqTableScene = G->hqTable;
    int nvis = G->nvis;
    uint32_t i = G->visidx[vi];
    uint32_t s = G->secptr[vi];
    const tscache_t* SC = G->SC[vi];
    lvert* sv = W->sv;
    W->emit_bank = 0;
    s_hq_bank = 0;
    (void)M; (void)Mbin; (void)nvis; (void)s; (void)cullRefS2;
    (void)tier_faithful;
        int flags   = SC->flags;
        int originX = SC->originX, originY = SC->originY, originZ = SC->originZ;
        double s4 = originX - camx, s5 = camy - originY, s6 = camz - originZ;

        int nLpV = SC->nLpV, nLpF = SC->nLpF;
        int nHpV = SC->nHpV, nHpF = SC->nHpF;

        /* Tier verdicts (drawHP, drawLP, lpFarOnly, lpGapOnly, fillLP) come from
         * sector_verdicts, which applies the layout's flag bits (no-HP, no-LP,
         * force-LP) and, per layout and mode, the draw's distance binning, the
         * tier-faithful distance tests, and the LP LOD-handoff and gap-fill
         * substitutions. */
        /* cbin is the bounding-centre view-Z through the .s BINNING matrix
         * (g_Camera+0x14, not the +0x00 projection matrix the vertex walk
         * uses; the two differ in scale). lodThr = m_LodDistance>>4 (the same
         * lodDist that drives fog: ir0 = fogRef−4·m3 crosses 0 exactly at
         * m3 = lodThr). */
        const tn_secverd* SV = &G->verd[vi];
        double cbin = SV->cbin;
        int lodThr = fogRefBase >> 2;
        int drawHP = SV->drawHP;
        int drawLP = SV->drawLP;
        if (s_fardbg_active)
            fprintf(stderr, "[fardbg-sec] s=%d flags=%04X cbin=%.1f rad=%d lodThr=%d fogRefBase=%d "
                            "drawHP=%d drawLP=%d nLpF=%d nHpF=%d\n",
                    (int)i, flags, cbin, SC->radius, lodThr, fogRefBase,
                    drawHP, drawLP, nLpF, nHpF);
        /* OPENPETE_TERRAIN_BINCHECK=<frame> - gate: compare the native
         * sector-bin verdict against the .s's OWN HP list, decoded from the
         * render arena after this tick's func_800258F0 ran. The HP list
         * (arena+0x1C00, `ptr|bits` entries, NULL-terminated) SURVIVES the
         * call; the LP list (+0x2000) is clobbered by the HP walk's subdivide
         * lists, so LP membership is attested by the face-set join instead.
         * arena+0x44D4 (D_800771C8-D_8006FCF4) is the pre-pass's surviving
         * frustum-visible byte map (0xFF = passed), scoping the compare:
         * native deliberately draws sectors the .s frustum-culls (GPU clips).
         * Expected: native drawHP == HP-list membership for every
         * frustum-visible sector. */
        {
            static int s_binchk = -2;
            ENV_INT_LATCH(s_binchk, "OPENPETE_TERRAIN_BINCHECK", 0);
            if (s_binchk && openpete_frame >= (unsigned)s_binchk
                && openpete_frame < (unsigned)s_binchk + 2u && !openpete_no_lod) {
                extern uint32_t psx_runtime_g_sony_image_base;
                uint32_t B = psx_runtime_g_sony_image_base;
                static uint32_t hp_list[192]; static int hp_n = -1;
                static unsigned hp_frame;
                if (hp_frame != openpete_frame || hp_n < 0) {
                    hp_frame = openpete_frame; hp_n = 0;
                    for (int k = 0; k < 192; k++) {
                        uint32_t e2 = rd(cpu, B + 0x1C00u + (uint32_t)k * 4);
                        if (!e2) break;
                        hp_list[hp_n++] = e2 & ~3u;
                    }
                    fprintf(stderr, "[bincheck-hdr] frame=%u hpList=%d\n",
                            openpete_frame, hp_n);
                }
                int inHP = 0;
                for (int k = 0; k < hp_n; k++) if (hp_list[k] == s) { inHP = 1; break; }
                int vis = tvis_visible(cpu, i);
                fprintf(stderr, "[bincheck] s=%u nativeHP=%d listHP=%d vis=%d flags=%04X "
                        "cbin=%.1f rad=%d%s\n",
                        (unsigned)i, drawHP, inHP, vis, flags, cbin, SC->radius,
                        (vis && drawHP != inHP) ? " MISMATCH" : "");
            }
        }
        /* .s HP-list membership (lodThr > cbin − radius): the .s evaluates the
         * CLUT-fog ladder only inside this envelope; beyond it the .s draws
         * LP, so ladder rows past it are unauthored for many tiles (the row
         * offset lands on unrelated palettes). Beyond-envelope HP (native's
         * no-pop-in extension) keeps the base palette, the closer stand-in
         * for the LP look. The envelope stretches with --fog-scale
         * (k = 4/s_fogscale; ×1 at stock, bit-identical). */
        int secInLOD = SV->secInLOD;
        /* LP ride-alongs for sectors the bin leaves LP-unlisted:
         * - lpFarOnly (legacy, faithful-bin OFF only): the handoff substitutes
         *   a both-tier sector's LP for its dropped far HP. Under the faithful
         *   bin this must NOT fire - an LP-unlisted sector there is one the .s
         *   walks no LP for (fully-near; straddlers land in BOTH lists), and
         *   re-adding it would draw near-sector a=31 faces the game culls at
         *   the sector level.
         * - lpGapOnly: frozen fill-tag faces still ride - ONLY those - so
         *   og-attested hole cover survives the bin (a tag in a fully-near
         *   both-tier or no-HP sector; and under --no-lod, the sparse-HP
         *   holes of HP-drawing sectors; per-face gate below).
         * - lpUncovOnly (distance-binning games, --no-lod): the LP faces off
         *   the sector's HP mesh ride - ONLY those (tn_lp_face_rides). */
        int lpFarOnly = SV->lpFarOnly, lpGapOnly = SV->lpGapOnly;
        /* LP HOLE-FILL backing: an HP sector that does NOT already
         * draw its LP re-emits it as WORLD_FILL - stencil-gated in core to the
         * pixels no world geometry covered, i.e. the LP<->HP T-junction seams
         * (distant-water gaps). Because of the stencil gate it can never poke
         * through the HP; it draws at TRUE LP depth, no tier bias.
         *
         * DISTANCE GATE - the .s LP-list binning condition (func_800258F0's
         * list pre-pass; 0x80025940 is an instruction inside it):
         * LP is fed only when lodThr < centerZ + radius + 0x100 (the sector's
         * far edge crosses the LOD boundary; lodThr = m_LodDistance>>4, centerZ =
         * the projected bounding-CENTRE view-Z, radius = w1 & 0x1FFF). A NEAR
         * sector's LP is never drawn by the PSX, and feeding it here would let
         * a near hill's coarse LP bulge into a portal aperture (stencil==0:
         * nothing marks inside an aperture), painted at near depth and
         * depth-killing the portal-sky mask.
         * The seams the fill exists for sit exactly AT the LOD boundary, which
         * satisfies this condition by construction. Distance-dependence is
         * confined to sub-pixel seam slivers, so no visible pop-in. NB: reads
         * the RAW RAM lodDist - --lod extended never writes it (host-policy
         * global only), so this stays the real LOD band marker. */
        /* OPENPETE_TERRAIN_HOLEFILL=0 disables (A/B) */
        /* The fill runs for EVERY far-band sector with LP - including sectors
         * whose LP already draws in WORLD (LP-only 0x4000 / force-LP). For
         * those the fill is a screen-DILATED duplicate (see the feed): the PSX
         * integer scanline fill is INCLUSIVE where native's float raster +
         * centre-sampling drops sub-pixel rows, so the far water renders as
         * dashed slivers near screen edges under rotation (the carved-out
         * distant-water horizon gaps, also visible through the green
         * cyclorama). The dilated re-emit grows coverage ~a pixel
         * outward but paints only stencil==0 holes - overdraw is impossible,
         * and the PSX horizon's ~1.75px top-edge lead over native's is
         * recovered the same way (the sky never marks, so growing above the
         * drawn edge is allowed). */
        /* Under EXTENDED LOD (--lod extended / openpete_no_lod), skip the fill
         * for HP-bearing sectors: their HP never drops (no fog-band tier swap
         * to bridge), so any stencil-0 pixel there is genuine void/sky - and
         * the fill's fully-fogged per-face gate rides the RAW-RAM lodThr, so
         * beyond the real fog band it painted each sector's ENTIRE fogged LP
         * silhouette; where the coarse LP overhangs the HP edge that landed on
         * sky as fog-coloured "LP terrain" fringes popping in with distance.
         * LP-only /
         * force-LP sectors keep the fill in every mode - the far-water raster
         * slivers it exists for are pass-0 LP, not a tier handoff. */
        int fillLP = SV->fillLP;

        /* ---- retained-GPU ownership (terrain sector-mesh cache) ----
         * The prepass claimed/validated this sector's chunks; the GPU renders
         * it from them with the tier/fill verdicts above (published via the
         * sector-params UBO) and the face walk is SKIPPED entirely. The
         * sector_set staging write is host memory, distinct slot per sector -
         * safe from pool workers (rhi_* calls stay on the owning thread). */
        if (G->own && G->own[vi]) {
            /* own == 2: owned, NO-DRAW - the whole bounding sphere sits
             * behind the near threshold, so the CPU walk would emit nothing
             * for this sector. Publish no flags at all: the per-build sector
             * reset left SF_DRAWN clear, which is exactly "draw nothing",
             * and the walk is skipped like any owned sector. */
            if (G->own[vi] == 2) return;
            uint32_t sf = OPTC_SF_DRAWN
                | (drawHP    ? OPTC_SF_DRAWHP    : 0u)
                | (drawLP    ? OPTC_SF_DRAWLP    : 0u)
                | (lpFarOnly ? OPTC_SF_LPFARONLY : 0u)
                | (lpGapOnly ? OPTC_SF_LPGAPONLY : 0u)
                | ((uint32_t)SV->lpUncovOnly * OPTC_SF_LPUNCOV)
                /* the walk never fills a poly-anim sector (stale LP bank) */
                | (fillLP && !SC->polyanim ? OPTC_SF_FILLLP : 0u)
                | (secInLOD  ? OPTC_SF_SECINLOD  : 0u)
                /* own == 3: near-variant present - draw the clip-fan chunks
                 * and kill the plain ones (the two sets are exclusive). */
                | (G->own[vi] == 3 ? OPTC_SF_NEARVAR : 0u)
                | (((uint32_t)s_tgpu[(int)(SC - s_tsc)].gen & 7u) << 12)
                | (((uint32_t)SV->lpNearThr & OPTC_SF_LPNEAR_MASK) << OPTC_SF_LPNEAR_SHIFT);
            /* Pack LP grade (lp_grade_apply's source choice, same priority):
             * the bake carries raw LP colours, so the affine rides the
             * sector params for the vertex stage to apply. */
            float grade6[6]; const float* gr = NULL;
            if (s_lpg_active) {
                const float *p = NULL, *q = NULL;
                if (SC->lpg_state == 1)  { p = SC->lpg_p;   q = SC->lpg_q; }
                else if (s_lpg_mean_ok) { p = s_lpg_mean;  q = s_lpg_mean + 3; }
                if (p) {
                    for (int c = 0; c < 3; c++) { grade6[c] = p[c]; grade6[3 + c] = q[c]; }
                    gr = grade6; sf |= OPTC_SF_LPGRADE;
                }
            }
            openpete_tercache_sector_set((int)(SC - s_tsc),
                                         (float)originX, (float)originY,
                                         (float)originZ, sf, gr);
            return;
        }

        W->walk_sectors++;

        /* per-sector accounting for OPENPETE_TERRAIN_SECTORDUMP (spans both passes) */
        int sd_triBefore = W->tri_n;
        double sd_m3min = 1e18, sd_m3max = -1e18;
        float sd_bx0 = 1e18f, sd_by0 = 1e18f, sd_bx1 = -1e18f, sd_by1 = -1e18f;
        int sd_vfront = 0, sd_nv = 0, sd_nf = 0;

      const canim_t* CAm = tsc_canim_mix(SC);   /* env-anim colours, sub-tick */
      for (int pass = 0; pass < 6; pass++) {
        int passFill = (pass == 2 || pass == 5);  /* LP hole-fill backing */
        int passBorrow = (pass == 3);             /* HP faces borrowed from the source level */
        int passBorLP = (pass >= 4);              /* LP faces borrowed from the source level */
        int passLP = (pass == 0 || pass == 2 || passBorLP);
                                                  /* 0 = WORLD LP (behind HP), 1 = HP, 2 = fill LP,
                                                   * 3 = borrowed HP, 4 = borrowed WORLD LP,
                                                   * 5 = borrowed fill LP */
        int fmDumpOnly = 0;                       /* facemap: project + dump, never emit */
        if (pass == 0 && !drawLP) {
            /* The inspector forces the same dump-only walk facemap uses, so an
             * LP face is pickable even in a sector whose LP tier never draws -
             * aiming at a hole then names the face that would cover it. */
            if ((s_facemap_active || s_fp_on) && !(flags & 0x2000) && nLpF > 0) fmDumpOnly = 1;
            else continue;
        }
        if ((pass == 1 || passBorrow) && !drawHP) continue;
        if (pass == 4 && !drawLP) continue;
        if (passFill && !fillLP) continue;
        if (passFill && SC->polyanim) continue;   /* stale LP bank - never backstop-fill */
        tn_passsrc P;
        if (!tn_pass_source(SC, pass, CAm, &P)) continue;
        const uint32_t faces = P.faces; const int nv = P.nv, nf = P.nf;
        const tsvert_t* cverts = P.V; const tsface_t* cfaces = P.F;
        const float* cvF = P.vF;   /* sub-tick fractional verts (poly-anim sectors only) */
        const uint32_t lpColBase = P.lpCol, nearBase = P.nearB, farBase = P.farB;
        const uint8_t *h_lp = P.h_lp, *h_near = P.h_near, *h_far = P.h_far;
        const int faceStride = passLP ? 8 : 16;
        int host_col = passLP ? (h_lp != NULL) : (h_near != NULL && h_far != NULL);
        W->emit_noHP = passLP;     /* tag emitted tris with this pass's tier */
        /* Borrowed HP and imported sectors: auxiliary bank, source table,
         * and the source table's cell verdicts. */
        const uint32_t hqTable = tn_pass_bank(W, passBorrow, SC, hqTableScene);
        W->emit_fill = passFill;   /* fill pass -> WORLD_FILL category in the feed */
        sd_nv += nv; sd_nf += nf;

        for (int k = 0; k < nv; k++) {
            double Xf, Yf, Zf;
            if (cvF) { Xf = cvF[k*3]; Yf = cvF[k*3+1]; Zf = cvF[k*3+2]; }
            else     { Xf = cverts[k].x; Yf = cverts[k].y; Zf = cverts[k].z; }
            double m1, m2, m3;
            ngte_mac(M, s5 - Yf, s6 - Zf, Xf + s4, &m1, &m2, &m3);
            sv[k].m1 = m1; sv[k].m2 = m2; sv[k].m3 = m3; sv[k].vz = m3; sv[k].ok = (m3 > 0.0);
            proj_screen(m1, m2, m3, &sv[k].sx, &sv[k].sy);
            if (m3 > 0.0) {
                sd_vfront++;
                if (m3 < sd_m3min) sd_m3min = m3;
                if (m3 > sd_m3max) sd_m3max = m3;
                if (sv[k].sx < sd_bx0) sd_bx0 = sv[k].sx;
                if (sv[k].sy < sd_by0) sd_by0 = sv[k].sy;
                if (sv[k].sx > sd_bx1) sd_bx1 = sv[k].sx;
                if (sv[k].sy > sd_by1) sd_by1 = sv[k].sy;
            }
            if (W->serial && m3 > 0.0 && s_tv_n < MAX_TVERTS) {       /* feed the geometry A/B */
                s_tvx[s_tv_n] = sv[k].sx; s_tvy[s_tv_n] = sv[k].sy; s_tvz[s_tv_n] = m3; s_tv_n++;
            }
        }

        for (int f = 0; f < nf; f++) {
            /* Face decode from the sector cache (indices, flags, colour offsets
             * and texmat are all static; see tsc_decode_faces). Per-present
             * work starts at the projection use. */
            const tsface_t* F = &cfaces[f];
            int i0 = F->i0, i1 = F->i1, i2 = F->i2, i3 = F->i3;
            int flip = F->flip;
            int fogRef = fogRefBase + (F->fogbias ? 0x2000 : 0);
            int c0 = F->c0, c1 = F->c1, c2 = F->c2, c3 = F->c3;
            uint32_t hqW3 = F->w3;
            const tmat* mat = &F->mat;
            const float* tu = F->tu; const float* tv = F->tv;
            if (mat->textured) W->tex_faces++;
            if (i0 >= nv || i1 >= nv || i2 >= nv || i3 >= nv) { W->dbg_oob++; continue; }
            int quad = F->quad;
            W->dbg_faces++;
            W->emit_sec = (int)i; W->emit_face = f;   /* fardbg attribution identity */
            /* Inspector tint class: a listed FILL tag vs a face preview forced
             * through (the untagged candidates). HP faces carry neither. */
            W->emit_fptag = (passLP && F->gapfill) ? (F->fillcand ? 2 : 1) : 0;
            W->emit_env = (F->tgen != 0);            /* env-anim member (rewrite set) */
            /* Portal OT-conformance key: the face's .s OT bucket base - the
             * raw 4-corner view-Z sum ((Σ4 SZ)>>7 in the walk; .s tris sum
             * the shared i2==i3 corner twice, and so does this), in emit_otkey
             * units (the sum of the four SZ terms, SZ = 4·m3). Ambient per
             * face so every ttri it emits (direct, near-clipped, or the 2x2
             * subdivide) carries the PARENT face's key - the .s verdict is
             * per-face, and a split tri's own 3-vert sum can land on the
             * other side of the sky's bucket. */
            W->emit_otkey = (float)((sv[i0].m3 + sv[i1].m3 + sv[i2].m3 + sv[i3].m3) * 4.0);
            /* Authored OT-slot depth offset: the .s adds a per-game base
             * plus a multiple of the face's authored bias to its OT index;
             * ported as world units on the test depth at the emit (1 slot =
             * 128). The terms are game_layout.terrain data because the two
             * games' inserts do not agree on the base. Set ambient here so
             * the near/HQ subdivide and clip paths inherit the PARENT
             * face's offset, like otkey. */
            W->emit_nearband = 0;
            W->emit_otslots = passLP
                ? (uint16_t)((unsigned)game_layout.terrain.lp_base_slots
                             + (unsigned)game_layout.terrain.lp_bias_slots * F->otbias)
                : (uint16_t)((unsigned)game_layout.terrain.hp_bias_slots * F->otbias);
            /* A face in the .s's FOREGROUND band (corner-depth sum < 0x2000) is
             * marked emit_nearband, and its OT-slot offset is dropped unless
             * the layout keys near/HQ faces through the ordinary insert
             * (nearband_ot_keys).
             *
             * HP: the classifier (the sum + flag/corner terms - the exact .s
             * test from gamestate) routes those faces to the near/HQ queues,
             * where they are DrawPrim-drawn, mask-bit-protected, and never
             * inserted into the OT - there is no OT index for the authored
             * bias to offset. Foreground faces contest each other by
             * protected draw order, which raw per-pixel depth approximates;
             * a whole-face world offset instead moves a grazing face's
             * crossing line across the floor. The CLASSIFIER is the right
             * gate, not the .s's filled queue: a foreground face the .s
             * clip-rejects before classification (extreme grazing
             * projection - native draws it, GPU-clipped) never reaches the
             * queue yet must not carry a bias either - og never gave it an
             * OT key at all.
             *
             * LP: the LP walk has no defer path, so og does key foreground
             * LP (a=31 / force-LP 0x8000 / fill-tag cover - the only LP the
             * admission lets survive this near). But an 8192+-world stored
             * offset on walkable foreground LP (force-LP primary surfaces,
             * the fill water planes) breaks the port's true moby/Spyro-vs-
             * terrain occlusion exactly where it is most contested. True
             * depth already orders a base-under-overlay pair (the HP detail
             * physically sits above its LP base), and exact coplanar ties are
             * the stencil/bias machinery's job, not a world-scale push's. The
             * flag terms are HP-record semantics, so LP tests the distance
             * band alone (word3 = 0 makes the classifier exactly that
             * test). */
            if (face_is_near_canonical(sv[i0].m3, sv[i1].m3, sv[i2].m3,
                                       (quad ? sv[i3].m3 : sv[i2].m3),
                                       passLP ? 0u : hqW3))
                { W->emit_nearband = 1;
                  /* A game whose near/HQ faces key through the ordinary
                   * insert keeps that insert's authored bias; one whose
                   * near/HQ queue has no OT index has nothing to bias. */
                  if (!game_layout.terrain.nearband_ot_keys)
                      W->emit_otslots = 0; }
            /* .s HP-walk texture-fog select (generated C block_80027040-809C,
             * tri twin 0x80027460): per-face key from the corner SZs (SZ=4·m3;
             * the same key feeds the OT bucket), t0 = key − fogRef + 4096.
             *   t0 > 0 (in/beyond the fog band): tiledef half +0, CLUT halfword
             *     += (t0>>8)<<6 - cly += Δ, a LADDER of pre-baked progressively
             *     fogged palette rows. This is the palette half of the PSX fog
             *     model (the DPCS vertex blend is the other half); without it
             *     the fog band renders with the base (unfogged) palette.
             *   t0 ≤ 0 (near): the +8 tiledef half (bpp8 near tile), Δ = 0.
             * Under --no-lod the .s suppresses both (saturated fogRef at the
             * walk's LOD loads + the no_lod_tier_bump channel) as a side effect
             * of the shared lodDist reads; natively we keep CANONICAL fog by
             * true distance - no-lod means mesh detail, not fog removal.
             * OPENPETE_TERRAIN_CLUTFOG=0 kills (A/B); force-tier debug modes
             * (OPENPETE_TERRAIN_TEXTBL != 0) keep the static half.
             * The 0x3E at block_80027080 is the PRIM CODE byte (ABE GT4), not
             * a packet length: the .s reads the SAME tiledef entry (masked
             * &0x7F) and applies the SAME ladder to negative-texByte faces.
             * Animated tiles are plain in-place entry rewrites (func_8002A6FC),
             * kept live by tsc_refresh_anim; a ladder applied to a stale entry
             * lands on the wrong palettes. */
            tmat matSel;
            int dbg_half = 0, dbg_delta = 0;   /* fardbg: chosen tiledef half + ladder row */
            if (!passLP && s_clutfog && secInLOD && texTblMode == 0 && F->mat.textured) {
                /* SZ = s_fogscale·m3 - the whole ladder ramp rides the
                 * --fog-scale curve (s_fogscale = 4/k; exact .s at k=1).
                 * ALWAYS four corners: .s HP tris are quads with i2==i3 and
                 * the walk sums the shared vertex TWICE. Summing only 3
                 * under-shoots Δ by ~25% on every fog-band triangle. */
                int szsum = (int)(sv[i0].m3 * s_fogscale) + (int)(sv[i1].m3 * s_fogscale)
                          + (int)(sv[i2].m3 * s_fogscale) + (int)(sv[i3].m3 * s_fogscale);
                int key = (szsum >> 7) << 5;
                int t0k = key - fogRef + 4096;
                if (game_layout.terrain.ladder_nearest_corner) {
                    /* Spyro 2/3 (environment.s 80024C94-80024E2C): each
                     * corner's depth-cue IR0 = fogRef - SZ is clamped to
                     * [0, 0xFFF] as it is blended, the largest survives, and
                     * the row is 15 - (IR0 >> 8); row 0 takes the near tile
                     * half. Expressed on the mean-key form: t0k <= 0 is the
                     * near half and t0k >> 8 the row, so t0k here is
                     * 256 * row. */
                    int irmax = 0;
                    const int cq[4] = { i0, i1, i2, i3 };
                    for (int q2 = 0; q2 < 4; q2++) {
                        int ir0 = fogRef - (int)(sv[cq[q2]].m3 * s_fogscale);
                        if (ir0 < 0) ir0 = 0; else if (ir0 > 0xFFF) ir0 = 0xFFF;
                        if (ir0 > irmax) irmax = ir0;
                    }
                    t0k = (15 - (irmax >> 8)) << 8;
                }
                if (t0k <= 0) {
                    if (F->matN.textured) { matSel = F->matN; mat = &matSel; tu = F->tuN; tv = F->tvN; dbg_half = 1; }
                } else {
                    /* Clamp to the DEEPEST AUTHORED row: the ladder is exactly
                     * 16 rows (Δ 0..15). Within the fog band t0 < 4096 so the
                     * .s only ever produces Δ ≤ 15; faces whose quantized key
                     * lands at/past saturation (straddling faces the handoff
                     * keeps - only all-corner-beyond faces drop) would compute
                     * Δ ≥ 16 and sample the NEIGHBOURING tile's palette. */
                    int delta = t0k >> 8;
                    if (delta > 15) delta = 15;
                    dbg_delta = delta;
                    if (delta > 0) {
                        matSel = F->mat;
                        uint16_t hw = (uint16_t)((uint32_t)matSel.clut_raw + ((uint32_t)delta << 6));
                        matSel.clx = (int)(hw & 0x3F) * 16;
                        matSel.cly = (int)(hw >> 6) & 0x1FF;
                        matSel.fog_delta = delta;
                        mat = &matSel;
                    }
                }
                /* Every face that reached this select fades on the published
                 * ladder ramp - mark it (plus its fogRef bias) for the
                 * CONTINUOUS pack-fog model. Copy-on-mark: `mat` may still
                 * point into the shared sector cache. */
                if (mat != &matSel) { matSel = *mat; mat = &matSel; }
                matSel.fog_delta = (matSel.fog_delta & 0xFF) | OPENPETE_TP_FOG_CONT
                                 | (F->fogbias ? OPENPETE_TP_FOG_FOGBIAS : 0);
            }
            /* NCLIP back-face sign (2D cross product == GTE MAC0; flip negates it,
             * word3 & 0x2). front = nclip >= 0 for HP, > 0 for LP. The GTE
             * saturates SX/SY to [-1024, 1023] BEFORE NCLIP - a far-offscreen
             * corner (glancing screen-edge face) must be clamped the same way
             * or the cross product flips sign vs the game's verdict. */
            /* The corners are not integer-truncated first (mirroring MAC0>>16):
             * the game's UNR divide ≠ float division, so sub-pixel rounding does
             * not track it. Floats are kept; only the saturation window is
             * mirrored. */
            #define GTE_SAT(v) ((v) < -1024.0f ? -1024.0 : ((v) > 1023.0f ? 1023.0 : (double)(v)))
            /* NCLIP facing, mirroring the .s HP walk exactly (block_80026C30..
             * 80026CD0; tri variant block_8002720C..80027220):
             *  - HALF 1: the walk RTPT-loads SXY = corners (i0, i1, i3) for a
             *    quad (the GT4 slot order TL,TR,BL - NOT (i0,i1,i2)) and
             *    (i0, i1, i2) for a tri. Accept when the flip-adjusted MAC0
             *    (word3 & 0x2 negates) is >= 0.
             *  - HALF 2 (quads only, when half 1 flunks): SXY0 is re-loaded
             *    with corner i2 -> NCLIP over (i2, i1, i3). The word3
             *    double-sided bit jumps straight to the emit path
             *    (Spyro 1 w3 & 0x4; Spyro 3 w3 & 0x800, environment.s
             *    :2516 and :2873 - 0x4 there is a texture-index bit);
             *    otherwise FLIP selects the accepted sign - set -> accept
             *    n2 >= 0 (block_80026CC0), clear -> accept n2 <= 0
             *    (block_80026CA8; the triple's orientation is inverted vs
             *    half 1, hence the opposite base sign).
             *  - A double-sided TRI skips its NCLIP verdict outright, branching
             *    past it beside the straddle flag (Spyro 1 r_environment.s
             *    0x800271F8; Spyro 2 environment.s 0x80025028 and Spyro 3
             *    environment.s 0x800237EC OR the bit into that flag), so it
             *    emits facing either way. LP double-sided faces likewise skip
             *    both halves (the LP arm below).
             * The face draws when either half accepts: a twisted quad near
             * edge-on is visible through one half only, while a genuine
             * back-to-back wall pair fails both. */
            /* GTE UNR divide-overflow regime: RTPS saturates the quotient when
             * SZ3 <= H/2 (SZ = 4·m3, so m3 <= H/8) - the .s's own SXY for such
             * a corner is pinned garbage, and the saturated cross sign is
             * chaotic noise (saturation collapses a wide span to a narrow one
             * and the sign flips with a small pan while the true cross stays
             * stable). The .s verdict is not mirrorable there - use the TRUE
             * facing (unsaturated float cross), which still culls genuine
             * back-to-back near wall pairs. */
            int unrOvf = 0, unrEff = 0;
            {   const int ci[4] = { i0, i1, i2, i3 };
                int nc2 = quad ? 4 : 3;
                for (int q2 = 0; q2 < nc2; q2++) {
                    if (sv[ci[q2]].m3 < s_view.H_canon / 8.0) unrOvf = 1;  /* the GTE's H: the regime does not move with --fov */
                    if (sv[ci[q2]].m3 < s_view.H_eff / 8.0) unrEff = 1;
                }
            }
            NGTE_LIMIT(NGTE_SITE_TERRAIN_UNR, unrOvf, unrEff);
            #define NSAT(v) (unrOvf ? (double)(v) : GTE_SAT(v))
            double nx0 = NSAT(sv[i0].sx), ny0 = NSAT(sv[i0].sy);
            double nx1 = NSAT(sv[i1].sx), ny1 = NSAT(sv[i1].sy);
            double nclip;
            int front, front2 = 0;
            if (faceStride == 8) {
                /* LP walk (block_80026404 quad / block_80026604 tri): no flip
                 * bit, fixed signs, and the corner ORDER differs from HP -
                 *  - HALF 1: NCLIP over (i0, i1, i2) (scanline order, not the
                 *    GT4 slot order), accepted STRICTLY positive.
                 *  - HALF 2 (quads, half 1 flunked): SXY0 re-loaded with i3 ->
                 *    NCLIP over (i3, i1, i2), accepted STRICTLY negative (the
                 *    triple's orientation inverts vs half 1).
                 *  - w0 bit 7 = double-sided: the quad path jumps past the
                 *    half-2 verdict straight to the far-cull/admission gates
                 *    (block_80026448->80026484); the tri path adds bit7<<23 =
                 *    0x40000000 to MAC0, which dwarfs any saturated cross.
                 * (LP does not use HP's (i0,i1,i3) order and sign.) */
                double nx2 = NSAT(sv[i2].sx), ny2 = NSAT(sv[i2].sy);
                nclip = (nx1 - nx0) * (ny2 - ny0) - (nx2 - nx0) * (ny1 - ny0);
                front = (nclip > 0.0);
                if (quad) {
                    double nx3 = NSAT(sv[i3].sx), ny3 = NSAT(sv[i3].sy);
                    double nclip2 = (nx1 - nx3) * (ny2 - ny3) - (nx2 - nx3) * (ny1 - ny3);
                    front2 = (nclip2 < 0.0);
                }
                if (F->dside) front2 = 1;
            } else {
                int h1 = quad ? i3 : i2;
                double nxA = NSAT(sv[h1].sx), nyA = NSAT(sv[h1].sy);
                nclip = (nx1 - nx0) * (nyA - ny0) - (nxA - nx0) * (ny1 - ny0);
                if (flip) nclip = -nclip;
                front = (nclip >= 0.0);
                front2 = F->dside;
                if (quad) {
                    double nx2 = NSAT(sv[i2].sx), ny2 = NSAT(sv[i2].sy);
                    double nx3 = NSAT(sv[i3].sx), ny3 = NSAT(sv[i3].sy);
                    double nclip2 = (nx1 - nx2) * (ny3 - ny2) - (nx3 - nx2) * (ny1 - ny2);
                    front2 |= flip ? (nclip2 >= 0.0) : (nclip2 <= 0.0);
                }
            }
            /* Far-band identity dump: full per-face record for the offline
             * whole-face join vs captured prims. Backfaces included (fr=0 -
             * dumped BEFORE the cull gate so the attribution can name faces
             * neither side draws); fill pass skipped (duplicate of pass 0). */
            if (s_fardbg_active && !passFill) {
                const int ci[4] = { i0, i1, i2, i3 };
                const int cc[4] = { c0, c1, c2, c3 };
                int nc = quad ? 4 : 3;
                fprintf(stderr, "[fardbg-face] s=%d p=%d f=%d q=%d fb=%d fr=%d fogRef=%d "
                        "abe=%d half=%d dlt=%d cl=%d,%d tp=%d,%d bpp=%d",
                        (int)i, pass, f, quad, F->fogbias, front, fogRef,
                        F->abe, dbg_half, dbg_delta, mat->clx, mat->cly,
                        mat->tpx, mat->tpy, mat->bpp);
                for (int q2 = 0; q2 < nc; q2++) {
                    const lvert* V = &sv[ci[q2]];
                    uint32_t cn = 0, cf2 = 0, cb2 = 0;
                    if (passLP) {
                        cb2 = rd(cpu, lpColBase + (uint32_t)cc[q2]) & 0xFFFFFFu;
                    } else {
                        cn  = rd(cpu, nearBase + (uint32_t)cc[q2]) & 0xFFFFFFu;
                        cf2 = rd(cpu, farBase  + (uint32_t)cc[q2]) & 0xFFFFFFu;
                        cb2 = terrain_fog_blend(cn, cf2, V->m3, fogRef);
                    }
                    fprintf(stderr, " c%d=%.2f,%.2f,%.1f,%06X,%06X,%06X",
                            q2, V->sx, V->sy, V->m3, cb2, cn, cf2);
                }
                fputc('\n', stderr);
            }
            /* facemap: one line per LP face with projected corners (backfaces
             * and behind-camera corners included - m3 ≤ 0 corners are marked
             * by their value; the offline side clips). secdraw distinguishes
             * "sector's LP pass ran normally" from the forced dump-only walk. */
            if (s_facemap_active && passLP && !passFill && !passBorLP) {
                const int ci[4] = { i0, i1, i2, i3 };
                int nc = quad ? 4 : 3;
                fprintf(stderr, "[facemap] s=%u f=%d q=%d gf=%d flr=%d fb=%d fr=%d secdraw=%d",
                        (unsigned)i, f, quad, F->gapfill, F->floorf, F->fogbias, front, !fmDumpOnly);
                for (int q2 = 0; q2 < nc; q2++) {
                    const lvert* V = &sv[ci[q2]];
                    fprintf(stderr, " v%d=%.2f,%.2f,%.2f", q2, V->sx, V->sy, V->m3);
                }
                fputc('\n', stderr);
            }
            /* Fill-tag inspector pick - BEFORE every drop gate, so a face the
             * gates reject is still identifiable (that is the under-fill case:
             * the surface that should cover the hole is the one not drawn).
             * The fill pass is skipped: it re-walks faces pass 0 already
             * offered. */
            if (s_fp_on && !passFill && !passBorLP)
                fp_test_face(passLP, (int)i, f, F->fillcand, sv, i0, i1, i2, i3, quad);
            if (fmDumpOnly) continue;
            /* fardbg verdict trail: every face dropped AFTER its [fardbg-face]
             * row announces itself, so the offline face-set join can subtract
             * (the dump alone is the pre-verdict universe). r= names the gate. */
            #define FARDBG_DROP(reason) do { \
                if (s_fardbg_active && !passFill) \
                    fprintf(stderr, "[fardbg-drop] s=%d p=%d f=%d r=%s\n", \
                            (int)i, pass, f, (reason)); } while (0)
            if (passLP && !passFill && !tn_lp_face_rides(SV, F)) { FARDBG_DROP("rideonly"); continue; }
            /* Authored per-face LP admission (the .s test, exact): with
             * slot = (four corner SZ3 summed, tris doubling the shared corner)
             * >> 5 and s3 = (m_LodDistance >> 7) - 0x20, an LP face draws iff
             * slot + 8*admit >= s3; a deficit of -31..-1 falls back to
             * per-vertex "draw if ANY corner SZ3 >= (s3 - 8*admit)*8"; deeper
             * deficits reject. LP is a FAR tier - the .s drops it NEAR (coarse
             * alternates the player is never meant to see up close). Applies
             * in every mode; fill-tag faces are exempt (og-attested hole cover at any range)
             * - but only while OPENPETE_TERRAIN_GAPFILL=1, so the tag A/B
             * knob keeps its contract in original mode too.
             * OPENPETE_TERRAIN_LP_ADMIT=0 restores the legacy fog-corner-only
             * behaviour (A/B). A game with the LP near outcode
             * (terrain.lp_near_sz) takes that rule instead
             * (tn_lp_face_near_drop). */
            /* .s LP far cull (r_environment.s:792-793 quad / 930-931 tri):
             * slot = ΣSZ3>>5 tested against s2 = m_CullingDistance>>7 - drop
             * iff slot >= s2. A distinct, larger distance than m_LodDistance;
             * every LP face takes it, fill tags included (og-attested cover
             * cannot extend past og's own draw distance). Skipped under
             * --lod extended: extending the draw distance is that mode's
             * purpose. OPENPETE_TERRAIN_LP_FARCULL=0 disables (A/B). */
            if (passLP && !passFill && !openpete_no_lod) {
                if (G->lp_farcull) {
                    double fcz3 = quad ? sv[i3].m3 : sv[i2].m3;
                    double fcslot = (sv[i0].m3 + sv[i1].m3 + sv[i2].m3 + fcz3) / 32.0;
                    if (fcslot >= (double)cullRefS2) { FARDBG_DROP("farcull"); continue; }
                }
            }
            if (passLP && !passFill && !(s_gapfill_en && F->gapfill)) {
                const double cz[4] = { sv[i0].m3, sv[i1].m3, sv[i2].m3, sv[i3].m3 };
                const char* why = tn_lp_face_near_drop(SV, F, cz, quad, G->lp_admit, fogRefBase);
                if (why) { FARDBG_DROP(why); continue; }
            }
            if (lod_handoff) {
                int nc = quad ? 4 : 3, beyond = 0;
                const int ci[4] = { i0, i1, i2, i3 };
                for (int q2 = 0; q2 < nc; q2++)
                    if (fogRef - (int)(sv[ci[q2]].m3 * s_fogscale) <= 0) beyond++;
                if (!passLP) {
                    /* .s HP LOD drop, exact (r_environment.s:1385-1387; tri
                     * 1759-1760): the walk sums the four corner SZ3s (tri
                     * records store i2==i3, so the shared corner counts twice)
                     * and drops the face iff sum >= m_LodDistance - one strict
                     * compare. No per-vertex band, no per-face modifier: the
                     * w3&1 flag pushes FOG saturation out by 0x2000 but not
                     * the drop, so its faces retire at the same distance while
                     * still partly unfogged. SZ3 = viewZ/4 = m3·(s_fogscale at
                     * stock), so the sum compare rides the --fog-scale curve
                     * exactly like the fog it must stay hidden inside
                     * (bit-exact vs the .s at stock fog).
                     * OPENPETE_TERRAIN_HP_DROP=0 restores the legacy
                     * all-corners-fully-fogged approximation, which kept
                     * straddling faces the .s mean-drops and honoured the
                     * fog-push flag the .s drop ignores (A/B). */
                    if (G->hp_drop) {
                        double hpz3 = quad ? sv[i3].m3 : sv[i2].m3;
                        double hpsum = (sv[i0].m3 + sv[i1].m3 + sv[i2].m3 + hpz3)
                                       * s_fogscale;
                        if (hpsum >= 4.0 * (double)fogRefBase) { FARDBG_DROP("hpdrop"); continue; }
                    } else if (beyond == nc) { FARDBG_DROP("hpdrop"); continue; }
                }
                if (passLP && !passFill && lpFarOnly && beyond == 0
                    && !G->lp_admit) {
                    /* Legacy (LP_ADMIT=0) near gate for handoff-riding LP: the
                     * fog-corner test drops LP until a corner crosses the fog
                     * line, but the .s's actual near cull is the authored
                     * admission above - it admits LP well INSIDE the fog band
                     * (the authored LP/HP overlap is wide), so running both
                     * re-drops mid-band LP the game draws. LP far-only -
                     * EXCEPT classified HP-coverage-gap fillers (the frozen
                     * fill tags), which draw at all distances so a sparse near
                     * HP mesh doesn't open a hole to the cyclorama (at the fog
                     * line the game itself draws these faces the moment one
                     * corner crosses, so this only stabilizes that verdict
                     * inward).
                     * OPENPETE_TERRAIN_LPKEEP=1 keeps ALL such faces (probe);
                     * LPKEEPDBG=<frame> logs each kept face - sector/face id,
                     * world verts, screen bbox - to identify hole culprits. */
                    static int s_lpkeep = -2, s_lpkeepdbg;
                    if (s_lpkeep == -2) {
                        const char* e = getenv("OPENPETE_TERRAIN_LPKEEP");
                        s_lpkeep = (e && *e && *e != '0');
                        e = getenv("OPENPETE_TERRAIN_LPKEEPDBG");   /* = frame to log at */
                        s_lpkeepdbg = (e && *e) ? atoi(e) : 0;
                    }
                    int gfKeep = s_gapfill_en && F->gapfill;
                    if (!s_lpkeep && !gfKeep) { FARDBG_DROP("lpfar"); continue; }
                    if (s_lpkeepdbg && openpete_frame >= (unsigned)s_lpkeepdbg
                        && openpete_frame < (unsigned)s_lpkeepdbg + 4u) {
                        float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
                        const int ci2[4] = { i0, i1, i2, i3 };
                        fprintf(stderr, "[lpkeep] s=%u f=%d q=%d gf=%d", i, f, quad, F->gapfill);
                        for (int q2 = 0; q2 < nc; q2++) {
                            const tsvert_t* V = &cverts[ci2[q2]];
                            const lvert* P = &sv[ci2[q2]];
                            if (P->sx < bx0) bx0 = P->sx; if (P->sx > bx1) bx1 = P->sx;
                            if (P->sy < by0) by0 = P->sy; if (P->sy > by1) by1 = P->sy;
                            fprintf(stderr, " v%d=%d,%d,%d", q2,
                                    originX + V->x, originY + V->y, originZ + V->z);
                        }
                        fprintf(stderr, " bbox=%.0f,%.0f..%.0f,%.0f\n", bx0, by0, bx1, by1);
                    }
                }
            }
            /* HOLE-FILL per-face far gate: only FULLY-FOGGED faces
             * (every corner at/past the exact fog saturation, ir0 = fogRef−4·m3
             * ≤ 0 - the proven decode scale, independent of the shipped
             * s_fogscale), unless OPENPETE_TERRAIN_FILL_FOGSLACK widens it.
             * The T-junction seams the fill exists for are at the
             * fully-fogged horizon; a mid-band face admitted here paints a
             * dark 1-2px LP fringe along terrain-vs-sky silhouettes (native's
             * float raster is a hair tighter than the PSX integer edge). */
            if (passFill) {
                /* Transparent LP never rides the fill: it blends instead of
                 * covering, so a stencil-gated twin over the face's own raster
                 * (transparents write no stencil) would blend the same pixels
                 * TWICE. The fill exists to patch opaque raster slivers. */
                if (F->mat.blend) continue;
                /* Slack (OPENPETE_TERRAIN_FILL_FOGSLACK, default 0: only
                 * fully-fogged faces; 1024 admits faces whose every corner is
                 * ≥75% fogged). A strict ir0≤0 drops the LP of sectors
                 * STRADDLING the LOD boundary, leaving their far rows' raster
                 * gaps unfilled; a slack admits those while excluding the
                 * mid-band faces that fringe sky silhouettes. */
                static int s_fogslack = -1;
                if (s_fogslack < 0) { const char* e = getenv("OPENPETE_TERRAIN_FILL_FOGSLACK"); s_fogslack = (e && *e) ? atoi(e) : 0;   /* default 0: fully-fogged faces only */ }
                int nc2 = quad ? 4 : 3, farAll = 1, fogged = 1;
                const int cf[4] = { i0, i1, i2, i3 };
                for (int q2 = 0; q2 < nc2 && farAll; q2++) {
                    int ir0 = fogRef - (int)(sv[cf[q2]].m3 * 4.0);
                    if (ir0 > 0) fogged = 0;
                    if (ir0 > s_fogslack) farAll = 0;
                }
                if (!farAll) continue;
                /* Slack-admitted (NOT fully fogged) faces pass ONLY when screen-
                 * THIN (bbox height ≤ OPENPETE_TERRAIN_FILL_THINPX, default 16px):
                 * the class the slack exists for is 1px straddling water rows. An
                 * unrestricted slack admits mid-band LP BODIES whose coarse
                 * silhouette ghosts into stencil-0 sky above terrain tops and
                 * depth-notches neighbouring geometry. */
                if (!fogged) {
                    static float s_thin = -1.0f;
                    if (s_thin < 0.0f) { const char* e2 = getenv("OPENPETE_TERRAIN_FILL_THINPX"); s_thin = (e2 && *e2) ? (float)atof(e2) : 16.0f; }
                    float ty0 = 1e9f, ty1 = -1e9f; int thinOk = 1;
                    for (int q2 = 0; q2 < nc2; q2++) {
                        const lvert* V = &sv[cf[q2]];
                        if (V->m3 <= 0.0) { thinOk = 0; break; }  /* behind-camera: not the sliver class */
                        if (V->sy < ty0) ty0 = V->sy;
                        if (V->sy > ty1) ty1 = V->sy;
                    }
                    if (!thinOk || ty1 - ty0 > s_thin) continue;
                }
            }
            /* calibration probe (OPENPETE_TERRAIN_CALDBG): find the Σm3 + minm3
             * boundary that separates the .s's deferred cells (queue oracle) from
             * the rest, to calibrate face_is_near's scale/threshold. */
            if (faceStride == 16 && s_caldbg && front) {  /* front-facing only */
                double rs = sv[i0].m3+sv[i1].m3+sv[i2].m3+sv[i3].m3;
                double rmn = sv[i0].m3; if(sv[i1].m3<rmn)rmn=sv[i1].m3;
                if(sv[i2].m3<rmn)rmn=sv[i2].m3; if(sv[i3].m3<rmn)rmn=sv[i3].m3;
                if (is_deferred(faces + (uint32_t)f * 16u)) {
                    s_cal_qn++; if (rs > s_cal_qsum_max) s_cal_qsum_max = rs;
                    if (rmn > s_cal_qmin_max) s_cal_qmin_max = rmn;
                } else if (rs > 0 && rs < 4096.0) {
                    if (rs < s_cal_nqsum_min) s_cal_nqsum_min = rs;
                }
            }
            /* Near/HQ subdivide dispatch: if this face classifies as a
             * near foreground cell (face_is_near, replicating the .s defer test from
             * gamestate), subdivide it 2×2 and texture from the per-cell HQ record
             * (word2 & 0x7F -> m_HQTexturePointer + idx*168; word2 & 0x80 -> ABE)
             * instead of the flat LQ emit - exactly the cells func_800276A0
             * subdivides. Corner->vertex: TL=i0,TR=i1,BL=i3,BR=i2 (the .s grid order).
             * Near colours are the raw HP near-colour array (no DPCS fog on near
             * cells - func_800276A0 reads s5[idx] directly). */
            /* Near TRIANGLES (i2==i3) never take the quad subdivides (BL==BR
             * would make degenerate sub-quads). The drain splits them in
             * triangle passes of their own, the +0x1E00 queue's 16-way split
             * (0x80029588) and the +0x2000 queue's 4-way split (0x80027F28):
             * terrain_near_emit_tri (tn_face_near). */
            /* Portal-guard tag (PSX mask-bit mirror): the .s DEFERS this face
             * (DrawPrim-drawn, mask-protected from the OT walk incl. the portal
             * sky) - every tri it emits below (near-subdivide OR the regular
             * fall-through) gets re-fed as OPENPETE_SCENE_PORTAL_GUARD when
             * portals are active (terrain_feed_native). Tag from the QUEUE
             * ORACLE (the .s's own defer decision), NOT face_is_near - the
             * classifier over-covers the real queue by ~20x (see the defer
             * calibration note) and would wrongly protect the through-hole
             * floor the preview must cover. */
            W->emit_guard = (faceStride == 16 && s_defer_n > 0 &&
                            is_deferred(faces + (uint32_t)f * 16u));
            /* `front` is NCLIP on the PROJECTED corners - meaningless when a
             * corner sits behind the camera (proj_screen mirrors it) and not
             * the .s verdict when a corner carries the GTE vertex flag (the
             * .s skips the face NCLIP there). tface_facing decides a
             * straddler's facing on its camera-space corners instead (see
             * there). */
            int straddle = tface_straddle(sv, i0, i1, i2, i3, quad, faceStride == 16);
            int near_front, cfront;
            const int satv = tface_facing(sv, i0, i1, i2, i3, quad, flip, F->dside,
                                          faceStride == 16, straddle, F->w3, front, front2,
                                          &near_front, &cfront);
            W->dbg_satback += satv & 1; W->dbg_satemit += satv >> 1;
            int near = (faceStride == 16 && s_nearhq && hqTable && near_front &&
                        tn_face_near(sv, F, faces + (uint32_t)f * 16u));
            {   /* OPENPETE_TERRAIN_NEARDBG: census of STRADDLING foreground quads
                 * (a corner behind the camera) by route - near/HQ vs LQ fall-through. */
                static int s_ndbg = -1; static long s_nd_near, s_nd_excl, s_nd_lq; static int s_nd_n;
                ENV_FLAG_LATCH(s_ndbg, "OPENPETE_TERRAIN_NEARDBG");
                int vis_straddle = straddle && (sv[i0].m3 > 0.0 || sv[i1].m3 > 0.0 ||
                                                sv[i2].m3 > 0.0 || sv[i3].m3 > 0.0);
                if (s_ndbg && faceStride == 16 && quad && vis_straddle) {
                    static long s_nd_q, s_nd_qtot;   /* .s-queue oracle agreement (needs VERIFY) */
                    if (near) s_nd_near++;
                    else if (tface_near_excl(hqW3)) s_nd_excl++;   /* hard-excluded from HQ (no Tiledef) */
                    else s_nd_lq++;
                    if (s_defer_n > 0) { s_nd_qtot++;
                        if (is_deferred(faces + (uint32_t)f * 16u)) s_nd_q++; }
                    if (++s_nd_n >= 200) {
                        fprintf(stderr, "[neardbg] visible straddling quads: near=%ld excl=%ld lq=%ld  queue: %ld/%ld deferred\n",
                                s_nd_near, s_nd_excl, s_nd_lq, s_nd_q, s_nd_qtot);
                        s_nd_n = 0;
                    }
                }
            }
            if (s_nearq >= 0)   /* debug: use the queue oracle directly (exact 37) */
                near = (faceStride == 16 && s_nearhq && hqTable && tn_queue_near(quad, faces + (uint32_t)f*16u));
            if (near) {
                tn_near_face(W, cpu, sv, F, host_col, h_near, nearBase, hqTable,
                             faces + (uint32_t)f * 16u, straddle);
                FARDBG_DROP("nearhq");  /* emitted subdivided - the face-set
                                         * join can't corner-match the parent */
                continue;
            }
            /* NCLIP back-face cull, mirroring the .s walk. A Z-buffer hides
             * *occluded* back-faces, but NOT back-faces that sit nearer than
             * the front geometry: back-to-back wall pairs (a room interior
             * and the cliff/waterfall outside it) interpenetrate within a few
             * world units, so emit-all let the far side's back-face win depth
             * and punch through. The game
             * never draws those faces at all - NCLIP culls them - so cull is
             * the canonical visibility rule, not a perf knob.
             * Straddling faces (a corner behind the camera) take
             * tface_facing's camera-space verdict (HP) or are waived (LP).
             * One carve-out where the .s verdict can't be mirrored:
             *  - fill passes: native-only hole cover with no .s counterpart,
             *    stencil-gated to pixels nothing else drew - stay emit-all.
             * Both tiers are whole-face verdicts: the .s emits the full
             * GT4/GT3 when either half accepts (LP: block_80026490's emit
             * path is shared by the front, double-sided and half-2 routes). */
            /* OPENPETE_TERRAIN_CULL bitmask: bit0 = cull HP faces, bit1 =
             * cull LP faces (both default on; each mirrors its walk's exact
             * per-half NCLIP rule above). */
            int cullable = ((faceStride == 16) ? (s_cull & 1) : (s_cull & 2))
                           && !passFill;
            {   /* OPENPETE_TERRAIN_CULLDBG="frame,x0,y0,x1,y1" (PSX screen rect):
                 * log every back-verdict face whose bbox intersects the rect. */
                static int s_cdbg = -2; static long s_cd[5];
                if (s_cdbg == -2) {
                    const char* e = getenv("OPENPETE_TERRAIN_CULLDBG");
                    s_cdbg = (e && sscanf(e, "%ld,%ld,%ld,%ld,%ld",
                              &s_cd[0], &s_cd[1], &s_cd[2], &s_cd[3], &s_cd[4]) == 5);
                }
                if (s_cdbg > 0 && !cfront && openpete_frame >= (unsigned)s_cd[0]
                    && openpete_frame < (unsigned)s_cd[0] + 2u) {
                    float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
                    const int ci[4] = { i0, i1, i2, i3 };
                    int nc = quad ? 4 : 3;
                    for (int q2 = 0; q2 < nc; q2++) {
                        const lvert* V = &sv[ci[q2]];
                        if (V->sx < bx0) bx0 = V->sx; if (V->sx > bx1) bx1 = V->sx;
                        if (V->sy < by0) by0 = V->sy; if (V->sy > by1) by1 = V->sy;
                    }
                    if (bx1 >= s_cd[1] && bx0 <= s_cd[3] && by1 >= s_cd[2] && by0 <= s_cd[4]) {
                        fprintf(stderr, "[culldbg] s=%u f=%d hp=%d q=%d flip=%d abe=%d w3=%08X "
                                "nclip=%.2f straddle=%d pass=%d bbox=%.0f,%.0f,%.0f,%.0f m3=%.0f,%.0f,%.0f,%.0f\n",
                                i, f, faceStride == 16, quad, flip, F->abe, F->w3,
                                nclip, straddle, pass, bx0, by0, bx1, by1,
                                sv[i0].m3, sv[i1].m3, sv[i2].m3, sv[i3].m3);
                    }
                }
            }
            if (cullable && !cfront) { W->dbg_nclip++; FARDBG_DROP("nclip"); continue; }
            /* Native per-corner colour: RGB888 + DPCS fog (HP) or flat (LP). The
             * colour index is per face corner (word1); the fog viewZ is the
             * corner's vertex m3. */
            uint32_t rc0, rc1, rc2, rc3;
            if (host_col) {
                rc0 = terrain_color_h(passLP, h_lp, h_near, h_far, c0, sv[i0].vz, fogRef);
                rc1 = terrain_color_h(passLP, h_lp, h_near, h_far, c1, sv[i1].vz, fogRef);
                rc2 = terrain_color_h(passLP, h_lp, h_near, h_far, c2, sv[i2].vz, fogRef);
                rc3 = quad ? terrain_color_h(passLP, h_lp, h_near, h_far, c3, sv[i3].vz, fogRef) : 0;
            } else {
                rc0 = terrain_color(cpu, passLP, lpColBase, nearBase, farBase, c0, sv[i0].vz, fogRef);
                rc1 = terrain_color(cpu, passLP, lpColBase, nearBase, farBase, c1, sv[i1].vz, fogRef);
                rc2 = terrain_color(cpu, passLP, lpColBase, nearBase, farBase, c2, sv[i2].vz, fogRef);
                rc3 = quad ? terrain_color(cpu, passLP, lpColBase, nearBase, farBase, c3, sv[i3].vz, fogRef) : 0;
            }
            /* Authored-data corrections (unpainted vertex colours) - see
             * k_lp_artfix. Applies to LP world faces and their fill twins;
             * the table indexes the scene's own faces, so borrowed faces
             * take none. */
            if (passLP && !passBorLP) {
                uint32_t fixrgb;
                if (lp_artfix_lookup(cpu, (int)i, f, &fixrgb))
                    rc0 = rc1 = rc2 = rc3 = fixrgb;
            }
            /* Pack-aware LP grade: regrade the authored (stock-space) LP
             * colours the way the pack regraded this sector's own textures,
             * restoring the colour agreement the .s handoff relies on. */
            if (passLP && s_lpg_active) {
                rc0 = lp_grade_apply(SC, rc0);
                rc1 = lp_grade_apply(SC, rc1);
                rc2 = lp_grade_apply(SC, rc2);
                if (quad) rc3 = lp_grade_apply(SC, rc3);
            }
            /* UV scroll (env texture anim): translate every corner V by
             * the tile's fractional offset. One block covers GT4, GT3 (the
             * rotation below only permutes which corner feeds which slot, so a
             * uniform translation is permutation-invariant), both LOD halves
             * (the game writes the same scroll byte into the V slots of both
             * halves of the tiledef entry, whatever its stride, so tv and tvN
             * share one offset), and near-plane clipping (ngte_clip_lerp interpolates
             * UV linearly). */
            float tvScroll[4];
            if (s_tanim_have && faceStride == 16 && !W->emit_bank && F->tb != -1 && s_tanim_act[F->tb & 0x7F]) {
                float dv = s_tanim_dvlq[F->tb & 0x7F];
                for (int k2 = 0; k2 < 4; k2++) tvScroll[k2] = tv[k2] + dv;
                tv = tvScroll;
            }
            /* UV corners. decode_tiledef gives tu/tv[0..3] = [TL,TR,BL,BR]. A QUAD
             * (GT4) uses them directly via the .s slot reorder (i0->TL, i1->TR,
             * i3->BL, i2->BR). A TRIANGLE (GT3) carries a 2-bit UV ORIENTATION in
             * bits 8-9 of the texture word (w2): the .s GT3 emit (func_800258F0
             * .L80027534 - `t4 & 0x300`) selects one of 4 corner rotations,
             * applied below through triRot. */
            lcorner k0 = { rc0, tu[0], tv[0] };                  /* i0 = TL */
            lcorner k1 = { rc1, tu[1], tv[1] };                  /* i1 = TR */
            lcorner k3 = { rc3, tu[2], tv[2] };                  /* i3 = BL */
            lcorner k2 = { rc2, quad ? tu[3] : tu[2], quad ? tv[3] : tv[2] };  /* i2 = BR/BL */
            /* GPU triangulation (affine-faithful): a quad is the prim strip
             * slots (0,1,2)+(1,2,3) = (i0,i1,i3)+(i1,i3,i2) - diagonal i1-i3
             * (NOT a FAN i0-i2: affine texture/colour warp is diagonal-dependent,
             * the cause of the "slanted textures"). A tri is just (i0,i1,i2). */
            if (quad && faceStride == 8) {
                /* LP (untextured G4) quad: the .s LP loop (r_environment .L80026390)
                 * NCLIPs tri(i0,i1,i2) then tri(i3,i1,i2) - a plain GPU strip with
                 * diagonal i1-i2 and NO i2/i3 slot swap. (HP's GT4 swaps i2/i3 below;
                 * applying that swap to LP's scanline-ordered corners bowties the quad
                 * -> overlapping tris + gouraud-colour bleed = the missing distant-
                 * terrain polys + brown artifacts.) Untextured, so corner UVs unused.
                 * Face visibility (either NCLIP half) was decided above; the GPU
                 * draws the whole G4, so both halves emit together. */
                emit_tri(W, sv, i0, i1, i2, k0, k1, k2, mat);
                emit_tri(W, sv, i1, i2, i3, k1, k2, k3, mat);
            } else if (quad) {
                emit_tri(W, sv, i0, i1, i3, k0, k1, k3, mat);
                emit_tri(W, sv, i1, i3, i2, k1, k3, k2, mat);
            } else {
                /* GT3 UV orientation: corner indices into [TL,TR,BL,BR]=[0,1,2,3]
                 * for slots (i0,i1,i2). rot 0 = {TL,TR,BL} (the .s .L80027588),
                 * 1 = {TR,BR,TL}, 2 = {BR,BL,TR}, 3 = {BL,TL,BR} (.L800275A4/C4/DC). */
                static const int triRot[4][3] = {{0,1,2},{1,3,0},{3,2,1},{2,0,3}};
                int rot = (faceStride == 16) ? (int)F->trirot : 0;
                const int* tcn = triRot[rot];
                lcorner t0 = { rc0, tu[tcn[0]], tv[tcn[0]] };
                lcorner t1 = { rc1, tu[tcn[1]], tv[tcn[1]] };
                lcorner t2 = { rc2, tu[tcn[2]], tv[tcn[2]] };
                tn_flat_tri_emit(W, cpu, F, faceStride == 16, straddle, mat,
                                 tn_cv(&sv[i0], t0), tn_cv(&sv[i1], t1), tn_cv(&sv[i2], t2));
            }
        }
      }  /* pass (LP / HP) */
        {   /* OPENPETE_TERRAIN_BINDBG: calibrate the binning-centre projection
             * against the sector's ACTUAL projected vertex-Z range (one frame). */
            static int s_bindbg = -1; static unsigned s_bin_f; static int s_bin_lines;
            ENV_FLAG_LATCH(s_bindbg, "OPENPETE_TERRAIN_BINDBG");
            (void)s_bin_f;
            if (s_bindbg && nvis >= 20 && s_bin_lines < 200) {
                s_bin_lines++;
                fprintf(stderr, "[bindbg] f=%u sec=%u flags=%04X cbin=%.0f r=%d m3=[%.0f..%.0f] lodThr=%d hp=%d lp=%d fill=%d\n",
                        openpete_frame, i, flags, cbin, SC->radius, sd_m3min, sd_m3max, lodThr, drawHP, drawLP, fillLP);
            }
        }
        if (s_sd_active && s_sd_n < 1024) {
            secrec* r = &s_sd[s_sd_n++];
            r->idx = (int)i; r->flags = flags;
            r->noHP = (drawLP ? 1 : 0) | (drawHP ? 2 : 0);   /* 1=LP 2=HP 3=BOTH */
            r->nLpV = nLpV; r->nLpF = nLpF; r->nHpV = nHpV; r->nHpF = nHpF;
            r->nv = sd_nv; r->nf = sd_nf; r->tris = W->tri_n - sd_triBefore; r->vfront = sd_vfront;
            r->ox = originX; r->oy = originY; r->oz = originZ;
            r->m3min = sd_m3min; r->m3max = sd_m3max;
            r->bx0 = sd_bx0; r->by0 = sd_by0; r->bx1 = sd_bx1; r->by1 = sd_by1;
        }
        if (s_fardbg_active)
            fprintf(stderr, "[fardbg-secend] s=%d tris=%d nv=%d nf=%d vfront=%d "
                            "drawHP=%d drawLP=%d\n",
                    (int)i, W->tri_n - sd_triBefore, sd_nv, sd_nf, sd_vfront,
                    drawHP, drawLP);
}

/** @brief Workpool job: walks one prepass sector into a worker's triangle stream and records the run.
 *
 * Sets s_pre_rec[item] to the worker, the worker's tri_n before the walk and
 * the triangles the walk added; the count is 0 when the prepass resolved no
 * decoded sector for the item. Writes only s_wctx[worker] and
 * s_pre_rec[item].
 * @param item The index into the visible-sector list.
 * @param worker The worker id, below TN_MAX_WORKERS.
 * @param arg The tn_wcfg of this walk. */
static void walk_sector_job(int item, int worker, void* arg) {
    tn_wcfg* G = (tn_wcfg*)arg;
    tn_wctx* W = &s_wctx[worker];
    s_pre_rec[item].count = 0;
    if (!G->SC[item]) return;
    s_pre_rec[item].worker = (uint8_t)worker;
    s_pre_rec[item].start  = W->tri_n;
    walk_sector(W, G, item);
    s_pre_rec[item].count  = W->tri_n - s_pre_rec[item].start;
}

/** @brief Builds the retained path's global flag word (OPTC_GF_*) for this present.
 *
 * Each bit is one walk rule the vertex stage mirrors: the near/HQ subdivide,
 * CLUT fog, the HP and LP back-face culls, the LOD handoff, gap fill,
 * --no-lod, the always-HQ pin, the layout's nearest-corner ladder, near-band
 * OT keys, the 4x4 cells, the tier-4 and tier-2 triangle splits, and OT key
 * resolution.
 * @param nearhq 1 when the near/HQ subdivide is on and an HQ table exists.
 * @param lod_handoff 1 when the LP LOD handoff is on.
 * @return The flag word. */
static uint32_t tc_global_flags(int nearhq, int lod_handoff) {
    return (nearhq            ? OPTC_GF_NEARHQ  : 0u)
         | (s_clutfog         ? OPTC_GF_CLUTFOG : 0u)
         | ((s_cull & 1)      ? OPTC_GF_CULLHP  : 0u)
         | ((s_cull & 2)      ? OPTC_GF_CULLLP  : 0u)
         | (lod_handoff       ? OPTC_GF_HANDOFF : 0u)
         | (s_gapfill_en      ? OPTC_GF_GAPFILL : 0u)
         | (openpete_no_lod   ? OPTC_GF_NOLOD   : 0u)
         /* the vertex stage's variant select mirrors face_is_near's
          * extended branch. */
         | (hq_pin_active()   ? OPTC_GF_HQPIN   : 0u)
         | (game_layout.terrain.ladder_nearest_corner ? OPTC_GF_LADNEAR : 0u)
         | (game_layout.terrain.nearband_ot_keys      ? OPTC_GF_NBKEYS  : 0u)
         | (game_layout.terrain.hq_cells4             ? OPTC_GF_CELLS4  : 0u)
         | (game_layout.terrain.hq_tri4_cells         ? OPTC_GF_TRI4    : 0u)
         | (game_layout.terrain.hq_tri2_tiles         ? OPTC_GF_TRI2    : 0u)
         | (game_layout.terrain.lp_near_sz            ? OPTC_GF_LPNEAROUT : 0u)
         | ((s_otbias_on && openpete_native_key_resolve_on()) ? OPTC_GF_KEYON : 0u);
}

/* Sector occlusion-cull verdicts, in the order they are tested. */
enum { TOCC_KEEP = 0, TOCC_FRUSTUM = 1, TOCC_DEPTH = 2 };

/** @brief Returns the occlusion-cull verdict for a sector's view-space bounding sphere under this walk's camera.
 *
 * The frustum reject is a test on the sector's own projected rect, so every
 * walk evaluates it under its own camera. The depth verdict reads the
 * occluder grid, which advances once per present while the canonical walk
 * runs once per tick: a sub-tick re-extract reuses its tick's canonical depth
 * verdict (s_tocc_latch) instead of querying a grid that has moved on. A
 * reused frustum verdict would be wrong in the other direction: the
 * re-extract's interpolated camera lags the tick camera, so a sector the tick
 * camera has panned off is still in the re-extract's frame. A sector not
 * wholly in front of the camera has a meaningless rect and is kept.
 * @param i The sector index.
 * @param oc1 The sphere centre's horizontal view coordinate.
 * @param oc2 The sphere centre's vertical view coordinate.
 * @param oc3 The sphere centre's view depth.
 * @param radius The sphere radius.
 * @return TOCC_KEEP, TOCC_FRUSTUM or TOCC_DEPTH. */
static int tocc_sector_verdict(uint32_t i, double oc1, double oc2, double oc3,
                               double radius)
{
    if (!(oc3 > radius + 1.0)) return TOCC_KEEP;
    double inv = s_H / oc3;
    float sx  = (float)(s_ofx + oc1 * inv);
    float sy  = (float)(s_ofy + oc2 * inv);
    float rpx = (float)(radius * inv * 1.5 + 8.0);            /* generous bbox */
    float vz_near = (float)((oc3 - radius) * 16.0);           /* world/16 -> full-world */
    int own_off = openpete_native_occl_offscreen(sx - rpx, sy - rpx,
                                                 sx + rpx, sy + rpx, vz_near);
    if (!s_vanim_sub) {
        if (own_off) return TOCC_FRUSTUM;
        return openpete_native_occl_query(sx - rpx, sy - rpx, sx + rpx, sy + rpx,
                                          vz_near, &s_occl_cam) ? TOCC_DEPTH : TOCC_KEEP;
    }
    int lat = (i < 1024u && s_tocc_latch_stamp[i] == s_tocc_tick) ? s_tocc_latch[i]
                                                                  : TOCC_KEEP;
    int v = own_off ? TOCC_FRUSTUM : (lat == TOCC_DEPTH ? TOCC_DEPTH : TOCC_KEEP);
    if (lat == TOCC_FRUSTUM && !own_off) {
        if (v) s_tocc_latoff++;
        else   s_tocc_offadm++;
    }
    return v;
}

/** @brief Walks the visible sectors of the current view into the terrain triangle stream and feeds the native scene. */
static void terrain_build(CPUState* cpu, int32_t occGroup) {
    ngte_view_build(cpu, &s_view);
    /* Publish this walk's camera for the occluder grid: the depth pre-pass of
     * the scene this walk feeds is what the grid reduces, so the grid must be
     * tagged with this camera and queries evaluated under a camera that has
     * moved away from it must not take its verdict. */
    ngte_occl_camera(&s_view, &s_occl_cam);
    openpete_native_occl_set_camera(&s_occl_cam);
    s_vanim_pres++;                /* new build -> prior lerp/roll bank stamps go stale */
    terrain_texrecords(cpu);
    terrain_animdbg(cpu);
    tsc_census();
    tsc_vhash_census_flush();
    {   /* OPENPETE_TERRAIN_OCCL_DBG: occlusion-cull census (~1/120 walks) */
        static int on = -1, n; static long prev_tris;
        ENV_FLAG_LATCH(on, "OPENPETE_TERRAIN_OCCL_DBG");
        if (on && ++n >= 120) {
            fprintf(stderr, "[ter-occl f=%u] sectors-culled=%ld/window last-tris=%ld\n",
                    openpete_frame, s_tocc_culled, prev_tris);
            s_tocc_culled = 0; n = 0;
        }
        prev_tris = s_tri_n;
    }
    if (!s_vanim_sub) s_tocc_tick++;
    s_fardbg_occl0 = s_tocc_culled;
    s_tocc_latoff = s_tocc_offadm = 0;
    s_tv_n = 0; s_tri_n = 0; s_cv_n = 0; s_walk_sectors = 0;
    s_dbg_faces = s_dbg_oob = s_dbg_nclip = s_dbg_behind = s_dbg_spike = s_dbg_clipped = 0;
    s_dbg_satback = s_dbg_satemit = 0;
    s_dbg_spike_tris = 0;
    trisplit_reset(s_trisplit_faces, s_trisplit_subs, s_trisplit_scull, s_trisplit_matdiff);
    memset(s_cells4_faces, 0, sizeof(s_cells4_faces));
    memset(s_drain_skip, 0, sizeof(s_drain_skip));
    s_drain_skip_legacy = tn_drain_skip_legacy();
    s_near_q_agree = s_near_c_only = 0;
    ENV_FLAG_LATCH(s_sectordump, "OPENPETE_TERRAIN_SECTORDUMP");
    s_sd_n = 0;
    /* Fire the sector dump once, on the target frame (OPENPETE_TERRAIN_VERIFY_FRAME). */
    { static int s_sd_fired = 0; static unsigned s_sd_frame = 0xFFFFFFFFu;
      if (s_sd_frame == 0xFFFFFFFFu) { const char* e = getenv("OPENPETE_TERRAIN_VERIFY_FRAME");
                                       s_sd_frame = e ? (unsigned)strtoul(e, NULL, 0) : 0; }
      s_sd_active = (s_sectordump > 0 && !s_sd_fired && openpete_frame >= s_sd_frame);
      if (s_sd_active) s_sd_fired = 1; }
    ENV_FLAG_LATCH(s_caldbg, "OPENPETE_TERRAIN_CALDBG");
    if (s_nearq == -2) { const char* e = getenv("OPENPETE_TERRAIN_NEARQ"); s_nearq = (e && *e && *e != '0') ? 1 : -1; }
    s_cal_qn = 0; s_cal_qsum_max = 0; s_cal_qmin_max = 0; s_cal_nqsum_min = 1e18;
    /* The render decision uses face_is_near (gamestate); the deferred QUEUE is read
     * ONLY as a verify oracle / for the NEARQ debug path - never in the shipping
     * feed path, which does not consume the .s's per-frame output. */
    int wantOracle = terrain_game.portal_sky_recent() ||   /* portal guard reads the queue */
                     s_caldbg || s_nearq > 0 ||
                     terrain_verify_env();
    terrain_dbg_init();
    /* FAITHFUL LOD (OPENPETE_TERRAIN_LOD_FAITHFUL, DEFAULT ON): mirror the .s
     * sector binning - HP list = lodThr > centreZ − radius (HP ends at the LOD
     * distance), LP list = 0x8000 || lodThr < centreZ + radius + 0x100 (LP
     * substitutes beyond it), noLP 0x2000 tested first (overrides force-LP).
     * With the exact per-face tests in place the bin is almost pure pre-pass:
     * every face of an HP-unlisted sector fails the per-face drop anyway
     * (all corners ≥ c − r ≥ lodDist), and the LP bin's +0x100 equals the
     * admission s3's −0x20 (both 4096 world), so a=0 faces agree at the
     * boundary by construction. The behavioural content is the near-sector
     * LP exclusion the per-face admission alone cannot see: an a=31
     * (admit-to-camera) face in a fully-near sector is culled by the .s at
     * the sector level. Forced OFF under
     * --no-lod: HP-everywhere IS that mode's intent. NB the bin uses the raw
     * lodThr - under a user-stretched --fog-scale the per-face tests ride the
     * curve and this doesn't; stock is exact. =0 restores admission-only. */
    if (s_tier_faithful < 0) {
        const char* e = getenv("OPENPETE_TERRAIN_LOD_FAITHFUL");
        s_tier_faithful = (e && *e) ? (*e != '0') : 1;
    }
    /* no-lod override applied per walk (not baked into the latch): the
     * draw-distance toggle can flip openpete_no_lod mid-level. */
    int tier_faithful = s_tier_faithful && !openpete_no_lod;
    if (s_fogscale < 0.0) {  /* viewZ = m3 (m3 already in SZ3 scale); env-tunable */
        const char* e = getenv("OPENPETE_TERRAIN_FOGSCALE");
        s_fogscale = (e && *e) ? atof(e) : 1.0;
    }
    if (s_fogdpcs < 0) {
        /* DEFAULT ON: the exact decode is per-corner exact at every band. It
         * pairs with the CLUT-row fog (s_clutfog); without that row fog the
         * under-fogged lerp partly compensates and the exact decode alone
         * over-brightens vistas. */
        const char* e = getenv("OPENPETE_TERRAIN_FOGDPCS");
        s_fogdpcs = (e && *e) ? (*e != '0') : 1;
    }
    /* The whole fog model rides ONE user knob, --fog-scale (fog-distance
     * multiplier k; openpete_fog_scale: explicit --fog-scale wins, else 100
     * under --no-lod / 1 stock). Internally SZ = s_fogscale·m3 with
     * s_fogscale = 4/k: k=1 -> the exact PSX curve (SZ = 4·m3); k=100 ->
     * saturation ~100x farther, i.e. fog effectively off - extended LOD
     * already draws the far detail, so the default stops fogging it away.
     * (k=4 gives s_fogscale 1, the lerp curve's scale.) The exact math, the
     * palette ladder and the saturation handoff all ride the stretched curve.
     * Re-resolved EVERY walk, not latched at boot: the settings-menu
     * draw-distance toggle (openpete_no_lod_set) flips openpete_fog_scale
     * live, and the LP/HP fog-saturation handoff below rides s_fogscale, so
     * a boot-latched value would keep the handoff at the boot mode's
     * saturation distance after a toggle. OPENPETE_TERRAIN_FOGSCALE
     * force-overrides (debug). */
    {
        static int s_fs_env = -1;
        if (s_fs_env < 0) s_fs_env = getenv("OPENPETE_TERRAIN_FOGSCALE") ? 1 : 0;
        if (s_fogdpcs && !s_fs_env)
            s_fogscale = 4.0 / (double)openpete_fog_scale;
    }
    if (s_clutfog < 0) {
        const char* e = getenv("OPENPETE_TERRAIN_CLUTFOG");
        s_clutfog = (e && *e) ? (*e != '0') : 1;
    }
    /* .s LP/HP fog-saturation handoff - HP faces are dropped once FULLY beyond
     * fog saturation (ir0 <= 0 at every corner) and LP substitutes (far-only).
     * DEFAULT ON: beyond saturation the .s
     * picture IS the LP tier (raw palette - identity-join-proven), and any HP
     * content native paints there is invented (the artisans far-water green
     * stripe under the CLUT ladder). The swap happens strictly inside
     * fully-fogged pixels, so there is no visible pop-in. It is disabled
     * for the walk under --no-lod (see lod_handoff below).
     * OPENPETE_TERRAIN_LOD_HANDOFF=0 restores HP-everywhere (A/B). */
    {
        static tn_envvbl_t hc, gc;
        /* Rides the --fog-scale curve via s_fogscale (the per-face beyond
         * test below uses it), so under a stretched curve the handoff fires
         * only at the stretched saturation distance. */
        s_lod_handoff = tn_env_vbl(&hc, "OPENPETE_TERRAIN_LOD_HANDOFF", 1, 1);
        /* =0 disables the near-LP-gap fill (A/B) */
        s_gapfill_en  = tn_env_vbl(&gc, "OPENPETE_TERRAIN_GAPFILL", 1, 1);
    }
    /* Effective handoff for THIS walk. Under --no-lod the near/far LOD handoff
     * must NOT run: it keys off the RAW-RAM lodDist (--lod extended is pure
     * host-side policy - the openpete_no_lod global; RAM keeps the authored
     * 0x8000), so it keeps dropping HP at the real fog-saturation band (m3 >= fogRef) and opens
     * the HP/LP seam the from-above freecam view exposes (an LP-only far sector
     * abutting an HP near sector, neither reaching the boundary). --lod extended
     * means HP everywhere it exists; the poke-free holefill bridges the residual
     * sector-boundary seam. Original LOD keeps the handoff verbatim - this local
     * equals s_lod_handoff there, so that path stays byte-for-byte unchanged. */
    int lod_handoff = s_lod_handoff && !openpete_no_lod;
    if (s_nearz < 0.0) {     /* near plane in world/16 view-Z (env-tunable) */
        const char* e = getenv("OPENPETE_TERRAIN_NEARZ");
        s_nearz = (e && *e) ? atof(e) : 1.0;
    }
    { static tn_envvbl_t fog_c; s_fogdbg = tn_env_vbl(&fog_c, "OPENPETE_TERRAIN_FOGDBG", 0, 1); }
    /* Far-band identity instrument (fog decode): one-shot full dump of
     * per-sector binning state + per-face corner records (screen xy, m3, raw
     * colour words per tier) for the offline identity JOIN against the
     * captured .s prims (which self-classify LP/HP by prim code 0x30/0x38 vs
     * 0x34/0x3C). Use with OPENPETE_TERRAIN_VERIFY (no feed) so the single
     * build is the canonical verify one. */
    if (s_fardbg < 0) { const char* e = getenv("OPENPETE_TERRAIN_FARDBG"); s_fardbg = (e && *e) ? atoi(e) : 0; }
    /* value > 1 = fire at that openpete_frame (feed mode, pairs with a
     * --screenshot-on-frame of the same number); 1 = first build (verify mode) */
    if (s_fardbg_budget < 0) { const char* e = getenv("OPENPETE_TERRAIN_FARDBG_N");
                              s_fardbg_budget = (e && *e) ? atoi(e) : 1;
                              if (s_fardbg_budget < 1) s_fardbg_budget = 1; }
    if (s_fardbg_lean < 0) { const char* e = getenv("OPENPETE_TERRAIN_FARDBG_LEAN");
                             s_fardbg_lean = (e && *e && *e != '0') ? 1 : 0; }
    /* The dump spans the next OPENPETE_TERRAIN_FARDBG_N consecutive walks from
     * the trigger frame, so an intermediate re-extract (which runs after the
     * tick's canonical walk) is reachable; N=1 is the single-walk form. */
    s_fardbg_active = s_fardbg && !s_fardbg_done
                      && (s_fardbg == 1 || openpete_frame >= (unsigned)s_fardbg);
    /* OPENPETE_TERRAIN_FACEMAP=<frame>: one-shot per-LP-face screen-footprint
     * dump (fires on the first build at/after <frame> - no VBL-parity trap) for
     * offline hole->face attribution: rasterize the dumped footprints and
     * intersect with the {og covers ∧ native uncovered} pixel mask to derive
     * the fill-face list. Forces a dump-only LP walk for sectors whose LP pass
     * would not otherwise run, so DROPPED candidates are attributable too -
     * the pixels needing attribution are exactly the ones nothing drew. */
    if (s_facemap < 0) { const char* e = getenv("OPENPETE_TERRAIN_FACEMAP"); s_facemap = (e && *e) ? atoi(e) : 0; }
    s_facemap_active = s_facemap && !s_facemap_done && openpete_frame >= (unsigned)s_facemap;
    int M[3][3]; memcpy(M, s_view.cam_M, sizeof M);   /* m_ProjectionMatrix */
    int Mbin[3][3]; ngte_read_matrix(cpu, game_layout.cam.view_matrix, Mbin); /* .s sector-binning matrix (LP/HP LOD lists) */
    {   /* World->view scale bound for the rim broadphase's sphere radius: M is
         * not orthonormal (the Y row bakes a ~0.625 squash), so a world-space
         * radius is only valid in view space scaled by the largest row norm. */
        double rm = 0.0;
        for (int r = 0; r < 3; r++) {
            double n2 = (double)M[r][0]*M[r][0] + (double)M[r][1]*M[r][1]
                      + (double)M[r][2]*M[r][2];
            if (n2 > rm) rm = n2;
        }
        s_rim_rowmax = sqrt(rm) / 4096.0;
        if (s_rim_rowmax < 1e-6) s_rim_rowmax = 1.0;
        /* Row-2 norm alone bounds view-Z growth per world unit - the
         * retained-GPU near claim's radius scale. */
        s_tgpu_l3 = sqrt((double)M[2][0]*M[2][0] + (double)M[2][1]*M[2][1] +
                         (double)M[2][2]*M[2][2]) / 4096.0;
        if (s_tgpu_l3 < 1e-6) s_tgpu_l3 = 1.0;
    }
    s_ofx = s_view.ofx;
    s_ofy = s_view.ofy;
    s_H   = s_view.H_eff;
    /* Arm the fill-tag inspector's pick point set for this walk (see fp_begin:
     * the crosshair, plus an optional grid sweep). */
    s_fp_on = openpete_fillpick_active();
    if (s_fp_on) fp_begin();
    /* Camera position in world/16 units at FRACTIONAL precision (÷16.0, not >>4).
     * The .s projects terrain in world/16 to fit the GTE's 16-bit range, so it
     * truncates the camera to 16-unit steps - fine at the 30 Hz lockstep, but under
     * sub-tick interpolation it makes the whole terrain frame jump in 16-world-unit
     * chunks while mobys/Spyro (full-precision camera) move smoothly = the terrain
     * jitter, worst under rotation (distant terrain sweeps fast). The native path
     * projects in float, so carrying the low 4 bits costs nothing and tracks the
     * smooth camera. Scale is unchanged (still /16), so depth / near-clip / fog are
     * untouched; only the camera reference gains sub-16 precision. */
    /* Sub-tick floor fix: when a re-extract bracket is active, use the FRACTIONAL
     * interpolated camera position (un-rounded) so terrain - which is purely
     * camera-driven - is TRULY continuous, not just stepped at the 1-world-unit
     * lerp32 grid. Falls back to the (floored) RAM at the 30 Hz lockstep. */
    double camx = s_view.cam[0] / 16.0, camy = s_view.cam[1] / 16.0, camz = s_view.cam[2] / 16.0;
    int fogRefBase = (int32_t)tenv_lod_dist(cpu) >> 2;
    /* Publish this walk's fog-ladder ramp for the CONTINUOUS pack-fog model
     * (HD-replaced draws only; see texture_pack_fog_ladder_set): the .s row
     * select is Δ = (key − fogRef + 4096) >> 8 with key ≈ face-mean
     * m3·fogscale and 16 authored rows, so in full-world view-Z (16·m3)
     * t = Δ/15 = vz·fogscale/(16·3840) + (4096 − fogRef)/3840. Face-level
     * +0x2000 fogRef bias rides the per-draw fog_delta flags. */
    texture_pack_fog_ladder_set(
        s_clutfog ? (float)(s_fogscale / (16.0 * 3840.0)) : 0.0f,
        s_clutfog ? (float)((4096.0 - (double)fogRefBase) / 3840.0) : 0.0f);
    /* Pack-aware LP grade (tsc_lp_grade): fold the previous walk's level mean
     * for no-signal sectors, then arm and reset this walk's accumulator. */
    if (s_lpg_en < 0) { const char* e = getenv("OPENPETE_TEXPACK_LP_FIT");
                        s_lpg_en = !(e && *e == '0'); }
    s_lpg_active = s_lpg_en && texture_pack_replacements_active();
    /* The mean is a stand-in for sectors of THIS level that yielded no fit; it
     * must not survive a level change. A pack covering one level leaves every
     * sector of the next unmatched, so the accumulator stays empty there and
     * the fold below would pin the old level's mean over its whole LP tier
     * indefinitely. */
    { int lvl = (int)(int32_t)rd(cpu, game_layout.level_id);
      static int s_lpg_lvl = -1;
      if (lvl != s_lpg_lvl) {
          tgpu_level_edge(s_lpg_lvl, lvl);
          s_lpg_lvl = lvl;
          s_lpg_mean_ok = 0;
          s_lpg_acc_n = 0;
          memset(s_lpg_acc, 0, sizeof s_lpg_acc);
          /* HD-fold entry table + atlas are level-lifetime (the pack's
           * identities die with the level's VRAM uploads); baked entry ids
           * die with them - the registry-generation dirty check rebakes. */
          openpete_native_hdfold_reset();
      } }
    if (s_lpg_acc_n > 0) {
        for (int c = 0; c < 6; c++) s_lpg_mean[c] = (float)(s_lpg_acc[c] / s_lpg_acc_n);
        s_lpg_mean_ok = 1;
    }
    memset(s_lpg_acc, 0, sizeof s_lpg_acc);
    s_lpg_acc_n = 0;
    /* .s LP far cull threshold, slot units (r_environment.s:637-642: the walk
     * precomputes s2 = m_CullingDistance >> 7 once, before the sector loop). */
    int cullRefS2 = (int32_t)tenv_rd(cpu, game_layout.env.cull_dist) >> 7;
    uint32_t lqTable = tenv_rd(cpu, game_layout.env.lq_tex);  /* HP-walk per-quad Tiledef */
    uint32_t hqTable = tenv_rd(cpu, game_layout.env.hq_tex);  /* near/detail Tiledef (bpp8 tiles) */
    tsc_tile_gens(cpu, lqTable, tenv_rd(cpu, game_layout.env.tex_count));  /* env-anim liveness */
    tsc_scroll_refresh(cpu);                                           /* env-anim UV scroll (sub-tick) */
    tface_differ_refresh(cpu, hqTable, tenv_rd(cpu, game_layout.env.tex_count));
    /* OPENPETE_TERRAIN_TEXTBL: 0=auto(near->HQ,far->LQ) 1=force LQ 2=force HQ (debug) */
    static tn_envvbl_t textbl_c;
    int texTblMode = tn_env_vbl(&textbl_c, "OPENPETE_TERRAIN_TEXTBL", 0, 0);
    if (s_nearhq < 0) {      /* near/HQ subdivide pass enable (Route 2, default ON) */
        const char* e = getenv("OPENPETE_TERRAIN_NEARHQ");
        s_nearhq = (e && *e) ? (*e != '0') : 1;
    }
    s_tex_faces = s_tex_tris = 0;
    if (wantOracle) terrain_read_defer(cpu);   /* oracle only (verify/debug) - see above */
    else s_defer_n = 0;

    static lvert sv[256];
    uint32_t sectorPtr = tenv_rd(cpu, game_layout.env.sector_ptr);
    uint32_t sectorCnt = tenv_rd(cpu, game_layout.env.sector_count);
    tsc_vert_anim_refresh(cpu, sectorPtr, sectorCnt);  /* geometry env-anim liveness */
    tsc_col_anim_refresh(cpu, sectorPtr, sectorCnt);   /* colour env-anim (sub-tick) */
    /* Under extended LOD the walk's sector table extends past the scene's
     * with the HP source level's sectors the scene lacks (tsv_refresh); the
     * scene's own sectors keep their indices and the guest's verdicts. */
    const uint32_t guestCnt = sectorCnt;
    tsv_extend(cpu, &sectorPtr, &sectorCnt);
    /* Visible-sector index set: canonical 4:3 verdict (D_800771C8) at native
     * aspect; occlusion-group candidate set in widescreen (terrain_vislist). */
    static uint16_t visidx[1024];
    /* Sector bounding-sphere frustum inputs, at the half-widths of the
     * displayed frame: 1.0 is the 4:3 frame the draw itself tests, wider under
     * Hor+ or a --fov above the stock angle. Built for every game whose layout
     * names the sector-header sphere (radius_mask); the frustum candidate set
     * and the draw's own sphere cull read the same widths. */
    tvis_sphere sph, *sphp = NULL;
    double cull_wx = 1.0, cull_wy = 1.0;
    if (s_view.extended) {
        cull_wx = (double)s_view.F / s_view.s;   /* Hor+ widens X only; --fov both */
        cull_wy = 1.0 / s_view.s;
    }
    if (game_layout.terrain.radius_mask) {
        sph.sectorPtr = sectorPtr;
        memcpy(sph.M, Mbin, sizeof(sph.M));
        sph.camx = camx; sph.camy = camy; sph.camz = camz;
        sph.wx = cull_wx; sph.wy = cull_wy;
        sphp = &sph;
    }
    int nvis = terrain_vislist(cpu, guestCnt, visidx, occGroup, sphp);
    nvis = tsv_append_vis(cpu, sphp, guestCnt, sectorCnt, visidx, nvis);
    s_tsc_stamp++;   /* LRU epoch: everything touched this walk outlives strangers */
    /* OPENPETE_TERRAIN_TIMERS=1: per-SECTOR walk-cost census (~1/s). Attributes
     * the terrain feeder's µs to individual sectors (accounted at the top of
     * the NEXT iteration so every exit path of the huge loop body is covered)
     * and names the top offenders. Inert unless the env is set. */
    static int tt_on = -1;
    ENV_FLAG_LATCH(tt_on, "OPENPETE_TERRAIN_TIMERS");
    static uint32_t tt_us[1024], tt_tris[1024];      /* window-accumulated per sector */
    static uint64_t tt_win_us, tt_report_t;
    static uint32_t tt_presents, tt_nvis_sum, tt_nvis_max, tt_pres_max_us;
    uint64_t tt_prev = 0; uint32_t tt_prev_i = 0; int tt_prev_valid = 0, tt_prev_tris = 0;
    uint64_t tt_present_us = 0;
    /* ---- parallel sector walk ---------------------------------------------
     * Serial mode (1 thread, any debug/verify instrument active, or the very
     * first walk so every one-time getenv latch initializes single-threaded)
     * fuses walk_sector into the prepass loop below - the exact legacy order,
     * writing s_tri directly. Parallel mode: the prepass resolves the decode
     * cache + occlusion serially (tsc_get mutates shared slots), then the
     * pool walks sectors into per-worker buffers, merged in sector order -
     * byte-identical output to serial by construction. */
    {   /* lp_artfix_lookup latches g_LevelId once per VBL inside the walk;
         * resolve it here, serially, so workers only hit the cached path. */
        uint32_t afx_dummy;
        (void)lp_artfix_lookup(cpu, -1, -1, &afx_dummy);
    }
    tn_wcfg G;
    G.cpu = cpu;
    memcpy(G.M, M, sizeof(G.M)); memcpy(G.Mbin, Mbin, sizeof(G.Mbin));
    G.camx = camx; G.camy = camy; G.camz = camz;
    {   /* OPENPETE_TERRAIN_CAMDBG=1 (temp): per-build game-camera pose in
         * --freecam-pose form, for replanting a movie frame's view. */
        static int s_camdbg = -1;
        ENV_FLAG_LATCH(s_camdbg, "OPENPETE_TERRAIN_CAMDBG");
        if (s_camdbg)
            fprintf(stderr, "[cam f=%u] pos=(%d, %d, %d) yaw=%d pitch=%d sub=%d\n",
                    openpete_frame,
                    (int32_t)rd(cpu, game_layout.cam.position),
                    (int32_t)rd(cpu, game_layout.cam.position + 4u),
                    (int32_t)rd(cpu, game_layout.cam.position + 8u),
                    (int16_t)(rd(cpu, game_layout.cam.rotation + 4u) & 0xFFFFu),
                    (int16_t)(rd(cpu, game_layout.cam.rotation) >> 16),
                    s_vanim_sub);
    }
    G.fogRefBase = fogRefBase; G.cullRefS2 = cullRefS2; G.texTblMode = texTblMode;
    G.cull_wx = cull_wx; G.cull_wy = cull_wy;
    G.tier_faithful = tier_faithful; G.lod_handoff = lod_handoff;
    G.hqTable = hqTable;
    G.visidx = visidx; G.nvis = nvis;
    G.SC = s_pre_SC; G.secptr = s_pre_sec;
    {   static tn_envvbl_t hole_c, fc_c, adm_c, hpd_c;
        G.holefill   = tn_env_vbl(&hole_c, "OPENPETE_TERRAIN_HOLEFILL", 1, 1);
        G.lp_farcull = tn_env_vbl(&fc_c,  "OPENPETE_TERRAIN_LP_FARCULL", 1, 1);
        G.lp_admit   = tn_env_vbl(&adm_c, "OPENPETE_TERRAIN_LP_ADMIT", 1, 1);
        G.hp_drop    = tn_env_vbl(&hpd_c, "OPENPETE_TERRAIN_HP_DROP", 1, 1);
    }
    G.verd = s_pre_verd;
    G.own  = s_tgpu_own;
    /* ---- terrain sector-mesh GPU cache: per-walk gate + globals ----
     * When ON, unclaimed sectors skip the CPU face walk and render from
     * their retained GPU chunks; terrain.vert mirrors every per-face
     * verdict. Whole-walk fallback triggers: renderer-side conditions
     * (openpete_native_tercache_usable), any debug env whose semantics live
     * in the CPU walk, the fill-tag inspector (every face must project to be
     * pickable), and non-default texture-table modes. Portal recency claims
     * per-SECTOR (the outline broad-phase below), not the whole walk. */
    /* One canonical tick can invoke this hook SEVERAL times, appending to
     * one scene stamp (the .s renders more than one environment pass - the
     * portal-preview destination render is the common case - and the interp
     * bracket can flip the camera between the calls). One retained state
     * cannot represent a multi-camera scene, so only the FIRST hook build
     * of a stamp drives it; later same-stamp builds walk pure CPU and must
     * NOT touch the staging (a restage would wipe the first build's
     * sectors). Re-extract builds (s_vanim_sub) always drive: their build
     * IS the displayed state. */
    static uint32_t s_tb_last_stamp = 0xFFFFFFFFu;
    int tb_first = s_vanim_sub || openpete_frame != s_tb_last_stamp;
    if (!s_vanim_sub) s_tb_last_stamp = openpete_frame;
    int tgpu_on = tb_first
               && openpete_tercache_ready()
               && openpete_native_tercache_usable()
               && !tgpu_env_block()
               && !s_fp_on
               && texTblMode == 0;
    s_tgpu_pplane_n = 0; s_tgpu_claim_all = 0; s_tgpu_rim_ok = 0;
    if (tgpu_on && terrain_game.portal_sky_recent()) {
        s_tgpu_pplane_n = terrain_game.portal_sky_planes(cpu, &s_view, s_tgpu_pplane, 6);
        /* Recent portal but planes momentarily gone: claim everything (safe). */
        if (s_tgpu_pplane_n == 0) s_tgpu_claim_all = 1;
        else s_tgpu_rim_ok = tgpu_portal_frames(cpu);
    }
    openpete_tercache_globals tcg;
    if (tgpu_on) {
        memset(&tcg, 0, sizeof(tcg));
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) tcg.mrow[r][c] = (float)M[r][c];
        tcg.ofx = s_ofx; tcg.ofy = s_ofy; tcg.H = s_H; tcg.H_canon = s_view.H_canon;
        tcg.camx = camx; tcg.camy = camy; tcg.camz = camz;
        tcg.fogscale = s_fogscale;
        /* Poly-anim lerp alpha: sub-tick paired builds carry the spine's
         * alpha; canonical (and unpaired) builds draw cur exactly. */
        tcg.vanim_alpha = (s_vanim_sub && s_vanim_on) ? s_vanim_alpha : 1.0;
        tcg.canim_alpha = s_canim_a;   /* env-anim colour lerp alpha (1 = current) */
        tcg.nearz = s_nearz;   /* near-variant clip plane (clip_push_tri) */
        /* Per-tile UV-scroll offsets for this build (zero for a tile that is
         * not on the scroller list). The retained cache and the CPU walk read
         * the same tables, so a sector drawn from a bake scrolls identically
         * to one walked this build. */
        if (s_tanim_have)
            for (int t = 0; t < 128; t++)
                if (s_tanim_act[t]) {
                    tcg.scroll_dv[t][0] = s_tanim_dvlq[t];
                    tcg.scroll_dv[t][1] = s_tanim_dvhq[t][0];
                    tcg.scroll_dv[t][2] = s_tanim_dvhq[t][1];
                    memcpy(tcg.scroll_dvc[t], s_tanim_dvcell[t], sizeof(tcg.scroll_dvc[t]));
                }
        tcg.fog_ref_base = fogRefBase;
        tcg.cull_ref_s2  = cullRefS2;
        tcg.gflags = tc_global_flags(s_nearhq && hqTable, lod_handoff);
        {   /* OPENPETE_TERRAIN_GPU_TINT=1: retained draws tint by sector id
             * (temp bring-up attribution; unregistered). */
            const char* e = getenv("OPENPETE_TERRAIN_GPU_TINT");
            if (e && *e && *e != '0')
                tcg.gflags |= (1u << 29) | ((uint32_t)(atoi(e) & 7) << 26);
        }
        /* Portal inner-rim frames for the vertex-stage cull (zeroed rim_n
         * from the memset when frames are absent - retained tris then carry
         * no rim verdict). */
        tgpu_rim_globals_fill(&tcg);
        tgpu_cover_globals_fill(&tcg);
    }
    /* OPENPETE_TERRAIN_GPU_CENSUS=1: one stderr line per hook walk with THIS
     * walk's claim counts by reason (deltas of the window counters, printed
     * before tgpu_census can reset them) - the claim-count metric,
     * harvested by scripts/terrain-claim-census.py. Temp instrument;
     * unregistered; GPU_-prefixed so it is safe with the cache engaged. */
    static int s_cen = -1;
    ENV_FLAG_LATCH(s_cen, "OPENPETE_TERRAIN_GPU_CENSUS");
    long cen0[13];
    /* Any active walk instrument forces serial: their censuses/dumps are
     * whole-walk statics with serial semantics (per-sector timing, ordered
     * dumps), and several latch mid-walk. */
    static int s_dbg_env = -1;
    if (s_dbg_env < 0)
        s_dbg_env = (getenv("OPENPETE_TERRAIN_TIMERS")   || getenv("OPENPETE_TERRAIN_SECTORDUMP") ||
                     getenv("OPENPETE_TERRAIN_FARDBG")   || getenv("OPENPETE_TERRAIN_FACEMAP") ||
                     getenv("OPENPETE_TERRAIN_CALDBG")   || getenv("OPENPETE_TERRAIN_NEARQ") ||
                     getenv("OPENPETE_TERRAIN_FOGDBG")   || getenv("OPENPETE_TERRAIN_CULLDBG") ||
                     getenv("OPENPETE_TERRAIN_BINDBG")   || getenv("OPENPETE_TERRAIN_BINCHECK") ||
                     getenv("OPENPETE_TERRAIN_NEARDBG")  || getenv("OPENPETE_TERRAIN_LPKEEPDBG") ||
                     getenv("OPENPETE_TERRAIN_VERIFY")) ? 1 : 0;
    static int s_first_walk = 1;
    int nw = openpete_workpool_threads();
    if (nw > TN_MAX_WORKERS) nw = TN_MAX_WORKERS;
    int wserial = s_dbg_env || s_fp_on || s_first_walk || nw <= 1;
    s_first_walk = 0;
  tn_retry_serial:;
    /* Per-walk counters reset AFTER the retry label: an eviction-conflict
     * serial retry replaces the aborted walk's numbers instead of summing
     * with them (summed lines inflated census rows exactly 2x). */
    s_tgpu_secs_walk = 0; s_tgpu_claims_walk = 0; s_tgpu_canim_walk = 0; s_tgpu_canim_mv = 0;
    if (s_cen) {
        cen0[0] = s_tgcl_near;   cen0[1] = s_tgcl_portal;
        cen0[2] = s_tgcl_anim;   cen0[3] = s_tgcl_scroll;
        cen0[4] = s_tgcl_fill;   cen0[5] = s_tgcl_build;
        cen0[6] = s_tgcl_colour; cen0[7] = s_tgcl_slot;
        cen0[8] = s_tgcl_texpack; cen0[9] = s_tgcl_nearb;
        cen0[10] = s_tgcl_nodraw; cen0[11] = s_tgcl_prim;
        cen0[12] = s_tgcl_pcov;
    }
    /* The build stamp: a scene id while an eager scene is being extracted
     * (each scene of a burst gets its own retained state), else the VBL. */
    const uint32_t tc_stamp = openpete_native_scene_build_stamp();
    tgpu_resize_adopt_prepare();
    if (tgpu_on) openpete_tercache_frame_begin(&tcg, tc_stamp, s_vanim_sub);
    else if (openpete_tercache_ready() && tb_first)
        openpete_tercache_frame_skip(tc_stamp, s_vanim_sub);
    int nw_used = wserial ? 1 : nw;
    if (!s_tri_v.elem_size)
        mod_fvec_init(&s_tri_v, sizeof(ttri), TN_TRI_INIT, "terrain.walk", TN_TRI_INIT);
    for (int w = 0; w < nw_used; w++) {
        tn_wctx* W = &s_wctx[w];
        W->out = wserial ? &s_tri_v : &s_wbuf[w];
        if (!W->out->elem_size)
            mod_fvec_init(W->out, sizeof(ttri), TN_TRI_INIT, "terrain.walk.worker", TN_TRI_INIT);
        W->tri = (ttri*)W->out->data; W->tri_cap = (int)W->out->cap;
        W->tri_n = 0;
        W->dbg_faces = W->dbg_oob = W->dbg_nclip = W->dbg_behind = 0;
        W->dbg_spike = W->dbg_clipped = 0;
        W->dbg_satback = W->dbg_satemit = 0;
        W->dbg_spike_tris = 0;
        trisplit_reset(W->trisplit_faces, W->trisplit_subs, W->trisplit_scull, W->trisplit_matdiff);
        memset(W->cells4_faces, 0, sizeof(W->cells4_faces));
        memset(W->drain_skip, 0, sizeof(W->drain_skip));
        W->tex_faces = W->tex_tris = 0;
        W->near_q_agree = W->near_c_only = 0;
        W->walk_sectors = 0;
        W->serial = wserial;
    }
    /* Faces the CPU face walk will actually process this build: sectors the
     * retained-GPU cache owns return from walk_sector before touching a face,
     * so their geometry is not walk work and must not count toward the
     * fan-out decision below. With the cache warm this total collapses to
     * near zero even on a whole-level vista. */
    long walk_faces = 0;
    if (!wserial && op_prof_on()) {
        static int id_ = -1;
        if (id_ < 0) id_ = op_prof_intern("ter.prepass");
        op_prof_begin(id_);
    }
    for (int vi = 0; vi < nvis; vi++) {
        uint32_t i = visidx[vi];
        if (tt_on) {
            uint64_t tnow = openpete_phase_now_us();
            if (tt_prev_valid && tt_prev_i < 1024u) {
                uint32_t d = (uint32_t)(tnow - tt_prev);
                tt_us[tt_prev_i] += d; tt_present_us += d;
                tt_tris[tt_prev_i] += (uint32_t)(s_wctx[0].tri_n - tt_prev_tris);
            }
            tt_prev = tnow; tt_prev_i = i; tt_prev_tris = s_wctx[0].tri_n; tt_prev_valid = 1;
        }
        s_pre_SC[vi] = NULL;
        uint32_t s = rd(cpu, sectorPtr + i * 4);
        s_pre_sec[vi] = s;
        if (!s) continue;
        /* Tier-1 sector decode cache: static level data unpacked once. */
        static int s_hidet = -2;
        if (s_hidet == -2) { const char* e = getenv("OPENPETE_TERRAIN_HIDETAIL"); s_hidet = e ? atoi(e) : -1; }
        const tscache_t* SC = tsc_get(cpu, s, lqTable, hqTable, texTblMode,
                                      s_hidet >= 0 ? s_hidet : 0,
                                      sectorPtr, sectorCnt);
        tsc_refresh_anim(cpu, (tscache_t*)SC, lqTable, hqTable, texTblMode,
                         s_hidet >= 0 ? s_hidet : 0);

        /* Sector occlusion cull - the extended-LOD win. A sector whose entire
         * screen bbox sits BEHIND nearer opaque terrain paints zero pixels: the
         * GPU depth-tests it away, but the CPU still paid to extract, sort and
         * upload it. Under --lod extended the game's distance cull is defeated,
         * so fully-occluded distant sectors (everything behind a wall) still get
         * walked. Reuse the moby occlusion grid (built from the WORLD-opaque
         * terrain depth pre-pass, frames-lagged like the moby query): cull a
         * sector only when EVERY grid cell over its (inflated) bbox is nearer
         * than the sector's NEAREST point - partial occlusion keeps it, so no
         * pop-in. openpete_native_occl_query returns 0 (keep) whenever the grid
         * is absent/stale/too-near, or was rendered from a camera outside the
         * bound of this walk's, so this is a pure conservative skip.
         * Display-only: the canonical PSX render that writes gameplay channels
         * is a separate pass, untouched (and the native feed is inert headless,
         * so the oracle/determinism gates are unaffected). The drawn image is
         * identical with the cull off. OPENPETE_TERRAIN_OCCL=0 disables (A/B). */
        static tn_envvbl_t tocc_c;
        int s_tocc = tn_env_vbl(&tocc_c, "OPENPETE_TERRAIN_OCCL", 1, 1);
        /* The fill-tag inspector must see every sector the frustum keeps: a
         * culled sector's faces never project, so they would be silently
         * unpickable. The cull is a conservative perf skip, so dropping it
         * costs only frame rate. */
        if (s_fp_on) s_tocc = 0;
        /* View-space bounding sphere - consumed by the occlusion cull below
         * and stamped for the portal-rim sector broadphase (rim_sector_cands),
         * which needs it for every walked sector, culled or not (a culled
         * sector emits nothing, so its candidacy is moot, but the stamp must
         * cover every sector that CAN emit this walk). */
        double oc1, oc2, oc3;
        ngte_mac(M, camy - SC->centreY, camz - SC->centreZ,
                 SC->centreX - camx, &oc1, &oc2, &oc3);
        if (i < 1024u) {
            s_secv[i][0] = (float)oc1; s_secv[i][1] = (float)oc2;
            s_secv[i][2] = (float)oc3; s_secv[i][3] = (float)SC->radius;
            s_secv_anim[i] = (uint8_t)(SC->polyanim != 0);
            s_secv_stamp[i] = s_vanim_pres;
        }
        if (s_tocc) {
            int culled = tocc_sector_verdict(i, oc1, oc2, oc3, (double)SC->radius);
            if (!s_vanim_sub && i < 1024u) {
                s_tocc_latch[i] = (uint8_t)culled;
                s_tocc_latch_stamp[i] = s_tocc_tick;
            }
            if (culled) {
                s_tocc_culled++;
                continue;   /* off screen or behind nearer terrain: skip extraction */
            }
        }

        /* Pack-aware LP grade pre-warm: the texture-pack resolve pipeline is
         * not audited for concurrent callers, so the lazy per-sector fit is
         * computed here, serially; the walk only reads SC->lpg_*. */
        if (s_lpg_active) tsc_lp_grade((tscache_t*)SC);
        /* Sector-level tier/fill verdicts - one computation site, consumed
         * by both the retained-GPU claim below and the face walk. */
        sector_verdicts(SC, &G, &s_pre_verd[vi]);
        /* Level-mean accumulator for the pack LP grade: every visible fitted
         * sector whose LP tier draws contributes - the same set the walk's
         * pass-0 entry used to accumulate, now gathered here (serially) so
         * retained sectors contribute without walking. */
        if (s_lpg_active && s_pre_verd[vi].drawLP && SC->lpg_state == 1) {
            for (int c = 0; c < 3; c++) {
                s_lpg_acc[c]     += SC->lpg_p[c];
                s_lpg_acc[3 + c] += SC->lpg_q[c];
            }
            s_lpg_acc_n++;
        }
        /* ---- retained-GPU claim (main thread: chunk builds write RHI
         * buffers, so they stay on the owning thread). An unclaimed
         * sector's chunks are validated or rebuilt here; the walk then
         * publishes its verdict flags and
         * skips the face loop. Claim reasons, in test order:
         *  - near: the bounding sphere may reach the near plane; the CPU
         *    walk near-clips, the GPU path guarantees m3 > 0 instead.
         *  - portal: a portal is recent and either the planes are gone
         *    (claim everything) or the rim frames are unavailable and the
         *    sector's projected bbox meets an opening's outline. With the
         *    frames derived the vertex stage runs the rim cull and the
         *    blended passes' fragment stage the transparent cover, so
         *    nothing claims. Opaque plane-cover needs no claim (per-pixel
         *    in core).
         *  - (the pack-aware LP regrade does not claim: the bake carries
         *    raw colours and the per-sector grade affine rides the sector
         *    params - lp_grade_apply is mirrored in the vertex stage under
         *    OPTC_SF_LPGRADE, own fit and level mean alike.)
         *  - build claims (scroll tiles, arena-full, unmapped colours,
         *    pack-replaced faces, selector-matched faces) come back from
         *    tgpu_ensure. */
        s_tgpu_own[vi] = 0;
        if (tgpu_on) {
            int claim = 0, want_near = 0;
            const double nearBound = s_tgpu_l3 * ((double)SC->radius + 16.0);
            if (oc3 + nearBound < 16.0) {
                /* NO-DRAW verdict: the whole bounding sphere sits behind the
                 * near threshold, so the CPU walk near-culls every face and
                 * emits nothing - owning the sector WITHOUT publishing (or
                 * validating chunks) reproduces that exactly, with zero CPU
                 * walk cost. Genuine near-CROSSERS (sphere straddling the
                 * threshold) still claim below: they need the walk's
                 * near-clip and near-band HQ semantics. */
                s_tgpu_own[vi] = 2;
                s_tgcl_nodraw++;
            } else if (tgpu_near_reach(SC, M, camx, camy, camz, oc3)) {
                /* Sphere-crossing sector: refined by the vertex AABB. The
                 * sphere bound (centre - l3*(radius+16)) inflates in every
                 * direction, so ~1/5 of the sectors it flags provably keep
                 * every VERTEX at or beyond the same 16.0 view-Z the sphere
                 * test guards - the exact regime every owned sector already
                 * renders in - and stay on the GPU. Sectors with real
                 * near-band/clip/straddle vertices (the bulk: the ground the
                 * camera stands on straddles the eye plane) still claim.
                 * Poly-anim sectors skip the refinement: their live verts
                 * ride the anim banks, not the decoded AABB.
                 *
                 * What survives the refinement takes the NEAR VARIANT: chunks
                 * whose vertex stage re-runs the walk's near-plane clip fan
                 * and its straddle waivers, so the sector still renders from
                 * the GPU. Poly-anim sectors keep the claim (no anim x near
                 * twin), as does the lever-off A/B. */
                if (!SC->polyanim && !tsc_canim(SC) && tgpu_nearvar_on()) {
                    want_near = 1;
                } else {
                    claim = 1; s_tgcl_near++;
                    if (oc3 + nearBound < 16.0)
                        s_tgcl_nearb++;   /* census: fully-behind sub-bucket */
                }
            } else if (s_tgpu_claim_all) {
                claim = 1; s_tgcl_portal++;
            } else if (s_tgpu_pplane_n && oc3 > 1.0) {
                /* Portal claim, refined (see the s_tgpu_pplane comment). With
                 * the rim frames derived, NOTHING portal-side claims: the
                 * inner-rim cull runs in the vertex stage on the emitted tri
                 * rebuilt from the corner attributes (portal_rim_classify
                 * mirror), and the blended passes' fragment stage discards
                 * what a portal preview covers (the cover planes ride the
                 * build's globals). */
                int pcl = 0;
                if (!s_tgpu_rim_ok) {
                    /* Refinement frames unavailable (plane provider disabled):
                     * blanket projected-bbox claim, the pre-refinement rule. */
                    const double csx = s_ofx + oc1 * s_H / oc3;
                    const double csy = s_ofy + oc2 * s_H / oc3;
                    const double rpx = (double)SC->radius * s_H / oc3 * 1.5 + 16.0;
                    for (int pi = 0; pi < s_tgpu_pplane_n; pi++) {
                        const openpete_portal_skyplane* pp = &s_tgpu_pplane[pi];
                        if (csx + rpx >= pp->x0 && csx - rpx <= pp->x1 &&
                            csy + rpx >= pp->y0 && csy - rpx <= pp->y1) {
                            pcl = 1; break;
                        }
                    }
                }
                if (pcl) { claim = 1; s_tgcl_portal++; }
            }
            if (!claim && s_tgpu_own[vi] != 2) {
                /* own == 2 (behind-camera no-draw) skips the validate/build
                 * entirely - publishing such a sector DRAWN would hand the
                 * shader mirror behind-camera vertices (negative W), whose
                 * wrapped rasterization poisons the depth prepass. That
                 * guarantee is the near-claim's whole job; no-draw keeps it
                 * by never entering the drawn set. */
                int r = tgpu_ensure(cpu, SC, hqTable, (int)i, want_near);
                if (r == 0) {
                    s_tgpu_own[vi] = want_near ? 3 : 1;
                    if (want_near) s_tgpu_nvsecs++;
                    s_tgpu_secs++; s_tgpu_secs_walk++;
                    tgpu_canim_census(SC, s_cen);
                } else {
                    claim = 1;
                    if (r == 2) s_tgcl_scroll++;
                    else if (r == 3) s_tgcl_colour++;
                    else if (r == 4) s_tgcl_slot++;
                    else if (r == 5) s_tgcl_texpack++;
                    else s_tgcl_build++;
                }
            }
            if (claim) { s_tgpu_claims++; s_tgpu_claims_walk++; }
        }
        s_pre_SC[vi] = SC;
        if (!s_tgpu_own[vi]) walk_faces += SC->nLpF + SC->nHpF;
        if (wserial) walk_sector(&s_wctx[0], &G, vi);
    }
    if (!wserial) {
        /* Decode-cache eviction hazard: the prepass resolves EVERY visible
         * sector's slot before any walk, so two visible sectors colliding in
         * one probe window leave the earlier SC pointer describing the WRONG
         * sector (the serial walk re-resolves per sector and never sees
         * this). Validate every resolved slot still owns its sector; any
         * mismatch falls the whole present back to the serial fused walk,
         * whose per-sector re-resolve is the legacy (thrash-but-correct)
         * behaviour. Rare: needs 9+ visible sectors in one 8-way window
         * (dense sector layouts). */
        int tsc_evicted = 0;
        for (int vi = 0; vi < nvis; vi++)
            if (s_pre_SC[vi] && s_pre_SC[vi]->s != s_pre_sec[vi]) { tsc_evicted = 1; break; }
        if (tsc_evicted) {
            static unsigned s_ev_last; static int s_ev_n;
            if (openpete_frame >= s_ev_last + 600u) {   /* ~10s throttle */
                fprintf(stderr, "[terrain-walk] tsc eviction conflict (nvis=%d) x%d"
                        " -- serial fallback\n", nvis, s_ev_n + 1);
                s_ev_last = openpete_frame; s_ev_n = 0;
            } else s_ev_n++;
            if (op_prof_on()) op_prof_end();   /* ter.prepass */
            wserial = 1;
            goto tn_retry_serial;
        }
        if (op_prof_on()) {
            op_prof_end();   /* ter.prepass */
            static int id_ = -1;
            if (id_ < 0) id_ = op_prof_intern("ter.walk");
            op_prof_begin(id_);
        }
        /* Fan out only when the walk is big enough to repay the dispatch.
         * Waking the helpers and joining them costs ~20us whatever the run
         * holds, and the pool's own item stream is the same work in the same
         * order either way, so running the jobs inline below the threshold is
         * bit-identical and strictly cheaper. Break-even is where the ~7/8 of
         * the walk the helpers absorb equals that floor: at the measured
         * 0.032-0.056us per walked face the crossover sits between ~410 and
         * ~720 faces depending on the scene, so the constant sits under the
         * whole range. Erring low is the cheap direction - fanning out 400
         * faces too eagerly wastes ~20us, while serializing a cache-cold
         * 10k-face vista would cost ~300. With the retained-GPU cache warm
         * walk_faces is 0 even on a whole-level vista (every sector is owned
         * and returns before touching a face), which is the case this gate
         * exists for. */
        /* Calibration override: re-derive the bound on other hardware without
         * a rebuild (sweep it against the ter.walk / ter.faces census). */
        static int s_fanmin = -1;
        if (s_fanmin < 0) { const char* e = getenv("OPENPETE_TERRAIN_FANOUT_MIN");
                            s_fanmin = (e && *e) ? atoi(e) : TN_FANOUT_MIN_FACES; }
        if (walk_faces < s_fanmin)
            for (int vi = 0; vi < nvis; vi++) walk_sector_job(vi, 0, &G);
        else
            openpete_workpool_run(nvis, walk_sector_job, &G);
        if (op_prof_on()) {
            op_prof_end();   /* ter.walk */
            static int idf_ = -1;
            if (idf_ < 0) idf_ = op_prof_intern("ter.faces");
            op_prof_count(idf_, (uint64_t)walk_faces);
            /* 1 when this walk fanned out, 0 when it ran inline: the census
             * average is the fraction of walks above the threshold. */
            static int idp_ = -1;
            if (idp_ < 0) idp_ = op_prof_intern("ter.par");
            op_prof_count(idp_, walk_faces < s_fanmin ? 0u : 1u);
            static int id_ = -1;
            if (id_ < 0) id_ = op_prof_intern("ter.merge");
            op_prof_begin(id_);
        }
        /* Ordered merge: worker runs land in s_tri in sector order, so the
         * stream (and every downstream consumer) matches the serial walk. */
        for (int vi = 0; vi < nvis; vi++) {
            int n = s_pre_rec[vi].count;
            if (n <= 0) continue;
            if (mod_fvec_reserve_noted(&s_tri_v, (size_t)(s_tri_n + n)) != 0)
                n = (int)s_tri_v.cap - s_tri_n;   /* merge what fits (logged by the vec) */
            if (n <= 0) break;
            memcpy(&s_tri[s_tri_n],
                   (const ttri*)s_wbuf[s_pre_rec[vi].worker].data + s_pre_rec[vi].start,
                   (size_t)n * sizeof(ttri));
            s_tri_n += n;
        }
        if (op_prof_on()) op_prof_end();   /* ter.merge */
    } else {
        s_tri_n = s_wctx[0].tri_n;
    }
    for (int w = 0; w < nw_used; w++) {   /* census roll-up */
        tn_wctx* W = &s_wctx[w];
        s_dbg_faces += W->dbg_faces; s_dbg_oob += W->dbg_oob;
        s_dbg_nclip += W->dbg_nclip; s_dbg_behind += W->dbg_behind;
        s_dbg_spike += W->dbg_spike; s_dbg_clipped += W->dbg_clipped;
        s_dbg_satback += W->dbg_satback; s_dbg_satemit += W->dbg_satemit;
        s_dbg_spike_tris += W->dbg_spike_tris;
        trisplit_sum(W);
        s_tex_faces += W->tex_faces; s_tex_tris += W->tex_tris;
        s_near_q_agree += W->near_q_agree; s_near_c_only += W->near_c_only;
        s_walk_sectors += W->walk_sectors;
    }
    {   /* OPENPETE_TERRAIN_NEARCEN=1: one line per terrain walk carrying the
         * near/HQ routing counts and the walk's triangle total. Headless runs
         * one walk per tick, so the line is that tick's per-present count. */
        static int s_ncen = -1;
        if (s_ncen < 0) { const char* e = getenv("OPENPETE_TERRAIN_NEARCEN"); s_ncen = (e && *e && *e != '0'); }
        if (s_ncen)
            fprintf(stderr, "[nearcen] frame=%u nearFaces=%d subTris=%d terrainTris=%d sectors=%d "
                    "tri4Faces=%d tri4Subs=%d tri4StraddleCulls=%d tri4MatDiff=%d "
                    "tri2Faces=%d tri2Subs=%d tri2StraddleCulls=%d tri2MatDiff=%d "
                    "cells4Faces=%d cells4Pinned=%d\n",
                    openpete_frame, s_dbg_spike, s_dbg_spike_tris, s_tri_n, s_walk_sectors,
                    s_trisplit_faces[0], s_trisplit_subs[0], s_trisplit_scull[0],
                    s_trisplit_matdiff[0], s_trisplit_faces[1], s_trisplit_subs[1],
                    s_trisplit_scull[1], s_trisplit_matdiff[1], s_cells4_faces[0],
                    s_cells4_faces[1]);
    }
    {   /* The glitch-mark walk ring: the same counters, kept for the last N
         * walks so a walk that produced the wrong geometry can be read back
         * after the fact. s_fardbg_occl0 holds s_tocc_culled as it stood at
         * this walk's entry, so the difference is this walk's own culls. */
        openpete_glitch_walk_t gw;
        gw.ord = 0;
        gw.tick = openpete_frame;
        gw.sectors = s_walk_sectors;
        gw.tris = s_tri_n;
        gw.near_faces = s_dbg_spike;
        gw.near_tris = s_dbg_spike_tris;
        gw.occl_culled = (int32_t)(s_tocc_culled - s_fardbg_occl0);
        gw.sub = s_vanim_sub;
        openpete_glitch_mark_note_walk(&gw);
    }
    if (tgpu_on) {
        openpete_tercache_frame_commit();
        if (s_cen)
            fprintf(stderr, "[tgpu-cen f=%u] sub=%d secs=%d claims=%d "
                    "near=%ld portal=%ld anim=%ld scroll=%ld fill=%ld "
                    "arena=%ld col=%ld slot=%ld tp=%ld nearb=%ld "
                    "nodraw=%ld prim=%ld pcov=%ld canim=%d canim_moved=%d\n",
                    openpete_frame, s_vanim_sub,
                    s_tgpu_secs_walk, s_tgpu_claims_walk,
                    s_tgcl_near - cen0[0], s_tgcl_portal - cen0[1],
                    s_tgcl_anim - cen0[2], s_tgcl_scroll - cen0[3],
                    s_tgcl_fill - cen0[4], s_tgcl_build - cen0[5],
                    s_tgcl_colour - cen0[6], s_tgcl_slot - cen0[7],
                    s_tgcl_texpack - cen0[8],
                    s_tgcl_nearb - cen0[9], s_tgcl_nodraw - cen0[10],
                    s_tgcl_prim - cen0[11], s_tgcl_pcov - cen0[12], s_tgpu_canim_walk, s_tgpu_canim_mv);
        tgpu_census();
        if (op_prof_on()) {   /* the claim-ratchet metric, per present */
            static int id_secs = -1, id_claims = -1;
            if (id_secs < 0)   id_secs   = op_prof_intern("tc.secs");
            if (id_claims < 0) id_claims = op_prof_intern("tc.claims");
            op_prof_count(id_secs,   (uint64_t)s_tgpu_secs_walk);
            op_prof_count(id_claims, (uint64_t)s_tgpu_claims_walk);
        }
    }
    if (tt_on) {   /* close the walk-cost census: last sector + window report */
        uint64_t tnow = openpete_phase_now_us();
        if (tt_prev_valid && tt_prev_i < 1024u) {
            uint32_t d = (uint32_t)(tnow - tt_prev);
            tt_us[tt_prev_i] += d; tt_present_us += d;
            tt_tris[tt_prev_i] += (uint32_t)(s_tri_n - tt_prev_tris);
        }
        tt_presents++; tt_nvis_sum += (uint32_t)nvis;
        if ((uint32_t)nvis > tt_nvis_max) tt_nvis_max = (uint32_t)nvis;
        tt_win_us += tt_present_us;
        if ((uint32_t)tt_present_us > tt_pres_max_us) tt_pres_max_us = (uint32_t)tt_present_us;
        if (tt_report_t == 0) tt_report_t = tnow;
        if (tnow - tt_report_t >= 1000000u && tt_presents > 0) {
            char line[512]; int off = 0;
            off += snprintf(line + off, sizeof(line) - (size_t)off,
                            "[terrain-tt f=%u] presents=%u nvis avg=%u max=%u walk avg=%uus max=%uus tsc hit=%ld miss=%ld | top",
                            openpete_frame, tt_presents, tt_nvis_sum / tt_presents, tt_nvis_max,
                            (uint32_t)(tt_win_us / tt_presents), tt_pres_max_us,
                            s_tsc_hit, s_tsc_miss);
            s_tsc_hit = 0; s_tsc_miss = 0;
            for (int k = 0; k < 6; k++) {           /* top-6 sectors by window µs */
                uint32_t best = 0, bi = 0;
                for (uint32_t j = 0; j < 1024u; j++)
                    if (tt_us[j] > best) { best = tt_us[j]; bi = j; }
                if (!best) break;
                off += snprintf(line + off, sizeof(line) - (size_t)off,
                                " s%u=%uus/%utri", bi, best / tt_presents, tt_tris[bi] / tt_presents);
                tt_us[bi] = 0;
            }
            fprintf(stderr, "%s  (per-present avgs over window)\n", line);
            memset(tt_us, 0, sizeof(tt_us)); memset(tt_tris, 0, sizeof(tt_tris));
            tt_win_us = 0; tt_presents = 0; tt_nvis_sum = 0; tt_nvis_max = 0;
            tt_pres_max_us = 0; tt_report_t = tnow;
        }
    }

    /* ---- LP<->HP gap probe (OPENPETE_TERRAIN_SECTORDUMP) -------------------- */
    if (s_sd_active && s_sd_n > 0) {
        fprintf(stderr, "\n[sectordump] frame=%u  visible=%d  nativeTris=%d  capturedPrims=%d\n",
                openpete_frame, s_sd_n, s_tri_n, s_gte_n);
        fprintf(stderr, "  %-4s %-5s %-3s %5s %5s %5s %5s %5s %6s  %-10s  %-9s  %-21s\n",
                "vi", "flags", "nHP", "nLpV", "nLpF", "nHpV", "nHpF", "tris", "vfront",
                "m3[min..max]", "tier", "screen-bbox");
        for (int j = 0; j < s_sd_n; j++) {
            secrec* r = &s_sd[j];
            const char* tlab = r->noHP == 3 ? "BOTH" : r->noHP == 1 ? "LP" : r->noHP == 2 ? "HP" : "-";
            fprintf(stderr,
                "  %-4d %04X  %-3d %5d %5d %5d %5d %5d %6d  %5.0f..%-5.0f  %-9s  %4.0f,%-4.0f %4.0f,%-4.0f\n",
                r->idx, r->flags, r->noHP, r->nLpV, r->nLpF, r->nHpV, r->nHpF, r->tris, r->vfront,
                r->m3min, r->m3max, tlab,
                r->bx0, r->by0, r->bx1, r->by1);
        }
        /* coverage-gap grid: cells covered by captured (PSX) prims but NOT by any
         * native tri = the cyclorama-bleed band. Grid over the 4:3 screen. */
        enum { GW = 64, GH = 36 };
        static uint8_t cap_cov[GW*GH], nat_cov[GW*GH];
        memset(cap_cov, 0, sizeof(cap_cov)); memset(nat_cov, 0, sizeof(nat_cov));
        const float SW = 512.0f, SH = 256.0f;   /* PSX 4:3 framebuffer extent */
        /* mark native tris - SKIP near-clip giant slivers (any vertex far
         * off-screen). Those don't rasterize across the visible band on the GPU
         * but a raw point-in-tri test would falsely mark them as coverage and
         * hide the real hole. */
        int skipped_giant = 0;
        for (int t = 0; t < s_tri_n; t++) {
            ttri* T = &s_tri[t];
            float mx = 0;
            for (int k = 0; k < 3; k++) {
                float ax = T->sx[k] < 0 ? -T->sx[k] : T->sx[k];
                float ay = T->sy[k] < 0 ? -T->sy[k] : T->sy[k];
                if (ax > mx) mx = ax; if (ay > mx) mx = ay;
            }
            if (mx > 1500.0f) { skipped_giant++; continue; }
            for (int gy = 0; gy < GH; gy++) for (int gx = 0; gx < GW; gx++) {
                float px = (gx + 0.5f) * SW / GW, py = (gy + 0.5f) * SH / GH;
                if (pt_in_tri(px, py, T->sx[0],T->sy[0], T->sx[1],T->sy[1], T->sx[2],T->sy[2]))
                    nat_cov[gy*GW+gx] = 1;
            }
        }
        fprintf(stderr, "  (skipped %d giant/near-clip tris from native coverage)\n", skipped_giant);
        /* mark captured prims (quad -> two tris) */
        for (int p = 0; p < s_gte_n; p++) {
            prim_t* g = &s_gte[p];
            for (int gy = 0; gy < GH; gy++) for (int gx = 0; gx < GW; gx++) {
                float px = (gx + 0.5f) * SW / GW, py = (gy + 0.5f) * SH / GH;
                int hit = pt_in_tri(px,py, g->x[0],g->y[0], g->x[1],g->y[1], g->x[2],g->y[2]);
                if (!hit && g->nv == 4)
                    hit = pt_in_tri(px,py, g->x[1],g->y[1], g->x[2],g->y[2], g->x[3],g->y[3]);
                if (hit) cap_cov[gy*GW+gx] = 1;
            }
        }
        int gap = 0;
        fprintf(stderr, "  gap map ('#'=PSX-only/missing, '.'=native-ok, ' '=neither, 'o'=native-extra):\n");
        for (int gy = 0; gy < GH; gy++) {
            fprintf(stderr, "  ");
            for (int gx = 0; gx < GW; gx++) {
                int c = cap_cov[gy*GW+gx], n = nat_cov[gy*GW+gx];
                char ch = (c && !n) ? '#' : (c && n) ? '.' : (!c && n) ? 'o' : ' ';
                if (c && !n) gap++;
                fputc(ch, stderr);
            }
            fputc('\n', stderr);
        }
        fprintf(stderr, "  gap cells (PSX-covered, native-missing): %d / %d\n", gap, GW*GH);
        /* OPENPETE_TERRAIN_PIXBOX="x0,y0,x1,y1" (PSX screen px): dump every
         * captured .s prim and every native tri whose bbox intersects the box -
         * prim-level forensics for a coverage gap the grid localizes. */
        const char* pb = getenv("OPENPETE_TERRAIN_PIXBOX");
        if (pb && *pb) {
            float bx0, by0, bx1, by1;
            if (sscanf(pb, "%f,%f,%f,%f", &bx0, &by0, &bx1, &by1) == 4) {
                for (int p = 0; p < s_gte_n; p++) {
                    prim_t* g = &s_gte[p];
                    float mnx=1e9f,mny=1e9f,mxx=-1e9f,mxy=-1e9f;
                    for (int k = 0; k < g->nv; k++) {
                        if (g->x[k]<mnx)mnx=g->x[k]; if (g->x[k]>mxx)mxx=g->x[k];
                        if (g->y[k]<mny)mny=g->y[k]; if (g->y[k]>mxy)mxy=g->y[k];
                    }
                    if (mxx < bx0 || mnx > bx1 || mxy < by0 || mny > by1) continue;
                    fprintf(stderr, "  [pixbox-psx] code=%02X nv=%d tex=%d xy=", g->code, g->nv, g->textured);
                    for (int k = 0; k < g->nv; k++) fprintf(stderr, "(%.0f,%.0f)", g->x[k], g->y[k]);
                    fprintf(stderr, " rgb0=%06X\n", g->rgb[0]);
                }
                for (int t = 0; t < s_tri_n; t++) {
                    ttri* T = &s_tri[t];
                    float mnx=1e9f,mny=1e9f,mxx=-1e9f,mxy=-1e9f;
                    for (int k = 0; k < 3; k++) {
                        if (T->sx[k]<mnx)mnx=T->sx[k]; if (T->sx[k]>mxx)mxx=T->sx[k];
                        if (T->sy[k]<mny)mny=T->sy[k]; if (T->sy[k]>mxy)mxy=T->sy[k];
                    }
                    if (mxx < bx0 || mnx > bx1 || mxy < by0 || mny > by1) continue;
                    fprintf(stderr, "  [pixbox-nat] s=%d f=%d tier=%d fill=%d tex=%d blend=%d "
                                    "xy=(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f) vz=%.0f\n",
                            T->sec, T->face, T->tier, T->fill, T->textured, T->blend,
                            T->sx[0],T->sy[0],T->sx[1],T->sy[1],T->sx[2],T->sy[2], T->vz[0]);
                }
            }
        }
    }

    /* Far-band identity dump, captured side: every .s prim (same canonical
     * frame - capture_prims ran in the same hook invocation) + the prim-code
     * histogram. Tier attribution is the prim code itself: 0x30/0x38 = LP
     * (untextured gouraud), 0x34/0x3C = HP (textured gouraud). */
    if (s_fardbg_active) {
        fprintf(stderr, "[fardbg-hdr] frame=%u walk=%d sub=%d alpha=%.4f occl=%ld gridage=%d "
                        "prims=%u sampled=%d fogRefBase=%d cullS2=%d cullRaw=0x%X "
                        "latoff=%d offadm=%d\n",
                openpete_frame, s_fardbg_walk, s_vanim_sub,
                (s_vanim_sub && s_vanim_on) ? s_vanim_alpha : 1.0,
                s_tocc_culled - s_fardbg_occl0, openpete_native_occl_grid_age(),
                s_prim_total, s_gte_n, fogRefBase, cullRefS2,
                (unsigned)tenv_rd(cpu, game_layout.env.cull_dist),
                s_tocc_latoff, s_tocc_offadm);
        for (int c = 0; c < 256; c++)
            if (s_code_hist[c])
                fprintf(stderr, "[fardbg-hist] code=%02X n=%u\n", c, s_code_hist[c]);
        for (int p = 0; p < s_gte_n && !s_fardbg_lean; p++) {
            prim_t* g = &s_gte[p];
            fprintf(stderr, "[fardbg-prim] code=%02X n=%d", g->code, g->nv);
            for (int k = 0; k < g->nv; k++)
                fprintf(stderr, " c%d=%.0f,%.0f,%06X", k, g->x[k], g->y[k], g->rgb[k]);
            fputc('\n', stderr);
        }
        /* The post-clip triangle list = exactly what native rasterizes (the
         * offline face-cover replica got the quad ring order wrong; this is
         * ground truth for native-winner attribution). */
        for (int t2 = 0; t2 < s_tri_n && !s_fardbg_lean; t2++) {
            ttri* T = &s_tri[t2];
            fprintf(stderr, "[fardbg-tri] s=%d f=%d tier=%d fill=%d tex=%d "
                    "v0=%.2f,%.2f,%.1f,%06X v1=%.2f,%.2f,%.1f,%06X v2=%.2f,%.2f,%.1f,%06X\n",
                    T->sec, T->face, T->tier, T->fill, T->textured,
                    T->sx[0], T->sy[0], T->vz[0], T->rgb[0],
                    T->sx[1], T->sy[1], T->vz[1], T->rgb[1],
                    T->sx[2], T->sy[2], T->vz[2], T->rgb[2]);
        }
        s_fardbg_walk++;
        if (s_fardbg_walk >= s_fardbg_budget) s_fardbg_done = 1;
        s_fardbg_active = 0;
    }
    if (s_facemap_active) {
        /* Header last so its presence marks a complete dump. Screen space is
         * the walk's 512-wide/240-line frame (8-line overscan crop):
         * screenshot px = (1.25*sx, 2*(sy-8)) at 640x448. */
        fprintf(stderr, "[facemap-hdr] frame=%u ofx=%.2f ofy=%.2f H=%.0f nolod=%d fogRefBase=%d\n",
                openpete_frame, s_ofx, s_ofy, s_H, openpete_no_lod, fogRefBase);
        s_facemap_done = 1;
        s_facemap_active = 0;
    }
    if (s_fp_on) fp_publish(cpu);
}

/* Feed the built tris to the native scene. Textured faces sample the PSX VRAM
 * mirror via the Tiledef material (modulated by the per-vertex DPCS-fogged
 * colour); untextured faces are gouraud. Colour = native RGB888 + DPCS fog. */
static int s_notex = -1;   /* OPENPETE_TERRAIN_NOTEX: force untextured (debug) */
static int s_tiercolor = -1;  /* OPENPETE_TERRAIN_TIERCOLOR: LP=red, HP=blue */
static int s_tieronly = -2;   /* OPENPETE_TERRAIN_TIERONLY: 1=LP only, 2=HP only */

/* ---- portal sky cover (the plane-cover rule) -------------------------------
 * Inside an opening's footprint the destination preview covers everything at
 * or behind the opening plane − M (openpete_portal_plane_margin, default
 * 256 world). The portal feeder's aperture mask and preview colours test at
 * plane − M, so covered OPAQUE terrain needs no CPU treatment: it draws at
 * TRUE depth and the preview repaints over it per pixel - including the
 * threshold-pad class (a face spanning from just in front of the plane into
 * the throat, depth-coincident with the plane at the base: its coincident
 * and behind portions fall inside M and are covered; only a genuinely-near
 * lip shows, the portal-ground-disocclusion f2440 requirement). Transparent
 * covered terrain keeps a CPU whole-face drop (it blends AFTER the portal
 * sequence, over the preview) - same predicate, applied at the centroid.
 *
 * This replaces the og-OT key mirror (the sky-bucket TERRAIN_KEY fan + the
 * face-insertion comparison): the .s's OT pushes are tier-asymmetric (fan
 * mean+64 slots, LP mean+64+8·bias, HP mean+4·bias), so the og verdict is a
 * rendering artifact wherever tier and distance decouple - with LOD forced
 * off, the .s itself paints HP terrain over its own preview fan. */
static int s_pconform = -1;        /* OPENPETE_TERRAIN_PORTAL_CONFORM=0 disables (A/B) */
static int s_otbias_on = 1;        /* OPENPETE_TERRAIN_OT_BIAS this VBL (shared: the
                                    * emit-site pull and the conform front cap must
                                    * agree, or the cap's pre-offset compensation
                                    * lands at the wrong final depth) */
static openpete_portal_skyplane s_pplane[6];
static int s_pplane_n = 0;

/* Conform one built tri against the active portal skies. Returns 1 and fills
 * ovz[3] (world/16 scale, the ttri unit) when the face is sky-covered. */
static int s_pconform_dbg = -1;   /* OPENPETE_TERRAIN_PORTAL_CONFORM_DBG=x,y probe pixel */
static float s_pcdbg_x, s_pcdbg_y;
/* ---- portal inner-rim detection -------------------------------------------
 * Some arches carry a thin band of terrain lying IN the portal's opening plane.
 * It is coincident with the destination-sky preview, so no depth ordering can
 * separate them - the band must be excluded from the contest rather than
 * out-depthed. (Not every arch has one, so finding zero faces is a valid
 * result, not a failure: tree_tops and gnorc_cove have none from either side,
 * and dream_weavers authors its rims visible from one side only.)
 *
 * Coplanarity ALONE is not the predicate: a portal is a hole in a wall, and
 * that wall is coplanar with the opening by construction - beast_makers has
 * wall faces 20 units off the plane and thousands of units across. What
 * separates them is WHERE they sit, not how big they are: the rim intrudes
 * INTO the opening, the wall surrounds it. So the second term is containment
 * in the opening POLYGON - the cyclorama's own footprint - and not its
 * bounding box, which necessarily swallows the surrounding wall.
 *
 * Every term is measured in the opening plane or perpendicular to it, never on
 * screen. View space is a rigid transform of world space, so those are the
 * world-space answers and do not move as the camera does. A view-Z offset does
 * move: an earlier version classified the same face as rim head-on and as
 * frame at an angle. Screen-space size is unusable for the same reason - the
 * same faces measure 3.6 px and 29.8 px from two cameras. */
/** @brief Returns the largest perpendicular distance of a rim face's farthest vertex from the opening plane, in world units.
 *
 * OPENPETE_PORTAL_RIM_EPS, else 384; negative values clamp to 0; latched on
 * first use. Paired with portal_rim_near, which bounds the nearest vertex:
 * this one alone bounds a face's extent, not where it sits.
 * @return The bound. */
static double portal_rim_eps(void) {
    static double v = -1.0;
    if (v < 0.0) { const char* e = getenv("OPENPETE_PORTAL_RIM_EPS");
                   v = (e && *e) ? atof(e) : 384.0; if (v < 0.0) v = 0.0; }
    return v;
}
/** @brief Returns the largest perpendicular distance of a rim face's nearest vertex from the opening plane, in world units.
 *
 * OPENPETE_PORTAL_RIM_NEAR, else 128; negative values clamp to 0; latched
 * on first use. The rim lies in the plane, so some part of it touches the
 * plane; portal_rim_eps bounds only how far the face's far side strays,
 * which alone admits a small face wholly offset from the plane, such as an
 * arch's front wall face. Without this bound the peace_keepers 25 jamb
 * (sector 180 face 25, 287..377 units in front of the plane, 8 degrees off
 * parallel, dropping inside the opening) is culled and leaves a hole in
 * the arch at pos=(15962,121384,50254) yaw=1420 pitch=228.
 *
 * Over every arch from a near and a far camera, 401 of 421 culled faces sit
 * within 12.5 units of the plane and the other 20 beyond 240 (16 of them on
 * gnorc_gnexus, levelId 61, an arch with no rim). 128 sits an order of
 * magnitude clear of the real rims and matches portal_rim_reach, so it
 * absorbs the same authoring drift perpendicular to the plane that the
 * reach absorbs within it.
 * @return The bound. */
static double portal_rim_near(void) {
    static double v = -1.0;
    if (v < 0.0) { const char* e = getenv("OPENPETE_PORTAL_RIM_NEAR");
                   v = (e && *e) ? atof(e) : 128.0; if (v < 0.0) v = 0.0; }
    return v;
}
/** @brief Returns the cosine of the largest angle between a rim face and the opening plane.
 *
 * OPENPETE_PORTAL_RIM_ANGLE in degrees, clamped to [0, 90], else 30;
 * latched on first use. Returned as the cosine so the test is a plain
 * dot-product compare. 60 recovers a stone_hill band but admits faces on
 * beast_makers (levelId 43), which has no rim; 45 recovers nothing.
 * @return The cosine bound. */
static double portal_rim_cos(void) {
    static double v = -1.0;
    if (v < 0.0) {
        const char* e = getenv("OPENPETE_PORTAL_RIM_ANGLE");
        double deg = (e && *e) ? atof(e) : 30.0;
        if (deg < 0.0) deg = 0.0;
        if (deg > 90.0) deg = 90.0;
        v = cos(deg * 3.14159265358979323846 / 180.0);
    }
    return v;
}
/** @brief Returns how far outside the opening polygon a face may reach and still count as rim, in world units, measured in the plane.
 *
 * OPENPETE_PORTAL_RIM_REACH, else 128; negative values clamp to 0; latched
 * on first use. A portal whose polygon is authored inset from the visible
 * stone carries its rim outside it entirely: at a pointed top the polygon's
 * straight edges cut inside the real peak, so those faces have no vertex
 * within the outline and no tolerance on distance or angle recovers them.
 * The rim is a band adjacent to the opening boundary on either side.
 *
 * The distance is measured in the opening plane because a screen-space
 * reach depends on the camera: on screen it admits about 7k px of non-rim
 * faces on beast_makers (levelId 43) at every value from 32 up. In the
 * plane, levelId 43 stays at zero for 64, 128 and 256 while levelId 44
 * goes from 1432 to 6706 px; 256 over-admits (peace_keepers_23 goes from
 * 5106 to 9201 px). The bound also absorbs up to 128 units of authoring
 * drift, so an opening that is mis-placed by less than that still matches.
 * @return The bound. */
static double portal_rim_reach(void) {
    static double v = -1.0;
    if (v < 0.0) { const char* e = getenv("OPENPETE_PORTAL_RIM_REACH");
                   v = (e && *e) ? atof(e) : 128.0; if (v < 0.0) v = 0.0; }
    return v;
}
/* Classify a tri against the active portals. Returns -1 when the face is not a
 * candidate at all (not over an opening, or nowhere near its plane); otherwise
 * a BITMASK of the terms it fails, so 0 means accepted as inner rim:
 *   bit0 RIMFAIL_CONTAIN  does not intrude into the opening polygon
 *   bit1 RIMFAIL_DIST     does not straddle the plane's band: the nearest
 *                         vertex is further than `near`, or the farthest
 *                         further than `eps`
 *   bit2 RIMFAIL_ANGLE    face not within the angle tolerance of parallel
 * Reporting the exact combination (rather than a yes/no) is what lets a miss be
 * pointed at directly instead of inferred from a pixel count. */
#define RIMFAIL_CONTAIN 1
#define RIMFAIL_DIST    2
#define RIMFAIL_ANGLE   4
#define RIM_CAND_WINDOW 400.0   /* world units either side of the plane */
#define RIM_CAND_MARGIN 600.0   /* world units outside the opening, in-plane */

/* Containment measured IN THE OPENING PLANE rather than on screen.
 *
 * Screen containment tests the face's projection against the polygon's
 * projection, and those two carry different perspective shifts whenever the
 * face is offset from the plane - so a rim band 200 units behind the opening
 * lands inside the projected outline from one angle and outside it from
 * another. Observed directly: stone_hill's band alternated between
 * contain-fail and not-a-candidate as the camera moved, with distance and
 * angle both passing throughout.
 *
 * Instead: build a 2D basis on the plane from the view-space outline, drop
 * both the polygon and the face onto it along the normal, and test there. The
 * verdict then depends only on the geometry. */
typedef struct { double u, v; } rim_uv;
/** @brief Builds a 2D basis in an opening's plane and drops the outline onto it.
 *
 * The origin is the first outline vertex and U its first edge projected into
 * the plane and normalised; W = n x U. Each outline vertex becomes its (U, W)
 * coordinates in poly.
 * @param pp The plane.
 * @param n The unit view-space plane normal.
 * @param O Receives the origin.
 * @param U Receives the first basis axis.
 * @param W Receives the second basis axis.
 * @param poly Receives pp->vn points.
 * @param maxn The capacity of poly.
 * @return 1 on success, 0 when the outline has fewer than 3 or more than
 *         maxn points or its first edge is parallel to the normal.
 */
static int rim_plane_basis(const openpete_portal_skyplane* pp,
                           const double n[3], double O[3], double U[3], double W[3],
                           rim_uv* poly, int maxn) {
    if (pp->vn < 3 || pp->vn > maxn) return 0;
    O[0] = pp->vx[0]; O[1] = pp->vy[0]; O[2] = pp->vz[0];
    double e[3] = { pp->vx[1]-O[0], pp->vy[1]-O[1], pp->vz[1]-O[2] };
    const double d = e[0]*n[0] + e[1]*n[1] + e[2]*n[2];
    e[0] -= d*n[0]; e[1] -= d*n[1]; e[2] -= d*n[2];   /* into the plane */
    const double el = sqrt(e[0]*e[0] + e[1]*e[1] + e[2]*e[2]);
    if (el < 1e-9) return 0;
    U[0] = e[0]/el; U[1] = e[1]/el; U[2] = e[2]/el;
    W[0] = n[1]*U[2] - n[2]*U[1];
    W[1] = n[2]*U[0] - n[0]*U[2];
    W[2] = n[0]*U[1] - n[1]*U[0];
    for (int i = 0; i < pp->vn; i++) {
        const double r[3] = { pp->vx[i]-O[0], pp->vy[i]-O[1], pp->vz[i]-O[2] };
        poly[i].u = r[0]*U[0] + r[1]*U[1] + r[2]*U[2];
        poly[i].v = r[0]*W[0] + r[1]*W[1] + r[2]*W[2];
    }
    return 1;
}
/** @brief Returns the distance from a point to the nearest edge of a polygon.
 * @param poly The polygon's vertices in plane coordinates.
 * @param pn The vertex count.
 * @param u The point's U.
 * @param v The point's V.
 * @return The distance in plane units.
 */
static double rim_uv_edge_dist(const rim_uv* poly, int pn, double u, double v) {
    double best = 1e30;
    for (int i = 0, j = pn - 1; i < pn; j = i++) {
        const double ax = poly[j].u, ay = poly[j].v;
        const double ex = poly[i].u - ax, ey = poly[i].v - ay;
        const double L2 = ex*ex + ey*ey;
        double t = (L2 > 1e-9) ? ((u-ax)*ex + (v-ay)*ey) / L2 : 0.0;
        if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
        const double dx = u - (ax + t*ex), dy = v - (ay + t*ey);
        const double d = sqrt(dx*dx + dy*dy);
        if (d < best) best = d;
    }
    return best;
}
/** @brief Tests a point against a polygon by the even-odd rule.
 * @param poly The polygon's vertices in plane coordinates.
 * @param pn The vertex count.
 * @param u The point's U.
 * @param v The point's V.
 * @return 1 when the point is inside, else 0.
 */
static int rim_uv_contains(const rim_uv* poly, int pn, double u, double v) {
    int in = 0;
    for (int i = 0, j = pn - 1; i < pn; j = i++) {
        if ((poly[i].v > v) != (poly[j].v > v) &&
            u < (poly[j].u - poly[i].u) * (v - poly[i].v) / (poly[j].v - poly[i].v) + poly[i].u)
            in = !in;
    }
    return in;
}
/** @brief Tests a face dropped onto an opening plane against the rim classifier's containment term.
 *
 * The face touches the opening when a vertex or its centroid lies inside the
 * outline (a face can straddle a non-convex opening with its centre in and
 * every corner out, hence both), or a vertex lies within @p reach of an
 * outline edge.
 * @param pv The outline in plane coordinates.
 * @param pn Its vertex count.
 * @param fuv The face's vertices in plane coordinates.
 * @param reach The reach bound (portal_rim_reach).
 * @return 1 when the term passes, else 0. */
static int rim_face_touches(const rim_uv* pv, int pn, const rim_uv fuv[3], double reach) {
    double cu = 0.0, cv = 0.0;
    int touches = 0;
    for (int k = 0; k < 3; k++) {
        cu += fuv[k].u; cv += fuv[k].v;
        if (!touches && rim_uv_contains(pv, pn, fuv[k].u, fuv[k].v)) touches = 1;
    }
    if (touches || rim_uv_contains(pv, pn, cu / 3.0, cv / 3.0)) return 1;
    for (int k = 0; reach > 0.0 && k < 3; k++)
        if (rim_uv_edge_dist(pv, pn, fuv[k].u, fuv[k].v) <= reach) return 1;
    return 0;
}
/* Per-present frame of an opening plane - everything portal_rim_classify
 * needs that depends only on the PLANE, not the face under test: the unit
 * view-space normal, the in-plane 2D basis, the outline dropped onto it and
 * that outline's UV bbox. Derived once per feed (rim_frames_derive) instead
 * of per classified triangle: with a portal sky active every WORLD tri runs
 * the classifier, and rebuilding the basis per tri (two sqrts + the outline
 * projection) dominated the whole terrain feed on portal-heavy scenes. */
typedef struct {
    int    valid;
    double n[3];                      /* unit view-space plane normal */
    double O[3], U[3], W[3];          /* in-plane basis (rim_plane_basis) */
    rim_uv pv[16];                    /* outline in (U,W) coords */
    double pulo, puhi, pvlo, pvhi;    /* outline UV bbox */
} rim_frame_t;
static rim_frame_t s_rimfr[6];
static double s_rim_ofx, s_rim_ofy, s_rim_H;

/** @brief Derives each active portal plane's rim frame for this feed.
 *
 * Latches the view's projection centre and effective focal length into
 * s_rim_ofx, s_rim_ofy and s_rim_H. For each plane in s_pplane, recovers the
 * unit view-space normal from the reciprocal-depth fit, builds the in-plane
 * basis with rim_plane_basis (up to 16 outline points) and records the
 * outline's bounding box. A plane with fewer than 3 outline points, a
 * degenerate normal or basis, or an effective focal length under 1 is left
 * invalid.
 * @param cpu Unused.
 */
static void rim_frames_derive(CPUState* cpu) {
    s_rim_ofx = s_view.ofx;
    s_rim_ofy = s_view.ofy;
    s_rim_H   = s_view.H_eff;
    for (int pi = 0; pi < s_pplane_n; pi++) {
        rim_frame_t* rf = &s_rimfr[pi];
        const openpete_portal_skyplane* pp = &s_pplane[pi];
        rf->valid = 0;
        if (s_rim_H < 1.0 || pp->vn < 3) continue;
        /* Unit view-space normal of the opening plane, recovered from the
         * inv-vz fit: the fit is inv = (n.x/(H k))x + (n.y/(H k))y +
         * (n.z - n.x ofx/H - n.y ofy/H)/k, so n is proportional to
         * (a H, b H, c + a ofx + b ofy). */
        double nx = pp->a * s_rim_H, ny = pp->b * s_rim_H;
        double nz = pp->c + pp->a * s_rim_ofx + pp->b * s_rim_ofy;
        double nn = sqrt(nx*nx + ny*ny + nz*nz);
        if (nn < 1e-12) continue;
        rf->n[0] = nx / nn; rf->n[1] = ny / nn; rf->n[2] = nz / nn;
        if (!rim_plane_basis(pp, rf->n, rf->O, rf->U, rf->W, rf->pv, 16)) continue;
        rf->pulo = rf->pv[0].u; rf->puhi = rf->pv[0].u;
        rf->pvlo = rf->pv[0].v; rf->pvhi = rf->pv[0].v;
        for (int i = 1; i < pp->vn; i++) {
            if (rf->pv[i].u < rf->pulo) rf->pulo = rf->pv[i].u;
            if (rf->pv[i].u > rf->puhi) rf->puhi = rf->pv[i].u;
            if (rf->pv[i].v < rf->pvlo) rf->pvlo = rf->pv[i].v;
            if (rf->pv[i].v > rf->pvhi) rf->pvhi = rf->pv[i].v;
        }
        rf->valid = 1;
    }
}

/* Portal-rim sector BROADPHASE - conservative, exact by the subset property.
 *
 * With any portal sky active, every WORLD tri runs portal_rim_classify; on
 * portal-heavy scenes that per-tri work dominated the feed even after the
 * per-plane frame hoist. But the classifier's candidate gates are spatial
 * (within RIM_CAND_WINDOW of the plane, in-plane bbox within
 * RIM_CAND_MARGIN), so a SECTOR whose whole bounding sphere provably fails
 * a gate for every plane cannot contain a rim candidate: every emitted tri
 * (both quad halves, near-band sub-quads, near-clip fragments - all subsets
 * of the sector's geometry) would return "not a candidate", and skipping
 * the classifier for it draws exactly the same picture.
 *
 * Conservative inflation of the sphere: rowmax covers the non-orthonormal
 * projection matrix; the vz/H term covers the classifier reconstructing
 * view verts from (possibly integer-snapped) SCREEN coords, an error that
 * grows with view-Z; poly-anim sectors always classify (animated verts can
 * leave the header sphere).
 *
 * OPENPETE_PORTAL_RIM_BROAD: 1 (default) on, 0 off, 2 = verify - classify
 * every tri live (old behaviour) and report any rim verdict the broadphase
 * would have skipped. */
static uint8_t s_sec_rimcand[1024];
static int s_rimbroad = -1;

/** @brief Returns the broadphase verdict for a sector.
 * @param sec The sector index.
 * @return 1 when the sector may hold rim candidates (also for an index
 *         outside 0..1023), else 0.
 */
static int rim_sec_cand(int sec) {
    return (sec >= 0 && sec < 1024) ? s_sec_rimcand[sec] : 1;
}

/** @brief Tests whether a view-space bounding sphere can contain an inner-rim candidate of any active portal.
 *
 * Called by the feed broadphase (rim_sector_cands) and by
 * tgpu_rim_sphere_cand, the retained-GPU claim's wrapper over the same test.
 * The radius is inflated by
 * 5% plus 64 units, plus about 4 px of screen-snap reconstruction error at
 * the sphere's far view-Z. The sphere is a candidate for a portal when it
 * reaches within RIM_CAND_WINDOW of the opening plane and within
 * RIM_CAND_MARGIN of the opening's in-plane bounding box.
 * @param cx The centre's view-space X, in world units.
 * @param cy The centre's view-space Y, in world units.
 * @param cz The centre's view-space Z, in world units.
 * @param rv The radius in world units, already scaled by the rowmax.
 * @return 1 when some valid portal frame admits the sphere, else 0. */
static int rim_sphere_cand_core(double cx, double cy, double cz, double rv) {
    double slack = rv * 0.05 + 64.0;
    if (s_rim_H >= 1.0) {
        double vzmax = cz + rv; if (vzmax < 16.0) vzmax = 16.0;
        slack += 4.0 * vzmax / s_rim_H;   /* ~4 px of screen-snap reconstruction */
    }
    const double r = rv + slack;
    for (int pi = 0; pi < s_pplane_n; pi++) {
        const rim_frame_t* rf = &s_rimfr[pi];
        if (!rf->valid) continue;
        const double dx = cx - rf->O[0], dy = cy - rf->O[1], dz = cz - rf->O[2];
        const double dperp = fabs(dx*rf->n[0] + dy*rf->n[1] + dz*rf->n[2]);
        if (dperp - r > RIM_CAND_WINDOW) continue;   /* sphere wholly beyond the window */
        const double cu = dx*rf->U[0] + dy*rf->U[1] + dz*rf->U[2];
        const double cv = dx*rf->W[0] + dy*rf->W[1] + dz*rf->W[2];
        if (cu + r < rf->pulo - RIM_CAND_MARGIN || cu - r > rf->puhi + RIM_CAND_MARGIN ||
            cv + r < rf->pvlo - RIM_CAND_MARGIN || cv - r > rf->pvhi + RIM_CAND_MARGIN)
            continue;                                /* sphere wholly outside the in-plane bbox */
        return 1;
    }
    return 0;
}

/** @brief Fills s_sec_rimcand, the per-sector portal-rim broadphase, for this feed.
 *
 * Latches OPENPETE_PORTAL_RIM_BROAD into s_rimbroad on first use. With no
 * active plane the table is left as it was. Otherwise a sector not stamped
 * this walk gets 0, a poly-anim sector gets 1, and any other sector gets
 * rim_sphere_cand_core over its view-space bounding sphere.
 */
static void rim_sector_cands(void) {
    if (s_rimbroad < 0) { const char* e = getenv("OPENPETE_PORTAL_RIM_BROAD");
                          s_rimbroad = (e && *e) ? atoi(e) : 1; }
    if (!s_pplane_n) return;
    for (int i = 0; i < 1024; i++) {
        if (s_secv_stamp[i] != s_vanim_pres) { s_sec_rimcand[i] = 0; continue; }
        if (s_secv_anim[i]) { s_sec_rimcand[i] = 1; continue; }
        s_sec_rimcand[i] = (uint8_t)rim_sphere_cand_core(
            (double)s_secv[i][0] * 16.0, (double)s_secv[i][1] * 16.0,
            (double)s_secv[i][2] * 16.0,
            (double)s_secv[i][3] * 16.0 * s_rim_rowmax);
    }
}

/* ---- retained-GPU portal-claim refinement (walk side) ---------------------- */
/** @brief Derives the rim frames for the walk's portal claim tests from the walk's own plane fetch.
 *
 * Copies s_tgpu_pplane into s_pplane and runs rim_frames_derive. The feed
 * re-derives the frames later in the same extraction with the same camera
 * and plane set, so both sides test identical geometry; the claim must be
 * at least as inclusive as the feed's rim_sec_cand, which it is exactly
 * (same core, same inputs: the claim site feeds the values it stamps into
 * s_secv through the same float round-trip).
 * @param cpu The guest CPU state.
 * @return 1 when the frames were derived; 0 when
 *         OPENPETE_TERRAIN_PORTAL_CONFORM=0 disables the plane provider. */
static int tgpu_portal_frames(CPUState* cpu) {
    if (s_pconform < 0) { const char* e = getenv("OPENPETE_TERRAIN_PORTAL_CONFORM");
                          s_pconform = (e && *e) ? (*e != '0') : 1; }
    if (!s_pconform) return 0;
    memcpy(s_pplane, s_tgpu_pplane,
           sizeof(openpete_portal_skyplane) * (size_t)s_tgpu_pplane_n);
    s_pplane_n = s_tgpu_pplane_n;
    rim_frames_derive(cpu);
    return 1;
}
/** @brief Tests a sector's bounding sphere against the active portal rim frames.
 *
 * Rounds the inputs to float, scales them by 16 into world units (the radius
 * also by s_rim_rowmax) and returns rim_sphere_cand_core of the result.
 * @param oc1 The sphere centre's horizontal view coordinate, in local units
 *        of 16 world units.
 * @param oc2 The sphere centre's vertical view coordinate, likewise.
 * @param oc3 The sphere centre's view depth, likewise.
 * @param radius The sphere radius, likewise.
 * @return 1 when some valid portal frame admits the sphere, else 0. */
static int tgpu_rim_sphere_cand(double oc1, double oc2, double oc3, int radius) {
    return rim_sphere_cand_core(
        (double)(float)oc1 * 16.0, (double)(float)oc2 * 16.0,
        (double)(float)oc3 * 16.0,
        (double)(float)radius * 16.0 * s_rim_rowmax);
}

/** @brief Stages the walk's rim frames and classifier knobs into the retained globals.
 *
 * terrain.vert then runs portal_rim_classify on each emitted triangle,
 * rebuilt from the corner attributes and the flags16 half/sub-triangle bit.
 * At most 6 planes and 16 polygon vertices per plane are staged. rim_n
 * stays 0, so the vertex stage culls nothing, when this walk derived no
 * frames (s_tgpu_rim_ok clear).
 * @param g The retained globals. */
static void tgpu_rim_globals_fill(openpete_tercache_globals* g) {
    if (!s_tgpu_rim_ok) return;
    g->rim_ofx = s_rim_ofx; g->rim_ofy = s_rim_ofy; g->rim_H = s_rim_H;
    g->rim_knobs[0] = portal_rim_eps();   g->rim_knobs[1] = portal_rim_near();
    g->rim_knobs[2] = portal_rim_reach(); g->rim_knobs[3] = portal_rim_cos();
    int n = s_pplane_n > 6 ? 6 : s_pplane_n;
    for (int pi = 0; pi < n; pi++) {
        const rim_frame_t* rf = &s_rimfr[pi];
        const openpete_portal_skyplane* pp = &s_pplane[pi];
        g->rim_abc[pi][0] = pp->a; g->rim_abc[pi][1] = pp->b;
        g->rim_abc[pi][2] = pp->c;
        g->rim_abc[pi][3] = rf->valid ? 1.0 : 0.0;
        if (!rf->valid) continue;
        for (int k = 0; k < 3; k++) {
            g->rim_O[pi][k]   = rf->O[k]; g->rim_U[pi][k] = rf->U[k];
            g->rim_W[pi][k]   = rf->W[k]; g->rim_nrm[pi][k] = rf->n[k];
        }
        g->rim_O[pi][3] = pp->rim_fragment ? 1.0 : 0.0;
        g->rim_nrm[pi][3] = (double)pp->vn;
        g->rim_bbox[pi][0] = rf->pulo; g->rim_bbox[pi][1] = rf->puhi;
        g->rim_bbox[pi][2] = rf->pvlo; g->rim_bbox[pi][3] = rf->pvhi;
        for (int k = 0; k < pp->vn && k < 16; k++) {
            g->rim_pv[pi * 16 + k][0] = rf->pv[k].u;
            g->rim_pv[pi * 16 + k][1] = rf->pv[k].v;
        }
    }
    g->rim_n = n;
}

/** @brief Classifies a projected triangle against the active portal openings as inner-rim geometry.
 *
 * For each valid rim frame the triangle's view-space vertices are rebuilt
 * from its screen positions and view-Z, and the triangle is a candidate when
 * its nearest vertex is within RIM_CAND_WINDOW of the opening plane and its
 * in-plane bounding box overlaps the opening's grown by RIM_CAND_MARGIN. A
 * candidate gets a mask of failed terms: RIMFAIL_CONTAIN when
 * rim_face_touches fails (no vertex, centroid or vertex within the reach
 * bound touches the outline); RIMFAIL_DIST when the farthest vertex is beyond the
 * eps bound or the nearest beyond the near bound; RIMFAIL_ANGLE when the
 * face normal is degenerate or its cosine with the plane normal is below
 * portal_rim_cos. The lowest mask over the planes is returned, stopping at
 * 0. A vertex whose plane reciprocal-depth is not positive skips the plane.
 * @param cpu Unused; the camera constants come from rim_frames_derive.
 * @param t The triangle.
 * @param plane Receives the accepting plane's index on a 0 return; may be NULL.
 * @return -1 when no plane makes it a candidate, else the lowest failure
 *         mask, 0 for an accepted inner rim.
 */
static int portal_rim_classify(CPUState* cpu, const ttri* t, int* plane) {
    (void)cpu;   /* per-present camera constants come from rim_frames_derive */
    if (!s_pplane_n) return -1;
    const double ofx = s_rim_ofx, ofy = s_rim_ofy, H = s_rim_H;
    if (H < 1.0) return -1;
    const double eps = portal_rim_eps(), reach = portal_rim_reach();
    const double near = portal_rim_near();
    int best = -1;
    for (int pi = 0; pi < s_pplane_n; pi++) {
        const openpete_portal_skyplane* pp = &s_pplane[pi];
        const rim_frame_t* rf = &s_rimfr[pi];
        if (!rf->valid) continue;
        const double nx = rf->n[0], ny = rf->n[1], nz = rf->n[2];
        /* perpendicular distances + view-space verts. Along the pixel ray
         * v = (x-ofx, y-ofy, H), a view-Z offset dz from the plane is a
         * perpendicular offset dz * (n.v)/H. */
        double V[3][3], dmin = 1e30, dmax = 0.0; int okz = 1;
        for (int k = 0; k < 3; k++) {
            const double inv = pp->a * t->sx[k] + pp->b * t->sy[k] + pp->c;
            if (inv <= 1e-9) { okz = 0; break; }
            const double vz = t->vz[k] * 16.0;
            V[k][0] = (t->sx[k]-ofx) * vz / H; V[k][1] = (t->sy[k]-ofy) * vz / H; V[k][2] = vz;
            const double dz = vz - 1.0 / inv;
            const double d = fabs(dz * (nx*(t->sx[k]-ofx) + ny*(t->sy[k]-ofy) + nz*H) / H);
            if (d < dmin) dmin = d;
            if (d > dmax) dmax = d;
        }
        if (!okz) continue;
        /* Candidate gate, entirely IN THE PLANE.
         *
         * Every screen-space gate on this path has turned out to be a bug:
         * measuring depth along view-Z, testing containment against the
         * projected polygon, and finally this - a face is near the opening or
         * it is not, and walking around the arch must not change the answer.
         * A face offset from the plane projects with a different perspective
         * shift than the polygon does, so a screen bbox overlap admitted it
         * from some angles and dropped it from others (stone_hill rendered
         * untinted at pos=(97051,49348,8798) yaw=581 pitch=162 and tinted a
         * few degrees away).
         *
         * So: drop the face onto the plane, and require its in-plane bbox to
         * overlap the opening's, grown by RIM_CAND_MARGIN world units. The
         * plane is infinite, so some spatial bound is needed - this one is a
         * property of the geometry rather than of the viewpoint. */
        const double *O = rf->O, *U = rf->U, *W = rf->W;
        const rim_uv* pv = rf->pv;
        if (dmin > RIM_CAND_WINDOW) continue;
        const double pulo = rf->pulo, puhi = rf->puhi;
        const double pvlo = rf->pvlo, pvhi = rf->pvhi;
        rim_uv fuv[3];
        double fulo = 1e30, fuhi = -1e30, fvlo = 1e30, fvhi = -1e30;
        for (int k = 0; k < 3; k++) {
            const double r[3] = { V[k][0]-O[0], V[k][1]-O[1], V[k][2]-O[2] };
            fuv[k].u = r[0]*U[0] + r[1]*U[1] + r[2]*U[2];
            fuv[k].v = r[0]*W[0] + r[1]*W[1] + r[2]*W[2];
            if (fuv[k].u < fulo) fulo = fuv[k].u; if (fuv[k].u > fuhi) fuhi = fuv[k].u;
            if (fuv[k].v < fvlo) fvlo = fuv[k].v; if (fuv[k].v > fvhi) fvhi = fuv[k].v;
        }
        if (fuhi < pulo - RIM_CAND_MARGIN || fulo > puhi + RIM_CAND_MARGIN ||
            fvhi < pvlo - RIM_CAND_MARGIN || fvlo > pvhi + RIM_CAND_MARGIN) continue;
        int flags = 0;
        /* contain: the face dropped onto the opening plane along its normal
         * and tested against the polygon there (rim_face_touches). */
        if (!rim_face_touches(pv, pp->vn, fuv, reach)) flags |= RIMFAIL_CONTAIN;
        /* dist: the face must both REACH the plane (nearest vertex within
         * `near`) and not stray past `eps` on its far side. */
        if (dmax > eps || dmin > near) flags |= RIMFAIL_DIST;
        const double u[3] = { V[1][0]-V[0][0], V[1][1]-V[0][1], V[1][2]-V[0][2] };
        const double w[3] = { V[2][0]-V[0][0], V[2][1]-V[0][1], V[2][2]-V[0][2] };
        double fx = u[1]*w[2]-u[2]*w[1], fy = u[2]*w[0]-u[0]*w[2], fz = u[0]*w[1]-u[1]*w[0];
        const double fl = sqrt(fx*fx + fy*fy + fz*fz);
        if (fl < 1e-9) flags |= RIMFAIL_ANGLE;
        else if (fabs((fx*nx + fy*ny + fz*nz) / fl) < portal_rim_cos()) flags |= RIMFAIL_ANGLE;
        if (best < 0 || flags < best) best = flags;
        if (best == 0) { if (plane) *plane = pi; return 0; }
    }
    return best;
}

/* What the feed does with a triangle under the inner-rim rule. */
enum { RIM_KEEP = 0, RIM_DROP, RIM_FRAGMENT };
/** @brief Returns the feed's inner-rim verdict for a triangle.
 * @param cpu The guest CPU state.
 * @param t The triangle.
 * @return RIM_KEEP when no plane accepts it as inner rim; RIM_DROP when the
 *         accepting plane drops its rim whole; RIM_FRAGMENT when that plane
 *         discards its rim per fragment (rim_fragment). */
static int portal_rim_face(CPUState* cpu, const ttri* t) {
    int pi = 0;
    if (portal_rim_classify(cpu, t, &pi) != 0) return RIM_KEEP;
    return s_pplane[pi].rim_fragment ? RIM_FRAGMENT : RIM_DROP;
}

/* Transparent terrain the preview covers: the preview colours draw
 * depth-test-only after the opaque terrain, and the blended classes draw
 * after the portal sequence, so a blended face behind the opening would
 * composite over the preview. The renderer's cover pipelines discard such
 * fragments (portal_cover_scene.glsl, terrain.frag) at or behind the opening
 * plane - M inside its outline. */
/** @brief Reports whether the transparent-terrain portal cover discard is enabled.
 *
 * OPENPETE_PORTAL_TRANSP_COVER=0 disables it, so blended terrain shows
 * through the portal preview; unset or any other value enables it. Latched
 * on the first call.
 * @return 1 when enabled, else 0. */
static int transp_cover_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_PORTAL_TRANSP_COVER");
                 v = (e && *e) ? (*e != '0') : 1; }
    return v;
}
/** @brief Stages the portal cover planes into the retained globals for the fragment discards.
 *
 * The planes are the same screen-space openings the CPU feed publishes to
 * the renderer; at most 6 planes and 16 outline points per plane are
 * staged. They serve the blended passes' cover discard and the inner-rim
 * discard (terrain.frag). Filled only when the rim frames are up
 * (tgpu_portal_frames copied this build's planes into s_pplane); otherwise
 * the blanket portal claim keeps every overlap sector on the CPU walk. The
 * feed-side lever (OPENPETE_PORTAL_TRANSP_COVER=0) clears cov_blend, which
 * turns the blended discard off on both sides alike.
 * @param g The retained globals. */
static void tgpu_cover_globals_fill(openpete_tercache_globals* g) {
    if (!s_tgpu_rim_ok) return;
    int n = s_pplane_n > 6 ? 6 : s_pplane_n;
    g->cov_blend = transp_cover_on();
    g->cov_margin = (double)terrain_game.portal_plane_margin();
    for (int pi = 0; pi < n; pi++) {
        const openpete_portal_skyplane* pp = &s_pplane[pi];
        g->cov_abc[pi][0] = pp->a;
        g->cov_abc[pi][1] = pp->b;
        g->cov_abc[pi][2] = pp->c;
        g->cov_pn[pi] = pp->pn > 16 ? 16 : pp->pn;
        g->cov_bbox[pi][0] = pp->x0; g->cov_bbox[pi][1] = pp->y0;
        g->cov_bbox[pi][2] = pp->x1; g->cov_bbox[pi][3] = pp->y1;
        for (int k = 0; k < pp->pn && k < 16; k++) {
            g->cov_pxy[pi][k][0] = pp->px[k];
            g->cov_pxy[pi][k][1] = pp->py[k];
        }
    }
    g->cov_n = n;
}

/** @brief Sets the scene feed's key-cell state for a triangle that carries a key table.
 *
 * A triangle of a face the drain inserts finer than it keys each fragment by
 * its own sub-cell: its face-local coordinates and the face's key table ride
 * the scene triangle, sticky until the feed clears them. A triangle of a
 * folded quadrant (its table's fold bit) also sets the face's fold-space
 * corners. A triangle with no table (kc[0] zero) sets nothing.
 * @param t The triangle. */
static void tn_feed_key_cells(const ttri* t) {
    if (!t->kc[0]) return;
    const float fl[6] = { t->fx[0], t->fy[0], t->fx[1], t->fy[1], t->fx[2], t->fy[2] };
    openpete_native_scene_set_key_cells(fl, t->kc);
    const unsigned q = t->kc[0] >> 30, w = q < 2 ? t->kc[1] : t->kc[2];
    if (((t->kc[0] >> 14) & 3u) == 1u && ((w >> ((q & 1u) * 16u + 14u)) & 1u))
        openpete_native_scene_set_key_fold(t->kn);
}

/** @brief Returns the portal inner-rim verdict the feed applies to a triangle.
 *
 * The rim lies in the opening plane, coincident with the destination-sky
 * preview, so no depth ordering separates them; on the original it never
 * shows. A plane that drops its rim removes the face (RIM_DROP); a plane
 * whose rim is discarded per fragment keeps it for the renderer's inner-rim
 * pipelines (RIM_FRAGMENT). OPENPETE_PORTAL_RIM_CULL=0 keeps every face. The
 * sector broadphase skips the classifier for a sector that cannot hold a rim
 * face; OPENPETE_PORTAL_RIM_BROAD=2 classifies every face and logs a rim
 * face in a sector the broadphase rejected.
 * @param cpu The guest CPU state.
 * @param t The triangle.
 * @return RIM_KEEP, RIM_DROP or RIM_FRAGMENT. */
static int tn_rim_verdict(CPUState* cpu, const ttri* t) {
    static int s_cull = -1;
    if (s_cull < 0) { const char* e = getenv("OPENPETE_PORTAL_RIM_CULL");
                      s_cull = (e && *e) ? (*e != '0') : 1; }
    if (!s_cull) return RIM_KEEP;
    if (s_rimbroad == 1 && !rim_sec_cand(t->sec)) return RIM_KEEP;
    const int rim = portal_rim_face(cpu, t);
    if (s_rimbroad == 2 && rim != RIM_KEEP && !rim_sec_cand(t->sec)) {
        static int logged = 0;
        if (logged < 32) { logged++;
            fprintf(stderr, "[rim-broad] VIOLATION f=%u sec=%d face=%d "
                    "(rim face in a sector the broadphase rejected)\n",
                    openpete_frame, t->sec, t->face); }
    }
    return rim;
}

/** @brief Feeds this build's terrain triangles to the native scene.
 *
 * Latches the debug toggles, fetches up to 6 portal opening planes for this
 * present's camera, republishes them for the plane-cover sweep, derives the
 * rim frames and the sector broadphase, and runs the one-shot diagnostics.
 * Each triangle in s_tri then becomes a scene triangle: it is skipped by the
 * tier-only debug filter; it takes its material, its corners (the fill tier
 * dilated when the knob is set), its depth as true view-Z scaled from
 * world/16, its fogged colour or a debug tint, and, under the key resolve,
 * its ordering key in koff. Its tag, fog delta and key cells are set, an
 * accepted portal inner rim is dropped, and it is added to the WORLD or
 * WORLD_FILL group, again to PORTAL_GUARD when that experiment is on. It
 * stores nothing to guest RAM, and the per-feed scene state it sets is
 * cleared before it returns.
 * @param cpu The guest CPU state.
 */
static void terrain_feed_native(CPUState* cpu) {
    OP_PROF_SCOPE("ter.feed");
    openpete_native_scene_mark_terrain_feed();
    {
        static tn_envvbl_t nt_c, tc_c, to_c, ob_c;
        s_notex     = tn_env_vbl(&nt_c, "OPENPETE_TERRAIN_NOTEX", 0, 1);
        s_tiercolor = tn_env_vbl(&tc_c, "OPENPETE_TERRAIN_TIERCOLOR", 0, 1);
        s_tieronly  = tn_env_vbl(&to_c, "OPENPETE_TERRAIN_TIERONLY", 0, 0);
        s_otbias_on = tn_env_vbl(&ob_c, "OPENPETE_TERRAIN_OT_BIAS", 1, 1);
    }
    if (s_pconform < 0) { const char* e = getenv("OPENPETE_TERRAIN_PORTAL_CONFORM"); s_pconform = (e && *e) ? (*e != '0') : 1; }
    if (s_pconform_dbg < 0) {
        const char* e = getenv("OPENPETE_TERRAIN_PORTAL_CONFORM_DBG");
        s_pconform_dbg = 0;
        if (e && *e && sscanf(e, "%f,%f", &s_pcdbg_x, &s_pcdbg_y) == 2) s_pconform_dbg = 1;
    }
    /* Active portal-sky opening planes, derived against THIS present's camera
     * bracket (canonical or sub-tick interpolated) so plane and verts agree. */
    s_pplane_n = s_pconform ? terrain_game.portal_sky_planes(cpu, &s_view, s_pplane, 6) : 0;
    /* Republish the openings for the core-side plane-cover invariant sweep
     * (OPENPETE_PORTAL_COVER_CHECK) - core sees the complete scene but not
     * the game-side plane provider. */
    {
        openpete_native_cover_plane cvp[6];
        int cn = 0;
        for (int pi = 0; pi < s_pplane_n && cn < 6; pi++) {
            const openpete_portal_skyplane* pp = &s_pplane[pi];
            if (pp->pn < 3) continue;
            openpete_native_cover_plane* o = &cvp[cn++];
            o->a = pp->a; o->b = pp->b; o->c = pp->c; o->pn = pp->pn;
            for (int k = 0; k < pp->pn; k++) { o->px[k] = pp->px[k]; o->py[k] = pp->py[k]; }
        }
        openpete_native_set_cover_planes(cvp, cn, terrain_game.portal_plane_margin(),
                                         transp_cover_on());
    }
    rim_frames_derive(cpu);
    rim_sector_cands();
    if (s_pconform_dbg > 0 && s_pplane_n > 0) {
        static int lines = 0;
        if (lines < 24) {
            lines++;
            fprintf(stderr, "[pconf f=%u] planes=%d", openpete_frame, s_pplane_n);
            for (int i = 0; i < s_pplane_n; i++)
                fprintf(stderr, " [%d]p=%08X skyot=%.0f back=%.0f bbox=(%.0f,%.0f)-(%.0f,%.0f)",
                        i, s_pplane[i].portal, s_pplane[i].ot_vz, s_pplane[i].back_vz,
                        s_pplane[i].x0, s_pplane[i].y0, s_pplane[i].x1, s_pplane[i].y1);
            fprintf(stderr, "\n");
        }
    }
    { static int s_cap = -1, s_peak = 0;
      ENV_FLAG_LATCH(s_cap, "OPENPETE_SCENE_CAPDBG");
      if (s_cap && s_tri_n > s_peak) { s_peak = s_tri_n;
          fprintf(stderr, "[terraincap] NEW PEAK terrain_tris=%d (capacity %zu)\n",
                  s_tri_n, s_tri_v.cap); } }
    /* OPENPETE_PORTAL_FIT_DBG=<frame>: one-shot dump of each active opening and
     * the terrain around it, expressed in the OPENING PLANE's own 2D basis.
     *
     * This is the measurement behind the per-portal art corrections. An arch's
     * true opening is the hole in the terrain mesh, not the authored outline
     * polygon, and on several arches the two disagree. Both are dumped in the
     * same (u,v) frame so the correction can be FITTED from geometry rather
     * than judged by eye - the outline in plane coords, then every nearby face
     * with its own plane coords and its rim classification, so the surviving
     * (post-cull) geometry can be separated from the rim that is dropped.
     * Units are world units throughout. */
    { static int s_fit = -2; static unsigned s_fit_f = 0;
      if (s_fit == -2) { const char* e = getenv("OPENPETE_PORTAL_FIT_DBG");
                         s_fit = (e && *e) ? 1 : -1; s_fit_f = (e && *e) ? (unsigned)atoi(e) : 0; }
      if (s_fit == 1 && openpete_frame >= s_fit_f && s_pplane_n > 0) {
        s_fit = -1;                                  /* one shot */
        const double ofx = s_view.ofx, ofy = s_view.ofy, H = s_view.H_eff;
        for (int pi = 0; pi < s_pplane_n && H >= 1.0; pi++) {
          const openpete_portal_skyplane* pp = &s_pplane[pi];
          if (pp->vn < 3) continue;
          double nx = pp->a * H, ny = pp->b * H;
          double nz = pp->c + pp->a * ofx + pp->b * ofy;
          double nn = sqrt(nx*nx + ny*ny + nz*nz);
          if (nn < 1e-12) continue;
          nx /= nn; ny /= nn; nz /= nn;
          double O[3], U[3], W[3]; rim_uv pv[16];
          const double nn3[3] = { nx, ny, nz };
          if (!rim_plane_basis(pp, nn3, O, U, W, pv, 16)) continue;
          const int levelId = (int32_t)cpu->read_word(pp->portal + 0x1C);
          /* The CORRECTION basis, expressed in this census's (U,W) frame.
           *
           * portal_outline_world applies dh/dv/sh/sv about world-space in-plane
           * axes (vertical = world +Z projected into the opening plane,
           * horizontal = normal x vertical), while the basis above is built
           * from the outline's own first edge. The two span the same plane but
           * differ by an in-plane rotation, so a fit computed here would apply
           * along the wrong axes. Emitting both axes in (U,W) coordinates lets
           * the fit be converted exactly. */
          double zx, zy, zz;              /* world +Z, in view space */
          ngte_mac(s_view.cam_M, 0.0, -1.0, 0.0, &zx, &zy, &zz);
          { const double zd = zx*nx + zy*ny + zz*nz;    /* project into plane */
            zx -= zd*nx; zy -= zd*ny; zz -= zd*nz; }
          const double zl = sqrt(zx*zx + zy*zy + zz*zz);
          if (zl < 1e-9) continue;                      /* opening lies flat */
          zx /= zl; zy /= zl; zz /= zl;                 /* = code's vertical */
          const double hax[3] = { ny*zz - nz*zy, nz*zx - nx*zz, nx*zy - ny*zx };
          fprintf(stderr, "[pfit] portal=%08X levelId=%d vn=%d "
                  "vax=%.6f,%.6f hax=%.6f,%.6f\n",
                  pp->portal, levelId, pp->vn,
                  zx*U[0] + zy*U[1] + zz*U[2], zx*W[0] + zy*W[1] + zz*W[2],
                  hax[0]*U[0] + hax[1]*U[1] + hax[2]*U[2],
                  hax[0]*W[0] + hax[1]*W[1] + hax[2]*W[2]);
          for (int k = 0; k < pp->vn; k++)
            fprintf(stderr, "[pfit]   out %d %.2f %.2f\n", k, pv[k].u, pv[k].v);
          double pulo = pv[0].u, puhi = pv[0].u, pvlo = pv[0].v, pvhi = pv[0].v;
          for (int i = 1; i < pp->vn; i++) {
            if (pv[i].u < pulo) pulo = pv[i].u; if (pv[i].u > puhi) puhi = pv[i].u;
            if (pv[i].v < pvlo) pvlo = pv[i].v; if (pv[i].v > pvhi) pvhi = pv[i].v;
          }
          /* Search box: the opening grown by its own size, so the arch jamb and
           * the ground meeting the sill are both included without dragging in
           * the whole sector. */
          const double gu = (puhi - pulo), gv = (pvhi - pvlo);
          for (int i = 0; i < s_tri_n; i++) {
            const ttri* t = &s_tri[i];
            double V[3][3]; int okz = 1; double dmin = 1e30, dmax = 0.0;
            for (int k = 0; k < 3; k++) {
              const double inv = pp->a * t->sx[k] + pp->b * t->sy[k] + pp->c;
              if (inv <= 1e-9) { okz = 0; break; }
              const double vz = t->vz[k] * 16.0;
              V[k][0] = (t->sx[k]-ofx) * vz / H; V[k][1] = (t->sy[k]-ofy) * vz / H; V[k][2] = vz;
              const double dz = vz - 1.0 / inv;
              const double d = fabs(dz * (nx*(t->sx[k]-ofx) + ny*(t->sy[k]-ofy) + nz*H) / H);
              if (d < dmin) dmin = d;
              if (d > dmax) dmax = d;
            }
            if (!okz) continue;
            if (dmin > 1024.0) continue;          /* only geometry AT the opening */
            rim_uv f[3];
            double fulo = 1e30, fuhi = -1e30, fvlo = 1e30, fvhi = -1e30;
            for (int k = 0; k < 3; k++) {
              const double r[3] = { V[k][0]-O[0], V[k][1]-O[1], V[k][2]-O[2] };
              f[k].u = r[0]*U[0] + r[1]*U[1] + r[2]*U[2];
              f[k].v = r[0]*W[0] + r[1]*W[1] + r[2]*W[2];
              if (f[k].u < fulo) fulo = f[k].u; if (f[k].u > fuhi) fuhi = f[k].u;
              if (f[k].v < fvlo) fvlo = f[k].v; if (f[k].v > fvhi) fvhi = f[k].v;
            }
            if (fuhi < pulo - gu || fulo > puhi + gu ||
                fvhi < pvlo - gv || fvlo > pvhi + gv) continue;
            fprintf(stderr, "[pfit]   f %d %d %d %.1f %.1f %.2f %.2f %.2f %.2f %.2f %.2f\n",
                    t->sec, t->face, portal_rim_classify(cpu, t, NULL), dmin, dmax,
                    f[0].u, f[0].v, f[1].u, f[1].v, f[2].u, f[2].v);
          }
        }
      } }
    /* OPENPETE_PORTAL_RIM_DBG=<frame>: one-shot census of terrain faces near a
     * portal's opening plane, printing each face's per-term verdict. The rim
     * predicate has three terms and a yes/no highlight cannot say which one
     * rejected a face - that ambiguity is what made earlier misses guesswork.
     * Fires on >= the target frame, never ==: canonical feeds do not land on a
     * fixed parity, so an equality test can miss entirely. */
    { static int s_rim = -2; static unsigned s_rim_f = 0;
      if (s_rim == -2) { const char* e = getenv("OPENPETE_PORTAL_RIM_DBG");
                         s_rim = (e && *e) ? 1 : -1; s_rim_f = (e && *e) ? (unsigned)atoi(e) : 0; }
      if (s_rim == 1 && openpete_frame >= s_rim_f && s_pplane_n > 0) {
        s_rim = -1;                                  /* one shot */
        const double ofx = s_view.ofx, ofy = s_view.ofy, H = s_view.H_eff;
        for (int pi = 0; pi < s_pplane_n; pi++) {
          const openpete_portal_skyplane* pp = &s_pplane[pi];
          double nx = pp->a * H, ny = pp->b * H;
          double nz = pp->c + pp->a * ofx + pp->b * ofy;
          double nn = sqrt(nx*nx + ny*ny + nz*nz);
          if (nn < 1e-12) continue;
          nx /= nn; ny /= nn; nz /= nn;
          fprintf(stderr, "[rim] portal=%08X bbox=(%.0f,%.0f)-(%.0f,%.0f) skyot=%.0f n=(%.3f,%.3f,%.3f)\n",
                  pp->portal, pp->x0, pp->y0, pp->x1, pp->y1, pp->ot_vz, nx, ny, nz);
          for (int i = 0; i < s_tri_n; i++) {
            const ttri* t = &s_tri[i];
            float cx = (t->sx[0]+t->sx[1]+t->sx[2])/3.0f, cy = (t->sy[0]+t->sy[1]+t->sy[2])/3.0f;
            if (cx < pp->x0-8 || cx > pp->x1+8 || cy < pp->y0-8 || cy > pp->y1+8) continue;
            /* PERPENDICULAR distance to the opening plane, in world units:
             * along the pixel ray v = (x-ofx, y-ofy, H), a view-Z offset dz
             * from the plane is a perpendicular offset dz * (n.v)/H. */
            double d[3]; int ok = 1;
            for (int k = 0; k < 3; k++) {
              double inv = pp->a * t->sx[k] + pp->b * t->sy[k] + pp->c;
              if (inv <= 1e-9) { ok = 0; break; }
              double dz = t->vz[k] * 16.0 - 1.0 / inv;   /* + = behind the plane */
              double vx = t->sx[k] - ofx, vy = t->sy[k] - ofy;
              d[k] = dz * (nx*vx + ny*vy + nz*H) / H;
            }
            if (!ok) continue;
            double amax = fabs(d[0]) > fabs(d[1]) ? fabs(d[0]) : fabs(d[1]);
            if (fabs(d[2]) > amax) amax = fabs(d[2]);
            if (amax > 400.0) continue;              /* only the near-plane band */
            double VV[3][3];
            for (int k = 0; k < 3; k++) {
              const double vz = t->vz[k] * 16.0;
              VV[k][0] = (t->sx[k]-ofx) * vz / H; VV[k][1] = (t->sy[k]-ofy) * vz / H; VV[k][2] = vz;
            }
            const double u0[3] = { VV[1][0]-VV[0][0], VV[1][1]-VV[0][1], VV[1][2]-VV[0][2] };
            const double w0[3] = { VV[2][0]-VV[0][0], VV[2][1]-VV[0][1], VV[2][2]-VV[0][2] };
            double fnx = u0[1]*w0[2]-u0[2]*w0[1], fny = u0[2]*w0[0]-u0[0]*w0[2], fnz = u0[0]*w0[1]-u0[1]*w0[0];
            const double fl = sqrt(fnx*fnx + fny*fny + fnz*fnz);
            double para = 0.0;
            if (fl > 1e-9) para = fabs((fnx*nx + fny*ny + fnz*nz) / fl);
            const double ang = acos(para > 1.0 ? 1.0 : para) * 180.0 / 3.14159265358979323846;
            const int cls = portal_rim_classify(cpu, t, NULL);
            fprintf(stderr, "  sec=%d face=%d tier=%d texid=%d d=(%.1f,%.1f,%.1f) "
                    "|d|max=%.1f ang=%.1fdeg at=(%.1f,%.1f) => %s%s%s%s\n",
                    t->sec, t->face, t->tier, t->texid, d[0], d[1], d[2], amax, ang, cx, cy,
                    cls < 0 ? "not-a-candidate" : cls == 0 ? "ACCEPT" : "REJECT",
                    (cls > 0 && (cls & RIMFAIL_CONTAIN)) ? " contain" : "",
                    (cls > 0 && (cls & RIMFAIL_DIST))    ? " dist" : "",
                    (cls > 0 && (cls & RIMFAIL_ANGLE))   ? " angle" : "");
          }
        }
      } }
    static float s_fill_dilate = -1.0f;  /* OPENPETE_TERRAIN_FILL_DILATE px (0 disables) */
    if (s_fill_dilate < 0.0f) {
        const char* e = getenv("OPENPETE_TERRAIN_FILL_DILATE");
        s_fill_dilate = (e && *e) ? (float)atof(e) : 0.0f;  /* quantize/grow OFF by default (they cause silhouette ghosting/spikes + sub-tick crawl; envs re-enable for experiments) */
    }
    /* Level-scoped selector keys resolve at draw time, which has no cpu -
     * push the id from here, the feed that runs whenever a world renders. */
    openpete_mods_set_level((int)cpu->read_word(game_layout.level_id));
    /* Untextured-band material: one resolve per feed - the whole
     * beyond-LOD gouraud band shares a single per-instance outcome. */
    const int untex_slot = openpete_mods_material_terrain_untex_slot(cpu);
    /* Texture-keyed materials: one resolve per (feed, Tiledef id),
     * memoized - -1 = unresolved. */
    int16_t tex_slot[128];
    memset(tex_slot, 0xFF, sizeof tex_slot);
    /* Terrain self-snaps (push_tri floors under the integer snap; subdivide
     * midpoints carry exact on-edge positions) - the renderer chokepoint
     * must pass these coordinates through untouched. */
    openpete_native_scene_set_presnap(1);
    /* OPENPETE_TERRAIN_PXPROBE=x,y[,f0,f1] (walk space): print every terrain
     * tri covering that point with its identity + depth inputs, for VBLs in
     * [f0,f1] (default 1899-1904). Gives the sector/face/tier/slot attribution
     * behind a pixel, which the completed scene no longer carries. */
    { static int s_pxp = -1; static float ppx, ppy;
      static unsigned pf0 = 1899, pf1 = 1904;
      if (s_pxp < 0) { const char* e = getenv("OPENPETE_TERRAIN_PXPROBE");
          s_pxp = (e && *e && sscanf(e, "%f,%f,%u,%u", &ppx, &ppy, &pf0, &pf1) >= 2) ? 1 : 0; }
      if (s_pxp && openpete_frame >= pf0 && openpete_frame <= pf1) {
        for (int i = 0; i < s_tri_n; i++) {
            const ttri* t = &s_tri[i];
            const float ax = t->sx[0], ay = t->sy[0];
            const float e1x = t->sx[1]-ax, e1y = t->sy[1]-ay;
            const float e2x = t->sx[2]-ax, e2y = t->sy[2]-ay;
            const float det = e1x*e2y - e2x*e1y;
            if (det > -1e-6f && det < 1e-6f) continue;
            const float rx = ppx-ax, ry = ppy-ay;
            const float b1 = (rx*e2y - e2x*ry)/det;
            const float b2 = (e1x*ry - rx*e1y)/det;
            const float b0 = 1.0f - b1 - b2;
            if (b0 < -0.001f || b1 < -0.001f || b2 < -0.001f) continue;
            const float vz = b0*t->vz[0] + b1*t->vz[1] + b2*t->vz[2];
            fprintf(stderr, "[pxprobe f=%u %.0f,%.0f] tri#%d sec=%d face=%d tier=%d "
                    "fill=%d blend=%d slots=%d otkey=%.0f vz16=%.1f (full %.0f, +slots %.0f) "
                    "xy=(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f) vzv=(%.0f,%.0f,%.0f)\n",
                    openpete_frame, (double)ppx, (double)ppy, i, t->sec, t->face,
                    t->tier, t->fill, t->blend, (int)t->otslots, (double)t->otkey,
                    (double)vz, (double)vz*16.0, (double)(vz*16.0 + OPENPETE_OT_SLOT_WORLD*t->otslots),
                    (double)t->sx[0], (double)t->sy[0], (double)t->sx[1], (double)t->sy[1],
                    (double)t->sx[2], (double)t->sy[2],
                    (double)t->vz[0]*16.0, (double)t->vz[1]*16.0, (double)t->vz[2]*16.0);
        }
      } }
    /* OPENPETE_TERRAIN_TRIDUMP=<frame>[,path] - one-shot full-feed dump of
     * every terrain tri with its FACE IDENTITY and depth inputs, on the first
     * feed whose VBL is >= <frame>. The identity-granular sibling of the
     * scene dump: sec/face/tier is what per-pixel winner attribution (og OT
     * raster vs native raster) needs and the scene dump cannot carry. */
    { static int s_td = -2; static unsigned s_td_f = 0; static char s_td_path[512];
      if (s_td == -2) { const char* e = getenv("OPENPETE_TERRAIN_TRIDUMP");
          s_td = -1;
          if (e && *e) {
              const char* c = strchr(e, ',');
              s_td = 1; s_td_f = (unsigned)atoi(e);
              snprintf(s_td_path, sizeof s_td_path, "%s",
                       (c && c[1]) ? c + 1 : "/tmp/terrain-tridump.txt");
          } }
      if (s_td == 1 && openpete_frame >= s_td_f) {
        s_td = -1;
        FILE* fp = fopen(s_td_path, "w");
        if (fp) {
            fprintf(fp, "# frame=%u n=%d\n", openpete_frame, s_tri_n);
            fprintf(fp, "# sec,face,tier,fill,blend,textured,slots,"
                        "x0,y0,vz0,x1,y1,vz1,x2,y2,vz2,otkey,nearband,keygrid,"
                        "fx0,fy0,fx1,fy1,fx2,fy2,kc0..kc7"
                        "  (vz/otkey true world units)\n");
            for (int i = 0; i < s_tri_n; i++) {
                const ttri* t = &s_tri[i];
                fprintf(fp, "%d,%d,%d,%d,%d,%d,%d,"
                            "%.2f,%.2f,%.1f,%.2f,%.2f,%.1f,%.2f,%.2f,%.1f,"
                            "%.1f,%d,%u,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
                            "%u,%u,%u,%u,%u,%u,%u,%u\n",
                        t->sec, t->face, t->tier, t->fill, t->blend, t->textured,
                        (int)t->otslots,
                        (double)t->sx[0], (double)t->sy[0], (double)t->vz[0]*16.0,
                        (double)t->sx[1], (double)t->sy[1], (double)t->vz[1]*16.0,
                        (double)t->sx[2], (double)t->sy[2], (double)t->vz[2]*16.0,
                        (double)t->otkey, (int)t->nearband, (t->kc[0] >> 14) & 3u,
                        (double)t->fx[0], (double)t->fy[0], (double)t->fx[1],
                        (double)t->fy[1], (double)t->fx[2], (double)t->fy[2],
                        t->kc[0], t->kc[1], t->kc[2], t->kc[3], t->kc[4], t->kc[5],
                        t->kc[6], t->kc[7]);
            }
            fclose(fp);
        }
      } }
    /* Fill-tag inspector tint: the listed FILL faces green, the preview's
     * untagged candidates magenta, the face under the crosshair yellow. LP is
     * untextured gouraud, so a flat colour replaces the whole surface and the
     * tag set reads at a glance while the HP scene around it stays normal. */
    openpete_fillpick_hit fp_hit;
    int fp_show = openpete_fillpick_active() && openpete_fillpick_get(&fp_hit);
    if (!fp_show) memset(&fp_hit, 0, sizeof fp_hit);
    int fp_tint = openpete_fillpick_active();

    for (int i = 0; i < s_tri_n; i++) {
        ttri* t = &s_tri[i];
        if (s_tieronly == 1 && !t->tier) continue;   /* LP only */
        if (s_tieronly == 2 &&  t->tier) continue;   /* HP only */
        /* Transparent terrain the preview covers (plane-cover rule). Opaque
         * covered faces need no CPU treatment: they draw at TRUE depth
         * behind the opening plane, and the preview - mask and colours
         * testing at plane − M - repaints over them per pixel. Transparent
         * classes draw AFTER the portal sequence and would blend on top of
         * the preview (a waterfall shining through the arch); every blended
         * class, on both tiers and through the HD replacement pipes too, is
         * discarded per fragment by the renderer's cover twins against the
         * planes published earlier in this feed
         * (openpete_native_set_cover_planes), which keeps exactly the part of
         * a face outside the opening or in front of its plane. */
        openpete_scene_tri tri; memset(&tri, 0, sizeof(tri));
        uint32_t fp_col = 0;
        if (fp_tint) {
            if (fp_show && t->tier && t->sec == fp_hit.sec && t->face == fp_hit.face)
                fp_col = 0xFF00FFFFu;                          /* yellow: under the crosshair */
            else if (t->fptag == 1) fp_col = 0xFF00FF00u;      /* green:   listed FILL tag */
            else if (t->fptag == 2) fp_col = 0xFFFF00FFu;      /* magenta: preview candidate */
        }
        int tex = (!s_flat && !s_notex && !s_tiercolor) ? t->textured : 0;
        if (fp_col) tex = 0;
        /* blend rides for untextured faces too: LP semi-transparent gouraud
         * (face w1 bit 2) has no texture but still routes to the ALPHA/ADD
         * scene groups, mirroring the .s's 0x32/0x3A prim codes. */
        tri.textured = tex; tri.blend = t->blend;
        tri.tpage_x = t->tpx; tri.tpage_y = t->tpy;
        tri.clut_x = t->clx; tri.clut_y = t->cly; tri.bpp = t->bpp;
        /* FILL tris are dilated a sub-pixel outward from their centroid: the
         * PSX integer scanline fill is inclusive where native's float raster +
         * GPU centre-sampling drops sub-pixel rows (the dashed far-water
         * slivers). Stencil==0 confines the growth to genuine holes. */
        float dsx[3], dsy[3];
        for (int k = 0; k < 3; k++) { dsx[k] = t->sx[k]; dsy[k] = t->sy[k]; }
        if (t->fill && s_fill_dilate > 0.0f) {
            /* FILL coords are floor-QUANTIZED to mirror the PSX's truncating
             * GTE (pixbox forensics f2730: the same LP quads sit +0.2..+1.0px
             * below the PSX integer coords with per-row VARIANCE - at the
             * horizon, where rows are ~1px tall, that variance opens the
             * dashed water gaps; floor() reproduces the PSX row edges almost
             * everywhere), then grown by the residual dilate PER AXIS (not
             * radially - these rows are ~80px wide and ~1px tall, so a
             * normalized radial offset is nearly all horizontal and closes
             * nothing) to bridge the occasional remaining ±1 divergence.
             * Stencil==0 confines all growth to genuine holes. */
            /* Vertical growth is DOWN-only: floor() already lifts the top edge
             * by the truncated fraction (~0.5px avg, matching the PSX edge);
             * additionally growing UP pushed the fill's fully-fogged LP-only
             * strips ABOVE the terrain silhouette into the sky (a dark fringe
             * on the peace-keepers cliff line at tick 4888). */
            static float s_fill_up = -1.0f;   /* OPENPETE_TERRAIN_FILL_UP: upward growth px */
            if (s_fill_up < 0.0f) { const char* e = getenv("OPENPETE_TERRAIN_FILL_UP"); s_fill_up = (e && *e) ? (float)atof(e) : 0.7f; }
            /* CONTINUOUS growth (the DEFAULT, not a floor()-quantize):
             * floor() snapped the fill verts to the pixel
             * grid EVERY PRESENT, so under the sub-tick spine the fill fringe
             * crawled/jittered against the smoothly-interpolated canonical
             * terrain (ghosting / spikes at terrain tops, all
             * levels - a temporal artifact, invisible in canonical stills).
             * Same coverage envelope without snapping: shift UP by the mean
             * truncation bias (~0.55px - native float sits +0.2..+1.0px below
             * the PSX ints) and grow per-axis from the centroid.
             * OPENPETE_TERRAIN_FILL_QUANT=1 restores floor() (A/B lever). */
            static int s_fill_quant = -1;
            if (s_fill_quant < 0) { const char* e = getenv("OPENPETE_TERRAIN_FILL_QUANT"); s_fill_quant = e ? (*e && *e != '0') : 1;   /* default floor (canonical fidelity); 0 = continuous (smoother sub-tick) */ }
            float cx = (dsx[0]+dsx[1]+dsx[2]) / 3.0f, cy = (dsy[0]+dsy[1]+dsy[2]) / 3.0f;
            for (int k = 0; k < 3; k++) {
                if (s_fill_quant) {
                    dsx[k] = floorf(dsx[k]) + (dsx[k] > cx ? s_fill_dilate : -s_fill_dilate);
                    dsy[k] = floorf(dsy[k]) + (dsy[k] > cy ? s_fill_dilate : -s_fill_up);
                } else {
                    dsx[k] += (dsx[k] > cx ? s_fill_dilate : -s_fill_dilate);
                    dsy[k] += -0.55f + (dsy[k] > cy ? s_fill_dilate : -s_fill_up);
                }
            }
        }
        for (int k = 0; k < 3; k++) {
            tri.v[k].sx = dsx[k]; tri.v[k].sy = dsy[k];
            /* Terrain view-Z is in world/16 units (cam>>4, verts /16); the
             * actors feed FULL-scale view-Z. scene_viewz expects ONE shared
             * scale, so ×16 to match - else terrain's depth is ~16× too small
             * (always nearest) and draws on top of every actor.
             * LP ×1.03: with the stencil hole-fill carrying
             * every INTER-sector LP backing role, this pull's one remaining
             * consumer is the 0x8000 force-LP set - a sector whose LP is the
             * PRIMARY surface backing a sparse HP detail overlay (stone-hill
             * sector 79: 3 HP vs 12 LP faces). That LP must OCCLUDE geometry
             * behind it, so it cannot ride the additive-only stencil fill; and
             * it is NOT a coplanar tie-break, so the constant hardware
             * bias is the wrong shape - the two meshes diverge in WORLD units,
             * which a distance-PROPORTIONAL pull models correctly. Retained,
             * scoped, no longer scene-magic. (Also applies to no-HP 0x4000
             * LP, where it is inert - nothing shares its surface.) */
            /* (A face-mean depth clamp - pushing a face's near half back to
             * its .s OT-key mean so mobys beat the ground they interpenetrate
             * - was tried here and REMOVED: the flattened depth corrupts the
             * shadow decal's world-height reconstruction (a hard bright wedge
             * where Spyro's drop shadow belongs), and the moby-side one-slot
             * tie pull covers the pedestal cases without touching terrain.) */
            openpete_worldvz wz = openpete_worldvz_scaled((float)t->vz[k], 16.0f);
            /* TRUE stored depth, BOTH tiers. The .s's authored OT-slot
             * offsets (LP += ((w1>>3)&0x1F)*8 + 0x40 slots, HP +=
             * ((w3>>3)&0x7)*4, 128 world/slot) are ordering KEYS, not
             * geometry: they ride tri.koff into the per-pixel key-resolve
             * contest (the HP key pass + the LP fragment discard in
             * quad_key.frag / psx_scene_key.frag) and never enter the depth
             * buffer - og's keys decide WHICH terrain surface owns a pixel;
             * true geometry decides everything against that surface
             * (mobys, Spyro, particles, decals, the occlusion grid). */
            tri.v[k].z  = openpete_native_scene_viewz(wz);
            tri.v[k].pvz = tri.v[k].z;  /* perspective W == the stored view-Z */
            tri.v[k].u = t->u[k]; tri.v[k].v = t->v[k];
            /* Textured: modulate the sampled texel by the fogged colour (the
             * pipeline does tex*colour/128, so feed the full RGB). Untextured:
             * the gouraud colour; s_flat = a debug coverage colour;
             * s_tiercolor = LP red / HP blue / fill green. */
            tri.v[k].rgba = fp_col       ? fp_col
                          : s_tiercolor  ? (t->fill ? 0xFF00FF00u
                                         : t->tier ? 0xFF0000FFu : 0xFFFF0000u)
                          : s_flat       ? 0xFF20C040u
                          : (0xFF000000u | (t->rgb[k] & 0xFFFFFFu));
        }
        /* og OT key channel: koff carries the face's ABSOLUTE og
         * OT key (+0.5 sentinel) - the .s insertion index in world units:
         * corner-mean view-Z + 128*(authored slot bias). Every opaque
         * non-fill face both EMITS its flat key into the key image and
         * CONTESTS it (strict less-by-half-slot, so a face never loses to
         * its own key and og ties fall through to true depth). One rule
         * covers the LP<->HP partition, intra-tier authored order, AND the
         * portal sky contest (the sky fans below emit bucket+64).
         *
         * Near-band faces take one of two shapes, chosen by
         * game_layout.terrain.nearband_ot_keys - which .s insert reaches
         * them.
         *
         * nearband_ot_keys = 1: the classified faces key through the
         * ordinary insert like every other face, so each publishes a
         * positive key on the one shared scale and emits it. That is what
         * puts a near ground face and a moby face in the same 128-world
         * slot, which the slot contest then resolves to the terrain.
         *
         * nearband_ot_keys = 0: the faces reach a queue with no OT index,
         * so they are contest-only (negative koff = |key|, no slot offsets).
         * The OT dump proves near cells ride the same painter as everything
         * else (dark_hollow: the beast face at slot 671 of 694 paints over
         * the wall quads at slots 254-399, and the og framebuffer shows the
         * beast), so an authored-biased moby key must be able to win near
         * ground/walls per-pixel (the embedded pedestal-disc rim, and og's
         * own moby-over-near-wall cases). They never emit: a huge
         * under-camera face's flat-key raster would discard everything
         * behind it, and true depth already wins the viewer-side surface's
         * own pixels. (Adjudication trap: dying mobys death-flash per VBL -
         * judge these cases on surface identity, never colour.)
         *
         * FILL twins stay 0 - stencil-gated to uncovered pixels, they must
         * never self-discard. Transparent terrain CONTESTS but never EMITS
         * (the key-pass pack drops blend != 0): it doesn't occlude, but the
         * og painter still covers it - the cyclorama's sky-bucket keys must
         * beat a beyond-the-sky LP waterfall veil exactly as they beat the
         * opaque terrain next to it. Blended NEAR-BAND faces stay 0: the
         * negative-koff branch carries the moby embed rule, which a veil
         * blending OVER a moby must not inherit. OT_BIAS=0 zeroes the
         * channel: raw true depth (the A/B leg). */
        if (s_otbias_on && !t->fill &&
            openpete_native_key_resolve_on()) {
            static int s_nbc = -1;   /* OPENPETE_TERRAIN_NEARBAND_CONTEST=0 disables the near-band contest-only keys (A/B) */
            if (s_nbc < 0) { const char* e = getenv("OPENPETE_TERRAIN_NEARBAND_CONTEST");
                s_nbc = (e && *e) ? (*e != '0') : 1; }
            double mean = (t->otkey > 0.0f) ? (double)t->otkey
                        : (t->vz[0] + t->vz[1] + t->vz[2]) * 16.0 / 3.0;
            double key = mean + OPENPETE_OT_SLOT_WORLD * (double)t->otslots + 0.5;
            if (!t->nearband)
                tri.koff = (float)key;
            else if (!s_nbc || tri.blend)
                tri.koff = 0.0f;
            else if (game_layout.terrain.nearband_ot_keys)
                tri.koff = (float)key;              /* keys through the ordinary insert */
            else
                tri.koff = (float)-(mean + 0.5);    /* contest-only, never emitted */
        }
        int tid = (tex && t->texid >= 0 && t->texid < 128) ? t->texid : -1;
        if (tid >= 0 && tex_slot[tid] < 0) {
            tex_slot[tid] = (int16_t)openpete_mods_material_terrain_tex_slot(cpu, tid);
            static int s_tsr = -2;
            if (s_tsr == -2) { const char* e2 = getenv("OPENPETE_TEXSEL_DBG");
                               s_tsr = e2 ? atoi(e2) : -1; }
            if (s_tsr == tid && openpete_frame > 2000u) { static int pr2;
                if (pr2++ < 12) fprintf(stderr, "[texsel-resolve f=%u] tid=%d -> slot=%d\n",
                                        openpete_frame, tid, tex_slot[tid]); }
        }
        {   /* OPENPETE_TEXSEL_DBG=<tid>: once per present, count that id's
             * textured tris + missing-texid textured tris + its slot. */
            static int s_tsdbg = -2;
            if (s_tsdbg == -2) { const char* e2 = getenv("OPENPETE_TEXSEL_DBG");
                                 s_tsdbg = e2 ? atoi(e2) : -1; }
            if (s_tsdbg >= 0) {
                static unsigned lf = 0xFFFFFFFFu; static int nt, nm;
                if (openpete_frame != lf) {
                    if (lf != 0xFFFFFFFFu)
                        fprintf(stderr, "[texsel f=%u] tid=%d tris=%d slot=%d "
                                "textured-missing-texid=%d\n", lf, s_tsdbg, nt,
                                tex_slot[s_tsdbg], nm);
                    lf = openpete_frame; nt = 0; nm = 0;
                }
                if (tid == s_tsdbg) nt++;
                if (tex && tid < 0) nm++;
            }
        }
        openpete_native_scene_set_tag(tex
            ? (tid >= 0
                   ? OPENPETE_TAG_TERRAIN_TEX(t->tier, t->env,
                                              t->sec < 0 ? 0 : t->sec,
                                              tid, tex_slot[tid])
                   : OPENPETE_TAG_TERRAIN(t->tier, t->env, t->sec < 0 ? 0 : t->sec))
            : OPENPETE_TAG_TERRAIN_UNTEX(t->tier, t->env,
                                         t->sec < 0 ? 0 : t->sec, untex_slot));
        openpete_native_scene_set_fog_delta(t->fog_delta);
        {   const int rim = tn_rim_verdict(cpu, t);
            if (rim == RIM_DROP) continue;
            openpete_native_scene_set_portal_rim(rim == RIM_FRAGMENT);
        }
        /* OPENPETE_PORTAL_RIM_SHOW=1: paint classified faces one colour per
         * failure COMBINATION, so the picture says which TERM is responsible
         * rather than merely that something was rejected. A yes/no highlight
         * left every miss to inference; this is what made the predicate
         * debuggable. Pair with OPENPETE_PORTAL_RIM_CULL=0 - the cull runs
         * first, so accepted faces are already gone by here - and with
         * --render-paths terrain, which excludes the previews that would
         * otherwise z-fight the very faces under test. */
        {   static int s_show = -1;
            ENV_FLAG_LATCH(s_show, "OPENPETE_PORTAL_RIM_SHOW");
            if (s_show) {
                const int cls = portal_rim_classify(cpu, t, NULL);
                if (cls >= 0) {
                    /* vertex rgba packs R in the low byte:
                     *   RED     accepted (all terms pass)
                     *   BLUE    contain            GREEN   distance
                     *   CYAN    contain+distance   YELLOW  angle
                     *   MAGENTA contain+angle      ORANGE  distance+angle
                     *   WHITE   all three */
                    static const uint32_t k_rim_cols[8] = {
                        0xFF0000FFu, 0xFFFF0000u, 0xFF00FF00u, 0xFFFFFF00u,
                        0xFF00FFFFu, 0xFFFF00FFu, 0xFF0080FFu, 0xFFFFFFFFu,
                    };
                    tri.textured = 0;
                    const uint32_t c = k_rim_cols[cls & 7];
                    for (int k = 0; k < 3; k++) tri.v[k].rgba = c;
                }
            }
        }
        tn_feed_key_cells(t);
        openpete_native_scene_add_tri(&tri, t->fill ? OPENPETE_SCENE_WORLD_FILL
                                                    : OPENPETE_SCENE_WORLD, 0);
        openpete_native_scene_set_tag(0);
        /* PSX mask-bit mirror: re-feed .s-deferred (near-band) tris as the
         * portal guard, drawn after the portal sky (see the category doc).
         * Opaque only (the defer test excludes ABE); gated on a portal having
         * fed sky within the last couple of presents. */
        {   /* OPENPETE_PORTAL_GUARD=1 (EXPERIMENT, default OFF): re-feed
             * .s-deferred tris as the portal guard. Disabled by default: at
             * magic-crafters f3000 the PsyCross reference shows the preview
             * covering MOST deferred faces (only the arch pillar behaves
             * protected), so redrawing all of them regressed the probes by
             * ~+12k strong-diff px - the mask/drain model is incomplete
             * (open question: PsyCross FBO stencil attachment + drain-vs-
             * DrawOTag split order). */
            static int gen = -1;
            ENV_FLAG_LATCH(gen, "OPENPETE_PORTAL_GUARD");
            if (gen && t->guard && tri.blend == 0 && terrain_game.portal_sky_recent())
                openpete_native_scene_add_tri(&tri, OPENPETE_SCENE_PORTAL_GUARD, 0);
        }
        openpete_native_scene_set_key_cells(NULL, NULL);
        openpete_native_scene_set_fog_delta(0);   /* never leak past this feeder */
    }
    openpete_native_scene_set_presnap(0);   /* never leak past this feeder */
    openpete_native_scene_set_portal_rim(0);
    /* The former TERRAIN_KEY aperture fan (the sky bucket's og key rastered
     * into the key image, discarding og-covered opaque terrain per pixel) is
     * RETIRED with the plane-cover rule: opaque terrain behind the opening
     * draws at true depth and the preview repaints over it at plane − M. */
    {   /* guard census (debug, stderr-throttled) */
        static int cen = -1; static int lines;
        ENV_FLAG_LATCH(cen, "OPENPETE_PORTAL_GUARD_DBG");
        if (cen && lines < 40 && openpete_frame >= 2980) {
            int g = 0;
            for (int i = 0; i < s_tri_n; i++) if (s_tri[i].guard) g++;
            lines++;
            fprintf(stderr, "[portal-guard] f=%u tris=%d guard=%d recent=%d defer_n=%d\n",
                    openpete_frame, s_tri_n, g, terrain_game.portal_sky_recent(), s_defer_n);
        }
    }
}

/** @brief Finds the captured textured prim nearest a native triangle's screen centroid.
 *
 * A quad prim is two native triangles, so its own centroid sits half a tile
 * from either triangle's; the quad's two half centroids are candidates too,
 * so a triangle of a subdivided quad pairs with the prim that drew it.
 * @param ncx The native centroid's x.
 * @param ncy The native centroid's y.
 * @param out_d2 Receives the squared distance.
 * @return The prim's index in s_gte, or -1 when no textured prim was
 *         captured. */
static int uvdbg_nn_prim(float ncx, float ncy, float* out_d2) {
    int bj = -1; float bd = 1e18f;
    for (int p = 0; p < s_gte_n; p++) {
        prim_t* g = &s_gte[p]; if (!g->textured) continue;
        float gcx = 0, gcy = 0;
        for (int k = 0; k < g->nv; k++) { gcx += g->x[k]; gcy += g->y[k]; }
        gcx /= g->nv; gcy /= g->nv;
        float dx = ncx - gcx, dy = ncy - gcy, d = dx * dx + dy * dy;
        for (int h = 0; h < (g->nv == 4 ? 2 : 0); h++) {
            float hx = (g->x[h] + g->x[h+1] + g->x[h+2]) / 3.f;
            float hy = (g->y[h] + g->y[h+1] + g->y[h+2]) / 3.f;
            float hdx = ncx - hx, hdy = ncy - hy, hd2 = hdx * hdx + hdy * hdy;
            if (hd2 < d) d = hd2;
        }
        if (d < bd) { bd = d; bj = p; }
    }
    *out_d2 = bd; return bj;
}

/** @brief Compares a native triangle's corner UVs with the captured prim's at the same screen positions.
 *
 * Each native vertex is paired with the prim vertex nearest it and must carry
 * the same rounded UV. A rotated or mirrored tile carries the same four UV
 * pairs as the identity, permuted between the corners, so only the
 * per-corner pairing sees an orientation divergence; the page, bpp and CLUT
 * all agree across it. A corner with no prim vertex within 3 px is not
 * scored.
 * @param t The native triangle.
 * @param g The captured prim.
 * @param ok Cleared on a disagreement; left unchanged otherwise.
 * @return The count of corners scored. */
static int uvdbg_corner_uv(const ttri* t, const prim_t* g, int* ok) {
    int scored = 0;
    for (int k = 0; k < 3; k++) {
        int gbest = -1; float gbd = 1e18f;
        for (int gk = 0; gk < g->nv; gk++) {
            float ddx = t->sx[k] - (float)g->x[gk], ddy = t->sy[k] - (float)g->y[gk];
            float dd = ddx * ddx + ddy * ddy;
            if (dd < gbd) { gbd = dd; gbest = gk; }
        }
        if (gbest < 0 || gbd > 9.0f) continue;
        scored++;
        if (g->u[gbest] != (int)lrintf(t->u[k]) || g->v[gbest] != (int)lrintf(t->v[k])) *ok = 0;
    }
    return scored;
}

/** @brief Prints the native-vs-captured A/B report of one verify tick to stderr.
 *
 * Geometry: each native projected vertex is matched to the nearest captured
 * prim vertex, independent of order and topology (the .s emits LP for far
 * sectors, so ordering does not line up; the nearest-neighbour distance
 * measures decode plus projection), with mean, worst, within-1 and within-2
 * px counts and percentiles overall and on screen. It then prints the face
 * walk counts, the near-queue classifier against the game's queues, the
 * calibration line, and a colour A/B over tight geometry matches.
 * OPENPETE_TERRAIN_UVDBG adds the per-triangle UV and tile A/B with its
 * summaries; OPENPETE_TERRAIN_POINT="x,y;..." lists, per screen point, the
 * captured prims and the native triangles covering it. */
static void terrain_compare(void) {
    /* Gather captured prim verts. */
    static float cx[MAX_PRIMS * 4], cy[MAX_PRIMS * 4]; int cn = 0;
    for (int p = 0; p < s_gte_n && cn < MAX_PRIMS * 4 - 4; p++) {
        prim_t* t = &s_gte[p];
        for (int k = 0; k < t->nv; k++) { cx[cn] = t->x[k]; cy[cn] = t->y[k]; cn++; }
    }
    if (!cn || !s_tv_n) {
        fprintf(stderr, "[terrain-ab] native=%d captured=%d (no compare)\n", s_tv_n, cn);
        return;
    }
    static float nn[MAX_TVERTS]; int nnn = 0;
    static float nn_on[MAX_TVERTS]; int nnon = 0;   /* on-screen subset */
    double sum = 0; float worst = 0; int within1 = 0, within2 = 0;
    for (int i = 0; i < s_tv_n; i++) {
        float best = 1e9f;
        for (int j = 0; j < cn; j++) {
            float dx = s_tvx[i] - cx[j], dy = s_tvy[i] - cy[j];
            float d = dx * dx + dy * dy;
            if (d < best) best = d;
        }
        best = (float)sqrt((double)best);
        sum += best; if (best > worst) worst = best;
        if (best <= 1.0f) within1++;
        if (best <= 2.0f) within2++;
        nn[nnn++] = best;
        if (s_tvx[i] >= -16.f && s_tvx[i] <= 528.f && s_tvy[i] >= -16.f && s_tvy[i] <= 272.f)
            nn_on[nnon++] = best;
    }
    /* insertion sort (small N, once per dump) for percentiles */
    for (int a = 1; a < nnn; a++) { float v = nn[a]; int b = a - 1;
        while (b >= 0 && nn[b] > v) { nn[b+1] = nn[b]; b--; } nn[b+1] = v; }
    float med = nn[nnn/2], p90 = nn[(nnn*9)/10];
    /* on-screen median */
    for (int a = 1; a < nnon; a++) { float v = nn_on[a]; int b = a - 1;
        while (b >= 0 && nn_on[b] > v) { nn_on[b+1] = nn_on[b]; b--; } nn_on[b+1] = v; }
    float med_on = nnon ? nn_on[nnon/2] : 0.f;
    fprintf(stderr,
        "[terrain-ab] sectors=%d nativeV=%d tris=%d capturedV=%d  mean=%.2f median=%.2f p90=%.1f worst=%.1f"
        "  <=1px=%.0f%% <=2px=%.0f%%  onscreen(n=%d med=%.2f)\n",
        s_walk_sectors, s_tv_n, s_tri_n, cn, sum / s_tv_n, med, p90, worst,
        100.0 * within1 / s_tv_n, 100.0 * within2 / s_tv_n, nnon, med_on);
    fprintf(stderr, "[terrain-faces] walked=%d oob=%d fullyBehind=%d nearClipped=%d nclipCulled=%d satBack=%d satEmit=%d emitted=%d  texturedFaces=%d texTris=%d hqCells=%d skipKeyed=%d skipMasked=%d foldedQuads=%d\n",
        s_dbg_faces, s_dbg_oob, s_dbg_behind, s_dbg_clipped, s_dbg_nclip, s_dbg_satback, s_dbg_satemit,
        s_tri_n, s_tex_faces, s_tex_tris, s_dbg_spike, s_drain_skip[0], s_drain_skip[1], s_drain_skip[2]);
    fprintf(stderr, "[terrain-defer] queue(oracle): +0x2000{hdr=%d cell=%d} +0x1E00{hdr=%d cell=%d} total=%d  |  classifier: near=%d (×4 subquads -> %d bpp8 prims)  agreeWithQueue=%d classifierOnly=%d queueOnly=%d\n",
        s_defer_hdr2, s_defer_cell2, s_defer_hdr1e, s_defer_cell1e, s_defer_n,
        s_dbg_spike, s_dbg_spike * 4, s_near_q_agree, s_near_c_only, s_defer_n - s_near_q_agree);
    if (s_caldbg)
        fprintf(stderr, "[terrain-cal] queueCells=%d  max(Σm3 over queue)=%.0f  max(minM3 over queue)=%.0f  min(Σm3 over non-queue HP, <4096)=%.0f\n",
            s_cal_qn, s_cal_qsum_max, s_cal_qmin_max, s_cal_nqsum_min);

    /* Colour A/B: NN-match each native colour-vert (post-fog) to the nearest
     * captured prim corner by SCREEN position; where the geometry match is tight
     * (<=2px) the captured prim's RGB is the .s DPCS oracle for that corner, so
     * compare. (Shared verts carry per-corner colours, so a few NN mismatches are
     * expected - read the median + exact-% as the discriminator, not the worst.) */
    build_captured_colors();
    if (s_cc_n && s_cv_n) {
        long matched = 0, exact = 0, near4 = 0; double esum = 0; int eworst = 0;
        /* Near-regime subset: f saturates to 1 (no fog) when 4·m3 <= fogRef-0x1000;
         * with fogRef>=0x2000 that's m3 < 0x400. There the .s also renders HP
         * unfogged -> must match near-exactly. This isolates the decode (array
         * base/byte order) from the LP/HP tier confound + fog rounding. */
        long nmatched = 0, nexact = 0, nnear4 = 0; double nesum = 0; int nworst = 0;
        for (int i = 0; i < s_cv_n; i++) {
            float best = 1e18f; uint32_t crgb = 0;
            for (int j = 0; j < s_cc_n; j++) {
                float dx = s_cvx[i] - s_ccx[j], dy = s_cvy[i] - s_ccy[j], d = dx*dx + dy*dy;
                if (d < best) { best = d; crgb = s_ccrgb[j]; }
            }
            if (best > 4.0f) continue;                  /* geometry too loose to trust */
            uint32_t n = s_cvrgb[i];
            int dr = abs((int)(n & 0xFF)        - (int)(crgb & 0xFF));
            int dg = abs((int)((n >> 8) & 0xFF) - (int)((crgb >> 8) & 0xFF));
            int db = abs((int)((n >> 16) & 0xFF)- (int)((crgb >> 16) & 0xFF));
            int de = dr + dg + db;
            matched++; esum += de; if (de > eworst) eworst = de;
            if (de == 0) exact++;
            if (dr <= 4 && dg <= 4 && db <= 4) near4++;
            if (s_cvz[i] < 1024.0) {                     /* near (unfogged) regime */
                nmatched++; nesum += de; if (de > nworst) nworst = de;
                if (de == 0) nexact++;
                if (dr <= 4 && dg <= 4 && db <= 4) nnear4++;
            }
            if (s_fogdbg) {
                static int pr; if (pr < 6000) { pr++;
                    fprintf(stderr, "[colcmp] xy=(%.0f,%.0f) m3=%.1f native=%06X psx=%06X  dRGB=(%d,%d,%d)\n",
                            s_cvx[i], s_cvy[i], s_cvz[i], n, crgb, dr, dg, db); }
            }
        }
        if (matched)
            fprintf(stderr,
                "[terrain-color] matched=%ld  meanΔ(r+g+b)=%.2f exact=%.0f%% within±4=%.0f%% worstΔ=%d\n",
                matched, esum / matched, 100.0 * exact / matched, 100.0 * near4 / matched, eworst);
        else
            fprintf(stderr, "[terrain-color] no tight geometry matches to score colour\n");
        if (nmatched)
            fprintf(stderr,
                "[terrain-color-near] matched=%ld  meanΔ=%.2f exact=%.0f%% within±4=%.0f%% worstΔ=%d (m3<1024, unfogged HP-in-both)\n",
                nmatched, nesum / nmatched, 100.0 * nexact / nmatched, 100.0 * nnear4 / nmatched, nworst);
    }

    /* UV/tile A/B (OPENPETE_TERRAIN_UVDBG): for each native textured tri, NN-match
     * to the nearest captured TEXTURED prim by screen centroid; where the centroid
     * match is tight, the captured prim's UVs/tpage/CLUT are the .s oracle - print
     * native vs captured so a tile-select / UV-mapping / CLUT-row divergence is
     * visible directly (geometry A/B can't see it). Optional box filter via
     * OPENPETE_TERRAIN_UVBOX="x0,y0,x1,y1" to focus on one feature (e.g. platform). */
    { const char* e = getenv("OPENPETE_TERRAIN_UVDBG");
      if (e && *e && *e != '0') {
        float bx0=-1e9f,by0=-1e9f,bx1=1e9f,by1=1e9f;
        const char* be = getenv("OPENPETE_TERRAIN_UVBOX");
        if (be && *be) sscanf(be, "%f,%f,%f,%f", &bx0, &by0, &bx1, &by1);
        int shown = 0, want = 40;
        long uvmatch=0, uvexact_tp=0, uvexact_cl=0, tight=0, tdis=0, uvdis=0, uvcmp=0;
        double hi_min=1e18,hi_max=0,lo_min=1e18,lo_max=0,hi_sum=0,lo_sum=0; long hin=0,lon=0;
        for (int i = 0; i < s_tri_n; i++) {
            ttri* t = &s_tri[i];
            if (!t->textured) continue;
            float ncx=(t->sx[0]+t->sx[1]+t->sx[2])/3.f, ncy=(t->sy[0]+t->sy[1]+t->sy[2])/3.f;
            if (ncx<bx0||ncx>bx1||ncy<by0||ncy>by1) continue;
            /* nearest captured textured prim by centroid */
            float bd=1e18f; int bj=uvdbg_nn_prim(ncx, ncy, &bd);
            if (bj<0) continue; bd=sqrtf(bd);
            if (bd > 6.0f) continue;                /* loose match - skip */
            prim_t* g=&s_gte[bj];
            /* decode captured material the same way terrain_texmat does */
            int gtpx=(g->tpage&0xF)*64, gtpy=((g->tpage>>4)&1)*256;
            int gclx=(g->clut&0x3F)*16, gcly=(g->clut>>6)&0x1FF;
            int gmode=(g->tpage>>7)&3, gbpp=(gmode==0)?4:(gmode==1)?8:16;
            uvmatch++;
            int tp_ok = (gtpx==t->tpx && gtpy==t->tpy && gbpp==t->bpp);
            int cl_ok = (gclx==t->clx && gcly==t->cly);
            /* A triangle scoring fewer than two corners is not counted. */
            int uv_ok = 1, uv_cmp = uvdbg_corner_uv(t, g, &uv_ok);
            if (tp_ok) uvexact_tp++;
            if (cl_ok) uvexact_cl++;
            /* Only print TIGHT matches (d<2.5px, trustworthy spatial pairing) that
             * actually DISAGREE - those are real per-tile decode divergences, not
             * NN noise from grabbing an adjacent checkerboard tile. */
            int printall = getenv("OPENPETE_TERRAIN_UVALL") != NULL;
            if (((printall && bd < 6.0f) || (bd < 2.5f && (!tp_ok || !cl_ok || (!uv_ok && uv_cmp >= 2)))) && shown < want) { shown++;
                fprintf(stderr,
                  "[uvdbg] c=(%.0f,%.0f) d=%.1f %s%s%s | NAT tp=(%d,%d) bpp=%d cl=(%d,%d) uv=[(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)]\n"
                  "                          | PSX tp=(%d,%d) bpp=%d cl=(%d,%d) uv=",
                  ncx, ncy, bd, tp_ok?"":"TP! ", cl_ok?"":"CL! ", uv_ok?"":"UV! ", t->tpx, t->tpy, t->bpp, t->clx, t->cly,
                  t->u[0],t->v[0],t->u[1],t->v[1],t->u[2],t->v[2],
                  gtpx, gtpy, gbpp, gclx, gcly);
                for (int k=0;k<g->nv;k++) fprintf(stderr,"(%d,%d)",g->u[k],g->v[k]);
                fprintf(stderr, " raw tpage=%04X clut=%04X\n", g->tpage, g->clut);
            }
            if (bd < 2.5f) { tight++; if (!tp_ok || !cl_ok) tdis++;
                if (uv_cmp >= 2) { uvcmp++; if (!uv_ok) uvdis++; }
                /* empirical near/far fit: native mean view-Z (m3) vs the table the
                 * game chose (bpp8 = HQ/near, bpp4 = LQ/far). */
                double mvz = (t->vz[0]+t->vz[1]+t->vz[2])/3.0;
                if (gbpp==8){ if(mvz<hi_min)hi_min=mvz; if(mvz>hi_max)hi_max=mvz; hin++; hi_sum+=mvz; }
                else if (gbpp==4){ if(mvz<lo_min)lo_min=mvz; if(mvz>lo_max)lo_max=mvz; lon++; lo_sum+=mvz; }
            }
        }
        fprintf(stderr,"[nearfit] HQ/bpp8: n=%ld vz[%.0f..%.0f] mean=%.0f | LQ/bpp4: n=%ld vz[%.0f..%.0f] mean=%.0f\n",
            hin,hi_min,hi_max,hin?hi_sum/hin:0.0, lon,lo_min,lo_max,lon?lo_sum/lon:0.0);
        /* bpp histograms: native tris vs captured textured prims */
        { long nb[3]={0,0,0}, cb[3]={0,0,0};
          for(int i2=0;i2<s_tri_n;i2++){ if(!s_tri[i2].textured)continue;
              int b=s_tri[i2].bpp; nb[b==4?0:b==8?1:2]++; }
          for(int p=0;p<s_gte_n;p++){ prim_t* g=&s_gte[p]; if(!g->textured)continue;
              int m=(g->tpage>>7)&3,b=(m==0)?4:(m==1)?8:16; cb[b==4?0:b==8?1:2]++; }
          fprintf(stderr,"[uvdbg-bpphist] NAT 4bpp=%ld 8bpp=%ld 16bpp=%ld | PSX 4bpp=%ld 8bpp=%ld 16bpp=%ld\n",
              nb[0],nb[1],nb[2],cb[0],cb[1],cb[2]); }
        fprintf(stderr, "[uvdbg-summary] matched=%ld  tpage/bpp-exact=%ld (%.0f%%)  clut-exact=%ld (%.0f%%)\n",
            uvmatch, uvexact_tp, uvmatch?100.0*uvexact_tp/uvmatch:0.0,
            uvexact_cl, uvmatch?100.0*uvexact_cl/uvmatch:0.0);
        fprintf(stderr, "[uvdbg-tight] tight(<2.5px)=%ld disagree=%ld (%.0f%%)\n",
            tight, tdis, tight?100.0*tdis/tight:0.0);
        fprintf(stderr, "[uvdbg-uvcorner] scored=%ld uv-mismatch=%ld (%.0f%%)\n",
            uvcmp, uvdis, uvcmp?100.0*uvdis/uvcmp:0.0);
      }
    }

    /* Point probe (OPENPETE_TERRAIN_POINT="x,y;x,y;..."): for each screen point,
     * find the FRONTMOST native tri and the FRONTMOST .s prim covering it
     * (point-in-poly + nearest vz), print both tiles - exact, no NN looseness. */
    { const char* pe = getenv("OPENPETE_TERRAIN_POINT");
      if (pe && *pe) {
        const char* p = pe;
        while (*p) {
            float px, py; if (sscanf(p, "%f,%f", &px, &py) != 2) break;
            /* frontmost native tri (min vz) whose screen tri contains (px,py) */
            int bi=-1; double bvz=1e18;
            for (int i=0;i<s_tri_n;i++){ ttri* t=&s_tri[i];
                float ax=t->sx[0],ay=t->sy[0],bx=t->sx[1],by=t->sy[1],cx=t->sx[2],cy=t->sy[2];
                float d1=(px-bx)*(ay-by)-(ax-bx)*(py-by);
                float d2=(px-cx)*(by-cy)-(bx-cx)*(py-cy);
                float d3=(px-ax)*(cy-ay)-(cx-ax)*(py-ay);
                int neg=(d1<0)||(d2<0)||(d3<0), pos=(d1>0)||(d2>0)||(d3>0);
                if(neg&&pos) continue;                       /* not inside */
                double vz=(t->vz[0]+t->vz[1]+t->vz[2])/3.0;
                if(vz<bvz){bvz=vz;bi=i;} }
            /* ALL .s prims covering the point (draw order; last = painter winner) */
            int gj=-1;
            fprintf(stderr,"          (%.0f,%.0f) PSX covering prims (draw order):\n",px,py);
            for (int q=0;q<s_gte_n;q++){ prim_t* g=&s_gte[q];
                int inside=0;
                int tris[2][3]={{0,1,2},{0,2,3}}; int nt=(g->nv==4)?2:1;
                for(int tt=0;tt<nt&&!inside;tt++){ int*ix=tris[tt];
                    float ax=g->x[ix[0]],ay=g->y[ix[0]],bx=g->x[ix[1]],by=g->y[ix[1]],cx=g->x[ix[2]],cy=g->y[ix[2]];
                    float d1=(px-bx)*(ay-by)-(ax-bx)*(py-by);
                    float d2=(px-cx)*(by-cy)-(bx-cx)*(py-cy);
                    float d3=(px-ax)*(cy-ay)-(cx-ax)*(py-ay);
                    int neg=(d1<0)||(d2<0)||(d3<0), pos=(d1>0)||(d2>0)||(d3>0);
                    if(!(neg&&pos)) inside=1; }
                if(inside){ int gtpx=(g->tpage&0xF)*64, gtpy=((g->tpage>>4)&1)*256;
                    int gclx=(g->clut&0x3F)*16, gcly=(g->clut>>6)&0x1FF;
                    fprintf(stderr,"      prim#%d tp=(%d,%d) cl=(%d,%d) nv=%d xy=",q,gtpx,gtpy,gclx,gcly,g->nv);
                    for(int k=0;k<g->nv;k++) fprintf(stderr,"(%.0f,%.0f)",g->x[k],g->y[k]);
                    fprintf(stderr," uv=");
                    for(int k=0;k<g->nv;k++) fprintf(stderr,"(%d,%d)",g->u[k],g->v[k]);
                    fprintf(stderr,"\n"); gj=q; } }
            /* list ALL native tris covering the point (depth order) */
            fprintf(stderr,"[point] (%.0f,%.0f) NAT covering tris:\n",px,py);
            for (int i=0;i<s_tri_n;i++){ ttri* t=&s_tri[i];
                float ax=t->sx[0],ay=t->sy[0],bx=t->sx[1],by=t->sy[1],cx=t->sx[2],cy=t->sy[2];
                float d1=(px-bx)*(ay-by)-(ax-bx)*(py-by);
                float d2=(px-cx)*(by-cy)-(bx-cx)*(py-cy);
                float d3=(px-ax)*(cy-ay)-(cx-ax)*(py-ay);
                int neg=(d1<0)||(d2<0)||(d3<0), pos=(d1>0)||(d2>0)||(d3>0);
                if(neg&&pos) continue;
                double vz=(t->vz[0]+t->vz[1]+t->vz[2])/3.0;
                fprintf(stderr,"    tri#%d tp=(%d,%d) cl=(%d,%d) vz=%.0f xy=(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f) uv=(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)\n",
                    i,t->tpx,t->tpy,t->clx,t->cly,vz,
                    t->sx[0],t->sy[0],t->sx[1],t->sy[1],t->sx[2],t->sy[2],
                    t->u[0],t->v[0],t->u[1],t->v[1],t->u[2],t->v[2]); }
            (void)bi;(void)bvz;
            if(gj>=0){ prim_t* g=&s_gte[gj];
                int gtpx=(g->tpage&0xF)*64, gtpy=((g->tpage>>4)&1)*256;
                int gclx=(g->clut&0x3F)*16, gcly=(g->clut>>6)&0x1FF;
                int gm=(g->tpage>>7)&3, gbpp=(gm==0)?4:(gm==1)?8:16;
                fprintf(stderr,"          (%.0f,%.0f) PSX prim#%d tex=%d tp=(%d,%d) bpp=%d cl=(%d,%d) raw tpage=%04X clut=%04X uv=",
                    px,py,gj,g->textured,gtpx,gtpy,gbpp,gclx,gcly,g->tpage,g->clut);
                for(int k=0;k<g->nv;k++) fprintf(stderr,"(%d,%d)",g->u[k],g->v[k]);
                fprintf(stderr,"\n");
            } else fprintf(stderr,"          (%.0f,%.0f) PSX none\n",px,py);
            const char* sc=strchr(p,';'); if(!sc) break; p=sc+1;
        }
      }
    }

}

/* ---- gating --------------------------------------------------------------- */
/** @brief Returns whether OPENPETE_TERRAIN_VERIFY is set, latched on first use.
 * @return Nonzero when the verify dumps are enabled. */
static int terrain_verify_enabled(void) {
    static int v = -1;
    ENV_FLAG_LATCH(v, "OPENPETE_TERRAIN_VERIFY");
    return v;
}
/** @brief Returns the VBL from which the verify path runs, from OPENPETE_TERRAIN_VERIFY_FRAME.
 *
 * Read on the first call (0 when unset) and cached.
 * @return The first VBL of the verify reports.
 */
static unsigned terrain_verify_frame(void) {
    static unsigned f = 0xFFFFFFFFu;
    if (f == 0xFFFFFFFFu) { const char* e = getenv("OPENPETE_TERRAIN_VERIFY_FRAME"); f = e ? (unsigned)strtoul(e, NULL, 0) : 0; }
    return f;
}

/* Hook the world renderer func_800258F0 directly (NOT the func_8002B9CC wrapper):
 * level/overlay draw paths call func_800258F0 directly, bypassing func_8002B9CC,
 * so a wrapper hook misses in-level frames. Widescreen also hooks 0x800258F0 but
 * falls through (returns 0) at native aspect (disp_w_factor==1), and is
 * registered before us, so our hook is reached at native/headless aspect. (In
 * widescreen mode widescreen runs the renderer + returns 1 first; the native
 * capture/feed runs at native aspect so that's fine.) */
/* Occlusion group from the last real render call - reused by the sub-tick
 * re-extract (which has no a0). -1 = walk-all (valid index or -1, as the .s). */
static int32_t s_terrain_saved_occGroup = -1;

/* A canonical native build + feed held until the tick's portal capture
 * exists (terrain_game.env_feed_waits_for_portals). The world draw's RAM
 * verdicts persist until the portal draw that flushes it, which runs next
 * in the same draw. Census (OPENPETE_TERRAIN_DEFER_CENSUS=1, at exit):
 * deferred feeds, feeds flushed by the portal draw, and feeds still pending
 * when the next world draw began (an invariant breach, flushed there). */
static struct { CPUState* cpu; int32_t occ; int pending; } s_env_defer;
static unsigned s_env_defer_n, s_env_defer_flushed, s_env_defer_late;

/** @brief Print the deferred-feed census at exit. */
static void terrain_defer_census(void) {
    fprintf(stderr, "terrain: deferred env feed: deferred=%u flushed=%u late=%u\n",
            s_env_defer_n, s_env_defer_flushed, s_env_defer_late);
}

/** @brief The canonical native terrain build and scene feed of one world draw.
 *
 * Builds the native terrain for the draw's occlusion group, opens the native
 * scene for openpete_frame, and pushes the built triangles to it.
 * @param cpu      guest CPU state
 * @param occGroup the world draw's occlusion-group argument (a0), -1 = walk all */
static void terrain_native_build_feed(CPUState* cpu, int32_t occGroup) {
    { OP_PROF_ACCUM_SCOPE("terrain.build");
      terrain_build(cpu, occGroup); }            /* native walk + tris */
    openpete_native_scene_begin_frame(openpete_frame);  /* frame-keyed (composes) */
    { OP_PROF_ACCUM_SCOPE("terrain.feed");
      terrain_feed_native(cpu); }                /* push tris to the scene */
}

void terrain_native_flush_deferred(void) {
    if (!s_env_defer.pending) return;
    s_env_defer.pending = 0;
    s_env_defer_flushed++;
    terrain_native_build_feed(s_env_defer.cpu, s_env_defer.occ);
}

/** @brief Flush a deferred feed still pending when the next world draw begins.
 *
 * A pending feed there is never expected; it is counted as late in the census. */
static void terrain_defer_flush_late(void) {
    if (!s_env_defer.pending) return;
    s_env_defer_late++;
    terrain_native_flush_deferred();
}

/** @brief Run the canonical native build and feed now, or hold them for the tick's portal capture.
 *
 * They are held when terrain_game.env_feed_waits_for_portals reports that the
 * game draws the portals after this world draw; terrain_native_flush_deferred
 * runs them. The first held feed registers the exit census when
 * OPENPETE_TERRAIN_DEFER_CENSUS is set.
 * @param cpu      guest CPU state
 * @param occGroup the world draw's occlusion-group argument (a0), -1 = walk all */
static void terrain_native_build_feed_or_defer(CPUState* cpu, int32_t occGroup) {
    if (!terrain_game.env_feed_waits_for_portals ||
        !terrain_game.env_feed_waits_for_portals(cpu)) {
        terrain_native_build_feed(cpu, occGroup);
        return;
    }
    static int s_census = -1;
    if (s_census < 0) {
        const char* e = getenv("OPENPETE_TERRAIN_DEFER_CENSUS");
        s_census = (e && *e && *e != '0');
        if (s_census) atexit(terrain_defer_census);
    }
    s_env_defer.cpu = cpu; s_env_defer.occ = occGroup; s_env_defer.pending = 1;
    s_env_defer_n++;
}

/** @brief World-draw hook.
 *
 * Inert (returns 0) unless the terrain verify harness, the native scene, or a
 * render-path selection is on, and on re-entry. Flushes a deferred feed still
 * pending from the previous draw, saves the occlusion group (a0) for the
 * sub-tick re-extract, runs the game's world draw once through the module's
 * dispatch table, and records its polybuf interval under RP_TERRAIN. The native
 * terrain is built and fed, or held for the tick's portal capture, when the
 * draw emitted prims or under --no-lod, which marks RP_TERRAIN drawn when the
 * draw emitted none. OPENPETE_ENV_LODDIST overrides the environment's LOD
 * distance for the duration of the draw only. Under OPENPETE_TERRAIN_VERIFY
 * the first 8 eligible ticks dump and compare the native build.
 * @param cpu  guest CPU state
 * @param addr the hooked draw's address (unused)
 * @return 1 after running the draw, 0 when the hook is inert or re-entered */
static int on_world_draw_terrain(CPUState* cpu, uint32_t addr) {
    (void)addr;
    int verify = terrain_verify_enabled();
    int feed   = openpete_native_scene_enabled();
    if (!verify && !feed && !render_paths_active()) return 0;  /* inert: normal dispatch */
    static int s_in = 0; if (s_in) return 0; s_in = 1;
    terrain_defer_flush_late();

    /* Capture the occlusion-group arg BEFORE the real call clobbers a0. This is
     * the exact input the .s uses to build D_800771C8 - the ground truth for the
     * widescreen candidate set (terrain_vislist). */
    int32_t occGroup = (int32_t)cpu->a0;
    s_terrain_saved_occGroup = occGroup;   /* for sub-tick re-extract */

    /* OPENPETE_ENV_LODDIST=<int>: substitute g_Environment.m_LodDistance for
     * the DURATION OF THE TERRAIN RENDER CALL ONLY (poke, call, restore).
     * Measurement legs only (never a shipping mode): saturating it
     * (0x7FFFFFFF) makes the .s drop ALL LP and keep HP everywhere, so a
     * PsyCross -og capture becomes a pure-HP reference - "does authored HP
     * cover this pixel" from the game's own renderer, the ground-truth leg
     * for extended-LOD fidelity and the fill-tag sweep ({og-orig covers ∧
     * og-sat uncovered} = LP-only sightlines, a level-data property).
     * The bracket keeps every non-terrain reader of the field canonical.
     * Value ceiling: past ~2x the authored 0x8000 the big swamp levels
     * (misty_bog/beast_makers/ice_cavern) exhaust the frame's prim budget
     * under all-HP sector lists - empty renders, OT-walk hangs, or crashes -
     * so measurement legs use 0x10000, which is already far beyond anything
     * visible through fog. */
    static int s_lodpoke = -2;
    if (s_lodpoke == -2) { const char* e = getenv("OPENPETE_ENV_LODDIST");
                           s_lodpoke = (e && *e) ? (int)strtol(e, NULL, 0) : -1; }
    uint32_t lodsave = 0;
    if (s_lodpoke >= 0 && !game_layout.env.lod_dist) s_lodpoke = -1;   /* no field to poke */
    if (s_lodpoke >= 0) {
        lodsave = cpu->read_word(game_layout.env.lod_dist);
        cpu->write_word(game_layout.env.lod_dist, (uint32_t)s_lodpoke);
    }
    uint32_t startp = cpu->read_word(game_layout.polybuf.cursor);
    terrain_visibility_capture_arm();
    { OP_PROF_ACCUM_SCOPE("terrain.srend");
      /* The game's own world draw, by address through the module's dispatch
       * table (the raw recompiled entry, not the override chain), so the
       * verdicts it writes are intact. */
      openpete_recomp_module_get()->dispatch(cpu, game_layout.hooks.world_draw); }
    s_vis_arm = 0;
    if (s_lodpoke >= 0)
        cpu->write_word(game_layout.env.lod_dist, lodsave);
    uint32_t endp = cpu->read_word(game_layout.polybuf.cursor);

    render_path_record(RP_TERRAIN, startp, endp);

    if ((feed || verify) && endp > startp) capture_prims(cpu, startp, endp);

    /* `endp > startp` = the .s renderer emitted at least one prim, i.e. it had
     * something visible. Under EXTENDED LOD that is the wrong gate: the native
     * walk draws past the .s's LP culling distance by design, and a viewpoint
     * where EVERY sector is beyond that distance (freecam high above a level)
     * leaves the .s emitting nothing at all - so the feed never ran and the
     * level vanished while mobys, fed by their own producer, still drew.
     * Being inside this hook is the state signal that matters: during a level
     * transition the game stops calling func_800258F0 entirely, which is what
     * keeps a stale level out of the scene. */
    if (feed && (endp > startp || openpete_no_lod) && render_path_enabled(RP_TERRAIN)) {
        /* Canon-drawn is latched from the polybuf intervals, so an .s that
         * emitted nothing reads as NOT drawn and the sub-tick re-extract would
         * skip terrain - painting it on canonical presents and dropping it on
         * the intermediates. Mark it explicitly: the frame DID paint terrain,
         * natively. */
        if (endp <= startp) render_path_mark_drawn(RP_TERRAIN);
        /* The feed's portal-plane provider reads the portal capture, which a
         * draw that runs the portals after the environment has not produced
         * yet this tick; the sub-tick re-extract reads that tick's capture.
         * Hold the build and feed until the portal draw completes it. */
        terrain_native_build_feed_or_defer(cpu, occGroup);
    }

    if (verify && openpete_frame >= terrain_verify_frame()) {
        static int s_reports = 0;
        if (s_reports < 8) {
            terrain_dump(cpu);
            terrain_build(cpu, occGroup); /* native float decode + projection + faces */
            terrain_compare();          /* geometry A/B vs captured prims */
            s_reports++;
        }
    }
    s_in = 0;
    return 1;
}

/* Sub-tick re-extract; documented at the declaration in terrain.h. */
void terrain_reextract(CPUState* cpu, double alpha, int interp_on) {
    if (!openpete_native_scene_enabled()) return;
    if (!render_path_enabled(RP_TERRAIN)) return;
    /* Reproduce terrain ONLY if the canonical frame drew it. During a level
     * transition the game stops calling func_800258F0 (no world to render), but
     * s_terrain_saved_occGroup + the level data still in RAM are stale; replaying
     * them paints the old level's geometry over the cyclorama. The canon-drawn
     * mask is clear for RP_TERRAIN on those frames, so we skip. */
    if (!render_path_canon_drawn(RP_TERRAIN)) return;
    /* OPENPETE_TERRAIN_VANIM=0: snap poly-anim verts to cur (A/B)
     * OPENPETE_TERRAIN_TANIM=0: snap UV-scroll texture anim to cur (A/B)
     * OPENPETE_TERRAIN_CANIM=0: hold env-anim colours at the previous tick (census) */
    static tn_envvbl_t vanim_c, tanim_c, canim_c;
    int s_vanim_en = tn_env_vbl(&vanim_c, "OPENPETE_TERRAIN_VANIM", 1, 1);
    s_tanim_en = tn_env_vbl(&tanim_c, "OPENPETE_TERRAIN_TANIM", 1, 1);
    s_canim_en = tn_env_vbl(&canim_c, "OPENPETE_TERRAIN_CANIM", 1, 1);
    s_vanim_sub = 1; s_vanim_on = interp_on && s_vanim_en; s_vanim_alpha = alpha;
    terrain_build(cpu, s_terrain_saved_occGroup);
    terrain_feed_native(cpu);
    s_vanim_sub = 0; s_vanim_on = 0; s_tanim_en = 0;
}

/* Per-vertex accumulator for the two passes below. The decode mirrors
 * tsc_decode_verts + the origin application (x adds, y/z SUBTRACT - see the
 * hpVert dump in terrain_dump): world/16 point = (originX + vx,
 * originY - vy, originZ - vz). Bounding SPHERES were tried first and ran
 * ~2x too high: the header spheres bake vertical extent and padding into
 * the radius, so the lateral bound they give is far fatter than the mesh. */
typedef struct { double px, py, H, need; } td_acc;
/** @brief Accumulates a sector vertex bank into the bounds or the top-down height requirement.
 *
 * Each vertex is unpacked (tvert_unpack) and placed at (ox + vx, oy - vy,
 * oz - vz) in world/16. A non-NULL mm widens {minX, maxX, minY, maxY, maxZ}.
 * A non-NULL acc raises acc->need to the height at which the vertex still
 * projects in frame from a camera over (acc->px, acc->py): its Z plus the
 * larger of its lateral offsets scaled by acc->H over the half-extents 244
 * and 100.
 * @param cpu The guest CPU state.
 * @param verts The bank's address.
 * @param nv The vertex count.
 * @param flat The sector's flat-vertex flag.
 * @param ox The sector origin X.
 * @param oy The sector origin Y.
 * @param oz The sector origin Z.
 * @param mm The bounds to widen, or NULL.
 * @param acc The height accumulator, or NULL.
 */
static void td_scan_verts(CPUState* cpu, uint32_t verts, int nv, int flat,
                          int ox, int oy, int oz,
                          double mm[5], td_acc* acc) {
    for (int k = 0; k < nv; k++) {
        uint32_t w = rd(cpu, verts + (uint32_t)k * 4);
        int vx, vy, vz; tvert_unpack(w, flat, &vx, &vy, &vz);
        double x = (double)ox + (double)vx;
        double y = (double)oy - (double)vy;
        double z = (double)oz - (double)vz;
        if (mm) {
            if (x < mm[0]) mm[0] = x;
            if (x > mm[1]) mm[1] = x;
            if (y < mm[2]) mm[2] = y;
            if (y > mm[3]) mm[3] = y;
            if (z > mm[4]) mm[4] = z;
        }
        if (acc) {
            /* yaw-0 top-down maps world X to screen-horizontal (half extent
             * 256 walk px at 4:3, wider under Hor+) and world Y to vertical
             * (224-line crop -> half 112). 244/100 leave margin for the ~3°
             * tilt (pitch 992 = the drive clamp, not a true 90°). */
            double nx = fabs(x - acc->px) * acc->H / 244.0;
            double ny = fabs(y - acc->py) * acc->H / 100.0;
            double need = z + (nx > ny ? nx : ny);
            if (need > acc->need) acc->need = need;
        }
    }
}
/* Documented at the declaration in terrain.h; takes the top-down pose's first pass only. */
int terrain_native_sweep_pose(CPUState* cpu, int corner, int32_t v[5]) {
    uint32_t sectorPtr = tenv_rd(cpu, game_layout.env.sector_ptr);
    uint32_t sectorCnt = tenv_rd(cpu, game_layout.env.sector_count);
    if (!sectorPtr || !sectorCnt || sectorCnt > 1024) return 0;

    double mm[5] = { 1e18, -1e18, 1e18, -1e18, -1e18 };  /* minx,maxx,miny,maxy,maxz */
    int seen = 0;
    for (uint32_t i = 0; i < sectorCnt; i++) {
        uint32_t s = rd(cpu, sectorPtr + i * 4);
        if (!s) continue;
        uint32_t g1 = rd(cpu, s + 0x08), g2 = rd(cpu, s + 0x0C);
        uint32_t g3 = rd(cpu, s + 0x10), g4 = rd(cpu, s + 0x14);
        int flat = thdr_flat(rd(cpu, s + 0x04));
        int ox = (int)(g1 >> 16), oy = (int)(g1 & 0xFFFF), oz = (int)(g2 >> 16);
        int nLpV = (int)(g3 & 0xFF), nHpV = (int)(g4 & 0xFF);
        uint32_t hpOff = (g4 >> 24) & 0xFF;
        td_scan_verts(cpu, s + 0x1C, nLpV, flat, ox, oy, oz, mm, NULL);
        td_scan_verts(cpu, s + 0x1C + hpOff * 4, nHpV, flat, ox, oy, oz, mm, NULL);
        seen++;
    }
    if (!seen || mm[1] < mm[0]) return 0;

    /* Inset 30% from the centre so the circuit stays over the level rather
     * than orbiting its empty corners, and ride 64 (world/16) above the
     * highest vertex so the camera clears geometry instead of burrowing. */
    static const double sx[4] = { -1.0,  1.0,  1.0, -1.0 };
    static const double sy[4] = { -1.0, -1.0,  1.0,  1.0 };
    const int c = ((corner % 4) + 4) % 4;
    const int nxt = (c + 1) % 4;

    const double cx = (mm[0] + mm[1]) * 0.5, cy = (mm[2] + mm[3]) * 0.5;
    const double hx = (mm[1] - mm[0]) * 0.30, hy = (mm[3] - mm[2]) * 0.30;

    const double px = cx + sx[c] * hx,   py = cy + sy[c] * hy;
    const double nx = cx + sx[nxt] * hx, ny = cy + sy[nxt] * hy;

    /* Face the next waypoint: the drive basis is forward = (cos yaw, sin yaw). */
    const double yaw = atan2(ny - py, nx - px) * (4096.0 / (2.0 * 3.14159265358979323846));

    v[0] = (int32_t)(px * 16.0);
    v[1] = (int32_t)(py * 16.0);
    v[2] = (int32_t)((mm[4] + 64.0) * 16.0);
    v[3] = (int32_t)(int16_t)(uint16_t)(int32_t)yaw;
    v[4] = 0;      /* horizontal - looking into the level, not down at it */
    return 1;
}

int terrain_native_topdown_pose(CPUState* cpu, int32_t v[5]) {
    uint32_t sectorPtr = tenv_rd(cpu, game_layout.env.sector_ptr);
    uint32_t sectorCnt = tenv_rd(cpu, game_layout.env.sector_count);
    if (!sectorPtr || !sectorCnt || sectorCnt > 1024) return 0;
    /* The pose is written into g_Camera by freecam, so it is derived at the
     * canonical H: a display FOV must not reach that write. */
    ngte_view pv, cv; ngte_view_build(cpu, &pv); ngte_view_canon(&pv, &cv);
    double H = cv.H_eff;
    if (H < 1.0) H = 400.0;
    /* Pass 1: true vertex bounds (both banks) -> centre + level top. */
    double mm[5] = { 1e18, -1e18, 1e18, -1e18, -1e18 };  /* minx,maxx,miny,maxy,maxz */
    int seen = 0;
    for (uint32_t i = 0; i < sectorCnt; i++) {
        uint32_t s = rd(cpu, sectorPtr + i * 4);
        if (!s) continue;
        uint32_t g1 = rd(cpu, s + 0x08), g2 = rd(cpu, s + 0x0C);
        uint32_t g3 = rd(cpu, s + 0x10), g4 = rd(cpu, s + 0x14);
        int flat = thdr_flat(rd(cpu, s + 0x04));
        int ox = (int)(g1 >> 16), oy = (int)(g1 & 0xFFFF), oz = (int)(g2 >> 16);
        int nLpV = (int)(g3 & 0xFF), nHpV = (int)(g4 & 0xFF);
        uint32_t hpOff = (g4 >> 24) & 0xFF;
        td_scan_verts(cpu, s + 0x1C, nLpV, flat, ox, oy, oz, mm, NULL);
        td_scan_verts(cpu, s + 0x1C + hpOff * 4, nHpV, flat, ox, oy, oz, mm, NULL);
        seen++;
    }
    if (!seen || mm[1] < mm[0]) return 0;
    /* Pass 2: lowest camera that still projects every vertex in-frame. */
    td_acc acc = { (mm[0] + mm[1]) * 0.5, (mm[2] + mm[3]) * 0.5, H, mm[4] + 64.0 };
    for (uint32_t i = 0; i < sectorCnt; i++) {
        uint32_t s = rd(cpu, sectorPtr + i * 4);
        if (!s) continue;
        uint32_t g1 = rd(cpu, s + 0x08), g2 = rd(cpu, s + 0x0C);
        uint32_t g3 = rd(cpu, s + 0x10), g4 = rd(cpu, s + 0x14);
        int flat = thdr_flat(rd(cpu, s + 0x04));
        int ox = (int)(g1 >> 16), oy = (int)(g1 & 0xFFFF), oz = (int)(g2 >> 16);
        int nLpV = (int)(g3 & 0xFF), nHpV = (int)(g4 & 0xFF);
        uint32_t hpOff = (g4 >> 24) & 0xFF;
        td_scan_verts(cpu, s + 0x1C, nLpV, flat, ox, oy, oz, NULL, &acc);
        td_scan_verts(cpu, s + 0x1C + hpOff * 4, nHpV, flat, ox, oy, oz, NULL, &acc);
    }
    double pz = acc.need * 1.02;
    /* Ceiling: past view-Z ~32767 (world/16) the .s's 16-bit frustum stage
     * rejects every sector, the visibility verdict goes empty and the native walk -
     * whose 4:3 candidate set IS that verdict - draws nothing. Clamp under it; a level too wide to fit
     * from there gets cropped edges rather than a blank frame. */
    if (pz > 30000.0) {
        fprintf(stderr, "[terrain] topdown pose height clamped (%d -> 30000 world/16): "
                "level too wide for the .s frustum ceiling - edges may crop\n",
                (int)pz);
        pz = 30000.0;
    }
    v[0] = (int32_t)(acc.px * 16.0);
    v[1] = (int32_t)(acc.py * 16.0);
    v[2] = (int32_t)(pz * 16.0);
    v[3] = 0;      /* yaw */
    v[4] = 992;    /* pitch: straight down at the drive clamp */
    return 1;
}

/* The hook address is the layout's world-draw site, bound at boot: a
 * static initializer cannot read another TU's const. */
/** @brief Hooks the visibility function's entry to capture the sector visibility table it computes in the scratchpad.
 *
 * Bound only on a game whose draw computes the table in the scratchpad
 * (game_layout.hooks.visibility). While armed (terrain_visibility_capture_arm),
 * the world call (a0 == 0) runs the real function through the module's
 * dispatch, then copies min(sector count, TVIS_MAX) bytes from
 * game_layout.visibility.base before the draw's walk consumes them, and
 * disarms. A call made while not armed, with a0 != 0, or re-entered falls
 * through to the game.
 * @param cpu The guest CPU state.
 * @param addr The hooked address.
 * @return 1 when the call was handled here, 0 to fall through. */
static int on_visibility_terrain(CPUState* cpu, uint32_t addr) {
    if (!s_vis_arm || cpu->a0 != 0u) return 0;
    static int s_in = 0; if (s_in) return 0; s_in = 1;
    openpete_recomp_module_get()->dispatch(cpu, addr);
    s_in = 0;
    uint32_t n = cpu->read_word(game_layout.env.sector_count);
    if (n > TVIS_MAX) n = TVIS_MAX;
    for (uint32_t i = 0; i < n; i++) s_vis_cap[i] = cpu->read_byte(game_layout.visibility.base + i);
    s_vis_cap_n = (int)n;
    s_vis_arm = 0;
    return 1;
}

void terrain_visibility_capture_arm(void) {
    s_vis_cap_n = -1;
    s_vis_arm   = game_layout.hooks.visibility != 0u;
}

int terrain_visibility_capture(const uint8_t** out) {
    if (out) *out = s_vis_cap;
    return s_vis_cap_n;
}

static feature_dispatch_entry_t terrain_dispatch[] = {
    { 0u, on_world_draw_terrain },
    { 0u, on_visibility_terrain },
    { 0u, NULL },
};

/** @brief Binds the terrain module's dispatch entries to the layout's hook addresses.
 *
 * Entry 0 takes hooks.world_draw and entry 1 hooks.visibility, which is 0
 * on a game whose table stays in RAM.
 * @param opts Unused.
 */
static void terrain_apply_options(const openpete_options_t* opts) {
    (void)opts;
    terrain_dispatch[0].addr = game_layout.hooks.world_draw;
    terrain_dispatch[1].addr = game_layout.hooks.visibility;   /* 0 on a game whose table stays in RAM */
}

const feature_module_t terrain_module = {
    .name           = "terrain_native",
    .apply_options  = terrain_apply_options,
    .dispatch_table = terrain_dispatch,
};
