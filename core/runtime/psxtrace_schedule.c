/* core/runtime/psxtrace_schedule.c — the reference's timing, replayed.
 *
 * A golden captured with the DuckStation fork's recorders carries sidecars
 * beside the trace: `<trace>.vsync` (every waited libetc VSync call, with
 * the VBLANK count at entry and return and the counter the next query
 * returned), `<trace>.loads` (every CdRead with its issue and completion
 * VBLANK) and `<trace>.srand` (every srand seed). When the trace is
 * verified they become openpete's schedule: each waited VSync charges the
 * VBLs the reference spent since the previous one, a synchronous read
 * charges its VBLs inside the read, an asynchronous read completes after
 * the reference's delay, the game-visible VBL counter reads the
 * reference's value, and srand receives the reference's seed. The thunk
 * layer consumes the rows in order; a row whose shape differs from what
 * openpete issues (mode, read sectors or destination, exhausted rows at
 * the wrong record) is a schedule failure the verifier reports as a hard
 * fail with the record.
 */
#include "openpete_psxtrace.h"
#include "openpete_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef struct { uint32_t rec; int32_t mode; uint32_t entry_vbl, ret_vbl; int32_t counter; } sched_vsync_t;
typedef struct { uint32_t idx, issue_rec, issue_vbl, sectors, dst, mode, done_rec, done_vbl; int done; } sched_read_t;

static sched_vsync_t* s_vs = NULL;   static uint32_t s_vs_n = 0, s_vs_next = 0;
static sched_read_t*  s_rd = NULL;   static uint32_t s_rd_n = 0, s_rd_next = 0;
static uint32_t*      s_seed = NULL; static uint32_t s_seed_n = 0, s_seed_next = 0;
static int      s_active = 0;
static FILE*    s_log = NULL;        /* OPENPETE_SCHED_LOG=path: every consumed row */
static int      s_failed = 0;
static char     s_fail_reason[256];
static uint32_t s_reads_sync = 0, s_reads_async = 0, s_vsync_consumed = 0, s_long_frames = 0;

int openpete_sched_active(void) { return s_active; }
int openpete_sched_failed(const char** reason) {
    if (reason) *reason = s_fail_reason;
    return s_failed;
}

void openpete_sched_fail(const char* fmt, ...) {
    if (s_failed) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(s_fail_reason, sizeof(s_fail_reason), fmt, ap);
    va_end(ap);
    s_failed = 1;
    OPL_ERROR_F("psxtrace", "schedule: %s", s_fail_reason);
}

static char* read_sidecar(const char* trace, const char* ext, const char* magic) {
    char path[4096];
    snprintf(path, sizeof(path), "%s%s", trace, ext);
    FILE* fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)n + 1u);
    if (!buf || fread(buf, 1, (size_t)n, fp) != (size_t)n) { free(buf); fclose(fp); return NULL; }
    fclose(fp);
    buf[n] = 0;
    if (strncmp(buf, magic, strlen(magic)) != 0) {
        OPL_ERROR_F("psxtrace", "schedule: %s is not a %s file", path, magic);
        free(buf);
        return NULL;
    }
    return buf;
}

static int load_vsync(const char* trace) {
    char* buf = read_sidecar(trace, ".vsync", "psxtrace-vsync");
    if (!buf) return 0;
    uint32_t cap = 1024;
    s_vs = (sched_vsync_t*)malloc(cap * sizeof(*s_vs));
    sched_vsync_t* last_wait = NULL;
    for (char* line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] == '#' || line[0] == 'p') continue;
        unsigned idx, rec, evbl, rvbl; int mode; unsigned long long et, rt; unsigned v0;
        if (sscanf(line, "%u %u %d %u %u %llu %llu %u", &idx, &rec, &mode, &evbl, &rvbl, &et, &rt, &v0) != 8)
            continue;
        if (mode < 0) {
            /* The query after a wait: the counter the game saw for it. */
            if (last_wait && last_wait->counter < 0) last_wait->counter = (int32_t)v0;
            continue;
        }
        if (s_vs_n == cap) { cap *= 2; s_vs = (sched_vsync_t*)realloc(s_vs, cap * sizeof(*s_vs)); }
        s_vs[s_vs_n].rec = rec; s_vs[s_vs_n].mode = mode;
        s_vs[s_vs_n].entry_vbl = evbl; s_vs[s_vs_n].ret_vbl = rvbl; s_vs[s_vs_n].counter = -1;
        last_wait = &s_vs[s_vs_n];
        s_vs_n++;
    }
    free(buf);
    return 1;
}

