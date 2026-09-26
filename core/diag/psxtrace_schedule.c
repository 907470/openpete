/* core/diag/psxtrace_schedule.c — the reference's timing, replayed.
 *
 * A golden captured with the DuckStation fork's recorders carries sidecars
 * beside the trace: `<trace>.vsync` (every libetc VSync call: a waited
 * call with the VBLANK count at entry and return, a query with the
 * counter it returned, runs of equal queries as one row with a count),
 * `<trace>.loads` (every CdRead with its issue and completion VBLANK)
 * and `<trace>.srand` (every srand seed). When the trace is verified
 * they become openpete's schedule: each waited VSync charges the VBLs
 * the reference spent since the previous one, a synchronous read charges
 * its VBLs inside the read, an asynchronous read completes after the
 * reference's delay, each query answers the reference's value for that
 * read of the record (charging the VBLs whose interrupts the reference
 * had seen by then), and srand receives the reference's seed. The thunk
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

/* One libetc VSync call of the reference, in call order: a waited call
 * (mode >= 0) with the VBLANK count at entry and return, a run of n equal
 * queries (mode -1) from one call site with the counter they returned, or
 * a callback registration (mode -2) with its VBLANK count. ra is the
 * caller's return address (0 in a sidecar without the column). */
typedef struct { uint32_t rec; int32_t mode; uint32_t entry_vbl, ret_vbl; int32_t counter; uint32_t n, ra; uint64_t entry_tick; } sched_vsync_t;
typedef struct { uint32_t idx, issue_rec, issue_vbl, sectors, dst, mode, done_rec, done_vbl; int done; uint64_t done_tick; } sched_read_t;

static sched_vsync_t* s_vs = NULL;   static uint32_t s_vs_n = 0, s_vs_next = 0, s_vs_left = 0;
static uint32_t s_vs_waits = 0;       /* waited rows the sidecar holds */
static uint32_t s_reg_total = 0, s_reg_charged = 0, s_reg_matched = 0;   /* registration rows */
static uint64_t s_q_total = 0;        /* queries the sidecar holds (runs expanded) */
static sched_read_t*  s_rd = NULL;   static uint32_t s_rd_n = 0, s_rd_next = 0;
static uint32_t*      s_seed = NULL; static uint32_t s_seed_n = 0, s_seed_next = 0;
static int      s_active = 0;
static FILE*    s_log = NULL;        /* OPENPETE_SCHED_LOG=path: every consumed row */
static uint32_t s_query_logged = 0;
static int      s_failed = 0;
static char     s_fail_reason[256];
static uint32_t s_reads_sync = 0, s_reads_async = 0, s_vsync_consumed = 0, s_long_frames = 0, s_boot_waits = 0;
static int64_t  s_boot_offset = 0;   /* stamp[0] minus the position the boot reached */
static int32_t  s_counter_k = 0;    /* VBLANK count minus the libetc counter, constant per capture */
static int      s_counter_k_set = 0;
static uint64_t s_q_answered = 0;     /* queries answered from the sidecar */
static uint64_t s_q_extra = 0;        /* openpete's queries past the record's rows: answered pos - K */
static uint64_t s_q_unmade = 0;       /* the reference's queries openpete never made (rows dropped) */
static uint32_t s_q_charged = 0;      /* VBLs charged at a query (the interrupt landed inside the tick's work) */
static uint32_t s_q_behind = 0;       /* query rows of the record behind the position (dropped) */

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
    uint32_t cap = 4096;
    s_vs = (sched_vsync_t*)malloc(cap * sizeof(*s_vs));
    for (char* line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] == '#' || line[0] == 'p') continue;
        unsigned idx, rec, evbl, rvbl; int mode; unsigned long long et, rt; unsigned v0, n = 1, ra = 0;
        if (sscanf(line, "%u %u %d %u %u %llu %llu %u %u %x", &idx, &rec, &mode, &evbl, &rvbl, &et, &rt, &v0, &n, &ra) < 8)
            continue;
        if (s_vs_n == cap) { cap *= 2; s_vs = (sched_vsync_t*)realloc(s_vs, cap * sizeof(*s_vs)); }
        sched_vsync_t* r = &s_vs[s_vs_n++];
        r->rec = rec; r->mode = mode; r->entry_vbl = evbl; r->ret_vbl = rvbl;
        r->counter = (mode == -1) ? (int32_t)v0 : -1; r->n = (mode == -1 && n) ? n : 1u; r->ra = ra;
        r->entry_tick = et;
        if (mode == -1) {
            if (!s_counter_k_set) { s_counter_k = (int32_t)rvbl - (int32_t)v0; s_counter_k_set = 1; }
            s_q_total += r->n;
        } else if (mode == -2) {
            s_reg_total++;
        } else {
            s_vs_waits++;
        }
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
        char d_rec[32], d_vbl[32], d_tick[32] = "-";
        if (sscanf(line, "%u %u %u %llu %u %u %x %x %31s %31s %31s", &idx, &irec, &ivbl, &itick, &iline,
                   &sectors, &dst, &mode, d_rec, d_vbl, d_tick) < 10)
            continue;
        if (s_rd_n == cap) { cap *= 2; s_rd = (sched_read_t*)realloc(s_rd, cap * sizeof(*s_rd)); }
        sched_read_t* r = &s_rd[s_rd_n++];
        r->idx = idx; r->issue_rec = irec; r->issue_vbl = ivbl; r->sectors = sectors; r->dst = dst; r->mode = mode;
        r->done = (d_rec[0] != '-');
        r->done_rec = r->done ? (uint32_t)strtoul(d_rec, NULL, 10) : 0u;
        r->done_vbl = r->done ? (uint32_t)strtoul(d_vbl, NULL, 10) : 0u;
        r->done_tick = (r->done && d_tick[0] != '-') ? (uint64_t)strtoull(d_tick, NULL, 10) : 0u;
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
static const uint32_t* s_stamps = NULL; static uint32_t s_stamps_n = 0;

