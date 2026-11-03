// core/runtime/host_input_tick.c - the host loop's input tick: override
// arbitration (bk2 stamp, overlay stamp and suspension), the latch and the
// staging of each tick's input record, the post-tick input-log commit with
// its census, and the bk2 replay load and record flush.
#include "host_input_tick.h"

#include "openpete_devui.h"
#include "openpete_fiber.h"
#include "openpete_freecam.h"
#include "openpete_camlock.h"
#include "openpete_game_runtime.h"
#include "openpete_host_input.h"
#include "openpete_host_mouse.h"
#include "openpete_input_log.h"
#include "openpete_keymap.h"
#include "openpete_log.h"
#include "openpete_menu.h"
#include "openpete_mods.h"
#include "openpete_pace_log.h"
#include "bk2_feed.h"
#include "bk2_movie.h"
#include "bk2_record.h"
#include "run_internal.h"

#include "PsyX/PsyX_public.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#ifdef OPENPETE_HAS_PSYCROSS

/** @brief Return the name of the counter a movie's rows are indexed by.
 * @param key The movie's row key.
 * @return "openpete_consume", "openpete_vbl" or "openpete_frame". */
static const char* bk2_row_key_name(bk2_row_key_t key) {
    if (key == BK2_ROW_KEY_CONSUME) return "openpete_consume";
    return key == BK2_ROW_KEY_VBL ? "openpete_vbl" : "openpete_frame";
}

/* Documented at the declaration in host_input_tick.h. */
int bk2_replay_load(const openpete_options_t* opts, bk2_movie_t* bk2,
                    int* offset) {
    if (bk2_movie_load(opts->bk2_path, bk2) != 0) {
        return -1;
    }
    if (bk2->load_delay != openpete_cd_load_delay) {
        OPL_ERROR_F("bk2", "%s was recorded with a load delay of %d VBL(s); "
                    "this run uses %d. Pass --load-delay %d to replay it.",
                    opts->bk2_path, bk2->load_delay, openpete_cd_load_delay,
                    bk2->load_delay);
        bk2_movie_free(bk2);
        return -1;
    }
    if (bk2->row_key == BK2_ROW_KEY_CONSUME && !openpete_game_runtime()->consume_pc) {
        OPL_ERROR_F("bk2", "%s is keyed on consume; %s names no pad-ring consumer",
                    opts->bk2_path, game_get_display_name());
        bk2_movie_free(bk2);
        return -1;
    }
    if (bk2->row_key != BK2_ROW_KEY_CONSUME)
        OPL_INFO_F("bk2", "%s is keyed on %s: converted on load (replayed under "
                   "that key)", opts->bk2_path, bk2_row_key_name(bk2->row_key));
    /* Default offset is 0: both counters start at 0 on record and
     * replay, so the row numbers line up 1:1 and replay sees the same
     * game state the player saw when they pressed the button.
     * Auto-skipping to first_input_frame would collapse the player's
     * idle wait and fire input before the game is ready. Use
     * --bk2-frame-offset to opt into a manual skip. */
    *offset = opts->bk2_frame_offset_set ? opts->bk2_frame_offset : 0;
    const char* clock = bk2_row_key_name(bk2->row_key);
    OPL_INFO_F("bk2",
               "loaded %d frames from %s, first input at bk2-frame=%d, "
               "%s=%d -> bk2-frame=%s-(%d)%s",
               bk2->frame_count, opts->bk2_path, bk2->first_input_frame,
               clock, *offset, clock, *offset,
               opts->bk2_frame_offset_set ? " (explicit)" : "");
    return 0;
}

/** @brief Append the staged input record once per yield of the iteration and note each entry's movie rows.
 *
 * A movie carries one row per pad write of a yield, grouped by
 * openpete_consume: the iteration's writes (bk2_feed_iter_take), each with
 * the player 1 pad the host delivered at it, are noted on its last entry,
 * in the always-on note store the recorder and the bug-report segment
 * read. Holds input_log_lock across the appends and notes, so a finalize
 * on another thread sees each entry with its rows or not at all.
 * @param ticks Yields the iteration ran (openpete_frame's advance). */
static void input_log_commit_yields(unsigned ticks) {
    input_log_lock();
    unsigned e = 0u, e_last = 0u, j = 0u;
    const bk2_pad_write_t* pads = NULL;
    const unsigned writes = bk2_feed_iter_take(&e, &e_last, &j, &pads);
    for (unsigned t = 0; t < ticks; t++) {
        const int idx = input_log_append_staged();
        bk2_record_note(idx, t + 1 == ticks ? writes : 0u, e, e_last, j, pads);
    }
    input_log_unlock();
}

