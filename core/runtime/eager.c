// core/runtime/eager.c - eager production: at each canonical tick, plan
// which display instants of the coming span get a scene, produce them on the
// sim thread while it is idle, and publish them into the scene ring for the
// present thread to show on their instants.

#include "eager.h"
#include "host_loop_internal.h"
#include "run_internal.h"

#include "openpete_bugrec_subtick.h"
#include "openpete_discontinuity.h"
#include "openpete_fiber.h"
#include "openpete_interp.h"
#include "openpete_interp_objs.h"
#include "openpete_log.h"
#include "openpete_mods.h"
#include "openpete_native_render.h"
#include "openpete_pace_log.h"
#include "openpete_present_thread.h"
#include "openpete_profile.h"
#include "openpete_time.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef OPENPETE_HAS_PSYCROSS

/* Eager production.
 *
 * The pull model presented one scene at a time, extracting it on the sim
 * thread at the moment the pacer decided it was due -- and refused the
 * slots that fell inside the tick's protective margin, because extracting
 * and executing the guest share this thread. Nothing forces extraction to
 * happen at present time. Every present between tick N and tick N+1
 * interpolates the span [N-1, N], whose both endpoints are already in the
 * interp ring the moment tick N lands; so at that moment the whole span is
 * extractable, and this thread is about to be idle for most of a tick.
 *
 * So: at the canonical tick, PLAN which grid instants of the coming span
 * get a scene; then PRODUCE them one at a time whenever this thread is
 * idle, publishing each into the scene ring for the present thread, which
 * shows them on their instants -- through the next tick's execution. The
 * deadline check sits BETWEEN scenes, never inside one, so the guest never
 * resumes on interpolated RAM: production is scene-atomic. A scene whose
 * instant has already passed is skipped (except the canonical, which also
 * carries the tick boundary for mods).
 *
 * Running out of CPU inverts the failure mode. The pull model found out at
 * the boundary, one present at a time, and lost a contiguous run at the END
 * of the span. Here the budget (span length minus what this thread must
 * still do in it) and the per-scene cost are known before anything is
 * produced, so the shape of the degradation is chosen up front: a divisor
 * ladder (every 2nd slot, every 3rd ...) keeps the cadence uniform on a
 * fixed refresh. The trigger is a division, not a threshold on a curve.
 * Hysteresis is asymmetric -- drop a level the moment a span is cut short,
 * climb only after several consecutive spans with clear headroom -- because
 * a rate that oscillates is worse than a steady lower one.
 *
 * The lag. The spine shows span [N-1, N] from the moment tick N lands, at
 * alpha = (t - stamp_N) / period, so alpha reaches 1 when tick N+1 is DUE --
 * and tick N+1 does not land until it has executed, ~6 ms later. The slots
 * inside that execution sit at alpha 1.0-1.2: they need pose N+1, which does
 * not exist yet. No amount of extraction ahead of time fills them; the
 * timeline itself is one tick-execution too short. So an eager scene for
 * display instant t animates the instant t - lag, where lag tracks the tail
 * of the tick's execution time: every slot through the execution then sits
 * at alpha < 1 of a span whose endpoints are both known, no extrapolation,
 * and the price is a CONSTANT few milliseconds of display latency in place
 * of a hold of the same length thirty times a second.
 *
 * OPENPETE_EAGER=0 disables (on by default). OPENPETE_EAGER_POLICY=greedy
 * swaps the divisor ladder for "as many scenes as fit, evenly spaced across
 * the span" -- the A/B partner for the question whether uniform-on-grid
 * beats more-frames-off-grid. OPENPETE_EAGER_LEVEL=n pins the divisor;
 * OPENPETE_EAGER_LAG_US=n pins the lag. */
#define EAGER_MAX_TARGETS 128
#define EAGER_COST_WARMUP 8
#define EAGER_HEADROOM_RUNS 8
static struct {
    int       active;                 /* the present thread owns the display */
    int       n, i;                   /* planned targets, next to produce */
    op_time_t target[EAGER_MAX_TARGETS];
    int       level;                  /* divisor: produce every level-th slot */
    int       pinned;                 /* OPENPETE_EAGER_LEVEL */
    int       afford_pin;             /* OPENPETE_EAGER_AFFORD: force the budget's verdict (A/B) */
    int       policy;                 /* 0 divisor, 1 greedy */
    int       headroom_runs;
    int       hold_level;             /* the highest level a span of the current headroom run needs */
    op_time_t cost_ema, cost_dev; int cost_n;   /* per-scene production cost */
    op_time_t vbl_ema;                /* cost of a non-drawing VBL on this thread */
    op_time_t tick_ema, tick_dev;     /* execution time of a drawing VBL (the tick) */
    op_time_t tick_late;              /* the last tick's start past its pacer deadline, when a stall */
    int       span_late;              /* the open span was planned after a stalled tick start */
    op_time_t tick_peak;              /* decaying peak of the tick's execution time */
    op_time_t lag;                    /* animation lag behind the display instant */
    op_time_t lag_pin;                /* OPENPETE_EAGER_LAG_US */
    int       lag_headroom;
    int       dbg_level;
    unsigned  trimmed;                /* spans whose render-grid slot was pulled back to within a period of now */
    op_time_t anchor_age;             /* now - the span anchor at the last plan: 0 when anchored on now */
    op_time_t last_target;            /* the previous span's final display instant */
    int       world_live;             /* canonical presents are flowing (see the inline gate) */
    unsigned  spans, produced, skipped_late, cut, refused, level_up, level_down;
    unsigned  produced_inter;         /* of `produced`, the sub-tick re-extracts */
    int       span_cut;               /* this span ended with targets unproduced */
    unsigned  canon_frame;            /* openpete_frame at the last canonical, 0 after a discontinuity */
} s_eager = { .level = 1 };

/* Documented at the declaration in eager.h. */
int eager_active(void) { return s_eager.active; }

