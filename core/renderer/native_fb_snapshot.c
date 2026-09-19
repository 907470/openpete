/* native_fb_snapshot.c — see openpete_fb_snapshot.h. */

#include "openpete_fb_snapshot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* The VBL counter the scene ring stamps each completed frame with. */
extern unsigned openpete_frame;

/* One slot per live window. A screen holds at most one; the spare slots cover a
 * screen that keeps two copies (a before/after pair) without evicting either. */
#define FB_SNAP_MAX 4

/* The two display buffers occupy the left half of VRAM (a 512-texel-wide
 * framebuffer at y 0 and y 256); everything at x >= 512 is texture and scratch
 * space. A window copy runs from the first region to the second. */
#define FB_BUFFER_W  512
#define FB_BUFFER_H  256

typedef struct {
    int dst_x, dst_y, w, h;   /* VRAM texel rect the copy landed on */
    int src_x, src_y;         /* copy source, in framebuffer screen pixels */
    int pending;              /* waiting for a present to crop its pixels */
    unsigned frame;           /* the frame the copy was taken on */
    unsigned char *px;        /* RGBA8 crop of the native composed frame */
    int pw, ph;
    uint64_t key;             /* unique per stored image (see the header) */
} fb_snap_t;

static fb_snap_t s_slot[FB_SNAP_MAX];
static uint64_t  s_key_next = 1;
static unsigned  s_taken, s_resolved, s_quads, s_area_hits;

/* A draw area the game put, with the native frame that stands for the content
 * of the buffer it covers. Two areas serve a double-buffered game; the spares
 * cover a screen that puts a third (an off-screen render target). */
#define FB_AREA_MAX 4
typedef struct {
    int x, y, w, h;           /* DRAWENV clip rect, VRAM texels */
    int ofx, ofy;             /* DRAWENV drawing offset */
    int live;                 /* the game has put this area */
    int pending;              /* waiting for a present to stand for its content */
    unsigned frame;           /* the frame the copy that put it was taken on */
    unsigned char *px;        /* RGBA8 crop of that present over the area's rect */
    int pw, ph;
    uint64_t key;             /* unique per stored image (see the header) */
} fb_area_t;
static fb_area_t s_area[FB_AREA_MAX];
static int       s_area_next;   /* the slot a new clip rect takes when all are live */
static int       s_area_cur = -1;  /* the area the game last put: the buffer it draws */

/* Mean and spread of an RGBA8 image's luma: a stored frame that is one clear
 * colour has a spread near zero, a frame carrying the world does not. */
static void fb_image_stats(const unsigned char *px, int pw, int ph,
                           double *mean, double *spread)
{
    const size_t n = (size_t)pw * (size_t)ph;
    double s = 0.0, s2 = 0.0;
    for (size_t i = 0; i < n; i++) {
        const double l = (px[i * 4] + px[i * 4 + 1] + px[i * 4 + 2]) / 3.0;
        s += l; s2 += l * l;
    }
    *mean = n ? s / (double)n : 0.0;
    const double v = n ? s2 / (double)n - (*mean) * (*mean) : 0.0;
    *spread = v > 0.0 ? sqrt(v) : 0.0;
}

static int fbsnap_census(void)
{
    static int s = -1;
    if (s < 0) { const char *e = getenv("OPENPETE_FBSNAP_CENSUS"); s = (e && *e && *e != '0'); }
    return s;
}

/* The screen rect an area covers: the clip rect less the drawing offset. */
static void fb_area_screen(const fb_area_t *a, int *sx, int *sy)
{ *sx = a->x - a->ofx; *sy = a->y - a->ofy; }

static void fb_area_drop(fb_area_t *a)
{ free(a->px); a->px = NULL; a->pw = a->ph = 0; a->pending = 0; }

