/* tests/unit/psxtrace_schedule_test.c - the schedule's account of a CdRead
 * or waited VSync that its sidecars have no row left for.
 *
 * The schedule replays the reference capture's sidecars (`.loads`,
 * `.vsync`) against the trace's per-record stamps. A call past the
 * sidecar's rows after the trace's last record is one the capture stopped
 * before: it is counted as past the trace end and the schedule stays
 * live. The same read inside the records is a call the reference never
 * made, a schedule failure. The schedule keeps its state in file statics,
 * so each case runs in its own process (argv[1]: past-end or inside).
 */
#include "openpete_psxtrace.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The trace verifier's anchor counters, which the schedule reads for the
 * current record: record = hits - target + 1. */
uint32_t openpete_psxtrace_anchor_hits = 0;
uint32_t openpete_psxtrace_anchor_hit_target = 1;

/** @brief Accept the root counter 2 reader the schedule installs for an `.rcnt2` sidecar; the cases carry none.
 *
 * @param reader The reader, ignored. */
void openpete_psx_set_root_counter2_reader(int (*reader)(uint16_t*)) { (void)reader; }

static int g_fail = 0;

/** @brief Print a failed check and mark the case failed.
 *
 * @param ok The check's result.
 * @param what What was checked. */
static void check(int ok, const char* what) {
    if (!ok) {
        printf("FAIL: %s\n", what);
        g_fail = 1;
    }
}

/** @brief Write text to the file path + ext.
 *
 * @param path Trace path the sidecar sits beside.
 * @param ext Sidecar suffix.
 * @param text File contents.
 * @return 1 when written, else 0. */
static int write_file(const char* path, const char* ext, const char* text) {
    char p[512];
    snprintf(p, sizeof(p), "%s%s", path, ext);
    FILE* fp = fopen(p, "wb");
    if (!fp) return 0;
    fputs(text, fp);
    fclose(fp);
    return 1;
}

/** @brief Remove the sidecars and the captured log of a case. */
static void cleanup(const char* path) {
    const char* exts[] = { ".loads", ".vsync", ".log" };
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        char p[512];
        snprintf(p, sizeof(p), "%s%s", path, exts[i]);
        remove(p);
    }
}

/** @brief Run one case and return its exit status.
 *
 * Two records stamped at VBLANK 10 and 20; `.loads` holds one read issued
 * in record 1, `.vsync` one waited row of record 1. The first read and wait
 * consume those rows. The second read is made in record 2 (past-end) or
 * again in record 1 (inside).
 * @param past_end 1 for the past-end case, 0 for the inside case.
 * @return 0 when every check passed, else 1. */
static int run_case(int past_end) {
    const char* trace = past_end ? "psxtrace_schedule_test.past-end" : "psxtrace_schedule_test.inside";
    cleanup(trace);
    static const uint32_t stamps[2] = { 10, 20 };
    check(write_file(trace, ".loads", "psxtrace-loads v1\n0 1 12 1200 0 4 80010000 80 1 14 1400\n"), "write .loads");
    check(write_file(trace, ".vsync", "psxtrace-vsync v1\n0 1 0 15 16 1500 1600 0\n"), "write .vsync");
    check(openpete_sched_open(trace) == 1, "schedule opens the sidecars");
    openpete_sched_set_stamps(stamps, 2);

    uint32_t issue = 0, done = 0, done_rec = 0, target = 0;
    int sync = 0;
    openpete_psxtrace_anchor_hits = 1;   /* record 1 */
    check(openpete_sched_read(4, 0x80010000u, &issue, &done, &done_rec, &sync) == 1 && issue == 12,
          "the first read takes the sidecar's row");
    check(openpete_sched_next_vsync(0, &target) == 1 && target == 16, "the first wait takes the sidecar's row");

    if (past_end) openpete_psxtrace_anchor_hits = 2;   /* record 2: past the last stamp */
    check(openpete_sched_read(4, 0x80020000u, &issue, &done, &done_rec, &sync) == 0, "the second read finds no row");
    const char* reason = NULL;
    const int failed = openpete_sched_failed(&reason);
    if (past_end) {
        check(!failed, "a read past the trace end is no failure");
        check(openpete_sched_next_vsync(0, &target) == 0, "a wait past the trace end finds no row");
        check(!openpete_sched_failed(NULL), "a wait past the trace end is no failure");
    } else {
        check(failed && reason && strstr(reason, "CdRead 1 beyond the sidecar's 1 reads") != NULL,
              "a read beyond the sidecar inside the records fails the schedule");
    }

    /* The summary line names the counts; capture stderr to read it. */
    char logp[512];
    snprintf(logp, sizeof(logp), "%s.log", trace);
    fflush(stderr);
    check(freopen(logp, "w", stderr) != NULL, "redirect stderr");
    openpete_sched_summary();
    fflush(stderr);
    FILE* fp = fopen(logp, "rb");
    char buf[8192] = { 0 };
    size_t got = 0;
    if (fp) { got = fread(buf, 1, sizeof(buf) - 1, fp); fclose(fp); }
    check(got > 0, "the summary line was written");
    if (past_end) {
        check(strstr(buf, "1 VSync call(s) and 1 read(s) past the trace end") != NULL,
              "the summary counts one wait and one read past the trace end");
        check(strstr(buf, "FAILED") == NULL, "the summary names no failure");
    } else {
        check(strstr(buf, "0 VSync call(s) and 0 read(s) past the trace end") != NULL,
              "the summary counts nothing past the trace end");
        check(strstr(buf, "FAILED: CdRead 1 beyond") != NULL, "the summary names the failure");
    }
    cleanup(trace);
    printf("%s: %s\n", past_end ? "past-end" : "inside", g_fail ? "FAIL" : "PASS");
    return g_fail;
}

/** @brief Run the case argv[1] names (past-end or inside).
 *
 * @param argc Argument count.
 * @param argv Arguments.
 * @return 0 on PASS, 1 on a failed check, 64 on a usage error. */
int main(int argc, char** argv) {
    if (argc != 2) { fprintf(stderr, "usage: psxtrace_schedule_test past-end|inside\n"); return 64; }
    if (strcmp(argv[1], "past-end") == 0) return run_case(1);
    if (strcmp(argv[1], "inside") == 0) return run_case(0);
    fprintf(stderr, "psxtrace_schedule_test: unknown case %s\n", argv[1]);
    return 64;
}