/* Documented at the declaration in eager.h. */
int eager_world_live(void) { return s_eager.world_live; }

/* Documented at the declaration in eager.h. */
void eager_set_active(int on)
{
    if (on == s_eager.active) return;
    openpete_present_thread_set_active(on);   /* 0: drains */
    s_eager.active = on;
    OPL_INFO_F("eager", "%s", on ? "engaged" : "disengaged (inline presents)");
}

/* Documented at the declaration in eager.h. */
int eager_hold_done(void)
{
    static int armed = -1;
    static op_time_t until;
    if (armed < 0) {
        const char* e = getenv("OPENPETE_EAGER_HOLD_S");
        armed = (e && *e) ? 1 : 0;
        until = op_now() + (op_time_t)(armed ? atof(e) * 1e9 : 0); /* clock-guard: wall */
        if (armed) OPL_INFO_F("eager", "held off for %.3f s (OPENPETE_EAGER_HOLD_S)", atof(e));
    }
    return !armed || op_now() >= until; /* clock-guard: wall */
}
/** @brief Read the OPENPETE_EAGER_* tuning variables into s_eager, once.
 *
 * OPENPETE_EAGER_POLICY=greedy selects policy 1 (evenly spaced targets),
 * else policy 0 (every level-th grid slot). OPENPETE_EAGER_LEVEL, when
 * nonzero, disables the adaptive level ladder, and a positive value also
 * sets the level. OPENPETE_EAGER_AFFORD pins the per-span scene budget and
 * OPENPETE_EAGER_LAG_US (microseconds) pins the lag. Later calls do nothing. */
static void eager_init(void)
{
    static int inited;
    if (inited) return;
    inited = 1;
    const char *p = getenv("OPENPETE_EAGER_POLICY");
    s_eager.policy = (p && !strcmp(p, "greedy")) ? 1 : 0;
    const char *l = getenv("OPENPETE_EAGER_LEVEL");
    s_eager.pinned = (l && *l) ? atoi(l) : 0;
    if (s_eager.pinned > 0) s_eager.level = s_eager.pinned;
    const char *f = getenv("OPENPETE_EAGER_AFFORD");
    s_eager.afford_pin = (f && *f) ? atoi(f) : 0;
    const char *g = getenv("OPENPETE_EAGER_LAG_US");
    s_eager.lag_pin = (g && *g) ? (op_time_t)atoll(g) * 1000 : 0;
}

/** @brief Return the first instant of the lattice v(n) = base + n * period at or after t.
 *
 * An instant up to a millionth of a period before t counts as at t, so a t
 * already on the lattice does not round up a whole period.
 * @param t Time on the op_now() clock.
 * @param base Time of lattice instant 0.
 * @param period Lattice period in nanoseconds.
 * @return The lattice instant in nanoseconds. */
static op_time_t eager_lattice_ceil(op_time_t t, int64_t base, double period)
{
    const double n = ceil((double)(t - base) / period - 1e-6);
    return base + (op_time_t)(n * period + 0.5);
}

/** @brief Return the tail bound of one eager scene's production cost.
 *
 * The mean plus three deviations of the per-scene production time that
 * eager_produce_one measures. eager_produce stops before a scene whose tail
 * cost would pass its deadline, and eager_plan floors the render-grid
 * period at it when presenting is uncapped.
 * @return The bound in nanoseconds, or 0 before EAGER_COST_WARMUP scenes
 *         have been measured. */
static op_time_t eager_tail_cost(void)
{
    return s_eager.cost_n >= EAGER_COST_WARMUP
        ? s_eager.cost_ema + 3 * s_eager.cost_dev : 0;
}

/* The plan's budget keeps this much of the span unplanned against estimate
 * error; lateness beyond it is lateness the produce guard absorbs. */
#define EAGER_BUDGET_SAFETY_NS 1000000

/* Documented at the declaration in eager.h. */
void eager_note_vbl(op_time_t ex, op_time_t start_late, int drawing)
{
    const int stall = ex >= (op_time_t)g_openpete_tick_period_ns / 2;
    if (!drawing) {
        if (!stall) s_eager.vbl_ema = s_eager.vbl_ema ? (s_eager.vbl_ema * 7 + ex) / 8 : ex;
        return;
    }
    s_eager.tick_late = start_late >= (op_time_t)g_openpete_tick_period_ns / 2 ? start_late : 0;
    if (stall) return;
    if (s_eager.tick_ema) {
        const op_time_t d = ex > s_eager.tick_ema ? ex - s_eager.tick_ema : s_eager.tick_ema - ex;
        s_eager.tick_dev = (s_eager.tick_dev * 7 + d) / 8;
    }
    s_eager.tick_ema = s_eager.tick_ema ? (s_eager.tick_ema * 7 + ex) / 8 : ex;
    if (ex > s_eager.tick_peak) s_eager.tick_peak = ex;
}

/** @brief Record one intermediate scene the eager producer dropped as a SHED record.
 *
 * The record is the one the inline path writes for a present it does not
 * make: kind OP_PACE_INTERMEDIATE, the per-scene tail cost as the predicted
 * cost, and the time left before the scene's deadline as the margin.
 * @param reason PACE_SHED_NO_BUDGET or PACE_SHED_INSTANT_PASSED.
 * @param margin_ns Time left before the deadline; negative once it passed. */
static void eager_shed(uint16_t reason, op_time_t margin_ns)
{
    openpete_pace_shed(OP_PACE_INTERMEDIATE, reason,
                       (int64_t)eager_tail_cost(), (int64_t)margin_ns);
}

/** @brief Close the previous span: count it cut when targets were left unproduced and shed each one.
 *
 * Every target from s_eager.i to s_eager.n is an intermediate scene (the
 * canonical scene, index 0, is produced when its span is planned) that the
 * produce guard held back for the deadline or a full scene ring, and the new
 * plan discards it. Each is written as one PACE_SHED_NO_BUDGET record while
 * the world is live; while it is not, production was withheld for the
 * liveness gate and the display holds the last image, which is not a shed.
 * Sets span_cut for the plan's level ladder. */
