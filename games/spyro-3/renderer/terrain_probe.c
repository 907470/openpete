// games/spyro-3/renderer/terrain_probe.c — the calibrated terrain probe.
//
// Hooks the world draw, runs it once, brackets the primitives it emitted
// between the polybuf cursor before and after, walks them by OT tag length,
// decodes the same tick's visible sectors through game_layout and the world
// draw's own vertex arithmetic, projects them as the draw's RTPS does, and
// reports the fraction of emitted polygon vertices that land on a projected
// sector vertex within a tolerance. The visibility table lives in the
// scratchpad only between func_8002D0D8 returning and the draw's walk, so
// it is captured by a hook on that function while the world draw runs.
//
// Read-only: no store into PSX RAM. Inert unless OPENPETE_TERRAIN_PROBE is
// set (1 = summary at exit, 2 = one line per tick as well);
// OPENPETE_TERRAIN_PROBE_TOL sets the match tolerance in pixels (default 1.5).
//
// The world draw's arithmetic (environment.s, the HP loop at 80022E74):
//   sector header: w0 centre {Y lo16, X hi16}, w1 {radius|flags lo16, Z hi16},
//   w2 xyPos {Y lo16, X hi16}, w3 zPos (Z in bits 14..), w4 LP counts
//   {verts, colours, faces, -}, w5 HP counts {verts, colours, faces,
//   word offset of the HP data}, w6 terminator, data from +0x1C.
//   quarter-unit world position of a packed vertex v:
//     X = (xyPos >> 14) + ((v >> 19) & 0x1FFC)
//     Y = ((xyPos & 0xFFFF) << 2) + ((v >> 8) & 0x1FFC)
//     Z = (zPos >> 14) + ((v << 3) & 0x1FFC)
//   GTE input (VX, VY, VZ) = (camY/4 - Y, camZ/4 - Z, X - camX/4), rotation
//   from g_Camera+0x00, translation 0, then RTPS; SXY2 is the emitted XY.
// The Spyro 1 packing puts Z at (v << 2) & 0xFFC; the probe also projects
// with that packing as the control that one decoder switch is what separates
// the two formats.

#include "feature_registry.h"
#include "game_layout.h"
#include "psx_runtime_internal.h"
#include "openpete_fiber.h"      /* openpete_frame */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void func_80022378(CPUState* cpu);   /* the world draw */
extern void func_8002D0D8(CPUState* cpu);   /* the visibility computation */

/* ---- knobs -------------------------------------------------------------- */
static int    s_mode = -1;      /* 0 off, 1 summary, 2 per tick */
static double s_tol  = 1.5;

static int probe_mode(void) {
    if (s_mode < 0) {
        const char* e = getenv("OPENPETE_TERRAIN_PROBE");
        s_mode = (e && *e) ? atoi(e) : 0;
        const char* t = getenv("OPENPETE_TERRAIN_PROBE_TOL");
        if (t && *t) s_tol = atof(t);
    }
    return s_mode;
}

/* ---- the visibility capture ---------------------------------------------- */
#define VIS_MAX 1024
static int     s_capture_armed;
static uint8_t s_vis[VIS_MAX];
static int     s_vis_n;

static int on_visibility(CPUState* cpu, uint32_t addr) {
    (void)addr;
    if (!s_capture_armed || cpu->a0 != 0u) return 0;   /* only the world call inside the draw */
    static int s_in = 0; if (s_in) return 0; s_in = 1;
    func_8002D0D8(cpu);
    s_in = 0;
    uint32_t n = cpu->read_word(game_layout.env.sector_count);
    if (n > VIS_MAX) n = VIS_MAX;
    for (uint32_t i = 0; i < n; i++)
        s_vis[i] = cpu->read_byte(game_layout.visibility.base + i);
    s_vis_n = (int)n;
    s_capture_armed = 0;
    return 1;
}

/* ---- projected sector vertices, hashed on a screen grid ------------------ */
#define PV_MAX   65536
#define GRID_W   256          /* cells of 8 px over [-1024, 1024) */
#define GRID_N   (GRID_W * GRID_W)
typedef struct { float sx, sy, z; uint8_t hp; uint8_t s1z; uint16_t sec; int next; } pvert_t;
static pvert_t s_pv[PV_MAX];
static int     s_pv_n;
static int     s_grid[GRID_N];