static int load_reads(const char* trace) {
    char* buf = read_sidecar(trace, ".loads", "psxtrace-loads");
    if (!buf) return 0;
    uint32_t cap = 256;
    s_rd = (sched_read_t*)malloc(cap * sizeof(*s_rd));
    for (char* line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] == '#' || line[0] == 'p') continue;
        unsigned idx, irec, ivbl, iline, sectors, dst, mode; unsigned long long itick;
        char d_rec[32], d_vbl[32];
        if (sscanf(line, "%u %u %u %llu %u %u %x %x %31s %31s", &idx, &irec, &ivbl, &itick, &iline,
                   &sectors, &dst, &mode, d_rec, d_vbl) != 10)
            continue;
        if (s_rd_n == cap) { cap *= 2; s_rd = (sched_read_t*)realloc(s_rd, cap * sizeof(*s_rd)); }
        sched_read_t* r = &s_rd[s_rd_n++];
        r->idx = idx; r->issue_rec = irec; r->issue_vbl = ivbl; r->sectors = sectors; r->dst = dst; r->mode = mode;
        r->done = (d_rec[0] != '-');
        r->done_rec = r->done ? (uint32_t)strtoul(d_rec, NULL, 10) : 0u;
        r->done_vbl = r->done ? (uint32_t)strtoul(d_vbl, NULL, 10) : 0u;
    }
    free(buf);
    return 1;
}

static int load_seeds(const char* trace) {
    char* buf = read_sidecar(trace, ".srand", "psxtrace-srand");
    if (!buf) return 0;
    uint32_t cap = 16;
    s_seed = (uint32_t*)malloc(cap * sizeof(*s_seed));
    for (char* line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] == '#' || line[0] == 'p') continue;
        unsigned idx, rec, vbl, seed;
        if (sscanf(line, "%u %u %u %x", &idx, &rec, &vbl, &seed) != 4) continue;
        if (s_seed_n == cap) { cap *= 2; s_seed = (uint32_t*)realloc(s_seed, cap * sizeof(*s_seed)); }
        s_seed[s_seed_n++] = seed;
    }
    free(buf);
    return 1;
}

static uint32_t s_pos = 0;          /* the reference VBLANK count openpete has reached */
static int      s_pos_set = 0;
static int32_t  s_counter_k = 0;    /* VBLANK count minus the libetc counter, constant per capture */
static int      s_counter_k_set = 0;
static const uint32_t* s_stamps = NULL; static uint32_t s_stamps_n = 0;

/* After a waited row the game-visible counter is the value the reference's
 * following query returned; the VBLANK-to-counter offset is re-derived
 * from it, since the interrupt that advances the counter and the vblank
 * event that advances the VBLANK count are not ordered identically at
 * every return. */
void openpete_sched_set_counter(int32_t counter) {
    if (counter >= 0 && s_pos_set) { s_counter_k = (int32_t)s_pos - counter; s_counter_k_set = 1; }
}

void openpete_sched_set_stamps(const uint32_t* stamps, uint32_t count) { s_stamps = stamps; s_stamps_n = count; }
uint32_t openpete_sched_pos(void) { return s_pos; }
void openpete_sched_advance_pos(uint32_t vbls) { s_pos += vbls; }
int openpete_sched_pos_known(void) { return s_pos_set; }