static void eager_close_span(void)
{
    if (!s_eager.n || s_eager.i >= s_eager.n) return;
    s_eager.cut++;
    s_eager.span_cut = 1;
    if (!s_eager.world_live) return;
    for (int q = s_eager.i; q < s_eager.n; q++) eager_shed(PACE_SHED_NO_BUDGET, 0);
}

/** @brief Shed the display-grid slots of a new span that the plan leaves without a scene for cost.
 *
 * Two plan verdicts leave a slot of the display grid unproduced because
 * scenes cost more than the span affords: the cost floor that coarsens the
 * grid period to the scene or present-thread cost, and the decimation level
 * (or the greedy policy's afford) that targets fewer slots than the grid
 * holds. Each such slot is the eager form of the inline present the host
 * loop sheds with PACE_SHED_NO_BUDGET, and is written as one. A level pinned
 * by OPENPETE_EAGER_LEVEL under the divisor policy is a configuration, not a
 * cost verdict, and sheds nothing; nor does any slot while the world is not
 * live, when no scene is produced and the display holds the last image.
 * @param first The span's first display instant.
 * @param span_end The span's last admissible display instant.
 * @param grid_period The display grid period before the cost floor.
 * @param period The planned grid period, at least grid_period.
 * @param ns The number of slots enumerated at period. */
static void eager_shed_unplanned(op_time_t first, op_time_t span_end,
                                 op_time_t grid_period, op_time_t period, int ns)
{
    if (!s_eager.world_live) return;
    int grid_slots = ns;
    if (grid_period > 0 && grid_period < period && span_end >= first) {
        const op_time_t g = (span_end - first) / grid_period + 1;
        grid_slots = g < EAGER_MAX_TARGETS ? (int)g : EAGER_MAX_TARGETS;
    }
    const int ladder = s_eager.pinned == 0 || s_eager.policy == 1;
    const int planned = ladder ? s_eager.n : ns;
    for (int q = planned; q < grid_slots; q++) eager_shed(PACE_SHED_NO_BUDGET, 0);
}

/* Room for the canonical scene between its plan and its vblank: production
 * (~0.25 ms), compose (0.3 ms on a light scene, 1.2 ms on a 48k-prim vista)
 * and the driver's own lead to the flip (a submit 0.33 ms before a vblank
 * still made it; one 0.13 ms before did not). */
#define EAGER_PUBLISH_LEAD_NS 2000000
/* The present thread's fitted vblank lattice, when targets go on it. */
typedef struct { int on; int64_t base; double period; } eager_lattice_t;
/* One span's display instants, as eager_span_slots enumerates them. */
typedef struct { op_time_t first, end; int ns; op_time_t slots[EAGER_MAX_TARGETS]; } eager_span_t;

/** @brief Enumerate the display instants of one span from its first render-grid slot.
 *
 * Raises span_end to first, puts first on the vblank lattice (no earlier than
 * EAGER_PUBLISH_LEAD_NS past now) when one is fitted, extends span_end by the
 * lag and, when the previous span's last target plus a period lies past
 * first, continues on the previous span's grid (unless it drifted a whole
 * period from this one). The slots step period from first through span_end;
 * a span holds at least the slot at first. Reads s_eager.lag and
 * s_eager.last_target and writes nothing else.
 * @param now The plan instant.
 * @param first The span's first render-grid slot.
 * @param span_end The instant the next tick lands.
 * @param period The planned grid period.
 * @param lat The vblank lattice.
 * @param out Receives first, the extended span_end, the slot count and the slots. */
static void eager_span_slots(op_time_t now, op_time_t first, op_time_t span_end,
                             op_time_t period, const eager_lattice_t *lat, eager_span_t *out)
{
    if (span_end < first) span_end = first;
    if (lat->on) {
        /* The canonical scene is produced right after this plan and
         * shown at once; its vblank must leave room for that. */
        op_time_t f = now + EAGER_PUBLISH_LEAD_NS;
        if (f < first) f = first;
        first = eager_lattice_ceil(f, lat->base, lat->period);
    }
    /* Display instants whose animation instant (t - lag) is inside the span.
     * The previous span already covers the instants up to its last target
     * (that is what the lag is for: its late scenes show through this tick's
     * execution), so this span begins at the slot after it -- a scene for an
     * instant the old span covers would sit at negative alpha of the new
     * bracket, a backward extrapolation the old bracket renders exactly. */
    /* The extension below moves the SLOT ENUMERATION past the instant
     * production stops at, which is correct - those late instants are shown
     * through the next tick's execution - but it is not time in which to
     * produce them. eager_plan's `prod_end` is that separate quantity. */
    span_end += s_eager.lag;
    if (s_eager.last_target && s_eager.last_target + period > first) {
        op_time_t f = s_eager.last_target + period;
        /* the old span's grid, unless it drifted a whole slot from ours */
        if (f - first > period) f = first + ((f - first) / period) * period;
        /* re-snapped: the lattice fit moves by microseconds between spans */
        if (lat->on) f = eager_lattice_ceil(f - period / 2, lat->base, lat->period);
        first = f;
    }
    int ns = 0;
    for (op_time_t t = first; t <= span_end && ns < EAGER_MAX_TARGETS; t += period) out->slots[ns++] = t;
    if (ns == 0) out->slots[ns++] = first;
    out->first = first; out->end = span_end; out->ns = ns;
}

