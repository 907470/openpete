/* openpete_frame_time_log.c - see header for format + rationale. */

#include "openpete_frame_time_log.h"
#include "openpete_log.h"

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>          /* FILETIME, GetSystemTimeAsFileTime */
#endif

#include <errno.h>
#include <string.h>
#include <time.h>

#define OFTL_MAGIC   0x4C54464Fu /* 'OFTL' little-endian */
#define OFTL_VERSION 1u
#define OFTL_REC_SIZE 32u

/** @brief Return the wall-clock time in nanoseconds since the Unix epoch, 0 when the POSIX clock read fails. */
static int64_t now_unix_ns(void) {
#if defined(_WIN32)
    /* GetSystemTimeAsFileTime gives 100-ns ticks since 1601-01-01. */
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t hundreds = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    /* 11644473600 sec from 1601 to 1970 epoch. */
    const uint64_t epoch_diff_100ns = 116444736000000000ULL;
    return (int64_t)((hundreds - epoch_diff_100ns) * 100ull);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
#endif
}

int openpete_frame_time_log_open(openpete_frame_time_log_t* log,
                                 const char* path,
                                 int64_t target_period_ns) {
    if (!log) return -1;
    memset(log, 0, sizeof(*log));
    if (!path || !*path) return -1;
    log->fp = fopen(path, "wb");
    if (!log->fp) {
        OPL_ERROR_F("frame-time-log", "fopen %s failed: %s",
                    path, strerror(errno));
        return -1;
    }
    log->target_period_ns = target_period_ns;

    /* Header (32 bytes, little-endian). */
    uint32_t magic        = OFTL_MAGIC;
    uint32_t version      = OFTL_VERSION;
    int64_t  period       = target_period_ns;
    int64_t  created      = now_unix_ns();
    uint32_t record_size  = OFTL_REC_SIZE;
    uint32_t reserved     = 0;
    fwrite(&magic,       4, 1, log->fp);
    fwrite(&version,     4, 1, log->fp);
    fwrite(&period,      8, 1, log->fp);
    fwrite(&created,     8, 1, log->fp);
    fwrite(&record_size, 4, 1, log->fp);
    fwrite(&reserved,    4, 1, log->fp);
    OPL_INFO_F("frame-time-log",
               "writing 32B records (target=%.3f ms) to %s",
               (double)target_period_ns / 1e6, path);
    return 0;
}

void openpete_frame_time_log_record(openpete_frame_time_log_t* log,
                                    uint32_t frame,
                                    uint32_t slip_count,
                                    int64_t  op_now_ns,
                                    int64_t  deadline_ns,
                                    int64_t  slack_ns) {
    if (!log || !log->fp) return;
    /* Pack as: u32 frame, u32 slip, i64 now, i64 deadline, i64 slack. */
    uint8_t buf[OFTL_REC_SIZE];
    memcpy(buf +  0, &frame,        4);
    memcpy(buf +  4, &slip_count,   4);
    memcpy(buf +  8, &op_now_ns,    8);
    memcpy(buf + 16, &deadline_ns,  8);
    memcpy(buf + 24, &slack_ns,     8);
    fwrite(buf, 1, OFTL_REC_SIZE, log->fp);
    log->records_written++;
}

void openpete_frame_time_log_close(openpete_frame_time_log_t* log) {
    if (!log || !log->fp) return;
    fflush(log->fp);
    fclose(log->fp);
    OPL_INFO_F("frame-time-log",
               "closed: %llu records (%.1f KB)",
               (unsigned long long)log->records_written,
               (double)(log->records_written * OFTL_REC_SIZE + 32) / 1024.0);
    log->fp = NULL;
}
