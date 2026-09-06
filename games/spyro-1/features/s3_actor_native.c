// games/spyro-1/features/s3_actor_native.c — the SPYRO 3 PLAYER MODEL as
// a host-side actor source for the mod actor render service (M12 of
// notes/plans/openpete-modding-framework.md; hover-anim upgrade of
// notes/plans/openpete-triangle-flutter.md).
//
// Renders poses of the Spyro 3 player model (230 verts, body+head, 62
// anims — hover = 0x12) entirely host-side: the blob is derived locally
// from the user's own Year of the Dragon disc (tools/extract_s3_player.py
// — never shipped) and parsed from an enabled mod's assets/s3/ dir; the
// decoder runs on host memory; the triangles feed the native scene
// through the same path as the S1 ghost. Zero guest-RAM involvement.
//
// Format (spyro-3 decomp recon 2026-07-11 + SpyroEdit's verified decoder
// SpyroRender.cpp ConvertAnimatedVertices/ConvertFaces/UpdateSpyro):
//   model: numAnims u32 @+0, total size @+0x34, anim offset table @+0x3C
//     (u32 model-relative, -1 = absent).
//   anim:  {u8 numFrames, numColours, headVertStart, numVerts;
//           u32 verts, faces, colours, data (model-relative);
//           frame records @+0x14, stride 0x14}.
//   frame: {u16 blockOffset; u16 flags (bit0 body/bit1 head start-on-
//           block); u32 word1 (bits[9:0] head block halfword-offset,
//           [19:10] body adjust byte-offset, [31:20] head adjust);
//           u32 headPos (packed 11/11/10, z*2); u32 shadowL, shadowR}.
//   vertex streams: per-vertex packed base words (11/11/10, z*2) +
//     block halfwords (bit0 chain, bit1 long/absolute) + adjust bytes
//     (LUT[b>>1] added to the base word, bit0 chain) — the same
//     128-entry delta LUT as Spyro 1 (byte-identical table).
//   faces: word0 = byte length; records of FIVE words (player):
//     [w] verts (tri if lo bytes equal, else quad), [w+1] 7-bit colour
//     indices, [w+2..4] UV/texpage words (unused in the gouraud v1).

#include "feature_registry.h"
#include "psx_runtime_internal.h"
#include "openpete_native_render.h"
#include "openpete_log.h"
#include "openpete_mods.h"
#include "spyro_native_reextract.h" /* openpete_native_interp_pos */
#include "native_gte.h"
#include "s3_actor_native.h"
#include "spyro1_actor_pose.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spyro1_symbols.gen.h"

extern unsigned openpete_frame;

/* ---- model blob ----------------------------------------------------- */

#define S3_MAX_VERTS 256

typedef struct {
    uint8_t* blob;
    uint32_t size;
    uint32_t num_anims;
} s3_model_t;

static s3_model_t s_m;      /* loaded model (blob = NULL until found) */
static int s_load_state;    /* 0 not tried, 1 loaded, -1 failed       */

static uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }

/* Scan enabled mods' asset roots (highest priority first) for
 * assets/s3/s3_player.bin — the derive-locally location documented by
 * tools/extract_s3_player.py. */
static void s3_try_load(void) {
    if (s_load_state != 0) return;
    s_load_state = -1;
    int n = openpete_mods_asset_root_count();
    for (int r = n - 1; r >= 0; r--) {
        const char* owner = NULL;
        const char* root = openpete_mods_asset_root(r, &owner);
        if (!root) continue;
        char p[1100];
        snprintf(p, sizeof p, "%s/s3/s3_player.bin", root);
        FILE* f = fopen(p, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz < 0x1000 || sz > 0x100000) { fclose(f); continue; }
        uint8_t* b = (uint8_t*)malloc((size_t)sz);
        if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) {
            free(b); fclose(f); continue;
        }
        fclose(f);
        uint32_t na = rd32(b), total = rd32(b + 0x34);
        if (na == 0 || na > 128 || total > (uint32_t)sz) {
            OPL_WARN_F("s3actor", "%s: %s does not parse as a player "
                       "model (numAnims=%u total=%u size=%ld)",
                       owner ? owner : "?", p, na, total, sz);
            free(b);
            continue;
        }
        s_m.blob = b; s_m.size = (uint32_t)sz; s_m.num_anims = na;
        s_load_state = 1;
        OPL_INFO_F("s3actor", "Spyro 3 player model loaded from %s "
                   "(%u anims, %u bytes)", p, na, total);
        return;
    }
}