/** @brief Write eager_plan's OPENPETE_EAGER_DBG lines for the span just planned.
 *
 * OPENPETE_EAGER_DBG=1 writes the plan line on a cut span, a level change, or
 * a tick that started half a tick or more past its deadline (late=).
 * 2 or more also writes one span line per plan: the offsets from now of the
 * render-grid slot passed in (in), the span's first display instant, its end
 * after the lag extension and the previous span's last target, the lag, the
 * anchor source (cap: the newest capture stamp, now: the plan instant) with
 * the capture stamp's age, the slot count, the level and the count of spans
 * whose slot was pulled back. 3 or more also writes the slot list beside the
 * one the untrimmed slot and the caller-side span_end clamp enumerate, and
 * whether the two are equal. Runs before the plan stores its targets, so
 * last_target is still the previous span's.
 * @param now The plan instant.
 * @param first_in The render-grid slot eager_plan was called with.
 * @param end_in The span_end eager_plan was called with.
 * @param period The planned grid period.
 * @param lat The vblank lattice.
 * @param sp The planned span.
 * @param budget The production budget in nanoseconds.
 * @param afford The number of scenes the budget affords. */
static void eager_plan_dbg(op_time_t now, op_time_t first_in, op_time_t end_in, op_time_t period,
                           const eager_lattice_t *lat, const eager_span_t *sp,
                           op_time_t budget, int afford)
{
    static int dbg = -1;
    if (dbg < 0) { const char *e = getenv("OPENPETE_EAGER_DBG"); dbg = (e && *e) ? atoi(e) : 0; }
    if (dbg <= 0) return;
    if (s_eager.span_cut || s_eager.level != s_eager.dbg_level || s_eager.tick_late > 0) {
        fprintf(stderr, "[eager f=%u] plan: slots=%d budget=%.2fms tail=%.3fms afford=%d level=%d cut=%d lag=%.2fms vbl=%.2fms late=%.2fms\n",
                openpete_frame, sp->ns, (double)budget / 1e6, (double)eager_tail_cost() / 1e6, afford,
                s_eager.level, s_eager.span_cut, (double)s_eager.lag / 1e6, (double)s_eager.vbl_ema / 1e6,
                (double)s_eager.tick_late / 1e6);
        s_eager.dbg_level = s_eager.level;
    }
    if (dbg < 2) return;
    const double last = s_eager.last_target ? (double)(s_eager.last_target - now) / 1e6 : 0.0;
    const op_time_t curr = openpete_interp_curr_tick_ns();
    const double age = curr ? (double)(now - curr) / 1e6 : -1.0;
    fprintf(stderr, "[eager f=%u] span: now=%.6f in=%.3f first=%.3f end=%.3f last=%.3f lag=%.3f anchor=%s age=%.3f ns=%d level=%d trimmed=%u\n",
            openpete_frame, (double)now / 1e9, (double)(first_in - now) / 1e6,
            (double)(sp->first - now) / 1e6, (double)(sp->end - now) / 1e6, last,
            (double)s_eager.lag / 1e6, s_eager.anchor_age ? "cap" : "now",
            age, sp->ns, s_eager.level, s_eager.trimmed);
    if (dbg < 3) return;
    eager_span_t old;
    eager_span_slots(now, first_in, end_in > first_in ? end_in : first_in, period, lat, &old);
    const int same = old.ns == sp->ns && old.first == sp->first && old.end == sp->end &&
                     memcmp(old.slots, sp->slots, sizeof(op_time_t) * (size_t)sp->ns) == 0;
    char buf[1024]; int o = snprintf(buf, sizeof(buf), "[eager f=%u] slots: same=%d new", openpete_frame, same);
    for (int q = 0; q < sp->ns && o < 900; q++) o += snprintf(buf + o, sizeof(buf) - (size_t)o, " %.3f", (double)(sp->slots[q] - now) / 1e6);
    o += snprintf(buf + o, sizeof(buf) - (size_t)o, " | old");
    for (int q = 0; q < old.ns && o < 1000; q++) o += snprintf(buf + o, sizeof(buf) - (size_t)o, " %.3f", (double)(old.slots[q] - now) / 1e6);
    fprintf(stderr, "%s\n", buf);
}

/** @brief Return how many scenes the span's production budget affords.
 *
 * The budget runs from now, less the start lateness a stall left the tick
 * (tick_late), to prod_end, less the update VBL's mean cost and
 * EAGER_BUDGET_SAFETY_NS. A stall's lateness is not capacity: the produce
 * guard stops a late span at its deadline, and a budget priced from now
 * would read one load as the machine affording one scene, raising the
 * level to the slot count. The tick's own execution stays in the
 * budget, so a run of long ticks still prices the spans it shortens. Past
 * warmup the scenes are priced on their sum, N times the mean cost plus a
 * spread allowance of eight deviations (two sigma of a sum of sixteen);
 * before it, on the per-scene tail, or one per slot before any cost exists.
 * OPENPETE_EAGER_AFFORD overrides the result, which is at least 1.
 * @param now The plan instant.
 * @param prod_end The pacer grid instant production must finish before.
 * @param ns The span's slot count.
 * @param budget_out Receives the budget in nanoseconds, floored at 0.
 * @return The number of scenes afforded. */
static int eager_afford(op_time_t now, op_time_t prod_end, int ns, op_time_t *budget_out)
{
    /* How many scenes fit is a question about their SUM, not about one of
     * them. `eager_tail_cost()` is a per-scene p99.7 bound, which is the right
     * question between scenes (the produce guard) and the wrong one here:
     * pricing all N at once at p99.7 charges every scene the worst scene's
     * cost, ~2x the true sum, since independent costs average out. The sum's
     * own spread grows as sqrt(N), so bound it that way -- with
     * N <= EAGER_MAX_TARGETS the sqrt term is a constant. Being wrong is also
     * cheap in one direction only: the produce guard still stops at the
     * deadline, and a cut span is observed and answered by the ladder. */
    const op_time_t tail = eager_tail_cost();
    op_time_t budget = prod_end - (now - s_eager.tick_late) - s_eager.vbl_ema - EAGER_BUDGET_SAFETY_NS;
    if (budget < 0) budget = 0;
    *budget_out = budget;
    int afford;
    if (s_eager.cost_n >= EAGER_COST_WARMUP && s_eager.cost_ema > 0) {
        const op_time_t spread = 8 * s_eager.cost_dev;      /* 2 sigma at N = 16 */
        const op_time_t room = budget > spread ? budget - spread : 0;
        afford = (int)(room / s_eager.cost_ema);
    } else {
        afford = tail > 0 ? (int)(budget / tail) : ns;
    }
    if (s_eager.afford_pin > 0) afford = s_eager.afford_pin;   /* a forced budget, for the policy A/B */
    return afford < 1 ? 1 : afford;
}

