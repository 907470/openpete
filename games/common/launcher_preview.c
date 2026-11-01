/* games/common/launcher_preview.c - see launcher_preview.h. */
#include "launcher_preview.h"

#include "freecam_game.h"
#include "game_build.h"
#include "game_host_desc.h"
#include "launcher_assets.h"
#include "openpete_disc_image.h"
#include "openpete_disc_trace.h"
#include "openpete_launcher.h"
#include "openpete_log.h"
#include "openpete_native_render.h"
#include "renderer/native_gte.h"   /* NGTE_Y_ROW */
#include "openpete_terrain_cache.h"
#include "openpete_time.h"
#include "game_layout.h"
#include "renderer/terrain.h"
#include "renderer/wad_subfile.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Each game's box picture: its homeworld, the level the box shows (the
 * three-box page's picture), at a free-camera pose that frames the hub.
 * The rows are the one place the level and pose are chosen. */
static const launcher_preview_spec k_preview_specs[] = {
    {"spyro-1", "artisans",       "pos=(95724, 59413, 11734) yaw=2597 pitch=-32"},
    {"spyro-2", "summer_forest",  "pos=(104985, 78616, 41379) yaw=1333 pitch=-85"},
    {"spyro-3", "sunrise_spring", "pos=(40739, 15058, 23398) yaw=933 pitch=-3"},
};

/* The environment a game runs its levels with, which the disc's level data
 * does not hold: the LOD and LP far-cull distances the game writes at run
 * time, the GTE projection the walk reads, and the rows its display shows.
 * Each value is the one the in-game walk publishes in gameplay at 4:3. */
typedef struct {
    const char* game;
    uint32_t lod_dist, cull_dist;
    double H, ofx, ofy;
    float clip_h, band;   /* the displayed rows: clip_h rows from row band */
} launcher_preview_env;

static const launcher_preview_env k_preview_env[] = {
    /* Spyro 1: m_LodDistance 0x8000 (initialization.c), m_CullingDistance
     * 0x28000 with the camera in an occlusion group (environment.c). */
    {"spyro-1", 0x8000u, 0x28000u, 341.0, 256.0, 120.0, 224.0f, 8.0f},
    /* Spyro 2/3: the LOD bins are the layout's constant (lod_dist_default);
     * the far cull 0x2C000 (environment +0x2C). */
    {"spyro-2", 0x7C00u, 0x2C000u, 341.0, 256.0, 120.0, 216.0f, 12.0f},
    {"spyro-3", 0x7C00u, 0x2C000u, 341.0, 256.0, 120.0, 216.0f, 12.0f},
};

/** @brief Reads a game's row of the harness variable OPENPETE_PREVIEW_VIEW ("<game>=<level>|<pose>", rows separated by ';'), the form a pose is tried in without a rebuild.
 * @param game The game id.
 * @param out Receives the row's level and pose, pointing into @p buf.
 * @param buf The row's text.
 * @param cap Its capacity.
 * @return 1 when the variable holds a row for the game. */
static int lp_view_override(const char* game, launcher_preview_spec* out, char* buf, size_t cap) {
    const char* e = getenv("OPENPETE_PREVIEW_VIEW");
    const size_t gn = strlen(game);
    for (const char* p = e; p && *p; p = strchr(p, ';') ? strchr(p, ';') + 1 : NULL) {
        if (strncmp(p, game, gn) != 0 || p[gn] != '=') continue;
        const char* end = strchr(p, ';');
        snprintf(buf, cap, "%.*s", (int)(end ? (size_t)(end - p - gn - 1) : strlen(p + gn + 1)), p + gn + 1);
        char* bar = strchr(buf, '|');
        if (!bar) return 0;
        *bar = 0;
        out->game = game;
        out->level = buf;
        out->pose = bar + 1;
        return 1;
    }
    return 0;
}

