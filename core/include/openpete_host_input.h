#ifndef OPENPETE_HOST_INPUT_H
#define OPENPETE_HOST_INPUT_H

/* Host-owned input decode.
 *
 * openpete owns *decode* (SDL -> a pad snapshot); PsyCross's override channel
 * still owns *injection* (the byte-exact write into the PSX pad buffer). The
 * two halves are deliberately split into a producer and a consumer with a
 * snapshot between them:
 *
 *   producer   openpete_host_input_sample()   pump-side: reads SDL, publishes
 *   consumer   openpete_host_input_latch()    sim-side: arbitrates, stamps
 *
 * Unsplit, both run on the host loop, back to back. Under the thread split
 * the producer runs on the main thread's pump and the consumer on the host
 * loop's worker thread; the snapshot is the only handoff between them, and
 * the consumer never reads SDL.
 *
 * Decode itself is PsyX_Pad_DecodeP1 - the same merge the non-override path
 * uses - so host-decoded bits are identical to PsyCross-decoded bits by
 * construction rather than by comparison.
 */

#include <stdbool.h>
#include <stdint.h>

#include "openpete_host_mouse.h"

typedef struct openpete_pad_snapshot {
    unsigned short bits;      /* active-low, released = 1 */
    unsigned char  axes[4];   /* PSX pad->analog order: RX, RY, LX, LY */
    bool           valid;
} openpete_pad_snapshot_t;

/** @brief Return 1 when OPENPETE_INPUT_LEGACY selects the path where PsyCross pumps, decodes and injects input itself.
 *
 * The variable is read on the first call and cached; a non-empty value not
 * starting with '0' selects the path. It is a diagnostic A/B against the
 * host-owned path. On the legacy path the mouse channel sees no events: the
 * snapshot carries SDL's own position and button mask, and the press,
 * motion and wheel totals never advance.
 * @return 1 on the legacy path, else 0. */
int openpete_host_input_legacy(void);

/** @brief Load the controller-mapping databases and log what each attached pad resolved to.
 *
 * Call once, after the gamepad subsystem is up: a mapping added before it
 * exists is discarded, and one added after still reaches a pad that is
 * already attached. Runs on both input paths.
 * @param cli_path --controller-db, NULL or empty when unset; the
 *        OPENPETE_CONTROLLER_DB environment variable is the fallback. */
void openpete_host_input_load_mappings(const char* cli_path);

/** @brief Disable the SDL event types no openpete, PsyCross or imgui consumer reads: joystick and gamepad UPDATE_COMPLETE.
 *
 * SDL_UpdateJoysticks pushes an UPDATE_COMPLETE per joystick with new state
 * on every pump, and on every pump while a Guide release shorter than
 * SDL_MINIMUM_GUIDE_BUTTON_DELAY_MS (250 ms) is held back, although that
 * pump sends no button event; the pump loop then dequeues one pair per
 * iteration for the whole delay. A disabled type is never queued
 * (SDL_UpdateJoysticks tests SDL_EventEnabled before the push). Call on the
 * pumping thread after SDL_Init; the setting lasts until the events
 * subsystem quits. */
void openpete_host_input_drop_unread_events(void);

/** @brief Log one arriving device's mapping provenance, for pads not attached when the databases loaded.
 *
 * Called on SDL_EVENT_JOYSTICK_ADDED, not GAMEPAD_ADDED: the latter fires
 * only for a device SDL can already map.
 * @param joystick_id SDL joystick instance id. */
void openpete_host_input_pad_arrived(unsigned int joystick_id);