/** @brief Move the decimation level for a span of ns slots of which afford scenes fit.
 *
 * Fast down, slow up, since an overrun is the harmful direction and a level
 * that flickers judders the rate. The level rises at once to ceil(ns /
 * afford), and by at least one when the previous span was cut short,
 * unless that span was planned after a stalled tick start (span_late): its
 * cut is the stall's, which the produce guard absorbed at the deadline, not
 * a verdict on what the steady span affords. It falls only after
 * EAGER_HEADROOM_RUNS consecutive spans each of which a lower level covers
 * with a quarter margin (afford * 4 >= ceil(ns / level) * 5), and then in one
 * move to the highest level any span of that run needed: the run measured
 * that level, so stepping down one level per run would only spread the same
 * verdict over several runs at a rate the run has already shown too low.
 * @param ns The span's slot count.
 * @param afford The scenes the budget affords, at least 1. */
static void eager_ladder_step(int ns, int afford)
{
    int need = (ns + afford - 1) / afford;      /* ceil(ns / afford) */
    if (need < 1) need = 1;
    if (s_eager.span_cut && !s_eager.span_late && need <= s_eager.level) need = s_eager.level + 1;
    if (need > s_eager.level) {
        s_eager.level = need; s_eager.level_down++;
        s_eager.headroom_runs = 0; s_eager.hold_level = 0;
        return;
    }
    const int m = afford * 4 / 5;                /* scenes afforded with a quarter margin */
    const int cover = m >= 1 ? (ns + m - 1) / m : s_eager.level;
    if (cover >= s_eager.level) { s_eager.headroom_runs = 0; s_eager.hold_level = 0; return; }
    if (cover > s_eager.hold_level) s_eager.hold_level = cover;
    if (++s_eager.headroom_runs < EAGER_HEADROOM_RUNS) return;
    s_eager.level = s_eager.hold_level > 1 ? s_eager.hold_level : 1;
    s_eager.level_up++;
    s_eager.headroom_runs = 0; s_eager.hold_level = 0;
}

/** @brief Move the animation lag toward the tick's execution tail, unless OPENPETE_EAGER_LAG_US pins it.
 *
 * Decays the tick-time peak 1/32 toward the tick mean, then raises the lag
 * at once to the peak plus 0.3 ms, or lowers it to that value only after 30
 * consecutive spans that wanted less. */
static void eager_lag_update(void)
{
    /* The lag follows the tick's execution tail: up at once (a slot at
     * alpha > 1 is a lost frame), down only after a second of headroom (a
     * lag that flickers warps animation time). */
    if (s_eager.lag_pin > 0) s_eager.lag = s_eager.lag_pin;
    else {
        /* The peak over roughly the last second, decaying toward the centre:
         * a tail bound that a rare long tick raises at once and that lets go
         * of it again, unlike a deviation term that a bimodal tick inflates
         * for good. */
        s_eager.tick_peak -= (s_eager.tick_peak - s_eager.tick_ema) / 32;
        const op_time_t want = s_eager.tick_peak + 300000;
        if (want > s_eager.lag) { s_eager.lag = want; s_eager.lag_headroom = 0; }
        else if (++s_eager.lag_headroom >= 30) { s_eager.lag = want; s_eager.lag_headroom = 0; }
    }
}

/** @brief Return the planned grid period: the render period moved onto the display's refresh grid, floored at the production cost.
 *
 * On the present thread's locked vblank lattice the period is a whole
 * number of refreshes; otherwise, on a vsynced display slower than the
 * render grid, a whole number of render slots per refresh. The result never
 * falls below the scene production tail cost (250 us before it is
 * measured) or the present thread's cost.
 * @param period The render grid period in nanoseconds.
 * @param lat The vblank lattice.
 * @param grid_period Receives the display grid period before the cost floor.
 * @return The planned grid period in nanoseconds. */
static op_time_t eager_grid_period(op_time_t period, const eager_lattice_t *lat, op_time_t *grid_period)
{
    const float hz = openpete_native_display_refresh_hz();
    if (lat->on) {
        /* The present thread has fitted the display's vblank lattice
         * from observed flips: targets go ON it, a whole number of
         * refreshes apart, so each target is a vblank and the
         * animation instant (target - lag) is exact. The nominal mode
         * rate is not good enough for this -- it was 2e-5 off the
         * measured one, a full slot of phase every ~50 s. */
        const op_time_t lp = (op_time_t)(lat->period + 0.5);
        op_time_t k = (period + lp / 2) / lp;             /* refreshes per render slot */
        if (k < 1) k = 1;
        period = (op_time_t)(lat->period * (double)k + 0.5);
    } else
    if (hz > 1.0f && openpete_native_display_vsync()) {
        const op_time_t rp = (op_time_t)(1e9 / hz);
        if (rp > period + period / 8) {
            const op_time_t k = (rp + period / 2) / period;   /* whole slots per refresh */
            if (k > 1) period *= k;
        }
    }
    /* Uncapped presenting (vsync off, --render-fps far above the display):
     * targets denser than scenes can be made would plan a span the ring
     * cannot hold and the burst cannot finish. Floor the grid at the
     * production cost; the ring's back-pressure below does the rest, and
     * the present thread shows the newest scene at whatever rate the GPU
     * allows. */
    *grid_period = period;
    op_time_t floor_ns = eager_tail_cost() ? eager_tail_cost() : 250000;
    const op_time_t pc = openpete_present_thread_cost_ns();
    if (pc > floor_ns) floor_ns = pc;
    if (period < floor_ns) period = floor_ns;
    return period;
}