const launcher_preview_spec* launcher_preview_spec_of(const char* game) {
    static _Thread_local launcher_preview_spec over;
    static _Thread_local char buf[256];
    if (game && lp_view_override(game, &over, buf, sizeof buf)) return &over;
    for (size_t i = 0; game && i < sizeof k_preview_specs / sizeof k_preview_specs[0]; i++)
        if (!strcmp(k_preview_specs[i].game, game)) return &k_preview_specs[i];
    return NULL;
}

/** @brief Returns a game's preview environment row, or NULL.
 * @param game The game id.
 * @return The row. */
static const launcher_preview_env* lp_env_of(const char* game) {
    for (size_t i = 0; game && i < sizeof k_preview_env / sizeof k_preview_env[0]; i++)
        if (!strcmp(k_preview_env[i].game, game)) return &k_preview_env[i];
    return NULL;
}

int launcher_preview_size(const char* game, uint32_t* w, uint32_t* h) {
    const launcher_preview_env* E = lp_env_of(game);
    if (!E) return -1;
    *w = LAUNCHER_PREVIEW_W;
    *h = (uint32_t)lroundf(2.0f * E->clip_h);
    return 0;
}

/** @brief Writes a formatted failure reason.
 * @param why The buffer.
 * @param cap Its capacity.
 * @param fmt The format.
 * @param ... Its arguments.
 * @return -1. */
static int lp_fail(char* why, size_t cap, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (why && cap) vsnprintf(why, cap, fmt, ap);
    va_end(ap);
    return -1;
}

/** @brief Reads a little-endian word of a host buffer, 0 when it does not fit.
 * @param b The buffer.
 * @param n Its length.
 * @param o The offset.
 * @return The word. */
static uint32_t lp_w(const uint8_t* b, uint32_t n, uint32_t o) {
    if (o > n || n - o < 4u) return 0u;
    return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) | ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24);
}

/** @brief Places a 512 x 512 VRAM image (1024-byte rows) at x 512, rows 0-511 of a 1024 x 512 PSX image.
 * @param src The image bytes.
 * @param n Their count; at least 0x80000.
 * @return The 1024 x 512 image, or NULL on an allocation failure. */
static uint16_t* lp_vram(const uint8_t* src, uint32_t n) {
    if (n < 0x80000u) return NULL;
    uint16_t* v = (uint16_t*)calloc(1024u * 512u, sizeof(uint16_t));
    if (!v) return NULL;
    for (uint32_t y = 0; y < 512u; y++)
        for (uint32_t x = 0; x < 512u; x++) {
            const uint8_t* p = src + y * 1024u + 2u * x;
            v[y * 1024u + 512u + x] = (uint16_t)(p[0] | (p[1] << 8));
        }
    return v;
}

/** @brief Fills the sector offsets of a sector block whose offsets are relative to a base.
 * @param L The level; bytes and len set.
 * @param count_at The count word's offset.
 * @param offs_at The first offset's byte offset.
 * @param base The offsets' base.
 * @param why Receives the failure's reason.
 * @param cap Its capacity.
 * @return 0, or -1. */
static int lp_sectors(launcher_preview_level* L, uint32_t count_at, uint32_t offs_at, uint32_t base,
                      char* why, size_t cap) {
    const uint32_t n = lp_w(L->bytes, L->len, count_at);
    if (n == 0 || n > 1024u || offs_at > L->len || (L->len - offs_at) / 4u < n)
        return lp_fail(why, cap, "sector table: %u sectors", n);
    L->sector_off = (uint32_t*)malloc(sizeof(uint32_t) * n);
    if (!L->sector_off) return lp_fail(why, cap, "sector table allocation");
    for (uint32_t k = 0; k < n; k++) {
        const uint32_t o = base + lp_w(L->bytes, L->len, offs_at + 4u * k);
        if (o > L->len || L->len - o < 0x1Cu) return lp_fail(why, cap, "sector %u outside the level data", k);
        L->sector_off[k] = o;
    }
    L->sector_count = (int)n;
    return 0;
}

/** @brief Reads a Spyro 1 level: the level header's VRAM image and the texture and environment components of its data.
 * @param img The disc.
 * @param e The WAD entry.
 * @param L Receives the level.
 * @param why Receives the failure's reason.
 * @param cap Its capacity.
 * @return 0, or -1. */
