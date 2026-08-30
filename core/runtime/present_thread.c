/* The present thread: the consumer half of eager production.
 *
 * The thread running the guest extracts every scene of a span while the
 * guest is idle and publishes them into the scene ring (native_render.c),
 * each tagged with the instant its animation alpha was computed for. This
 * thread sleeps until the earliest pending target, picks the pending scene
 * nearest that instant, composes it and presents it -- through the next
 * tick's execution, which is the whole point: the display no longer waits
 * for the sim thread, and the sim thread never waits for the display.
 *
 * Ownership: from set_active(1) until set_active(0) returns, this thread is
 * the only one issuing RHI frames. The guest-side producer touches the
 * device only through persistently-mapped arena writes, which the terrain
 * cache's retirement window keeps clear of any frame in flight. Everything
 * else the compose reads was copied into the ring slot at publish time.
 *
 * Pacing: the producer put the targets on the grid; decimation shows up as
 * targets that skip slots, and a late scene (its target already passed when
 * it was published) is shown at once rather than held for a slot that is
 * gone. Alpha binds at consume time: pick() takes the nearest pending scene
 * and retires the older ones unpresented.
 *
 * Two steps, like the extraction itself: the scene for the next instant is
 * RENDERED into a stage image right after the previous frame goes out, while
 * the display is still showing that one; at the instant, what is left is an
 * acquire and a blit (rhi_present_stage). The render -- 0.3 ms on a light
 * scene, up to 1.5 ms on a heavy vista, draw-call bound -- was the whole
 * residual when it sat between the instant and the vblank.
 *
 * Under a vsynced (FIFO) swapchain the display's vblanks are the only
 * instants a frame can appear, and every timestamp this engine takes itself
 * is a SUBMIT time. The driver's present_wait returns at the flip, and from
 * those flips this thread fits the vblank LATTICE (a base and a period) the
 * planner then puts its targets on -- so a target IS a vblank, the
 * animation instant (target - lag) is exact, and the scene for vblank k is
 * composed right after vblank k-1 flips: earliest possible, which is free
 * under FIFO (the frame waits in the queue for its vblank), and it turns a
 * late wake-up into slack instead of a doubled refresh. Without flips (no
 * present timing, mailbox/immediate) it falls back to submitting at the
 * target, as the lattice never locks.
 */
#include "openpete_present_thread.h"

#include "openpete.h"
#include "openpete_time.h"
#include "openpete_native_render.h"
#include "openpete_pace_log.h"
#include "openpete_log.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

static struct {
    SDL_Thread    *thr;
    SDL_Mutex     *mx;
    SDL_Condition *cv;        /* activate / kick / quit */
    SDL_Condition *idle_cv;   /* a compose finished */

    int  active;              /* the thread owns the display */
    int  busy;                /* a compose is in flight right now */
    int  quit;

    openpete_present_thread_cb cb; void *cb_ctx;

    /* census */
    unsigned  presents;       /* scenes that went out */
    unsigned  refused;        /* compose ran but the swapchain declined */
    unsigned  late;           /* compose instant already passed when picked */
    int       late_run;       /* consecutive late presents */
    int       drop_cooldown;  /* presents until another queue-drain drop may fire */
    unsigned  late_drops;     /* staged frames dropped to drain the present queue */
    op_time_t late_ns;        /* summed lateness */
    op_time_t late_max_ns;
    op_time_t render_ema;     /* render into a stage image, per scene */
    op_time_t present_ema;    /* acquire + blit + present, per frame */
    unsigned  render_refused; /* the render frame could not begin (rebuild pending, fence bound) */
    unsigned  discarded;      /* staged frames never presented (deactivated, stages rebuilt) */
    int       flip_pending;   /* a present is queued whose flip has not been observed */
    int       flip_holdoff;   /* presents left to make without a flip wait, after a timeout */
    unsigned  flips;          /* flips observed (present_wait returned at one) */
    unsigned  flip_timeouts;  /* the wait ran to its bound without a flip */
    op_time_t flip_over_ns;   /* summed overrun of the wait past its bound */
    op_time_t flip_over_max_ns;
    unsigned  drains;         /* set_active(0) calls */
    op_time_t drain_wait_ns;  /* time the caller spent waiting at a drain */
    op_time_t drain_wait_max_ns;
} s_pt;

static int s_enabled = -1;

