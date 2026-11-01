/* core/renderer/launcher_preview_gpu.c - the launcher's box picture on the
 * launcher's device: a job thread hands a built preview
 * (openpete_tercache_preview) to the display owner, which makes its
 * pipelines' compile jobs, installs the jobs the job thread ran, records the
 * draw inside one launcher present and reads the image back after it. The
 * queue keeps one owner: every submission and the readback happen on the
 * thread that presents. */

#include "openpete_native_render.h"
#include "openpete_terrain_cache.h"
#include "openpete_log.h"
#include "openpete_time.h"
#include "rhi.h"

#include <SDL3/SDL.h>

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* The mailbox between the job thread and the display owner. The job thread
 * posts a request (state POSTED) and waits; the display owner takes it
 * (TAKEN) inside a present and marks it DONE. A job thread that stops or
 * passes its deadline withdraws a POSTED request; a TAKEN one ends within
 * the present that took it, so the job thread waits for it. */
enum { LP_IDLE = 0, LP_POSTED, LP_TAKEN, LP_DONE };
enum { LP_MAKE_JOBS = 1, LP_RENDER };

static struct {
    _Atomic int state;
    int kind;
    /* LP_MAKE_JOBS */
    int slot_contest;
    void *jobs[OPTC_PREVIEW_PIPES];
    rhi_device *dev;
    /* LP_RENDER */
    const openpete_tercache_preview *p;
    uint8_t *rgba;
    size_t cap;
    int rc;
    char why[160];
    /* the display owner's open render, between the frame's record and its readback */
    openpete_tercache_preview_gpu *gpu;
    int recorded;   /* the draw is in the frame being presented */
    int fresh;      /* prepared at this present: the draw waits for the next one */
    uint64_t verts;
    double prepare_ms, record_ms, finish_ms;
    Uint32 wake;
} s_lp;

/** @brief Wake the display owner's event wait so it presents and takes the posted request. */
static void lp_wake(void)
{
    if (!s_lp.wake) return;
    SDL_Event e;
    SDL_zero(e);
    e.type = s_lp.wake;
    (void)SDL_PushEvent(&e);
}

/** @brief Post a request and wait for the display owner to finish it, or withdraw it at a stop or the deadline.
 *
 * @param stop Polled every 2 ms; nonzero withdraws a request not yet taken.
 * @param deadline_ns The op_now() time past which a request not yet taken is withdrawn.
 * @return 0 when the request was done, -2 when stopped, -3 at the deadline. */
static int lp_post_wait(int (*stop)(void), op_time_t deadline_ns)
{
    atomic_store(&s_lp.state, LP_POSTED);
    lp_wake();
    for (;;) {
        const int st = atomic_load(&s_lp.state);
        if (st == LP_DONE) { atomic_store(&s_lp.state, LP_IDLE); return 0; }
        const int give_up = (stop && stop()) ? -2 : (op_now() > deadline_ns ? -3 : 0);
        if (give_up && st == LP_POSTED) {
            int expect = LP_POSTED;
            if (atomic_compare_exchange_strong(&s_lp.state, &expect, LP_IDLE)) return give_up;
        }
        SDL_Delay(2);
    }
}

/** @brief Turns a withdrawn request's status into the call's result and reason.
 * @param rc lp_post_wait's status, -2 or -3.
 * @param late The reason at the deadline.
 * @param why Receives the reason.
 * @param why_cap Its capacity.
 * @return OPENPETE_PREVIEW_STOPPED or OPENPETE_PREVIEW_TRANSIENT. */
static int lp_withdrawn(int rc, const char *late, char *why, size_t why_cap)
{
    snprintf(why, why_cap, "%s", rc == -2 ? "stopped" : late);
    return rc == -2 ? OPENPETE_PREVIEW_STOPPED : OPENPETE_PREVIEW_TRANSIENT;
}

/** @brief Discards the held compile jobs.
 * @param jobs The jobs; each entry may be NULL. */
static void lp_discard(void *jobs[OPTC_PREVIEW_PIPES])
{
    for (int i = 0; i < OPTC_PREVIEW_PIPES; i++) rhi_compile_job_discard(s_lp.dev, (rhi_compile_job *)jobs[i]);
}

/** @brief Has the display owner make the preview pipelines' compile jobs, then runs them on this thread.
 *
 * The compiles run off the display owner: a pipeline the ahead walk
 * compiled is a pipeline-cache hit. On success the jobs are left in the
 * mailbox for the render request.
 * @param slot_contest The SLOT_CONTEST value compiled into the jobs.
 * @param stop The stop poll, or NULL.
 * @param deadline The op_now() time the call gives up at.
 * @param why Receives the failure's reason.
 * @param why_cap Its capacity.
 * @return An OPENPETE_PREVIEW_* value. */