/** @brief Producer: decode live SDL input as of the last event pump and publish it as one pad sample.
 *
 * Slot 0 comes from OPENPETE_INPUT_SYNTH's synthetic pattern when that is
 * on, else under the input lock from the player-1 gamepad only when it is
 * an SDL virtual gamepad, else from PsyX_Pad_DecodeP1. The timed pulse
 * source (OPENPETE_INPUT_PULSE) is applied over slot 0 last. Slots 1-3 are
 * decoded from their own gamepads, overlaid with their [keys.padN]
 * bindings when any are bound; a slot with no device publishes invalid.
 * Each slot's device instance id is stored for the rumble pump. A pad-bit
 * or axis change against the previous sample is logged to the pace log
 * with the publish instant and sequence number. Also advances the
 * OPENPETE_INPUT_VGAMEPAD virtual pads.
 * @param alt_held true suppresses the PsyX_Pad_DecodeP1 decode of slot 0
 *        the way PsyX_UpdateInput's Alt gate does, so the Enter of
 *        Alt+Enter cannot register as Start; slot 0 then publishes neutral
 *        rather than going stale. Slots 1-3, the synthetic source and the
 *        input-lock path ignore it. */
void openpete_host_input_sample(bool alt_held);

/** @brief Publish a neutral pad sample so the consumer has a valid slot 0 to stamp before the producer's first real sample.
 *
 * Required before starting a consumer on another thread: without it the
 * first latch leaves the override inactive and PsyX_Pad_InternalPadUpdates
 * falls through to its live path, which pumps SDL events from a thread
 * that does not own video. Slots 1-3 publish invalid. */
void openpete_host_input_seed_neutral(void);

/** @brief Consumer: stamp the published slot-0 snapshot into the PsyCross override channel and record the latched sample.
 *
 * Live decode is the lowest-priority override writer and fills the channel
 * whenever nothing else claims it, so the override is always active and
 * the host is always its source. When claimed, the channel is left to the
 * other writer and the latch record (publish instant, sequence, every
 * slot's snapshot) is cleared, so this tick reports no consumed sample.
 * When the seqlock read fails or slot 0 is invalid, nothing is stamped and
 * the previous record stands. Slots 1-3 are latched but never stamped.
 * @param claimed true when a higher-priority writer (bk2 replay, the open
 *        settings menu, the dev overlay while it owns the keyboard) holds
 *        the channel this iteration. */
void openpete_host_input_latch(bool claimed);

/* ---- host SDL input-state snapshot ------------------------------------- */
/** @brief Producer: publish a snapshot of SDL keyboard, modifier, mouse and per-slot gamepad state.
 *
 * Everything that reads live SDL keyboard or gamepad state reads this
 * snapshot instead. The SDL arrays are refreshed by the event pump, which
 * under the thread split runs on a different thread from the consumers.
 * The snapshot is double-buffered with an atomic index, each buffer a
 * seqlock, so every accessor's read comes from one publish. Injected raw keys are merged before the
 * input lock blanks the keyboard and modifiers; marked injected keys are
 * merged after it. On the legacy input path the mouse fields come from
 * SDL_GetMouseState. Every accessor below is consumer-side and never
 * touches SDL. */
void openpete_host_input_publish_sdl(void);

/** @brief Set or clear one injection-script key's held state in every snapshot published from now on.
 *
 * Pump thread only. An out-of-range scancode is ignored.
 * @param scancode SDL scancode, 1 .. SDL_SCANCODE_COUNT - 1.
 * @param down nonzero to hold, zero to release.
 * @param raw nonzero files the key with the physical keyboard, which the
 *        input lock blanks; zero makes it a marked injected key the lock
 *        leaves. */
void openpete_host_input_inject_key(int scancode, int down, int raw);

/** @brief Return whether a keyboard key is held in the published SDL snapshot.
 *
 * Injected keys are included; the input lock blanks the physical keyboard.
 * @param scancode SDL scancode.
 * @return 1 when held, 0 when released or the scancode is outside 1 ..
 *        SDL_SCANCODE_COUNT - 1. */
int      openpete_host_input_key_down(int scancode);
/** @brief Return whether a gamepad button is held on pad slot 0 in the published snapshot.
 *
 * @param button SDL_GamepadButton index.
 * @return 1 when held, 0 when released, out of range, or slot 0 has no
 *         device. */