/** @brief Read back the player 1 pad state the guest last read from PsyCross, with the axes in the log's order.
 *
 * @param bk2axes Receives the axes as LX, LY, RX, RY (PsyCross reports RX, RY, LX, LY).
 * @return The player 1 pad bits (1 released, 0 pressed). */
static uint16_t pad_p1_readback(unsigned char bk2axes[4]) {
    unsigned char ax[4];
    PsyX_Pad_GetLastP1Analog(ax);
    bk2axes[0] = ax[2]; bk2axes[1] = ax[3]; bk2axes[2] = ax[0]; bk2axes[3] = ax[1];
    return PsyX_Pad_GetLastP1Bits();
}

/* Documented at the declaration in host_input_tick.h. */
void bk2_record_exit_flush(void) {
    input_log_lock();
    if (!openpete_replaying) {
        unsigned char bk2axes[4];
        const uint16_t bits = pad_p1_readback(bk2axes);
        input_log_stage_p1_readback(bits, bk2axes);
        input_log_commit_yields(1u);
    }
    (void)bk2_record_finalize();
}

/** @brief Stamp a movie row into PsyCross's pad-override slot.
 *
 * A row outside the movie stamps no override (0xFFFF, inactive), which
 * hands the pad to live input.
 * @param m The replayed movie.
 * @param row The row, as bk2_feed_row returned it.
 * @return 1 when a movie row was stamped, else 0. */
static int bk2_stamp_row(const bk2_movie_t* m, int row) {
    const int in = row >= 0 && row < m->frame_count;
    unsigned short bits = 0xFFFF;
    if (in) {
        const bk2_frame_t* fr = &m->frames[row];
        bits = fr->buttons;
        /* bk2 axis order is LX,LY,RX,RY; PSX pad->analog is RX,RY,LX,LY. */
        g_PsyX_Pad_AnalogOverride[0] = fr->axes[2]; /* RStick X */
        g_PsyX_Pad_AnalogOverride[1] = fr->axes[3]; /* RStick Y */
        g_PsyX_Pad_AnalogOverride[2] = fr->axes[0]; /* LStick X */
        g_PsyX_Pad_AnalogOverride[3] = fr->axes[1]; /* LStick Y */
    }
    g_PsyX_Pad_InputOverride = bits;
    g_PsyX_Pad_OverrideActive = in;
    bk2_feed_stamped(row);
    return in;
}

/* Documented at the declaration in host_input_tick.h. */
int synth_pads_enabled(void) {
    static int s_on = -1;
    if (s_on < 0) {
        const char* v = getenv("OPENPETE_SYNTH_PADS");
        if (!v || !*v || *v == '0')       s_on = 0;
        else if (v[0] == 'v' || v[0] == 'V') s_on = 2;
        else                              s_on = 1;
    }
    return s_on;
}

/** @brief Compute the synthetic pad bits and axes for a VBL index and slot.
 *
 * With k = frame * 7 + slot * 1361, bits has bit (k % 16) cleared (buttons
 * are active low) and the axes are k, k / 3, k / 7 and k / 11, each modulo
 * 251. A pure function of its inputs.
 * @param frame The VBL index.
 * @param slot The pad slot.
 * @param bits Receives the button bits.
 * @param axes Receives the four axis bytes. */
static void synth_pad_for(unsigned frame, int slot,
                          uint16_t* bits, unsigned char axes[4]) {
    /* Distinct per slot and per VBL, and never 0xFFFF-with-centred-sticks,
     * so a dropped port is not mistaken for a legitimately idle one. */
    unsigned k = frame * 7u + (unsigned)slot * 1361u;
    *bits   = (uint16_t)~(1u << (k % 16));      /* one button held */
    axes[0] = (unsigned char)(k % 251u);
    axes[1] = (unsigned char)((k / 3u) % 251u);
    axes[2] = (unsigned char)((k / 7u) % 251u);
    axes[3] = (unsigned char)((k / 11u) % 251u);
}

/** @brief Test whether slots 1 to last_slot of an input record carry exactly the synthetic stream for a VBL.
 *
 * @param rec The input record.
 * @param frame The VBL index the stream is generated for.
 * @param last_slot The highest slot checked.
 * @return 1 when every checked port is present with the synthetic bits and
 *         axes, else 0. */