void openpete_sched_set_stamps(const uint32_t* stamps, uint32_t count) { s_stamps = stamps; s_stamps_n = count; }
uint32_t openpete_sched_pos(void) { return s_pos; }
void openpete_sched_advance_pos(uint32_t vbls) { s_pos += vbls; }
int openpete_sched_pos_known(void) { return s_pos_set; }


/* The load windows the reference's watched word describes: record indexes
 * the loader was active from and to (the first record hashed with the
 * value that entered the window, the first hashed with the value that
 * left it), by the game's predicate. */
typedef struct { uint32_t enter_rec, leave_rec; } sched_window_t;
static sched_window_t* s_win = NULL; static uint32_t s_win_n = 0, s_win_next = 0;
static uint32_t sched_cur_rec(void);
static int64_t  s_rec_offset = 0;     /* reference record index minus openpete's hit-derived one */
static int      s_in_load = 0, s_in_load_known = 0;
static uint32_t s_records_skipped = 0, s_windows_seen = 0, s_load_waits = 0;
extern int psyq_game_load_window_active(uint32_t value) __attribute__((weak));

static int load_watch(const char* trace) {
    char* buf = read_sidecar(trace, ".watch", "psxtrace-watch");
    if (!buf) return 0;
    if (!psyq_game_load_window_active) { free(buf); return 0; }
    uint32_t cap = 64;
    s_win = (sched_window_t*)malloc(cap * sizeof(*s_win));
    /* Each store's value is the next store's "old" (the breakpoint fires
     * before the store lands). */
    unsigned prev_rec = 0; int have_prev = 0; int active = 0;
    for (char* line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] == '#' || line[0] == 'p') continue;
        unsigned idx, rec, vbl, pc, oldv, newv;
        if (sscanf(line, "%u %u %u %x %x %x", &idx, &rec, &vbl, &pc, &oldv, &newv) != 6) continue;
        if (have_prev) {
            int a = psyq_game_load_window_active(oldv);   /* value the previous store set */
            if (a && !active) {
                if (s_win_n == cap) { cap *= 2; s_win = (sched_window_t*)realloc(s_win, cap * sizeof(*s_win)); }
                s_win[s_win_n].enter_rec = prev_rec; s_win[s_win_n].leave_rec = 0xFFFFFFFFu;
            } else if (!a && active && s_win_n < cap) {
                s_win[s_win_n].leave_rec = prev_rec; s_win_n++;
            }
            active = a;
        }
        prev_rec = rec; have_prev = 1;
    }
    free(buf);
    return 1;
}

/* Two bitmaps over the reference's VBLANKs. `s_sio0`: the vblanks the
 * kernel's controller/card handler ran on, from its store to its port-flip
 * word (.sio0); a vblank without one is a vblank the handler did not run
 * (its interrupt was taken by an earlier handler) or the card driver's
 * abort path, which returns before the flip. `s_padcmd`: the vblanks the
 * recorder saw the controller accept a command on (.padcmd), the vsyncs
 * libpad's command sequence advanced on; a served vblank without one is
 * an exchange libpad abandoned. */
typedef struct { uint8_t* bits; uint32_t n, first, served, skipped; } vbl_set_t;
static vbl_set_t s_sio0, s_padcmd;

