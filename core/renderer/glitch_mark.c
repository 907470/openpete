/* The glitch mark: see core/include/openpete_glitch_mark.h. */

#include "openpete_glitch_mark.h"
#include "openpete_log.h"
#include "openpete_osd.h"
#include "openpete_time.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GM_DEPTH_DEFAULT 600
#define GM_DEPTH_MIN     16
#define GM_DEPTH_MAX     65536

/* A ring slot. `seq` is odd while the record is being written and even once
 * it is whole, so a reader that sees the same even seq before and after its
 * copy knows the copy is one consistent record. */
typedef struct { _Atomic uint32_t seq; openpete_glitch_present_t r; } gm_pslot;
typedef struct { _Atomic uint32_t seq; openpete_glitch_walk_t    r; } gm_wslot;

static gm_pslot *s_pres;
static gm_wslot *s_walk;
static int       s_depth;

/* Total pushed, ever. The newest `min(count, depth)` records are live, the
 * oldest of them at index (count - live) % depth. */
static _Atomic uint32_t s_pres_n;
static _Atomic uint32_t s_walk_n;

/* The walk most recently completed, for the scene publish to snapshot. Its
 * own seq: the guest thread writes it, the publish reads it. */
static _Atomic uint32_t  s_last_seq;
static openpete_glitch_walk_t s_last;

static _Atomic int  s_request;          /* a mark is pending */
static char         s_reason[24] = "key";
static uint32_t     s_marks;            /* files written this run */

/* OPENPETE_GLITCH_MARK_AT, parsed once. */
enum { GM_AT_OFF = 0, GM_AT_PRESENT, GM_AT_WALK, GM_AT_MS };
static int      s_at_kind = GM_AT_OFF;
static uint32_t s_at_value;
static op_time_t s_t0;

static int s_inited;

void openpete_glitch_mark_init(void)
{
    if (s_inited) return;
    s_inited = 1;
    int n = GM_DEPTH_DEFAULT;
    const char *e = getenv("OPENPETE_GLITCH_MARK_N");
    if (e && *e) {
        long v = strtol(e, NULL, 10);
        /* 0 allocates nothing and makes every note a no-op, which is the
         * leg an A/B needs to price the ring out of one binary. */
        if (v <= 0) return;
        if (v < GM_DEPTH_MIN) v = GM_DEPTH_MIN;
        if (v > GM_DEPTH_MAX) v = GM_DEPTH_MAX;
        n = (int)v;
    }
    s_pres = calloc((size_t)n, sizeof *s_pres);
    s_walk = calloc((size_t)n, sizeof *s_walk);
    if (!s_pres || !s_walk) { free(s_pres); free(s_walk); s_pres = NULL; s_walk = NULL; return; }
    s_depth = n;
    s_t0 = op_now();   /* clock-guard: wall — the mark's age is wall time, not a display instant */

    const char *at = getenv("OPENPETE_GLITCH_MARK_AT");
    if (at && *at) {
        const char *p = at;
        int kind = GM_AT_PRESENT;
        if (*p == 'p' || *p == 'P') { p++; }
        else if (*p == 'w' || *p == 'W') { kind = GM_AT_WALK; p++; }
        char *end = NULL;
        long v = strtol(p, &end, 10);
        if (end && (*end == 'm' || *end == 'M')) kind = GM_AT_MS;
        if (v > 0) { s_at_kind = kind; s_at_value = (uint32_t)v; }
        else OPL_WARN_F("glitch", "OPENPETE_GLITCH_MARK_AT=\"%s\" is not a trigger", at);
    }
}

int openpete_glitch_mark_depth(void) { return s_depth; }