static int lp_load_header(disc_image_t* img, int e, launcher_preview_level* L, char* why, size_t cap) {
    uint32_t hn = 0, vn = 0;
    uint8_t* hdr = wad_disc_entry_load(img, e, 0u, 0x800u, &hn, "launcher");
    if (!hdr) return lp_fail(why, cap, "WAD entry %d header unreadable", e);
    const uint32_t voff = lp_w(hdr, hn, 0u), doff = lp_w(hdr, hn, 8u), dsz = lp_w(hdr, hn, 12u);
    free(hdr);
    uint8_t* vr = wad_disc_entry_load(img, e, voff, 0x80000u, &vn, "launcher");
    L->vram = vr ? lp_vram(vr, vn) : NULL;
    free(vr);
    if (!L->vram) return lp_fail(why, cap, "WAD entry %d: VRAM image unreadable", e);
    L->bytes = dsz ? wad_disc_entry_load(img, e, doff, dsz, &L->len, "launcher") : NULL;
    if (!L->bytes) return lp_fail(why, cap, "WAD entry %d: level data unreadable", e);
    const uint32_t tsz = lp_w(L->bytes, L->len, 0u), cnt = lp_w(L->bytes, L->len, 4u);
    if (cnt == 0 || cnt > 128u || tsz < 8u + 16u * cnt || tsz > L->len)
        return lp_fail(why, cap, "WAD entry %d: texture component (%u textures, %u bytes)", e, cnt, tsz);
    L->tex_count = cnt;
    L->lq_off = 8u;
    L->hq_off = 8u + 16u * cnt;
    /* The environment component: {size, count, offsets relative to its
     * second word} (LoadLevelData's PATCH_POINTER_RELATIVE_TO_COMPONENT). */
    return lp_sectors(L, tsz + 4u, tsz + 8u, tsz + 4u, why, cap);
}

/** @brief Reads a Spyro 2/3 level: sub-file 1's texture and sector blocks and sub-file 0's VRAM image.
 * @param img The disc.
 * @param e The WAD entry.
 * @param L Receives the level.
 * @param why Receives the failure's reason.
 * @param cap Its capacity.
 * @return 0, or -1. */
static int lp_load_subfiles(disc_image_t* img, int e, launcher_preview_level* L, char* why, size_t cap) {
    uint32_t vn = 0;
    uint8_t* vr = wad_disc_subfile_load(img, e, 0, &vn, "launcher");
    L->vram = vr ? lp_vram(vr, vn) : NULL;
    free(vr);
    if (!L->vram) return lp_fail(why, cap, "WAD entry %d: VRAM sub-file unreadable", e);
    L->bytes = wad_disc_subfile_load(img, e, 1, &L->len, "launcher");
    if (!L->bytes) return lp_fail(why, cap, "WAD entry %d: terrain sub-file unreadable", e);
    const uint32_t ts = lp_w(L->bytes, L->len, 0u), cnt = lp_w(L->bytes, L->len, 4u);
    if (cnt == 0 || cnt > 128u || ts != 8u + 48u * cnt || ts + 12u > L->len)
        return lp_fail(why, cap, "WAD entry %d: texture block (%u textures, %u bytes)", e, cnt, ts);
    L->tex_count = cnt;
    L->lq_off = L->hq_off = 8u;
    return lp_sectors(L, ts + 8u, ts + 12u, ts + 4u, why, cap);
}

int launcher_preview_level_load(disc_image_t* img, const openpete_game_build_t* b, int number,
                                launcher_preview_level* out, char* why, size_t cap) {
    memset(out, 0, sizeof *out);
    const game_assets_desc_t* A = (b && b->runtime && b->runtime->host) ? b->runtime->host->assets : NULL;
    if (!A) return lp_fail(why, cap, "the game declares no launcher assets");
    int e = 0;
    if (game_assets_level_entry(img, A, number, &e, why, cap) != 0) return -1;
    out->entry = e;
    int rc = A->sky_form == GAME_SKY_LEVEL_HEADER ? lp_load_header(img, e, out, why, cap)
                                                  : lp_load_subfiles(img, e, out, why, cap);
    if (rc == 0) {
        out->exe = game_assets_boot_exe(img, &out->exe_addr, &out->exe_len, why, cap);
        if (!out->exe) rc = -1;
    }
    if (rc != 0) launcher_preview_level_free(out);
    return rc;
}