int openpete_sched_open(const char* trace_path) {
    int vs = load_vsync(trace_path);
    int rd = load_reads(trace_path);
    int sd = load_seeds(trace_path);
    if (!vs && !rd && !sd) return 0;
    s_active = 1;
    { const char* lp = getenv("OPENPETE_SCHED_LOG"); if (lp && *lp) s_log = fopen(lp, "w"); }
    /* The position starts at the first event; the game-visible counter is
     * the VBLANK count minus a constant the first counter-carrying row
     * gives (libetc's counter starts at its own init, later than the
     * emulator's). */
    {
        uint32_t first = 0xFFFFFFFFu;
        if (s_vs_n) first = s_vs[0].entry_vbl;
        if (s_rd_n && s_rd[0].issue_vbl < first) first = s_rd[0].issue_vbl;
        if (first != 0xFFFFFFFFu) { s_pos = first; s_pos_set = 1; }
        for (uint32_t i = 0; i < s_vs_n; i++)
            if (s_vs[i].counter >= 0) { s_counter_k = (int32_t)s_vs[i].ret_vbl - s_vs[i].counter; s_counter_k_set = 1; break; }
    }
    OPL_INFO_F("psxtrace", "schedule: %s%u waited VSync rows, %s%u reads, %s%u seeds",
               vs ? "" : "no .vsync: ", s_vs_n, rd ? "" : "no .loads: ", s_rd_n, sd ? "" : "no .srand: ", s_seed_n);
    return 1;
}

/* The reference's position when openpete first meets the schedule: the
 * entry of its first waited VSync (no VBL passes on openpete before it). */
static void pos_init_from(uint32_t vbl) {
    if (!s_pos_set) { s_pos = vbl; s_pos_set = 1; }
}

int openpete_sched_next_vsync(int mode, uint32_t* target_out, int32_t* counter_out) {
    if (!s_active || !s_vs) return 0;
    extern uint32_t openpete_psxtrace_anchor_hits;
    extern uint32_t openpete_psxtrace_anchor_hit_target;
    /* The sidecar's rec column is the records written when the call was
     * made: the record after the hit the call follows. openpete's current
     * record is the same count. */
    const uint32_t cur_rec = (openpete_psxtrace_anchor_hits >= openpete_psxtrace_anchor_hit_target)
                                 ? openpete_psxtrace_anchor_hits - openpete_psxtrace_anchor_hit_target + 1u : 0u;
    if (s_stamps && cur_rec >= s_stamps_n) return 0;   /* past the trace's last record */
    if (s_vs_next >= s_vs_n || s_vs[s_vs_next].rec > cur_rec) {
        /* The reference made no more waited calls before its next anchor
         * hit: its frame ran long enough that the game's wait loop saw the
         * VBLs pass in its own work. openpete's call charges to that hit's
         * stamp and consumes no row. */
        if (s_stamps && cur_rec < s_stamps_n && s_pos_set) {
            if (s_log) fprintf(s_log, "vsync-long rec=%u target=%u pos=%u\n", cur_rec, s_stamps[cur_rec], s_pos);
            *target_out = s_stamps[cur_rec] > s_pos ? s_stamps[cur_rec] : s_pos;
            *counter_out = -1;
            return 1;
        }
        if (s_vs_next >= s_vs_n)
            openpete_sched_fail("VSync(%d) call %u beyond the sidecar's %u waited rows", mode, s_vs_next, s_vs_n);
        return 0;
    }
    const sched_vsync_t* r = &s_vs[s_vs_next++];
    if (s_log) {
        extern uint32_t openpete_psxtrace_anchor_hits;
        fprintf(s_log, "vsync %u ref_rec=%u op_hit=%u entry=%u ret=%u pos=%u\n", s_vs_next - 1, r->rec,
                openpete_psxtrace_anchor_hits, r->entry_vbl, r->ret_vbl, s_pos);
    }
    if (r->mode != mode)
        openpete_sched_fail("VSync row %u: mode %d, openpete called VSync(%d) (record %u)", s_vs_next - 1, r->mode, mode, r->rec);
    pos_init_from(r->entry_vbl);
    s_vsync_consumed++;
    /* VBLs the reference spent in game code after this wait returned (a
     * long frame) are charged by the next wait, whose return covers them:
     * where the interrupt landed inside the tick's work is timing the
     * schedule does not carry, and the game's counters read after it
     * differ for that record either way; charging late keeps the tick's
     * update count the reference's. */
    *target_out = r->ret_vbl;
    *counter_out = r->counter;
    return 1;
}

/* The counter the game's VSync(-1) returns on the reference at the
 * current position; -1 when the sidecar carries no counter. */
int32_t openpete_sched_counter(void) {
    if (!s_active || !s_pos_set || !s_counter_k_set) return -1;
    return (int32_t)s_pos - s_counter_k;
}