void openpete_glitch_mark_note_walk(const openpete_glitch_walk_t *w)
{
    if (!s_depth || !w) return;
    const uint32_t n = atomic_load_explicit(&s_walk_n, memory_order_relaxed);
    gm_wslot *sl = &s_walk[n % (uint32_t)s_depth];
    const uint32_t g = atomic_load_explicit(&sl->seq, memory_order_relaxed) + 1u;
    atomic_store_explicit(&sl->seq, g, memory_order_relaxed);   /* odd: writing */
    atomic_thread_fence(memory_order_release);
    sl->r = *w;
    sl->r.ord = n + 1u;
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&sl->seq, g + 1u, memory_order_relaxed);
    atomic_store_explicit(&s_walk_n, n + 1u, memory_order_release);

    const uint32_t lg = atomic_load_explicit(&s_last_seq, memory_order_relaxed) + 1u;
    atomic_store_explicit(&s_last_seq, lg, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    s_last = sl->r;
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&s_last_seq, lg + 1u, memory_order_relaxed);
}

void openpete_glitch_mark_last_walk(openpete_glitch_walk_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!s_depth) return;
    for (int try = 0; try < 4; try++) {
        const uint32_t a = atomic_load_explicit(&s_last_seq, memory_order_acquire);
        if (a & 1u) continue;
        openpete_glitch_walk_t tmp = s_last;
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&s_last_seq, memory_order_relaxed) == a) { *out = tmp; return; }
    }
}

void openpete_glitch_mark_note_present(const openpete_glitch_present_t *p)
{
    if (!s_depth || !p) return;
    const uint32_t n = atomic_load_explicit(&s_pres_n, memory_order_relaxed);
    gm_pslot *sl = &s_pres[n % (uint32_t)s_depth];
    const uint32_t g = atomic_load_explicit(&sl->seq, memory_order_relaxed) + 1u;
    atomic_store_explicit(&sl->seq, g, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    sl->r = *p;
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&sl->seq, g + 1u, memory_order_relaxed);
    atomic_store_explicit(&s_pres_n, n + 1u, memory_order_release);
}

void openpete_glitch_mark_request(const char *reason)
{
    if (!s_depth) return;
    if (reason && *reason) { snprintf(s_reason, sizeof s_reason, "%s", reason); }
    atomic_store_explicit(&s_request, 1, memory_order_release);
}

/* ---- the dump ------------------------------------------------------- */

static const char *const s_kind_name[] = { "canonical", "intermediate", "inline" };
static const char *const s_shows_name[] = { "scene", "vram-fb", "psycross-ref", "clear" };

static const char *name_of(const char *const *tab, int n, int i)
{ return (i >= 0 && i < n) ? tab[i] : "?"; }

/* Camera facing in the game's angle units, 4096 to the turn, so a record
 * pastes straight into --freecam-pose. Row 2 of the camera matrix is the
 * forward axis and world component 1 is up, which makes pitch the arcsine of
 * the forward row's component 1 and yaw the heading of the other two.
 * Computed at the dump and never on the present path: the record keeps the
 * matrix, so a convention error costs a rerun of the reader, not of the
 * capture. */
static void cam_angles(const float M[9], float *yaw, float *pitch)
{
    const double k = 4096.0 / (2.0 * 3.14159265358979323846);
    const double fx = M[6], fup = M[7], fz = M[8];
    double y = atan2(-fx, fz) * k;
    while (y < 0.0) y += 4096.0;
    while (y >= 4096.0) y -= 4096.0;
    const double u = fup < -1.0 ? -1.0 : fup > 1.0 ? 1.0 : fup;
    *yaw = (float)y;
    *pitch = (float)(asin(u) * k);
}

/* Directory of the run's log file, or "." when there is no log sink. */
static void mark_dir(char *out, size_t cap)
{
    const char *lp = openpete_log_file_path();
    if (lp && *lp) {
        const char *slash = NULL;
        for (const char *p = lp; *p; p++) if (*p == '/' || *p == '\\') slash = p;
        if (slash && (size_t)(slash - lp) < cap) {
            memcpy(out, lp, (size_t)(slash - lp));
            out[slash - lp] = '\0';
            return;
        }
    }
    snprintf(out, cap, ".");
}