void openpete_fb_snapshot_note_draw_env(int clip_x, int clip_y, int clip_w, int clip_h,
                                        int ofs_x, int ofs_y, int isbg)
{
    if (!openpete_fb_snapshot_enabled()) return;
    if (clip_w <= 0 || clip_h <= 0) return;
    int idx = -1;
    for (int i = 0; i < FB_AREA_MAX && idx < 0; i++)
        if (s_area[i].live && s_area[i].x == clip_x && s_area[i].y == clip_y &&
            s_area[i].w == clip_w && s_area[i].h == clip_h) idx = i;
    if (idx < 0)
        for (int i = 0; i < FB_AREA_MAX && idx < 0; i++)
            if (!s_area[i].live) idx = i;
    if (idx < 0) { idx = s_area_next; s_area_next = (s_area_next + 1) % FB_AREA_MAX; }
    fb_area_t *a = &s_area[idx];
    if (fbsnap_census())
        fprintf(stderr, "[fbsnap] drawenv area %d clip=%d,%d %dx%d ofs=%d,%d isbg=%d had_px=%d\n",
                idx, clip_x, clip_y, clip_w, clip_h, ofs_x, ofs_y, isbg, a->px != NULL);
    /* Auto-clear on: the buffer's next frame starts blank, so whatever image
     * stood for its content is gone. Auto-clear off: the game draws over what
     * the buffer holds, and a copy it has just taken stands until the buffer is
     * cleared again. */
    if (isbg) fb_area_drop(a);
    a->x = clip_x; a->y = clip_y; a->w = clip_w; a->h = clip_h;
    a->ofx = ofs_x; a->ofy = ofs_y; a->live = 1;
    s_area_cur = idx;
}

/* The area whose clip rect holds a VRAM rect, NULL when none does. */
static fb_area_t *fb_area_holding(int x, int y, int w, int h)
{
    for (int i = 0; i < FB_AREA_MAX; i++) {
        fb_area_t *a = &s_area[i];
        if (!a->live) continue;
        if (x < a->x || y < a->y || x + w > a->x + a->w || y + h > a->y + a->h) continue;
        return a;
    }
    return NULL;
}

int openpete_fb_snapshot_enabled(void)
{
    static int s_en = -1;
    if (s_en < 0) { const char *e = getenv("OPENPETE_FBSNAP"); s_en = !(e && *e == '0'); }
    return s_en;
}

/* Box-sample `src` (RGBA8 `sw` by `sh`, standing for the screen rect at
 * `asx`,`asy` of size `aw` by `ah`) over the screen rect `sx`,`sy` `w` by `h`.
 * Returns a fresh buffer and its size, or NULL when the rect falls outside. */
static unsigned char *fb_crop_image(const unsigned char *src, int sw, int sh,
                                    int asx, int asy, int aw, int ah,
                                    int sx, int sy, int w, int h,
                                    int *out_w, int *out_h)
{
    if (aw <= 0 || ah <= 0) return NULL;
    int x0 = (sx - asx) * sw / aw, x1 = (sx + w - asx) * sw / aw;
    int y0 = (sy - asy) * sh / ah, y1 = (sy + h - asy) * sh / ah;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > sw) x1 = sw; if (y1 > sh) y1 = sh;
    const int pw = x1 - x0, ph = y1 - y0;
    if (pw <= 0 || ph <= 0) return NULL;
    unsigned char *px = (unsigned char *)malloc((size_t)pw * (size_t)ph * 4u);
    if (!px) return NULL;
    for (int row = 0; row < ph; row++)
        memcpy(px + (size_t)row * pw * 4u,
               src + ((size_t)(y0 + row) * sw + x0) * 4u, (size_t)pw * 4u);
    *out_w = pw; *out_h = ph;
    return px;
}

/* Claim the window slot for a destination rect: the slot already holding that
 * rect, else a free one, else slot 0. The stored pixels stay until the new ones
 * arrive, so a quad fed between the move and its resolve draws the previous
 * window rather than falling back to the transparent VRAM copy. */