int      openpete_host_input_pad_button(int button);
/** @brief Return a gamepad axis value on pad slot 0 in the published snapshot.
 *
 * @param axis SDL_GamepadAxis index.
 * @return the raw SDL axis value, or 0 when out of range or slot 0 has no
 *        device. */
int      openpete_host_input_pad_axis(int axis);

/* The same reads, addressed by pad slot. Slot 0 is what the unqualified forms
 * above return; slots 1-3 are the additional connected gamepads, in the order
 * host_slot_gamepads assigns them. A slot with no device reads as all-released
 * and centred, which openpete_host_input_pad_connected distinguishes from a
 * device that is genuinely idle. */
/** @brief Return whether a gamepad button is held on a pad slot in the published snapshot.
 *
 * @param slot pad slot, 0 .. OPENPETE_PAD_SLOTS - 1.
 * @param button SDL_GamepadButton index.
 * @return 1 when held, 0 when released, out of range, or the slot has no
 *         device. */
int      openpete_host_input_pad_button_slot(int slot, int button);
/** @brief Return a gamepad axis value on a pad slot in the published snapshot.
 *
 * @param slot pad slot, 0 .. OPENPETE_PAD_SLOTS - 1.
 * @param axis SDL_GamepadAxis index.
 * @return the raw SDL axis value, or 0 when an argument is out of range or the
 *        slot has no device. */
int      openpete_host_input_pad_axis_slot(int slot, int axis);
/** @brief Return whether a pad slot had a gamepad when the snapshot was published.
 *
 * A connected pad holding nothing reads 1, which separates it from an empty
 * slot. Physical gamepads are absent from the snapshot while the input lock is
 * on.
 * @param slot pad slot, 0 .. OPENPETE_PAD_SLOTS - 1.
 * @return 1 when a device drove the slot, 0 when none did or the slot is out of
 *        range. */
int      openpete_host_input_pad_connected(int slot);
/** @brief Return the SDL_GamepadType of a pad slot's device in the published snapshot.
 *
 * Names the controller family whose button labels the player reads.
 * @param slot pad slot, 0 .. OPENPETE_PAD_SLOTS - 1.
 * @return the type, or SDL_GAMEPAD_TYPE_UNKNOWN (0) for an empty slot or one
 *        out of range. */
int      openpete_host_input_pad_type(int slot);

/** @brief Accept or refuse gamepad input while the window does not have focus (--pad-unfocused).
 *
 * Sets SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS at override priority, so
 * SDL keeps delivering every gamepad's button and axis events to an
 * unfocused window, and lets the rumble path drive the motors while
 * unfocused. SDL reads the hint through a hint callback, so any thread may
 * call this, before or after the gamepad subsystem starts. The input lock
 * still hides physical pads.
 * @param on nonzero accepts unfocused pad input; 0 restores SDL's default
 *        of dropping it. */
void     openpete_host_input_set_pad_unfocused(int on);
/** @brief Return whether gamepad input is accepted while the window is unfocused.
 *
 * @return 1 after openpete_host_input_set_pad_unfocused(1), else 0. */
int      openpete_host_input_pad_unfocused(void);
/** @brief Return the SDL keyboard modifier mask of the published snapshot.
 *
 * The mask is SDL_GetModState at the publish, or 0 while the input lock is on.
 * @return the SDL_Keymod bit mask. */
unsigned openpete_host_input_modstate(void);

/* The mouse half of the same snapshot. `button_index` is
 * OPENPETE_MOUSE_BTN_*. */
/** @brief Copy the mouse half of the published snapshot.
 *
 * The state is event-derived (position in window points, window size in
 * points, button mask, in-window flag), copied from the mouse channel at
 * publish.
 * @param out receives the state; NULL is ignored. */
