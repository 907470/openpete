/* ring_epilogue.h - included last by the generated ring translation unit:
 * the compose stand-in for native_render.c's render_frame_common, and the
 * inline compose the sim driver presents through.
 *
 * The stand-in reproduces the protocol content of render_frame_common call
 * for call under the same guards: s_last_frame_presented cleared; the
 * window-size check with its settle and discrete-resize branches, which
 * notify the RHI from the composing thread; the staged or swapchain begin
 * and the early return when it is refused; the scene retirement at the
 * begun serial; the overlay load pass; the blit to the swapchain when not
 * staged; the stage or swapchain end; s_last_frame_presented set. The
 * recording between those calls (and its locks) is not modelled. */
#ifndef PT_RING_EPILOGUE_H
#define PT_RING_EPILOGUE_H

/** Set before the stand-in's window-size read, so the size stub counts it as the compose's. */
extern int pt_standin_size_read;

/** Nonzero while the dev overlay draws over a post-processed composite (the load pass's guard). */
int pt_standin_overlay_live;

/** @brief The compose stand-in: render_frame_common's protocol calls under its guards.
 * @param record_pts_us Unused (the capture ring is not modelled).
 * @param have_ref Unused (the reference sync is not modelled). */
static void render_frame_common(uint64_t record_pts_us, bool have_ref)
{
    (void)record_pts_us; (void)have_ref;
    s_last_frame_presented = 0;
    int pw = 0, ph = 0;
    pt_standin_size_read = 1;
    if (!s_nr.rec_pinned &&
        SDL_GetWindowSizeInPixels(s_nr.window, &pw, &ph) && pw > 0 && ph > 0 &&
        (pw != s_nr.win_w || ph != s_nr.win_h)) {
        Uint64 now_ns = SDL_GetTicksNS();
        bool discrete = s_nr.resize_immediate_at_ns &&
                        now_ns - s_nr.resize_immediate_at_ns < NR_RESIZE_IMMEDIATE_GRACE_NS;
        if (discrete) {
            s_nr.resize_immediate_at_ns = 0;
            s_nr.win_w = s_nr.pending_w = pw;
            s_nr.win_h = s_nr.pending_h = ph;
            rhi_notify_resize_immediate(s_nr.dev);
        } else if (pw != s_nr.pending_w || ph != s_nr.pending_h) {
            s_nr.pending_w = pw;
            s_nr.pending_h = ph;
            s_nr.pending_since_ns = now_ns;
        } else if (now_ns - s_nr.pending_since_ns >= NR_RESIZE_SETTLE_NS) {
            s_nr.win_w = pw;
            s_nr.win_h = ph;
            rhi_notify_resize(s_nr.dev);
        }
    }
    int bf_ok = s_stage_target >= 0 ? rhi_begin_frame_staged(s_nr.dev) : rhi_begin_frame(s_nr.dev);
    if (!bf_ok) {
        return;
    }
    openpete_native_scene_retire(rhi_frame_serial(s_nr.dev));
    pt_note_compose(s_ps_slot ? s_ps_slot->scene_id : fakevk_inline_tag(), s_stage_target);
    fakevk_set_render(s_ps_slot ? s_ps_slot->scene_id : fakevk_inline_tag());
    if (pt_standin_overlay_live) {
        rhi_begin_offscreen_load(s_nr.dev);
        rhi_end_offscreen(s_nr.dev);
    }
    if (s_stage_target < 0) {
        rhi_blit_to_swapchain(s_nr.dev);
    }
    if (s_stage_target >= 0) rhi_end_frame_to_stage(s_nr.dev, s_stage_target);
    else                     rhi_end_frame(s_nr.dev);
    s_last_frame_presented = 1;
    fakevk_frame_done(rhi_frame_serial(s_nr.dev));
}

void pt_inline_compose(uint32_t tag)
{
    fakevk_set_inline_tag(tag);
    const scene_slot_t *prev;
    const int hk = hold_redirect_begin(&prev);
    render_frame_common(0, false);
    hold_redirect_end(hk, prev);
}

#endif
