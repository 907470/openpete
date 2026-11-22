/* pace_log.c - unified pacing event log + always-on pacing census.
 * See openpete_pace_log.h for the format and the reasoning behind it.
 */

#include "openpete_mutex_once.h"
#include "openpete_pace_log.h"
#include "openpete_present_thread.h"
#include "openpete_time.h"
#include "openpete_host_input.h"
#include "openpete_log.h"
#include "openpete_config.h"           /* openpete_config_env */
#include "openpete_native_render.h"   /* refusal counters for the present recorder */
#include "openpete_interp.h"          /* present-detail interpolation mode */
#include "openpete_interp_objs.h"     /* present-detail tick stamp */
#include "rhi.h"                      /* block-time phases */
#include "openpete_fiber.h"           /* openpete_frame: the present injection's VBL window */

#include <SDL3/SDL.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef OPENPETE_BUILD_ID
#define OPENPETE_BUILD_ID "unknown"
#endif

/* ------------------------------------------------------------------ */
/* writer                                                             */
/* ------------------------------------------------------------------ */

/* Records arrive from two threads under the host split - the sim worker emits
 * VBL/PRESENT/SLIP, main emits INPUT_EDGE and the resize windows - so the
 * append is locked. At ~500 records/s the mutex is far below the noise floor
 * of what is being measured; a lock-free ring would buy nothing but a class of
 * bug the instrument cannot afford. */
#define PACE_BUF_RECORDS 2048

typedef struct {
    uint8_t  type;
    uint8_t  flags;
    uint16_t aux;
    uint32_t index;
    int64_t  t_ns;
    int64_t  a, b, c, d, e, f;
} pace_rec_t;

/* The on-disk record must be exactly what the header advertises: a compiler
 * that pads pace_rec_t would write a file no reader can parse, silently. */
_Static_assert(sizeof(pace_rec_t) == OP_PACE_RECORD_BYTES,
               "pace_rec_t must match OP_PACE_RECORD_BYTES");

/* The open log file. Atomic: pace_sink_on tests it without s_lock on the
 * emitting threads, and a non-NULL load also makes s_lock, created before
 * the store, visible to them. */
static _Atomic(FILE*) s_fp;
static SDL_Mutex*  s_lock;
static pace_rec_t  s_buf[PACE_BUF_RECORDS];
static unsigned    s_buf_n;
static uint64_t    s_written;
static uint32_t    s_present_seq;
static int64_t     s_next_clocksample_ns;

/* CLOCKSAMPLE cadence: enough to fit a drift slope over a soak run, rare
 * enough to cost nothing. */
#define PACE_CLOCKSAMPLE_PERIOD_NS ((int64_t)10 * 1000 * 1000 * 1000)

static openpete_pace_census_t s_census;

const int64_t openpete_pace_hist_edges_ns[OP_PACE_HIST_BUCKETS] = {
    0,          /* early: started before the deadline                     */
    100000,     /* 100 µs - the target band                               */
    250000,     /* 250 µs - the p99 start-phase goal                      */
    500000,
    1000000,
    2000000,
    5000000,
    16715111,   /* one VBL: past here the tick is visibly late            */
    0           /* unbounded                                              */
};

/* The always-on ring: the newest PACE_RING_RECORDS records, kept whether or
 * not a log file is open, so a bug report carries the last 30 s of pacing.
 * Written by pace_emit under s_lock beside the file buffer; read under
 * s_lock by openpete_pace_ring_image. */
static pace_rec_t*  s_ring;
static uint64_t     s_ring_n;          /* records appended */
static uint64_t     s_ring_young_lost; /* overwritten while inside the window */
static _Atomic int  s_ring_on;
static char*        s_ring_env;
static unsigned     s_ring_env_bytes;
static int64_t      s_ring_created_unix_ns;
static int64_t      s_ring_epoch_ns;

/* The coarse session ring (openpete_pace_log.h): rows indexed by
 * (t_ns - s_ring_epoch_ns) / 1 s / s_coarse_span, folded by pace_emit under
 * s_lock from every record it stores. */
static uint32_t (*s_coarse)[OP_PACE_CR_FIELDS];
static uint32_t     s_coarse_used;     /* rows from row 0 to the newest written */
static uint32_t     s_coarse_span = 1; /* seconds per row */
static int64_t      s_coarse_last_present_ns;

int openpete_pace_log_active(void) { return s_fp != NULL; }

/** @brief Return whether any sink takes records: the log file or the ring. */
static int pace_sink_on(void)
{
    return atomic_load_explicit(&s_fp, memory_order_acquire) != NULL ||
           atomic_load_explicit(&s_ring_on, memory_order_acquire);
}

/** @brief Store one record in the ring over its oldest; the caller holds the writer lock.
 *
 * An overwritten record younger than OP_PACE_RING_WINDOW_NS against the new
 * one is counted in s_ring_young_lost: the ring then holds less than the
 * window. */
static void ring_put_locked(const pace_rec_t* r)
{
    pace_rec_t* slot = &s_ring[s_ring_n % OP_PACE_RING_RECORDS];
    if (s_ring_n >= OP_PACE_RING_RECORDS && r->t_ns - slot->t_ns < OP_PACE_RING_WINDOW_NS)
        s_ring_young_lost++;
    *slot = *r;
    s_ring_n++;
}

/** @brief Write the buffered records to the log file and empty the buffer; the caller holds the writer lock.
 *
 * A short write logs an error, closes the file and disables further
 * logging. The buffer is emptied in every case. */
static void pace_flush_locked(void)
{
    if (s_buf_n && s_fp) {
        const size_t n = fwrite(s_buf, sizeof(pace_rec_t), s_buf_n, s_fp);
        s_written += n;
        if (n != s_buf_n) {
            /* Disk full or a closed pipe. Say so once and stop: silently
             * dropping records would leave a short log that still parses,
             * and a truncated pacing log reads as a clean short run. */
            OPL_ERROR_F("pace", "write failed after %llu record(s) - closing",
                        (unsigned long long)s_written);
            fclose(s_fp);
            s_fp = NULL;
        }
    }
    s_buf_n = 0;
}

/* The census counters below are touched by the present thread as well as the
 * sim thread, and unlike the record stream they exist whether or not a log
 * file was opened -- so they get their own lock rather than borrowing the
 * writer's. Uncontended in practice: two threads, one short update each. */
static void* s_census_lock;   /* SDL_Mutex, openpete_mutex_once */

/** @brief Take the census lock, creating it on first use without a creation race. */
static void census_lock(void)
{
    SDL_LockMutex(openpete_mutex_once(&s_census_lock));
}

/** @brief Release the census lock. */
static void census_unlock(void)
{
    SDL_UnlockMutex(openpete_mutex_once(&s_census_lock));
}

/** @brief Add v to a u32 row field, saturating at UINT32_MAX.
 * @param f The field.
 * @param v The value to add. */
static void cr_add(uint32_t* f, uint64_t v)
{
    const uint64_t t = (uint64_t)*f + v;
    *f = t > UINT32_MAX ? UINT32_MAX : (uint32_t)t;
}

/** @brief Raise a u32 row field to v when v is larger, saturating at UINT32_MAX.
 * @param f The field.
 * @param v The candidate. */
static void cr_max(uint32_t* f, uint64_t v)
{
    if (v > UINT32_MAX) v = UINT32_MAX;
    if (v > *f) *f = (uint32_t)v;
}

/** @brief Merge adjacent coarse rows pairwise into the lower half and double the row span; the caller holds the writer lock.
 *
 * Counts and sums add, maxima take the larger; the merged row starts at the
 * first row's start and covers twice the span. */
static void coarse_halve_locked(void)
{
    const uint32_t half = OP_PACE_COARSE_ROWS / 2u;
    for (uint32_t i = 0; i < half; i++) {
        uint32_t* d = s_coarse[i];
        const uint32_t* a = s_coarse[2u * i];
        const uint32_t* b = s_coarse[2u * i + 1u];
        uint32_t m[OP_PACE_CR_FIELDS];
        for (int k = 0; k < OP_PACE_CR_FIELDS; k++) {
            const int is_max = k == OP_PACE_CR_IV_MAX_US || k == OP_PACE_CR_TICK_US_MAX ||
                               k == OP_PACE_CR_LEVEL_MAX || k == OP_PACE_CR_GPU_US_MAX;
            m[k] = a[k];
            if (is_max) cr_max(&m[k], b[k]); else cr_add(&m[k], b[k]);
        }
        m[OP_PACE_CR_T0_S] = 2u * i * s_coarse_span;
        m[OP_PACE_CR_SPAN_S] = 2u * s_coarse_span;
        memcpy(d, m, sizeof m);
    }
    memset(s_coarse[half], 0, (size_t)half * sizeof s_coarse[0]);
    s_coarse_span *= 2u;
    s_coarse_used = (s_coarse_used + 1u) / 2u;
}

/** @brief Return the microseconds in the low (hi = 0) or high (hi = 1) u32 word of a split slot.
 * @param v The slot.
 * @param hi Which word.
 * @return The word. */
static uint32_t split_word(int64_t v, int hi)
{
    return (uint32_t)((uint64_t)v >> (hi ? 32 : 0));
}

/** @brief Fold one record into its coarse row; the caller holds the writer lock.
 * @param r The record. */