/** @brief Plan the display instants of the span that starts now and adjust the decimation level.
 *
 * Counts the previous span as cut when it left targets unproduced. Pulls
 * first back by whole render periods to the grid's earliest instant at or
 * after now, counting each span it pulls back in s_eager.trimmed. Moves the
 * lag (unless pinned) up to the decaying tick-time peak plus 0.3 ms at once
 * and down only after 30 spans of headroom. Chooses the grid: on the
 * present thread's locked lattice a whole number of refreshes per render
 * slot with the first target on a vblank; otherwise, on a vsynced display
 * slower than the render grid, the refresh period; the period never falls
 * below the scene production cost, the present thread's cost, or 250 us
 * before the cost is measured. Enumerates the slots with eager_span_slots
 * (span_end raised to first and extended by the lag, continuing after the
 * previous span's last target). Estimates how many scenes fit
 * before prod_end (eager_afford) and moves the level (eager_ladder_step),
 * Records whether the span follows a stalled tick start. Stores every
 * level-th slot (policy 0) or afford evenly
 * spaced instants (policy 1) in s_eager.target, sets last_target and counts
 * the span.
 * @param now Current time on the op_now() clock.
 * @param first A render-grid instant at or after now.
 * @param period The render grid period in nanoseconds.
 * @param span_end The instant the next tick lands (alpha reaches 1).
 * @param prod_end The pacer grid instant production must finish before. */
static void eager_plan(op_time_t now, op_time_t first, op_time_t period,
                       op_time_t span_end, op_time_t prod_end)
{
    eager_close_span();   /* the previous span's books first */
    /* Precondition check: under eager the host loop keeps the render grid's
     * next slot within one period of now, so this trims nothing. A slot that
     * led by more would leave this tick's span one slot, or none past the
     * clamp to it; its earliest instant at or after now, on the same phase,
     * is the span's first, and s_eager.trimmed counts the breach. */
    const op_time_t first_in = first;
    if (first - now > period) {
        first -= ((first - now) / period) * period;
        s_eager.trimmed++;
    }
    eager_lag_update();
    /* Under FIFO the display shows at most one image per refresh, and the
     * window may sit on a slower monitor than --render-fps was chosen for
     * (480 on a 60 Hz panel): a scene per grid slot would be produced 8x
     * faster than any can be shown and superseded unpresented. Plan on the
     * coarser of the two grids, phase-preserving. */
    eager_lattice_t lat = { 0, 0, 0.0 };
    op_time_t grid_period;            /* the display grid before the cost floor */
    lat.on = openpete_native_display_vsync() &&
             openpete_present_thread_lattice(&lat.base, &lat.period);
    period = eager_grid_period(period, &lat, &grid_period);
    eager_span_t sp;
    eager_span_slots(now, first, span_end, period, &lat, &sp);
    const int ns = sp.ns;

    /* Budget against what this thread has left of the span once the update
     * VBL has run, and against `prod_end` (the pacer grid instant the produce
     * deadlines are derived from), never the span endpoint. The span is
     * anchored on the capture stamp, which PLL-locks to average tick
     * completion, so it lands about one tick execution past the moment
     * production actually stops; the lag extension above adds more.
     * Budgeting against either prices time this thread never has, so
     * `afford` would exceed the slot count even at a level that cannot keep
     * up, the proactive step would never fire, and the level would fall only
     * after a span was cut short, which is the oscillation the divisor ladder
     * exists to prevent. */
    op_time_t budget;
    const int afford = eager_afford(now, prod_end, ns, &budget);
    if (!s_eager.pinned) eager_ladder_step(ns, afford);
    eager_plan_dbg(now, first_in, span_end, period, &lat, &sp, budget, afford);
    s_eager.span_cut = 0;
    s_eager.span_late = s_eager.tick_late > 0;

    s_eager.n = 0; s_eager.i = 0;
    if (s_eager.policy == 1) {
        int k = afford < ns ? afford : ns;
        const op_time_t len = sp.end - sp.first;
        for (int q = 0; q < k; q++) s_eager.target[s_eager.n++] = sp.first + (len * q) / k;
    } else {
        for (int q = 0; q < ns; q += s_eager.level) s_eager.target[s_eager.n++] = sp.slots[q];
    }
    s_eager.last_target = s_eager.n ? s_eager.target[s_eager.n - 1] : 0;
    eager_shed_unplanned(sp.first, sp.end, grid_period, period, ns);
    s_eager.spans++;
}

/** @brief Extract one scene for display instant t and publish it into the scene ring.
 *
 * Syncs the VRAM mirror (the guest is idle), sets the interpolation present
 * target to the animation instant t - lag, runs the re-extract, flushes the
 * mods' present work and publishes the scene tagged with t, its kind and
 * alpha; a published scene kicks the present thread, a refused one counts
 * as refused. The canonical scene, first of the span, also raises
 * g_openpete_canon_lag_leg for the re-extract. An intermediate scene first
 * spends the OPENPETE_PACE_INJECT_PRESENT_US fault injection (none unless
 * set) inside the measured span. The scene's production time updates the
 * cost mean and deviation that eager_tail_cost reads and is logged as its
 * PRESENT_SPAN.
 * @param t Display instant on the op_now() clock.
 * @param canonical Nonzero for the span's canonical scene. */