static void write_walk_line(FILE *fp, const openpete_glitch_walk_t *w, const char *tag)
{
    fprintf(fp, "%s ord=%u tick=%u sub=%d sectors=%d tris=%d nearFaces=%d "
                "nearTris=%d occl=%d\n",
            tag, w->ord, w->tick, w->sub, w->sectors, w->tris,
            w->near_faces, w->near_tris, w->occl_culled);
}

/* Copy one slot, retrying while its writer holds it. Returns 0 when the
 * record could not be read whole. */
static int read_pslot(gm_pslot *sl, openpete_glitch_present_t *out)
{
    for (int i = 0; i < 8; i++) {
        const uint32_t a = atomic_load_explicit(&sl->seq, memory_order_acquire);
        if (a & 1u) continue;
        *out = sl->r;
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&sl->seq, memory_order_relaxed) == a) return 1;
    }
    return 0;
}

static int read_wslot(gm_wslot *sl, openpete_glitch_walk_t *out)
{
    for (int i = 0; i < 8; i++) {
        const uint32_t a = atomic_load_explicit(&sl->seq, memory_order_acquire);
        if (a & 1u) continue;
        *out = sl->r;
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&sl->seq, memory_order_relaxed) == a) return 1;
    }
    return 0;
}

static int dump(char *path_out, size_t path_cap)
{
    const uint32_t pn = atomic_load_explicit(&s_pres_n, memory_order_acquire);
    const uint32_t wn = atomic_load_explicit(&s_walk_n, memory_order_acquire);

    /* Snapshot both rings before any file IO. The other thread keeps
     * presenting while the mark is serviced, and the slot holding the OLDEST
     * record is the next one it overwrites, so reading the ring straight
     * into the file loses the oldest record to a newer one. Copying first
     * narrows that window from the length of the write to a memcpy; the
     * ordinal check below catches whatever still slips through. */
    openpete_glitch_present_t *psnap = malloc((size_t)s_depth * sizeof *psnap);
    openpete_glitch_walk_t    *wsnap = malloc((size_t)s_depth * sizeof *wsnap);
    if (!psnap || !wsnap) {
        free(psnap); free(wsnap);
        OPL_WARN_F("glitch", "out of memory for the mark snapshot");
        return 0;
    }
    unsigned lost_p = 0, lost_w = 0;
    for (int i = 0; i < s_depth; i++)
        if (!read_pslot(&s_pres[i], &psnap[i])) psnap[i].serial = 0;
    for (int i = 0; i < s_depth; i++)
        if (!read_wslot(&s_walk[i], &wsnap[i])) wsnap[i].ord = 0;

    char dir[1024];
    mark_dir(dir, sizeof dir);
    char path[1200];
    snprintf(path, sizeof path, "%s/glitch-mark-%u.txt", dir, pn ? pn : wn);
    FILE *fp = fopen(path, "w");
    if (!fp) { OPL_WARN_F("glitch", "cannot write %s", path); return 0; }

    const double ms = (double)(op_now() - s_t0) / 1e6;   /* clock-guard: wall */
    fprintf(fp, "# openpete glitch mark\n");
    fprintf(fp, "mark reason=%s at_ms=%.1f depth=%d presents_total=%u walks_total=%u\n",
            s_reason, ms, s_depth, pn, wn);

    const uint32_t plive = pn < (uint32_t)s_depth ? pn : (uint32_t)s_depth;
    fprintf(fp, "presents n=%u\n", plive);
    fprintf(fp, "# P serial tick kind shows alpha paired stamp tris tercache gridage "
                "cam yaw pitch | walk census of the scene composed\n");
    for (uint32_t i = 0; i < plive; i++) {
        const uint32_t want = pn - plive + i + 1u;   /* serial this slot must hold */
        const openpete_glitch_present_t r = psnap[(want - 1u) % (uint32_t)s_depth];
        if (r.serial != want) {
            /* The slot was recycled or unreadable between the mark and the
             * snapshot; the record that belonged to this instant is gone. */
            fprintf(fp, "P lost serial=%u\n", want);
            lost_p++;
            continue;
        }
        float yaw = 0.f, pitch = 0.f;
        if (r.cam_valid) cam_angles(r.cam_M, &yaw, &pitch);
        fprintf(fp, "P serial=%u tick=%u kind=%s shows=%s alpha=%.4f paired=%d "
                    "stamp=%08x tris=%d tercache=%d gridage=%d ",
                r.serial, r.tick,
                name_of(s_kind_name, 3, r.kind), name_of(s_shows_name, 4, r.shows),
                (double)r.alpha, r.paired, r.stamp, r.tris, r.tercache, r.grid_age);
        if (r.cam_valid)
            fprintf(fp, "cam=(%.0f, %.0f, %.0f) yaw=%.0f pitch=%.0f ",
                    (double)r.cam_pos[0], (double)r.cam_pos[1], (double)r.cam_pos[2],
                    (double)yaw, (double)pitch);
        else
            fprintf(fp, "cam=none ");
        fprintf(fp, "| wOrd=%u wTick=%u wSub=%d wSectors=%d wTris=%d wNearFaces=%d "
                    "wNearTris=%d wOccl=%d\n",
                r.walk.ord, r.walk.tick, r.walk.sub, r.walk.sectors, r.walk.tris,
                r.walk.near_faces, r.walk.near_tris, r.walk.occl_culled);
    }

    const uint32_t wlive = wn < (uint32_t)s_depth ? wn : (uint32_t)s_depth;
    fprintf(fp, "walks n=%u\n", wlive);
    fprintf(fp, "# W ord tick sub sectors tris nearFaces nearTris occl\n");
    for (uint32_t i = 0; i < wlive; i++) {
        const uint32_t want = wn - wlive + i + 1u;
        const openpete_glitch_walk_t r = wsnap[(want - 1u) % (uint32_t)s_depth];
        if (r.ord != want) { fprintf(fp, "W lost ord=%u\n", want); lost_w++; continue; }
        write_walk_line(fp, &r, "W");
    }
    fprintf(fp, "lost presents=%u walks=%u\n", lost_p, lost_w);
    fclose(fp);
    free(psnap); free(wsnap);
    s_marks++;
    snprintf(path_out, path_cap, "%s", path);
    OPL_INFO_F("glitch", "glitch mark (%s): %u present(s), %u walk(s) -> %s",
               s_reason, plive, wlive, path);
    /* The file name is the one thing the person needs off the screen: the
     * run may have no terminal, and the name carries the present serial. */
    const char *base = path;
    for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    openpete_osd_notify("host", "glitch.mark", 4000, "GLITCH MARK: %s", base);
    return 1;
}

int openpete_glitch_mark_service(char *path_out, size_t path_cap)
{
    if (!s_depth || !path_out || !path_cap) return 0;
    if (s_at_kind != GM_AT_OFF) {
        uint32_t cur = 0;
        switch (s_at_kind) {
            case GM_AT_PRESENT: cur = atomic_load_explicit(&s_pres_n, memory_order_acquire); break;
            case GM_AT_WALK:    cur = atomic_load_explicit(&s_walk_n, memory_order_acquire); break;
            default:            cur = (uint32_t)((op_now() - s_t0) / 1000000);   /* clock-guard: wall */
                                break;
        }
        if (cur >= s_at_value) {
            s_at_kind = GM_AT_OFF;      /* one shot: the trigger names one instant */
            openpete_glitch_mark_request("env");
        }
    }
    if (!atomic_exchange_explicit(&s_request, 0, memory_order_acq_rel)) return 0;
    return dump(path_out, path_cap);
}