static void vbl_set_add(vbl_set_t* v, uint32_t vbl) {
    if (vbl >= v->n) {
        uint32_t n = v->n ? v->n : 4096u;
        while (n <= vbl) n *= 2u;
        v->bits = (uint8_t*)realloc(v->bits, n / 8u + 1u);
        memset(v->bits + (v->n / 8u), 0, (n - v->n) / 8u + 1u);
        v->n = n;
    }
    v->bits[vbl / 8u] |= (uint8_t)(1u << (vbl % 8u));
    if (!v->first || vbl < v->first) v->first = vbl;
}
/** @brief Return 1 when the set holds vbl, counting the answer as served or skipped.
 *
 * Outside the covered range, or with the schedule inactive or the set
 * empty, known is set to 0 and the answer is 1 without counting. known may
 * be NULL. */
static int vbl_set_has(vbl_set_t* v, uint32_t vbl, int* known) {
    if (!s_active || !v->bits || vbl < v->first || vbl >= v->n) { if (known) *known = 0; return 1; }
    if (known) *known = 1;
    int has = (v->bits[vbl / 8u] >> (vbl % 8u)) & 1;
    if (has) v->served++; else v->skipped++;
    return has;
}

static int load_padcmd(const char* trace) {
    char* buf = read_sidecar(trace, ".padcmd", "psxtrace-padcmd");
    if (!buf) return 0;
    uint32_t max = 0;
    for (char* line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] == '#' || line[0] == 'p') continue;
        unsigned idx, rec, vbl, port, cmd;
        if (sscanf(line, "%u %u %u %u %x", &idx, &rec, &vbl, &port, &cmd) != 5) continue;
        vbl_set_add(&s_padcmd, vbl);
        if (vbl > max) max = vbl;
    }
    free(buf);
    if (max + 1u < s_padcmd.n) s_padcmd.n = max + 1u;
    return s_padcmd.n != 0;
}

static int load_sio0(const char* trace) {
    char* buf = read_sidecar(trace, ".sio0", "psxtrace-sio0");
    if (!buf) return 0;
    uint32_t max = 0;
    for (char* line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] == '#' || line[0] == 'p') continue;
        unsigned idx, rec, vbl, n;
        if (sscanf(line, "%u %u %u %u", &idx, &rec, &vbl, &n) != 4) continue;
        for (unsigned i = 0; i < n; i++) vbl_set_add(&s_sio0, vbl + i);
        if (vbl + n - 1u > max) max = vbl + n - 1u;
    }
    free(buf);
    if (max + 1u < s_sio0.n) s_sio0.n = max + 1u;
    return s_sio0.n != 0;
}

/* The reference's CdSync polls: one row per run of calls with the same
 * record, VBLANK, mode, value and call site. A poll of openpete's takes
 * the value of the next row of its record with its mode and site, and a
 * row whose VBLANK is ahead of the position charges the VBLs up to it (a
 * game spinning on the drive spent them in the spin). Rows of earlier
 * records are polls openpete's tick did not make. */
typedef struct { uint32_t rec, vbl, ra, n; int32_t mode, ret; uint64_t tick; } sched_cdsync_t;
/* The reference's clock (CPU cycles) at the row the last waited VSync or
 * CdSync poll was answered from; 0 when the answer came from no row. An
 * event the reference recorded with a clock value (a read's completion
 * callback) precedes that call when its value is lower. */
static uint64_t s_last_tick = 0, s_read_done_tick = 0;
uint64_t openpete_sched_last_tick(void) { return s_last_tick; }
uint64_t openpete_sched_read_done_tick(void) { return s_read_done_tick; }

static sched_cdsync_t* s_cs = NULL; static uint32_t s_cs_n = 0, s_cs_next = 0, s_cs_left = 0;
static uint32_t s_cs_answered = 0, s_cs_unmade = 0, s_cs_charged = 0;

static int load_cdsync(const char* trace) {
    char* buf = read_sidecar(trace, ".cdsync", "psxtrace-cdsync");
    if (!buf) return 0;
    uint32_t cap = 1024;
    s_cs = (sched_cdsync_t*)malloc(cap * sizeof(*s_cs));
    for (char* line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] == '#' || line[0] == 'p') continue;
        unsigned idx, rec, vbl, n, ra; int mode, ret; unsigned long long tick;
        if (sscanf(line, "%u %u %u %d %d %u %x %llu", &idx, &rec, &vbl, &mode, &ret, &n, &ra, &tick) != 8) continue;
        if (s_cs_n == cap) { cap *= 2; s_cs = (sched_cdsync_t*)realloc(s_cs, cap * sizeof(*s_cs)); }
        s_cs[s_cs_n].rec = rec; s_cs[s_cs_n].vbl = vbl; s_cs[s_cs_n].ra = ra; s_cs[s_cs_n].n = n;
        s_cs[s_cs_n].mode = mode; s_cs[s_cs_n].ret = ret; s_cs[s_cs_n].tick = tick; s_cs_n++;
    }
    free(buf);
    return s_cs_n != 0;
}

