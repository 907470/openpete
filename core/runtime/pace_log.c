/* pace_log.c — unified pacing event log (M1) + always-on pacing census (M8).
 * See openpete_pace_log.h for the format and the reasoning behind it.
 */

#include "openpete_pace_log.h"
#include "openpete_time.h"
#include "openpete_host_input.h"
#include "openpete_log.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef OPENPETE_BUILD_ID
#define OPENPETE_BUILD_ID "unknown"
#endif

/* ------------------------------------------------------------------ */
/* writer                                                             */
/* ------------------------------------------------------------------ */

/* Records arrive from two threads under the host split — the sim worker emits
 * VBL/PRESENT/SLIP, main emits INPUT_EDGE and the resize windows — so the
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

static FILE*       s_fp;
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
    100000,     /* 100 µs — the plan's target band                        */
    250000,     /* 250 µs — the stated p99 goal (S1)                      */
    500000,
    1000000,
    2000000,
    5000000,
    16715111,   /* one VBL: past here the tick is visibly late            */
    0           /* unbounded                                              */
};

int openpete_pace_log_active(void) { return s_fp != NULL; }

static void pace_flush_locked(void)
{
    if (s_buf_n && s_fp) {
        const size_t n = fwrite(s_buf, sizeof(pace_rec_t), s_buf_n, s_fp);
        s_written += n;
        if (n != s_buf_n) {
            /* Disk full or a closed pipe. Say so once and stop: silently
             * dropping records would leave a short log that still parses,
             * and a truncated pacing log reads as a clean short run. */
            OPL_ERROR_F("pace", "write failed after %llu record(s) — closing",
                        (unsigned long long)s_written);
            fclose(s_fp);
            s_fp = NULL;
        }
    }
    s_buf_n = 0;
}

static void pace_emit(const pace_rec_t* r)
{
    if (!s_fp) return;
    SDL_LockMutex(s_lock);
    if (s_fp) {                 /* re-check: close() may have won the race */
        s_buf[s_buf_n++] = *r;
        if (s_buf_n == PACE_BUF_RECORDS) pace_flush_locked();
    }
    SDL_UnlockMutex(s_lock);
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
    memset(hdr, 0, sizeof(hdr));
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
    PUT64(op_now());
    PUT64(OP_VBL_HZ_NUM);
    PUT64(OP_VBL_HZ_DEN);
    PUT64(OP_TICK_VBLS);
#undef PUT32
#undef PUT64

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
/* Read the first line of a sysfs/proc file into `out`. Returns 0 on success.
 * Absent files are the normal case off Linux and are not an error. */
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
    cpu_model(cpu, sizeof(cpu));
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
        "present_mode=%s\n"
        "fullscreen=%d\n"
        "render_fps=%d\n"
        "interp=%d\n"
        "runahead=%d\n"
        "tick_scale=%.6f\n"
        "pacer_slack_ns=%lld\n"
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
        "input_pulse_hold_us=%d\n",
        OPENPETE_BUILD_ID,
        SDL_GetPlatform(),
        cpu,
        SDL_GetNumLogicalCPUCores(),
        gov,
        gpu ? gpu : "unknown",
        driver ? driver : "unknown",
        refresh_mhz,
        present_mode ? present_mode : "unknown",
        fullscreen, render_fps, interp_on, runahead_n, tick_scale,
        (long long)op_pacer_slack_ns(),
        op_virtual_clock_active(),
        thread_split, input_legacy, headless,
        openpete_host_input_pulse_count(),
        openpete_host_input_pulse_bit(),
        openpete_host_input_pulse_hold_us());
}

/* ------------------------------------------------------------------ */
/* emit                                                               */
/* ------------------------------------------------------------------ */