void launcher_preview_level_free(launcher_preview_level* L) {
    free(L->bytes);
    free(L->sector_off);
    free(L->vram);
    free(L->exe);
    memset(L, 0, sizeof *L);
}

/** @brief Reports whether the harness variable OPENPETE_PREVIEW_FAIL forces this game's preview to fail.
 * @param game The game id.
 * @return 1 when it names the game or is 1. */
static int lp_forced_fail(const char* game) {
    const char* e = getenv("OPENPETE_PREVIEW_FAIL");
    return e && (!strcmp(e, "1") || !strcmp(e, game));
}

/** @brief Reports whether the running job was asked to stop, the draw's stop poll.
 * @return 1 when stopped. */
static int lp_stop(void) { return openpete_launcher_job_cancelled(); }

/** @brief Holds the job for OPENPETE_PREVIEW_DELAY_MS, the harness's slow preview, in 10 ms steps.
 * @param deadline The op_now() time the hold ends at, the bound.
 * @return 0, -2 when stopped, -3 at the bound. */
static int lp_delay(op_time_t deadline) {
    const char* e = getenv("OPENPETE_PREVIEW_DELAY_MS");
    const long ms = e ? atol(e) : 0;
    for (long t = 0; t < ms; t += 10) {
        if (openpete_launcher_job_cancelled()) return OPENPETE_PREVIEW_STOPPED;
        if (op_now() > deadline) return OPENPETE_PREVIEW_TRANSIENT;
        op_sleep_until(op_now() + 10 * OP_NS_PER_MS);
    }
    return 0;
}

/** @brief Returns the preview's time bound in milliseconds: OPENPETE_PREVIEW_BOUND_MS when set, else LAUNCHER_PREVIEW_BOUND_MS.
 * @return The bound. */
static long lp_bound_ms(void) {
    const char* e = getenv("OPENPETE_PREVIEW_BOUND_MS");
    const long v = (e && *e) ? atol(e) : 0;
    return v > 0 ? v : LAUNCHER_PREVIEW_BOUND_MS;
}

/** @brief Builds the preview of a loaded level from the game's spec and environment.
 * @param b The installed game's build record.
 * @param L The level.
 * @param E The game's environment row.
 * @param pose The pose text.
 * @param bg The clear colour.
 * @param P Receives the preview.
 * @param why Receives the failure's reason.
 * @param cap Its capacity.
 * @return 0, or -1. */