static void coarse_fold_locked(const pace_rec_t* r)
{
    if (r->t_ns < s_ring_epoch_ns) return;
    uint64_t sec = (uint64_t)(r->t_ns - s_ring_epoch_ns) / 1000000000u;
    while (sec / s_coarse_span >= OP_PACE_COARSE_ROWS) coarse_halve_locked();
    const uint32_t i = (uint32_t)(sec / s_coarse_span);
    uint32_t* row = s_coarse[i];
    if (i + 1u > s_coarse_used) s_coarse_used = i + 1u;
    row[OP_PACE_CR_T0_S] = i * s_coarse_span;
    row[OP_PACE_CR_SPAN_S] = s_coarse_span;
    switch (r->type) {
    case OP_PACE_VBL:
        cr_add(&row[OP_PACE_CR_VBLS], 1);
        if (r->flags & OP_PACE_F_NOT_LIVE) cr_add(&row[OP_PACE_CR_VBLS_NOT_LIVE], 1);
        break;
    case OP_PACE_PRESENT: {
        cr_add(&row[r->flags == OP_PACE_REPEAT ? OP_PACE_CR_REPEATS : OP_PACE_CR_PRESENTS], 1);
        if (s_coarse_last_present_ns && r->t_ns > s_coarse_last_present_ns) {
            const int64_t iv = r->t_ns - s_coarse_last_present_ns;
            cr_max(&row[OP_PACE_CR_IV_MAX_US], (uint64_t)(iv / 1000));
            if (iv > 25000000) cr_add(&row[OP_PACE_CR_IV_LONG], 1);
        }
        s_coarse_last_present_ns = r->t_ns;
        break;
    }
    case OP_PACE_SHED: cr_add(&row[OP_PACE_CR_SHEDS], 1); break;
    case OP_PACE_SLIP: cr_add(&row[OP_PACE_CR_SLIPS], 1); break;
    case OP_PACE_TICK_SPLIT:
        cr_add(&row[OP_PACE_CR_TICKS], 1);
        cr_add(&row[OP_PACE_CR_TICK_US_SUM], split_word(r->f, 0));
        cr_max(&row[OP_PACE_CR_TICK_US_MAX], split_word(r->f, 0));
        cr_add(&row[OP_PACE_CR_SIM_US_SUM], split_word(r->a, 0));
        cr_add(&row[OP_PACE_CR_EXTRACT_US_SUM], split_word(r->a, 1));
        cr_add(&row[OP_PACE_CR_TERRAIN_US_SUM], split_word(r->b, 0));
        cr_add(&row[OP_PACE_CR_MOBY_US_SUM], split_word(r->b, 1));
        cr_add(&row[OP_PACE_CR_SNAP_US_SUM], (uint64_t)split_word(r->c, 0) + split_word(r->c, 1));
        cr_add(&row[OP_PACE_CR_RENDER_US_SUM], split_word(r->d, 0));
        cr_add(&row[OP_PACE_CR_PRESENT_US_SUM], split_word(r->d, 1));
        cr_add(&row[OP_PACE_CR_EXTRACTS], (uint64_t)r->e & 0xFFFFu);
        cr_add(&row[OP_PACE_CR_HOST_US_SUM], split_word(r->f, 1));
        break;
    case OP_PACE_EAGER_PLAN:
        cr_add(&row[OP_PACE_CR_PLANS], 1);
        cr_add(&row[OP_PACE_CR_PLAN_SLOTS], (uint64_t)r->f & 0xFFFFu);
        cr_add(&row[OP_PACE_CR_PLAN_SCENES], ((uint64_t)r->f >> 32) & 0xFFFFu);
        if (r->flags & OP_PACE_EP_CUT) cr_add(&row[OP_PACE_CR_PLAN_CUTS], 1);
        cr_max(&row[OP_PACE_CR_LEVEL_MAX], r->aux & 0xFFu);
        break;
    case OP_PACE_GPU_FRAME:
        cr_add(&row[OP_PACE_CR_GPU_FRAMES], 1);
        cr_add(&row[OP_PACE_CR_GPU_US_SUM], (uint64_t)(r->a / 1000));
        cr_max(&row[OP_PACE_CR_GPU_US_MAX], (uint64_t)(r->a / 1000));
        break;
    default: break;
    }
}

/** @brief Append one record to the ring and to the file's write buffer, flushing the buffer when it fills.
 *
 * Does nothing when neither the ring is armed nor a log file is open. The
 * file check is repeated under the writer lock because a close on another
 * thread can clear the file between the first test and the lock. */
static void pace_emit(const pace_rec_t* r)
{
    if (!pace_sink_on()) return;
    SDL_LockMutex(s_lock);
    if (s_ring) ring_put_locked(r);
    if (s_coarse) coarse_fold_locked(r);
    if (s_fp) {                 /* re-check: close() may have won the race */
        s_buf[s_buf_n++] = *r;
        if (s_buf_n == PACE_BUF_RECORDS) pace_flush_locked();
    }
    SDL_UnlockMutex(s_lock);
}

/** @brief Write the fixed 64-byte file header into hdr.
 *
 * @param hdr Receives OP_PACE_HEADER_FIXED bytes.
 * @param env_bytes Bytes of the environment block that follows.
 * @param created_unix_ns Wall-clock creation time, ns since the Unix epoch.
 * @param epoch_ns The op_now() instant the log's timestamps are read against. */
static void pace_header_fill(uint8_t hdr[OP_PACE_HEADER_FIXED], unsigned env_bytes,
                             int64_t created_unix_ns, int64_t epoch_ns)
{
    memset(hdr, 0, OP_PACE_HEADER_FIXED);
    uint32_t u32;
    int64_t  i64;
    unsigned o = 0;
#define PUT32(v) do { u32 = (uint32_t)(v); memcpy(hdr + o, &u32, 4); o += 4; } while (0)
#define PUT64(v) do { i64 = (int64_t)(v);  memcpy(hdr + o, &i64, 8); o += 8; } while (0)
    PUT32(OP_PACE_MAGIC);
    PUT32(OP_PACE_VERSION);
    PUT32(OP_PACE_HEADER_FIXED + env_bytes);
    PUT32(OP_PACE_RECORD_BYTES);
    PUT32(env_bytes);
    PUT32(0);
    PUT64(created_unix_ns);
    PUT64(epoch_ns);
    PUT64(OP_VBL_HZ_NUM);
    PUT64(OP_VBL_HZ_DEN);
    PUT64(OP_TICK_VBLS);
#undef PUT32
#undef PUT64
}

int openpete_pace_ring_arm(const char* env)
{
    if (atomic_load_explicit(&s_ring_on, memory_order_acquire)) return 0;
    const char* off = getenv("OPENPETE_PACE_RING");
    if (off && *off == '0') {
        OPL_INFO_F("pace", "%s", "OPENPETE_PACE_RING=0: no pace ring");
        return -1;
    }
    if (!s_lock) s_lock = SDL_CreateMutex();
    pace_rec_t* ring = s_lock ? calloc(OP_PACE_RING_RECORDS, sizeof *ring) : NULL;
    const size_t env_bytes = env ? strlen(env) : 0u;
    char* env_copy = ring ? malloc(env_bytes + 1u) : NULL;
    if (!env_copy) {
        free(ring);
        OPL_ERROR_F("pace", "%s", "pace ring allocation failed; no pace ring");
        return -1;
    }
    if (env_bytes) memcpy(env_copy, env, env_bytes);
    env_copy[env_bytes] = '\0';
    /* The coarse rows are optional: without them the 30 s ring still runs. */
    uint32_t (*coarse)[OP_PACE_CR_FIELDS] = calloc(OP_PACE_COARSE_ROWS, sizeof *coarse);
    SDL_Time wall_ns = 0;
    SDL_GetCurrentTime(&wall_ns);
    SDL_LockMutex(s_lock);
    s_ring = ring;
    s_ring_env = env_copy;
    s_ring_env_bytes = (unsigned)env_bytes;
    s_ring_created_unix_ns = (int64_t)wall_ns;
    s_ring_epoch_ns = op_now();
    s_coarse = coarse;
    SDL_UnlockMutex(s_lock);
    if (!s_fp) s_next_clocksample_ns = op_now();
    atomic_store_explicit(&s_ring_on, 1, memory_order_release);
    return 0;
}

int openpete_pace_ring_image(int64_t window_ns, void** out, size_t* len, size_t* records)
{
    *out = NULL; *len = 0;
    if (records) *records = 0;
    if (!atomic_load_explicit(&s_ring_on, memory_order_acquire)) return -1;
    const size_t cap = OP_PACE_HEADER_FIXED + (size_t)s_ring_env_bytes +
                       (size_t)OP_PACE_RING_RECORDS * sizeof(pace_rec_t);
    uint8_t* img = malloc(cap);
    if (!img) return -1;
    pace_header_fill(img, s_ring_env_bytes, s_ring_created_unix_ns, s_ring_epoch_ns);
    memcpy(img + OP_PACE_HEADER_FIXED, s_ring_env, s_ring_env_bytes);
    pace_rec_t* dst = (pace_rec_t*)(void*)(img + OP_PACE_HEADER_FIXED + s_ring_env_bytes);
    size_t n = 0;
    SDL_LockMutex(s_lock);
    const uint64_t held = s_ring_n < OP_PACE_RING_RECORDS ? s_ring_n : OP_PACE_RING_RECORDS;
    const int64_t cutoff = window_ns > 0 ? (int64_t)op_now() - window_ns : INT64_MIN;
    for (uint64_t k = s_ring_n - held; k < s_ring_n; k++) {
        const pace_rec_t* r = &s_ring[k % OP_PACE_RING_RECORDS];
        if (r->t_ns >= cutoff) memcpy(&dst[n++], r, sizeof *r);
    }
    SDL_UnlockMutex(s_lock);
    *out = img;
    *len = OP_PACE_HEADER_FIXED + (size_t)s_ring_env_bytes + n * sizeof(pace_rec_t);
    if (records) *records = n;
    return 0;
}

int openpete_pace_coarse_image(void** out, size_t* len, size_t* rows)
{
    *out = NULL; *len = 0;
    if (rows) *rows = 0;
    if (!atomic_load_explicit(&s_ring_on, memory_order_acquire) || !s_coarse) return -1;
    const size_t hdr = 40u, rb = (size_t)OP_PACE_CR_FIELDS * 4u;
    uint8_t* img = malloc(hdr + (size_t)OP_PACE_COARSE_ROWS * rb);
    if (!img) return -1;
    SDL_LockMutex(s_lock);
    const uint32_t n = s_coarse_used, span = s_coarse_span;
    memcpy(img + hdr, s_coarse, (size_t)n * rb);
    const int64_t at = (int64_t)op_now() - s_ring_epoch_ns;
    SDL_UnlockMutex(s_lock);
    const uint32_t hv[4] = { OP_PACE_COARSE_VERSION, (uint32_t)rb, n, span };
    memcpy(img, OP_PACE_COARSE_MAGIC, 8);
    memcpy(img + 8, hv, sizeof hv);
    memcpy(img + 24, &s_ring_created_unix_ns, 8);
    memcpy(img + 32, &at, 8);
    *out = img;
    *len = hdr + (size_t)n * rb;
    if (rows) *rows = n;
    return 0;
}