static fb_snap_t *fb_window_slot(int dst_x, int dst_y, int w, int h)
{
    int idx = -1;
    for (int i = 0; i < FB_SNAP_MAX; i++)
        if (s_slot[i].dst_x == dst_x && s_slot[i].dst_y == dst_y &&
            s_slot[i].w == w && s_slot[i].h == h) { idx = i; break; }
    if (idx < 0)
        for (int i = 0; i < FB_SNAP_MAX; i++)
            if (!s_slot[i].px && !s_slot[i].pending) { idx = i; break; }
    if (idx < 0) idx = 0;
    fb_snap_t *s = &s_slot[idx];
    s->dst_x = dst_x; s->dst_y = dst_y; s->w = w; s->h = h;
    return s;
}

/* A copy covering the destination area whole: the game moving a finished frame
 * into the buffer it is not drawing. The destination takes the source area's
 * content image; with none (the source is the buffer being drawn) it waits for
 * the next present, which composes that same frame. */
static void fb_move_area_to_area(fb_area_t *from, fb_area_t *to,
                                 int dst_x, int dst_y, int w, int h)
{
    if (dst_x != to->x || dst_y != to->y || w != to->w || h != to->h) return;
    fb_area_drop(to);
    if (!from->px) {
        /* The source is the buffer being drawn, so the present composed next
         * is the frame this copy moved. After it both buffers hold that frame:
         * the destination by the copy, the source because the game has stopped
         * drawing into it. One capture stands for both. */
        to->pending = 1;
        from->pending = 1;
        to->frame = from->frame = openpete_frame;
        return;
    }
    to->pw = from->pw; to->ph = from->ph;
    const size_t n = (size_t)from->pw * (size_t)from->ph * 4u;
    to->px = (unsigned char *)malloc(n);
    if (to->px) { memcpy(to->px, from->px, n); to->key = s_key_next++; }
    else { to->pw = to->ph = 0; to->pending = 1; }
}

int openpete_fb_snapshot_on_move(int src_x, int src_y, int w, int h,
                                 int dst_x, int dst_y)
{
    if (!openpete_fb_snapshot_enabled()) return 0;
    if (w <= 0 || h <= 0) return 0;
    if (src_x + w > FB_BUFFER_W) return 0;

    fb_area_t *from = fb_area_holding(src_x, src_y, w, h);
    int ofx = 0, ofy = 0;
    if (from) { ofx = from->ofx; ofy = from->ofy; }
    else {
        /* No draw area recorded yet: the display buffers split at VRAM row 256
         * with no drawing offset. */
        ofy = (src_y >= FB_BUFFER_H) ? FB_BUFFER_H : 0;
        if (src_y + h > ofy + FB_BUFFER_H) return 0;
    }
    const int ssx = src_x - ofx, ssy = src_y - ofy;

    if (dst_x < FB_BUFFER_W) {
        fb_area_t *to = fb_area_holding(dst_x, dst_y, w, h);
        if (from && to && to != from) fb_move_area_to_area(from, to, dst_x, dst_y, w, h);
        return 0;
    }

    fb_snap_t *s = fb_window_slot(dst_x, dst_y, w, h);
    s->src_x = ssx; s->src_y = ssy;
    s->frame = openpete_frame;
    s_taken++;
    const fb_area_t *cur = (s_area_cur >= 0) ? &s_area[s_area_cur] : NULL;
    if (from && from->px && from != cur) {
        /* The copied buffer is not the one the game is drawing into, so no
         * present will compose it: crop the frame that stands for its content.
         * The buffer being drawn keeps the next-present rule, since the present
         * that follows this move composes exactly what the copy took. */
        int asx, asy, pw = 0, ph = 0;
        fb_area_screen(from, &asx, &asy);
        unsigned char *px = fb_crop_image(from->px, from->pw, from->ph,
                                          asx, asy, from->w, from->h,
                                          ssx, ssy, w, h, &pw, &ph);
        if (px) {
            s->pending = 0;
            openpete_fb_snapshot_store((int)(s - s_slot), px, pw, ph);
            s_area_hits++;
            if (fbsnap_census())
                fprintf(stderr, "[fbsnap] window %d,%d %dx%d from area clip=%d,%d "
                                "image=%dx%d hits=%u\n",
                        ssx, ssy, w, h, from->x, from->y, pw, ph, s_area_hits);
            return 1;
        }
    }
    s->pending = 1;
    return 1;
}