static int lp_build(const openpete_game_build_t* b, launcher_preview_level* L, const launcher_preview_env* E,
                    const char* pose, const uint8_t bg[3], openpete_tercache_preview* P, char* why, size_t cap) {
    int32_t v[5];
    if (freecam_pose_numbers(pose, v) < 5) return lp_fail(why, cap, "pose \"%s\": five numbers expected", pose);
    terrain_preview_view V;
    memset(&V, 0, sizeof V);
    V.pos[0] = v[0]; V.pos[1] = v[1]; V.pos[2] = v[2];
    V.yaw = v[3]; V.pitch = v[4];
    V.lod_dist = E->lod_dist; V.cull_dist = E->cull_dist;
    /* The projection --fov LAUNCHER_PREVIEW_FOV_DEG gives the game: the
     * canonical H scaled by the FOV over the game's displayed rows. */
    V.H_canon = E->H;
    V.H = E->H * openpete_native_fov_h_scale_at(LAUNCHER_PREVIEW_FOV_DEG, (int)lroundf(E->clip_h), E->H, NGTE_Y_ROW);
    V.ofx = E->ofx; V.ofy = E->ofy;
    const terrain_host_level H = { L->bytes, L->len, L->sector_off, L->sector_count, L->lq_off, L->hq_off,
                                   L->exe, L->exe_addr, L->exe_len };
    terrain_preview_stats st;
    if (terrain_preview_build(&H, &V, P, &st, why, cap) != 0) return -1;
    OPL_INFO_F("launcher", "preview of %s: %d sectors decoded, %d drawn from the pose, %d baked (%d near, %d failed); "
               "vertices opaque %llu, alpha %llu, add %llu, sub %llu, lp %llu; %llu in arenas not drawn; "
               "reads of the executable's tables %llu, other guest reads %llu, guest stores %llu", b->id, st.sectors, st.drawn, st.baked, st.near, st.bake_fail,
               (unsigned long long)st.verts[0], (unsigned long long)st.verts[1], (unsigned long long)st.verts[2],
               (unsigned long long)st.verts[3], (unsigned long long)st.verts[4], (unsigned long long)st.dropped,
               (unsigned long long)st.exe_reads, (unsigned long long)st.guest_reads, (unsigned long long)st.guest_writes);
    if (st.baked == 0) return lp_fail(why, cap, "no sector draws from the pose");
    /* The target is the stored size times LAUNCHER_PREVIEW_SSAA per axis,
     * resolved down after the sky is composited (lp_resolve). */
    (void)launcher_preview_size(b->id, &P->w, &P->h);
    P->w *= LAUNCHER_PREVIEW_SSAA;
    P->h *= LAUNCHER_PREVIEW_SSAA;
    for (int c = 0; c < 3; c++) P->bg[c] = (float)bg[c] / 255.0f;
    P->vram = L->vram;
    L->vram = NULL;
    /* The displayed PSX screen, 512 wide and the game's displayed rows
     * (its clip mapping at 4:3), over the whole target, which has the
     * displayed frame's aspect: the picture is not stretched. */
    memset(&P->clip, 0, sizeof P->clip);
    P->clip.disp_w = 512.0f;
    P->clip.clip_h = E->clip_h;
    P->clip.band = E->band;
    P->clip.freeze_wf = 1.0f;
    P->clip.full_extent = 1;
    return 0;
}

int launcher_preview_build_only(const openpete_game_build_t* b, const char* disc_path, char* why,
                                size_t cap) OP_REQUIRES(ingest) {
    const launcher_preview_spec* S = launcher_preview_spec_of(b->id);
    const launcher_preview_env* E = lp_env_of(b->id);
    const game_assets_desc_t* A = (b->runtime && b->runtime->host) ? b->runtime->host->assets : NULL;
    if (!S || !E || !A || !A->levels) return lp_fail(why, cap, "%s has no box picture", b->id);
    const int number = game_level_resolve(A->levels, S->level);
    disc_image_t* img = number >= 0 ? disc_image_open(disc_path, DISC_READ_DIRECT, OPENPETE_DISC_TAG_INGEST) : NULL;
    if (!img) return lp_fail(why, cap, "disc %s or level %s unusable", disc_path, S->level);
    launcher_preview_level L;
    int rc = launcher_preview_level_load(img, b, number, &L, why, cap);
    disc_image_close(img);
    if (rc != 0) return -1;
    openpete_tercache_preview P;
    memset(&P, 0, sizeof P);
    const uint8_t bg[3] = {0, 0, 0};
    rc = lp_build(b, &L, E, S->pose, bg, &P, why, cap);
    launcher_preview_level_free(&L);
    openpete_tercache_preview_free(&P);
    return rc;
}

/* ---- the sky dome behind the terrain ------------------------------------
 * The game draws its sky as gouraud triangles of the decoded sky vertices
 * through the camera's projection matrix with no translation (cyclorama.c),
 * projected by the GTE and filled in screen space; the dome is drawn that
 * way here, on the CPU, into the image behind the terrain's pixels. */

/* A dome vertex in view space (the projection matrix applied, no
 * translation) with its colour. */
typedef struct { double m[3]; double c[3]; } lp_dv;