int openpete_sched_cdsync(int mode, uint32_t ra, int32_t* ret_out, uint32_t* target_out) {
    s_last_tick = 0;
    if (!s_active || !s_cs || s_failed) return 0;
    const uint32_t cur_rec = sched_cur_rec();
    while (s_cs_next < s_cs_n && s_cs[s_cs_next].rec < cur_rec) { s_cs_unmade += s_cs_left ? s_cs_left : s_cs[s_cs_next].n; s_cs_next++; s_cs_left = 0; }
    uint32_t k = s_cs_next; uint32_t left = s_cs_left;
    while (k < s_cs_n && s_cs[k].rec == cur_rec && (s_cs[k].mode != mode || s_cs[k].ra != ra)) { k++; left = 0; }
    if (k >= s_cs_n || s_cs[k].rec != cur_rec) return 0;
    if (k != s_cs_next) { s_cs_unmade += s_cs_left ? s_cs_left : 0; s_cs_next = k; s_cs_left = 0; }
    if (!s_cs_left) s_cs_left = s_cs[k].n;
    *ret_out = s_cs[k].ret;
    *target_out = s_cs[k].vbl;
    s_last_tick = s_cs[k].tick;
    if (s_cs[k].vbl > s_pos) s_cs_charged += s_cs[k].vbl - s_pos;
    s_cs_answered++;
    if (--s_cs_left == 0) s_cs_next = k + 1;
    return 1;
}

/* Scans from the row cursor without moving it. */
int openpete_sched_cdsync_peek(uint32_t* ra_out, uint64_t* tick_out) {
    if (!s_active || !s_cs || s_failed) return 0;
    const uint32_t cur_rec = sched_cur_rec();
    uint32_t k = s_cs_next;
    while (k < s_cs_n && s_cs[k].rec < cur_rec) k++;
    if (k >= s_cs_n || s_cs[k].rec != cur_rec) return 0;
    *ra_out = s_cs[k].ra; *tick_out = s_cs[k].tick;
    return 1;
}

int openpete_sched_sio0_served(uint32_t vbl, int* known) { return vbl_set_has(&s_sio0, vbl, known); }
/* Before the first recorded command the game had not started polling the
 * controller, so the sidecar says nothing about those vblanks. */
int openpete_sched_padcmd_at(uint32_t vbl, int* known) { return vbl_set_has(&s_padcmd, vbl, known); }

int openpete_sched_open(const char* trace_path) {
    int vs = load_vsync(trace_path);
    int rd = load_reads(trace_path);
    int sd = load_seeds(trace_path);
    int wt = load_watch(trace_path);
    int pc = load_padcmd(trace_path);
    int so = load_sio0(trace_path);
    int cs = load_cdsync(trace_path);
    if (!vs && !rd && !sd) return 0;
    s_active = 1;
    if (wt) OPL_INFO_F("psxtrace", "schedule: %u load windows from the watched word", s_win_n);
    if (pc) OPL_INFO_F("psxtrace", "schedule: controller commands known from vblank %u to %u", s_padcmd.first, s_padcmd.n - 1u);
    if (so) OPL_INFO_F("psxtrace", "schedule: controller handler vblanks known from %u to %u", s_sio0.first, s_sio0.n - 1u);
    if (cs) OPL_INFO_F("psxtrace", "schedule: %u CdSync poll rows", s_cs_n);
    { const char* lp = getenv("OPENPETE_SCHED_LOG"); if (lp && *lp) { s_log = fopen(lp, "w"); if (s_log) setvbuf(s_log, NULL, _IOLBF, 0); } }
    /* The position starts at the first event; the game-visible counter is
     * the VBLANK count minus a constant the first query row gives
     * (libetc's counter starts at its own init, later than the
     * emulator's). */
    {
        uint32_t first = 0xFFFFFFFFu;
        if (s_vs_n) first = s_vs[0].entry_vbl;
        if (s_rd_n && s_rd[0].issue_vbl < first) first = s_rd[0].issue_vbl;
        if (first != 0xFFFFFFFFu) { s_pos = first; s_pos_set = 1; }
    }
    OPL_INFO_F("psxtrace", "schedule: %s%u waited VSync rows and %llu queries, %s%u reads, %s%u seeds",
               vs ? "" : "no .vsync: ", s_vs_waits, (unsigned long long)s_q_total, rd ? "" : "no .loads: ", s_rd_n, sd ? "" : "no .srand: ", s_seed_n);
    return 1;
}

/** @brief Set the position to vbl when it is not yet known.
 *
 * The first consumed waited VSync supplies its entry VBLANK and the first
 * consumed read its issue VBLANK; no VBL passes on openpete before the
 * schedule is first met. */