int openpete_sched_anchor_target(uint32_t record, uint32_t* target_out) {
    if (!s_active || !s_stamps || record >= s_stamps_n || !s_pos_set) return 0;
    *target_out = s_stamps[record];
    if (s_pos > s_stamps[record])
        openpete_sched_fail("anchor record %u: openpete at reference VBLANK %u, the reference at %u (ahead by %u)",
                            record, s_pos, s_stamps[record], s_pos - s_stamps[record]);
    else if (s_pos < s_stamps[record])
        s_long_frames++;
    return 1;
}

int openpete_sched_read(int sectors, uint32_t dst, uint32_t* issue_vbl_out, uint32_t* done_vbl_out, int* sync_out) {
    if (!s_active || !s_rd) return 0;
    if (s_rd_next >= s_rd_n) {
        openpete_sched_fail("CdRead %u beyond the sidecar's %u reads (%d sectors to 0x%08X)", s_rd_next, s_rd_n, sectors, dst);
        return 0;
    }
    const sched_read_t* r = &s_rd[s_rd_next++];
    if (s_log) {
        extern uint32_t openpete_psxtrace_anchor_hits;
        fprintf(s_log, "read %u ref_rec=%u op_hit=%u issue=%u pos=%u sectors=%u\n", r->idx, r->issue_rec,
                openpete_psxtrace_anchor_hits, r->issue_vbl, s_pos, r->sectors);
    }
    if (r->sectors != (uint32_t)sectors || r->dst != dst) {
        openpete_sched_fail("read %u: sidecar %u sectors to 0x%08X, openpete %d sectors to 0x%08X",
                            r->idx, r->sectors, r->dst, sectors, dst);
        return 0;
    }
    pos_init_from(r->issue_vbl);
    *issue_vbl_out = r->issue_vbl;
    if (r->done) {
        *done_vbl_out = r->done_vbl;
        *sync_out = (r->issue_rec == r->done_rec);
    } else {
        /* No completion callback on this game: the read ended before the
         * reference's next event, the following read's issue or the
         * following waited VSync's entry, whichever came first. */
        uint32_t bound = 0xFFFFFFFFu;
        if (s_rd_next < s_rd_n) bound = s_rd[s_rd_next].issue_vbl;
        if (s_vs && s_vs_next < s_vs_n && s_vs[s_vs_next].entry_vbl < bound) bound = s_vs[s_vs_next].entry_vbl;
        {
            extern uint32_t openpete_psxtrace_anchor_hits;
            extern uint32_t openpete_psxtrace_anchor_hit_target;
            const uint32_t next_rec = (openpete_psxtrace_anchor_hits >= openpete_psxtrace_anchor_hit_target)
                                          ? openpete_psxtrace_anchor_hits - openpete_psxtrace_anchor_hit_target + 1u : 0u;
            if (s_stamps && next_rec < s_stamps_n && s_stamps[next_rec] < bound) bound = s_stamps[next_rec];
        }
        *done_vbl_out = (bound != 0xFFFFFFFFu) ? bound : r->issue_vbl;
        *sync_out = 1;
    }
    if (*sync_out) s_reads_sync++; else s_reads_async++;
    return 1;
}

int openpete_sched_next_srand(uint32_t* seed_out) {
    if (!s_active || !s_seed) return 0;
    if (s_seed_next >= s_seed_n) {
        openpete_sched_fail("srand call %u beyond the sidecar's %u seeds", s_seed_next, s_seed_n);
        return 0;
    }
    *seed_out = s_seed[s_seed_next++];
    return 1;
}

void openpete_sched_summary(void) {
    if (!s_active) return;
    OPL_INFO_F("psxtrace", "schedule: consumed %u/%u waited VSync rows, %u/%u reads (%u sync, %u async), %u/%u seeds, "
               "%u records reached behind the reference's stamp (long frames, charged at the next wait)%s%s",
               s_vsync_consumed, s_vs_n, s_rd_next, s_rd_n, s_reads_sync, s_reads_async,
               s_seed_next, s_seed_n, s_long_frames, s_failed ? "; FAILED: " : "", s_failed ? s_fail_reason : "");
}
