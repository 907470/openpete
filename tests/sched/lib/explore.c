/* explore.c - the explorer: forks one child per schedule and enumerates
 * choice strings.
 *
 * DFS mode is stateless search over choice prefixes. A child runs its
 * prefix, then takes the default at every later choice and reports each
 * such choice's alternatives with their preemption and deviation costs.
 * Every alternative at a reported position becomes a new prefix (the
 * prefix, the defaults up to that position, the alternative), so each
 * complete choice string is run exactly once. The bound pairs (p, d) are
 * visited in order of p + d, then p; a prefix whose cost exceeds the pair
 * under exploration but fits the configured limits is deferred to the
 * first later pair that covers it, so no schedule runs twice and a failure
 * is found at the smallest pair that covers its cost. */
#define _GNU_SOURCE
#include "op_sched_internal.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/** A choice prefix waiting to run, with its accumulated cost. */
typedef struct prefix {
    uint16_t *ch;
    int len;
    int p, d;
} prefix;

/** A growable stack of prefixes. */
typedef struct pstack { prefix *v; long n, cap; } pstack;

/** A running child and the report it has sent so far. */
typedef struct child {
    pid_t pid;
    int fd;
    prefix pre;
    uint8_t *buf;
    size_t len, cap;
} child;

/** @brief Push a prefix onto a stack, growing it; exits on allocation failure.
 * @param st The stack.
 * @param p The prefix (ownership moves to the stack). */
static void ps_push(pstack *st, prefix p)
{
    if (st->n == st->cap) {
        st->cap = st->cap ? st->cap * 2 : 1024;
        st->v = realloc(st->v, (size_t)st->cap * sizeof *st->v);
        if (!st->v) { perror("sched: realloc"); exit(2); }
    }
    st->v[st->n++] = p;
}

/** @brief Build the prefix base[0..pos) (defaults beyond base's length) followed by alt.
 * @param base The parent prefix.
 * @param pos Position of the alternative.
 * @param alt The alternative.
 * @param dp Its preemption cost.
 * @param dd Its deviation cost.
 * @return The new prefix. */
static prefix prefix_extend(const prefix *base, int pos, int alt, int dp, int dd)
{
    prefix p = { calloc((size_t)pos + 1, sizeof(uint16_t)), pos + 1, base->p + dp, base->d + dd };
    if (!p.ch) { perror("sched: calloc"); exit(2); }
    memcpy(p.ch, base->ch, (size_t)base->len * sizeof(uint16_t));
    p.ch[pos] = (uint16_t)alt;
    return p;
}

/** @brief Parse a choice string ("-" or comma-separated indices) into a prefix.
 * @param s The string.
 * @param out The prefix.
 * @return 0 on success, -1 on a malformed string. */
static int prefix_parse(const char *s, prefix *out)
{
    memset(out, 0, sizeof *out);
    out->ch = calloc(SCHED_MAX_CHOICES, sizeof(uint16_t));
    if (!out->ch) return -1;
    if (!s || !strcmp(s, "-") || !*s) return 0;
    while (*s) {
        char *end;
        const long v = strtol(s, &end, 10);
        if (end == s || v < 0 || v >= SCHED_MAX_ALTS || out->len >= SCHED_MAX_CHOICES) return -1;
        out->ch[out->len++] = (uint16_t)v;
        s = end;
        if (*s == ',') s++;
        else if (*s) return -1;
    }
    return 0;
}

/** @brief Fork a child that runs one schedule; the parent keeps the read end of its report pipe.
 * @param cfg The configuration.
 * @param pre Its prefix.
 * @param pct Nonzero to draw choices by PCT after the prefix.
 * @param seed PCT seed.
 * @param trace_always Nonzero to write the trace on a pass too.
 * @param c Receives the child.
 * @return 0, or -1 when fork or pipe failed. */
static int child_spawn(const sched_config *cfg, prefix pre, int pct, uint64_t seed, int trace_always, child *c)
{
    int fds[2];
    if (pipe(fds) != 0) return -1;
    fflush(NULL);
    const pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        close(fds[0]);
        sched_child_plan plan = { cfg, pre.ch, pre.len, fds[1], pct, seed, cfg->trace_path, trace_always };
        sched_child_run(&plan);
    }
    close(fds[1]);
    memset(c, 0, sizeof *c);
    c->pid = pid; c->fd = fds[0]; c->pre = pre;
    return 0;
}

/** @brief Read what a child's pipe holds into its buffer.
 * @param c The child.
 * @return 1 at end of file, 0 when more may come, -1 on error. */