/* The present instant's guard after the previous vblank, and the least
 * lead a frame needs to make its vblank (the driver: a submit 0.33 ms
 * before a vblank still made it, one 0.13 ms before did not). The guard is
 * NOT the smallest that keeps the frame off the previous vblank (0.3 ms
 * did): a present issued 0.35 ms after a vblank BLOCKED inside the driver
 * for ~1.5 ms, until ~0.2 ms before the next -- measured at 0.3 (blocks),
 * 0.9 and 1.5 (do not) -- and a thread stuck in the present cannot render
 * the next frame ahead. At 0.9 the present returns in ~20 us and a 1.5 ms
 * render still finishes 0.6 ms before the following instant. */
enum { PT_COMPOSE_GUARD_NS = 900000, PT_MIN_LEAD_NS = 800000, PT_FLIP_HOLDOFF = 3 };

/* OPENPETE_PT_DBG=1: a per-iteration trace of the present thread's timing
 * decisions (waits, composes, lattice fits) to stderr, in microseconds
 * relative to the lattice base, bounded at PT_DBG_MAX lines. Diagnostic
 * only; it is how every finding in this file was made. */
static int pt_dbg(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("OPENPETE_PT_DBG"); on = (e && *e && *e != '0'); }
    return on;
}
static int s_dbg_n;
#define PT_DBG_MAX 60000

/* The vblank lattice: v(n) = base + n * period, least-squares over the last
 * LAT_WINDOW observed flips. Each flip's n is the nearest lattice index
 * under the current fit, so a refresh that showed nothing (a doubled frame)
 * simply skips an n -- the fit is of the DISPLAY's clock, not of our
 * presents. The period from the display mode (479.990 Hz) is only the seed:
 * the measured one differed from it by 2e-5, which walked the submit phase
 * a full slot every ~50 s and put a doubled refresh at every wrap. */
enum { LAT_WINDOW = 128, LAT_FIT_MIN = 32, LAT_LOCK_MIN = 48, LAT_CLEAN_RUN = 24, LAT_OUT_RUN = 8 };
static struct {
    int64_t  t[LAT_WINDOW];   /* flip time, relative to t0 */
    int64_t  n[LAT_WINDOW];   /* lattice index */
    int      cnt, head;
    int64_t  t0;              /* the first flip of this fit */
    double   base, period;    /* v(n) = t0 + base + n * period */
    int      locked;
    int      run_out;         /* consecutive rejected flips */
    int      run_clean;       /* consecutive flips within an eighth of a slot */
    uint64_t last_id;
    unsigned outliers, resets;
} s_lat;

static double dbg_rel(op_time_t t) { return (double)(t - s_lat.t0) / 1000.0; }

static void lattice_reset_why(const char *why, uint64_t id)
{
    if (pt_dbg() && s_lat.cnt && s_dbg_n < PT_DBG_MAX) {
        s_dbg_n++;
        fprintf(stderr, "[pt] lattice RESET (%s) id=%llu last=%llu cnt=%d period=%.2f\n",
                why, (unsigned long long)id, (unsigned long long)s_lat.last_id, s_lat.cnt, s_lat.period / 1000.0);
    }
}
static void lattice_reset(void)
{
    if (s_lat.cnt) s_lat.resets++;
    s_lat.cnt = s_lat.head = 0; s_lat.locked = 0; s_lat.last_id = 0;
    s_lat.run_out = s_lat.run_clean = 0;
}