static void pv_reset(void) { s_pv_n = 0; for (int i = 0; i < GRID_N; i++) s_grid[i] = -1; }
static int  pv_cell(double sx, double sy) {
    int cx = (int)floor((sx + 1024.0) / 8.0), cy = (int)floor((sy + 1024.0) / 8.0);
    if (cx < 0 || cy < 0 || cx >= GRID_W || cy >= GRID_W) return -1;
    return cy * GRID_W + cx;
}
static void pv_add(double sx, double sy, double z, int hp, int s1z, uint32_t sec) {
    if (s_pv_n >= PV_MAX) return;
    int c = pv_cell(sx, sy); if (c < 0) return;
    pvert_t* v = &s_pv[s_pv_n];
    v->sx = (float)sx; v->sy = (float)sy; v->hp = (uint8_t)hp; v->s1z = (uint8_t)s1z; v->sec = (uint16_t)sec; v->z = (float)z;
    v->next = s_grid[c]; s_grid[c] = s_pv_n++;
}
/* Nearest projected vertex within tol; bits: 1 = an S3-packed vertex matched,
 * 2 = it was HP, 4 = it was LP, 8 = an S1-packed (control) vertex matched.
 * *near receives the nearest S3-packed vertex distance within the searched
 * cells (up to `reach` cells each way), or a large value. */
static double s_near_x, s_near_y;   /* the nearest vertex of the last pv_match */
static int pv_match(double sx, double sy, double tol, int reach, double* near, int* near_sec, double* near_z) {
    int cx = (int)floor((sx + 1024.0) / 8.0), cy = (int)floor((sy + 1024.0) / 8.0);
    int r = 0; double best = 1e9; *near_sec = -1; *near_z = 0.0; s_near_x = s_near_y = 0.0;
    for (int dy = -reach; dy <= reach; dy++) for (int dx = -reach; dx <= reach; dx++) {
        int x = cx + dx, y = cy + dy;
        if (x < 0 || y < 0 || x >= GRID_W || y >= GRID_W) continue;
        for (int i = s_grid[y * GRID_W + x]; i >= 0; i = s_pv[i].next) {
            double ex = s_pv[i].sx - sx, ey = s_pv[i].sy - sy, d2 = ex * ex + ey * ey;
            if (!s_pv[i].s1z && d2 < best) { best = d2; *near_sec = s_pv[i].sec; *near_z = s_pv[i].z; s_near_x = s_pv[i].sx; s_near_y = s_pv[i].sy; }
            if (d2 > tol * tol) continue;
            if (s_pv[i].s1z) r |= 8;
            else r |= 1 | (s_pv[i].hp ? 2 : 4);
        }
    }
    *near = sqrt(best);
    return r;
}

/* 1 when (sx, sy) is within tol of the midpoint of two projected S3-packed
 * vertices of the same sector within 96 px: the signature of a subdivided
 * face. */
static int pv_midpoint(double sx, double sy, double tol) {
    int cx = (int)floor((sx + 1024.0) / 8.0), cy = (int)floor((sy + 1024.0) / 8.0);
    int cand[512]; int n = 0;
    for (int dy = -12; dy <= 12 && n < 512; dy++) for (int dx = -12; dx <= 12 && n < 512; dx++) {
        int x = cx + dx, y = cy + dy;
        if (x < 0 || y < 0 || x >= GRID_W || y >= GRID_W) continue;
        for (int i = s_grid[y * GRID_W + x]; i >= 0 && n < 512; i = s_pv[i].next)
            if (!s_pv[i].s1z) cand[n++] = i;
    }
    int r = 0;
    for (int a = 0; a < n; a++) for (int b = a + 1; b < n; b++) {
        double ax = s_pv[cand[a]].sx, ay = s_pv[cand[a]].sy, bx = s_pv[cand[b]].sx, by = s_pv[cand[b]].sy;
        double mx = 0.5 * (ax + bx) - sx, my = 0.5 * (ay + by) - sy;
        if (mx * mx + my * my <= tol * tol) r |= (s_pv[cand[a]].sec == s_pv[cand[b]].sec) ? 1 : 2;
        /* on the segment ab, strictly between the endpoints */
        double ex = bx - ax, ey = by - ay, l2 = ex * ex + ey * ey;
        if (l2 < 16.0) continue;
        double t = ((sx - ax) * ex + (sy - ay) * ey) / l2;
        if (t <= 0.02 || t >= 0.98) continue;
        double px = ax + t * ex - sx, py = ay + t * ey - sy;
        if (px * px + py * py <= tol * tol) r |= (s_pv[cand[a]].sec == s_pv[cand[b]].sec) ? 4 : 8;
    }
    return r;
}