void openpete_pace_ring_counts(openpete_pace_ring_counts_t* out)
{
    memset(out, 0, sizeof *out);
    if (!atomic_load_explicit(&s_ring_on, memory_order_acquire)) return;
    SDL_LockMutex(s_lock);
    out->appended = s_ring_n;
    out->held = s_ring_n < OP_PACE_RING_RECORDS ? s_ring_n : OP_PACE_RING_RECORDS;
    out->overwritten = s_ring_n - out->held;
    out->young_lost = s_ring_young_lost;
    if (out->held) {
        const pace_rec_t* oldest = &s_ring[(s_ring_n - out->held) % OP_PACE_RING_RECORDS];
        const pace_rec_t* newest = &s_ring[(s_ring_n - 1u) % OP_PACE_RING_RECORDS];
        out->span_ns = newest->t_ns - oldest->t_ns;
    }
    SDL_UnlockMutex(s_lock);
    out->bytes = (size_t)OP_PACE_RING_RECORDS * sizeof(pace_rec_t) + s_ring_env_bytes;
}

int openpete_pace_log_open(const char* path, const char* env)
{
    if (!path || !*path) return -1;
    if (s_fp) return 0;

    const unsigned env_bytes = env ? (unsigned)strlen(env) : 0u;
    FILE* fp = fopen(path, "wb");
    if (!fp) {
        OPL_ERROR_F("pace", "cannot open %s for writing", path);
        return -1;
    }

    SDL_Time wall_ns = 0;
    SDL_GetCurrentTime(&wall_ns);          /* ns since the Unix epoch */
    const int64_t created_unix_ns = (int64_t)wall_ns;

    uint8_t hdr[OP_PACE_HEADER_FIXED];
    pace_header_fill(hdr, env_bytes, created_unix_ns, op_now());

    if (fwrite(hdr, 1, sizeof(hdr), fp) != sizeof(hdr) ||
        (env_bytes && fwrite(env, 1, env_bytes, fp) != env_bytes)) {
        OPL_ERROR_F("pace", "header write to %s failed", path);
        fclose(fp);
        return -1;
    }

    if (!s_lock) s_lock = SDL_CreateMutex();
    if (!s_lock) {
        OPL_ERROR_F("pace", "%s", "SDL_CreateMutex failed");
        fclose(fp);
        return -1;
    }
    s_fp = fp;
    s_next_clocksample_ns = op_now();
    OPL_INFO_F("pace", "logging to %s (%u B env)", path, env_bytes);
    return 0;
}

void openpete_pace_log_close(void)
{
    if (!s_fp) return;
    SDL_LockMutex(s_lock);
    pace_flush_locked();
    FILE* fp = s_fp;
    s_fp = NULL;                /* emitters bail out from here on */
    SDL_UnlockMutex(s_lock);
    fflush(fp);
    fclose(fp);
    OPL_INFO_F("pace", "wrote %llu record(s)", (unsigned long long)s_written);
}

/* ------------------------------------------------------------------ */
/* environment block                                                  */
/* ------------------------------------------------------------------ */

#if defined(__linux__)
/** @brief Read the first line of a sysfs or proc file into out, without its newline.
 *
 * An absent file is a normal case and leaves out unchanged; the caller keeps
 * its default. A file with no readable line returns -1 with out
 * unspecified. */
static int read_first_line(const char* path, char* out, unsigned n)
{
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    int ok = fgets(out, (int)n, f) != NULL;
    fclose(f);
    if (!ok) return -1;
    char* nl = strchr(out, '\n');
    if (nl) *nl = '\0';
    return 0;
}
#endif

/** @brief Write the compositor identity for the environment block into out.
 *
 * The compositor owns the swapchain's release cadence, so it is part of the
 * machine: the same hardware under a different compositor presents on a
 * different schedule, and two logs that differ only here would otherwise
 * compare as one environment. The value is SDL's current video driver name,
 * "none" when there is none; on Linux "/<XDG_CURRENT_DESKTOP>" is appended
 * when that variable is set, since the driver says wayland or x11 but not
 * whose. */
static void compositor_id(char* out, unsigned n)
{
    const char* drv = SDL_GetCurrentVideoDriver();
    if (!drv || !drv[0]) {
        SDL_strlcpy(out, "none", n);
        return;
    }
    SDL_strlcpy(out, drv, n);
#if defined(__linux__)
    {
        /* The video driver says wayland or x11; the desktop says whose
         * wayland it is, and KWin and Mutter do not schedule alike. */
        const char* de = SDL_getenv("XDG_CURRENT_DESKTOP");
        if (de && de[0]) {
            SDL_strlcat(out, "/", n);
            SDL_strlcat(out, de, n);
        }
    }
#endif
}

/** @brief Write the CPU model name into out: the first "model name" line of /proc/cpuinfo on Linux, else "unknown".
 *
 * @param out Receives the NUL-terminated name, truncated to fit.
 * @param n Size of out. */
static void cpu_model(char* out, unsigned n)
{
    out[0] = '\0';
#if defined(__linux__)
    FILE* f = fopen("/proc/cpuinfo", "rb");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "model name", 10) == 0) {
                const char* c = strchr(line, ':');
                if (c) {
                    while (*++c == ' ') { }
                    SDL_strlcpy(out, c, n);
                    char* nl = strchr(out, '\n');
                    if (nl) *nl = '\0';
                }
                break;
            }
        }
        fclose(f);
    }
#endif
    if (!out[0]) SDL_strlcpy(out, "unknown", n);
}

void openpete_pace_env_build(char* buf, unsigned buf_bytes,
                             const char* gpu, const char* driver,
                             int refresh_mhz, const char* present_mode,
                             int fullscreen, int render_fps,
                             int interp_on, int runahead_n,
                             double tick_scale,
                             int thread_split, int input_legacy,
                             int headless)
{
    char cpu[256];
    char gov[64] = "unknown";
    char comp[96];
    cpu_model(cpu, sizeof(cpu));
    compositor_id(comp, sizeof(comp));
#if defined(__linux__)
    read_first_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",
                    gov, sizeof(gov));
#endif

    /* The pacer's own slack is part of the environment: two runs with
     * different OPENPETE_PACER_SLACK_US are not comparable, and the difference
     * is invisible in every other field. */
    SDL_snprintf(buf, buf_bytes,
        "build=%s\n"
        "platform=%s\n"
        "cpu=%s\n"
        "cpu_count=%d\n"
        "governor=%s\n"
        "gpu=%s\n"
        "driver=%s\n"
        "refresh_mhz=%d\n"
        "compositor=%s\n"
        "present_mode=%s\n"
        "fullscreen=%d\n"
        "render_fps=%d\n"
        "interp=%d\n"
        "runahead=%d\n"
        "tick_scale=%.6f\n"
        "pacer_slack_ns=%" SDL_PRIs64 "\n"
        "virtual_clock=%d\n"
        /* The three fields that decide whether an INPUT metric from this log
         * means anything. With the split off, producer and consumer are the
         * same thread in the same iteration and input staleness is
         * microseconds by construction; the legacy path never latches at all;
         * headless has no display and forces the split off. Without these in
         * the header the analyzer cannot tell which regime it is reading, and
         * a baseline would key two different regimes to the same machine. */
        "thread_split=%d\n"
        "input_legacy=%d\n"
        "headless=%d\n"
        "input_pulse=%d\n"
        "input_pulse_bit=0x%04X\n"
        "input_pulse_hold_us=%d\n"
        /* Eager production moves every present, and every flip wait, onto a
         * thread of their own: a FLIP record then covers the tick boundary
         * as well as mid-tick, which the analyzer must know to scope its
         * display-side statements. */
        "eager=%d\n"
        "eager_lattice=%d\n",
        OPENPETE_BUILD_ID,
        SDL_GetPlatform(),
        cpu,
        SDL_GetNumLogicalCPUCores(),
        gov,
        gpu ? gpu : "unknown",
        driver ? driver : "unknown",
        refresh_mhz,
        comp,
        present_mode ? present_mode : "unknown",
        fullscreen, render_fps, interp_on, runahead_n, tick_scale,
        (Sint64)op_pacer_slack_ns(),
        op_virtual_clock_active(),
        thread_split, input_legacy, headless,
        openpete_host_input_pulse_count(),
        openpete_host_input_pulse_bit(),
        openpete_host_input_pulse_hold_us(),
        openpete_present_thread_enabled(),
        openpete_present_thread_lattice_enabled());
}

/* ------------------------------------------------------------------ */
/* emit                                                               */
/* ------------------------------------------------------------------ */

/** @brief Write a CLOCKSAMPLE record pairing op_now() with the wall clock, at most once per 10 s.
 *
 * caller_ns only decides whether a sample is due; both clocks are read back
 * to back inside the call, because pairing the caller's older timestamp with
 * a fresh wall read would add a load-dependent offset indistinguishable
 * from the drift the record exists to measure. No-op without a sink. */
static void maybe_clocksample(int64_t caller_ns)
{
    if (!pace_sink_on() || caller_ns < s_next_clocksample_ns) return;
    s_next_clocksample_ns = caller_ns + PACE_CLOCKSAMPLE_PERIOD_NS;

    /* The pair the analyzer's host-clock check needs: the monotonic source
     * the pacer steps against, and a precise wall clock, read back to back
     * HERE.
     *
     * Both must be sampled at the same instant. The caller's timestamp is a
     * tick body older than this call - fiber, presents and captures happened
     * in between - so pairing it with a fresh wall read would put a variable,
     * load-correlated offset into every sample, which is indistinguishable
     * from clock drift in the very fit this record exists to feed. */
    const int64_t mono_ns = op_now();
    SDL_Time wall_ns = 0;
    SDL_GetCurrentTime(&wall_ns);
    pace_rec_t r = {0};
    r.type = OP_PACE_CLOCKSAMPLE;
    r.t_ns = mono_ns;
    r.a    = mono_ns;
    r.b    = (int64_t)wall_ns;
    pace_emit(&r);
}

