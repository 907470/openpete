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
 * Today both run on the host loop, back to back. The split exists because the
 * pump and the sim are destined for different threads, and a snapshot is the
 * only handoff that survives that move unchanged: the consumer never reads SDL.
 *
 * Decode itself is PsyX_Pad_DecodeP1 — the same merge the non-override path
 * uses — so host-decoded bits are identical to PsyCross-decoded bits by
 * construction rather than by comparison.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct openpete_pad_snapshot {
    unsigned short bits;      /* active-low, released = 1 */
    unsigned char  axes[4];   /* PSX pad->analog order: RX, RY, LX, LY */
    bool           valid;
} openpete_pad_snapshot_t;

/* 1 when OPENPETE_INPUT_LEGACY selects the pre-ownership path (PsyCross pumps,
 * decodes and injects). Diagnostic A/B only. */
int openpete_host_input_legacy(void);

/* Loads the controller-mapping databases and logs what each attached pad
 * resolved to. Call once, after the gamepad subsystem is up: a mapping added
 * before it exists is discarded, and one added after still reaches a pad that
 * is already attached. `cli_path` is --controller-db, NULL when unset (the
 * OPENPETE_CONTROLLER_DB environment variable is the fallback).
 *
 * Runs on both input paths — the legacy one needs mappings just as much. */
void openpete_host_input_load_mappings(const char* cli_path);

/* Reports one device's mapping provenance as it arrives, for the pads that
 * were not attached when the databases loaded. Hung off JOYSTICK_ADDED, not
 * GAMEPAD_ADDED: the latter fires only for a device SDL can already map. */
void openpete_host_input_pad_arrived(unsigned int joystick_id);

/* Producer. Decodes live SDL input (as of the last event pump) and publishes
 * it. `alt_held` suppresses the sample the way PsyX_UpdateInput's own Alt gate
 * does, so Alt+Enter cannot register as Start; a suppressed sample republishes
 * the neutral pad rather than going stale. */
void openpete_host_input_sample(bool alt_held);

/* Publish a neutral snapshot so the consumer has something valid to stamp
 * before the producer's first real sample. Required before starting a
 * consumer on another thread: without it the first latch leaves the override
 * inactive and the injection path falls back to reading SDL directly, on the
 * wrong thread. */
void openpete_host_input_seed_neutral(void);

/* Consumer. Stamps the published snapshot into the PsyCross override channel.
 * A no-op when `claimed` is set — that means a higher-priority override writer
 * (bk2 replay, the settings menu, the dev overlay, rewind) already owns the
 * channel this iteration. Live decode is the lowest-priority writer: it fills
 * the channel whenever nothing else claims it, which is what turns the
 * former "override inactive -> PsyCross polls SDL itself" fallback into
 * "override always active, host always the source". */
void openpete_host_input_latch(bool claimed);

/* ---- host SDL input-state snapshot -------------------------------------
 *
 * Everything that reads live SDL keyboard/gamepad state does so through this
 * snapshot instead of calling SDL directly. Same producer/consumer split as
 * the pad above and for the same reason: those SDL arrays are refreshed by
 * the event pump, and the pump is moving to a different thread from the host
 * loop that consumes them. A snapshot is the handoff that survives the move.
 *
 * Double-buffered with an atomic index, so a consumer always reads one
 * internally consistent sample rather than a half-updated one.
 *
 * Publishing is a producer-side (pump) act; every accessor below is
 * consumer-side and never touches SDL. */
void openpete_host_input_publish_sdl(void);

int      openpete_host_input_key_down(int scancode);
int      openpete_host_input_pad_button(int button);
int      openpete_host_input_pad_axis(int axis);

/* The same reads, addressed by pad slot. Slot 0 is what the unqualified forms
 * above return; slots 1-3 are the additional connected gamepads, in the order
 * host_slot_gamepads assigns them. A slot with no device reads as all-released
 * and centred, which openpete_host_input_pad_connected distinguishes from a
 * device that is genuinely idle. */
int      openpete_host_input_pad_button_slot(int slot, int button);
int      openpete_host_input_pad_axis_slot(int slot, int axis);
int      openpete_host_input_pad_connected(int slot);
unsigned openpete_host_input_modstate(void);

/* Last published snapshot (diagnostics / the parity instrument). */
void openpete_host_input_last(openpete_pad_snapshot_t* out);

/* The snapshot this iteration's latch consumed for one pad slot. Returns 1
 * when a device drove the slot, 0 otherwise (with `out->valid` false). Slot 0
 * is the sample that was stamped into the override channel; slots 1-3 are
 * latched for the input log and reach the guest only through a mod. */
int openpete_host_input_latched_slot(int slot, openpete_pad_snapshot_t* out);