/* ---- emitted vertices, hashed the same way (the reverse direction) ------- */
typedef struct { float sx, sy; int next; } evert_t;
static evert_t s_ev[PV_MAX];
static int     s_ev_n;
static int     s_egrid[GRID_N];
static void ev_reset(void) { s_ev_n = 0; for (int i = 0; i < GRID_N; i++) s_egrid[i] = -1; }
static void ev_add(double sx, double sy) {
    if (s_ev_n >= PV_MAX) return;
    int c = pv_cell(sx, sy); if (c < 0) return;
    s_ev[s_ev_n].sx = (float)sx; s_ev[s_ev_n].sy = (float)sy;
    s_ev[s_ev_n].next = s_egrid[c]; s_egrid[c] = s_ev_n++;
}
static int ev_near(double sx, double sy, double tol) {
    int cx = (int)floor((sx + 1024.0) / 8.0), cy = (int)floor((sy + 1024.0) / 8.0);
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
        int x = cx + dx, y = cy + dy;
        if (x < 0 || y < 0 || x >= GRID_W || y >= GRID_W) continue;
        for (int i = s_egrid[y * GRID_W + x]; i >= 0; i = s_ev[i].next) {
            double ex = s_ev[i].sx - sx, ey = s_ev[i].sy - sy;
            if (ex * ex + ey * ey <= tol * tol) return 1;
        }
    }
    return 0;
}

/* ---- the draw's projection --------------------------------------------- */
typedef struct { int M[3][3]; double ofx, ofy, H; int32_t cx4, cy4, cz4; } view_t;

static void view_read(CPUState* cpu, view_t* v) {
    uint32_t w[5];
    for (int i = 0; i < 5; i++) w[i] = cpu->read_word(game_layout.cam.proj_matrix + 4u * (uint32_t)i);
    v->M[0][0] = (int16_t)(w[0] & 0xFFFF); v->M[0][1] = (int16_t)(w[0] >> 16);
    v->M[0][2] = (int16_t)(w[1] & 0xFFFF); v->M[1][0] = (int16_t)(w[1] >> 16);
    v->M[1][1] = (int16_t)(w[2] & 0xFFFF); v->M[1][2] = (int16_t)(w[2] >> 16);
    v->M[2][0] = (int16_t)(w[3] & 0xFFFF); v->M[2][1] = (int16_t)(w[3] >> 16);
    v->M[2][2] = (int16_t)(w[4] & 0xFFFF);
    v->ofx = (double)(int32_t)cpu->gte_ctrl[24] / 65536.0;
    v->ofy = (double)(int32_t)cpu->gte_ctrl[25] / 65536.0;
    v->H   = (double)(uint16_t)(cpu->gte_ctrl[26] & 0xFFFF);
    v->cx4 = (int32_t)cpu->read_word(game_layout.cam.position)      >> 2;
    v->cy4 = (int32_t)cpu->read_word(game_layout.cam.position + 4u) >> 2;
    v->cz4 = (int32_t)cpu->read_word(game_layout.cam.position + 8u) >> 2;
}

/* RTPS as the GTE computes it: IR saturation, SZ3 clamp, the divide cap at
 * H/2. Returns 0 when the vertex is behind the camera. */
