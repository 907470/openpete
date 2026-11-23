/* harness.c - the present-thread interleaving harness: the real present
 * thread (present_thread.c), the real scene ring (copied from
 * native_render.c) and the real RHI present path (rhi_vulkan.c) over the
 * fake Vulkan device, run under the deterministic scheduler.
 *
 * The scheduled thread 0 is the sim thread: a driver script of the calls the
 * engine's clients make (eager's publish and kick, eager_set_active, the
 * borrow-and-return brackets, the guest-discontinuity flush, inline presents
 * through the compose, stop). Properties are checked at every step through
 * the scheduler's step hook, at the fake device's calls and presents, at
 * each drain's return and at quiescence. Every schedule runs in a forked
 * child of a process that created the device once; seam-site hits and
 * property evaluations go to counters in shared memory, from which the run
 * reports engagement and refuses a site never reached or a property never
 * evaluated. */
#define _GNU_SOURCE
#include "volk.h"
#include "op_sched.h"
#include "fakevk.h"
#include "pt_harness.h"
#include "ring_types.h"
#include "openpete_present_thread.h"
#include "openpete_native_render.h"
#include "openpete_pace_log.h"

#include <errno.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* rhi_vulkan.c's display owner, made global by the build: only its address is used. */
extern char s_display_owner[];

/* ---------------------------------------------------------------- counters */

/** The properties. */
typedef enum { P_OWNER, P_CLAIM, P_ACQUIRE, P_PRESENT, P_RETIRE, P_ACCOUNT, P_DRAIN, P_SERVE, P_COUNT } pt_prop;
static const char *const k_prop_names[P_COUNT] = {
    "P-OWNER", "P-CLAIM", "P-ACQUIRE", "P-PRESENT", "P-RETIRE", "P-ACCOUNT", "P-DRAIN", "P-SERVE",
};
static const char *const k_site_names[PT_SITE_COUNT] = {
    "pt: lock s_pt.mx", "pt: unlock s_pt.mx", "pt: condition wait (cv, idle_cv)", "pt: condition signal",
    "pt: thread join", "pt: clock op_now", "pt: sleep op_sleep_until", "pt: unlocked s_pt.active read",
    "pt: rewarm atomic", "claim at activation", "claim at drain", "stage acquire (present thread)",
    "render to stage", "unstaged render", "stage present", "stage release at drain", "flip wait",
    "window size (compose)", "window size (RHI)", "ring lock", "ring unlocked access (owned)",
    "vkAcquireNextImageKHR", "vkWaitForFences", "vkGetFenceStatus", "vkQueueSubmit", "vkQueuePresentKHR",
    "swapchain rebuild", "SDL_GetTicksNS", "display owner atomic", "display-changed atomic",
    "pt state unlocked access (owned)",
};
/* Sites the suite need not reach: unlocked accesses to the present thread's
 * state, which the census classes owned (the filter yields on them only when
 * the classification is wrong). */
static const pt_site k_site_optional[] = { PT_SITE_STATE_UNLOCKED };

/** Suite-wide counters, in memory shared by every process of a run. */
typedef struct pt_counters {
    unsigned long site[PT_SITE_COUNT];
    unsigned long prop_eval[P_COUNT];
    unsigned long drains, drains_multi_bracket;
} pt_counters;
static pt_counters *g_ctr;

void pt_site_hit(pt_site site) { if (g_ctr && site < PT_SITE_COUNT) __atomic_add_fetch(&g_ctr->site[site], 1, __ATOMIC_RELAXED); }
/** @brief Count one evaluation of a property.
 * @param p The property. */
static void prop_eval(pt_prop p) { if (g_ctr) __atomic_add_fetch(&g_ctr->prop_eval[p], 1, __ATOMIC_RELAXED); }

/* ------------------------------------------------------- schedule state */

enum { MAX_SCENES = 1024, MAX_THREADS = 32, MAX_SHOWN = 2048, MAX_INLINE = 256 };

/** Where a published scene ended up. */
typedef enum {
    B_NONE, B_PENDING, B_PICKED, B_RENDERED, B_STAGED, B_LIMBO,
    B_SUPERSEDED, B_REFUSED_RENDER, B_PRESENTED, B_PRESENTED_DIRECT, B_REFUSED_PRESENT, B_DISCARDED, B_DROPPED,
} pt_bin;
static const char *const k_bin_names[] = {
    "none", "pending", "picked", "rendered", "staged", "leaving-stage", "superseded", "refused-at-render",
    "presented", "presented-unstaged", "refused-at-present", "discarded", "dropped-late",
};

static struct {
    int pt_tid;                                   /* present thread's scheduler id, -1 before start */
    struct { int state; uint32_t id; uint64_t serial; } shadow[SCENE_RING];
    struct { int bin; int64_t target; int stage; int shown; } scene[MAX_SCENES];
    uint32_t max_id;
    int stage_last[8];                            /* scene last rendered into each stage image */
    int staged_shadow; uint32_t staged_id, limbo_id;
    unsigned presents, refused, discarded, drops; /* s_pt counters at the last step */
    unsigned direct_presents;
    int64_t shown_instant[MAX_SHOWN]; int nshown;
    struct { uint32_t tag; int64_t instant; } inl[MAX_INLINE]; int ninl;
    uint32_t inline_next;
    int claimed[MAX_THREADS]; unsigned claim_act[MAX_THREADS], claim_drains[MAX_THREADS];
    int draining; int64_t drain_t0, drain_busy_ns, drain_call0, drain_adv0; unsigned drain_brackets;
    int64_t prev_now; int prev_busy;
    int64_t serve_t0, serve_call0, serve_adv0; unsigned serve_picks0, serve_acq_refused0; unsigned long serve_refusals0;
    unsigned pt_cb_calls;
    int named_ring;
} g;

/* ------------------------------------------------------ step-hook checks */

/** @brief Fail the schedule naming a property.
 * @param p The property.
 * @param fmt printf format of the detail. */