static int lp_compile(int slot_contest, int (*stop)(void), op_time_t deadline, char *why, size_t why_cap)
{
    s_lp.kind = LP_MAKE_JOBS;
    s_lp.slot_contest = slot_contest;
    s_lp.dev = NULL;
    const int rc = lp_post_wait(stop, deadline);
    if (rc != 0) return lp_withdrawn(rc, "the launcher took no request before the time bound", why, why_cap);
    if (!s_lp.dev || !s_lp.jobs[0]) {
        snprintf(why, why_cap, "the terrain pipelines' compile jobs could not be made");
        return OPENPETE_PREVIEW_FAILED;
    }
    int failed = 0;
    for (int i = 0; i < OPTC_PREVIEW_PIPES; i++)
        failed |= rhi_compile_job_run(s_lp.dev, (rhi_compile_job *)s_lp.jobs[i], 0) != 0;
    const int stopped = stop && stop();
    if (!failed && !stopped && op_now() <= deadline) return OPENPETE_PREVIEW_OK;
    lp_discard(s_lp.jobs);
    if (failed) { snprintf(why, why_cap, "a terrain pipeline failed to compile"); return OPENPETE_PREVIEW_FAILED; }
    return lp_withdrawn(stopped ? -2 : -3, "the terrain pipelines compiled past the time bound", why, why_cap);
}

int openpete_native_launcher_preview_render(openpete_tercache_preview *p, int slot_contest,
                                            uint8_t *rgba, size_t cap, int (*stop)(void),
                                            uint64_t deadline_ns, char *why, size_t why_cap)
{
    if (!openpete_native_launcher_active()) {
        snprintf(why, why_cap, "no launcher device (a headless run draws no preview)");
        return OPENPETE_PREVIEW_TRANSIENT;
    }
    if (!s_lp.wake) s_lp.wake = SDL_RegisterEvents(1);
    int rc = lp_compile(slot_contest, stop, (op_time_t)deadline_ns, why, why_cap);
    if (rc != OPENPETE_PREVIEW_OK) return rc;
    s_lp.kind = LP_RENDER;
    s_lp.p = p;
    s_lp.rgba = rgba;
    s_lp.cap = cap;
    s_lp.rc = -1;
    s_lp.why[0] = 0;
    rc = lp_post_wait(stop, (op_time_t)deadline_ns);
    if (rc != 0) {
        lp_discard(s_lp.jobs);
        return lp_withdrawn(rc, "the launcher drew no present before the time bound", why, why_cap);
    }
    if (s_lp.rc != 0) {
        snprintf(why, why_cap, "%s", s_lp.why[0] ? s_lp.why : "the preview draw failed");
        return s_lp.rc;
    }
    OPL_INFO_F("launcher", "preview drawn on the launcher's present: %llu vertices, prepare %.1f ms, "
               "record %.1f ms, readback %.1f ms", (unsigned long long)s_lp.verts, s_lp.prepare_ms,
               s_lp.record_ms, s_lp.finish_ms);
    return OPENPETE_PREVIEW_OK;
}

/** @brief Return the milliseconds from a start time to now.
 * @param t0 The start.
 * @return The milliseconds. */
static double lp_ms(op_time_t t0) { return (double)(op_now() - t0) / 1e6; }

void openpete_native_launcher_preview_before_frame(struct rhi_device *dev)
{
    int expect = LP_POSTED;
    if (atomic_load(&s_lp.state) != LP_POSTED ||
        !atomic_compare_exchange_strong(&s_lp.state, &expect, LP_TAKEN))
        return;
    if (s_lp.kind == LP_MAKE_JOBS) {
        s_lp.dev = dev;
        if (openpete_tercache_preview_jobs(dev, s_lp.slot_contest, s_lp.jobs) != 0) s_lp.dev = NULL;
        atomic_store(&s_lp.state, LP_DONE);
        return;
    }
    const op_time_t t0 = op_now();
    s_lp.recorded = 0;
    s_lp.fresh = 1;
    s_lp.gpu = openpete_tercache_preview_prepare(dev, s_lp.p, s_lp.jobs);
    s_lp.prepare_ms = lp_ms(t0);
    if (s_lp.gpu) lp_wake();   /* the draw needs one more present */
    if (!s_lp.gpu) {
        s_lp.rc = OPENPETE_PREVIEW_FAILED;
        snprintf(s_lp.why, sizeof s_lp.why, "the preview's pipelines or device objects could not be made");
        atomic_store(&s_lp.state, LP_DONE);
    }
}

void openpete_native_launcher_preview_in_frame(void)
{
    if (atomic_load(&s_lp.state) != LP_TAKEN || !s_lp.gpu) return;
    /* The preparation (uploads, pipeline installs) and the draw with its
     * readback land on two presents, so neither adds both to one. */
    if (s_lp.fresh) { s_lp.fresh = 0; return; }
    const op_time_t t0 = op_now();
    s_lp.verts = openpete_tercache_preview_record(s_lp.gpu);
    s_lp.record_ms = lp_ms(t0);
    s_lp.recorded = 1;
}

void openpete_native_launcher_preview_after_frame(int frame_ran)
{
    /* A frame that did not begin (the swapchain was out of date) keeps the
     * prepared objects for the next present. */
    if (atomic_load(&s_lp.state) != LP_TAKEN || !s_lp.gpu || !frame_ran || !s_lp.recorded) return;
    const op_time_t t0 = op_now();
    const int rc = openpete_tercache_preview_finish(s_lp.gpu, s_lp.rgba, s_lp.cap);
    s_lp.gpu = NULL;
    s_lp.recorded = 0;
    s_lp.finish_ms = lp_ms(t0);
    if (rc != 0) {
        s_lp.rc = OPENPETE_PREVIEW_TRANSIENT;
        snprintf(s_lp.why, sizeof s_lp.why, "the preview's readback failed");
    } else {
        s_lp.rc = OPENPETE_PREVIEW_OK;
    }
    atomic_store(&s_lp.state, LP_DONE);
}