static int view_project(const view_t* v, int32_t X, int32_t Y, int32_t Z, double* sx, double* sy, double* z) {
    double vx = (double)(v->cy4 - Y), vy = (double)(v->cz4 - Z), vz = (double)(X - v->cx4);
    double m1 = (v->M[0][0]*vx + v->M[0][1]*vy + v->M[0][2]*vz) / 4096.0;
    double m2 = (v->M[1][0]*vx + v->M[1][1]*vy + v->M[1][2]*vz) / 4096.0;
    double m3 = (v->M[2][0]*vx + v->M[2][1]*vy + v->M[2][2]*vz) / 4096.0;
    if (m3 <= 0.0) return 0;
    *z = m3;
    double ir1 = m1 < -32768.0 ? -32768.0 : (m1 > 32767.0 ? 32767.0 : m1);
    double ir2 = m2 < -32768.0 ? -32768.0 : (m2 > 32767.0 ? 32767.0 : m2);
    double sz3 = m3 > 65535.0 ? 65535.0 : m3;
    double q = (v->H >= 2.0 * sz3) ? (131071.0 / 65536.0) : (v->H / sz3);
    *sx = v->ofx + ir1 * q;
    *sy = v->ofy + ir2 * q;
    return 1;
}

/* ---- per-run statistics -------------------------------------------------- */
static struct {
    unsigned long ticks, vacuous;
    unsigned long sectors_max, visible_sum, visible_max;
    unsigned long prims, polys, verts, matched, hp, lp, both, control;
    unsigned long code_hist[64];
    unsigned long flat_sectors;
    unsigned long polys_full;             /* every vertex matched */
    unsigned long miss_le4, miss_le16, miss_le64, miss_far;   /* unmatched: nearest distance */
    unsigned long sat_verts;              /* unmatched vertices on the SXY bound */
    unsigned long miss_onscreen;          /* unmatched vertices inside the 512x240 frame */
    unsigned long mid_sampled, mid_hit, mid_cross, seg_same, seg_cross;
    unsigned long miss_z_near, miss_z_mid, miss_z_far;   /* unmatched by the nearest vertex's view-Z: <0x100, <0x600, else */   /* every 8th unmatched vertex: lies on an edge midpoint of two projected vertices */
    unsigned long code_verts[64], code_matched[64];
    unsigned long sec_miss[VIS_MAX];      /* unmatched, by the nearest vertex's sector (the last level's count) */
    uint32_t sec_miss_count;              /* the sector count sec_miss[] belongs to */
    unsigned long sec_hp_v[VIS_MAX], sec_hp_hit[VIS_MAX], sec_lp_v[VIS_MAX], sec_lp_hit[VIS_MAX];   /* reverse: decoded vertices with an emitted vertex within tol */
    unsigned long rev_hp_v, rev_hp_hit, rev_lp_v, rev_lp_hit;
    int printed;
} S;