/* ---- vertex decode (SpyroEdit ConvertAnimatedVertices, int-exact) --- */

/* signed bit-field extract: bits [pos, pos+len) of v */
static inline int32_t bitss(uint32_t v, int pos, int len) {
    return (int32_t)(v << (32 - pos - len)) >> (32 - len);
}

/* The 128-entry canned-delta LUT — byte-identical between S1 and S3
 * (recon-verified), read from S1's own copy in guest RAM at D_VERT_LUT
 * would couple us to guest state; carry the table here instead, sourced
 * from SpyroEdit's host-endian dump (mobyVertAdjustTable). Generated:
 * LUT[i] = the packed 11/11/10 delta word for adjust byte i<<1. */
static int32_t s_lut[128];
static int     s_lut_ready;
static void s3_lut_init(CPUState* cpu) {
    if (s_lut_ready) return;
    /* Read S1's in-RAM copy once (identical data; saves embedding a
     * 512-byte table and keeps a single source of truth). */
    for (int i = 0; i < 128; i++)
        s_lut[i] = (int32_t)cpu->read_word(D_VERT_LUT + (uint32_t)(i << 2));
    s_lut_ready = 1;
}

typedef struct { int32_t x, y, z; } s3_v3;

/* Decode one segment's vertices for ONE keyframe into out[] (model
 * space). Mirrors SpyroEdit ConvertAnimatedVertices(isSpyro=true). */
static void s3_decode_segment(const uint32_t* verts, const uint8_t* blocks_base,
                              uint32_t block_hw_off, const uint8_t* adjusts,
                              int start_on_block, int nverts, s3_v3* out) {
    int curX = 0, curY = 0, curZ = 0;
    int curBlock = (int)block_hw_off;
    int nextBlock = start_on_block ? 1 : 0;
    int blockVert = 0;
    int adjustVert = 0;

    for (int i = 0; i < nverts; i++) {
        int usedBlock = 0;
        uint32_t vw = verts[i];
        int x = bitss(vw, 21, 11), y = bitss(vw, 10, 11), z = bitss(vw, 0, 10) * 2;

        if (nextBlock) {
            uint16_t hw = rd16(blocks_base + curBlock * 2);
            blockVert = (hw & 1) != 0;
            nextBlock = blockVert;
            if (hw & 0x2) { /* long block: absolute position */
                uint32_t pos = (uint32_t)hw |
                               ((uint32_t)rd16(blocks_base + (curBlock + 1) * 2) << 16);
                curX = bitss(pos, 21, 11);
                curY = bitss(pos, 11, 11);
                curZ = bitss(pos, 1, 11);
                x = 0; y = 0; z = 0;
                curBlock += 2;
            } else {        /* short block: delta */
                curX += bitss(hw, 9, 7);
                curY -= bitss(hw, 4, 7);
                curZ -= bitss(hw, 1, 5) * 4;
                curBlock += 1;
            }
            usedBlock = 1;
        }
        if (!usedBlock && !blockVert) {
            uint32_t adjusted = (uint32_t)((int64_t)s_lut[adjusts[adjustVert] >> 1] +
                                           (int64_t)vw);
            if (adjusts[adjustVert] & 1) nextBlock = 1;
            x = bitss(adjusted, 21, 11);
            y = bitss(adjusted, 10, 11);
            z = bitss(adjusted, 0, 10) * 2;
            adjustVert++;
        }
        curX += x; curY -= y; curZ -= z;
        out[i].x = curX; out[i].y = -curY; out[i].z = -curZ;
    }
}