static int child_read(child *c)
{
    if (c->cap - c->len < 65536) {
        c->cap = c->cap ? c->cap * 2 : 131072;
        c->buf = realloc(c->buf, c->cap);
        if (!c->buf) return -1;
    }
    const ssize_t r = read(c->fd, c->buf + c->len, c->cap - c->len);
    if (r < 0) return errno == EINTR ? 0 : -1;
    if (r == 0) return 1;
    c->len += (size_t)r;
    return 0;
}

/** What one finished child reported. */
typedef struct child_report {
    int status;           /* SCHED_CHILD_*, or -1 when the child died without an end record */
    long steps;
    int nchoices;
    char msg[512];
    char choices[4096];
} child_report;

/** @brief Decode a child's end record at buf[at..len).
 * @param b The buffer.
 * @param at Offset of the record's first byte after the tag.
 * @param len Buffer length.
 * @param rep Receives the record.
 * @return 0, or -1 when truncated. */
static int decode_end(const uint8_t *b, size_t at, size_t len, child_report *rep)
{
    int32_t st, nc; int64_t steps; uint16_t ml, cl;
    if (at + 18 > len) return -1;
    memcpy(&st, b + at, 4); memcpy(&steps, b + at + 4, 8); memcpy(&nc, b + at + 12, 4); memcpy(&ml, b + at + 16, 2);
    at += 18;
    if (at + ml + 2 > len) return -1;
    memcpy(rep->msg, b + at, ml < 511 ? ml : 511); rep->msg[ml < 511 ? ml : 511] = 0;
    at += ml;
    memcpy(&cl, b + at, 2); at += 2;
    if (at + cl > len) return -1;
    memcpy(rep->choices, b + at, cl < 4095 ? cl : 4095); rep->choices[cl < 4095 ? cl : 4095] = 0;
    rep->status = st; rep->steps = (long)steps; rep->nchoices = nc;
    return 0;
}

/** Bounds of a DFS exploration: the pair under exploration and the configured limits. */
typedef struct bounds { int p, d, max_p, max_d; } bounds;

/** @brief Expand a finished child's choice records into new prefixes: run now, deferred, or pruned.
 * @param c The child.
 * @param bd Bounds.
 * @param now Prefixes within the current pair.
 * @param later Prefixes deferred to a later pair.
 * @param r Result counters.
 * @param rep Receives the end record.
 * @return 0, or -1 on a malformed report. */
static int child_expand(const child *c, const bounds *bd, pstack *now, pstack *later,
                        sched_result *r, child_report *rep)
{
    size_t at = 0;
    int pos = c->pre.len;
    rep->status = -1;
    while (at < c->len) {
        const uint8_t tag = c->buf[at++];
        if (tag == SCHED_REC_END) return decode_end(c->buf, at, c->len, rep);
        if (tag != SCHED_REC_CHOICE || at + 2 > c->len) return -1;
        uint16_t n; memcpy(&n, c->buf + at, 2); at += 2;
        if (at + 2u * n > c->len) return -1;
        for (int k = 1; k < n; k++) {
            const int dp = c->buf[at + 2 * k], dd = c->buf[at + 2 * k + 1];
            const int p = c->pre.p + dp, d = c->pre.d + dd;
            if (p > bd->max_p || d > bd->max_d) { r->pruned++; continue; }
            ps_push(p <= bd->p && d <= bd->d ? now : later, prefix_extend(&c->pre, pos, k, dp, dd));
        }
        at += 2u * n;
        pos++;
    }
    return 0;
}

/** @brief Rename a failing child's trace file to the configured path, or delete a later one.
 * @param cfg The configuration.
 * @param pid The child.
 * @param keep Nonzero for the first failure. */
static void collect_trace(const sched_config *cfg, pid_t pid, int keep)
{
    if (!cfg->trace_path) return;
    char part[1024];
    snprintf(part, sizeof part, "%s.%ld.part", cfg->trace_path, (long)pid);
    if (keep) rename(part, cfg->trace_path);
    else unlink(part);
}

/** @brief Account a finished child's end record in the result.
 * @param cfg The configuration.
 * @param c The child.
 * @param rep Its end record.
 * @param bd The pair under exploration.
 * @param r The result. */