static void lattice_observe(uint64_t id, op_time_t t, double seed_period)
{
    /* Ids restart at 1 on a swapchain rebuild: a new surface, a new clock. */
    if (s_lat.cnt && id <= s_lat.last_id) { lattice_reset_why("id", id); lattice_reset(); }
    s_lat.last_id = id;
    if (!s_lat.cnt) {
        if (seed_period <= 0.0) return;
        s_lat.t0 = t; s_lat.base = 0.0; s_lat.period = seed_period;
        s_lat.t[0] = 0; s_lat.n[0] = 0; s_lat.cnt = 1; s_lat.head = 1;
        return;
    }
    const double rel = (double)(t - s_lat.t0);
    const double fn = (rel - s_lat.base) / s_lat.period;
    const int64_t n = (int64_t)floor(fn + 0.5);
    const double resid = rel - (s_lat.base + (double)n * s_lat.period);
    /* A flip a quarter-slot off the lattice is the thread waking late from
     * the wait (or a frame the display took a refresh late), not the
     * display moving; it would drag the fit. Judged once the fit has
     * something to judge against. */
    if (s_lat.cnt >= LAT_FIT_MIN && fabs(resid) > s_lat.period * 0.25) {
        s_lat.outliers++;
        /* A run of them is not noise but a fit that is wrong (locked on a
         * level-load transient, where frames miss vblanks and the driver
         * signals a superseded present when it is discarded, not when
         * anything flipped): the flips are where they are, and a lattice
         * that keeps rejecting them would stay wrong for good. */
        if (++s_lat.run_out >= LAT_OUT_RUN) { lattice_reset_why("outliers", id); lattice_reset(); }
        return;
    }
    s_lat.run_out = 0;
    s_lat.run_clean = fabs(resid) < s_lat.period * 0.125 ? s_lat.run_clean + 1 : 0;
    s_lat.t[s_lat.head] = (int64_t)rel; s_lat.n[s_lat.head] = n;
    s_lat.head = (s_lat.head + 1) % LAT_WINDOW;
    if (s_lat.cnt < LAT_WINDOW) s_lat.cnt++;
    double sn = 0, st = 0;
    for (int i = 0; i < s_lat.cnt; i++) { sn += (double)s_lat.n[i]; st += (double)s_lat.t[i]; }
    const double mn = sn / s_lat.cnt, mt = st / s_lat.cnt;
    /* Until the window holds enough refreshes to carry a slope, the period
     * stays the display mode's and only the phase is fitted: a period from
     * a handful of flips, each +-50 us, is off by tenths of a percent, which
     * across thirty refreshes is a phase error of a slot -- and a lattice
     * that keeps re-seeding never locks. */
    double p = s_lat.period;
    if (s_lat.cnt >= LAT_FIT_MIN) {
        double sxx = 0, sxy = 0;
        for (int i = 0; i < s_lat.cnt; i++) {
            const double dn = (double)s_lat.n[i] - mn;
            sxx += dn * dn; sxy += dn * ((double)s_lat.t[i] - mt);
        }
        if (sxx > 0.0) p = sxy / sxx;
        /* A fit that left the mode's rate by more than 1 % is not this
         * display (the mode changed under us): start over. */
        if (p < seed_period * 0.99 || p > seed_period * 1.01) { lattice_reset_why("period", id); lattice_reset(); return; }
    }
    s_lat.period = p; s_lat.base = mt - p * mn;
    /* Lock on a run of flips the fit already predicts to an eighth of a
     * slot: a lock taken on the first window that happens to fit would as
     * readily be a lock on a transient. */
    if (!s_lat.locked && s_lat.cnt >= LAT_LOCK_MIN && s_lat.run_clean >= LAT_CLEAN_RUN) s_lat.locked = 1;
    if (pt_dbg() && s_dbg_n < PT_DBG_MAX)
        fprintf(stderr, "[pt] lattice id=%llu n=%lld resid=%.0f period=%.2f base=%.0f cnt=%d locked=%d\n",
                (unsigned long long)id, (long long)n, resid / 1000.0, p / 1000.0, s_lat.base / 1000.0, s_lat.cnt, s_lat.locked);
}

/* Next lattice instant at or after `t` (mutex held). */
static op_time_t lattice_ceil_locked(op_time_t t)
{
    const double rel = (double)(t - s_lat.t0);
    const double n = ceil((rel - s_lat.base) / s_lat.period - 1e-6);
    return s_lat.t0 + (op_time_t)(s_lat.base + n * s_lat.period + 0.5);
}

int openpete_present_thread_lattice(int64_t *base_ns, double *period_ns)
{
    if (!s_pt.thr) return 0;
    SDL_LockMutex(s_pt.mx);
    const int ok = s_lat.locked;
    if (ok) {
        if (base_ns)   *base_ns   = s_lat.t0 + (int64_t)(s_lat.base + 0.5);
        if (period_ns) *period_ns = s_lat.period;
    }
    SDL_UnlockMutex(s_pt.mx);
    return ok;
}

/* Eager production is the shipped default: the span is extracted ahead of
 * the tick and presented from this thread's scene ring, so the tick's own
 * execution no longer opens a hole in the display. OPENPETE_EAGER=0 puts the
 * present back on the sim thread (the first diagnostic for a pacing
 * regression: it removes the ring, the lag and this thread in one move). */