int openpete_fb_snapshot_pending(void)
{
    for (int i = 0; i < FB_SNAP_MAX; i++) if (s_slot[i].pending) return 1;
    for (int i = 0; i < FB_AREA_MAX; i++) if (s_area[i].pending) return 1;
    return 0;
}

int openpete_fb_snapshot_take_pending_area(int *idx, int *src_x, int *src_y,
                                           int *w, int *h, unsigned *frame)
{
    for (int i = 0; i < FB_AREA_MAX; i++) {
        if (!s_area[i].pending) continue;
        s_area[i].pending = 0;
        int sx, sy; fb_area_screen(&s_area[i], &sx, &sy);
        if (frame) *frame = s_area[i].frame;
        if (idx)   *idx   = i;
        if (src_x) *src_x = sx;
        if (src_y) *src_y = sy;
        if (w)     *w     = s_area[i].w;
        if (h)     *h     = s_area[i].h;
        return 1;
    }
    return 0;
}

void openpete_fb_snapshot_store_area(int idx, unsigned char *px, int pw, int ph)
{
    if (idx < 0 || idx >= FB_AREA_MAX || !px || pw <= 0 || ph <= 0) { free(px); return; }
    fb_area_t *a = &s_area[idx];
    free(a->px);
    a->px = px; a->pw = pw; a->ph = ph;
    a->key = s_key_next++;
    if (fbsnap_census()) {
        double m = 0.0, sd = 0.0;
        fb_image_stats(px, pw, ph, &m, &sd);
        fprintf(stderr, "[fbsnap] area %d clip=%d,%d %dx%d ofs=%d,%d image=%dx%d "
                        "luma=%.1f spread=%.1f\n",
                idx, a->x, a->y, a->w, a->h, a->ofx, a->ofy, pw, ph, m, sd);
    }
}

int openpete_fb_snapshot_take_pending(int *idx, int *src_x, int *src_y,
                                      int *w, int *h, unsigned *frame)
{
    for (int i = 0; i < FB_SNAP_MAX; i++) {
        if (!s_slot[i].pending) continue;
        s_slot[i].pending = 0;
        if (frame) *frame = s_slot[i].frame;
        if (idx)   *idx   = i;
        if (src_x) *src_x = s_slot[i].src_x;
        if (src_y) *src_y = s_slot[i].src_y;
        if (w)     *w     = s_slot[i].w;
        if (h)     *h     = s_slot[i].h;
        return 1;
    }
    return 0;
}

void openpete_fb_snapshot_store(int idx, unsigned char *px, int pw, int ph)
{
    if (idx < 0 || idx >= FB_SNAP_MAX) { free(px); return; }
    if (!px || pw <= 0 || ph <= 0) { free(px); return; }
    fb_snap_t *s = &s_slot[idx];
    free(s->px);
    s->px = px; s->pw = pw; s->ph = ph;
    s->key = s_key_next++;
    s_resolved++;
}

/* A quad's texel box, offset by `du`,`dv` from a stored rect's origin, stays
 * inside that `w` by `h` rect. The page origin may precede the rect (a copy
 * landed at row 12 of a page is drawn with v 12..120), so only the box is
 * bounded. A quad's far corner names the edge past its last texel (a 128-row
 * copy is drawn with v 0..128), so the far bound is inclusive of the edge. */
static int fb_rect_holds(int du, int dv, int u0, int v0, int u1, int v1, int w, int h)
{
    return du + u0 >= 0 && dv + v0 >= 0 && du + u1 <= w && dv + v1 <= h;
}

/* The mapping from page texels to pixels of a `pw` by `ph` image standing for a
 * `w` by `h` texel rect whose origin is `du`,`dv` from the page origin. */