void     openpete_host_input_mouse(openpete_mouse_state_t* out);
/** @brief Return whether a mouse button is held in the published snapshot.
 *
 * @param button_index OPENPETE_MOUSE_BTN_*.
 * @return 1 when held, 0 when released or out of range. */
int      openpete_host_input_mouse_button(int button_index);

/** @brief Copy slot 0 of the last published pad snapshot.
 *
 * A diagnostic accessor with no caller at present; the parity instrument
 * compares against the latched sample instead.
 * @param out receives the snapshot, valid false when nothing was published
 *        or the seqlock retry bound was exhausted; NULL is ignored. */
void openpete_host_input_last(openpete_pad_snapshot_t* out);

/** @brief Copy the snapshot this iteration's latch consumed for one pad slot.
 *
 * Slot 0 is the sample stamped into the override channel; slots 1-3 are
 * latched for the input log and reach the guest only through a mod.
 * @param slot pad slot, 0 .. OPENPETE_PAD_SLOTS - 1.
 * @param out receives the snapshot; valid is false for an out-of-range slot
 *        or one no device drove.
 * @return 1 when a device drove the slot, else 0 (also for NULL out). */
int openpete_host_input_latched_slot(int slot, openpete_pad_snapshot_t* out);

/** @brief Return how many producer samples decoded a slot through its [keys.padN] bindings rather than the shared controller merge alone.
 *
 * Zero on a re-mapped slot means the re-map never took, which no
 * comparison of the resulting bytes can distinguish from a re-map that
 * agrees with the default. Slot 0 is decoded by the merge only and always
 * reads 0.
 * @param slot pad slot, 0 .. OPENPETE_PAD_SLOTS - 1.
 * @return the count, or 0 for an out-of-range slot. */
unsigned long long openpete_host_input_slot_keymap_decodes(int slot);

/** @brief Return op_now() at the publish of the last pad snapshot.
 *
 * Under the thread split the consumer runs on a different thread from the
 * producer, so input staleness is nonzero and this timestamp measures it.
 * @return the publish instant in nanoseconds, or 0 before the first publish
 *         or when the seqlock retry bound was exhausted. */
int64_t openpete_host_input_last_sample_ns(void);

/** @brief Return the publish instant of the snapshot the last latch consumed, captured at the latch.
 *
 * A tick's input staleness is measured against this, not against
 * openpete_host_input_last_sample_ns: the live value read after the tick
 * has run names a sample the tick never saw.
 * @return nanoseconds, or 0 when nothing has latched or the last latch was
 *         claimed (the legacy input path never latches, so its staleness is
 *         unmeasured). */
int64_t openpete_host_input_latched_sample_ns(void);

/** @brief Return the sequence number of the sample the last latch consumed.
 *
 * A timestamp only says a sample was published before a tick ran; the
 * sequence says which sample the tick took, so a pulse the latch never
 * picked up is countable instead of being credited to the next tick.
 * Sequence numbers are even and increase by 2 per publish; odd values are
 * the seqlock's in-flight state.
 * @return the sequence number, or 0 before the first latch or when the last
 *         latch was claimed by a higher-priority writer. */
unsigned openpete_host_input_latched_seq(void);

/** @brief Return the slot-0 pad bits of the sample the last latch consumed.
 *
 * Lets a caller verify a specific edge was consumed rather than superseded.
 * @return the active-low bits, or 0xFFFF (all released) when the latched slot-0
 *         sample is invalid. */
unsigned short openpete_host_input_latched_bits(void);

/** @brief Return how many times a pad-sample read exhausted the seqlock retry bound.
 *
 * Exhaustion means the producer was unavailable for the whole retry
 * window, not merely mid-publish. Expected to stay 0. A non-zero count
 * means some read kept the previous sample instead of a fresh one; a long
 * preemption of the pump thread produces it without a defect. The pace
 * log summary prints the count when non-zero.
 * @return the failure count since start. */