int openpete_present_thread_enabled(void)
{
    if (s_enabled < 0) {
        const char *e = getenv("OPENPETE_EAGER");
        s_enabled = (e && *e) ? (*e != '0') : 1;
    }
    return s_enabled;
}

/* OPENPETE_EAGER_LATTICE=1: plan and compose on the display's vblank
 * lattice (below), which implies present timing. Off, the thread composes
 * each scene at its target and observes flips only under
 * OPENPETE_PACE_FLIP_LOG -- the form the eager default was measured in. */
int openpete_present_thread_lattice_enabled(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("OPENPETE_EAGER_LATTICE"); on = (e && *e && *e != '0') ? 1 : 0; }
    return on;
}

/* Sleep until `t`, but wake early on a kick/deactivate/quit. Coarse
 * condvar waits for the bulk, a spin-capable hybrid for the last stretch:
 * at 480 Hz a slot is 2.08 ms and a millisecond timer is too blunt. Called
 * with the mutex held; returns with it held. */
static void wait_until_locked(op_time_t t)
{
    for (;;) {
        if (s_pt.quit || !s_pt.active) return;
        const op_time_t now = op_now();
        if (now >= t) return;
        const op_time_t left = t - now;
        /* A condvar timeout is a coarse timer (a millisecond or more of
         * oversleep on Windows), so it covers only the bulk, and only when
         * there is bulk: with less than 4 ms left the whole wait goes to the
         * hybrid sleep, which lands within tens of microseconds. Measured
         * before this split: ~10 % of presents 1.8 ms late, each followed by
         * one immediately -- the display saw pairs where the grid had two
         * slots. */
        if (left > 4000000) {
            SDL_WaitConditionTimeout(s_pt.cv, s_pt.mx, (Sint32)((left - 3000000) / 1000000));
            /* re-evaluate: a kick may have published an earlier target */
            return;
        }
        SDL_UnlockMutex(s_pt.mx);
        op_sleep_until(t);
        SDL_LockMutex(s_pt.mx);
        return;
    }
}

/* The vsynced slot, from the swapchain's refresh (0 = unknown / not FIFO,
 * or the lattice is not enabled: then there is nothing to lead). */
static double display_slot_ns(void)
{
    if (!openpete_present_thread_lattice_enabled()) return 0.0;
    const float hz = openpete_native_display_refresh_hz();
    if (hz <= 1.0f || !openpete_native_display_vsync()) return 0.0;
    return 1e9 / (double)hz;
}

/* Wait for the flip of the present last queued, at most until `bound`.
 * Called with the mutex held; releases it for the wait. Observing the flip
 * is what feeds the lattice, and it is also this thread's natural wake-up:
 * the scene for the next vblank is composed as soon as the previous one has
 * flipped. Counts as busy -- the swapchain is externally synchronised, so
 * the deactivating thread must not present while this wait is inside the
 * driver. */
