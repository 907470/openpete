/* The host loop's decision trace; see openpete_host_trace.h. */
#include "openpete_host_trace.h"

#include "openpete_fiber.h"
#include "openpete_log.h"
#include "openpete_time.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

host_trace_cur_t g_host_trace;

static FILE*    s_fp OP_GUARDED_BY(sim);      /* thread-roles: the host loop's trace writer runs on sim */
static uint32_t s_iter OP_GUARDED_BY(sim);    /* thread-roles: written per host iteration on sim */
static uint8_t  s_steps[HT_CLOSE_STEPS_PER_REC] OP_GUARDED_BY(sim);  /* thread-roles: teardown steps run on sim */
static unsigned s_nsteps OP_GUARDED_BY(sim);  /* thread-roles: teardown steps run on sim */

/** @brief Store a 32-bit value little-endian.
 *
 * @param p Destination, 4 bytes.
 * @param v The value. */
static void put32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/** @brief Store a 64-bit value little-endian.
 *
 * @param p Destination, 8 bytes.
 * @param v The value. */
static void put64(uint8_t* p, int64_t v)
{
    put32(p, (uint32_t)(uint64_t)v);
    put32(p + 4, (uint32_t)((uint64_t)v >> 32));
}

/** @brief Write one record and flush it, so the stream survives a kill or an exec.
 *
 * @param rec HT_REC_BYTES bytes. */
static void write_rec(const uint8_t* rec) OP_REQUIRES(sim)
{
    if (fwrite(rec, 1, HT_REC_BYTES, s_fp) != HT_REC_BYTES || fflush(s_fp) != 0) {
        OPL_ERROR_F("loop-trace", "write failed: %s - trace closed", strerror(errno));
        fclose(s_fp);
        s_fp = NULL;
        g_host_trace.on = 0;
    }
}

void host_trace_open(int resumed, int headless) OP_REQUIRES(sim)
{
    const char* path = getenv("OPENPETE_LOOP_TRACE");
    if (!path || !*path) return;
    s_fp = fopen(path, resumed ? "ab" : "wb");
    if (!s_fp) {
        OPL_ERROR_F("loop-trace", "fopen %s failed: %s", path, strerror(errno));
        return;
    }
    g_host_trace.on = 1;
    uint8_t rec[HT_REC_BYTES] = {0};
    rec[0] = HT_REC_HEADER;
    memcpy(rec + 8, "OPLT", 4);
    put32(rec + 12, HT_VERSION);
    put32(rec + 16, (resumed ? HT_H_RESUMED : 0u) | (headless ? HT_H_HEADLESS : 0u));
    write_rec(rec);
    OPL_INFO_F("loop-trace", "writing one %d-byte record per host iteration to %s%s",
               HT_REC_BYTES, path, resumed ? " (appended: handoff resume)" : "");
}

/** @brief Map an exit reason string to its trace code.
 *
 * @param r The loop's exit reason, or NULL.
 * @return The host_trace_exit_t code; HT_X_OTHER for an unlisted string. */
static uint8_t exit_code_of(const char* r)
{
    static const char* const k_names[] = {
        NULL, "frame-limit", "bk2-end", "psxtrace-eof", "psxtrace-divergence",
        "psxtrace-error", "user-quit", "game-returned", "record-frames",
        "max-runtime-seconds",
    };
    if (!r) return HT_X_NONE;
    for (unsigned i = 1; i < sizeof k_names / sizeof k_names[0]; i++)
        if (strcmp(r, k_names[i]) == 0) return (uint8_t)i;
    return HT_X_OTHER;
}

/* The always-on ring of iteration records: s_ring[0, s_ring_n) holds the
 * records of iterations s_ring_first .. s_ring_first + s_ring_n - 1. */
#define HT_RING_MAX 32768u
static uint8_t* s_ring OP_GUARDED_BY(sim);          /* thread-roles: the host loop writes it on sim */
static size_t   s_ring_n OP_GUARDED_BY(sim), s_ring_cap OP_GUARDED_BY(sim);  /* thread-roles: sim */
static uint32_t s_ring_first OP_GUARDED_BY(sim);    /* thread-roles: sim */
static unsigned long long s_ring_dropped OP_GUARDED_BY(sim);  /* thread-roles: sim */