/* VBLs `slot` decoded through its [keys.padN] bindings rather than the shared
 * controller merge. Zero on a slot the user re-mapped means the re-map never
 * took, which no comparison of the resulting bytes can distinguish from a
 * re-map that happens to agree with the default. */
unsigned long long openpete_host_input_slot_keymap_decodes(int slot);

/* op_now() at the moment the last snapshot was published, or 0 before the
 * first one. The consumer runs on a different thread from the producer, so
 * "how stale was the input this tick consumed" is a real quantity now rather
 * than zero by construction; this is what makes it measurable. */
int64_t openpete_host_input_last_sample_ns(void);

/* Publish instant of the snapshot the last latch actually CONSUMED, captured
 * at the latch. This, not the one above, is what a tick's input staleness is
 * measured against: reading the live value after the tick has run names a
 * sample the tick never saw. Zero when nothing has latched (the legacy input
 * path never latches, and its staleness is therefore unmeasured, not zero). */
int64_t openpete_host_input_latched_sample_ns(void);

/* Sequence number of the sample the last latch consumed, or 0 if that
 * iteration consumed none (a higher-priority writer had claimed the pad).
 *
 * This is what makes an input measurement attributable. A timestamp can only
 * say a sample was published before a tick ran; the sequence says WHICH
 * sample the tick actually took, so a pulse the latch never picked up is
 * countable instead of being credited to whichever tick happened to follow
 * it. Sequence numbers are even and increase by 2 per publish (the odd
 * values are the seqlock's in-flight state). */
unsigned openpete_host_input_latched_seq(void);

/* Pad bits of that same sample, so a caller can verify a specific edge was
 * consumed rather than merely superseded. */
unsigned short openpete_host_input_latched_bits(void);

/* Times the consumer's seqlock retry bound was exhausted — the producer was
 * unavailable for the whole retry window, not merely mid-publish. Expected to
 * stay 0. A non-zero count means some tick reused the previous sample instead
 * of a fresh one, which is not necessarily a bug (a long preemption of the
 * pump thread produces it) but is never nothing, so it is surfaced. */
unsigned openpete_host_input_seqlock_failures(void);

/* Timed pulse source (M5 Tier A), OPENPETE_INPUT_PULSE=<count>. Count is the
 * schedule's size; `fired` is how many press instants the run actually reached
 * (a short run reaches fewer). The analyzer needs both: "every injected pulse
 * was consumed" is only checkable against the number that was injected, and a
 * log with none must not be allowed to report a passing sweep. */
int openpete_host_input_pulse_count(void);
int openpete_host_input_pulses_fired(void);

/* Which pad bit the pulse source drives, so the analyzer can separate injected
 * presses from whatever else happened to be pressed. 0 when the source is off. */
unsigned short openpete_host_input_pulse_bit(void);

/* How long each pulse is held. The analyzer needs it to tell a plumbing defect
 * from a truth: a press lost while the latch interval was SHORTER than the
 * hold was available and skipped, which is a bug; one lost across an interval
 * longer than the hold is input sampling working as specified. */
int openpete_host_input_pulse_hold_us(void);

/* ---- rumble recorder (vibration oracle) --------------------------------
 *
 * Live only with OPENPETE_INPUT_VGAMEPAD: the virtual pad's descriptor carries
 * a Rumble callback, which makes every motor command the whole chain delivers
 * observable from a headless run. Vibration has no pixels and no gameplay-state
 * bytes, so this is the only instrument that can see it at all.
 *
 * `_live` is the gate on believing any of the others. A zero motor count means
 * nothing on its own — it reads the same whether nothing rumbled or the
 * recorder was never wired — so a self-probe issues one deliberate non-zero
 * pair at attach and asserts it arrived. Check `_live` before treating a zero
 * as evidence; a harness that skips it is reporting a vacuous pass. */
int  openpete_host_input_vpad_rumble_calls(void);
int  openpete_host_input_vpad_rumble_nonzero(void);
int  openpete_host_input_vpad_rumble_live(void);
void openpete_host_input_vpad_rumble_last(unsigned* low, unsigned* high);

/* Parity instrument (--input-parity). Compares what the host actually drove
 * into the pad against a fresh live decode taken in the same iteration, on the
 * frames where live decode is supposed to be the source. Counts engagement
 * (frames carrying real input) and refuses to report a pass at zero, because a
 * run that never pressed anything proves nothing. */
void openpete_host_input_parity_enable(void);
void openpete_host_input_parity_check(bool claimed);
/* Returns 1 = pass, 0 = mismatches seen, -1 = vacuous (nothing engaged). */
int  openpete_host_input_parity_report(void);

#endif /* OPENPETE_HOST_INPUT_H */