static void pos_init_from(uint32_t vbl) {
    if (!s_pos_set) { s_pos = vbl; s_pos_set = 1; }
}

/* After a schedule failure this returns 0 and the wait runs under openpete's own count. */
int openpete_sched_next_vsync(int mode, uint32_t* target_out) {
    if (!s_active || !s_vs || s_failed) return 0;
    s_last_tick = 0;
    extern uint32_t openpete_psxtrace_anchor_hits;
    extern uint32_t openpete_psxtrace_anchor_hit_target;
    /* The sidecar's rec column is the records written when the call was
     * made: the record after the hit the call follows. openpete's current
     * record is the same count. */
    const uint32_t cur_rec = sched_cur_rec();
    if (s_stamps && cur_rec >= s_stamps_n) return 0;   /* past the trace's last record */
    if (s_in_load) {
        /* Inside a load window the records do not correspond and neither do
         * the waits: each charges its own count; the window's end sets the
         * position to the reference's. */
        if (!s_pos_set) return 0;
        *target_out = s_pos + (uint32_t)((mode == 0) ? 1 : mode);
        s_load_waits++;
        if (s_log) fprintf(s_log, "load-wait op_hit=%u mode=%d pos=%u\n", openpete_psxtrace_anchor_hits, mode, s_pos);
        return 1;
    }
    if (cur_rec == 0 && getenv("OPENPETE_SCHED_BOOT_OWN")) {
        /* Before the first anchor hit the boot's call sequence is not the
         * reference's on every game (a card probe, a splash the game
         * times by rand); the rows of record 0 are skipped and a wait
         * charges its own count, the position advancing with it. The
         * first anchor's stamp reconciles the position. */
        while (s_vs_next < s_vs_n && s_vs[s_vs_next].rec == 0) { s_vs_next++; s_vs_left = 0; }
        if (!s_pos_set) return 0;
        *target_out = s_pos + (uint32_t)((mode == 0) ? 1 : mode);
        s_boot_waits++;
        if (s_log && s_boot_waits <= 2000) fprintf(s_log, "boot-wait %u mode=%d pos=%u\n", s_boot_waits, mode, s_pos);
        return 1;
    }
    /* The next waited row of this record or an earlier one; the query
     * rows before it are reads the reference made before waiting that
     * openpete did not (its tick's work spent no VBL). */
    uint32_t w = s_vs_next;
    while (w < s_vs_n && s_vs[w].rec <= cur_rec && s_vs[w].mode < 0) w++;
    if (w >= s_vs_n || s_vs[w].rec > cur_rec) {
        /* The reference made no more waited calls before its next anchor
         * hit: its frame ran long enough that the game's wait loop saw the
         * VBLs pass in its own work. openpete's call charges to that hit's
         * stamp and consumes no row. */
        if (s_stamps && cur_rec < s_stamps_n && s_pos_set) {
            if (s_log) fprintf(s_log, "vsync-long rec=%u target=%u pos=%u\n", cur_rec, s_stamps[cur_rec], s_pos);
            if (s_stamps[cur_rec] <= s_pos) {
                /* Nothing left to charge: the reference's tick reached
                 * its next hit without this wait. A wait charging nothing
                 * would spin the game's wait loop forever, so the schedule
                 * ends here and the wait runs under openpete's count. */
                openpete_sched_fail("VSync(%d) at record %u: the reference made no wait here (position %u at its stamp)",
                                    mode, cur_rec, s_pos);
                return 0;
            }
            *target_out = s_stamps[cur_rec];
            return 1;
        }
        if (w >= s_vs_n)
            openpete_sched_fail("VSync(%d) call beyond the sidecar's %u waited rows", mode, s_vs_waits);
        return 0;
    }
    for (; s_vs_next < w; s_vs_next++) if (s_vs[s_vs_next].mode == -1) s_q_unmade += s_vs_left ? s_vs_left : s_vs[s_vs_next].n;
    s_vs_left = 0;
    const sched_vsync_t* r = &s_vs[s_vs_next++];
    if (s_log) {
        extern uint32_t openpete_psxtrace_anchor_hits;
        fprintf(s_log, "vsync %u ref_rec=%u op_hit=%u entry=%u ret=%u pos=%u\n", s_vs_next - 1, r->rec,
                openpete_psxtrace_anchor_hits, r->entry_vbl, r->ret_vbl, s_pos);
    }
    if (r->mode != mode)
        openpete_sched_fail("VSync row %u: mode %d, openpete called VSync(%d) (record %u)", s_vs_next - 1, r->mode, mode, r->rec);
    pos_init_from(r->entry_vbl);
    s_last_tick = r->entry_tick;
    s_vsync_consumed++;
    /* VBLs the reference spent in game code after this wait returned (a
     * long frame) are charged at the next anchor hit: the tick's work
     * read its counters before the interrupts landed. */
    *target_out = r->ret_vbl;
    return 1;
}