static int synth_pads_match_upto(const openpete_input_rec_t* rec, unsigned frame,
                                 int last_slot) {
    for (int sl = 1; sl <= last_slot; sl++) {
        uint16_t b; unsigned char ax[4];
        synth_pad_for(frame, sl, &b, ax);
        const openpete_input_port_t* pt = &rec->port[sl];
        if (!pt->present || pt->bits != b || memcmp(pt->axes, ax, 4) != 0)
            return 0;
    }
    return 1;
}

int synth_pads_match(const openpete_input_rec_t* rec, unsigned frame) {
    const int last = (synth_pads_enabled() == 2) ? 1 : OPENPETE_PAD_SLOTS - 1;
    return synth_pads_match_upto(rec, frame, last);
}

/* VBLs each pad slot was recorded present for. Slot 0 is every logged VBL by
 * construction; a zero on slots 1-3 is what separates "the extra players were
 * carried" from "nothing was ever plugged in", which no downstream comparison
 * can distinguish on its own. */
static unsigned long long s_slot_present_vbls[OPENPETE_PAD_SLOTS] = {0};

/* VBLs on which two slots carried different bytes. Presence alone cannot
 * catch a routing bug that fed one device into several slots: crossed slots
 * are present every VBL and identical every VBL. A pair that never differs
 * over a whole run was never independently routed. */
static unsigned long long s_slot_differed[OPENPETE_PAD_SLOTS][OPENPETE_PAD_SLOTS] = {{0}};

static unsigned long long s_synth_stage_checks = 0;
static unsigned long long s_synth_stage_mismatches = 0;
unsigned long long s_synth_rewind_checks = 0;
unsigned long long s_synth_rewind_mismatches = 0;

/* Pre-yield-staging equality instrument. The input record is staged before
 * the tick so an in-tick reader can see it, but the bytes the log stores for
 * slot 0 remain the post-yield guest readback. These count how often the two
 * were compared and how often they disagreed; a nonzero mismatch means an
 * in-tick slot-0 read would see bytes the log never recorded. */
static unsigned long long s_p1_stage_checks = 0;
static unsigned long long s_p1_stage_mismatches = 0;
/* First disagreement, kept for the report: a count alone cannot say whether
 * the cause is a one-off boot transient or a live decode path. */
static unsigned s_p1_stage_first_frame = 0;
static uint16_t s_p1_stage_first_staged_bits = 0;
static uint16_t s_p1_stage_first_read_bits = 0;
static unsigned char s_p1_stage_first_staged_axes[4] = {0};
static unsigned char s_p1_stage_first_read_axes[4] = {0};

/* Documented at the declaration in host_input_tick.h. */
void host_input_exit_report(void) {
    /* Staged-vs-readback equality for slot 0. Zero checks means the override
     * channel was never stamped before a tick (no host-owned input at all),
     * which makes the mismatch count uninformative rather than clean. */
    OPL_INFO_F("input-log",
               "slot-0 staging: %llu check(s), %llu mismatch(es)",
               s_p1_stage_checks, s_p1_stage_mismatches);
    OPL_INFO_F("input-log",
               "pad slots present (VBLs): p1=%llu p2=%llu p3=%llu p4=%llu",
               s_slot_present_vbls[0], s_slot_present_vbls[1],
               s_slot_present_vbls[2], s_slot_present_vbls[3]);
    OPL_INFO_F("input-log",
               "pad slot independence (VBLs differing): "
               "p1/p2=%llu p1/p3=%llu p1/p4=%llu p2/p3=%llu p2/p4=%llu p3/p4=%llu",
               s_slot_differed[0][1], s_slot_differed[0][2], s_slot_differed[0][3],
               s_slot_differed[1][2], s_slot_differed[1][3], s_slot_differed[2][3]);
    if (synth_pads_enabled())
        OPL_INFO_F("input-log",
                   "synthetic slots 1-3: %llu stage check(s)/%llu mismatch(es), "
                   "%llu rewind check(s)/%llu mismatch(es)",
                   s_synth_stage_checks, s_synth_stage_mismatches,
                   s_synth_rewind_checks, s_synth_rewind_mismatches);
    if (s_p1_stage_mismatches)
        OPL_ERROR_F("input-log",
                    "slot-0 staging first mismatch at frame %u: "
                    "staged bits=0x%04X axes=%u,%u,%u,%u; "
                    "readback bits=0x%04X axes=%u,%u,%u,%u",
                    s_p1_stage_first_frame,
                    s_p1_stage_first_staged_bits,
                    s_p1_stage_first_staged_axes[0], s_p1_stage_first_staged_axes[1],
                    s_p1_stage_first_staged_axes[2], s_p1_stage_first_staged_axes[3],
                    s_p1_stage_first_read_bits,
                    s_p1_stage_first_read_axes[0], s_p1_stage_first_read_axes[1],
                    s_p1_stage_first_read_axes[2], s_p1_stage_first_read_axes[3]);
}