static void probe_report(void) {
    if (S.printed || probe_mode() == 0) return;
    S.printed = 1;
    if (S.ticks == 0 || S.polys == 0 || S.visible_sum == 0) {
        fprintf(stderr, "[terrain-probe] VACUOUS: ticks=%lu polys=%lu visible=%lu\n",
                S.ticks, S.polys, S.visible_sum);
        return;
    }
    fprintf(stderr,
        "[terrain-probe] ticks=%lu sectors max=%lu visible avg=%.1f max=%lu flat=%lu | prims=%lu polys=%lu "
        "verts=%lu matched=%lu (%.2f%%, tol=%.2f px) hp-only=%lu lp-only=%lu both=%lu | control(S1 Z packing)=%lu (%.2f%%)\n",
        S.ticks, S.sectors_max, (double)S.visible_sum / (double)S.ticks, S.visible_max, S.flat_sectors,
        S.prims, S.polys, S.verts, S.matched, 100.0 * (double)S.matched / (double)S.verts, s_tol,
        S.hp, S.lp, S.both, S.control, 100.0 * (double)S.control / (double)S.verts);
    fprintf(stderr, "[terrain-probe] polys fully matched=%lu (%.2f%%) | unmatched nearest: <=4px %lu, <=16px %lu, <=64px %lu, farther %lu, on SXY bound %lu\n",
        S.polys_full, 100.0 * (double)S.polys_full / (double)S.polys, S.miss_le4, S.miss_le16, S.miss_le64, S.miss_far, S.sat_verts);
    fprintf(stderr, "[terrain-probe] unmatched inside the 512x240 frame=%lu of %lu; sampled %lu unmatched: same-sector midpoint %lu, cross-sector midpoint %lu, on a same-sector segment %lu, on a cross-sector segment %lu; by nearest view-Z: <0x100 %lu, <0x600 %lu, farther %lu\n",
        S.miss_onscreen, S.verts - S.matched, S.mid_sampled, S.mid_hit, S.mid_cross, S.seg_same, S.seg_cross, S.miss_z_near, S.miss_z_mid, S.miss_z_far);
    fprintf(stderr, "[terrain-probe] codes:");
    for (int c = 0; c < 64; c++) if (S.code_hist[c])
        fprintf(stderr, " %02X:%lu(%.1f%%)", c, S.code_hist[c], S.code_verts[c] ? 100.0 * (double)S.code_matched[c] / (double)S.code_verts[c] : 0.0);
    fprintf(stderr, "\n[terrain-probe] reverse: decoded HP verts with an emitted vertex within tol %lu of %lu (%.1f%%), LP %lu of %lu (%.1f%%)\n",
        S.rev_hp_hit, S.rev_hp_v, S.rev_hp_v ? 100.0 * (double)S.rev_hp_hit / (double)S.rev_hp_v : 0.0,
        S.rev_lp_hit, S.rev_lp_v, S.rev_lp_v ? 100.0 * (double)S.rev_lp_hit / (double)S.rev_lp_v : 0.0);
    fprintf(stderr, "[terrain-probe] worst sectors by unmatched-nearest (sector count %u), with their reverse HP/LP coverage:", S.sec_miss_count);
    for (int n = 0; n < 8; n++) {
        unsigned long best = 0; int bi = -1;
        for (uint32_t i = 0; i < S.sec_miss_count && i < VIS_MAX; i++) if (S.sec_miss[i] > best) { best = S.sec_miss[i]; bi = (int)i; }
        if (bi < 0) break;
        fprintf(stderr, " %d:%lu(hp %.0f%%, lp %.0f%%)", bi, best,
                S.sec_hp_v[bi] ? 100.0 * (double)S.sec_hp_hit[bi] / (double)S.sec_hp_v[bi] : 0.0,
                S.sec_lp_v[bi] ? 100.0 * (double)S.sec_lp_hit[bi] / (double)S.sec_lp_v[bi] : 0.0);
        S.sec_miss[bi] = 0;
    }
    fprintf(stderr, "\n");
}

