/* openpete_frame_time_log.h — binary frame-time recorder.
 *
 * Phase 4.1 of notes/plans/openpete-frame-pacing.md. Off by default;
 * activated by `--frame-time-log PATH`. Writes one 32-byte record per
 * present, plus a 32-byte header up front.
 *
 *   header (32 bytes):
 *     u32 magic            'OFTL' = 0x4C54464F  (LE)
 *     u32 version          1
 *     i64 target_period_ns nominal pacing period (e.g. 33366667 for 29.97)
 *     i64 created_unix_ns
 *     u32 record_size      32
 *     u32 reserved         0
 *
 *   record (32 bytes):
 *     u32 frame
 *     u32 slip_count
 *     i64 op_now_ns
 *     i64 deadline_ns
 *     i64 slack_ns         (deadline_ns - op_now_ns; signed, negative = overshoot)
 *
 * Consumed by scripts/measure-pacing.py.
 *
 * Caller is responsible for picking sensible deadline / slip values.
 * Until Phase 1 wires the real high-res pacer, the baseline path can
 * synthesise a deadline as `start_op_ns + frame * target_period_ns`,
 * which gives an honest read of the legacy ticker's wall-clock drift
 * against an ideal cadence — exactly the before/after we want.
 */

#ifndef OPENPETE_FRAME_TIME_LOG_H
#define OPENPETE_FRAME_TIME_LOG_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct openpete_frame_time_log {
    FILE*    fp;
    int64_t  target_period_ns;
    uint64_t records_written;
} openpete_frame_time_log_t;

/* Open PATH for writing and emit the header. Returns 0 on success. */
int  openpete_frame_time_log_open(openpete_frame_time_log_t* log,
                                  const char* path,
                                  int64_t target_period_ns);

/* Append one record. Cheap (single 32-byte fwrite, no flush per call). */
void openpete_frame_time_log_record(openpete_frame_time_log_t* log,
                                    uint32_t frame,
                                    uint32_t slip_count,
                                    int64_t  op_now_ns,
                                    int64_t  deadline_ns,
                                    int64_t  slack_ns);

/* Flush + close. Safe to call on an unopened log (no-op). */
void openpete_frame_time_log_close(openpete_frame_time_log_t* log);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_FRAME_TIME_LOG_H */