/* Decode a whole keyframe (body + head + headPos) into out[nverts]. */
static int s3_decode_frame(const uint8_t* anim, int frame, s3_v3* out) {
    uint8_t numFrames = anim[0];
    uint8_t headVertStart = anim[2], numVerts = anim[3];
    if (frame >= numFrames) return -1;
    /* anim stream fields are SHARED-POOL-relative (the boot fixup adds
     * pool = model + [model+0x38]; on disc they are raw pool offsets) */
    const uint8_t* model = s_m.blob;
    const uint8_t* pool  = model + rd32(model + 0x38);
    const uint32_t* verts = (const uint32_t*)(pool + rd32(anim + 4));
    const uint8_t*  data  = pool + rd32(anim + 0x10);
    const uint8_t*  fr    = anim + 0x14 + frame * 0x14;
    uint16_t blockOffset = rd16(fr);
    uint16_t flags       = rd16(fr + 2);
    uint32_t word1       = rd32(fr + 4);
    uint32_t headPos     = rd32(fr + 8);

    const uint8_t* blocks  = data + blockOffset;          /* halfword base */
    const uint8_t* adjB    = data + blockOffset + ((word1 >> 10) & 0x3FF);
    const uint8_t* adjH    = data + blockOffset + (word1 >> 20);
    uint32_t headBlockHw   = (word1 & 0x3FF) / 2;

    s3_decode_segment(verts, blocks, 0, adjB, flags & 1, headVertStart, out);
    s3_decode_segment(verts + headVertStart, blocks, headBlockHw, adjH,
                      (flags & 2) != 0, numVerts - headVertStart,
                      out + headVertStart);

    int hx = bitss(headPos, 21, 11), hy = bitss(headPos, 10, 11),
        hz = bitss(headPos, 0, 10) * 2;
    for (int v = headVertStart; v < numVerts; v++) {
        out[v].x += hx; out[v].y += -(-hy); out[v].z += hz;
    }
    /* SpyroEdit adds headOffset as (headX, headY, headZ) in its own
     * output space; ours negates y/z at emit — headPos y sign folds to
     * +hy here (verified visually; flip here if the head sits wrong). */
    (void)numFrames;
    return numVerts;
}

/* ---- emit: pose -> projected, lit triangles in the native scene ----- */

/* Model-units -> Spyro 1 world units. SpyroEdit renders S3 player verts
 * and world positions through one scale, so model space is world-scaled
 * already; S1 vs S3 world-unit ratio is absorbed here. Tunable while
 * calibrating: OPENPETE_S3_SCALE (float, default 1.0). */
static double s3_scale(void) {
    static double v = -1.0;
    if (v < 0.0) {
        const char* e = getenv("OPENPETE_S3_SCALE");
        v = (e && *e) ? atof(e) : 1.0;
        if (v <= 0.0) v = 1.0;
    }
    return v;
}

/* M = PM * Ry*Rx*Rz from a packed byte-euler word — the same
 * composition the S1 player renderer uses (spyro_native.c
 * spyro_compose_euler, byte-table path). */
static void s3_compose_euler(CPUState* cpu, const int PM[3][3], uint32_t rot,
                             int out[3][3]) {
    uint32_t iy = (rot >> 15) & 0x1FEu, ix = (rot & 0xFF00u) >> 7,
             iz = (rot & 0xFFu) << 1;
    int sy = (int16_t)cpu->read_half(D_SIN_TABLE + iy);
    int cy = (int16_t)cpu->read_half(D_SIN_TABLE + 0x80u + iy);
    int sx = (int16_t)cpu->read_half(D_SIN_TABLE + ix);
    int cx = (int16_t)cpu->read_half(D_SIN_TABLE + 0x80u + ix);
    int sz = (int16_t)cpu->read_half(D_SIN_TABLE + iz);
    int cz = (int16_t)cpu->read_half(D_SIN_TABLE + 0x80u + iz);
    int Ry[3][3] = {{cy,0,-sy},{0,4096,0},{sy,0,cy}};
    int Rx[3][3] = {{4096,0,0},{0,cx,-sx},{0,sx,cx}};
    int Rz[3][3] = {{cz,-sz,0},{sz,cz,0},{0,0,4096}};
    int t1[3][3], t2[3][3];
    ngte_mat_mul3(PM, Ry, t1);
    ngte_mat_mul3(t1, Rx, t2);
    ngte_mat_mul3(t2, Rz, out);
}

static s3_v3  s_dv[S3_MAX_VERTS];        /* decoded (blended) model verts */
static float  s_px[S3_MAX_VERTS], s_py[S3_MAX_VERTS], s_pz[S3_MAX_VERTS];
static int    s_sat[S3_MAX_VERTS];

