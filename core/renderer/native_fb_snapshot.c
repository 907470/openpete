/* native_fb_snapshot.c — see openpete_fb_snapshot.h. */

#include "openpete_fb_snapshot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    unsigned char *px;        /* RGBA8 crop of the native composed frame */
    int pw, ph;
    uint64_t key;             /* unique per stored image (see the header) */
} fb_snap_t;

static fb_snap_t s_slot[FB_SNAP_MAX];
static uint64_t  s_key_next = 1;
static unsigned  s_taken, s_resolved, s_quads;

int openpete_fb_snapshot_enabled(void)
{
    static int s_en = -1;
    if (s_en < 0) { const char *e = getenv("OPENPETE_FBSNAP"); s_en = !(e && *e == '0'); }
    return s_en;
}

int openpete_fb_snapshot_on_move(int src_x, int src_y, int w, int h,
                                 int dst_x, int dst_y)
{
    if (!openpete_fb_snapshot_enabled()) return 0;
    if (w <= 0 || h <= 0) return 0;
    /* Source wholly inside one display buffer, destination wholly outside both. */
    if (src_x + w > FB_BUFFER_W) return 0;
    if (dst_x < FB_BUFFER_W) return 0;
    const int buf_y = (src_y >= FB_BUFFER_H) ? FB_BUFFER_H : 0;
    if (src_y + h > buf_y + FB_BUFFER_H) return 0;

    int idx = -1;
    for (int i = 0; i < FB_SNAP_MAX; i++)
        if (s_slot[i].dst_x == dst_x && s_slot[i].dst_y == dst_y &&
            s_slot[i].w == w && s_slot[i].h == h) { idx = i; break; }
    if (idx < 0)
        for (int i = 0; i < FB_SNAP_MAX; i++)
            if (!s_slot[i].px && !s_slot[i].pending) { idx = i; break; }
    if (idx < 0) idx = 0;   /* all live: the oldest slot takes the newest window */

    fb_snap_t *s = &s_slot[idx];
    /* The stored pixels stay until the new ones arrive: a quad fed between the
     * move and the present that resolves it draws the previous window rather
     * than falling back to the transparent VRAM copy. */
    s->dst_x = dst_x; s->dst_y = dst_y; s->w = w; s->h = h;
    s->src_x = src_x; s->src_y = src_y - buf_y;
    s->pending = 1;
    s_taken++;
    return 1;
}

int openpete_fb_snapshot_pending(void)
{
    for (int i = 0; i < FB_SNAP_MAX; i++) if (s_slot[i].pending) return 1;
    return 0;
}

int openpete_fb_snapshot_take_pending(int *idx, int *src_x, int *src_y,
                                      int *w, int *h)
{
    for (int i = 0; i < FB_SNAP_MAX; i++) {
        if (!s_slot[i].pending) continue;
        s_slot[i].pending = 0;
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

int openpete_fb_snapshot_find(int tpage_x, int tpage_y, int bpp,
                             int u0, int v0, int u1, int v1,
                             float *su, float *sv, float *ox, float *oy)
{
    if (!openpete_fb_snapshot_enabled()) return -1;
    if (bpp != 16) return -1;
    for (int i = 0; i < FB_SNAP_MAX; i++) {
        const fb_snap_t *s = &s_slot[i];
        if (!s->px) continue;
        /* The page origin sits at or after the stored rect's origin and the
         * quad's texel box stays inside the rect: the quad samples this copy
         * and nothing beyond it. A quad's far corner names the edge past its
         * last texel (a 128-row copy is drawn with v 0..128), so the far bound
         * is inclusive of the rect's own edge. */
        const int du = tpage_x - s->dst_x, dv = tpage_y - s->dst_y;
        if (du < 0 || dv < 0) continue;
        if (du + u0 < 0 || dv + v0 < 0) continue;
        if (du + u1 > s->w || dv + v1 > s->h) continue;
        const float sx = (float)s->pw / (float)s->w;
        const float sy = (float)s->ph / (float)s->h;
        if (su) *su = sx;
        if (sv) *sv = sy;
        if (ox) *ox = (float)du * sx;
        if (oy) *oy = (float)dv * sy;
        s_quads++;
        return i;
    }
    return -1;
}

const unsigned char *openpete_fb_snapshot_pixels(int idx, int *pw, int *ph)
{
    if (idx < 0 || idx >= FB_SNAP_MAX || !s_slot[idx].px) return NULL;
    if (pw) *pw = s_slot[idx].pw;
    if (ph) *ph = s_slot[idx].ph;
    return s_slot[idx].px;
}

uint64_t openpete_fb_snapshot_key(int idx)
{
    if (idx < 0 || idx >= FB_SNAP_MAX) return 0;
    return s_slot[idx].key;
}

void openpete_fb_snapshot_counts(unsigned *taken, unsigned *resolved,
                                 unsigned *quads)
{
    if (taken)    *taken    = s_taken;
    if (resolved) *resolved = s_resolved;
    if (quads)    *quads    = s_quads;
}