static void eager_produce_one(op_time_t t, int canonical)
{
    extern int g_openpete_canon_lag_leg;
    openpete_native_reextract_fn re = openpete_native_get_reextract();
    const op_time_t t0 = op_now();
    if (!canonical) openpete_pace_inject_present();   /* fault injection, off by default */
    uint64_t pt0 = openpete_phase_timers_on() ? openpete_phase_now_us() : 0;
    openpete_native_vram_sync();                 /* guest idle: the mirror cannot tear */
    openpete_interp_set_present_target(t - s_eager.lag);   /* the animation instant this display instant shows */
    if (canonical) g_openpete_canon_lag_leg = 1;
    openpete_native_scene_eager_begin();
    openpete_bugrec_subtick_begin(canonical ? OP_PACE_CANONICAL : OP_PACE_INTERMEDIATE, BUGREC_SUB_EAGER);
    const op_time_t rex0 = op_now();
    re(&g_game.cpu);
    const op_time_t rex_ns = op_now() - rex0;
    openpete_native_scene_sig_report(canonical);
    g_openpete_canon_lag_leg = 0;
    openpete_mods_present_flush(&g_game.cpu, canonical);
    openpete_interp_set_present_target(0);
    const int kind = canonical ? OP_PACE_CANONICAL : OP_PACE_INTERMEDIATE;
    int paired = 0;
    const double alpha = openpete_pace_extract_alpha(&paired);
    const int slot = openpete_native_scene_publish(t, kind, alpha, paired);
    uint32_t scene_id = 0, stamp = 0;
    openpete_native_scene_slot_ids(slot, &scene_id, &stamp);
    openpete_bugrec_subtick_end(alpha, paired, slot, scene_id, stamp, (int64_t)t);
    if (slot >= 0) {
        openpete_present_thread_kick();
        s_eager.produced++;
        if (!canonical) s_eager.produced_inter++;
    }
    else s_eager.refused++;
    if (pt0) openpete_phase_add(OPPT_REX_TOTAL, openpete_phase_now_us() - pt0);
    if (op_prof_on())
        op_prof_scene((uint32_t)openpete_canon_tick,
                      canonical ? OP_PROF_CANONICAL : OP_PROF_INTERMEDIATE);
    const op_time_t span = op_now() - t0;
    openpete_pace_present_span((uint8_t)kind, t0, t0 + span, rex_ns);
    if (s_eager.cost_n) {
        op_time_t d = span > s_eager.cost_ema ? span - s_eager.cost_ema : s_eager.cost_ema - span;
        /* A sample past the bound is one stall, not the width of the cost
         * distribution. Left whole, 8 * cost_dev outgrows the span's budget and
         * decimates the rate until the slow window below forgets it, about two
         * seconds after a single 140 ms scene. The bound moves with the mean, so
         * a real rise in cost is still learned. */
        const op_time_t d_cap = s_eager.cost_ema + 4 * s_eager.cost_dev;
        if (d > d_cap) d = d_cap;
        /* Slower than the mean: the deviation feeds the plan's capacity
         * verdict, and an 8-sample window is half a tick - short enough that a
         * single costly scene moves the level and starts a limit cycle. */
        s_eager.cost_dev = (s_eager.cost_dev * 31 + d) / 32;
    }
    s_eager.cost_ema = s_eager.cost_n ? (s_eager.cost_ema * 7 + span) / 8 : span;
    if (s_eager.cost_n < EAGER_COST_WARMUP) s_eager.cost_n++;
}

/** @brief Produce the span's planned scenes in order while they fit before deadline.
 *
 * The canonical scene, first of the span, is always produced. A later scene
 * is skipped, and recorded as a PACE_SHED_INSTANT_PASSED shed, when its
 * instant is more than half a period past; the walk
 * stops when the scene ring has one free slot or less, or when the tail
 * cost of one more scene would pass the deadline, and resumes from the same
 * scene on the next call. The checks run between scenes only.
 * @param deadline Time by which production must stop.
 * @param period Render-grid period in nanoseconds.
 * @return The number of scenes produced. */
static int eager_produce(op_time_t deadline, op_time_t period)
{
    int made = 0;
    const op_time_t tail = eager_tail_cost();
    while (s_eager.i < s_eager.n) {
        const op_time_t now = op_now();
        const op_time_t t = s_eager.target[s_eager.i];
        const int canonical = s_eager.i == 0;
        if (!canonical && t < now - period / 2) {   /* its instant is gone */
            eager_shed(PACE_SHED_INSTANT_PASSED, t - now);
            s_eager.i++; s_eager.skipped_late++; continue;
        }
        if (!canonical && openpete_native_scene_ring_free() <= 1) break;   /* ring full: resume next iteration */
        if (!canonical && now + tail > deadline) {
            static int dbg = -1;
            if (dbg < 0) { const char *e = getenv("OPENPETE_EAGER_DBG"); dbg = (e && *e && *e != '0'); }
            if (dbg) fprintf(stderr, "[eager f=%u] stop: %d/%d produced, deadline in %.2fms, tail %.3fms\n",
                             openpete_frame, s_eager.i, s_eager.n, (double)(deadline - now) / 1e6, (double)tail / 1e6);
            break;
        }
        eager_produce_one(t, canonical);
        s_eager.i++; made++;
    }
    return made;
}

/** @brief Measure the VBLs since the previous canonical present.
 *
 * A guest discontinuity zeroes canon_frame, so the first canonical after one
 * measures an unknown gap of 0.
 * @return The gap in VBLs, 0 when unknown. */
static unsigned eager_canon_gap(void)
{
    const unsigned f = (unsigned)openpete_frame;
    const unsigned gap = s_eager.canon_frame && f > s_eager.canon_frame ? f - s_eager.canon_frame : 0;
    s_eager.canon_frame = f;
    return gap;
}