static void fb_fill_map(int du, int dv, int pw, int ph, int w, int h,
                        float *su, float *sv, float *ox, float *oy)
{
    const float sx = (float)pw / (float)w, sy = (float)ph / (float)h;
    if (su) *su = sx;
    if (sv) *sv = sy;
    if (ox) *ox = (float)du * sx;
    if (oy) *oy = (float)dv * sy;
}

/* The window slot whose copy holds the texel box, -1 when none does. */
static int fb_find_window(int tpage_x, int tpage_y, int u0, int v0, int u1, int v1,
                          float *su, float *sv, float *ox, float *oy)
{
    for (int i = 0; i < FB_SNAP_MAX; i++) {
        const fb_snap_t *s = &s_slot[i];
        const int du = tpage_x - s->dst_x, dv = tpage_y - s->dst_y;
        if (!s->px || !fb_rect_holds(du, dv, u0, v0, u1, v1, s->w, s->h)) continue;
        fb_fill_map(du, dv, s->pw, s->ph, s->w, s->h, su, sv, ox, oy);
        return i;
    }
    return -1;
}

/* The draw area whose content image holds the texel box, -1 when none does. A
 * quad whose texels lie in a draw buffer samples the framebuffer itself rather
 * than a copy of it — the shrink of a frozen frame before it is copied out —
 * and the area's clip rect stands in for a copy's destination rect. */
static int fb_find_area(int tpage_x, int tpage_y, int u0, int v0, int u1, int v1,
                        float *su, float *sv, float *ox, float *oy)
{
    for (int i = 0; i < FB_AREA_MAX; i++) {
        const fb_area_t *a = &s_area[i];
        const int du = tpage_x - a->x, dv = tpage_y - a->y;
        if (!a->px || !fb_rect_holds(du, dv, u0, v0, u1, v1, a->w, a->h)) continue;
        fb_fill_map(du, dv, a->pw, a->ph, a->w, a->h, su, sv, ox, oy);
        return FB_SNAP_MAX + i;
    }
    return -1;
}

int openpete_fb_snapshot_find(int tpage_x, int tpage_y, int bpp,
                             int u0, int v0, int u1, int v1,
                             float *su, float *sv, float *ox, float *oy)
{
    if (!openpete_fb_snapshot_enabled() || bpp != 16) return -1;
    int idx = fb_find_window(tpage_x, tpage_y, u0, v0, u1, v1, su, sv, ox, oy);
    if (idx < 0) idx = fb_find_area(tpage_x, tpage_y, u0, v0, u1, v1, su, sv, ox, oy);
    if (idx >= 0) s_quads++;
    return idx;
}

const unsigned char *openpete_fb_snapshot_pixels(int idx, int *pw, int *ph)
{
    if (idx >= FB_SNAP_MAX) {
        const fb_area_t *a = &s_area[idx - FB_SNAP_MAX];
        if (idx - FB_SNAP_MAX >= FB_AREA_MAX || !a->px) return NULL;
        if (pw) *pw = a->pw;
        if (ph) *ph = a->ph;
        return a->px;
    }
    if (idx < 0 || !s_slot[idx].px) return NULL;
    if (pw) *pw = s_slot[idx].pw;
    if (ph) *ph = s_slot[idx].ph;
    return s_slot[idx].px;
}

uint64_t openpete_fb_snapshot_key(int idx)
{
    if (idx >= FB_SNAP_MAX)
        return (idx - FB_SNAP_MAX < FB_AREA_MAX) ? s_area[idx - FB_SNAP_MAX].key : 0;
    if (idx < 0) return 0;
    return s_slot[idx].key;
}

void openpete_fb_snapshot_counts(unsigned *taken, unsigned *resolved,
                                 unsigned *quads)
{
    if (taken)    *taken    = s_taken;
    if (resolved) *resolved = s_resolved;
    if (quads)    *quads    = s_quads;
}