static void wait_flip_locked(op_time_t bound)
{
    const op_time_t now = op_now();
    if (bound <= now) return;
    s_pt.busy = 1;
    SDL_UnlockMutex(s_pt.mx);
    uint64_t id = 0, waited = 0;
    /* One BLOCKING wait, a millisecond or more long, on the newest id
     * only. Polling is not an option on this path: the driver answers a
     * wait with a timeout under about a millisecond with VK_SUCCESS at
     * once, for a present it has not displayed (every first poll after a
     * present, measured), so a poll would confirm every frame the moment
     * it was queued. The older ids ARE confirmed first, in order (see
     * rhi_present_wait): a wait on the newest with an older one still
     * unconfirmed returns coarse, 0.5 ms-quantised times. */
    const int r = openpete_native_render_present_wait((uint64_t)(bound - now), &id, &waited);
    const op_time_t after = op_now();
    SDL_LockMutex(s_pt.mx);
    s_pt.busy = 0;
    SDL_SignalCondition(s_pt.idle_cv);
    if (pt_dbg() && s_dbg_n < PT_DBG_MAX) {
        s_dbg_n++;
        fprintf(stderr, "[pt] wait id=%llu r=%d from=%.0f bound=%.0f ret=%.0f waited=%.0f outstanding=%d\n",
                (unsigned long long)id, r, dbg_rel(now), dbg_rel(bound), dbg_rel(after),
                (double)waited / 1000.0, openpete_native_render_present_outstanding());  /* after: confirmed by this wait */
    }
    if (r == 1) {
        s_pt.flips++;
        s_pt.flip_pending = 0;
        lattice_observe(id, after, display_slot_ns());
        openpete_pace_flip(id, (int64_t)after, (int64_t)waited);
    } else {
        /* 0: still not displayed by the bound (a late flip; the next scene
         * goes ahead regardless). -1: nothing outstanding or no timing. */
        if (r == 0) {
            s_pt.flip_timeouts++;
            if (after > bound) {
                s_pt.flip_over_ns += after - bound;
                if (after - bound > s_pt.flip_over_max_ns) s_pt.flip_over_max_ns = after - bound;
            }
            /* ONE wait per present. Re-waiting on an id that has timed out
             * (and polling it) turned the driver's flip reports coarse --
             * 0.5 ms-quantised return times, against +-40 us when each id
             * is waited on once -- and the lattice fed on those reports
             * fell apart. The id is confirmed, stale, at a later wait.
             *
             * And no wait at all for the next few presents. The driver's
             * timeout is coarse -- a wait bounded at the compose instant
             * came back up to a millisecond after it -- so a late flip
             * makes the next compose late, its frame misses too, and a
             * heavy scene (a 48k-prim vista: compose and submit 1-1.9 ms)
             * never gets back on the lattice: every wait ran to its bound
             * for the rest of the leg. The lattice does not need every
             * flip; the compose does need its instant. */
            s_pt.flip_pending = 0;
            s_pt.flip_holdoff = PT_FLIP_HOLDOFF;
        } else s_pt.flip_pending = 0;
    }
}

/* The present instant for a frame targeted at `target`: a guard after the
 * vblank before it (any earlier and a fit error could let the frame slip
 * onto that vblank instead; any later and the acquire + blit could miss
 * its own). OPENPETE_PT_GUARD_US overrides the guard, for measurement.
 * Mutex held. */

static op_time_t compose_guard_ns(void)
{
    static op_time_t g = -1;
    if (g < 0) { const char *e = getenv("OPENPETE_PT_GUARD_US"); g = (e && *e) ? (op_time_t)atoll(e) * 1000 : PT_COMPOSE_GUARD_NS; }
    return g;
}
static op_time_t compose_at_locked(op_time_t target, double slot)
{
    if (slot <= 0.0) return target;                 /* not vsynced: no lattice to lead */
    /* The same lead whether or not the lattice is locked: a slot less a
     * guard. Unlocked, the target is some unknown phase of a refresh, and
     * a frame ready a slot early still takes the first vblank after it --
     * one per refresh, which is what the lattice is fitted from. A shorter
     * lead there (0.8 ms was tried) is not enough for a heavy scene's
     * compose, every frame then misses, and the lattice never locks. */
    const double period = s_lat.locked ? s_lat.period : slot;
    return target - (op_time_t)(period + 0.5) + compose_guard_ns();
}

/* The frame rendered ahead, waiting for its instant. */
static struct {
    int       valid;
    int       stage;          /* stage image it was rendered into */
    uint32_t  gen;            /* rhi stage generation at render time */
    op_time_t target;
    double    alpha; int paired, kind;
} s_staged;
static int s_stage_next;