static void account(const sched_config *cfg, const child *c, const child_report *rep,
                    const bounds *bd, sched_result *r)
{
    r->schedules++;
    r->steps_total += rep->steps;
    if (rep->steps > r->steps_max) r->steps_max = rep->steps;
    if (rep->nchoices > r->choices_max) r->choices_max = rep->nchoices;
    if (rep->status == SCHED_CHILD_PASS) return;
    r->failures++;
    const int first = !r->found;
    collect_trace(cfg, c->pid, first);
    if (!first) return;
    r->found = 1;
    r->found_p = bd->p; r->found_d = bd->d;
    snprintf(r->choices, sizeof r->choices, "%s", rep->choices);
    snprintf(r->message, sizeof r->message, "%s",
             rep->status < 0 ? "child ended without a report (crash or abort)" : rep->msg);
}

/** @brief Wait for one child's pipe to close, reap it, and decode its report into the stacks and result.
 * @param cfg The configuration.
 * @param kids Running children.
 * @param nk Their count; the reaped child is removed.
 * @param bd Bounds.
 * @param now Current-pair stack.
 * @param later Deferred stack.
 * @param r Result.
 * @return 0, or -1 on an explorer error. */
static int reap_one(const sched_config *cfg, child *kids, int *nk, const bounds *bd,
                    pstack *now, pstack *later, sched_result *r)
{
    struct pollfd pf[64];
    for (int i = 0; i < *nk; i++) pf[i] = (struct pollfd){ kids[i].fd, POLLIN, 0 };
    for (;;) {
        if (poll(pf, (nfds_t)*nk, -1) < 0) { if (errno == EINTR) continue; return -1; }
        for (int i = 0; i < *nk; i++) {
            if (!pf[i].revents) continue;
            const int e = child_read(&kids[i]);
            if (e < 0) return -1;
            if (e == 0) continue;
            child *c = &kids[i];
            int ws = 0;
            close(c->fd);
            waitpid(c->pid, &ws, 0);
            child_report rep;
            memset(&rep, 0, sizeof rep);
            if (child_expand(c, bd, now, later, r, &rep) != 0) rep.status = -1;
            if (rep.status == SCHED_CHILD_PASS && !(WIFEXITED(ws) && WEXITSTATUS(ws) == 0)) rep.status = -1;
            account(cfg, c, &rep, bd, r);
            free(c->buf); free(c->pre.ch);
            kids[i] = kids[--*nk];
            return 0;
        }
    }
}

/** @brief Report whether exploration must stop: a failure under stop_on_fail, or the schedule cap.
 * @param cfg The configuration.
 * @param r The result so far.
 * @return 1 to stop. */
static int must_stop(const sched_config *cfg, const sched_result *r)
{
    if (cfg->stop_on_fail && r->found) return 1;
    return cfg->max_schedules > 0 && r->schedules >= cfg->max_schedules;
}

/** @brief Run every prefix on the current-pair stack (and the prefixes they spawn) to completion.
 * @param cfg The configuration.
 * @param bd Bounds.
 * @param now Current-pair stack.
 * @param later Deferred stack.
 * @param r Result.
 * @return 0, 1 when stopped early, -1 on an explorer error. */
static int run_pair(const sched_config *cfg, const bounds *bd, pstack *now, pstack *later, sched_result *r)
{
    child kids[64];
    int nk = 0;
    const int jobs = cfg->jobs > 1 ? (cfg->jobs < 64 ? cfg->jobs : 64) : 1;
    int stopped = 0;
    while (now->n > 0 || nk > 0) {
        while (!stopped && now->n > 0 && nk < jobs) {
            if (child_spawn(cfg, now->v[--now->n], 0, 0, 0, &kids[nk]) != 0) return -1;
            nk++;
        }
        if (nk == 0) break;
        if (reap_one(cfg, kids, &nk, bd, now, later, r) != 0) return -1;
        if (must_stop(cfg, r)) stopped = 1;
    }
    return stopped;
}

/** @brief Move every deferred prefix the pair covers onto the current-pair stack.
 * @param bd The pair.
 * @param now Current-pair stack.
 * @param later Deferred stack. */
static void undefer(const bounds *bd, pstack *now, pstack *later)
{
    long keep = 0;
    for (long i = 0; i < later->n; i++) {
        if (later->v[i].p <= bd->p && later->v[i].d <= bd->d) ps_push(now, later->v[i]);
        else later->v[keep++] = later->v[i];
    }
    later->n = keep;
}

/** @brief DFS over the joint bound: pairs in order of p + d, then p.
 * @param cfg The configuration.
 * @param r The result.
 * @return 0, or -1 on an explorer error. */