unsigned openpete_host_input_seqlock_failures(void);

/** @brief Return the timed pulse source's schedule size, OPENPETE_INPUT_PULSE, or 0 when the source is off.
 *
 * The env vars are read on the first call of any pulse accessor and
 * latched; a negative or absent value turns the source off. The pace log
 * header records it so the analyzer can check the consumed press count
 * against the injected count, and treat a log with no pulses as carrying
 * no sweep result.
 * @return The pulse count, 0 when the source is off. */
int openpete_host_input_pulse_count(void);
/** @brief Return how many press instants of the pulse schedule have passed.
 *
 * A run shorter than the schedule reaches fewer than
 * openpete_host_input_pulse_count.
 * @return The number of pulses whose press instant has passed. */
int openpete_host_input_pulses_fired(void);

/** @brief Return the pad bit the timed pulse source presses, OPENPETE_INPUT_PULSE_BIT (default 0x0100, L2).
 *
 * The pace log header records it so the analyzer can separate injected
 * presses from other presses.
 * @return the bit mask, or 0 when the source is off. */
unsigned short openpete_host_input_pulse_bit(void);

/** @brief Return how long each timed pulse is held, OPENPETE_INPUT_PULSE_HOLD_US (default 20000).
 *
 * The pace log header records it. A press lost while the latch interval
 * was shorter than the hold was available and skipped, which is a defect;
 * one lost across an interval longer than the hold is sampling working as
 * specified.
 * @return the hold in microseconds, or 0 when the source is off. */
int openpete_host_input_pulse_hold_us(void);

/* ---- rumble recorder (vibration oracle) --------------------------------
 *
 * Live only with OPENPETE_INPUT_VGAMEPAD: the virtual pad's descriptor carries
 * a Rumble callback, which makes every motor command the whole chain delivers
 * observable from a headless run. Vibration has no pixels and no gameplay-state
 * bytes, so this is the only instrument that can see it at all.
 *
 * `_live` is the gate on believing any of the others. A zero motor count means
 * nothing on its own - it reads the same whether nothing rumbled or the
 * recorder was never wired - so a self-probe issues one deliberate non-zero
 * pair at attach and asserts it arrived. Check `_live` before treating a zero
 * as evidence; a harness that skips it is reporting a vacuous pass.
 *
 * Counted per virtual pad, `pad` indexing the pads OPENPETE_INPUT_VGAMEPAD
 * attached (0 is the one PsyCross takes as player 1). A single global counter
 * cannot answer which device a command reached, which is the whole question
 * once more than one slot can be driven: one pad's commands would be
 * indistinguishable from another's. Each pad carries its own callback,
 * counters and self-probe. */
/** @brief Return how many Rumble callbacks one virtual pad received after its self-probe.
 *
 * The counter is zeroed after the attach-time self-probe, so it counts
 * only commands the actuator path delivered.
 * @param pad virtual pad index, 0 .. OPENPETE_PAD_SLOTS - 1.
 * @return the callback count, or 0 for an out-of-range index. */
int  openpete_host_input_vpad_rumble_calls(int pad);
/** @brief Return how many non-zero Rumble callbacks one virtual pad received after its self-probe.
 *
 * Zeroed with the call count after the attach-time self-probe.
 * @param pad virtual pad index, 0 .. OPENPETE_PAD_SLOTS - 1.
 * @return the callback count, or 0 for an out-of-range index. */
int  openpete_host_input_vpad_rumble_nonzero(int pad);
/** @brief Return whether one virtual pad's recorder received the attach-time self-probe pair.
 *
 * The flag latches when the Rumble callback sees the probe pair and is not
 * cleared afterwards; a zero here means the recorder is not wired and every
 * other count from that pad is void.
 * @param pad virtual pad index, 0 .. OPENPETE_PAD_SLOTS - 1.
 * @return 1 when the probe arrived, else 0 (also for an out-of-range index). */