int spyro1_s3_actor_emit(CPUState* cpu, const void* pose, int len,
                         uint32_t tint_rgba) {
    if (!pose || len < (int)sizeof(spyro1_s3_actor_pose_t)) return 0;
    const spyro1_s3_actor_pose_t* p = (const spyro1_s3_actor_pose_t*)pose;
    if (p->magic != SPYRO1_S3_POSE_MAGIC ||
        p->version != SPYRO1_S3_POSE_VERSION) return 0;
    if (!openpete_native_render_enabled()) return 0;
    unsigned sf = openpete_native_scene_frame();
    if (sf == 0xFFFFFFFFu || (unsigned)(openpete_frame - sf) > 6u) return 0;

    s3_try_load();
    if (s_load_state != 1) return 0;
    s3_lut_init(cpu);

    const uint8_t* model = s_m.blob;
    if (p->anim >= s_m.num_anims) return 0;
    int32_t aoff = (int32_t)rd32(model + 0x3C + (uint32_t)p->anim * 4);
    if (aoff <= 0 || (uint32_t)aoff >= s_m.size) return 0;
    const uint8_t* anim = model + aoff;
    uint8_t numFrames = anim[0], numColours = anim[1];
    int nv;

    /* Two-keyframe blend: decode both real frames, lerp model-space. */
    static s3_v3 va[S3_MAX_VERTS], vb[S3_MAX_VERTS];
    if (s3_decode_frame(anim, p->frame_a % numFrames, va) <= 0) return 0;
    nv = s3_decode_frame(anim, p->frame_b % numFrames, vb);
    if (nv <= 0) return 0;
    int bl = p->blend;
    for (int i = 0; i < nv; i++) {
        s_dv[i].x = va[i].x + ((vb[i].x - va[i].x) * bl) / 255;
        s_dv[i].y = va[i].y + ((vb[i].y - va[i].y) * bl) / 255;
        s_dv[i].z = va[i].z + ((vb[i].z - va[i].z) * bl) / 255;
    }

    /* Camera transform: PM * euler(rot), TR = PM*(cam - pos)/4096 — the
     * same derivation as the S1 ghost (spyro_native.c spyro_derive_phaseA
     * body case), against the CURRENT camera. */
    int PM[3][3]; ngte_read_matrix(cpu, G_CAMERA, PM);
    double cfx = (double)(int32_t)cpu->read_word(G_CAMERA + 0x28);
    double cfy = (double)(int32_t)cpu->read_word(G_CAMERA + 0x2C);
    double cfz = (double)(int32_t)cpu->read_word(G_CAMERA + 0x30);
    { double cf[3];
      if (openpete_native_interp_pos(G_CAMERA, cf)) { cfx = cf[0]; cfy = cf[1]; cfz = cf[2]; } }
    double IR1 = cfy - p->pos[1], IR2 = cfz - p->pos[2], IR3 = p->pos[0] - cfx;
    double TR[3];
    for (int i = 0; i < 3; i++)
        TR[i] = (PM[i][0]*IR1 + PM[i][1]*IR2 + PM[i][2]*IR3) / 4096.0;
    int M[3][3]; s3_compose_euler(cpu, PM, p->rot, M);

    double ofx = (double)((int32_t)cpu->gte_ctrl[24] >> 16);
    double ofy = (double)((int32_t)cpu->gte_ctrl[25] >> 16);
    double H   = (double)(cpu->gte_ctrl[26] & 0xFFFF);
    double sc  = s3_scale();

    { static int s_dbg = -1;
      if (s_dbg < 0) { const char* e = getenv("OPENPETE_S3_DBG"); s_dbg = (e && *e && *e != '0'); }
      if (s_dbg) {
          static unsigned last = 0;
          if (openpete_frame - last >= 100u) { last = openpete_frame;
              int x0=1<<30,y0=1<<30,z0=1<<30,x1=-(1<<30),y1=-(1<<30),z1=-(1<<30);
              for (int i = 0; i < nv; i++) {
                  if (s_dv[i].x<x0)x0=s_dv[i].x; if (s_dv[i].x>x1)x1=s_dv[i].x;
                  if (s_dv[i].y<y0)y0=s_dv[i].y; if (s_dv[i].y>y1)y1=s_dv[i].y;
                  if (s_dv[i].z<z0)z0=s_dv[i].z; if (s_dv[i].z>z1)z1=s_dv[i].z;
              }
              fprintf(stderr, "[s3-dbg f=%u] model bbox (%d,%d,%d)-(%d,%d,%d) "
                      "v0=(%d,%d,%d) v115=(%d,%d,%d) TR=(%.0f,%.0f,%.0f)\n",
                      openpete_frame, x0,y0,z0, x1,y1,z1,
                      s_dv[0].x, s_dv[0].y, s_dv[0].z,
                      s_dv[115].x, s_dv[115].y, s_dv[115].z, TR[0], TR[1], TR[2]);
          } } }

    int nsat = 0;
    for (int i = 0; i < nv; i++) {
        ngte_proj o;
        ngte_project_gte(M, TR, s_dv[i].x * sc, s_dv[i].y * sc,
                         s_dv[i].z * sc, ofx, ofy, H, 1, &o);
        s_px[i] = (float)o.sx; s_py[i] = (float)o.sy;
        s_pz[i] = (float)o.m3; s_sat[i] = o.sat;
        nsat += o.sat ? 1 : 0;
    }
    { static int s_dbg2 = -1;
      if (s_dbg2 < 0) { const char* e = getenv("OPENPETE_S3_DBG"); s_dbg2 = (e && *e && *e != '0'); }
      if (s_dbg2) { static unsigned last2 = 0;
          if (openpete_frame - last2 >= 100u) { last2 = openpete_frame;
              fprintf(stderr, "[s3-dbg f=%u] sat=%d/%d proj0=(%.0f,%.0f vz=%.0f)\n",
                      openpete_frame, nsat, nv, s_px[0], s_py[0], s_pz[0]); } } }

    /* Face walk (SpyroEdit ConvertFaces, animated+player): 5-word
     * records, byte vert indices, 7-bit colour indices. Gouraud v1 —
     * the UV/texpage words are S3 VRAM coords we don't have. */
    const uint8_t*  pool    = model + rd32(model + 0x38);
    const uint8_t*  colours = pool + rd32(anim + 0xC);
    const uint8_t*  faces   = pool + rd32(anim + 8);
    uint32_t flen = rd32(faces);
    uint32_t maxw = flen / 4 + 1;
    unsigned tA = (tint_rgba >> 24) & 0xFFu, tR = (tint_rgba >> 16) & 0xFFu;
    unsigned tG = (tint_rgba >> 8) & 0xFFu,  tB = tint_rgba & 0xFFu;
    int emitted = 0;
    int skip_idx = 0, skip_sat = 0, nrec = 0;
    openpete_native_scene_begin_frame(sf);
    for (uint32_t w = 1; w + 1 < maxw; w += 5) {
        nrec++;
        uint32_t fw = rd32(faces + w * 4);
        uint32_t cw = rd32(faces + w * 4 + 4);
        int quad = ((fw & 0xFF) != ((fw >> 8) & 0xFF));
        int vi[4], ci[4], nvf;
        if (quad) {
            vi[0] = (int)((fw >> 8) & 0xFF); vi[1] = (int)(fw & 0xFF);
            vi[2] = (int)((fw >> 16) & 0xFF); vi[3] = (int)((fw >> 24) & 0xFF);
            ci[0] = (int)((cw >> 10) & 0x7F); ci[1] = (int)((cw >> 3) & 0x7F);
            ci[2] = (int)((cw >> 17) & 0x7F); ci[3] = (int)((cw >> 24) & 0x7F);
            nvf = 4;
        } else {
            vi[0] = (int)((fw >> 24) & 0xFF); vi[1] = (int)((fw >> 16) & 0xFF);
            vi[2] = (int)((fw >> 8) & 0xFF);
            ci[0] = (int)((cw >> 24) & 0x7F); ci[1] = (int)((cw >> 17) & 0x7F);
            ci[2] = (int)((cw >> 10) & 0x7F);
            nvf = 3;
        }
        /* two tris per quad: (0,1,2) + (1,3,2) — the GT4 split */
        int tri_idx[2][3] = { {0, 1, 2}, {1, 3, 2} };
        int ntri = (nvf == 4) ? 2 : 1;
        for (int t = 0; t < ntri; t++) {
            int a = tri_idx[t][0], b = tri_idx[t][1], c = tri_idx[t][2];
            int ia = vi[a], ib = vi[b], ic = vi[c];
            if (ia >= nv || ib >= nv || ic >= nv) { skip_idx++; continue; }
            if (s_sat[ia] || s_sat[ib] || s_sat[ic]) { skip_sat++; continue; }
            /* screen-space backface cull (double-sided for v1: keep) */
            openpete_scene_tri st = {0};
            int idx[3] = { ia, ib, ic };
            int cidx[3] = { ci[a], ci[b], ci[c] };
            for (int k = 0; k < 3; k++) {
                st.v[k].sx = s_px[idx[k]]; st.v[k].sy = s_py[idx[k]];
                st.v[k].z  = openpete_native_depth_from_viewz(
                                 openpete_worldvz_full(s_pz[idx[k]]));
                st.v[k].u = 0.0f; st.v[k].v = 0.0f;
                uint32_t col = (cidx[k] < (int)numColours)
                                 ? rd32(colours + (uint32_t)cidx[k] * 4)
                                 : 0x808080u;
                uint32_t r = (((col      ) & 0xFFu) * tR) / 255u;
                uint32_t g = (((col >>  8) & 0xFFu) * tG) / 255u;
                uint32_t bcol = (((col >> 16) & 0xFFu) * tB) / 255u;
                st.v[k].rgba = r | (g << 8) | (bcol << 16) | 0xFF000000u;
            }
            st.textured = 0; st.tpage_x = 0; st.tpage_y = 0;
            st.clut_x = 0; st.clut_y = 0; st.bpp = 0;
            st.blend = (tA < 0xFFu) ? 1 : 0;
            openpete_native_scene_add_tri(&st, OPENPETE_SCENE_ACTOR, 0);
            emitted++;
        }
    }
    { static int s_fd = -1;
      if (s_fd < 0) { const char* e = getenv("OPENPETE_S3_DBG"); s_fd = (e && *e && *e != '0'); }
      if (s_fd) { static unsigned l3 = 0;
          if (openpete_frame - l3 >= 100u) { l3 = openpete_frame;
              int over = 0, mx = 0;
              for (uint32_t w2 = 1; w2 + 1 < maxw; w2 += 5) {
                  uint32_t cw2 = rd32(faces + w2 * 4 + 4);
                  int cs[4] = { (int)((cw2>>10)&0x7F), (int)((cw2>>3)&0x7F),
                                (int)((cw2>>17)&0x7F), (int)((cw2>>24)&0x7F) };
                  for (int k = 0; k < 4; k++) { if (cs[k] > mx) mx = cs[k];
                      if (cs[k] >= (int)numColours) over++; }
              }
              fprintf(stderr, "[s3-dbg f=%u] faces: flen=%u recs=%d emit=%d skip_idx=%d skip_sat=%d "
                      "numColours=%u cidx-max=%d over=%d colours0-3: %08x %08x %08x %08x\n",
                      openpete_frame, flen, nrec, emitted, skip_idx, skip_sat,
                      numColours, mx, over, rd32(colours), rd32(colours+4),
                      rd32(colours+8), rd32(colours+12)); } } }
    static int s_first = 0;
    if (!s_first && emitted) {
        s_first = 1;
        OPL_INFO_F("s3actor", "first S3 actor emit: anim 0x%x frames %u/%u "
                   "blend %u -> %d tris (%d verts)", p->anim, p->frame_a,
                   p->frame_b, p->blend, emitted, nv);
    }
    return emitted;
}