/* The view-Z a dome triangle is clipped at (the directions are scaled to 32767). */
#define LP_DOME_NEAR 16.0

/** @brief Clips a view-space triangle to the half-space in front of the eye (m3 >= LP_DOME_NEAR), giving up to four vertices.
 * @param in The triangle.
 * @param out Receives the polygon.
 * @return The polygon's vertex count, 0 when the triangle lies behind. */
static int lp_dome_clip(const lp_dv in[3], lp_dv out[4]) {
    int n = 0;
    for (int i = 0; i < 3; i++) {
        const lp_dv* a = &in[i];
        const lp_dv* b = &in[(i + 1) % 3];
        const int ia = a->m[2] >= LP_DOME_NEAR, ib = b->m[2] >= LP_DOME_NEAR;
        if (ia) out[n++] = *a;
        if (ia != ib && n < 4) {
            const double t = (LP_DOME_NEAR - a->m[2]) / (b->m[2] - a->m[2]);
            lp_dv* o = &out[n++];
            for (int k = 0; k < 3; k++) {
                o->m[k] = a->m[k] + t * (b->m[k] - a->m[k]);
                o->c[k] = a->c[k] + t * (b->c[k] - a->c[k]);
            }
        }
    }
    return n;
}

/** @brief Fills one screen-space triangle with colours interpolated across it (gouraud), into an RGBA image.
 * @param img The image.
 * @param w Its width.
 * @param h Its height.
 * @param x The corners' x.
 * @param y The corners' y.
 * @param c The corners' colours, 0-255 per channel. */
static void lp_fill(uint8_t* img, int w, int h, const double x[3], const double y[3], const double c[3][3]) {
    const double area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
    if (area == 0.0) return;
    double x0 = fmin(x[0], fmin(x[1], x[2])), x1 = fmax(x[0], fmax(x[1], x[2]));
    double y0 = fmin(y[0], fmin(y[1], y[2])), y1 = fmax(y[0], fmax(y[1], y[2]));
    const int ix0 = x0 < 0 ? 0 : (int)x0, iy0 = y0 < 0 ? 0 : (int)y0;
    const int ix1 = x1 > w - 1 ? w - 1 : (int)x1, iy1 = y1 > h - 1 ? h - 1 : (int)y1;
    for (int py = iy0; py <= iy1; py++)
        for (int px = ix0; px <= ix1; px++) {
            const double sx = px + 0.5, sy = py + 0.5;
            const double w0 = ((x[1] - sx) * (y[2] - sy) - (x[2] - sx) * (y[1] - sy)) / area;
            const double w1 = ((x[2] - sx) * (y[0] - sy) - (x[0] - sx) * (y[2] - sy)) / area;
            const double w2 = 1.0 - w0 - w1;
            if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0) continue;
            uint8_t* o = img + 4u * ((size_t)py * (size_t)w + (size_t)px);
            for (int k = 0; k < 3; k++) {
                const double v = w0 * c[0][k] + w1 * c[1][k] + w2 * c[2][k];
                o[k] = (uint8_t)(v < 0.0 ? 0.0 : (v > 255.0 ? 255.0 : v + 0.5));
            }
            o[3] = 255;
        }
}

/** @brief Draws a dome into an RGBA image: the clear colour, then every triangle projected through the preview's matrix and screen mapping.
 * @param P The preview: its projection matrix, projection and clip mapping.
 * @param D The dome.
 * @param img The image, P->w x P->h.
 * @return The triangles filled. */