/** @brief Return the phase-error histogram bucket of err_ns.
 *
 * The first bucket whose upper edge in openpete_pace_hist_edges_ns exceeds
 * the error; the last bucket takes everything at or past the final edge. A
 * negative error (started early) lands in bucket 0. */
static unsigned hist_bucket(int64_t err_ns)
{
    for (unsigned i = 0; i < OP_PACE_HIST_BUCKETS - 1; i++)
        if (err_ns < openpete_pace_hist_edges_ns[i]) return i;
    return OP_PACE_HIST_BUCKETS - 1;
}

/* Pad-read probe state. Written and read only on the thread that runs the
 * game fiber, so no atomics: the dispatcher, the VBL bracket and the VBL
 * emit are all that thread. */
static uint32_t s_vbl_index;
static int64_t  s_vbl_exec_start_ns;
static int64_t  s_vbl_latch_ns;
static unsigned s_vbl_pad_reads;
static int      s_pad_probe_armed;
/* Open between vbl_begin and the VBL emit; suspended for fiber runs that
 * belong to a different tick (speculation, rewind replay). A read outside
 * both is real and is counted, but it is not evidence about this VBL. */
static int      s_vbl_bracket_open;
static int      s_vbl_suspend;

void openpete_pace_vbl_suspend(int on) { s_vbl_suspend = on ? 1 : 0; }

void openpete_pace_pad_probe_arm(void)   { s_pad_probe_armed = 1; }
int  openpete_pace_pad_probe_armed(void) { return s_pad_probe_armed; }

void openpete_pace_vbl_begin(uint32_t vbl_index, int64_t exec_start_ns,
                              int64_t latch_ns)
{
    s_vbl_index         = vbl_index;
    s_vbl_exec_start_ns = exec_start_ns;
    s_vbl_latch_ns      = latch_ns;
    s_vbl_pad_reads     = 0;
    s_vbl_bracket_open  = 1;
    s_vbl_suspend       = 0;
}

uint16_t openpete_pace_pad_reads_this_vbl(void)
{
    return s_vbl_pad_reads > 0xFFFFu ? 0xFFFFu : (uint16_t)s_vbl_pad_reads;
}

void openpete_pace_pad_read(void)
{
    const int64_t now  = (int64_t)op_now();
    const int     live = s_vbl_bracket_open && !s_vbl_suspend;

    if (!live) {
        /* A real read, by a fiber run that is not the VBL this bracket names.
         * Counting it in `pad_reads` would break the "exactly one per VBL"
         * result, and measuring its offset would place it in a period it
         * never ran in. Recorded, so it is visible, and excluded. */
        census_lock();
        s_census.pad_reads_unattributed++;
        census_unlock();
        if (!pace_sink_on()) return;
        pace_rec_t ur = {0};
        ur.type  = OP_PACE_PAD_READ;
        ur.flags = OP_PACE_PR_UNATTRIBUTED;
        ur.index = s_vbl_index;
        ur.t_ns  = now;
        pace_emit(&ur);
        return;
    }

    const unsigned n = ++s_vbl_pad_reads;

    census_lock();
    s_census.pad_reads++;
    if (n == 1) {
        s_census.pad_read_vbls++;
        /* Only the FIRST read of a VBL is a phase measurement: later ones are
         * the game looking again at bytes that cannot have changed, since
         * nothing rewrites the buffer between the latch and the next latch. */
        if (s_vbl_exec_start_ns) {
            const int64_t off = now - s_vbl_exec_start_ns;
            if (off > s_census.pad_read_offset_max_ns)
                s_census.pad_read_offset_max_ns = off;
        }
        if (s_vbl_latch_ns) {
            const int64_t age = now - s_vbl_latch_ns;
            if (age > s_census.pad_read_age_max_ns)
                s_census.pad_read_age_max_ns = age;
        }
    }
    census_unlock();

    if (!pace_sink_on()) return;
    pace_rec_t r = {0};
    r.type  = OP_PACE_PAD_READ;
    r.aux   = (uint16_t)(n > 0xFFFFu ? 0xFFFFu : n);
    r.index = s_vbl_index;
    r.t_ns  = now;
    r.a     = s_vbl_exec_start_ns;
    r.b     = s_vbl_latch_ns;
    pace_emit(&r);
}

void openpete_pace_vbl(uint32_t vbl_index, uint8_t flags, uint16_t pad_reads,
                       int64_t deadline_ns, int64_t latch_ns,
                       int64_t exec_start_ns, int64_t exec_end_ns,
                       unsigned latched_seq, uint16_t latched_bits)
{
    /* Under the virtual clock (--record-cfr) op_now returns SCHEDULED time,
     * advanced only by the pacer's own sleep, so exec_start equals the
     * deadline exactly on every VBL. That is not a perfectly paced run; it is
     * a clock that cannot express lateness. Classify it unpaced so neither
     * the census nor the log ever reports a fabricated 100%-on-grid. */
    if (op_virtual_clock_active()) flags |= OP_PACE_F_UNLOCK;

    const int paced = !(flags & OP_PACE_F_UNLOCK) && deadline_ns != 0;

    /* A VBL whose grid was legitimately abandoned - paused, single-stepped,
     * rewound, running while the world is not live (CD load, level
     * transition), or the first tick after a clamp discarded wall time - is
     * not evidence about pacing. Its lateness is still reported (in the
     * transient max, and for a clamp in the slip ledger as well), just kept
     * out of the headline distribution: the boot stall otherwise sets the max
     * on every single run, and an ambient alarm that always reads +15 ms is
     * one nobody looks at. Nothing is hidden; it is binned. */
    const uint8_t transient = OP_PACE_F_PAUSED | OP_PACE_F_STEPPED |
                              OP_PACE_F_REWIND | OP_PACE_F_NOT_LIVE |
                              OP_PACE_F_SLIPPED;

    census_lock();
    s_census.vbls++;
    if (paced) {
        s_census.vbls_paced++;
        /* The start-phase error, the headline metric: how late the tick
         * actually started against the grid point it was paced to. Signed -
         * an early start is a different defect from a late one and must not
         * average away. */
        const int64_t err = exec_start_ns - deadline_ns;
        if (flags & transient) {
            s_census.vbls_transient++;
            if (err > s_census.phase_err_transient_max_ns)
                s_census.phase_err_transient_max_ns = err;
        } else {
            s_census.vbls_clean++;
            if (err > s_census.phase_err_max_ns) s_census.phase_err_max_ns = err;
            if (err < s_census.phase_err_min_ns) s_census.phase_err_min_ns = err;
            s_census.phase_hist[hist_bucket(err)]++;
            /* Only a VBL that actually CONSUMED a host sample contributes.
             * A claimed iteration leaves the previous publish instant
             * standing, and charging that age to a tick which never looked at
             * the channel grows the bound without bound on a bk2 run - a
             * number about nothing, in the shape of a measurement. */
            if (latch_ns && latched_seq >= 2u) {
                const int64_t off = exec_start_ns - latch_ns;
                s_census.latch_samples++;
                if (off > s_census.latch_offset_max_ns)
                    s_census.latch_offset_max_ns = off;
            } else if (latched_seq == OP_PACE_LATCH_CLAIMED) {
                s_census.vbls_claimed++;
            }
        }
    }
    census_unlock();

    /* Close the bracket here, not below the log-file check: the census must
     * classify reads identically whether or not a log is being written, or
     * the always-on numbers describe a different rule from the recorded ones. */
    s_vbl_bracket_open = 0;

    if (!pace_sink_on()) return;
    pace_rec_t r = {0};
    r.type  = OP_PACE_VBL;
    r.flags = flags;
    r.aux   = pad_reads;
    r.index = vbl_index;
    r.t_ns  = exec_start_ns;
    r.a = deadline_ns; r.b = latch_ns; r.c = exec_start_ns; r.d = exec_end_ns;
    r.e = (int64_t)latched_seq;
    r.f = (int64_t)latched_bits;
    pace_emit(&r);
    maybe_clocksample(exec_start_ns);
}

/* What the NEXT present will show, published by the re-extract that resolves
 * it. Kept here rather than passed through the present call because the alpha
 * is decided several layers below the pacer, and threading it up would put a
 * display quantity in every signature between. */
static int64_t s_next_alpha_micro;
static int64_t s_next_alpha_paired;
static int64_t s_slot_ns;         /* nominal present spacing, 0 = unknown */
static int64_t s_prev_present_ns; /* for the gap between consecutive presents */

void openpete_pace_set_slot_ns(int64_t slot_ns)
{
    s_slot_ns = slot_ns > 0 ? slot_ns : 0;
}

/* The extraction side's own copy, never touched by a present record. */
static int64_t s_extract_alpha_micro; static int s_extract_alpha_paired;
double openpete_pace_extract_alpha(int *paired)
{
    if (paired) *paired = s_extract_alpha_paired;
    return (double)s_extract_alpha_micro / 1e6;
}
void openpete_pace_note_alpha(double alpha, int paired)
{
    s_extract_alpha_micro  = (int64_t)(alpha * 1e6);
    s_extract_alpha_paired = paired ? 1 : 0;
    /* Micro-alpha: 1e-6 of a tick, signed. The canonical present is allowed a
     * slightly NEGATIVE alpha (it fires a little before its grid stamp), so
     * this must not be stored unsigned. */
    s_next_alpha_micro  = (int64_t)(alpha * 1e6);
    s_next_alpha_paired = paired ? 1 : 0;
}

double openpete_pace_last_alpha(void)
{
    return (double)s_next_alpha_micro / 1e6;
}
int openpete_pace_last_alpha_paired(void) { return s_next_alpha_paired; }