static void maybe_clocksample(int64_t caller_ns)
{
    if (!s_fp || caller_ns < s_next_clocksample_ns) return;
    s_next_clocksample_ns = caller_ns + PACE_CLOCKSAMPLE_PERIOD_NS;

    /* The pair the analyzer needs for C1: the monotonic source the pacer
     * steps against, and a precise wall clock, read back to back HERE.
     *
     * Both must be sampled at the same instant. The caller's timestamp is a
     * tick body older than this call — fiber, presents and captures happened
     * in between — so pairing it with a fresh wall read would put a variable,
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
        s_census.pad_reads_unattributed++;
        if (!s_fp) return;
        pace_rec_t ur = {0};
        ur.type  = OP_PACE_PAD_READ;
        ur.flags = OP_PACE_PR_UNATTRIBUTED;
        ur.index = s_vbl_index;
        ur.t_ns  = now;
        pace_emit(&ur);
        return;
    }

    const unsigned n = ++s_vbl_pad_reads;

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

    if (!s_fp) return;
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

    /* A VBL whose grid was legitimately abandoned — paused, single-stepped,
     * rewound, running while the world is not live (CD load, level
     * transition), or the first tick after a clamp discarded wall time — is
     * not evidence about pacing. Its lateness is still reported (in the
     * transient max, and for a clamp in the slip ledger as well), just kept
     * out of the headline distribution: the boot stall otherwise sets the max
     * on every single run, and an ambient alarm that always reads +15 ms is
     * one nobody looks at. Nothing is hidden; it is binned. */
    const uint8_t transient = OP_PACE_F_PAUSED | OP_PACE_F_STEPPED |
                              OP_PACE_F_REWIND | OP_PACE_F_NOT_LIVE |
                              OP_PACE_F_SLIPPED;

    s_census.vbls++;
    if (paced) {
        s_census.vbls_paced++;
        /* S1, the plan's headline metric: how late the tick actually started
         * against the grid point it was paced to. Signed — an early start is
         * a different defect from a late one and must not average away. */
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
             * the channel grows the bound without bound on a bk2 run — a
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

    /* Close the bracket here, not below the log-file check: the census must
     * classify reads identically whether or not a log is being written, or
     * the always-on numbers describe a different rule from the recorded ones. */
    s_vbl_bracket_open = 0;

    if (!s_fp) return;
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

void openpete_pace_present(uint8_t kind, int64_t done_ns,
                           int64_t fence_block_ns, int64_t acquire_block_ns,
                           int64_t queue_present_block_ns,
                           int64_t recreate_block_ns)
{
    s_census.presents[kind == OP_PACE_INTERMEDIATE ? 1 : 0]++;
    if (!s_fp) return;
    pace_rec_t r = {0};
    r.type  = OP_PACE_PRESENT;
    r.flags = kind;
    r.index = s_present_seq++;
    r.t_ns  = done_ns;
    r.a = fence_block_ns; r.b = acquire_block_ns;
    r.c = queue_present_block_ns; r.d = recreate_block_ns;
    pace_emit(&r);
}

void openpete_pace_shed(uint8_t kind, uint16_t reason,
                        int64_t predicted_cost_ns, int64_t deadline_margin_ns)
{
    s_census.sheds++;
    if (!s_fp) return;
    pace_rec_t r = {0};
    r.type  = OP_PACE_SHED;
    r.flags = kind;
    r.aux   = reason;
    r.t_ns  = op_now();
    r.a = predicted_cost_ns; r.b = deadline_margin_ns;
    pace_emit(&r);
}

void openpete_pace_slip(uint8_t kind, int64_t periods, int64_t behind_ns)
{
    const unsigned k = (kind == OP_PACE_SLIP_RENDER) ? 1u : 0u;
    s_census.slip_events[k]++;
    s_census.slip_periods[k] += (uint64_t)(periods > 0 ? periods : 0);
    if (!s_fp) return;
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
    s_census.input_edges++;
    if (!s_fp) return;
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

    if (over > s_census.wake_overshoot_max_ns)
        s_census.wake_overshoot_max_ns = over;
    if (behind > s_census.wake_behind_max_ns)
        s_census.wake_behind_max_ns = behind;
    s_census.wakes++;
    if (!s_fp) return;
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
    if (!s_fp) return;
    pace_rec_t r = {0};
    r.type  = OP_PACE_PRESENT_SPAN;
    r.flags = kind;
    r.t_ns  = end_ns;
    r.a     = start_ns;
    r.b     = end_ns - start_ns;
    r.c     = extract_ns;
    pace_emit(&r);
}

void openpete_pace_window(uint16_t id, int begin)
{
    if (!s_fp) return;
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
    if (out) *out = s_census;
}

void openpete_pace_census_format(char* buf, unsigned buf_bytes)
{
    const openpete_pace_census_t* c = &s_census;

    if (c->vbls_paced == 0 || c->vbls_clean == 0) {
        /* An unlocked run (headless fast-forward, Tab, --record-cfr's virtual
         * clock) never followed the grid, so quoting a phase error against it
         * would be a number about nothing. Report engagement instead. */
        /* Either nothing ran against a wall deadline, or everything that did
         * was a transient. Quoting a phase error against a grid nothing
         * followed would be a number about nothing; report engagement. */
        SDL_snprintf(buf, buf_bytes,
            "unpaced (%llu VBL, %llu present) — no deadline was in force; "
            "slips sim %llu/%llu render %llu/%llu",
            (unsigned long long)c->vbls,
            (unsigned long long)(c->presents[0] + c->presents[1]),
            (unsigned long long)c->slip_events[0],
            (unsigned long long)c->slip_periods[0],
            (unsigned long long)c->slip_events[1],
            (unsigned long long)c->slip_periods[1]);
        return;
    }

    /* Fraction of clean paced VBLs that started within the 250 µs S1 goal —
     * the one number worth reading at a glance. Buckets 0..2 are (early,
     * <100 µs, <250 µs); bucket 0 is early starts, which the loop cannot
     * produce today (sim_due gates on now >= deadline) but which a future
     * pacer change could, and an early start is as much a phase error as a
     * late one. */
    const uint64_t early   = c->phase_hist[0];
    const uint64_t on_grid = c->phase_hist[1] + c->phase_hist[2];
    const double pct = c->vbls_clean
        ? 100.0 * (double)on_grid / (double)c->vbls_clean : 0.0;

    /* An unmeasured quantity prints as n/a. A latch bound of "0.000 ms" on a
     * run where no sample ever contributed reads as a perfect result, which
     * is worse than an absent one. */
    char latch[96], early_s[48];
    if (c->latch_samples)
        SDL_snprintf(latch, sizeof(latch), "%.3f ms over %llu VBL",
                     (double)c->latch_offset_max_ns / 1e6,
                     (unsigned long long)c->latch_samples);
    else
        SDL_strlcpy(latch, "n/a", sizeof(latch));
    /* A run where another writer owned the pad throughout (bk2 replay) has no
     * host input phase to report. Saying so is the difference between an
     * absent measurement and a flattering one. */
    if (c->vbls_claimed) {
        char tail[48];
        SDL_snprintf(tail, sizeof(tail), " (+%llu claimed)",
                     (unsigned long long)c->vbls_claimed);
        SDL_strlcat(latch, tail, sizeof(latch));
    }
    if (early)
        SDL_snprintf(early_s, sizeof(early_s), ", %llu early (max %+.3f ms)",
                     (unsigned long long)early,
                     (double)c->phase_err_min_ns / 1e6);
    else
        early_s[0] = '\0';

    /* Three distinguishable states, because two of them are absences: no
     * probe wired (unmeasured), a probe that never fired (a real and alarming
     * finding — the game stopped reading its pad), and a measurement. */
    /* Reads by fiber runs that were not the VBL they landed in — speculation
     * under runahead, rewind replay. Real reads, excluded from the phase
     * distribution, and named rather than dropped so "exactly one per VBL"
     * cannot quietly mean "one per VBL plus some others we ignored". */
    char unattr[48];
    if (c->pad_reads_unattributed)
        SDL_snprintf(unattr, sizeof(unattr), " (+%llu speculative/replayed)",
                     (unsigned long long)c->pad_reads_unattributed);
    else
        unattr[0] = '\0';

    char padr[176];
    if (!s_pad_probe_armed)
        SDL_strlcpy(padr, "pad read: no probe", sizeof(padr));
    else if (c->pad_read_vbls == 0)
        SDL_strlcpy(padr, "pad read: NEVER (probe armed)", sizeof(padr));
    else
        SDL_snprintf(padr, sizeof(padr),
                     "pad read: %llu in %llu VBL(s)%s, <=%+.3f ms into the "
                     "VBL, sample age <=%+.3f ms",
                     (unsigned long long)c->pad_reads,
                     (unsigned long long)c->pad_read_vbls,
                     unattr,
                     (double)c->pad_read_offset_max_ns / 1e6,
                     (double)c->pad_read_age_max_ns / 1e6);

    /* The seqlock never failing is the expected state, so it prints nothing;
     * a single failure means some latch consumed no sample at all and must
     * not hide inside a healthy-looking line. */
    char seqfail[64];
    const unsigned sf = openpete_host_input_seqlock_failures();
    if (sf) SDL_snprintf(seqfail, sizeof(seqfail),
                         "; INPUT SEQLOCK FAILED %u time(s)", sf);
    else    seqfail[0] = '\0';

    SDL_snprintf(buf, buf_bytes,
        "%llu VBL (%llu paced, %llu clean); %llu+%llu present; "
        "phase %.2f%% within 250us, max %+.3f ms%s; "
        "transient %llu VBL max %+.3f ms; wake<=%+.3f ms behind<=%+.3f ms; "
        "latch<=%s; %s; %llu input edge(s); "
        "slips sim %llu/%llu render %llu/%llu; sheds %llu%s",
        (unsigned long long)c->vbls,
        (unsigned long long)c->vbls_paced,
        (unsigned long long)c->vbls_clean,
        (unsigned long long)c->presents[0],
        (unsigned long long)c->presents[1],
        pct,
        (double)c->phase_err_max_ns / 1e6,
        early_s,
        (unsigned long long)c->vbls_transient,
        (double)c->phase_err_transient_max_ns / 1e6,
        (double)c->wake_overshoot_max_ns / 1e6,
        (double)c->wake_behind_max_ns / 1e6,
        latch, padr,
        (unsigned long long)c->input_edges,
        (unsigned long long)c->slip_events[0],
        (unsigned long long)c->slip_periods[0],
        (unsigned long long)c->slip_events[1],
        (unsigned long long)c->slip_periods[1],
        (unsigned long long)c->sheds,
        seqfail);
}

/* ------------------------------------------------------------------ */
/* instrument self-verification                                       */
/* ------------------------------------------------------------------ */

int64_t openpete_pace_inject_present(void)
{
    static int     s_init;
    static int64_t s_us;
    if (!s_init) {
        s_init = 1;
        const char* e = getenv("OPENPETE_PACE_INJECT_PRESENT_US");
        s_us = (e && *e) ? strtoll(e, NULL, 10) : 0;
        if (s_us > 0)
            OPL_WARN_F("pace",
                       "INJECTING %lld us into every intermediate present — "
                       "shed-predicate fault injection, never a play path",
                       (long long)s_us);
    }
    if (s_us <= 0 || op_virtual_clock_active()) return 0;
    const op_time_t until = op_now() + s_us * OP_NS_PER_US;
    while (op_now() < until) { }
    return s_us;
}

int64_t openpete_pace_inject_canonical(void)
{
    static int     s_init;
    static int64_t s_us;
    if (!s_init) {
        s_init = 1;
        const char* e = getenv("OPENPETE_PACE_INJECT_CANON_US");
        s_us = (e && *e) ? strtoll(e, NULL, 10) : 0;
        if (s_us > 0)
            OPL_WARN_F("pace",
                       "INJECTING %lld us into every CANONICAL present — "
                       "a present that cannot be shed; fault injection, "
                       "never a play path",
                       (long long)s_us);
    }
    if (s_us <= 0 || op_virtual_clock_active()) return 0;
    const op_time_t until = op_now() + s_us * OP_NS_PER_US;
    while (op_now() < until) { }
    return s_us;
}

int64_t openpete_pace_inject(int paced)
{
    static int     s_init;
    static int64_t s_us;
    static int64_t s_every = 30;
    static uint64_t s_seen;

    if (!s_init) {
        s_init = 1;
        const char* e = getenv("OPENPETE_PACE_INJECT_US");
        if (e && *e) {
            char* end = NULL;
            s_us = strtoll(e, &end, 10);
            if (end && *end == ',') {
                const int64_t every = strtoll(end + 1, NULL, 10);
                if (every > 0) s_every = every;
            }
            if (s_us > 0)
                OPL_WARN_F("pace",
                           "INJECTING a %lld us stall every %lld paced VBL — "
                           "instrument calibration only, never a play path",
                           (long long)s_us, (long long)s_every);
        }
    }
    if (s_us <= 0 || !paced) return 0;
    /* The spin below waits on op_now, which the virtual clock advances only
     * from op_sleep_until — so spinning on it there never terminates. */
    if (op_virtual_clock_active()) return 0;
    if ((s_seen++ % (uint64_t)s_every) != 0) return 0;

    /* Busy-wait, not a sleep: a sleep would be absorbed by the pacer's own
     * slack accounting on some platforms and measure the sleep primitive
     * rather than the log's ability to see a late start. */
    const op_time_t until = op_now() + s_us * OP_NS_PER_US;
    while (op_now() < until) { }
    return s_us;
}
