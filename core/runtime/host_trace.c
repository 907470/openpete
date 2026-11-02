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

void host_trace_iter_end(int quit, const char* exit_reason, uint8_t pace_flags,
                         int64_t next_sim, int64_t next_render) OP_REQUIRES(sim)
{
    g_host_trace.open = 0;
    if (!s_fp) return;
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
    write_rec(rec);
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