/** @brief Count one present in the census and write its PRESENT record; openpete_pace_present_alpha's body.
 *
 * @param kind OP_PACE_CANONICAL, OP_PACE_INTERMEDIATE or OP_PACE_REPEAT.
 * @param done_ns When the present path returned.
 * @param fence_block_ns Time blocked on the frame fence.
 * @param acquire_block_ns Time blocked acquiring a swapchain image.
 * @param queue_present_block_ns Time blocked in the queue present.
 * @param recreate_block_ns Time spent rebuilding the swapchain.
 * @param image_fence_block_ns Time blocked on the acquired image's previous submission.
 * @param alpha_micro Alpha the image showed, in 1e-6 of a tick.
 * @param paired 0 when the image had no prev/curr pair.
 * @return The present's sequence number, the PRESENT record's index. */
static uint32_t present_record(uint8_t kind, int64_t done_ns,
                               int64_t fence_block_ns, int64_t acquire_block_ns,
                               int64_t queue_present_block_ns,
                               int64_t recreate_block_ns,
                               int64_t image_fence_block_ns,
                               int64_t alpha_micro, int paired)
{
    census_lock();
    s_census.presents[kind <= OP_PACE_REPEAT ? kind : 0]++;
    if (s_prev_present_ns) {
        const int64_t gap = done_ns - s_prev_present_ns;
        if (kind == OP_PACE_INTERMEDIATE) {
            if (s_slot_ns && gap > 2 * s_slot_ns) s_census.gaps_mid_long++;
        } else if (kind == OP_PACE_REPEAT) {
            /* Deliberately updates no gap statistic of its own. A repeat's
             * contribution is that it moves s_prev_present_ns, which shortens
             * the hold the NEXT canonical present measures -- that shortening
             * is the display hole actually closing. Giving repeats their own
             * gap counter as well would report the same event twice. */
        } else {
            /* How long one image was held while the tick ran. An EMA, not a
             * percentile: this is a display for a human reading it live, and
             * the quantity is steady enough per scene that a centre estimate
             * is honest. */
            s_census.hold_ema_ns = s_census.hold_ema_ns
                ? (s_census.hold_ema_ns * 7 + gap) / 8 : gap;
        }
    }
    s_prev_present_ns = done_ns;
    const uint32_t seq = s_present_seq++;
    census_unlock();
    if (!pace_sink_on()) return seq;
    pace_rec_t r = {0};
    r.type  = OP_PACE_PRESENT;
    r.flags = kind;
    r.index = seq;
    r.t_ns  = done_ns;
    r.a = fence_block_ns; r.b = acquire_block_ns;
    r.c = queue_present_block_ns; r.d = recreate_block_ns;
    /* The animation time this frame showed, which is what makes a present
     * countable as MOTION rather than as an event: two presents at the same
     * alpha are the same image however far apart they were submitted. */
    r.e = alpha_micro;
    /* The image-fence wait is where a GPU-bound present under a non-blocking
     * present mode waits: the acquire returns at once and the image's
     * previous frame has not retired. aux marks the packed layout. */
    r.aux = OP_PACE_PRESENT_IMAGE_FENCE;
    r.f = (paired ? 1 : 0) |
          (int64_t)((uint64_t)(image_fence_block_ns > 0 ? image_fence_block_ns : 0) << 1);
    pace_emit(&r);
    return seq;
}

/** @brief Write the PRESENT_DETAIL record of the present just recorded as seq.
 *
 * Read on the thread that presented, right after the present: the RHI's
 * last-present statics are written only by the display owner. The
 * interpolation mode is the OP_PACE_PD_* bits; the tick stamp and the
 * present-clock instant are the interpolation module's.
 * @param kind The present's kind.
 * @param done_ns The PRESENT record's t_ns.
 * @param seq The PRESENT record's index.
 * @param paired The PRESENT record's paired flag. */
static void present_detail(uint8_t kind, int64_t done_ns, uint32_t seq, int paired)
{
    if (!pace_sink_on()) return;
    uint32_t image = 0, w = 0, h = 0;
    uint64_t presents = 0;
    rhi_last_present(&image, &w, &h, &presents);
    pace_rec_t r = {0};
    r.type  = OP_PACE_PRESENT_DETAIL;
    r.flags = (uint8_t)((openpete_interp_enabled() ? OP_PACE_PD_INTERP : 0u) |
                        (openpete_present_thread_active() ? OP_PACE_PD_EAGER : 0u) |
                        (openpete_interp_deterministic() ? OP_PACE_PD_DETERMINISTIC : 0u) |
                        (paired ? OP_PACE_PD_PAIRED : 0u));
    r.aux   = kind;
    r.index = seq;
    r.t_ns  = done_ns;
    r.a = image == UINT32_MAX ? -1 : (int64_t)image;
    r.b = (int64_t)w;
    r.c = (int64_t)h;
    r.d = (int64_t)openpete_interp_tick_stamp();
    r.e = (int64_t)openpete_interp_present_now();
    r.f = (int64_t)presents;
    pace_emit(&r);
}

/** @brief Write one SHED record per fence or acquire shed taken.
 *
 * A present attempt the swapchain refused is a shed, not a present. Three
 * refusal paths exist (fence, acquire budget, acquire back-off); every one
 * is reported, or the display rate reads as full while the user watches
 * half of it.
 * @param kind Scene kind the SHED records carry.
 * @param nf Fence sheds taken.
 * @param na Acquire sheds taken.
 * @param blk RHI block times, indexed by RHI_BLOCK_*. */
static void pace_write_sheds(uint8_t kind, uint64_t nf, uint64_t na, const int64_t *blk)
{
    for (uint64_t i = 0; i < nf; i++)
        openpete_pace_shed(kind, PACE_SHED_SLOT_BUSY, blk[RHI_BLOCK_FENCE], 0);
    for (uint64_t i = 0; i < na; i++)
        openpete_pace_shed(kind, PACE_SHED_NO_IMAGE, blk[RHI_BLOCK_ACQUIRE], 0);
}

/* Documented at the declaration in openpete_pace_log.h. */
int openpete_pace_record_sheds(uint8_t kind)
{
    uint64_t nf = 0, na = 0;
    openpete_native_render_take_sheds(&nf, &na);
    if (!nf && !na) return 0;
    int64_t blk[RHI_BLOCK_PHASE_COUNT] = {0};
    rhi_take_block_times(blk, RHI_BLOCK_PHASE_COUNT);
    pace_write_sheds(kind, nf, na, blk);
    return 1;
}

/* Refusals since the last call become SHED records; see openpete_pace_log.h. */
void openpete_pace_record_present(uint8_t kind, double alpha, int paired)
{
    int64_t blk[RHI_BLOCK_PHASE_COUNT] = {0};
    rhi_take_block_times(blk, RHI_BLOCK_PHASE_COUNT);
    const int64_t now_ns = op_now();
    uint64_t nf = 0, na = 0;
    openpete_native_render_take_sheds(&nf, &na);
    if (nf || na) { pace_write_sheds(kind, nf, na, blk); return; }
    const uint32_t seq = present_record(kind, now_ns, blk[RHI_BLOCK_FENCE], blk[RHI_BLOCK_ACQUIRE],
                                        blk[RHI_BLOCK_PRESENT], blk[RHI_BLOCK_RECREATE],
                                        blk[RHI_BLOCK_IMAGE_FENCE], (int64_t)(alpha * 1e6), paired);
    present_detail(kind, now_ns, seq, paired);
}

void openpete_pace_present(uint8_t kind, int64_t done_ns,
                           int64_t fence_block_ns, int64_t acquire_block_ns,
                           int64_t queue_present_block_ns,
                           int64_t recreate_block_ns,
                           int64_t image_fence_block_ns)
{
    openpete_pace_present_alpha(kind, done_ns, fence_block_ns, acquire_block_ns,
                                queue_present_block_ns, recreate_block_ns,
                                image_fence_block_ns, s_next_alpha_micro, s_next_alpha_paired);
}

/* The alpha is an argument so a present on another thread logs its own scene's alpha. */
void openpete_pace_present_alpha(uint8_t kind, int64_t done_ns,
                                 int64_t fence_block_ns, int64_t acquire_block_ns,
                                 int64_t queue_present_block_ns,
                                 int64_t recreate_block_ns,
                                 int64_t image_fence_block_ns,
                                 int64_t alpha_micro, int paired)
{
    (void)present_record(kind, done_ns, fence_block_ns, acquire_block_ns,
                         queue_present_block_ns, recreate_block_ns, image_fence_block_ns,
                         alpha_micro, paired);
}

void openpete_pace_shed(uint8_t kind, uint16_t reason,
                        int64_t predicted_cost_ns, int64_t deadline_margin_ns)
{
    census_lock();
    s_census.sheds++;
    census_unlock();
    if (!pace_sink_on()) return;
    pace_rec_t r = {0};
    r.type  = OP_PACE_SHED;
    r.flags = kind;
    r.aux   = reason;
    r.t_ns  = op_now();
    r.a = predicted_cost_ns; r.b = deadline_margin_ns;
    pace_emit(&r);
}

void openpete_pace_cd_miss(int64_t waited_ns, uint64_t end_offset)
{
    if (!pace_sink_on()) return;
    pace_rec_t r = {0};
    r.type = OP_PACE_CD_MISS;
    r.t_ns = op_now();
    r.a = waited_ns; r.b = (int64_t)end_offset;
    pace_emit(&r);
}

void openpete_pace_flip(uint64_t id, int64_t t_ns, int64_t waited_ns)
{
    if (!pace_sink_on()) return;
    pace_rec_t r = {0};
    r.type  = OP_PACE_FLIP;
    r.index = (uint32_t)id;
    r.t_ns  = t_ns;
    r.a = (int64_t)id;
    /* How long the wait actually blocked. A near-zero wait means the flip had
     * ALREADY happened and t_ns is when we asked, not when the display
     * changed - the analysis must drop those rather than average them in. */
    r.b = waited_ns;
    pace_emit(&r);
}