/* The reference record index the next anchor hit produces: openpete's
 * hit count plus the offset the load-window re-alignments accumulated. */
uint32_t openpete_sched_current_record(void) { return sched_cur_rec(); }

static uint32_t sched_cur_rec(void) {
    extern uint32_t openpete_psxtrace_anchor_hits;
    extern uint32_t openpete_psxtrace_anchor_hit_target;
    int64_t r = (openpete_psxtrace_anchor_hits >= openpete_psxtrace_anchor_hit_target)
                    ? (int64_t)openpete_psxtrace_anchor_hits - openpete_psxtrace_anchor_hit_target + 1 : 0;
    r += s_rec_offset;
    return r < 0 ? 0u : (uint32_t)r;
}

/* Leaving a window re-aligns the record index with the reference's window end. */
int openpete_sched_anchor_window(uint32_t record, uint32_t value, uint32_t* skip_to_rec) {
    if (!s_active || s_failed) return 0;
    int a = (s_win && psyq_game_load_window_active) ? psyq_game_load_window_active(value) : 0;
    int was = s_in_load_known ? s_in_load : 0;
    if (s_log && (a || was || getenv("OPENPETE_SCHED_LOG_REC"))) fprintf(s_log, "anchor record=%u word=0x%X pos=%u\n", record, value, s_pos);
    if (!s_win || !psyq_game_load_window_active) return 0;
    s_in_load = a; s_in_load_known = 1;
    if (a && !was) s_windows_seen++;
    if (!a && was) {
        if (s_win_next >= s_win_n) {
            openpete_sched_fail("load window %u ended at record %u; the reference's watched word saw %u windows",
                                s_windows_seen, record, s_win_n);
            return 0;
        }
        const sched_window_t* w = &s_win[s_win_next++];
        if (w->leave_rec == 0xFFFFFFFFu) return 0;  /* the reference's window ran to the trace's end */
        if (w->leave_rec < record) {
            openpete_sched_fail("load window %u: openpete left it at record %u, the reference at %u (openpete's loader took longer)",
                                s_windows_seen, record, w->leave_rec);
            return 0;
        }
        if (w->leave_rec > record) {
            s_records_skipped += w->leave_rec - record;
            s_rec_offset += (int64_t)(w->leave_rec - record);
            /* Rows inside the reference's window openpete never made. */
            while (s_vs && s_vs_next < s_vs_n && s_vs[s_vs_next].rec <= w->leave_rec) s_vs_next++;
            s_vs_left = 0;
            if (s_log) fprintf(s_log, "window %u: openpete record %u -> reference record %u (%u skipped)\n",
                               s_windows_seen, record, w->leave_rec, w->leave_rec - record);
            *skip_to_rec = w->leave_rec;
            return 1;
        }
    }
    return 0;
}

int openpete_sched_in_load(void) { return s_in_load; }

/* A callback registration: the next registration row of this record (or
 * an earlier one) gives the VBLANK the reference registered at; the VBLs
 * its tick's work spent before it are charged there, so the callback
 * does not run for them. Rows before it that are reads or waits stay.
 * Returns 1 with the target when the position lies behind it. */
int openpete_sched_registration(uint32_t ra, uint32_t* target_out) {
    if (!s_active || !s_vs || s_failed || !s_pos_set) return 0;
    const uint32_t cur_rec = sched_cur_rec();
    if (s_in_load || (s_stamps && cur_rec >= s_stamps_n)) return 0;
    uint32_t k = s_vs_next;
    while (k < s_vs_n && s_vs[k].rec <= cur_rec && s_vs[k].mode != -2) k++;
    if (k >= s_vs_n || s_vs[k].rec > cur_rec) return 0;
    if (s_vs[k].ra != 0 && s_vs[k].ra != ra) {
        if (s_log) fprintf(s_log, "registration ra=0x%08X: the reference's next is from 0x%08X (record %u)\n", ra, s_vs[k].ra, s_vs[k].rec);
        return 0;
    }
    /* The row is consumed in place (rows before it stay for their own
     * consumers): its mode becomes a no-op marker. */
    s_vs[k].mode = -3;
    s_reg_matched++;
    if (s_log) fprintf(s_log, "registration ra=0x%08X rec=%u vbl=%u pos=%u\n", ra, s_vs[k].rec, s_vs[k].entry_vbl, s_pos);
    if (s_vs[k].entry_vbl > s_pos) { s_reg_charged += s_vs[k].entry_vbl - s_pos; *target_out = s_vs[k].entry_vbl; return 1; }
    return 0;
}