/* ---- the world-draw hook ------------------------------------------------ */
static int on_world_draw(CPUState* cpu, uint32_t addr) {
    (void)addr;
    int mode = probe_mode();
    if (!mode) return 0;
    static int s_in = 0; if (s_in) return 0; s_in = 1;
    static int s_atexit = 0; if (!s_atexit) { atexit(probe_report); s_atexit = 1; }

    view_t view; view_read(cpu, &view);
    s_vis_n = 0; s_capture_armed = 1;
    uint32_t startp = cpu->read_word(game_layout.polybuf.cursor);
    func_80022378(cpu);
    uint32_t endp = cpu->read_word(game_layout.polybuf.cursor);
    s_capture_armed = 0;

    uint32_t sectorPtr = cpu->read_word(game_layout.env.sector_ptr);
    uint32_t sectorCnt = cpu->read_word(game_layout.env.sector_count);
    if (sectorCnt > S.sectors_max) S.sectors_max = sectorCnt;
    if (sectorCnt != S.sec_miss_count) {
        memset(S.sec_miss, 0, sizeof S.sec_miss); S.sec_miss_count = sectorCnt;
        memset(S.sec_hp_v, 0, sizeof S.sec_hp_v); memset(S.sec_hp_hit, 0, sizeof S.sec_hp_hit);
        memset(S.sec_lp_v, 0, sizeof S.sec_lp_v); memset(S.sec_lp_hit, 0, sizeof S.sec_lp_hit);
    }
    static unsigned s_dump_vbl = 0xFFFFFFFFu;
    if (s_dump_vbl == 0xFFFFFFFFu) { const char* e = getenv("OPENPETE_TERRAIN_PROBE_DUMP_VBL"); s_dump_vbl = e ? (unsigned)strtoul(e, NULL, 0) : 0u; }
    FILE* dump = (s_dump_vbl && openpete_frame >= s_dump_vbl && openpete_frame < s_dump_vbl + 2u) ? fopen("terrain-probe-dump.txt", "a") : NULL;

    /* Decode and project every visible sector's LP and HP vertices. */
    pv_reset(); ev_reset();
    unsigned long visible = 0, sumLpF = 0, sumHpF = 0;
    for (uint32_t i = 0; i < sectorCnt && i < (uint32_t)s_vis_n; i++) {
        if (!s_vis[i]) continue;
        visible++;
        uint32_t sec = cpu->read_word(sectorPtr + 4u * i);
        if (sec < 0x80000000u || sec >= 0x80200000u) continue;
        uint32_t w1 = cpu->read_word(sec + 0x04u);
        uint32_t xy = cpu->read_word(sec + 0x08u), zp = cpu->read_word(sec + 0x0Cu);
        uint32_t w4 = cpu->read_word(sec + 0x10u), w5 = cpu->read_word(sec + 0x14u);
        if ((w1 >> 12) & 1u) S.flat_sectors++;
        int32_t bx = (int32_t)(xy >> 14), by = (int32_t)((xy & 0xFFFFu) << 2), bz = (int32_t)(zp >> 14);
        uint32_t nLp = w4 & 0xFFu, nHp = w5 & 0xFFu, hpOff = w5 >> 24;
        sumLpF += (w4 >> 16) & 0xFFu; sumHpF += (w5 >> 16) & 0xFFu;
        uint32_t lists[2][2] = { { sec + 0x1Cu, nLp }, { sec + 0x1Cu + 4u * hpOff, nHp } };
        for (int hp = 0; hp < 2; hp++) {
            for (uint32_t k = 0; k < lists[hp][1]; k++) {
                uint32_t v = cpu->read_word(lists[hp][0] + 4u * k);
                int32_t X = bx + (int32_t)((v >> 19) & 0x1FFCu);
                int32_t Y = by + (int32_t)((v >> 8) & 0x1FFCu);
                int32_t Z = bz + (int32_t)((v << 3) & 0x1FFCu);
                int32_t Z1 = bz + (int32_t)((v << 2) & 0xFFCu);   /* the Spyro 1 packing, control */
                double sx, sy, z;
                if (view_project(&view, X, Y, Z, &sx, &sy, &z)) pv_add(sx, sy, z, hp, 0, i);
                if (Z1 != Z && view_project(&view, X, Y, Z1, &sx, &sy, &z)) pv_add(sx, sy, z, hp, 1, i);
            }
        }
    }

    /* Walk the emitted primitives and match each polygon vertex. */
    unsigned long prims = 0, polys = 0, verts = 0, matched = 0, control = 0;
    uint32_t p = startp;
    int guard = 0;
    while (p < endp && guard++ < 200000) {
        uint32_t w0 = cpu->read_word(p);
        uint32_t len = (w0 >> 24) & 0xFFu;
        if (len == 0 || len > 24) break;
        uint32_t stride = (len + 1u) * 4u;
        uint8_t code = (uint8_t)(cpu->read_word(p + 4u) >> 24);
        prims++;
        if ((code & 0xE0) == 0x20) {
            S.code_hist[code & 0x3F]++;
            polys++;
            int gouraud = (code & 0x10) != 0, quad = (code & 0x08) != 0, tex = (code & 0x04) != 0;
            int nv = quad ? 4 : 3;
            uint32_t c = 2;
            int full = 1;
            for (int k = 0; k < nv; k++) {
                if (gouraud && k > 0) c++;
                uint32_t xyw = cpu->read_word(p + 4u * c); c++;
                if (tex) c++;
                double sx = (double)(int16_t)(xyw & 0xFFFFu), sy = (double)(int16_t)(xyw >> 16);
                double near, near_z; int near_sec;
                ev_add(sx, sy);
                int r = pv_match(sx, sy, s_tol, 8, &near, &near_sec, &near_z);
                if (dump && !(r & 1)) fprintf(dump, "miss vbl=%u code=%02X k=%d sx=%.0f sy=%.0f near=%.1f sec=%d z=%.0f nx=%.1f ny=%.1f dx=%.1f dy=%.1f\n", openpete_frame, code, k, sx, sy, near, near_sec, near_z, s_near_x, s_near_y, sx - s_near_x, sy - s_near_y);
                verts++; S.code_verts[code & 0x3F]++;
                if (r & 1) { matched++; S.code_matched[code & 0x3F]++; if ((r & 6) == 6) S.both++; else if (r & 2) S.hp++; else S.lp++; }
                else {
                    full = 0;
                    if (sx >= 0.0 && sx < 512.0 && sy >= 0.0 && sy < 240.0) S.miss_onscreen++;
                    if (near_sec >= 0 && near_sec < VIS_MAX) S.sec_miss[near_sec]++;
                    if (near_z < 256.0) S.miss_z_near++; else if (near_z < 1536.0) S.miss_z_mid++; else S.miss_z_far++;
                    if (((S.verts + verts) & 7u) == 0u) {
                        S.mid_sampled++; int m = pv_midpoint(sx, sy, s_tol);
                        if (m & 1) S.mid_hit++; if (m & 2) S.mid_cross++; if (m & 4) S.seg_same++; if (m & 8) S.seg_cross++;
                    }
                    if (near <= 4.0) S.miss_le4++; else if (near <= 16.0) S.miss_le16++;
                    else if (near <= 64.0) S.miss_le64++; else S.miss_far++;
                    if (sx <= -1024.0 || sx >= 1023.0 || sy <= -1024.0 || sy >= 1023.0) S.sat_verts++;
                }
                if (r & 8) control++;
            }
            if (full) S.polys_full++;
        }
        p += stride;
    }

    /* Reverse direction: each decoded vertex against the emitted set. */
    for (int i = 0; i < s_pv_n; i++) {
        if (s_pv[i].s1z) continue;
        int hit = ev_near(s_pv[i].sx, s_pv[i].sy, s_tol);
        unsigned sec = s_pv[i].sec;
        if (s_pv[i].hp) { S.rev_hp_v++; S.sec_hp_v[sec]++; if (hit) { S.rev_hp_hit++; S.sec_hp_hit[sec]++; } }
        else            { S.rev_lp_v++; S.sec_lp_v[sec]++; if (hit) { S.rev_lp_hit++; S.sec_lp_hit[sec]++; } }
        if (dump && !hit) fprintf(dump, "rev-miss vbl=%u sec=%u hp=%d sx=%.1f sy=%.1f z=%.0f\n", openpete_frame, sec, s_pv[i].hp, s_pv[i].sx, s_pv[i].sy, s_pv[i].z);
    }
    if (dump) fclose(dump);
    S.ticks++;
    S.visible_sum += visible; if (visible > S.visible_max) S.visible_max = visible;
    S.prims += prims; S.polys += polys; S.verts += verts; S.matched += matched; S.control += control;
    if (polys == 0 || visible == 0) S.vacuous++;
    if (mode >= 2)
        fprintf(stderr, "[terrain-probe] vbl=%u sectors=%u visible=%lu faces(lp=%lu hp=%lu) prims=%lu polys=%lu verts=%lu matched=%lu (%.1f%%) control=%lu\n",
                openpete_frame, sectorCnt, visible, sumLpF, sumHpF, prims, polys, verts, matched,
                verts ? 100.0 * (double)matched / (double)verts : 0.0, control);
    s_in = 0;
    return 1;
}

/* Hook addresses are the layout's, bound at boot: a static initializer
 * cannot read another TU's const. */
static feature_dispatch_entry_t probe_dispatch[] = {
    { 0u, on_world_draw },
    { 0u, on_visibility },
    { 0u, NULL },
};

static void probe_apply_options(const openpete_options_t* opts) {
    (void)opts;
    probe_dispatch[0].addr = game_layout.hooks.world_draw;
    probe_dispatch[1].addr = game_layout.hooks.visibility;
}

const feature_module_t terrain_probe_module = {
    .name           = "terrain_probe",
    .apply_options  = probe_apply_options,
    .dispatch_table = probe_dispatch,
};