void openpete_pace_slip(uint8_t kind, int64_t periods, int64_t behind_ns)
{
    const unsigned k = (kind == OP_PACE_SLIP_RENDER) ? 1u : 0u;
    census_lock();
    s_census.slip_events[k]++;
    s_census.slip_periods[k] += (uint64_t)(periods > 0 ? periods : 0);
    census_unlock();
    if (!pace_sink_on()) return;
    pace_rec_t r = {0};
    r.type  = OP_PACE_SLIP;
    r.flags = kind;
    r.t_ns  = op_now();
    r.a = periods; r.b = behind_ns;
    pace_emit(&r);
}

void openpete_pace_input_edge(int64_t sample_ns, unsigned seq, uint16_t bits,
                              uint16_t bits_delta, int analog_delta)
{
    census_lock();
    s_census.input_edges++;
    census_unlock();
    if (!pace_sink_on()) return;
    pace_rec_t r = {0};
    r.type  = OP_PACE_INPUT_EDGE;
    r.flags = analog_delta ? 1u : 0u;
    r.aux   = bits_delta;
    r.t_ns  = sample_ns;
    r.a     = bits;
    r.e     = (int64_t)seq;
    pace_emit(&r);
}

void openpete_pace_wake(int64_t request_ns, int64_t target_ns, int64_t wake_ns)
{
    /* The OS only owes us time it could actually have slept: measure its
     * share from the later of the target and the moment we asked. Measuring
     * from the target alone charges the scheduler for a deadline that was
     * already gone before the sleep began. */
    const int64_t from   = (request_ns > target_ns) ? request_ns : target_ns;
    const int64_t over   = wake_ns - from;
    const int64_t behind = (request_ns > target_ns) ? request_ns - target_ns : 0;

    census_lock();
    if (over > s_census.wake_overshoot_max_ns)
        s_census.wake_overshoot_max_ns = over;
    if (behind > s_census.wake_behind_max_ns)
        s_census.wake_behind_max_ns = behind;
    s_census.wakes++;
    census_unlock();
    if (!pace_sink_on()) return;
    pace_rec_t r = {0};
    r.type  = OP_PACE_WAKE;
    r.t_ns  = wake_ns;
    r.a     = target_ns;
    r.b     = over;
    r.c     = behind;
    pace_emit(&r);
}

void openpete_pace_present_span(uint8_t kind, int64_t start_ns,
                                int64_t end_ns, int64_t extract_ns)
{
    if (!pace_sink_on()) return;
    openpete_pace_split_add(OP_PACE_SPLIT_EXTRACT, extract_ns);
    pace_rec_t r = {0};
    r.type  = OP_PACE_PRESENT_SPAN;
    r.flags = kind;
    r.t_ns  = end_ns;
    r.a     = start_ns;
    r.b     = end_ns - start_ns;
    r.c     = extract_ns;
    pace_emit(&r);
}

/* An iteration is recorded individually once its active time crosses this.
 * The pump's ordinary work is tens of microseconds; a millisecond means it
 * stopped publishing input for at least one whole iteration period, which is
 * the smallest stall worth a record of its own. */
#define PACE_HOST_PUMP_FLOOR_NS   ((int64_t)1000000)
/* The same, for the PARK - and it needs its own floor because a stall can
 * land on either side of the wait's return and only one of them is a cost.
 * A Win32 modal size/move loop runs the OS's loop INSIDE the wait, so the
 * iteration parks for the whole drag and does no work at all: judged on
 * active time it is the quietest iteration of the run, while input
 * publication has in fact stopped for seconds. The wait parks about 1 ms, so
 * anything past four of them is the OS holding the thread rather than the
 * loop idling. */
#define PACE_HOST_PUMP_PARK_FLOOR_NS ((int64_t)4000000)
/* How often the summary lands. Ten a second is free next to ~60 VBL and a few
 * hundred presents, and short enough that a stall's neighbourhood is still
 * resolved by the surrounding summaries. */
#define PACE_HOST_PUMP_WINDOW_NS  ((int64_t)100000000)

void openpete_pace_host_pump(int64_t start_ns, int64_t wait_end_ns,
                             int64_t pump_end_ns, int64_t input_end_ns,
                             int64_t end_ns, int window_op)
{
    /* Main-thread only, so the accumulators need no lock of their own; the
     * census counters they feed are shared and take theirs below. */
    static int64_t  s_win_start_ns;
    static uint64_t s_win_iters;
    static uint64_t s_win_long;
    static int64_t  s_win_active_ns;
    static int64_t  s_win_active_max_ns;
    static int64_t  s_win_pump_max_ns;
    static int64_t  s_win_park_ns;
    static uint32_t s_iter;

    const int64_t park   = wait_end_ns - start_ns;
    const int64_t pump   = pump_end_ns - wait_end_ns;
    const int64_t active = end_ns - wait_end_ns;

    census_lock();
    s_census.host_pump_iters++;
    s_census.host_pump_active_ns += active;
    s_census.host_pump_park_ns += park;
    if (active > s_census.host_pump_max_ns) s_census.host_pump_max_ns = active;
    if (pump > s_census.host_pump_events_max_ns)
        s_census.host_pump_events_max_ns = pump;
    if (park > s_census.host_pump_park_max_ns)
        s_census.host_pump_park_max_ns = park;
    if (active >= PACE_HOST_PUMP_FLOOR_NS ||
        park >= PACE_HOST_PUMP_PARK_FLOOR_NS) s_census.host_pump_long++;
    census_unlock();

    s_iter++;
    if (!s_win_start_ns) s_win_start_ns = start_ns;
    s_win_iters++;
    s_win_active_ns += active;
    s_win_park_ns += park;
    if (active > s_win_active_max_ns) s_win_active_max_ns = active;
    if (pump > s_win_pump_max_ns) s_win_pump_max_ns = pump;

    if (!pace_sink_on()) {
        /* No sink: the window accumulators would grow without ever being
         * emitted, and their max would then describe the whole run rather than
         * a window. The census above already carries the always-on half. */
        if (end_ns - s_win_start_ns >= PACE_HOST_PUMP_WINDOW_NS) {
            s_win_start_ns = 0;
            s_win_iters = s_win_long = 0;
            s_win_active_ns = s_win_active_max_ns = s_win_pump_max_ns = 0;
            s_win_park_ns = 0;
        }
        return;
    }

    if (active >= PACE_HOST_PUMP_FLOOR_NS ||
        park >= PACE_HOST_PUMP_PARK_FLOOR_NS) {
        s_win_long++;
        pace_rec_t r = {0};
        r.type  = OP_PACE_HOST_PUMP;
        r.flags = (uint8_t)((window_op ? OP_PACE_HP_WINDOW : 0u)
                            | (park >= PACE_HOST_PUMP_PARK_FLOOR_NS
                               ? OP_PACE_HP_PARKED : 0u));
        r.index = s_iter;
        r.t_ns  = end_ns;
        r.a     = start_ns;
        r.b     = wait_end_ns - start_ns;      /* parked, not a cost */
        r.c     = pump;
        r.d     = input_end_ns - pump_end_ns;
        r.e     = end_ns - input_end_ns;
        pace_emit(&r);
    }

    if (end_ns - s_win_start_ns >= PACE_HOST_PUMP_WINDOW_NS) {
        pace_rec_t s = {0};
        s.type  = OP_PACE_HOST_PUMP;
        s.flags = OP_PACE_HP_SUMMARY;
        s.index = (uint32_t)s_win_iters;
        s.t_ns  = end_ns;
        s.a     = s_win_start_ns;
        s.b     = s_win_active_ns;
        s.c     = s_win_active_max_ns;
        s.d     = s_win_pump_max_ns;
        s.e     = (int64_t)s_win_long;
        s.f     = s_win_park_ns;
        pace_emit(&s);
        s_win_start_ns = 0;
        s_win_iters = s_win_long = 0;
        s_win_active_ns = s_win_active_max_ns = s_win_pump_max_ns = 0;
        s_win_park_ns = 0;
    }
}

void openpete_pace_window(uint16_t id, int begin)
{
    if (!pace_sink_on()) return;
    pace_rec_t r = {0};
    r.type  = OP_PACE_WINDOW;
    r.flags = begin ? 1u : 0u;
    r.aux   = id;
    r.t_ns  = op_now();
    pace_emit(&r);
}

/* ------------------------------------------------------------------ */
/* census                                                             */
/* ------------------------------------------------------------------ */

void openpete_pace_census_get(openpete_pace_census_t* out)
{
    if (!out) return;
    census_lock();
    *out = s_census;
    census_unlock();
}