/* ---- test harness ----------------------------------------------------
 * OPENPETE_S3_ACTOR_TEST=1: every canonical tick with the native
 * renderer live, draw the hover cycle next to the live player —
 * decode/projection proof without any mod plumbing. */
void spyro1_s3_actor_test_emit(CPUState* cpu) {
    static int s_on = -1;
    if (s_on < 0) { const char* e = getenv("OPENPETE_S3_ACTOR_TEST"); s_on = (e && *e && *e != '0'); }
    if (!s_on) return;
    spyro1_s3_actor_pose_t p;
    memset(&p, 0, sizeof p);
    p.magic = SPYRO1_S3_POSE_MAGIC;
    p.version = SPYRO1_S3_POSE_VERSION;
    p.pos[0] = (float)(int32_t)cpu->read_word(G_SPYRO + 0x00) + 600.0f;
    p.pos[1] = (float)(int32_t)cpu->read_word(G_SPYRO + 0x04);
    p.pos[2] = (float)(int32_t)cpu->read_word(G_SPYRO + 0x08) + 200.0f;
    p.rot  = cpu->read_word(G_SPYRO + 0x0C);
    { const char* e = getenv("OPENPETE_S3_ACTOR_ANIM");
      p.anim = (e && *e) ? (uint8_t)strtoul(e, NULL, 0) : SPYRO1_S3_ANIM_HOVER; }
    unsigned t = openpete_frame / 4u;   /* ~7.5 fps keyframe cadence */
    p.frame_a = (uint8_t)(t % 8u);
    p.frame_b = (uint8_t)((t + 1u) % 8u);
    p.blend   = (uint8_t)((openpete_frame % 4u) * 64u);
    spyro1_s3_actor_emit(cpu, &p, (int)sizeof p, 0xFFFFFFFFu);
}