/** @brief Append one iteration record to the ring, dropping the oldest eighth when it holds HT_RING_MAX.
 * @param rec HT_REC_BYTES bytes. */
static void ring_put(const uint8_t* rec) OP_REQUIRES(sim)
{
    if (s_ring_n == HT_RING_MAX) {
        const size_t drop = HT_RING_MAX / 8u;
        memmove(s_ring, s_ring + drop * HT_REC_BYTES, (s_ring_n - drop) * HT_REC_BYTES);
        s_ring_n -= drop;
        s_ring_first += (uint32_t)drop;
        s_ring_dropped += drop;
    }
    if (s_ring_n == s_ring_cap) {
        const size_t cap = s_ring_cap ? s_ring_cap * 2u : 4096u;
        uint8_t* g = (uint8_t*)realloc(s_ring, cap * HT_REC_BYTES);
        if (!g) { s_ring_dropped++; return; }
        s_ring = g;
        s_ring_cap = cap;
    }
    memcpy(s_ring + s_ring_n++ * HT_REC_BYTES, rec, HT_REC_BYTES);
}

uint32_t host_trace_iter_index(void) OP_REQUIRES(sim) { return s_iter; }

void host_trace_ring_retain_from(uint32_t iter) OP_REQUIRES(sim)
{
    if (iter <= s_ring_first) return;
    size_t drop = iter - s_ring_first;
    if (drop > s_ring_n) drop = s_ring_n;
    memmove(s_ring, s_ring + drop * HT_REC_BYTES, (s_ring_n - drop) * HT_REC_BYTES);
    s_ring_n -= drop;
    s_ring_first = iter;
}

size_t host_trace_ring_records(const uint8_t** recs) OP_REQUIRES(sim)
{
    *recs = s_ring;
    return s_ring_n;
}

size_t host_trace_ring_held(size_t* bytes) OP_REQUIRES(sim)
{
    if (bytes) *bytes = s_ring_cap * HT_REC_BYTES;
    return s_ring_n;
}

void host_trace_iter_end(int quit, const char* exit_reason, uint8_t pace_flags,
                         int64_t next_sim, int64_t next_render) OP_REQUIRES(sim)
{
    g_host_trace.open = 0;
    if (op_virtual_clock_active()) g_host_trace.flags |= HT_F_VCLOCK;
    uint8_t rec[HT_REC_BYTES] = {0};
    rec[0] = HT_REC_ITER;
    rec[1] = g_host_trace.present;
    rec[2] = g_host_trace.ra;
    rec[3] = quit ? exit_code_of(exit_reason) : (uint8_t)HT_X_NONE;
    put32(rec + 4, g_host_trace.flags);
    put32(rec + 8, s_iter++);
    put32(rec + 12, openpete_frame);
    put32(rec + 16, openpete_feed_key_now());
    put32(rec + 20, openpete_canon_tick);
    rec[24] = pace_flags;
    rec[25] = g_host_trace.verdict;
    put64(rec + 32, next_sim);
    put64(rec + 40, next_render);
    ring_put(rec);
    if (s_fp) write_rec(rec);
}

void host_trace_loop_end(const char* exit_reason) OP_REQUIRES(sim)
{
    if (g_host_trace.open) host_trace_iter_end(1, exit_reason, 0, 0, 0);
}

/** @brief Write the buffered close steps as one close record. */
static void flush_steps(void) OP_REQUIRES(sim)
{
    uint8_t rec[HT_REC_BYTES] = {0};
    rec[0] = HT_REC_CLOSE;
    rec[1] = (uint8_t)s_nsteps;
    memcpy(rec + 8, s_steps, s_nsteps);
    s_nsteps = 0;
    write_rec(rec);
}

void host_trace_close_step(host_trace_step_t step) OP_REQUIRES(sim)
{
    if (!s_fp) return;
    s_steps[s_nsteps++] = (uint8_t)step;
    if (s_nsteps == HT_CLOSE_STEPS_PER_REC) flush_steps();
}

void host_trace_finish(void) OP_REQUIRES(sim)
{
    if (!s_fp) return;
    flush_steps();
    fclose(s_fp);
    s_fp = NULL;
    g_host_trace.on = 0;
}