void openpete_pace_census_format(char* buf, unsigned buf_bytes)
{
    openpete_pace_census_t snap;
    openpete_pace_census_get(&snap);
    const openpete_pace_census_t* c = &snap;

    if (c->vbls_paced == 0 || c->vbls_clean == 0) {
        /* No VBL ran against a wall deadline (headless fast-forward, Tab,
         * --record-cfr's virtual clock), or every paced VBL was a transient,
         * so no phase error is defined; the line reports VBL, present and
         * slip counts instead. */
        SDL_snprintf(buf, buf_bytes,
            "unpaced (%" SDL_PRIu64 " VBL, %" SDL_PRIu64 " present) - no deadline was in force; "
            "slips sim %" SDL_PRIu64 "/%" SDL_PRIu64 " render %" SDL_PRIu64 "/%" SDL_PRIu64,
            (Uint64)c->vbls,
            (Uint64)(c->presents[0] + c->presents[1]
                                 + c->presents[2]),
            (Uint64)c->slip_events[0],
            (Uint64)c->slip_periods[0],
            (Uint64)c->slip_events[1],
            (Uint64)c->slip_periods[1]);
        return;
    }

    /* Fraction of clean paced VBLs that started within the 250 µs
     * start-phase goal - the one number worth reading at a glance. Buckets
     * 0..2 are (early, <100 µs, <250 µs); bucket 0 is early starts, which
     * the loop cannot produce today (sim_due gates on now >= deadline) but
     * which a future pacer change could, and an early start is as much a
     * phase error as a late one. */
    const uint64_t early   = c->phase_hist[0];
    const uint64_t on_grid = c->phase_hist[1] + c->phase_hist[2];
    const double pct = c->vbls_clean
        ? 100.0 * (double)on_grid / (double)c->vbls_clean : 0.0;

    /* An unmeasured quantity prints as n/a. A latch bound of "0.000 ms" on a
     * run where no sample ever contributed reads as a perfect result, which
     * is worse than an absent one. */
    char latch[96], early_s[48];
    if (c->latch_samples)
        SDL_snprintf(latch, sizeof(latch), "%.3f ms over %" SDL_PRIu64 " VBL",
                     (double)c->latch_offset_max_ns / 1e6,
                     (Uint64)c->latch_samples);
    else
        SDL_strlcpy(latch, "n/a", sizeof(latch));
    /* A run where another writer owned the pad throughout (bk2 replay) has no
     * host input phase to report. Saying so is the difference between an
     * absent measurement and a flattering one. */
    if (c->vbls_claimed) {
        char tail[48];
        SDL_snprintf(tail, sizeof(tail), " (+%" SDL_PRIu64 " claimed)",
                     (Uint64)c->vbls_claimed);
        SDL_strlcat(latch, tail, sizeof(latch));
    }
    if (early)
        SDL_snprintf(early_s, sizeof(early_s), ", %" SDL_PRIu64 " early (max %+.3f ms)",
                     (Uint64)early,
                     (double)c->phase_err_min_ns / 1e6);
    else
        early_s[0] = '\0';

    /* Three distinguishable states, because two of them are absences: no
     * probe wired (unmeasured), a probe that never fired (a real and alarming
     * finding - the game stopped reading its pad), and a measurement. */
    /* Reads by fiber runs that were not the VBL they landed in - speculation
     * under runahead, rewind replay. Real reads, excluded from the phase
     * distribution, and named rather than dropped so "exactly one per VBL"
     * cannot quietly mean "one per VBL plus some others we ignored". */
    char unattr[48];
    if (c->pad_reads_unattributed)
        SDL_snprintf(unattr, sizeof(unattr), " (+%" SDL_PRIu64 " speculative/replayed)",
                     (Uint64)c->pad_reads_unattributed);
    else
        unattr[0] = '\0';

    char padr[176];
    if (!s_pad_probe_armed)
        SDL_strlcpy(padr, "pad read: no probe", sizeof(padr));
    else if (c->pad_read_vbls == 0)
        SDL_strlcpy(padr, "pad read: NEVER (probe armed)", sizeof(padr));
    else
        SDL_snprintf(padr, sizeof(padr),
                     "pad read: %" SDL_PRIu64 " in %" SDL_PRIu64 " VBL(s)%s, <=%+.3f ms into the "
                     "VBL, sample age <=%+.3f ms",
                     (Uint64)c->pad_reads,
                     (Uint64)c->pad_read_vbls,
                     unattr,
                     (double)c->pad_read_offset_max_ns / 1e6,
                     (double)c->pad_read_age_max_ns / 1e6);

    /* The seqlock never failing is the expected state, so it prints nothing;
     * a single failure means some latch consumed no sample at all and must
     * not hide inside a healthy-looking line. */
    /* The pumping thread. Absent under the single-threaded path, where the
     * pump shares the sim's thread and its cost is already inside the VBL
     * bracket - printing a zero there would claim a measurement that was
     * never taken. */
    char pump[160];
    if (c->host_pump_iters)
        SDL_snprintf(pump, sizeof(pump),
                     "; pump %" SDL_PRIu64 " iter(s), %.3f ms mean active + %.3f ms "
                     "parked, max %.3f ms active (events alone %.3f ms) / "
                     "%.3f ms parked, %" SDL_PRIu64 " long",
                     (Uint64)c->host_pump_iters,
                     (double)c->host_pump_active_ns
                         / (double)c->host_pump_iters / 1e6,
                     (double)c->host_pump_park_ns
                         / (double)c->host_pump_iters / 1e6,
                     (double)c->host_pump_max_ns / 1e6,
                     (double)c->host_pump_events_max_ns / 1e6,
                     (double)c->host_pump_park_max_ns / 1e6,
                     (Uint64)c->host_pump_long);
    else
        pump[0] = '\0';

    char seqfail[64];
    const unsigned sf = openpete_host_input_seqlock_failures();
    if (sf) SDL_snprintf(seqfail, sizeof(seqfail),
                         "; INPUT SEQLOCK FAILED %u time(s)", sf);
    else    seqfail[0] = '\0';

    SDL_snprintf(buf, buf_bytes,
        "%" SDL_PRIu64 " VBL (%" SDL_PRIu64 " paced, %" SDL_PRIu64 " clean); %" SDL_PRIu64 "+%" SDL_PRIu64 "+%" SDL_PRIu64 " present; "
        "phase %.2f%% within 250us, max %+.3f ms%s; "
        "transient %" SDL_PRIu64 " VBL max %+.3f ms; wake<=%+.3f ms behind<=%+.3f ms; "
        "latch<=%s; %s; %" SDL_PRIu64 " input edge(s); "
        "slips sim %" SDL_PRIu64 "/%" SDL_PRIu64 " render %" SDL_PRIu64 "/%" SDL_PRIu64 "; sheds %" SDL_PRIu64 "%s%s",
        (Uint64)c->vbls,
        (Uint64)c->vbls_paced,
        (Uint64)c->vbls_clean,
        (Uint64)c->presents[0],
        (Uint64)c->presents[1],
        (Uint64)c->presents[2],
        pct,
        (double)c->phase_err_max_ns / 1e6,
        early_s,
        (Uint64)c->vbls_transient,
        (double)c->phase_err_transient_max_ns / 1e6,
        (double)c->wake_overshoot_max_ns / 1e6,
        (double)c->wake_behind_max_ns / 1e6,
        latch, padr,
        (Uint64)c->input_edges,
        (Uint64)c->slip_events[0],
        (Uint64)c->slip_periods[0],
        (Uint64)c->slip_events[1],
        (Uint64)c->slip_periods[1],
        (Uint64)c->sheds,
        pump, seqfail);
}

/* ------------------------------------------------------------------ */
/* instrument self-verification                                       */
/* ------------------------------------------------------------------ */

/** @brief Parse up to `n` comma-separated integers from environment variable `name` into `v`, leaving the slots it does not reach as they are.
 * @param name The variable.
 * @param v The values, holding their defaults on entry.
 * @param n The slots in `v`.
 * @return The values parsed, 0 when the variable is unset or empty. */
static int pace_env_ints(const char* name, int64_t* v, int n)
{
    const char* e = getenv(name);
    if (!e || !*e) return 0;
    char* end = NULL;
    int got = 0;
    const char* at = e;
    while (got < n) {
        v[got] = strtoll(at, &end, 10);
        got++;
        if (*end != ',') break;
        at = end + 1;
    }
    return got;
}

/** @brief Stall the calling thread for `us` microseconds: a busy-wait on the real clock, an advance of the virtual clock under it.
 *
 * Busy-wait, not a sleep: a sleep would be absorbed by the pacer's own
 * slack accounting on some platforms and measure the sleep primitive
 * rather than the log's ability to see a late start. The virtual clock
 * advances only from op_sleep_until, so a spin on op_now there never
 * terminates: the stall is the clock's advance, and a measured span is
 * then exactly the injected cost.
 * @param us The stall, > 0.
 * @return `us`. */
static int64_t pace_stall(int64_t us)
{
    const op_time_t until = op_now() + us * OP_NS_PER_US;
    if (op_virtual_clock_active()) {
        op_sleep_until(until);
        return us;
    }
    while (op_now() < until) { }
    return us;
}

int64_t openpete_pace_inject_present(void)
{
    static int     s_init;
    static int64_t s_us[3] = {0, 0, 1};   /* first, second, block */
    static int64_t s_vbl[2] = {0, 0};     /* openpete_frame window [from, to); to 0: without end */
    static long    s_calls;
    if (!s_init) {
        s_init = 1;
        if (pace_env_ints("OPENPETE_PACE_INJECT_PRESENT_US", s_us, 3) == 1) s_us[1] = s_us[0];
        if (s_us[2] < 1) s_us[2] = 1;
        pace_env_ints("OPENPETE_PACE_INJECT_PRESENT_VBL", s_vbl, 2);
        if (s_us[0] > 0 || s_us[1] > 0)
            OPL_WARN_F("pace",
                       "INJECTING %lld,%lld us (switching every %lld present(s)) into "
                       "intermediate presents from VBL %lld to %lld (0: without end)%s - "
                       "shed-predicate fault injection, never a play path",
                       (long long)s_us[0], (long long)s_us[1], (long long)s_us[2],
                       (long long)s_vbl[0], (long long)s_vbl[1],
                       op_virtual_clock_active() ? " on the virtual clock" : "");
    }
    if ((int64_t)openpete_frame < s_vbl[0] || (s_vbl[1] > 0 && (int64_t)openpete_frame >= s_vbl[1])) return 0;
    const int64_t us = s_us[(s_calls++ / s_us[2]) & 1];
    return us > 0 ? pace_stall(us) : 0;
}

int64_t openpete_pace_inject_canonical(void)
{
    static int     s_init;
    static int64_t s_us;
    if (!s_init) {
        s_init = 1;
        pace_env_ints("OPENPETE_PACE_INJECT_CANON_US", &s_us, 1);
        if (s_us > 0)
            OPL_WARN_F("pace",
                       "INJECTING %lld us into every CANONICAL present - "
                       "a present that cannot be shed; fault injection, "
                       "never a play path",
                       (long long)s_us);
    }
    if (s_us <= 0 || op_virtual_clock_active()) return 0;
    return pace_stall(s_us);
}