static int present_thread_main(void *unused)
{
    (void)unused;
    SDL_LockMutex(s_pt.mx);
    for (;;) {
        if (s_pt.quit) break;
        if (!s_pt.active) {
            lattice_reset_why("inactive", 0);
            lattice_reset();
            s_pt.flip_pending = 0; s_pt.flip_holdoff = 0;
            if (s_staged.valid) { s_staged.valid = 0; s_pt.discarded++; }
            SDL_WaitCondition(s_pt.cv, s_pt.mx);
            continue;
        }
        const double slot = display_slot_ns();
        op_time_t now = op_now();
        const int on_lattice = s_lat.locked && slot > 0.0;

        if (!s_staged.valid) {
            /* Nothing rendered ahead: render the scene for the next present
             * instant NOW, while the display is still showing the last one.
             * The render (0.3 ms on a light scene, up to 1.5 ms on a heavy
             * vista) is then off the path between the compose instant and
             * the vblank; what remains there is an acquire and a blit. */
            const op_time_t target = openpete_native_scene_next_target();
            if (!target) {
                /* Nothing pending: the flip of the last present is still
                 * worth observing (it keeps the lattice current); otherwise
                 * sleep until a publish kicks us. Bounded so a lost kick
                 * cannot park the display for good. */
                if (s_pt.flip_pending) wait_flip_locked(now + 4000000);
                else SDL_WaitConditionTimeout(s_pt.cv, s_pt.mx, 4);
                continue;
            }
            /* Pick against the first vblank whose PRESENT instant (a guard
             * after the vblank before it) a render started now can meet,
             * else against now: the animation moment nearest the flip.
             * Picking by the earliest target would, once behind, render
             * every stale scene in turn and never catch up; this retires
             * the older ones instead. */
            const op_time_t pick_at = on_lattice
                ? lattice_ceil_locked(now + s_pt.render_ema + 100000
                                      + (op_time_t)(s_lat.period + 0.5) - compose_guard_ns())
                : now;
            const int slot_idx = openpete_native_scene_pick(pick_at);
            if (slot_idx < 0) continue;
            op_time_t t_ns = 0; double alpha = 0.0; int paired = 0, kind = OP_PACE_INTERMEDIATE;
            openpete_native_scene_slot_info(slot_idx, &t_ns, &alpha, &paired, &kind);
            /* OPENPETE_PT_STAGED=0: render at the instant instead (A/B). */
            static int staged_on = -1;
            if (staged_on < 0) { const char *e = getenv("OPENPETE_PT_STAGED"); staged_on = !(e && *e == '0'); }
            const int nstages = staged_on ? openpete_native_stage_count() : 0;
            const int stage = nstages > 0 ? (s_stage_next++ % nstages) : -1;
            if (pt_dbg() && s_dbg_n < PT_DBG_MAX) {
                s_dbg_n++;
                fprintf(stderr, "[pt] render target=%.0f now=%.0f stage=%d\n",
                        dbg_rel(t_ns), dbg_rel(now), stage);
            }
            s_pt.busy = 1;
            SDL_UnlockMutex(s_pt.mx);
            const op_time_t c0 = op_now();
            const uint32_t gen = openpete_native_stage_gen();
            int did;
            if (stage >= 0) did = openpete_native_render_scene_to_stage(slot_idx, stage, (uint64_t)(c0 / 1000));
            else {
                /* Unstaged: wait for the instant here, then render and present in one. */
                const op_time_t at1 = compose_at_locked(t_ns, slot);
                if (op_now() < at1) op_sleep_until(at1);
                did = openpete_native_render_scene(slot_idx, (uint64_t)(op_now() / 1000));
            }
            const op_time_t c = op_now() - c0;
            SDL_LockMutex(s_pt.mx);
            s_pt.busy = 0;
            SDL_SignalCondition(s_pt.idle_cv);
            if (!did) { s_pt.render_refused++; continue; }
            if (stage < 0) {
                openpete_pace_record_present((uint8_t)kind, alpha, paired);
                if (s_pt.cb) s_pt.cb(s_pt.cb_ctx, kind, alpha, paired);
                s_pt.presents++;
                if (s_pt.flip_holdoff > 0) s_pt.flip_holdoff--; else s_pt.flip_pending = 1;
                continue;
            }
            s_pt.render_ema = s_pt.render_ema ? (s_pt.render_ema * 7 + c) / 8 : c;
            /* And the swapchain image, now: an image the display let go of
             * at the last vblank is not available until the driver has
             * processed the release, and a present on it blocks until
             * then (1.3-1.7 ms, measured). Held here, it is ready by the
             * instant. */
            SDL_UnlockMutex(s_pt.mx);
            openpete_native_present_stage_acquire();
            SDL_LockMutex(s_pt.mx);
            s_staged.valid = 1; s_staged.stage = stage; s_staged.gen = gen;
            s_staged.target = t_ns; s_staged.alpha = alpha; s_staged.paired = paired; s_staged.kind = kind;
            continue;
        }

        /* A frame is staged: present it at its instant -- a guard after the
         * previous vblank on the lattice, else at its target. */
        const op_time_t at = compose_at_locked(s_staged.target, slot);
        if (now < at) {
            if (s_pt.flip_pending) {
                /* The previous flip is due a guard before `at`: wait for
                 * it, never past `at` (the present instant is the lattice's,
                 * not the flip's -- a present that waited for a LATE flip
                 * would itself be late, and so would every one after it),
                 * and never longer than a slot. */
                op_time_t bound = at;
                const op_time_t cap = now + (op_time_t)(slot > 0.0 ? slot : 4e6) + PT_COMPOSE_GUARD_NS;
                if (bound > cap) bound = cap;
                wait_flip_locked(bound);
                continue;
            }
            wait_until_locked(at);
            continue;
        }
        if (pt_dbg() && s_dbg_n < PT_DBG_MAX) {
            s_dbg_n++;
            fprintf(stderr, "[pt] present target=%.0f at=%.0f now=%.0f stage=%d\n",
                    dbg_rel(s_staged.target), dbg_rel(at), dbg_rel(now), s_staged.stage);
        }
        if (now - at > 1000000) {          /* >1 ms past its present instant */
            s_pt.late++; s_pt.late_ns += now - at;
            if (now - at > s_pt.late_max_ns) s_pt.late_max_ns = now - at;
            /* Sustained lateness with newer scenes already pending is the
             * deep-queue equilibrium, not load: after a stall (a swapchain
             * rebuild is ~40 ms) the backlog goes out back-to-back,
             * stacking presents with no vblank between, and a FIFO queue
             * that deep never drains on its own — the blocking acquire
             * then eats a whole refresh per frame, and from then on
             * presents run a slot late in a stable alternating pattern
             * (one a slot late, the next fired at once). Because it
             * alternates, the detector is a leaky counter, not a
             * consecutive run: +2 on a late present, -1 on a punctual
             * one, so the pattern accrues while healthy jitter decays.
             * One dropped frame shrinks the queue by one; the cooldown
             * keeps this off a machine that is merely slow (its lateness
             * survives any number of drops, so dropping would only shed
             * frames it could have shown). */
            if (s_pt.drop_cooldown > 0) s_pt.drop_cooldown--;
            else if ((s_pt.late_run += 2) >= 16) {
                const int64_t nt = openpete_native_scene_next_target();
                if (nt && nt > s_staged.target) {
                    s_staged.valid = 0;
                    s_pt.late_drops++;
                    s_pt.late_run = 0;
                    s_pt.drop_cooldown = 16;
                    continue;
                }
            }
        } else if (s_pt.late_run > 0) s_pt.late_run--;
        const int kind = s_staged.kind, paired = s_staged.paired;
        const double alpha = s_staged.alpha;
        const int stage = s_staged.stage;
        const int stale_gen = s_staged.gen != openpete_native_stage_gen();
        s_staged.valid = 0;
        if (stale_gen) { s_pt.discarded++; continue; }   /* the stages were rebuilt under it */
        s_pt.busy = 1;
        SDL_UnlockMutex(s_pt.mx);
        const op_time_t c0 = op_now();
        const int did = openpete_native_present_stage(stage);
        const op_time_t c = op_now() - c0;
        /* The refusal ledger is what turns a declined swapchain into SHED
         * records; run it even without a present. */
        openpete_pace_record_present((uint8_t)kind, alpha, paired);
        if (did && s_pt.cb) s_pt.cb(s_pt.cb_ctx, kind, alpha, paired);
        SDL_LockMutex(s_pt.mx);
        s_pt.busy = 0;
        if (did) {
            s_pt.presents++;
            s_pt.present_ema = s_pt.present_ema ? (s_pt.present_ema * 7 + c) / 8 : c;
            if (s_pt.flip_holdoff > 0) s_pt.flip_holdoff--;
            else s_pt.flip_pending = 1;
        } else s_pt.refused++;
        SDL_SignalCondition(s_pt.idle_cv);
    }
    SDL_UnlockMutex(s_pt.mx);
    return 0;
}