/** @brief Return the actions that keep reading through the dev overlay's keyboard and pad suspensions.
 *
 * imgui's nav focus covers the whole panel while it is focused, so
 * suspending every action would leave the source that opened the panel
 * unable to close it, and the pad unable to navigate it: `devui` and every
 * KM_CTX_DEVUI action are exempt.
 * @param n receives the number of actions.
 * @return the actions, a static list. */
static const openpete_action_t* devui_suspend_exempt(int* n) {
    static const openpete_action_t kExempt[] = {
        OPK_DEVUI, OPK_DEVUI_UP, OPK_DEVUI_DOWN, OPK_DEVUI_LEFT,
        OPK_DEVUI_RIGHT, OPK_DEVUI_ACTIVATE, OPK_DEVUI_CANCEL, OPK_DEVUI_MENU,
        OPK_DEVUI_SLOWER, OPK_DEVUI_FASTER,
    };
    *n = (int)(sizeof kExempt / sizeof kExempt[0]);
    return kExempt;
}

/** @brief Write player 1's override channel as all buttons released and sticks centred.
 *
 * @param active 1 to make the neutral record the pad's input, 0 to hand the
 *        channel back to live decode (the overlays' clear-once latch). */
static void pad_override_neutral(int active) {
    g_PsyX_Pad_InputOverride  = 0xFFFF;
    g_PsyX_Pad_OverrideActive = active;
    g_PsyX_Pad_AnalogOverride[0] = g_PsyX_Pad_AnalogOverride[1] =
    g_PsyX_Pad_AnalogOverride[2] = g_PsyX_Pad_AnalogOverride[3] = 128;
}

/** @brief Return whether a bk2 movie holds a row for this VBL.
 *
 * @param active a movie is loaded for replay.
 * @param row the movie row at the current row position (bk2_feed_row).
 * @param count the movie's row count.
 * @return true when row indexes a movie row. */
static bool bk2_frame_in_range(bool active, int row, int count) {
    return active && row >= 0 && row < count;
}

/* The input tick's latches across iterations. Reset per run by
 * host_input_latch_init. */
static struct {
    bool prev_overlay_hold;        /* an overlay held the pad neutral on the previous iteration */
    unsigned short bk2_last_bits;  /* the movie bits the bk2 transition log printed last */
} s_input_latch;

/* Documented at the declaration in host_input_tick.h. */
void host_input_latch_init(void) {
    s_input_latch.prev_overlay_hold = false;
    s_input_latch.bk2_last_bits = 0xFFFF;
}

/** @brief Hold player 1's pad neutral while an overlay owns it, through the override channel.
 *
 * The settings menu owns the pad while open; a binding capture from its
 * start until the inputs it latched are released
 * (openpete_keymap_capture_neutral); the dev overlay while imgui
 * owns the keyboard (a text field or navigation focus, so typed characters
 * cannot reach the PSX pad through PsyCross's keyboard map) or navigates
 * with the pad (an overlay window holds navigation focus, so the buttons
 * that move through the panel do not also move Spyro; a click outside
 * every overlay window drops the focus and the pad returns to the game);
 * the bug-report note prompt from its request to its close.
 * The sim keeps running behind the menu and the overlay, so settings
 * preview live. A bk2
 * frame in range outranks both: the movie's input stays the pad's input.
 * On the iteration an overlay lets go the channel is handed back to live
 * decode once, unless a movie is loaded, whose stamp rewrites it.
 * @param bk2_active a movie is loaded for replay.
 * @param bk2_in_range the movie holds a frame for this VBL.
 * @param prev_hold the previous iteration's hold, updated.
 * @return true when this call claimed the override channel. */
static bool overlay_pad_stamp(bool bk2_active, bool bk2_in_range, bool* prev_hold) {
    const bool hold = openpete_menu_is_open() ||
                      openpete_devui_wants_keyboard() ||
                      openpete_devui_note_active() ||
                      openpete_devui_padnav_engaged() ||
                      openpete_keymap_capture_neutral();
    const bool was = *prev_hold;
    *prev_hold = hold;
    if (hold && !bk2_in_range) {
        pad_override_neutral(1);
        return true;
    }
    if (!hold && was && !bk2_active) pad_override_neutral(0);
    return false;
}