int64_t openpete_pace_inject(int paced) OP_REQUIRES(sim)
{
    static int      s_init;
    /* us, every, from, count */
    static int64_t  s_spec[4] OP_GUARDED_BY(sim) = {0, 30, 0, 0};
    static uint64_t s_seen;
    static int64_t  s_done OP_GUARDED_BY(sim);

    if (!s_init) {
        s_init = 1;
        pace_env_ints("OPENPETE_PACE_INJECT_US", s_spec, 4);
        if (s_spec[1] < 1) s_spec[1] = 30;
        if (s_spec[2] < 0) s_spec[2] = 0;
        if (s_spec[0] > 0)
            OPL_WARN_F("pace",
                       "INJECTING a %lld us stall every %lld paced VBL from paced VBL %lld, "
                       "%lld time(s) (0: without end)%s - instrument calibration only, "
                       "never a play path",
                       (long long)s_spec[0], (long long)s_spec[1], (long long)s_spec[2],
                       (long long)s_spec[3],
                       op_virtual_clock_active() ? ", as virtual-clock advances" : "");
    }
    if (s_spec[0] <= 0 || !paced) return 0;
    const int64_t k = (int64_t)(s_seen++) - s_spec[2];
    if (k < 0 || k % s_spec[1] != 0) return 0;
    if (s_spec[3] > 0 && s_done >= s_spec[3]) return 0;
    s_done++;
    return pace_stall(s_spec[0]);
}

/* ------------------------------------------------------------------ */
/* per-tick CPU split, planner and GPU records                        */
/* ------------------------------------------------------------------ */

/* Accumulated since the last TICK_SPLIT record; added from the sim and
 * present threads, read and zeroed by the sim thread. */
static _Atomic uint64_t s_split_ns[OP_PACE_SPLIT_COUNT];
static _Atomic uint32_t s_split_n[OP_PACE_SPLIT_COUNT];

int openpete_pace_split_on(void) { return pace_sink_on(); }

int64_t op_pace_split_now(void) { return (int64_t)op_now(); }

/* The calling thread's innermost open split scope. A guard lives on its
 * scope's stack frame and the scopes close in LIFO order, so the chain
 * through parent is the thread's open scopes. */
static _Thread_local op_pace_split_guard* s_split_top;

void op_pace_split__enter(op_pace_split_guard* g, int which)
{
    g->which = which;
    g->child_ns = 0;
    g->parent = NULL;
    g->t0 = pace_sink_on() ? (int64_t)op_now() : 0;
    if (!g->t0) return;
    g->parent = s_split_top;
    s_split_top = g;
}

void op_pace_split__cleanup(op_pace_split_guard* g)
{
    if (!g->t0) return;
    const int64_t dur = (int64_t)op_now() - g->t0;
    openpete_pace_split_add(g->which, dur - g->child_ns);
    if (g->parent) g->parent->child_ns += dur;
    s_split_top = g->parent;
}

void openpete_pace_split_add(int which, int64_t ns)
{
    if ((unsigned)which >= OP_PACE_SPLIT_COUNT || !pace_sink_on()) return;
    atomic_fetch_add_explicit(&s_split_ns[which], ns > 0 ? (uint64_t)ns : 0u, memory_order_relaxed);
    atomic_fetch_add_explicit(&s_split_n[which], 1u, memory_order_relaxed);
}

/** @brief Take an accumulator's microseconds and zero it, saturated to u32.
 * @param which OP_PACE_SPLIT_* slot.
 * @return The microseconds. */
static uint64_t split_take_us(int which)
{
    const uint64_t us = atomic_exchange_explicit(&s_split_ns[which], 0u, memory_order_relaxed) / 1000u;
    return us > UINT32_MAX ? UINT32_MAX : us;
}

/** @brief Take an accumulator's call count and zero it, saturated to u16.
 * @param which OP_PACE_SPLIT_* slot.
 * @return The count. */
static uint64_t split_take_n(int which)
{
    const uint32_t n = atomic_exchange_explicit(&s_split_n[which], 0u, memory_order_relaxed);
    return n > 0xFFFFu ? 0xFFFFu : n;
}

void openpete_pace_tick_split(uint32_t vbl_index, int64_t tick_ns)
{
    if (!pace_sink_on()) return;
    uint64_t us[OP_PACE_SPLIT_COUNT];
    for (int k = 0; k < OP_PACE_SPLIT_COUNT; k++) us[k] = split_take_us(k);
    const uint64_t n_ex = split_take_n(OP_PACE_SPLIT_EXTRACT), n_rd = split_take_n(OP_PACE_SPLIT_RENDER);
    const uint64_t n_pr = split_take_n(OP_PACE_SPLIT_PRESENT);
    const uint64_t n_sn = split_take_n(OP_PACE_SPLIT_SNAP_REX) + split_take_n(OP_PACE_SPLIT_SNAP_CANON);
    for (int k = 0; k < OP_PACE_SPLIT_COUNT; k++) (void)atomic_exchange_explicit(&s_split_n[k], 0u, memory_order_relaxed);
    pace_rec_t r = {0};
    r.type  = OP_PACE_TICK_SPLIT;
    r.flags = OP_PACE_TS_EXCLUSIVE;
    r.index = vbl_index;
    r.t_ns  = (int64_t)op_now();
    r.a = (int64_t)(us[OP_PACE_SPLIT_SIM] | us[OP_PACE_SPLIT_EXTRACT] << 32);
    r.b = (int64_t)(us[OP_PACE_SPLIT_TERRAIN] | us[OP_PACE_SPLIT_MOBY] << 32);
    r.c = (int64_t)(us[OP_PACE_SPLIT_SNAP_REX] | us[OP_PACE_SPLIT_SNAP_CANON] << 32);
    r.d = (int64_t)(us[OP_PACE_SPLIT_RENDER] | us[OP_PACE_SPLIT_PRESENT] << 32);
    r.e = (int64_t)(n_ex | n_rd << 16 | n_pr << 32 | (n_sn > 0xFFFFu ? 0xFFFFu : n_sn) << 48);
    const int64_t t_us = tick_ns > 0 ? tick_ns / 1000 : 0;
    r.f = (int64_t)((uint64_t)(t_us > (int64_t)UINT32_MAX ? (int64_t)UINT32_MAX : t_us) |
                    us[OP_PACE_SPLIT_HOST] << 32);
    pace_emit(&r);
}

void openpete_pace_eager_plan(uint32_t vbl_index, int64_t t_ns, uint8_t flags,
                              int level, int std_level, int slots, int afford,
                              int planned, int made, int64_t period_ns, int64_t budget_ns,
                              int64_t tail_ns, int64_t lag_ns, int64_t vbl_ns,
                              int64_t late_ns, int64_t anchor_age_ns, int head_vbls,
                              int spare, int capacity, int final_planned,
                              int cap, int capped, unsigned superseded)
{
    if (!pace_sink_on()) return;
#define EP_U16(v) ((uint64_t)((v) < 0 ? 0 : (v) > 0xFFFF ? 0xFFFF : (v)))
#define EP_U8(v) ((uint64_t)((v) < 0 ? 0 : (v) > 0xFF ? 0xFF : (v)))
    pace_rec_t r = {0};
    r.type  = OP_PACE_EAGER_PLAN;
    r.flags = flags;
    r.aux   = (uint16_t)(EP_U16(level) > 0xFF ? 0xFF : EP_U16(level)) |
              (uint16_t)((EP_U16(std_level) > 0xFF ? 0xFF : EP_U16(std_level)) << 8);
    r.index = vbl_index;
    r.t_ns  = t_ns;
    const int64_t age10 = anchor_age_ns <= 0 ? 0 : anchor_age_ns / 10000 > 0x1FFF ? 0x1FFF : anchor_age_ns / 10000;
    const uint64_t hv = head_vbls < 0 ? 7u : head_vbls > 7 ? 7u : (uint64_t)head_vbls;
    r.a = (int64_t)((uint64_t)(budget_ns < 0 ? 0 : budget_ns > (int64_t)UINT32_MAX ? (int64_t)UINT32_MAX : budget_ns) |
                    EP_U16(made) << 32 | (uint64_t)age10 << 48 | hv << 61);
    r.b = (int64_t)((uint64_t)(tail_ns < 0 ? 0 : tail_ns > (int64_t)UINT32_MAX ? (int64_t)UINT32_MAX : tail_ns) |
                    EP_U8(spare) << 32 | EP_U8(capacity) << 40 | EP_U8(final_planned) << 48 | OP_PACE_EP_B_SPAN);
    r.c = lag_ns; r.e = late_ns;
    const uint64_t cap15 = cap < 0 ? 0u : cap > 0x7FFF ? 0x7FFFu : (uint64_t)cap;
    const uint64_t sup16 = superseded > 0xFFFFu ? 0xFFFFu : (uint64_t)superseded;
    r.d = (int64_t)((uint64_t)(vbl_ns < 0 ? 0 : vbl_ns > (int64_t)UINT32_MAX ? (int64_t)UINT32_MAX : vbl_ns) |
                    cap15 << 32 | (uint64_t)(capped ? 1u : 0u) << 47 | sup16 << 48);
    r.f = (int64_t)(EP_U16(slots) | EP_U16(afford) << 16 | EP_U16(planned) << 32 |
                    EP_U16(period_ns / 1000) << 48);
#undef EP_U16
#undef EP_U8
    pace_emit(&r);
}

void openpete_pace_gpu_frame(uint64_t token, int64_t frame_ns, int64_t busy_ns, int zones)
{
    if (!pace_sink_on()) return;
    pace_rec_t r = {0};
    r.type  = OP_PACE_GPU_FRAME;
    r.aux   = (uint16_t)(zones < 0 ? 0 : zones > 0xFFFF ? 0xFFFF : zones);
    r.index = (uint32_t)token;
    r.t_ns  = (int64_t)op_now();
    r.a = frame_ns; r.b = busy_ns; r.c = (int64_t)token;
    pace_emit(&r);
}

int openpete_pace_gpu_records_wanted(void)
{
    static int on = -1;
    if (on < 0) { const char* e = openpete_config_env("OPENPETE_BUGREPORT_GPU"); on = (e && *e && *e != '0'); }
    return on;
}
