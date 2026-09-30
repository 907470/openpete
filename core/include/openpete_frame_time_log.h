/* openpete_frame_time_log.h — binary frame-time recorder.
 *
 * Off by default;
 * activated by `--frame-time-log PATH`. Writes one 32-byte record per
 * present, plus a 32-byte header up front.
 *
 *   header (32 bytes):
 *     u32 magic            'OFTL' = 0x4C54464F  (LE)
 *     u32 version          1
 *     i64 target_period_ns nominal pacing period (e.g. 33430222 for 29.913)
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

/** @brief Zero log, open path for binary writing, and write the 32-byte header.
 *
 * The header carries the magic, version 1, target_period_ns, the creation
 * time in Unix nanoseconds, and the record size 32; its fwrite results are
 * not checked. A failed fopen is logged at error level.
 * @param log Recorder state; zeroed first, so a failed open leaves it closed.
 * @param path Output file path; NULL or empty fails.
 * @param target_period_ns Nominal present period stored in the header.
 * @return 0 on success, -1 when log is NULL, path is NULL or empty, or fopen
 *         fails. */
int  openpete_frame_time_log_open(openpete_frame_time_log_t* log,
                                  const char* path,
                                  int64_t target_period_ns);

/** @brief Append one 32-byte record with a single fwrite, without a flush.
 *
 * No-op when log is NULL or not open. Increments records_written, which the
 * caller in run.c uses to advance the synthetic deadline by one period per
 * present.
 * @param log Open recorder.
 * @param frame openpete_frame at the present; repeats across a tick's
 *        sub-tick presents.
 * @param slip_count Missed-deadline count; the run.c caller writes 0.
 * @param op_now_ns op_now() at the present.
 * @param deadline_ns The present's ideal time.
 * @param slack_ns deadline_ns - op_now_ns; negative is an overshoot. */
void openpete_frame_time_log_record(openpete_frame_time_log_t* log,
                                    uint32_t frame,
                                    uint32_t slip_count,
                                    int64_t  op_now_ns,
                                    int64_t  deadline_ns,
                                    int64_t  slack_ns);

/** @brief Flush and close the file and log the record count and size at info level.
 *
 * No-op when log is NULL or not open; fp is NULL afterwards, so a second
 * call is a no-op too.
 * @param log Recorder to close. */
void openpete_frame_time_log_close(openpete_frame_time_log_t* log);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_FRAME_TIME_LOG_H */