int32_t openpete_sched_counter(void) {
    if (!s_active || !s_pos_set || !s_counter_k_set) return -1;
    return (int32_t)s_pos - s_counter_k;
}

static uint32_t s_query_ra = 0;
static void query_log(const char* how, int32_t counter, uint32_t target) {
    extern uint32_t openpete_psxtrace_anchor_hits;
    static long lo = -1, hi = -1;
    if (lo < 0) { const char* w = getenv("OPENPETE_SCHED_LOG_REC"); lo = 0; hi = 0; if (w) sscanf(w, "%ld:%ld", &lo, &hi); }
    if ((s_query_logged < 400) || ((long)openpete_psxtrace_anchor_hits >= lo && (long)openpete_psxtrace_anchor_hits < hi)) {
        s_query_logged++;
        fprintf(s_log, "query %s hit=%u pos=%u counter=%d target=%u ra=0x%08X\n", how, openpete_psxtrace_anchor_hits, s_pos, counter, target, s_query_ra);
    }
}

/* The counter the game's VSync(-1) returns: the reference's answer to the
 * same read of the record, whose VBLANK (counter + K) the position is
 * charged to when it lies ahead (the interrupt landed inside the tick's
 * work before this read). Before the first anchor, inside a load window,
 * or past the record's rows the answer is the position's. -1 when the
 * sidecar carries no counter. */
int32_t openpete_sched_query(uint32_t ra, uint32_t* target_out) {
    if (!s_active || !s_pos_set || !s_counter_k_set) return -1;
    const uint32_t cur_rec = sched_cur_rec();
    int32_t counter = (int32_t)s_pos - s_counter_k;
    *target_out = s_pos;
    s_query_ra = ra;
    const int replay = s_vs && !(cur_rec == 0 && getenv("OPENPETE_SCHED_BOOT_OWN")) && !s_in_load && !s_failed && !(s_stamps && cur_rec >= s_stamps_n);
    if (replay) {
        /* The next query row of this record from the same call site answers
         * the read. Query rows before it are reads openpete never made: an
         * earlier record's, rows behind the position (the reference's reads
         * during a span a read charged at once), the library's own polls
         * (thunked on the host) and a poll loop's extra iterations. A waited
         * row stops the search: the reference waited before its next read
         * from this site. A sidecar without call sites (ra 0) matches any
         * site. */
        uint32_t k = s_vs_next;
        while (k < s_vs_n && s_vs[k].rec <= cur_rec && s_vs[k].mode < 0 &&
               (s_vs[k].mode == -2 || s_vs[k].rec < cur_rec || (uint32_t)(s_vs[k].counter + s_counter_k) < s_pos ||
                (s_vs[k].ra != 0 && s_vs[k].ra != ra)))
            k++;
        if (k < s_vs_n && s_vs[k].rec == cur_rec && s_vs[k].mode == -1) {
            for (; s_vs_next < k; s_vs_next++) {
                if (s_vs[s_vs_next].mode != -1) { s_vs_left = 0; continue; }
                s_q_unmade += s_vs_left ? s_vs_left : s_vs[s_vs_next].n;
                if (s_vs[s_vs_next].rec == cur_rec) s_q_behind++;
                s_vs_left = 0;
            }
            if (!s_vs_left) s_vs_left = s_vs[k].n;
            counter = s_vs[k].counter;
            uint32_t target = (uint32_t)(counter + s_counter_k);
            if (target > s_pos) { s_q_charged += target - s_pos; *target_out = target; }
            s_q_answered++;
            if (--s_vs_left == 0) s_vs_next++;
            if (s_log) query_log("row", counter, *target_out);
            return counter;
        }
        /* The reference waited before its next read, or made no more reads
         * this record: the position's value. */
        s_q_extra++;
    }
    if (s_log) query_log(replay ? "extra" : "pos", counter, *target_out);
    return counter;
}

int openpete_sched_anchor_target(uint32_t record, uint32_t* target_out) {
    if (!s_active || !s_stamps || record >= s_stamps_n || !s_pos_set || s_failed) return 0;
    *target_out = s_stamps[record];
    if (record == 0) {
        /* The boot ran under openpete's own counting; from here the
         * position is the reference's. */
        if (s_log) fprintf(s_log, "anchor 0: boot ended at pos %u, stamp %u (%u boot waits)\n", s_pos, s_stamps[0], s_boot_waits);
        s_boot_offset = (int64_t)s_stamps[0] - (int64_t)s_pos;
        s_pos = s_stamps[0];
        return 1;
    }
    if (s_pos > s_stamps[record] && !s_in_load)
        openpete_sched_fail("anchor record %u: openpete at reference VBLANK %u, the reference at %u (ahead by %u)",
                            record, s_pos, s_stamps[record], s_pos - s_stamps[record]);
    else if (s_pos < s_stamps[record])
        s_long_frames++;
    return 1;
}