/** @brief Suspend the hotkey sources the dev overlay owns this iteration.
 *
 * While imgui owns the keyboard every keyboard-sourced action goes inert;
 * while the overlay navigates with the pad every live pad-sourced action
 * does too, so the buttons that move through the panel fire no savestate,
 * freecam or other pad-bound action, and the freecam axes are zeroed as
 * the settings menu zeroes them. `devui` and the KM_CTX_DEVUI actions are
 * exempt (devui_suspend_exempt), except from the keyboard suspension while
 * a text field takes input, which swallows every key; a key held then stays
 * latched until released (openpete_keymap_latch_held_keys), so the field
 * letting go does not turn it into a press. While the settings
 * menu is open every live pad source outside the menu's own actions and
 * the overlay toggles (openpete_keymap_menu_pad_exempt) is suspended the
 * same way, so a hold: pad hotkey sharing a button with menu navigation
 * never fires in the menu. The bug-report note prompt suspends both
 * sources as a focused text field and pad navigation do, from its request,
 * before its first frame publishes a want. A binding capture zeroes the
 * freecam axes too.
 * Esc-close works through the event pump. */
static void overlay_input_suspend(void) {
    int n_exempt = 0;
    const openpete_action_t* exempt = devui_suspend_exempt(&n_exempt);
    const bool note = openpete_devui_note_active() != 0;
    const bool padnav = note || openpete_devui_padnav_engaged() != 0;
    const bool text = note || openpete_devui_wants_text_input() != 0;
    openpete_keymap_suspend_keyboard(
        (note || openpete_devui_wants_keyboard()) ? 1 : 0, text ? NULL : exempt, n_exempt);
    if (text) openpete_keymap_latch_held_keys();
    if (padnav) {
        openpete_keymap_suspend_pad(1, exempt, n_exempt);
    } else if (openpete_menu_is_open()) {
        int n_menu = 0;
        const openpete_action_t* menu_ex = openpete_keymap_menu_pad_exempt(&n_menu);
        openpete_keymap_suspend_pad(1, menu_ex, n_menu);
    } else {
        openpete_keymap_suspend_pad(0, NULL, 0);
    }
    const bool zero = padnav || openpete_keymap_capture_neutral();
    if (!zero || !(openpete_freecam_active() || openpete_camlock_active()))
        return;
    openpete_freecam_axes ax = {0};
    openpete_freecam_set_axes(&ax);
}

/** @brief Stamp the movie row of this tick into the override channel, publish the mods' bk2 status and log each change of the stamped bits.
 *
 * Out-of-range rows (before the offset, or past the movie's end) stamp no
 * override, so live keyboard and gamepad input takes over once the movie
 * ends; with --bk2-stop-on-end a row past the end stamps nothing and ends
 * the run instead.
 * @param S The session; its bk2 movie is loaded for replay.
 * @return -1 when the run ends at the movie's end, 1 when a movie row
 *         claimed the override channel, else 0. */
static int input_bk2_stamp(const host_session_t* S) OP_REQUIRES(sim)
{
    int bk2_frame = bk2_feed_row();
    if (bk2_frame >= S->bk2.movie.frame_count && S->opts->bk2_stop_on_end)
        return -1;
    const int ovr_active = bk2_stamp_row(&S->bk2.movie, bk2_frame);
    const unsigned short bits = (unsigned short)g_PsyX_Pad_InputOverride;
    {
        /* Mods' read-only bk2 status (api->bk2_status) - also
         * published pre-offset / past-end (overriding = 0). */
        openpete_mods_bk2_status_t st = {
            1, bk2_frame, (int)S->bk2.movie.frame_count, ovr_active };
        openpete_mods_bk2_publish(&st);
    }
    /* Log every input transition so the user can audit the
     * playback timeline. Mirrors the press/release format used
     * on the record path. */
    if (bits != s_input_latch.bk2_last_bits) {
        char p_buf[17], r_buf[17];
        bk2_format_button_diff(s_input_latch.bk2_last_bits, bits, p_buf, r_buf);
        OPL_INFO_F("bk2",
                   "openpete_frame=%u bk2_frame=%d press=[%s] release=[%s] bits=0x%04X",
                   openpete_frame, bk2_frame, p_buf, r_buf, bits);
        s_input_latch.bk2_last_bits = bits;
    }
    /* Pre-offset / past-end frames leave ovr_active 0 on purpose:
     * that is the documented hand-back to live input once a movie
     * ends, which now means the live-decode latch. */
    return ovr_active ? 1 : 0;
}

