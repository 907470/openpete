/* op_sched_internal.h - the contract between the explorer (parent) and the
 * scheduler runtime (forked child). */
#ifndef OPENPETE_SCHED_INTERNAL_H
#define OPENPETE_SCHED_INTERNAL_H

#include "op_sched.h"

/** Most choices one schedule may make. */
#define SCHED_MAX_CHOICES 65536
/** Most alternatives at one choice. */
#define SCHED_MAX_ALTS 64

/** Report record tags, child to parent. */
enum { SCHED_REC_CHOICE = 'C', SCHED_REC_END = 'E' };

/** Child exit statuses. */
enum { SCHED_CHILD_PASS = 0, SCHED_CHILD_FAIL = 1 };

/** What the explorer hands a child before it runs. */
typedef struct sched_child_plan {
    const sched_config *cfg;
    const uint16_t *prefix; /**< choices to replay before taking defaults */
    int prefix_len;
    int report_fd;          /**< pipe to the explorer, or -1 */
    int pct;                /**< draw choices by PCT after the prefix */
    uint64_t pct_seed;
    const char *trace_path; /**< where a failing (or every, when trace_always) trace goes */
    int trace_always;
} sched_child_plan;

/** @brief Run one schedule in the calling (forked, single-threaded) process and exit with its status.
 * @param plan What to run. */
void sched_child_run(const sched_child_plan *plan) __attribute__((noreturn));

/** @brief Write all of a buffer to a descriptor, retrying short writes.
 * @param fd Descriptor.
 * @param buf Bytes.
 * @param n Length.
 * @return 0 on success, -1 on error. */
int sched_write_all(int fd, const void *buf, size_t n);

#endif