int openpete_sched_read(int sectors, uint32_t dst, uint32_t* issue_vbl_out, uint32_t* done_vbl_out, uint32_t* done_rec_out, int* sync_out) {
    if (!s_active || !s_rd || s_failed) return 0;
    if (s_rd_next >= s_rd_n) {
        openpete_sched_fail("CdRead %u beyond the sidecar's %u reads (%d sectors to 0x%08X)", s_rd_next, s_rd_n, sectors, dst);
        return 0;
    }
    const sched_read_t* r = &s_rd[s_rd_next++];
    s_read_done_tick = r->done ? r->done_tick : 0u;
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
    /* The reference's next event after this read: the following read's
     * issue, the following waited VSync's entry or the next anchor stamp,
     * whichever comes first. A read the game spun on ends by it, and the
     * VBLs between a completion and it are the tick's own work after the
     * read (a loader's parse), spent before the game samples its counters
     * again. */
    uint32_t bound = 0xFFFFFFFFu;
    if (s_rd_next < s_rd_n) bound = s_rd[s_rd_next].issue_vbl;
    /* The first waited call entered at or after the issue, wherever the
     * row pointer stands (the boot's rows are skipped, not consumed). */
    for (uint32_t i = 0; s_vs && i < s_vs_n; i++) {
        if (s_vs[i].mode < 0 || s_vs[i].entry_vbl < r->issue_vbl) continue;
        if (s_vs[i].entry_vbl < bound) bound = s_vs[i].entry_vbl;
        break;
    }
    {
        extern uint32_t openpete_psxtrace_anchor_hits;
        extern uint32_t openpete_psxtrace_anchor_hit_target;
        const uint32_t next_rec = sched_cur_rec();
        if (s_stamps && next_rec < s_stamps_n && s_stamps[next_rec] < bound) bound = s_stamps[next_rec];
    }
    if (r->done) {
        /* The game waited on VSync while this read was in flight when a
         * waited row lies inside it: then the waits carry the VBLs and the
         * completion is deferred to its VBLANK. A read the game spun on
         * without waiting is charged inside the read (a deferral would
         * never complete), through to the next event. */
        int waited_inside = 0;
        for (uint32_t i = 0; s_vs && i < s_vs_n; i++) {
            if (s_vs[i].mode < 0) continue;
            if (s_vs[i].entry_vbl >= r->done_vbl) break;
            if (s_vs[i].entry_vbl >= r->issue_vbl) { waited_inside = 1; break; }
        }
        *sync_out = !waited_inside;
        *done_vbl_out = (*sync_out && bound != 0xFFFFFFFFu && bound > r->done_vbl) ? bound : r->done_vbl;
        *done_rec_out = r->done_rec;
    } else {
        /* No completion callback on this game: the read ended by the next event. */
        *done_vbl_out = (bound != 0xFFFFFFFFu) ? bound : r->issue_vbl;
        *done_rec_out = r->issue_rec;
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
               "queries: %llu/%llu answered from the reference's rows, %llu past the record's rows, %llu of the reference's unmade, "
               "%u VBL(s) charged at a query, %u rows behind the position dropped; "
               "registrations: %u/%u matched, %u VBL(s) charged; "
               "%u records reached behind the reference's stamp (long frames, charged at the anchor), "
               "boot: %u waits under openpete's count, %lld VBL(s) from the first stamp, "
               "%u load windows (%u/%u on the reference), %u reference records skipped at their ends; "
               "controller handler: %u vblanks served, %u skipped, commands on %u, none on %u; "
               "CdSync polls: %u answered from the reference's rows, %u of the reference's unmade, %u VBL(s) charged%s%s",
               s_vsync_consumed, s_vs_waits, s_rd_next, s_rd_n, s_reads_sync, s_reads_async,
               s_seed_next, s_seed_n,
               (unsigned long long)s_q_answered, (unsigned long long)s_q_total, (unsigned long long)s_q_extra,
               (unsigned long long)s_q_unmade, s_q_charged, s_q_behind,
               s_reg_matched, s_reg_total, s_reg_charged,
               s_long_frames, s_boot_waits, (long long)s_boot_offset,
               s_windows_seen, s_win_next, s_win_n, s_records_skipped,
               s_sio0.served, s_sio0.skipped, s_padcmd.served, s_padcmd.skipped,
               s_cs_answered, s_cs_unmade, s_cs_charged,
               s_failed ? "; FAILED: " : "", s_failed ? s_fail_reason : "");
}