/* Documented at the declaration in host_input_tick.h. */
bool host_input_arbitrate(const host_session_t* S, host_iter_t* it) OP_REQUIRES(sim)
{
    /* Override-channel arbitration for this iteration. The channel has
     * several writers with different priorities; live decode is the
     * lowest and fills it whenever nothing else claims it. Anything that
     * needs the pad to NOT be live input claims it. */
    it->ovr_claimed = false;
    it->bk2_stamped_frame = openpete_frame;  /* see host_input_restamp_after_ops */
    if (it->sim_due && S->bk2.active) {
        const int r = input_bk2_stamp(S);
        if (r < 0) return true;
        if (r) it->ovr_claimed = true;
    }

    /* A bk2 frame in range for the movie's row counter owns the override
     * channel; the overlays' neutral stamp yields to it. Evaluated on
     * every iteration, not only sim-due ones, because the overlays stamp
     * on every iteration and the movie restamps only on a tick. */
    if (overlay_pad_stamp(S->bk2.active,
                          bk2_frame_in_range(S->bk2.active, bk2_feed_row(),
                                             S->bk2.movie.frame_count),
                          &s_input_latch.prev_overlay_hold))
        it->ovr_claimed = true;
    overlay_input_suspend();

    /* Under the thread split the cursor is main's business (see
     * openpete_cursor_update): SetCursor only reaches a window from the
     * thread that owns it. Unsplit, this loop IS that thread. */
    if (!openpete_split_applies(S->opts)) {
        openpete_cursor_update(S->opts->headless);
        openpete_host_mouse_apply_capture();
    }
    return false;
}

/* Documented at the declaration in host_input_tick.h. */
void host_input_restamp_after_ops(const host_session_t* S, host_iter_t* it) OP_REQUIRES(sim)
{
    if (!S->bk2.active || openpete_frame == it->bk2_stamped_frame) return;
    /* The arbitration's claim was decided for the row before the jump; it
     * is decided again for the new one, with the same priorities: a movie
     * row in range owns the channel, else an overlay holding the pad keeps
     * it neutral, else live input takes it. */
    if (bk2_stamp_row(&S->bk2.movie, bk2_feed_row())) {
        it->ovr_claimed = true;
    } else if (s_input_latch.prev_overlay_hold) {
        pad_override_neutral(1);
        it->ovr_claimed = true;
    } else {
        it->ovr_claimed = false;
    }
}

/** @brief Pump, publish, sample and latch the host's input for this iteration and record which sample the VBL consumes.
 *
 * Input runs as four explicit steps instead of PsyX_UpdateInput's two:
 * pump -> decode -> arbitrate+stamp -> inject. openpete owns decode and
 * the latch point; PsyCross still owns the byte-exact write into the PSX
 * pad buffer, through the same override channel bk2 replay has always
 * used. Ordering is load-bearing: the decode must read SDL state from THIS
 * iteration's pump, or live input lags a full iteration behind. The Alt
 * gate that PsyX_UpdateInput applied to its injection half moves onto the
 * sample, so Alt+Enter still cannot register as Start. The legacy A/B path
 * (openpete_host_input_legacy) leaves the latch record at no latch.
 * @param split The thread split applies: main owns the pump, the publish and the pad sample.
 * @param it The iteration; reads ovr_claimed, writes the latch record. */