int openpete_present_thread_start(void)
{
    if (!openpete_present_thread_enabled()) return 0;
    if (s_pt.thr) return 1;
    s_pt.mx      = SDL_CreateMutex();
    s_pt.cv      = SDL_CreateCondition();
    s_pt.idle_cv = SDL_CreateCondition();
    if (!s_pt.mx || !s_pt.cv || !s_pt.idle_cv) return 0;
    s_pt.thr = SDL_CreateThread(present_thread_main, "openpete-present", NULL);
    if (!s_pt.thr) {
        OPL_WARN_F("present-thread", "SDL_CreateThread failed: %s", SDL_GetError());
        return 0;
    }
    OPL_INFO_F("present-thread", "%s", "started: eager scenes present on their own targets");
    return 1;
}

void openpete_present_thread_set_callback(openpete_present_thread_cb cb, void *ctx)
{
    s_pt.cb = cb; s_pt.cb_ctx = ctx;
}

void openpete_present_thread_kick(void)
{
    if (!s_pt.thr) return;
    SDL_LockMutex(s_pt.mx);
    SDL_SignalCondition(s_pt.cv);
    SDL_UnlockMutex(s_pt.mx);
}

int openpete_present_thread_active(void) { return s_pt.thr && s_pt.active; }

void openpete_present_thread_set_active(int on)
{
    if (!s_pt.thr) return;
    SDL_LockMutex(s_pt.mx);
    if (on) {
        if (!s_pt.active) { s_pt.active = 1; SDL_SignalCondition(s_pt.cv); }
        SDL_UnlockMutex(s_pt.mx);
        return;
    }
    if (s_pt.active) {
        s_pt.active = 0;
        SDL_SignalCondition(s_pt.cv);
        s_pt.drains++;
        /* Nothing pending survives: a scene extracted for a moment the
         * inline path is about to present would be shown twice. */
        openpete_native_scene_ring_flush();
        const op_time_t t0 = op_now();
        while (s_pt.busy) SDL_WaitCondition(s_pt.idle_cv, s_pt.mx);
        const op_time_t w = op_now() - t0;
        s_pt.drain_wait_ns += w;
        if (w > s_pt.drain_wait_max_ns) s_pt.drain_wait_max_ns = w;
    }
    SDL_UnlockMutex(s_pt.mx);
}