int  openpete_host_input_vpad_rumble_live(int pad);
/** @brief Report the motor pair of the last Rumble callback one virtual pad received.
 *
 * Zero until a callback arrives after the self-probe. An out-of-range pad
 * reports 0, 0.
 * @param pad virtual pad index, 0 .. OPENPETE_PAD_SLOTS - 1.
 * @param low receives the large-motor value; NULL is ignored.
 * @param high receives the small-motor value; NULL is ignored. */
void openpete_host_input_vpad_rumble_last(int pad, unsigned* low, unsigned* high);
/** @brief Hold or release one button of the first virtual pad from the injection script.
 *
 * The first call switches the virtual pads from their synthetic pattern to
 * the scripted state for the rest of the run: buttons and axes as scripted
 * (openpete_host_input_vpad_script_axis), axes centred until scripted. Pump
 * thread only, where the script steps and the pads are driven.
 * A button outside the SDL gamepad button range is ignored.
 * @param button an SDL_GamepadButton.
 * @param down nonzero to hold, 0 to release. */
void openpete_host_input_vpad_script_button(int button, int down);
/** @brief Set one axis of the first virtual pad from the injection script.
 *
 * Switches the virtual pads to the scripted state as
 * openpete_host_input_vpad_script_button does. Pump thread only. An axis
 * outside the SDL gamepad axis range is ignored.
 * @param axis an SDL_GamepadAxis.
 * @param value the axis value, clamped to -32768..32767. */
void openpete_host_input_vpad_script_axis(int axis, int value);

/** @brief Return how many OPENPETE_INPUT_VGAMEPAD virtual pads are open.
 *
 * @return the count, 0 when the variable is unset or attach failed. */
int  openpete_host_input_vpad_count(void);

/** @brief Return the SDL joystick instance id driving a pad slot, as stored by the last producer sample.
 *
 * An id, not a handle: these gamepads are opened and pruned on the
 * sampling thread while the rumble pump runs on another, so the pump
 * resolves the id with SDL_GetGamepadFromID at drive time and a prune in
 * between reads as NULL rather than as a use-after-close.
 * @param slot pad slot, 1 .. OPENPETE_PAD_SLOTS - 1.
 * @return the instance id, or 0 for an empty or out-of-range slot (slot 0
 *         included). */
unsigned openpete_host_input_slot_device_id(int slot);

/** @brief Arm the input parity instrument regardless of OPENPETE_INPUT_PARITY.
 *
 * The instrument is otherwise armed by OPENPETE_INPUT_PARITY set to a
 * non-empty value not starting with '0', read once; no caller invokes
 * this function and no command-line flag reaches it. When armed,
 * openpete_host_input_parity_check compares the sample the iteration
 * latched against the bits and axes that reached the PSX pad buffer on
 * iterations no other writer claimed, and counts mismatches, stale
 * latches (a sequence number repeated from the previous latch) and
 * engaged iterations (a button pressed or an axis more than 1 from 128);
 * openpete_host_input_parity_report returns -1 at zero engagement. */
void openpete_host_input_parity_enable(void);
/** @brief Compare the latched pad sample against the bits and axes in the PSX pad buffer.
 *
 * Does nothing when the instrument is not armed, when `claimed`, before
 * the game registers a pad buffer, or when the iteration latched no
 * valid sample.
 * @param claimed True when a bk2 movie or the open settings menu drives the pad override this iteration. */
void openpete_host_input_parity_check(bool claimed);
/** @brief Log the parity instrument's verdict and return it.
 *
 * Stale latches are logged beside the verdict and do not change it.
 * @return 1 on pass or when the instrument is not armed, 0 when mismatches
 *         were seen, -1 when no iteration carried input (vacuous). */
int  openpete_host_input_parity_report(void);

#endif /* OPENPETE_HOST_INPUT_H */