static void input_pump_and_latch(bool split, host_iter_t* it) OP_REQUIRES(sim)
{
    /* Publish the SDL-state snapshot on both paths: hotkeys, mod key
     * queries and the Alt gate all read it, so the legacy A/B would
     * otherwise run with every binding reading a never-updated buffer.
     * Legacy pumps inside PsyX_UpdateInput below, so it publishes the
     * previous pump's state - the same one-iteration lag the polled
     * bindings have always had. */
    if (!split) openpete_host_input_publish_sdl();

    it->vbl.latch_ns   = 0;
    it->vbl.latch_seq  = OP_PACE_LATCH_NONE;
    it->vbl.latch_bits = 0xFFFFu;

    if (openpete_host_input_legacy()) {
        /* Pre-ownership path, kept as a diagnostic A/B: PsyCross both
         * pumps and decodes, and the override channel carries only what
         * bk2/menu/devui stamped. The two paths must produce identical
         * gameplay for any input the host is not driving. */
        pad_restamp_if_due(0);
        PsyX_UpdateInput();
        return;
    }
    /* Under the thread split main owns the pump, the SDL-state
     * publish and the pad sample; this thread only consumes what was
     * published. That asymmetry is the whole point of the seam - the
     * consumer never touches SDL. */
    if (!split) {
        openpete_host_mouse_pump_iteration();
        openpete_pump_events();
        /* Re-publish now the pump has refreshed SDL, so the pad sample
         * and this iteration's consumers see current state rather than
         * the pre-pump snapshot taken above. */
        openpete_host_input_publish_sdl();
        pad_restamp_if_due(0);
        openpete_host_input_sample(
            (openpete_host_input_modstate() & SDL_KMOD_ALT) != 0);
    }
    openpete_host_input_latch(it->ovr_claimed);
    it->vbl.latch_ns   = openpete_host_input_latched_sample_ns();
    it->vbl.latch_seq  = it->ovr_claimed ? OP_PACE_LATCH_CLAIMED
                                         : openpete_host_input_latched_seq();
    it->vbl.latch_bits = openpete_host_input_latched_bits();
    PsyX_Pad_InternalPadUpdates();
    openpete_host_input_parity_check(it->ovr_claimed);
}

/** @brief Stage pad slots 1-3 of this tick's input record: the latched devices, the movie's second player, and the synthetic stream.
 *
 * Slots from the latch are absent when no device drives the slot, and
 * absent whenever a higher-priority writer claimed the channel: under bk2
 * replay or the settings menu the extra players are driven by the movie,
 * not by whatever is plugged in.
 * @param bk2 The session's bk2 movie. */
static void input_stage_extra_slots(const host_bk2_t* bk2) OP_REQUIRES(sim)
{
    for (int sl = 1; sl < OPENPETE_PAD_SLOTS; sl++) {
        openpete_pad_snapshot_t sn;
        if (!openpete_host_input_latched_slot(sl, &sn)) continue;
        /* Snapshot axes are PSX pad->analog order; the log stores bk2. */
        const unsigned char slot_axes[4] = {
            sn.axes[2], sn.axes[3], sn.axes[0], sn.axes[1] };
        input_log_stage_port(sl, sn.bits, slot_axes);
    }
    /* Under bk2 replay the extra player comes from the movie, not from
     * whatever is plugged in: the latch above was cleared when the movie
     * claimed the channel. Movie axes are already in the log's bk2
     * order, so they need no reordering. A movie declaring one port
     * leaves the slot absent, which is what it recorded. */
    if (bk2->active && bk2->movie.players >= 2 && bk2->movie.p2_frames) {
        int bf2 = bk2_feed_row();
        if (bf2 >= 0 && bf2 < bk2->movie.frame_count) {
            const bk2_frame_t* f2 = &bk2->movie.p2_frames[bf2];
            input_log_stage_port(1, f2->buttons, f2->axes);
        }
    }

    /* The harness overrides live devices so the N-port carry stays
     * testable on a machine with one pad, or none. */
    if (synth_pads_enabled() == 1) {
        for (int sl = 1; sl < OPENPETE_PAD_SLOTS; sl++) {
            uint16_t b; unsigned char ax[4];
            synth_pad_for(openpete_frame, sl, &b, ax);
            input_log_stage_port(sl, b, ax);
        }
    }
}

/* Documented at the declaration in host_input_tick.h. */
void host_input_latch(const host_session_t* S, host_iter_t* it) OP_REQUIRES(sim)
{
    input_pump_and_latch(S->split, it);

    /* Stage this tick's input record. Every override writer - bk2
     * replay, the settings menu, devui, the live host latch - has
     * stamped the channel by now, so slot 0's staged bytes are the ones
     * the tick is about to consume, published early enough for a
     * consumer running INSIDE the tick to read them. Slots 1-3 stay
     * empty until host sampling fills them.
     *
     * An inactive override means PsyCross decodes live inside the tick
     * and there is nothing to stage yet; the post-yield readback
     * (host_input_commit) fills slot 0 in that case. */
    input_log_stage_reset(openpete_frame);
    /* Before the game calls PadInitDirect there is no pad buffer, so the
     * post-yield readback reports PsyCross's no-buffer default rather
     * than anything the host stamped. Staging that same default keeps
     * the staged record equal to what the log will carry, instead of
     * publishing a host value the guest has nowhere to receive. */
    const bool p1_ready = PsyX_Pad_P1Ready() != 0;
    it->p1_staged = p1_ready && g_PsyX_Pad_OverrideActive != 0;
    if (!p1_ready) {
        const unsigned char no_buffer[4] = { 128, 128, 128, 128 };
        input_log_stage_port(0, 0xFFFF, no_buffer);
    }
    input_stage_extra_slots(&S->bk2);
    if (it->p1_staged) {
        /* PSX pad->analog order is RX,RY,LX,LY; the log stores bk2
         * order, LX,LY,RX,RY. */
        const unsigned char staged_axes[4] = {
            (unsigned char)g_PsyX_Pad_AnalogOverride[2],
            (unsigned char)g_PsyX_Pad_AnalogOverride[3],
            (unsigned char)g_PsyX_Pad_AnalogOverride[0],
            (unsigned char)g_PsyX_Pad_AnalogOverride[1],
        };
        input_log_stage_port(0, (uint16_t)g_PsyX_Pad_InputOverride,
                             staged_axes);
    }
}