static void prop_fail(pt_prop p, const char *fmt, ...) __attribute__((format(printf, 2, 3), noreturn));
static void prop_fail(pt_prop p, const char *fmt, ...)
{
    char msg[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    sched_fail("%s: %s", k_prop_names[p], msg);
}

/** @brief Return the scheduler id of the present thread, or -1 before it exists.
 * @return The id. */
static int pt_tid(void) { return s_pt.thr ? (int)(intptr_t)s_pt.thr - 1 : -1; }

/** One classification rule: operations of the kinds in a mask whose object lies in a range count for a site. */
typedef struct site_rule { unsigned kinds; const void *lo; size_t len; pt_site site; } site_rule;
#define K(k) (1u << (k))
#define K_ACCESS (K(SCHED_OP_ATOMIC) | K(SCHED_OP_READ) | K(SCHED_OP_WRITE))

/** @brief Build the classification rules from the objects that exist now; first match wins.
 * @param r Receives the rules (at least 16).
 * @return Their count. */
static int site_rules(site_rule *r)
{
    int n = 0;
    const void *mx = s_pt.mx, *cv = s_pt.cv, *icv = s_pt.idle_cv;
    if (mx) {
        r[n++] = (site_rule){ K(SCHED_OP_LOCK), mx, 1, PT_SITE_PT_LOCK };
        r[n++] = (site_rule){ K(SCHED_OP_UNLOCK), mx, 1, PT_SITE_PT_UNLOCK };
    }
    for (int k = 0; k < 2; k++) {
        const void *c = k ? icv : cv;
        if (!c) continue;
        r[n++] = (site_rule){ K(SCHED_OP_COND_WAIT), c, 1, PT_SITE_PT_COND_WAIT };
        r[n++] = (site_rule){ K(SCHED_OP_SIGNAL), c, 1, PT_SITE_PT_SIGNAL };
    }
    if (s_ring_mx) r[n++] = (site_rule){ K(SCHED_OP_LOCK), s_ring_mx, 1, PT_SITE_RING_LOCK };
    r[n++] = (site_rule){ K(SCHED_OP_JOIN), NULL, SIZE_MAX, PT_SITE_PT_JOIN };
    r[n++] = (site_rule){ K(SCHED_OP_CLOCK), pt_seam_op_now, 1, PT_SITE_PT_CLOCK };
    r[n++] = (site_rule){ K(SCHED_OP_CLOCK), NULL, 1, PT_SITE_SDL_TICKS };
    r[n++] = (site_rule){ K_ACCESS, &s_pt.active, sizeof s_pt.active, PT_SITE_PT_ACTIVE_UNLOCKED };
    r[n++] = (site_rule){ K_ACCESS, &s_pt.rewarm, sizeof s_pt.rewarm, PT_SITE_PT_REWARM };
    r[n++] = (site_rule){ K_ACCESS, s_display_owner, sizeof(SDL_ThreadID), PT_SITE_DISPLAY_OWNER };
    r[n++] = (site_rule){ K_ACCESS, fakevk_display_changed_addr(), 1, PT_SITE_DISPLAY_CHANGED };
    r[n++] = (site_rule){ K_ACCESS, s_ring, sizeof s_ring, PT_SITE_RING_UNLOCKED };
    r[n++] = (site_rule){ K_ACCESS, &s_ring_census, sizeof s_ring_census, PT_SITE_RING_UNLOCKED };
    r[n++] = (site_rule){ K_ACCESS, &s_ring_head, sizeof s_ring_head, PT_SITE_RING_UNLOCKED };
    r[n++] = (site_rule){ K_ACCESS, &s_pt, sizeof s_pt, PT_SITE_STATE_UNLOCKED };
    r[n++] = (site_rule){ K_ACCESS, &s_staged, sizeof s_staged, PT_SITE_STATE_UNLOCKED };
    r[n++] = (site_rule){ K_ACCESS, &s_lat, sizeof s_lat, PT_SITE_STATE_UNLOCKED };
    return n;
}

/** @brief Count the seam site of a yield by the first rule matching its operation and object.
 * @param kind The pending operation.
 * @param obj The operation's object. */
static void classify(sched_op_kind kind, const void *obj)
{
    site_rule r[32];
    const int n = site_rules(r);
    const uintptr_t a = (uintptr_t)obj;
    for (int i = 0; i < n; i++) {
        if (!(r[i].kinds & K(kind))) continue;
        const uintptr_t lo = (uintptr_t)r[i].lo;
        if (a >= lo && a - lo < r[i].len) { pt_site_hit(r[i].site); return; }
    }
}

/** @brief Set a scene's bin, checking the transition comes from an expected bin.
 * @param id Scene id.
 * @param from Expected current bin, or B_NONE for any.
 * @param to The new bin. */
static void bin_move(uint32_t id, pt_bin from, pt_bin to)
{
    if (id == 0 || id >= MAX_SCENES) prop_fail(P_ACCOUNT, "scene id %u out of the harness's range", id);
    if (from != B_NONE && g.scene[id].bin != (int)from)
        prop_fail(P_ACCOUNT, "scene %u moved to %s from %s, expected from %s", id, k_bin_names[to],
                  k_bin_names[g.scene[id].bin], k_bin_names[from]);
    g.scene[id].bin = to;
}

/** @brief A slot went FREE -> PENDING: a publish; the slot's last frame must have retired.
 * @param i The slot.
 * @param from_id Its previous scene. */
static void ring_published(int i, uint32_t from_id)
{
    (void)from_id;
    const uint32_t id = s_ring[i].scene_id;
    if (g.shadow[i].serial && fakevk_serial_complete(g.shadow[i].serial) == 0)
        prop_fail(P_RETIRE, "scene %u published into slot %d while frame serial %llu that read it still executes",
                  id, i, (unsigned long long)g.shadow[i].serial);
    bin_move(id, B_NONE, B_PENDING);
    g.scene[id].target = s_ring[i].target_ns;
    g.scene[id].stage = -2;
    if (id > g.max_id) g.max_id = id;
}
/** @brief A slot went PENDING -> FREE: superseded at a pick or flushed.
 * @param i The slot.
 * @param from_id Its scene. */
static void ring_superseded(int i, uint32_t from_id) { (void)i; bin_move(from_id, B_PENDING, B_SUPERSEDED); }
/** @brief A slot went PENDING -> PRESENTING: picked.
 * @param i The slot.
 * @param from_id Its scene. */
static void ring_picked(int i, uint32_t from_id) { (void)i; bin_move(from_id, B_PENDING, B_PICKED); }
/** @brief A slot went PRESENTING -> FREE: the render was refused.
 * @param i The slot.
 * @param from_id Its scene. */
static void ring_refused(int i, uint32_t from_id) { (void)i; bin_move(from_id, B_PICKED, B_REFUSED_RENDER); }
/** @brief A slot went PRESENTING -> PRESENTED: rendered, to a stage image or straight to the swapchain.
 * @param i The slot.
 * @param from_id Its scene. */
static void ring_rendered(int i, uint32_t from_id)
{
    g.shadow[i].serial = s_ring[i].presented_serial;
    const int stage = g.scene[from_id].stage;
    if (stage < 0) { bin_move(from_id, B_PICKED, B_PRESENTED_DIRECT); g.direct_presents++; return; }
    bin_move(from_id, B_PICKED, B_RENDERED);
    if (stage < 8) g.stage_last[stage] = (int)from_id;
}
/** @brief A slot went PRESENTED -> FREE: retired; the fake GPU must have completed its frame.
 * @param i The slot.
 * @param from_id Its scene. */
static void ring_retired(int i, uint32_t from_id)
{
    if (fakevk_serial_complete(g.shadow[i].serial) == 0)
        prop_fail(P_RETIRE, "slot %d (scene %u) freed while frame serial %llu still executes on the GPU",
                  i, from_id, (unsigned long long)g.shadow[i].serial);
}

/** The slot lifecycle's transitions, [from][to]; NULL is a transition the ring never makes. */
static void (*const k_ring_step[4][4])(int, uint32_t) = {
    [SS_FREE]       = { [SS_PENDING] = ring_published },
    [SS_PENDING]    = { [SS_FREE] = ring_superseded, [SS_PRESENTING] = ring_picked },
    [SS_PRESENTING] = { [SS_FREE] = ring_refused, [SS_PRESENTED] = ring_rendered },
    [SS_PRESENTED]  = { [SS_FREE] = ring_retired },
};

/** @brief Follow every ring slot's transition since the last step: account scenes, and check retirement against the fake GPU. */
static void track_ring(void)
{
    prop_eval(P_RETIRE);
    for (int i = 0; i < SCENE_RING; i++) {
        const int st = s_ring[i].state, from = g.shadow[i].state;
        const uint32_t id = s_ring[i].scene_id;
        if (st == from && (st == SS_FREE || id == g.shadow[i].id)) continue;
        if (st < 0 || st > SS_PRESENTED || !k_ring_step[from][st] || id >= MAX_SCENES)
            sched_fail("harness: ring slot %d moved %d -> %d (scene %u -> %u) between two steps", i, from, st,
                       g.shadow[i].id, id);
        k_ring_step[from][st](i, from == SS_FREE ? id : g.shadow[i].id);
        g.shadow[i].state = st;
        g.shadow[i].id = id;
    }
}

/** @brief Attribute the present thread's counter changes since the last step to the scene leaving the stage, if any. */
static void track_staged(void)
{
    prop_eval(P_ACCOUNT);
    const int v = s_staged.valid;
    if (v != g.staged_shadow) {
        if (v) {
            const int st = s_staged.stage;
            const int id = st >= 0 && st < 8 ? g.stage_last[st] : 0;
            if (id <= 0) prop_fail(P_ACCOUNT, "a frame was staged in stage %d, which holds no rendered scene", st);
            bin_move((uint32_t)id, B_RENDERED, B_STAGED);
            g.staged_id = (uint32_t)id;
        } else {
            if (g.limbo_id) prop_fail(P_ACCOUNT, "scene %u left the stage before scene %u was accounted", g.staged_id, g.limbo_id);
            bin_move(g.staged_id, B_STAGED, B_LIMBO);
            g.limbo_id = g.staged_id;
            g.staged_id = 0;
        }
        g.staged_shadow = v;
    }
    const struct { unsigned now, *last; pt_bin bin; } c[4] = {
        { s_pt.discarded, &g.discarded, B_DISCARDED }, { s_pt.late_drops, &g.drops, B_DROPPED },
        { s_pt.refused, &g.refused, B_REFUSED_PRESENT }, { s_pt.presents, &g.presents, B_PRESENTED },
    };
    for (int k = 0; k < 4; k++) {
        while (*c[k].last < c[k].now) {
            (*c[k].last)++;
            if (g.limbo_id) { bin_move(g.limbo_id, B_LIMBO, c[k].bin); g.limbo_id = 0; continue; }
            if (c[k].bin == B_PRESENTED && g.direct_presents > 0) { g.direct_presents--; continue; }
            prop_fail(P_ACCOUNT, "present thread counted a %s with no staged scene leaving the stage", k_bin_names[c[k].bin]);
        }
    }
}

/** @brief Accumulate the present thread's busy time while a drain is in progress. */
static void track_drain(void)
{
    const int64_t now = sched_now();
    if (g.draining) {
        if (g.prev_busy) g.drain_busy_ns += now - g.prev_now;
        if (g.prev_busy && !s_pt.busy) g.drain_brackets++;
    }
    g.prev_now = now;
    g.prev_busy = s_pt.busy;
}

/** @brief Report whether any ring slot holds a pending scene.
 * @return 1 when one does. */
static int ring_pending(void)
{
    for (int i = 0; i < SCENE_RING; i++)
        if (s_ring[i].state == SS_PENDING) return 1;
    return 0;
}

/** P-SERVE's bound past the idle wait: one render and present of the fake device at its default costs. */
#define PT_SERVE_IDLE_NS 4000000
#define PT_SERVE_SLACK_NS 1000000

/** @brief Time how long a pending scene waits for a pick while the present thread should serve it. */
static void track_serve(void)
{
    prop_eval(P_SERVE);
    const int tid = g.pt_tid;
    const int c = s_pt.thr && s_pt.active && !s_pt.quit && !g.draining && !s_staged.valid &&
                  fakevk_presentable() && ring_pending();
    const unsigned picks = atomic_load_explicit(&s_pt.picks, memory_order_relaxed);
    /* A refused acquire (the device's, or the RHI's while a rebuild is pending or settling)
     * restarts the window: in acquire-first order no pick follows a refused acquire, by design. */
    if (!c || g.serve_t0 < 0 || picks != g.serve_picks0 || fakevk_acquire_refusals() != g.serve_refusals0 ||
        s_pt.acq_first_refused != g.serve_acq_refused0) {
        g.serve_t0 = -1;
        if (!c) return;
        g.serve_t0 = sched_now();
        g.serve_picks0 = picks;
        g.serve_refusals0 = fakevk_acquire_refusals();
        g.serve_acq_refused0 = s_pt.acq_first_refused;
        g.serve_call0 = fakevk_thread_call_ns(tid);
        g.serve_adv0 = sched_advanced_ns();
        return;
    }
    const int64_t waited = sched_now() - g.serve_t0 - (sched_advanced_ns() - g.serve_adv0)
                         - (fakevk_thread_call_ns(tid) - g.serve_call0);
    if (waited > PT_SERVE_IDLE_NS + PT_SERVE_SLACK_NS)
        prop_fail(P_SERVE, "a pending scene went unpicked for %.3f ms with the present thread active and nothing staged",
                  (double)waited / 1e6);
}

/** @brief The scheduler's step hook: count the seam site, then check every per-step property.
 * @param arg Unused.
 * @param thr The yielding thread.
 * @param kind Its pending operation.
 * @param obj The operation's object.
 * @param ra Its code address. */
static void step_hook(void *arg, int thr, sched_op_kind kind, const void *obj, const void *ra)
{
    (void)arg; (void)ra; (void)thr;
    if (g.pt_tid < 0) g.pt_tid = pt_tid();
    if (!g.named_ring && s_ring_mx) { sched_name(s_ring_mx, "ring_mx"); g.named_ring = 1; }
    if (kind < SCHED_OP_KINDS) classify(kind, obj);
    prop_eval(P_OWNER);
    if (rhi_seam_violations()) prop_fail(P_OWNER, "rhi_seam_violations() = %u", rhi_seam_violations());
    track_ring();
    track_staged();
    track_drain();
    track_serve();
}

/* ------------------------------------------------- device-side checks */

/** @brief Fake device hook at every modelled call: count its site and check P-CLAIM.
 * @param name Entry point.
 * @param obj Its object. */
static void on_call(const char *name, const void *obj)
{
    (void)obj;
    const int me = sched_self();
    static const struct { const char *n; pt_site s; } k[] = {
        { "vkAcquireNextImageKHR", PT_SITE_VK_ACQUIRE }, { "vkWaitForFences", PT_SITE_VK_FENCE_WAIT },
        { "vkGetFenceStatus", PT_SITE_VK_FENCE_POLL }, { "vkQueueSubmit", PT_SITE_VK_SUBMIT },
        { "vkQueuePresentKHR", PT_SITE_VK_PRESENT }, { "vkWaitForPresentKHR", PT_SITE_FLIP_WAIT },
        { "vkCreateSwapchainKHR", PT_SITE_VK_REBUILD },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (!strcmp(name, k[i].n)) pt_site_hit(k[i].s);
    if (me == g.pt_tid && !strcmp(name, "vkAcquireNextImageKHR")) pt_site_hit(PT_SITE_STAGE_ACQUIRE);
    prop_eval(P_CLAIM);
    if (me < 0 || me >= MAX_THREADS) return;
    if (me == g.pt_tid) {
        if (!g.claimed[me] || g.claim_act[me] != s_pt.activation)
            prop_fail(P_CLAIM, "present thread calls %s in activation %u; its last claim %s activation %u",
                      name, s_pt.activation, g.claimed[me] ? "served" : "(none) served", g.claim_act[me]);
    } else if (me == 0) {
        if (!g.claimed[0] || g.claim_drains[0] != s_pt.drains)
            prop_fail(P_CLAIM, "sim thread calls %s after drain %u; its last claim followed drain %u",
                      name, s_pt.drains, g.claim_drains[0]);
    }
}

/** @brief Return the display instant of a present's content: a ring scene's target, or an inline present's instant.
 * @param content Scene id or inline tag.
 * @param found Receives 1 when the content is known.
 * @return The instant. */
static int64_t content_instant(uint32_t content, int *found)
{
    *found = 1;
    if (content & 0x80000000u) {
        for (int i = 0; i < g.ninl; i++) if (g.inl[i].tag == content) return g.inl[i].instant;
    } else if (content > 0 && content < MAX_SCENES && g.scene[content].bin != B_NONE) {
        return g.scene[content].target;
    }
    *found = 0;
    return 0;
}

/** @brief Fake device hook at every present: P-ACQUIRE's foreign-acquire clause and P-PRESENT.
 * @param sh What the present showed. */
static void on_shown(const fakevk_shown *sh)
{
    prop_eval(P_ACQUIRE);
    prop_eval(P_PRESENT);
    if (!sh->fresh) {
        if (sh->presenter == 0) pt_site_hit(PT_SITE_STAGE_RELEASE);
        return;
    }
    if (sh->presenter != sh->acquirer)
        prop_fail(P_ACQUIRE, "an image acquired by T%d was presented with new content by T%d", sh->acquirer, sh->presenter);
    if (sh->presenter == g.pt_tid) pt_site_hit(PT_SITE_STAGE_PRESENT);
    if (sh->result != VK_SUCCESS && sh->result != VK_SUBOPTIMAL_KHR) return;
    int found = 0;
    const int64_t at = content_instant(sh->content, &found);
    if (!found) return;
    for (int i = 0; i < g.nshown; i++)
        if (g.shown_instant[i] == at)
            prop_fail(P_PRESENT, "instant %lld presented twice (the second by T%d, content %#x)",
                      (long long)at, sh->presenter, sh->content);
    if (g.nshown < MAX_SHOWN) g.shown_instant[g.nshown++] = at;
    if (!(sh->content & 0x80000000u)) g.scene[sh->content].shown = 1;
}

void pt_note_compose(uint32_t content, int stage)
{
    if (content & 0x80000000u) return;
    if (content > 0 && content < MAX_SCENES) g.scene[content].stage = stage;
    pt_site_hit(stage >= 0 ? PT_SITE_RENDER_STAGED : PT_SITE_RENDER_UNSTAGED);
}

void pt_note_sleep(void) { if (sched_self() == g.pt_tid) pt_site_hit(PT_SITE_PT_SLEEP); }

void __real_rhi_display_claim(rhi_device *dev);
void __wrap_rhi_display_claim(rhi_device *dev);
/** @brief Record each claim of the display (thread, activation and drain it follows), then claim.
 * @param dev The device. */
void __wrap_rhi_display_claim(rhi_device *dev)
{
    const int me = sched_self();
    if (sched_active() && me >= 0 && me < MAX_THREADS) {
        g.claimed[me] = 1;
        g.claim_act[me] = s_pt.activation;
        g.claim_drains[me] = s_pt.drains;
        if (me == g.pt_tid) pt_site_hit(PT_SITE_CLAIM_ACTIVATION);
        else if (s_pt.drains) pt_site_hit(PT_SITE_CLAIM_DRAIN);
    }
    __real_rhi_display_claim(dev);
}

/* ------------------------------------------------------------- driver */

/** @brief The present thread's callback after each present (eager_present_cb's slot); counts only.
 * @param ctx Unused.
 * @param kind Scene kind.
 * @param alpha Sub-tick alpha.
 * @param paired Paired flag. */
static void pt_present_cb(void *ctx, int kind, double alpha, int paired)
{
    (void)ctx; (void)kind; (void)alpha; (void)paired;
    g.pt_cb_calls++;
}

/** @brief Sleep the sim thread for a duration of virtual time (a tick's work).
 * @param ns Duration. */
static void drv_sleep(int64_t ns) { sched_sleep_until(sched_now() + ns, __builtin_return_address(0)); }

/** @brief Start the session as host_session and host_eager_start do: claim, start the present thread, set its callback, engage. */
static void drv_begin(void)
{
    openpete_native_display_claim();
    if (!openpete_present_thread_start()) sched_fail("harness: openpete_present_thread_start failed");
    openpete_present_thread_set_callback(pt_present_cb, NULL);
    sched_name(s_pt.mx, "s_pt.mx");
    sched_name(s_pt.cv, "s_pt.cv");
    sched_name(s_pt.idle_cv, "s_pt.idle_cv");
    openpete_present_thread_set_active(1);
}

/** @brief Publish one scene and kick the present thread, as eager's extraction does.
 * @param target Present instant.
 * @param kind OP_PACE_CANONICAL or OP_PACE_INTERMEDIATE.
 * @return The ring slot, or -1 when the ring was full. */
static int drv_publish(int64_t target, int kind)
{
    const int slot = openpete_native_scene_publish(target, kind, 0.5, 0);
    uint32_t scene_id = 0, stamp = 0;
    openpete_native_scene_slot_ids(slot, &scene_id, &stamp);
    if (slot >= 0) openpete_present_thread_kick();
    return slot;
}

/** @brief Begin timing a deactivation (P-DRAIN). */
static void drain_begin(void)
{
    g.draining = 1;
    g.drain_t0 = sched_now();
    g.drain_busy_ns = 0;
    g.drain_brackets = 0;
    g.drain_call0 = fakevk_thread_call_ns(0);
    g.drain_adv0 = sched_advanced_ns();
    g.prev_now = sched_now();
    g.prev_busy = s_pt.busy;
}

/** @brief End a deactivation: P-DRAIN's time bound and P-ACQUIRE's no-dangling-acquire check. */
static void drain_end(void)
{
    track_drain();
    g.draining = 0;
    prop_eval(P_DRAIN);
    prop_eval(P_ACQUIRE);
    const int64_t idle = sched_now() - g.drain_t0 - g.drain_busy_ns - (fakevk_thread_call_ns(0) - g.drain_call0)
                       - (sched_advanced_ns() - g.drain_adv0);
    if (idle > 0)
        prop_fail(P_DRAIN, "set_active(0) took %.3f ms beyond the present thread's busy brackets and its own release",
                  (double)idle / 1e6);
    if (g_ctr) {
        __atomic_add_fetch(&g_ctr->drains, 1, __ATOMIC_RELAXED);
        if (g.drain_brackets > 1) __atomic_add_fetch(&g_ctr->drains_multi_bracket, 1, __ATOMIC_RELAXED);
    }
    /* A lost device presents nothing again: an acquire it holds is gone with it. */
    if (fakevk_acquired_outstanding() && !fakevk_lost())
        prop_fail(P_ACQUIRE, "set_active(0) returned with %d swapchain image(s) acquired and neither presented nor released",
                  fakevk_acquired_outstanding());
}

/** @brief Take the display back, as the screenshot and capture brackets do.
 * @return The previous active state, for drv_return. */
static int drv_borrow(void)
{
    drain_begin();
    const int was = openpete_present_thread_borrow();
    drain_end();
    return was;
}

/** @brief Hand the display back after drv_borrow.
 * @param was drv_borrow's return value. */
static void drv_return(int was) { openpete_present_thread_return(was); }

/** @brief Disengage eager production (eager_set_active(0)): a drain. */
static void drv_disengage(void)
{
    drain_begin();
    openpete_present_thread_set_active(0);
    drain_end();
}

/** @brief Present inline on the sim thread through the compose, showing the instant of the newest scene still pending, else now.
 *
 * The inline path presents the current moment, which the producer may
 * already have extracted a scene for; a deactivation flushes such scenes,
 * so in a correct schedule none is pending here. */
static void drv_inline(void)
{
    int64_t at = sched_now();
    for (int i = 0; i < SCENE_RING; i++)
        if (s_ring[i].state == SS_PENDING && s_ring[i].target_ns > at) at = s_ring[i].target_ns;
    const uint32_t tag = 0x80000000u | ++g.inline_next;
    if (g.ninl < MAX_INLINE) { g.inl[g.ninl].tag = tag; g.inl[g.ninl].instant = at; g.ninl++; }
    pt_inline_compose(tag);
}

/** @brief Stop the session as close_threads_and_bk2 does; the deactivation stop begins with is timed on its own (P-DRAIN), then the quit and join. */
static void drv_stop(void)
{
    drv_disengage();
    openpete_present_thread_stop();
}

/** @brief Check quiescence after stop: no slot presenting, every published scene in one final bin, census equal to the bins. */
static void quiesce_checks(void)
{
    track_ring();
    track_staged();
    prop_eval(P_ACCOUNT);
    if (g.limbo_id) prop_fail(P_ACCOUNT, "scene %u left the stage and was never accounted", g.limbo_id);
    unsigned n[16] = {0};
    for (uint32_t id = 1; id <= g.max_id; id++) {
        const int b = g.scene[id].bin;
        if (b < B_SUPERSEDED) prop_fail(P_ACCOUNT, "scene %u ended %s at quiescence", id, k_bin_names[b]);
        if (g.scene[id].shown && b != B_PRESENTED && b != B_PRESENTED_DIRECT)
            prop_fail(P_ACCOUNT, "scene %u reached the screen but is binned %s", id, k_bin_names[b]);
        n[b]++;
    }
    for (int i = 0; i < SCENE_RING; i++)
        if (s_ring[i].state == SS_PRESENTING || s_ring[i].state == SS_PENDING)
            prop_fail(P_ACCOUNT, "slot %d is %s at quiescence", i, s_ring[i].state == SS_PENDING ? "pending" : "presenting");
    const unsigned rendered = n[B_PRESENTED] + n[B_PRESENTED_DIRECT] + n[B_REFUSED_PRESENT] + n[B_DISCARDED] + n[B_DROPPED];
    if (s_ring_census.published != g.max_id)
        prop_fail(P_ACCOUNT, "ring census published %u, harness saw %u", s_ring_census.published, g.max_id);
    if (s_ring_census.superseded != n[B_SUPERSEDED])
        prop_fail(P_ACCOUNT, "ring census superseded %u, harness binned %u", s_ring_census.superseded, n[B_SUPERSEDED]);
    if (s_ring_census.presented != rendered)
        prop_fail(P_ACCOUNT, "ring census presented %u, harness saw %u scenes rendered", s_ring_census.presented, rendered);
    if (s_pt.presents != n[B_PRESENTED] + n[B_PRESENTED_DIRECT])
        prop_fail(P_ACCOUNT, "present thread counted %u presents, harness binned %u", s_pt.presents,
                  n[B_PRESENTED] + n[B_PRESENTED_DIRECT]);
}

/* ---------------------------------------------------------- scenarios */

/** @brief S1, borrow while asleep: one scene presented, then two published with the present thread asleep toward the first (or, with present timing, waiting on the previous flip); the sim borrows, presents inline, returns; twice. */
static void sc_s1(void)
{
    drv_begin();
    const int64_t t0 = sched_now();
    drv_publish(t0 + 500000, OP_PACE_CANONICAL);
    drv_sleep(1000000);
    drv_publish(t0 + 3000000, OP_PACE_CANONICAL);
    drv_publish(t0 + 5000000, OP_PACE_INTERMEDIATE);
    for (int k = 0; k < 2; k++) {
        drv_sleep(500000);
        const int was = drv_borrow();
        drv_inline();
        drv_return(was);
    }
    drv_sleep(6000000);
    drv_stop();
}

/** @brief S2, drain during a staged hold: after one present, a scene rendered and its image held for an instant far ahead; the sim disengages and engages, then borrows. */
static void sc_s2(void)
{
    drv_begin();
    const int64_t t0 = sched_now();
    drv_publish(t0 + 500000, OP_PACE_CANONICAL);
    drv_publish(t0 + 9000000, OP_PACE_CANONICAL);
    drv_sleep(2000000);
    drv_disengage();
    drv_inline();
    openpete_present_thread_set_active(1);
    drv_publish(sched_now() + 9000000, OP_PACE_CANONICAL);
    drv_sleep(2000000);
    const int was = drv_borrow();
    drv_inline();
    drv_return(was);
    drv_stop();
}

/** @brief Raise a display change from the event pump's thread at an instant.
 * @param arg The instant, as an int64_t pointer.
 * @return 0. */
static int pump_main(void *arg)
{
    sched_sleep_until(*(const int64_t *)arg, __builtin_return_address(0));
    fakevk_display_change();
    return 0;
}

/** @brief S3, swapchain change: a resize the compose sees on the present thread and inline, a minimised window across several acquires, a display change from the pump, out-of-date outcomes, then a borrow. */
static void sc_s3(void)
{
    static int64_t pump_at;
    drv_begin();
    const int64_t t0 = sched_now();
    pump_at = t0 + 1500000;
    const int pump = sched_thread_create(pump_main, &pump_at, "pump", __builtin_return_address(0));
    drv_publish(t0 + 2000000, OP_PACE_CANONICAL);
    drv_sleep(1000000);
    fakevk_set_window(1600, 900);
    drv_publish(t0 + 4000000, OP_PACE_CANONICAL);
    drv_sleep(3000000);
    int was = drv_borrow();
    drv_inline();
    drv_return(was);
    fakevk_set_window(0, 0);
    drv_publish(sched_now() + 2000000, OP_PACE_CANONICAL);
    drv_sleep(4000000);
    fakevk_set_window(1600, 900);
    drv_sleep(160000000);
    drv_publish(sched_now() + 1000000, OP_PACE_CANONICAL);
    drv_sleep(3000000);
    was = drv_borrow();
    drv_inline();
    drv_return(was);
    (void)pump;   /* the pump ends on its own; the scheduler waits for it */
    drv_stop();
}

/** @brief S4, ring pressure (ring of 3): a slowed present thread while the producer publishes faster; supersede at pick and ring-full refusal. */
static void sc_s4(void)
{
    setenv("OPENPETE_PRESENT_STALL_MS", "3", 1);
    drv_begin();
    const int64_t t0 = sched_now();
    for (int k = 0; k < 6; k++) {
        drv_publish(t0 + 1000000 * (int64_t)(k + 1), k & 1 ? OP_PACE_INTERMEDIATE : OP_PACE_CANONICAL);
        drv_sleep(500000);
    }
    drv_sleep(12000000);
    drv_stop();
}

/** @brief S5, discontinuity: the guest-discontinuity flush concurrent with the present thread's pick and render. */
static void sc_s5(void)
{
    drv_begin();
    const int64_t t0 = sched_now();
    drv_publish(t0 + 1000000, OP_PACE_CANONICAL);
    drv_publish(t0 + 2000000, OP_PACE_INTERMEDIATE);
    openpete_native_scene_ring_flush();
    drv_publish(t0 + 3000000, OP_PACE_CANONICAL);
    drv_sleep(500000);
    openpete_native_scene_ring_flush();
    drv_sleep(4000000);
    drv_stop();
}

/** @brief S5u: S5 with renders made at their instant straight to the swapchain (OPENPETE_PT_STAGED=0). */
static void sc_s5u(void)
{
    setenv("OPENPETE_PT_STAGED", "0", 1);
    sc_s5();
}

/** @brief S6, stop: deactivation then quit while the present thread waits on a flip (present timing) or sleeps toward an instant. */
static void sc_s6(void)
{
    drv_begin();
    const int64_t t0 = sched_now();
    drv_publish(t0 + 500000, OP_PACE_CANONICAL);
    drv_publish(t0 + 3500000, OP_PACE_CANONICAL);
    drv_sleep(1500000);
    drv_stop();
}

/** @brief S7, device loss: a publish, a borrow with an inline present and a stop, with device loss offered at every modelled call. */
static void sc_s7(void)
{
    drv_begin();
    const int64_t t0 = sched_now();
    drv_publish(t0 + 1000000, OP_PACE_CANONICAL);
    drv_sleep(1500000);
    const int was = drv_borrow();
    drv_inline();
    drv_return(was);
    drv_publish(sched_now() + 1000000, OP_PACE_CANONICAL);
    drv_sleep(2000000);
    drv_stop();
}

/** @brief S8, acquire-first order: the first staged present is refused (its acquire times out twice), so the next renders are made on images acquired before them, across a borrow. */
static void sc_s8(void)
{
    fakevk_force(FK_CALL_ACQUIRE, 0, VK_TIMEOUT);
    fakevk_force(FK_CALL_ACQUIRE, 1, VK_TIMEOUT);
    drv_begin();
    const int64_t t0 = sched_now();
    drv_publish(t0 + 500000, OP_PACE_CANONICAL);
    drv_sleep(60000000);
    drv_publish(sched_now() + 1000000, OP_PACE_CANONICAL);
    drv_sleep(600000);
    const int was = drv_borrow();
    drv_inline();
    drv_return(was);
    drv_publish(sched_now() + 1000000, OP_PACE_CANONICAL);
    drv_sleep(3000000);
    drv_stop();
}

/** One scenario: its script, ring size, bounds and device domain. */
typedef struct pt_scenario {
    const char *name;
    void (*fn)(void);
    int ring;
    int max_p, max_d;
    long max_schedules;
    int64_t horizon_ns;
    fakevk_domain dom;
    int advance;          /**< offer time advances while a thread is enabled (deviations) */
} pt_scenario;

#define GPU_DEFAULT { 200000 }, 1, 2083333
#define FENCE_BOUND_NS 500000000LL
static const pt_scenario k_scenarios[] = {
    { "S1", sc_s1, 48, 1, 1, 0, 2000000000LL, { { 0 }, GPU_DEFAULT }, 1 },
    { "S2", sc_s2, 48, 1, 1, 0, 2000000000LL, { { 0 }, GPU_DEFAULT }, 1 },
    { "S3", sc_s3, 48, 1, 1, 0, 3000000000LL,
      { { [FK_CALL_ACQUIRE] = FK_OUT_OUT_OF_DATE | FK_OUT_SUBOPTIMAL,
          [FK_CALL_PRESENT] = FK_OUT_OUT_OF_DATE | FK_OUT_SUBOPTIMAL }, GPU_DEFAULT }, 0 },
    { "S4", sc_s4, 3, 2, 1, 0, 2000000000LL, { { 0 }, GPU_DEFAULT }, 0 },
    { "S5", sc_s5, 48, 2, 1, 0, 2000000000LL, { { 0 }, GPU_DEFAULT }, 0 },
    { "S5u", sc_s5u, 48, 2, 1, 0, 2000000000LL, { { 0 }, GPU_DEFAULT }, 0 },
    { "S6", sc_s6, 48, 2, 1, 0, 2000000000LL, { { 0 }, { 200000, FENCE_BOUND_NS + 1 }, 2, 2083333 }, 0 },
    { "S7", sc_s7, 48, 1, 1, 0, 3000000000LL,
      { { [FK_CALL_ACQUIRE] = FK_OUT_DEVICE_LOST, [FK_CALL_SUBMIT] = FK_OUT_DEVICE_LOST,
          [FK_CALL_PRESENT] = FK_OUT_DEVICE_LOST, [FK_CALL_FENCE_WAIT] = FK_OUT_DEVICE_LOST,
          [FK_CALL_PRESENT_WAIT] = FK_OUT_DEVICE_LOST }, GPU_DEFAULT }, 1 },
    { "S8", sc_s8, 48, 2, 1, 0, 3000000000LL, { { 0 }, GPU_DEFAULT }, 0 },
};
enum { N_SCENARIOS = (int)(sizeof k_scenarios / sizeof k_scenarios[0]) };

static const pt_scenario *g_sc;

/** @brief The scheduled thread 0 of every schedule: install the hooks, run the scenario's script, check quiescence.
 * @param arg Unused. */
static void scenario_main(void *arg)
{
    (void)arg;
    memset(&g, 0, sizeof g);
    g.pt_tid = -1;
    g.serve_t0 = -1;
    fakevk_set_domain(&g_sc->dom);
    const fakevk_hooks hk = { on_call, on_shown };
    fakevk_set_hooks(&hk);
    sched_sdl_admit("openpete-present");
    sched_set_step_hook(step_hook, NULL);
    g_sc->fn();
    quiesce_checks();
}

/* ------------------------------------------------------------- runner */

/** Command-line options. */
typedef struct pt_opts {
    const char *out, *only, *replay, *counters, *sub;
    int timing, jobs, verdict, list, p, d;
    long max_schedules;
} pt_opts;

/** @brief Map the suite counters from a file, creating and zeroing it when asked.
 * @param path The file.
 * @param create Nonzero to create and zero it.
 * @return 0, or -1 on error. */
static int counters_map(const char *path, int create)
{
    const int fd = open(path, O_RDWR | (create ? O_CREAT | O_TRUNC : 0), 0644);
    if (fd < 0) { perror(path); return -1; }
    if (create && ftruncate(fd, sizeof(pt_counters)) != 0) { perror("ftruncate"); close(fd); return -1; }
    g_ctr = mmap(NULL, sizeof(pt_counters), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return g_ctr == MAP_FAILED ? (g_ctr = NULL, -1) : 0;
}

/** @brief Report whether a ring access is ordered without a yield: made under the ring mutex, or by the slot's single owner in its state.
 *
 * A FREE slot belongs to the publisher (the sim thread fills it outside the
 * lock), a PRESENTING or PRESENTED slot to the present thread (it reads the
 * slot it picked without the lock); a PENDING slot is shared and read only
 * under the lock.
 * @param addr The accessed address, inside s_ring.
 * @param thread The accessing thread.
 * @return 1 when the access needs no yield. */
static int ring_owned(const void *addr, int thread)
{
    const sched_mutex *m = (const sched_mutex *)s_ring_mx;
    if (m && m->owner == thread) return 1;
    const size_t i = (size_t)((const char *)addr - (const char *)s_ring) / sizeof s_ring[0];
    if (i >= SCENE_RING) return 0;
    const int st = s_ring[i].state;
    if (st == SS_FREE) return thread == 0;
    return (st == SS_PRESENTING || st == SS_PRESENTED) && thread == g.pt_tid;
}

/** @brief Create the device against the fake Vulkan device and register the address filter over the protocol state.
 * @param timing OPENPETE_PRESENT_TIMING for this configuration.
 * @return 0, or -1 when rhi_create failed. */
static int config_setup(int timing)
{
    setenv("OPENPETE_COMPILE_POOL", "0", 1);
    setenv("OPENPETE_PRESENT_TIMING", timing ? "1" : "0", 1);
    unsetenv("OPENPETE_EAGER");
    unsetenv("OPENPETE_PT_STAGED");
    SDL_Window *win = (SDL_Window *)(uintptr_t)0x1234;
    rhi_device *dev = rhi_create(win, "present_sched", 1, NULL);
    if (!dev) { fprintf(stderr, "present_sched: rhi_create failed: %s\n", rhi_create_error()); return -1; }
    s_nr.dev = dev; s_nr.enabled = 1; s_nr.headless = 0; s_nr.window = win;
    s_nr.win_w = s_nr.pending_w = 1280; s_nr.win_h = s_nr.pending_h = 720;
    /* The filter: s_pt, s_staged and s_lat under s_pt.mx; the ring under its mutex; the display atomics.
     * Excluded: the handles written once before the thread starts, the callback the present thread reads
     * only after an activation, and the relaxed statistics no protocol decision reads. */
    sched_watch_guarded(&s_pt, sizeof s_pt, "s_pt", (void *const *)&s_pt.mx);
    sched_watch_exclude(&s_pt.thr, sizeof s_pt.thr);
    sched_watch_exclude(&s_pt.mx, sizeof s_pt.mx);
    sched_watch_exclude(&s_pt.cv, sizeof s_pt.cv);
    sched_watch_exclude(&s_pt.idle_cv, sizeof s_pt.idle_cv);
    sched_watch_exclude(&s_pt.cb, sizeof s_pt.cb);
    sched_watch_exclude(&s_pt.cb_ctx, sizeof s_pt.cb_ctx);
    sched_watch_exclude(&s_pt.render_ema, sizeof s_pt.render_ema);
    sched_watch_exclude(&s_pt.present_ema, sizeof s_pt.present_ema);
    sched_watch_exclude(&s_pt.service_ema, sizeof s_pt.service_ema);
    sched_watch_exclude(&s_pt.picks, sizeof s_pt.picks);
    sched_watch_exclude(&s_pt.starved, sizeof s_pt.starved);
    sched_watch_guarded(&s_staged, sizeof s_staged, "s_staged", (void *const *)&s_pt.mx);
    sched_watch_guarded(&s_lat, sizeof s_lat, "s_lat", (void *const *)&s_pt.mx);
    sched_watch_owned(s_ring, sizeof s_ring, "s_ring", ring_owned);
    sched_watch_guarded(&s_ring_head, sizeof s_ring_head, "s_ring_head", (void *const *)&s_ring_mx);
    sched_watch_guarded(&s_ring_census, sizeof s_ring_census, "s_ring_census", (void *const *)&s_ring_mx);
    sched_watch(s_display_owner, sizeof(SDL_ThreadID), "s_display_owner");
    const void *chg = fakevk_display_changed_addr();
    if (!chg) { fprintf(stderr, "present_sched: the display-change flag was not found\n"); return -1; }
    sched_watch(chg, 1, "dev->display_changed");
    /* One inline present before the first fork, as boot presents before eager production starts:
     * every schedule starts from a device that has presented. */
    openpete_native_display_claim();
    pt_inline_compose(0x80000000u);
    return 0;
}

/** @brief Run every selected scenario of this binary's ring size in one present-timing configuration.
 * @param o Options.
 * @param timing The configuration.
 * @return 0 when every scenario passed, 1 when one failed, 2 on an error. */
static int run_config(const pt_opts *o, int timing)
{
    if (config_setup(timing) != 0) return 2;
    int rc = 0;
    for (int i = 0; i < N_SCENARIOS; i++) {
        const pt_scenario *sc = &k_scenarios[i];
        if (sc->ring != SCENE_RING) continue;
        if (o->only && strcmp(o->only, sc->name)) continue;
        g_sc = sc;
        char trace[1024], label[64];
        snprintf(trace, sizeof trace, "%s/present_sched-%s-timing%d.trace", o->out, sc->name, timing);
        snprintf(label, sizeof label, "%s timing=%d", sc->name, timing);
        sched_config c = {0};
        c.scenario = scenario_main;
        c.mode = o->replay ? SCHED_MODE_REPLAY : SCHED_MODE_DFS;
        c.replay = o->replay;
        c.max_preemptions = o->p >= 0 ? o->p : sc->max_p;
        c.max_deviations = o->d >= 0 ? o->d : sc->max_d;
        c.jobs = o->jobs;
        c.max_schedules = o->max_schedules ? o->max_schedules : sc->max_schedules;
        c.stop_on_fail = 1;
        c.trace_path = trace;
        c.horizon_ns = 1000000000LL + sc->horizon_ns;
        c.max_steps = 200000;
        c.quiet = 1;
        /* The advance domain: a compute slice and a 480 Hz display slot. */
        static const int64_t k_advance[] = { 50000, 2083333 };
        if (sc->advance || getenv("PT_ADVANCE")) { c.advance = k_advance; c.n_advance = 2; }
        sched_result r;
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        const int e = sched_explore(&c, &r);
        clock_gettime(CLOCK_MONOTONIC, &b);
        sched_print_result(label, &r);
        printf("present_sched: %s: %.2f s wall, %.3f ms/schedule\n", label,
               (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9,
               r.schedules ? ((double)(b.tv_sec - a.tv_sec) * 1e3 + (double)(b.tv_nsec - a.tv_nsec) / 1e6) / (double)r.schedules : 0.0);
        if (e == 2) return 2;
        if (r.found) { printf("present_sched: %s: FAIL, trace %s\n", label, trace); rc = 1; }
        fflush(stdout);
    }
    return rc;
}

/** @brief Print the suite's engagement and judge it: a seam site never reached or a property never evaluated is VACUOUS.
 * @return 0 when engaged, 1 when vacuous. */
static int engagement_verdict(void)
{
    int vacuous = 0;
    printf("present_sched: engagement (all schedules, both present-timing configurations, both ring sizes)\n");
    for (int s = 0; s < PT_SITE_COUNT; s++) {
        int optional = 0;
        for (size_t k = 0; k < sizeof k_site_optional / sizeof k_site_optional[0]; k++)
            optional |= k_site_optional[k] == (pt_site)s;
        printf("  site %-36s %12lu%s\n", k_site_names[s], g_ctr->site[s],
               g_ctr->site[s] ? "" : optional ? "  (not required)" : "  NEVER REACHED");
        if (!g_ctr->site[s] && !optional) vacuous = 1;
    }
    for (int p = 0; p < P_COUNT; p++) {
        printf("  property %-10s evaluations %12lu%s\n", k_prop_names[p], g_ctr->prop_eval[p],
               g_ctr->prop_eval[p] ? "" : "  NEVER EVALUATED");
        if (!g_ctr->prop_eval[p]) vacuous = 1;
    }
    printf("  drains %lu, of which waited through more than one busy bracket %lu\n",
           g_ctr->drains, g_ctr->drains_multi_bracket);
    if (vacuous) printf("present_sched: VACUOUS\n");
    return vacuous;
}

/** @brief Parse the command line.
 * @param argc Argument count.
 * @param argv Arguments.
 * @param o Receives the options.
 * @return 0, or -1 on a usage error. */
static int parse(int argc, char **argv, pt_opts *o)  // complexity-ratchet: one arm per command-line flag
{
    memset(o, 0, sizeof *o);
    o->out = "."; o->timing = -1; o->verdict = 1; o->p = -1; o->d = -1;
    const long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    o->jobs = ncpu > 16 ? 16 : ncpu > 1 ? (int)ncpu : 1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--out") && v) { o->out = v; i++; }
        else if (!strcmp(a, "--only") && v) { o->only = v; i++; }
        else if (!strcmp(a, "--replay") && v) { o->replay = v; i++; }
        else if (!strcmp(a, "--timing") && v) { o->timing = atoi(v); i++; }
        else if (!strcmp(a, "--jobs") && v) { o->jobs = atoi(v); i++; }
        else if (!strcmp(a, "--max-schedules") && v) { o->max_schedules = atol(v); i++; }
        else if (!strcmp(a, "--counters") && v) { o->counters = v; i++; }
        else if (!strcmp(a, "--sub") && v) { o->sub = v; i++; }
        else if (!strcmp(a, "--p") && v) { o->p = atoi(v); i++; }
        else if (!strcmp(a, "--d") && v) { o->d = atoi(v); i++; }
        else if (!strcmp(a, "--no-verdict")) o->verdict = 0;
        else return -1;
    }
    return 0;
}

/** @brief Run one present-timing configuration in a child process (each creates its own device).
 * @param o Options.
 * @param timing The configuration.
 * @return The child's status: 0 pass, 1 fail, 2 error. */
static int fork_config(const pt_opts *o, int timing)
{
    fflush(NULL);
    const pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 2; }
    if (pid == 0) _exit(run_config(o, timing));
    int ws = 0;
    while (waitpid(pid, &ws, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(ws) ? WEXITSTATUS(ws) : 2;
}

/** @brief Run the sub-harness (the other ring size) on the shared counters.
 * @param o Options.
 * @param counters The counters file.
 * @return Its exit status. */
static int run_sub(const pt_opts *o, const char *counters)
{
    char jobs[16];
    snprintf(jobs, sizeof jobs, "%d", o->jobs);
    fflush(NULL);
    const pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 2; }
    if (pid == 0) {
        execl(o->sub, o->sub, "--out", o->out, "--jobs", jobs, "--counters", counters, "--no-verdict", (char *)NULL);
        perror(o->sub);
        _exit(2);
    }
    int ws = 0;
    while (waitpid(pid, &ws, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(ws) ? WEXITSTATUS(ws) : 2;
}

/** @brief Entry: run every scenario of this ring size in both present-timing configurations (or the selected one), the sub-harness, and the engagement verdict.
 * @param argc Argument count.
 * @param argv Arguments.
 * @return 0 pass, 1 a property failed or the run was vacuous, 2 error. */
int main(int argc, char **argv)
{
    pt_opts o;
    if (parse(argc, argv, &o) != 0) {
        fprintf(stderr, "usage: %s [--out DIR] [--only Sn] [--timing 0|1] [--replay CHOICES] [--jobs N] "
                        "[--max-schedules N] [--p P] [--d D] [--counters FILE] [--sub BINARY] [--no-verdict]\n", argv[0]);
        return 2;
    }
    mkdir(o.out, 0755);
    char cpath[1024];
    if (o.counters) snprintf(cpath, sizeof cpath, "%s", o.counters);
    else snprintf(cpath, sizeof cpath, "%s/present_sched.counters", o.out);
    if (counters_map(cpath, !o.counters) != 0) return 2;
    if (o.replay) {
        if (!o.only || o.timing < 0) { fprintf(stderr, "present_sched: --replay needs --only and --timing\n"); return 2; }
        return run_config(&o, o.timing);
    }
    int rc = 0;
    for (int t = 0; t <= 1; t++) {
        if (o.timing >= 0 && t != o.timing) continue;
        const int e = fork_config(&o, t);
        if (e > rc) rc = e;
    }
    if (o.sub) {
        const int e = run_sub(&o, cpath);
        if (e > rc) rc = e;
    }
    if (o.verdict && !o.only && o.timing < 0 && engagement_verdict() && rc == 0) rc = 1;
    printf("present_sched: %s\n", rc == 0 ? "PASS" : rc == 1 ? "FAIL" : "ERROR");
    return rc;
}