static uint32_t lp_dome_draw(const openpete_tercache_preview* P, const launcher_preview_dome* D, uint8_t* img) {
    const size_t n = (size_t)P->w * P->h;
    for (size_t i = 0; i < n; i++) {
        img[4 * i] = D->bg[0]; img[4 * i + 1] = D->bg[1]; img[4 * i + 2] = D->bg[2]; img[4 * i + 3] = 255;
    }
    uint32_t filled = 0;
    const double sxk = (double)P->w / P->clip.disp_w, syk = (double)P->h / P->clip.clip_h;
    for (uint32_t t = 0; D->verts && D->tris && t < D->ntri; t++) {
        lp_dv tri[3], poly[4];
        int ok = 1;
        for (int k = 0; k < 3 && ok; k++) {
            const uint32_t vi = D->tris[3 * t + k];
            if (vi >= D->nv) { ok = 0; break; }
            const uint8_t* p = D->verts + 9u * vi;
            /* stored (x, -y, z): the decoded sky vertex is (x, y, z) */
            const double v[3] = { (double)(int16_t)(p[0] | (p[1] << 8)), -(double)(int16_t)(p[2] | (p[3] << 8)),
                                  (double)(int16_t)(p[4] | (p[5] << 8)) };
            for (int r = 0; r < 3; r++)
                tri[k].m[r] = (P->g.mrow[r][0] * v[0] + P->g.mrow[r][1] * v[1] + P->g.mrow[r][2] * v[2]) / 4096.0;
            for (int c = 0; c < 3; c++) tri[k].c[c] = p[6 + c];
        }
        if (!ok) continue;
        const int pn = lp_dome_clip(tri, poly);
        double x[4], y[4];
        for (int k = 0; k < pn; k++) {
            x[k] = (P->g.ofx + P->g.H * poly[k].m[0] / poly[k].m[2]) * sxk;
            y[k] = (P->g.ofy + P->g.H * poly[k].m[1] / poly[k].m[2] - P->clip.band) * syk;
        }
        for (int k = 1; k + 1 < pn; k++) {
            const double tx[3] = { x[0], x[k], x[k + 1] }, ty[3] = { y[0], y[k], y[k + 1] };
            const double tc[3][3] = { { poly[0].c[0], poly[0].c[1], poly[0].c[2] },
                                      { poly[k].c[0], poly[k].c[1], poly[k].c[2] },
                                      { poly[k + 1].c[0], poly[k + 1].c[1], poly[k + 1].c[2] } };
            lp_fill(img, (int)P->w, (int)P->h, tx, ty, tc);
        }
        filled += pn >= 3;
    }
    return filled;
}

/** @brief Resolves a supersampled image down in place: each output pixel the mean of its k x k source pixels, alpha opaque.
 * @param px The image, w * h RGBA; the first (w / k) * (h / k) pixels receive the result.
 * @param w The source width, a multiple of k.
 * @param h The source height, a multiple of k.
 * @param k The factor per axis. */
static void lp_resolve(uint8_t* px, uint32_t w, uint32_t h, uint32_t k) {
    if (k <= 1) return;
    const uint32_t ow = w / k, oh = h / k, n = k * k;
    for (uint32_t y = 0; y < oh; y++)
        for (uint32_t x = 0; x < ow; x++) {
            uint32_t sum[3] = { 0, 0, 0 };
            for (uint32_t j = 0; j < k; j++)
                for (uint32_t i = 0; i < k; i++) {
                    const uint8_t* s = px + ((size_t)(y * k + j) * w + (x * k + i)) * 4u;
                    sum[0] += s[0]; sum[1] += s[1]; sum[2] += s[2];
                }
            uint8_t* d = px + ((size_t)y * ow + x) * 4u;   /* at or before every source pixel still unread */
            for (int c = 0; c < 3; c++) d[c] = (uint8_t)((sum[c] + n / 2) / n);
            d[3] = 255;
        }
}

/** @brief Puts the dome behind the terrain: every pixel the terrain's draw left at the clear (alpha 0) takes the dome's.
 * @param px The terrain's image, read back with its alpha; made opaque.
 * @param dome The dome's image.
 * @param n The pixel count.
 * @return The pixels the dome shows through. */
static size_t lp_composite(uint8_t* px, const uint8_t* dome, size_t n) {
    size_t sky = 0;
    for (size_t i = 0; i < n; i++) {
        if (px[4 * i + 3] == 0) { memcpy(px + 4 * i, dome + 4 * i, 3); sky++; }
        px[4 * i + 3] = 255;
    }
    return sky;
}