void openpete_present_thread_stop(void)
{
    if (!s_pt.thr) return;
    openpete_present_thread_set_active(0);
    SDL_LockMutex(s_pt.mx);
    s_pt.quit = 1;
    SDL_SignalCondition(s_pt.cv);
    SDL_UnlockMutex(s_pt.mx);
    SDL_WaitThread(s_pt.thr, NULL);
    s_pt.thr = NULL;
    SDL_DestroyCondition(s_pt.idle_cv);
    SDL_DestroyCondition(s_pt.cv);
    SDL_DestroyMutex(s_pt.mx);
}

void openpete_present_thread_report(void)
{
    if (!s_pt.presents && !s_pt.refused) return;
    OPL_INFO_F("present-thread",
               "%u scene(s) presented, %u refused by the swapchain, %u render(s) refused, %u staged frame(s) discarded; "
               "%u presented late (%.3f ms mean, %.3f ms max), %u dropped to drain the queue; render %.3f ms ema, present step %.3f ms ema; "
               "%u drain(s), %.3f ms waited total, %.3f ms max",
               s_pt.presents, s_pt.refused, s_pt.render_refused, s_pt.discarded, s_pt.late,
               s_pt.late ? (double)s_pt.late_ns / s_pt.late / 1e6 : 0.0,
               (double)s_pt.late_max_ns / 1e6, s_pt.late_drops,
               (double)s_pt.render_ema / 1e6, (double)s_pt.present_ema / 1e6, s_pt.drains,
               (double)s_pt.drain_wait_ns / 1e6, (double)s_pt.drain_wait_max_ns / 1e6);
    if (s_pt.flips || s_pt.flip_timeouts)
        OPL_INFO_F("present-thread",
                   "display: %u flip(s) observed, %u wait(s) ran to their bound (overrun %.3f ms mean, %.3f ms max); "
                   "lattice %s, period %.4f ms (%.3f Hz), %u outlier(s), %u reset(s)",
                   s_pt.flips, s_pt.flip_timeouts,
                   s_pt.flip_timeouts ? (double)s_pt.flip_over_ns / s_pt.flip_timeouts / 1e6 : 0.0,
                   (double)s_pt.flip_over_max_ns / 1e6,
                   s_lat.locked ? "locked" : "unlocked", s_lat.period / 1e6,
                   s_lat.period > 0.0 ? 1e9 / s_lat.period : 0.0, s_lat.outliers, s_lat.resets);
    openpete_native_scene_ring_report();
}

unsigned openpete_present_thread_presents(void) { return s_pt.presents; }
/* What one present costs this thread, compose to submit, EMA. With vsync
 * off this is the display's real throughput ceiling; the planner spaces
 * targets no denser than it, or most scenes are made only to be superseded. */
int64_t openpete_present_thread_cost_ns(void) { return s_pt.render_ema + s_pt.present_ema; }
