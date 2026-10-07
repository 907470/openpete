/* hook_bracket.c: see hook_bracket.h. Every feeder of every game runs its
 * guest draw through these bodies; a change to the gate order, the cursor
 * reads or the guest call moves all of them at once. */

#include "renderer/hook_bracket.h"

#include "renderer/cut_guard.h"            /* CUT_HELD_EMPTY */

#include "game_layout.h"                 /* polybuf.cursor */
#include "openpete_native_render.h"      /* openpete_native_scene_enabled */
#include "openpete_profile.h"            /* op_prof_*: the guest draw's bucket */
#include "openpete_recomp_module.h"      /* the default guest draw: module dispatch */

int hook_bracket_run(CPUState* cpu, uint32_t addr, const hook_bracket_desc* d,
                     uint32_t* start, uint32_t* end, int* feed, int* verify) {
    int v = d->verify_enabled ? d->verify_enabled() : 0;
    int f = openpete_native_scene_enabled();
    /* --render-paths needs the bracket even with nothing to feed or verify, so
     * the ordering-table filter learns this path's polybuf range. */
    int paths = !(d->flags & HB_NO_RENDER_PATHS) && render_paths_active();
    int extra = d->extra_gate ? d->extra_gate() : 0;
    if (!v && !f && !paths && !extra) return 0;   /* inert: ordinary dispatch */

    hook_bracket_state* st = d->st;
    if (st->in) return 0;                         /* the dispatch below re-enters */
    st->in = 1;

    *feed = f;
    *verify = v;
    if (d->pre) d->pre(cpu, f, v);

    uint32_t s = cpu->read_word(game_layout.polybuf.cursor);

    /* The accumulating profile bucket, opened by hand because the scope macro
     * interns a string literal per source line and the tag here is per
     * descriptor. Interned once per latch; op_prof_intern yields -1 while the
     * profiler is off, which leaves the bucket closed. The bucket spans the
     * guest draw alone, not the cursor reads that bracket it. */
    int prof_id = -1;
    uint64_t t0 = 0;
    if (op_prof_on()) {
        if (!st->prof_id_plus1) st->prof_id_plus1 = op_prof_intern(d->prof_tag) + 1;
        prof_id = st->prof_id_plus1 - 1;
        if (prof_id >= 0) t0 = op_prof_now_us();
    }

    if (d->run) d->run(cpu, addr);
    else        openpete_recomp_module_get()->dispatch(cpu, addr);

    if (prof_id >= 0) op_prof_accum(prof_id, op_prof_now_us() - t0);

    uint32_t e = cpu->read_word(game_layout.polybuf.cursor);

    *start = s;
    *end   = e;
    render_path_record(d->path, s, e);   /* polybuf range for the OT filter */
    return 1;
}

int hook_bracket_end(const hook_bracket_desc* d) {
    d->st->in = 0;
    return 1;                            /* took over: the body already ran */
}

int hook_reextract_ready(render_path_t path) {
    return openpete_native_scene_enabled() &&
           render_path_enabled(path) &&
           render_path_canon_drawn(path);
}

int hook_reextract_ready_held(render_path_t path, int held) {
    if (held < 0) return hook_reextract_ready(path);
    return held != CUT_HELD_EMPTY && openpete_native_scene_enabled() && render_path_enabled(path);
}