int launcher_preview_render(const openpete_game_build_t* b, const char* disc_path, const launcher_preview_dome* dome,
                            uint8_t** rgba, double* ms, char* why, size_t cap) OP_REQUIRES(ingest) {
    *rgba = NULL;
    *ms = 0.0;
    const op_time_t t0 = op_now();
    const op_time_t deadline = t0 + (op_time_t)lp_bound_ms() * OP_NS_PER_MS;
    const launcher_preview_spec* S = launcher_preview_spec_of(b->id);
    const launcher_preview_env* E = lp_env_of(b->id);
    const game_assets_desc_t* A = (b->runtime && b->runtime->host) ? b->runtime->host->assets : NULL;
    if (!S || !E || !A || !A->levels) { lp_fail(why, cap, "%s has no box picture", b->id); return OPENPETE_PREVIEW_FAILED; }
    if (lp_forced_fail(b->id)) { lp_fail(why, cap, "forced (OPENPETE_PREVIEW_FAIL)"); return OPENPETE_PREVIEW_FAILED; }
    const int number = game_level_resolve(A->levels, S->level);
    if (number < 0) { lp_fail(why, cap, "level %s is not in the level table", S->level); return OPENPETE_PREVIEW_FAILED; }
    openpete_launcher_preview_progress(1, 4);
    disc_image_t* img = disc_image_open(disc_path, DISC_READ_DIRECT, OPENPETE_DISC_TAG_INGEST);
    if (!img) { lp_fail(why, cap, "disc %s unreadable", disc_path); return OPENPETE_PREVIEW_FAILED; }
    launcher_preview_level L;
    int rc = launcher_preview_level_load(img, b, number, &L, why, cap);
    disc_image_close(img);
    if (rc != 0) return OPENPETE_PREVIEW_FAILED;
    openpete_launcher_preview_progress(2, 4);
    openpete_tercache_preview P;
    memset(&P, 0, sizeof P);
    rc = lp_build(b, &L, E, S->pose, dome->bg, &P, why, cap);
    launcher_preview_level_free(&L);
    if (rc != 0) { openpete_tercache_preview_free(&P); return OPENPETE_PREVIEW_FAILED; }
    openpete_launcher_preview_progress(3, 4);
    rc = lp_delay(deadline);
    if (rc != 0) {
        lp_fail(why, cap, rc == OPENPETE_PREVIEW_STOPPED ? "stopped" : "past the time bound (%ld ms)", lp_bound_ms());
        openpete_tercache_preview_free(&P);
        return rc;
    }
    const size_t n = (size_t)P.w * P.h * 4u;
    uint8_t* px = (uint8_t*)malloc(n);
    if (!px) { openpete_tercache_preview_free(&P); lp_fail(why, cap, "image allocation"); return OPENPETE_PREVIEW_FAILED; }
    rc = openpete_native_launcher_preview_render(&P, game_layout.terrain.slot_ot_contest, px, n, lp_stop,
                                                 (uint64_t)deadline, why, cap);
    openpete_launcher_preview_progress(4, 4);
    uint8_t* sky = rc == OPENPETE_PREVIEW_OK ? (uint8_t*)malloc(n) : NULL;
    if (sky) {
        const uint32_t tris = lp_dome_draw(&P, dome, sky);
        const size_t shown = lp_composite(px, sky, n / 4u);
        OPL_INFO_F("launcher", "preview of %s: dome %u of %u triangles filled, %zu pixels show the sky",
                   b->id, tris, dome->ntri, shown);
    } else if (rc == OPENPETE_PREVIEW_OK) {
        rc = OPENPETE_PREVIEW_FAILED;
        lp_fail(why, cap, "dome image allocation");
    }
    free(sky);
    if (rc == OPENPETE_PREVIEW_OK) lp_resolve(px, P.w, P.h, LAUNCHER_PREVIEW_SSAA);
    openpete_tercache_preview_free(&P);
    if (rc != OPENPETE_PREVIEW_OK) { free(px); return rc; }
    *rgba = px;
    *ms = (double)(op_now() - t0) / 1e6;
    return OPENPETE_PREVIEW_OK;
}