static int explore_dfs(const sched_config *cfg, sched_result *r)
{
    pstack now = {0}, later = {0};
    ps_push(&now, (prefix){ NULL, 0, 0, 0 });
    const int mp = cfg->max_preemptions, md = cfg->max_deviations;
    r->reached_p = r->reached_d = -1;
    for (int sum = 0; sum <= mp + md; sum++) {
        for (int p = 0; p <= sum && p <= mp; p++) {
            const int d = sum - p;
            if (d > md) continue;
            const bounds bd = { p, d, mp, md };
            undefer(&bd, &now, &later);
            const int e = run_pair(cfg, &bd, &now, &later, r);
            if (e < 0) return -1;
            if (e > 0) return 0;
            r->reached_p = p; r->reached_d = d;
        }
    }
    r->complete = later.n == 0;
    free(now.v); free(later.v);
    return 0;
}

/** @brief Run one child to completion and decode its end record (PCT and replay).
 * @param cfg The configuration.
 * @param pre Its prefix.
 * @param pct Nonzero for PCT.
 * @param seed PCT seed.
 * @param trace_always Write the trace on a pass too.
 * @param r Result.
 * @return 0, or -1 on an explorer error. */
static int run_single(const sched_config *cfg, prefix pre, int pct, uint64_t seed, int trace_always, sched_result *r)
{
    child c;
    if (child_spawn(cfg, pre, pct, seed, trace_always, &c) != 0) return -1;
    int e;
    while ((e = child_read(&c)) == 0) {}
    close(c.fd);
    int ws = 0;
    waitpid(c.pid, &ws, 0);
    if (e < 0) return -1;
    child_report rep;
    memset(&rep, 0, sizeof rep);
    rep.status = -1;
    size_t at = 0;
    while (at < c.len) {
        const uint8_t tag = c.buf[at++];
        if (tag == SCHED_REC_END) { if (decode_end(c.buf, at, c.len, &rep) != 0) rep.status = -1; break; }
        if (tag != SCHED_REC_CHOICE || at + 2 > c.len) return -1;
        uint16_t n; memcpy(&n, c.buf + at, 2);
        at += 2 + 2u * n;   /* a replay reports the choices past its string; nothing to expand */
    }
    if (trace_always && rep.status == SCHED_CHILD_PASS && cfg->trace_path) collect_trace(cfg, c.pid, 1);
    const bounds bd = { 0, 0, 0, 0 };
    account(cfg, &c, &rep, &bd, r);
    free(c.buf);
    return 0;
}

/** @brief PCT sampling: pct_runs schedules, run i seeded from seed and i.
 * @param cfg The configuration.
 * @param r The result.
 * @return 0, or -1 on an explorer error. */
static int explore_pct(const sched_config *cfg, sched_result *r)
{
    uint64_t st = cfg->seed;
    for (long i = 0; i < cfg->pct_runs; i++) {
        st += 0x9e3779b97f4a7c15ull;
        uint64_t z = st;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        if (run_single(cfg, (prefix){ NULL, 0, 0, 0 }, 1, z ^ (z >> 31), 0, r) != 0) return -1;
        if (must_stop(cfg, r)) break;
    }
    r->complete = 0;
    return 0;
}

int sched_explore(const sched_config *cfg, sched_result *out)
{
    memset(out, 0, sizeof *out);
    if (cfg->trace_path) unlink(cfg->trace_path);
    int e = -1;
    if (cfg->mode == SCHED_MODE_DFS) e = explore_dfs(cfg, out);
    else if (cfg->mode == SCHED_MODE_PCT) e = explore_pct(cfg, out);
    else {
        prefix pre;
        if (prefix_parse(cfg->replay, &pre) != 0) { fprintf(stderr, "sched: malformed choice string\n"); return 2; }
        e = run_single(cfg, pre, 0, 0, 1, out);
        free(pre.ch);
    }
    if (e < 0) { perror("sched: explorer"); return 2; }
    return out->found ? 1 : 0;
}

void sched_print_result(const char *label, const sched_result *r)
{
    printf("sched: %s: schedules=%ld failures=%ld pruned=%ld steps/schedule max=%ld mean=%.1f "
           "choices max=%ld bound reached=(%d,%d)%s",
           label, r->schedules, r->failures, r->pruned, r->steps_max,
           r->schedules ? (double)r->steps_total / (double)r->schedules : 0.0, r->choices_max,
           r->reached_p, r->reached_d, r->complete ? " complete" : "");
    if (r->found)
        printf(" FOUND at (%d,%d): %s; choices %s", r->found_p, r->found_d, r->message, r->choices);
    printf("\n");
    fflush(stdout);
}