/* Documented at the declaration in eager.h. */
void eager_canonical(op_time_t now, op_time_t slot, op_time_t period,
                     op_time_t prod_end, op_time_t produce_end, int world_live)
{
    const op_time_t curr = openpete_interp_curr_tick_ns();
    op_time_t first = slot;
    while (first < now) first += period;    /* the next slot not yet gone */
    /* The span is anchored on the newest capture's stamp - unless
     * that stamp is stale. Captures dedupe when the interpolated
     * regions do not change (title screen, a cutscene holding
     * the camera), and a span anchored on a stamp a tick or
     * more old collapses to one or two slots while the inline
     * path keeps presenting the live scene at full rate. Anchor
     * on this tick's landing instead. */
    const op_time_t tp = (op_time_t)g_openpete_tick_period_ns;
    op_time_t anchor = (curr && now - curr < 2 * tp) ? curr : now;
    op_time_t span_end = anchor + tp;
    /* A canonical every VBL (the pause window's grow and shrink): the next
     * canonical lands one VBL on, not one tick, and the interp ring's stamp
     * lags it, since the ring commits at most every second VBL on a state the
     * 2D does not live in. A span of one tick from that stamp alternates a
     * scene run past the next canonical, drawn from the 2D carry that canonical
     * replaces, with a span of one slot. The span is this VBL's stamp plus one
     * VBL, over which the 2D carry's alpha runs 0 to 1. Its budget is one VBL
     * less the tick's mean cost, since there is no update VBL to produce
     * through; it is priced from now rather than from the next deadline, so a
     * tick that started late, which the produce guard absorbs, is not read as
     * the capacity of every span of the run. */
    if (eager_canon_gap() == 1) {
        const op_time_t tick = openpete_interp_tick_stamp();
        const op_time_t vbl_anchor = (tick && now - tick < tp) ? tick : now;
        anchor = vbl_anchor;
        span_end = vbl_anchor + tp / 2;
        prod_end = now + tp / 2 - s_eager.tick_ema;
    }
    s_eager.anchor_age = now - anchor;
    eager_plan(now, first, period, span_end, prod_end);
    /* The canonical scene, then as much of the span as fits
     * before the update VBL is due - the latter only while
     * the world is live. */
    s_eager.world_live = world_live;
    eager_produce(world_live ? produce_end - 500000 : now, period);
}

/* Documented at the declaration in eager.h. */
int eager_idle(int world_live, int may_produce, op_time_t deadline, op_time_t period)
{
    /* While canonical presents are not flowing (CD load, level transition,
     * a long producer stall) the guest is rewriting RAM and a re-extract
     * would read it mid-overwrite: the display holds the last image and no
     * scene is produced. */
    s_eager.world_live = world_live;
    if (!(may_produce && world_live)) return 0;
    eager_produce(deadline, period);
    return 1;
}

/** @brief Log the eager-production totals at shutdown.
 *
 * One `eager` info line: spans, scenes produced, skipped because their
 * instant passed, refused because the ring was full, and cut short; the
 * divisor level and its moves (shown as n/a under the greedy policy, where
 * the level does not decide the plan); scene cost mean and deviation; the
 * lag and the tick execution-time mean and deviation; the policy; the spans
 * whose render-grid slot was pulled back to within a period of now. Silent
 * when no span ran. */
static void eager_report(void)
{
    if (!s_eager.spans) return;
    /* Under the greedy policy the divisor is bookkeeping only (the plan
     * comes from `afford`), so it is reported as such. */
    OPL_INFO_F("eager", "%u span(s): %u scene(s) produced, %u skipped (instant passed), "
               "%u refused (ring full), %u cut short; level now %s%d (%u down, %u up); "
               "scene cost %.3f ms ema, %.3f ms dev; lag %.3f ms (tick %.3f ms ema, %.3f dev); policy %s; "
               "%u first slot(s) pulled back",
               s_eager.spans, s_eager.produced, s_eager.skipped_late, s_eager.refused,
               s_eager.cut, s_eager.policy ? "n/a " : "", s_eager.level, s_eager.level_down, s_eager.level_up,
               (double)s_eager.cost_ema / 1e6, (double)s_eager.cost_dev / 1e6,
               (double)s_eager.lag / 1e6, (double)s_eager.tick_ema / 1e6, (double)s_eager.tick_dev / 1e6,
               s_eager.policy ? "greedy" : "divisor", s_eager.trimmed);
}

/** @brief Drop the eager scene plan on a guest discontinuity (savestate load, rewind, level load).
 *
 * Clears the planned targets, the last target and the canonical gap's origin,
 * and resets the decaying tick-time peak to the tick mean. The scene ring flushes its own pending
 * scenes. Registered with openpete_on_guest_discontinuity. */
static void eager_on_discontinuity(void) { s_eager.n = s_eager.i = 0; s_eager.last_target = 0; s_eager.tick_peak = s_eager.tick_ema; s_eager.canon_frame = 0; }

/* The run's frame-time log, for presents made on the present thread; NULL
 * until eager production starts. */
static openpete_frame_time_log_t* s_eager_ftl;
/** @brief Record a present made on the present thread in the profiler trace and the frame-time log.
 *
 * Runs on the present thread after each present that reached the display.
 * The profiler's PRESENT record carries the kind and openpete_canon_tick at
 * the call. The frame-time log record carries openpete_frame at the call.
 * @param ctx Unused.
 * @param kind OP_PACE_CANONICAL, OP_PACE_INTERMEDIATE or OP_PACE_REPEAT.
 * @param alpha Unused.
 * @param paired Unused. */
static void eager_present_cb(void *ctx, int kind, double alpha, int paired)
{
    (void)ctx; (void)alpha; (void)paired;
    present_record_shown(s_eager_ftl, kind);
}

/* Documented at the declaration in eager.h. */
void host_eager_start(openpete_frame_time_log_t* ftl, const openpete_options_t* opts)
{
    if (!(openpete_native_scene_enabled() && !opts->headless && openpete_present_thread_start()))
        return;
    eager_init();
    s_eager_ftl = ftl;
    openpete_present_thread_set_callback(eager_present_cb, NULL);
    openpete_on_guest_discontinuity(eager_on_discontinuity);
    openpete_on_guest_discontinuity(openpete_native_scene_ring_flush);
}

/* Documented at the declaration in eager.h. */
void eager_exit_report(unsigned inline_rex)
{
    eager_report();
    if (s_eager.produced_inter || inline_rex)
        OPL_INFO_F("sub-tick", "%u re-extract(s) presented: %u eager, %u inline",
                   s_eager.produced_inter + inline_rex,
                   s_eager.produced_inter, inline_rex);
}

#endif /* OPENPETE_HAS_PSYCROSS */