/** @brief Count the slot-0 staging check and the synthetic-stream check of one ticked iteration.
 *
 * The guest readback stays the truth the log stores for slot 0. When the
 * override was stamped pre-yield the staged bytes are that same value
 * published early, and the counter proves it: a nonzero mismatch means an
 * in-tick slot-0 read saw bytes the log never recorded, which would desync
 * replay. The first mismatch is kept for the exit report.
 * @param p1_staged Slot 0 was staged before the yield.
 * @param frame_before_yield openpete_frame before the yield, the VBL the synthetic stream was staged for.
 * @param bits Player 1 pad bits the guest read.
 * @param bk2axes Player 1 axes the guest read, in the log's order. */
static void input_census_staging(bool p1_staged, unsigned frame_before_yield,
                                 uint16_t bits, const unsigned char bk2axes[4])
{
    if (p1_staged) {
        const openpete_input_port_t* st = &input_log_staged()->port[0];
        s_p1_stage_checks++;
        if (st->bits != bits || memcmp(st->axes, bk2axes, 4) != 0) {
            if (s_p1_stage_mismatches == 0) {
                s_p1_stage_first_frame = openpete_frame;
                s_p1_stage_first_staged_bits = st->bits;
                s_p1_stage_first_read_bits = bits;
                memcpy(s_p1_stage_first_staged_axes, st->axes, 4);
                memcpy(s_p1_stage_first_read_axes, bk2axes, 4);
            }
            s_p1_stage_mismatches++;
        }
    }
    if (synth_pads_enabled()) {
        s_synth_stage_checks++;
        if (!synth_pads_match(input_log_staged(), frame_before_yield))
            s_synth_stage_mismatches++;
    }
}

/** @brief Count the VBLs each staged slot was present for and each slot pair carried different bytes.
 *
 * @param ticks VBLs the iteration consumed. */
static void input_census_slots(unsigned ticks)
{
    for (int sl = 0; sl < OPENPETE_PAD_SLOTS; sl++)
        if (input_log_staged()->port[sl].present)
            s_slot_present_vbls[sl] += ticks;
    for (int a = 0; a < OPENPETE_PAD_SLOTS; a++) {
        const openpete_input_port_t* pa = &input_log_staged()->port[a];
        if (!pa->present) continue;
        for (int b = a + 1; b < OPENPETE_PAD_SLOTS; b++) {
            const openpete_input_port_t* pb = &input_log_staged()->port[b];
            if (!pb->present) continue;
            if (pa->bits != pb->bits ||
                memcmp(pa->axes, pb->axes, 4) != 0)
                s_slot_differed[a][b] += ticks;
        }
    }
}

/* Documented at the declaration in host_input_tick.h. */
void host_input_commit(const host_iter_t* it) OP_REQUIRES(sim)
{
    /* Per-VBL input log - ALWAYS on: one entry
     * per VBL the fiber consumed this iteration, all carrying the
     * tick-latched bits (the same expansion the bk2 recorder used
     * before the lift-out, so recorded movies stay byte-identical).
     * Consumers: bk2_record (window [begin,finalize)), the rewind
     * replay and the runahead inject drivers. */
    unsigned ticks = openpete_frame - it->frame_before_yield;
    unsigned char bk2axes[4];
    const uint16_t bits = pad_p1_readback(bk2axes);
    input_census_staging(it->p1_staged, it->frame_before_yield, bits, bk2axes);
    input_log_stage_p1_readback(bits, bk2axes);
    input_census_slots(ticks);
    input_log_commit_yields(ticks);
}

#endif /* OPENPETE_HAS_PSYCROSS */
