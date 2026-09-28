#include "openpete_host_input.h"
#include "openpete_host_mouse.h"
#include "openpete_input_lock.h"
#include "openpete_input_log.h"
#include "openpete_keymap.h"
#include "openpete_log.h"
#include "openpete_paths.h"
#include "openpete_rumble.h"

#include "PsyX/PsyX_public.h"

#include <SDL3/SDL.h>

#include "openpete_time.h"
#include "openpete_pace_log.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* Neutral pad: all buttons released (active-low), sticks centred. */
#define PAD_NEUTRAL_BITS 0xFFFF

/* The producer/consumer seam, published as ONE sample under a seqlock.
 *
 * Bits, axes and the publish instant have to travel together. Read
 * separately they can belong to different samples, and the damage is not
 * symmetric: a tick can be credited with a timestamp from a sample whose
 * BYTES it never consumed, which under-reports input delay — the flattering
 * direction, on the one quantity the thread split is known to have made
 * worse. Worse, the exposure window is one pump period wide and sits exactly
 * at the latch boundary, which is precisely the feature an input-window
 * measurement reads its answer from.
 *
 * `s_snap_seq` is odd while a publish is in flight and even when the sample
 * is whole; a reader that sees an odd count, or a different count either
 * side of its copy, retries. The producer never blocks inside the critical
 * section (a dozen bytes of stores), so a reader converges immediately. */
typedef struct {
    /* One snapshot per pad slot, published together under one sequence
     * number: the slots are sampled in the same host iteration and a reader
     * that could see slot 0 from this iteration and slot 1 from the previous
     * would attribute a co-op input pair to a tick that never saw it.
     * `snap[i].valid` doubles as "a device drove this slot". */
    openpete_pad_snapshot_t snap[OPENPETE_PAD_SLOTS];
    int64_t                 ns;    /* op_now() at publish */
} host_pad_sample_t;

static host_pad_sample_t   s_snap_live;   /* guarded by s_snap_seq */
static _Atomic unsigned    s_snap_seq;    /* even = stable, odd = publishing */

/* Bounded so a producer preempted mid-publish cannot hang the sim thread. The
 * publish itself is a dozen bytes, so a reader normally converges on the first
 * or second try; what does NOT converge quickly is the case where the OS
 * descheduled the producer between the odd and even counts — a ~10 ns window,
 * but a preemption landing in it lasts milliseconds. Spinning through the
 * whole bound then takes about a microsecond and gives up on a sample that
 * was moments from being whole, so the reader yields once it is clear this is
 * not contention but scheduling. */
#define PAD_SEQLOCK_TRIES     256
#define PAD_SEQLOCK_SPIN      16    /* tries before yielding the CPU */
static _Atomic unsigned s_snap_retry_fail;

/* The last sample the consumer actually copied, plus its identity. Worker-only.
 * `s_latched_seq` is what makes an input measurement attributable: it names
 * WHICH published sample a tick consumed, where a timestamp comparison can
 * only guess from ordering. */
static openpete_pad_snapshot_t s_latched_snap[OPENPETE_PAD_SLOTS];
static int64_t                 s_latched_ns;
static unsigned                s_latched_seq;

/* Synthetic input source (OPENPETE_INPUT_SYNTH=1).
 *
 * The decode itself needs no test: host-decoded bits come from
 * PsyX_Pad_DecodeP1, the same merge the non-override path runs, so the two are
 * identical by construction rather than by comparison. What DOES need proving
 * is the plumbing around it — that the value published this iteration, and not
 * a stale or mis-ordered one, is what reaches the PSX pad buffer, and that the
 * higher-priority override writers still win when they claim the channel.
 *
 * Proving that requires the decoded value to actually vary, which on a headless
 * machine no real keyboard can make it do. This source substitutes a
 * deterministic, frame-indexed pad value for the SDL read so the path can be
 * exercised end to end (and, being a pure function of the frame counter,
 * replays identically across runs). It is a diagnostic, never a play path. */
static int s_synth = -1;

/** @brief Return whether the synthetic pad source is enabled.
 *
 * Reads OPENPETE_INPUT_SYNTH on first use (set, non-empty and not starting with
 * 0 enables it), caches the result and logs once when enabled. Called from
 * openpete_host_input_sample on the pump thread. */
static int synth_on(void)
{
    if (s_synth < 0) {
        const char* e = getenv("OPENPETE_INPUT_SYNTH");
        s_synth = (e && *e && *e != '0') ? 1 : 0;
        if (s_synth)
            OPL_INFO_F("input", "SYNTHETIC input source active — diagnostic only");
    }
    return s_synth;
}

/** @brief Write the synthetic pad value for one VBL count.
 *
 * A rotating single button (Cross, Circle, Square, Triangle, Up, Down,
 * Start) is held for 8 VBLs at a time, followed by one 8-VBL neutral gap
 * per cycle, so both engaged and idle samples occur. While a button is held
 * the left stick sweeps (LX = 78 + count % 100, LY = 178 - count % 100);
 * otherwise every axis is 128.
 * @param frame the VBL counter (openpete_frame).
 * @param bits receives the active-low pad bits.
 * @param axes receives the RX, RY, LX, LY bytes. */
static void synth_pad(unsigned frame, unsigned short* bits, unsigned char axes[4])
{
    static const unsigned short kButtons[] = {
        0x4000, /* Cross    */
        0x2000, /* Circle   */
        0x8000, /* Square   */
        0x1000, /* Triangle */
        0x0010, /* Up       */
        0x0040, /* Down     */
        0x0008, /* Start    */
    };
    unsigned phase = (frame / 8u);
    unsigned slot  = phase % (sizeof(kButtons) / sizeof(kButtons[0]) + 1u);

    *bits = 0xFFFF;
    axes[0] = axes[1] = 128;
    axes[2] = axes[3] = 128;

    if (slot < sizeof(kButtons) / sizeof(kButtons[0])) {
        *bits = (unsigned short)(0xFFFF & ~kButtons[slot]);   /* active-low */
        axes[2] = (unsigned char)(128 + (int)(frame % 100u) - 50);
        axes[3] = (unsigned char)(128 - (int)(frame % 100u) + 50);
    }
}

/* Timed pulse source (M5 Tier A) — OPENPETE_INPUT_PULSE=<count>.
 *
 * The frame-indexed synth source above cannot answer an input-PHASE question:
 * it changes on a frame boundary, so every edge lands at the same point in the
 * grid period and a phase sweep degenerates to one bin. This source schedules
 * presses at wall instants instead, spread across the grid period, which is
 * what makes "which VBL consumes an edge arriving at phase p" measurable.
 *
 * It is in-process, just upstream of the latch: it validates the log plumbing
 * and the analyzer's attribution arithmetic, and NOTHING about the OS. It
 * cannot see the SDL pump, event delivery, focus, or a main thread parked in a
 * modal loop — during such a stall it publishes nothing and its own schedule
 * slides with it, so it would absolve exactly the hazard the thread split
 * introduced. Only an external injector (Tier B) can see that. Accordingly
 * this source must never be the producer behind an end-to-end latency claim.
 *
 * Presses are held for at least one grid period by default, so a pulse the
 * latch misses is a real finding rather than the expected outcome of a press
 * shorter than the sampling interval. Axes are held static: the synth source's
 * sweeping sticks emit an analog edge on nearly every publish, which would
 * bury the pulse edges the measurement is about. */
static int      s_pulse_n = -1;      /* -1 = unresolved, 0 = off */
static int      s_pulse_hold_ns;
static int      s_pulse_gap_ns;
static unsigned short s_pulse_bit;
static int64_t  s_pulse_t0;
static int      s_pulse_fired;       /* pulses whose press instant has passed */

/** @brief Parse the OPENPETE_INPUT_PULSE* variables on first call and report whether the timed pulse source is on.
 *
 * Reads the pulse count, the hold (default 20 ms), the gap (default 117 ms)
 * and the button bit (default 0x0100, L2), and logs the schedule once when
 * the count is positive.
 * @return Non-zero when at least one pulse is configured. */
static int pulse_on(void)
{
    if (s_pulse_n < 0) {
        const char* e = getenv("OPENPETE_INPUT_PULSE");
        s_pulse_n = (e && *e) ? atoi(e) : 0;
        if (s_pulse_n < 0) s_pulse_n = 0;

        const char* h = getenv("OPENPETE_INPUT_PULSE_HOLD_US");
        /* One VBL period plus a margin: long enough that every latch interval
         * overlaps the press, so "consumed == injected" is the expected
         * result and any shortfall names a defect. */
        s_pulse_hold_ns = (h && *h) ? atoi(h) * 1000 : 20 * 1000 * 1000;

        const char* g = getenv("OPENPETE_INPUT_PULSE_GAP_US");
        /* 117 ms is within 6 us of seven VBL periods, so the default gap
         * alone places every press at nearly the same phase; pulse_apply's
         * per-pulse jitter is what spreads the presses across the period. */
        s_pulse_gap_ns = (g && *g) ? atoi(g) * 1000 : 117 * 1000 * 1000;

        const char* b = getenv("OPENPETE_INPUT_PULSE_BIT");
        /* L2 by default: it reaches the pad buffer like any other bit but
         * does not advance a menu, so a sweep does not rewrite the scene it
         * is measuring. */
        s_pulse_bit = (unsigned short)((b && *b) ? strtoul(b, NULL, 0) : 0x0100u);

        if (s_pulse_n)
            OPL_INFO_F("input",
                       "timed pulse source: %d pulse(s), bit 0x%04X, hold %d us, "
                       "gap %d us — diagnostic, in-process only",
                       s_pulse_n, s_pulse_bit,
                       s_pulse_hold_ns / 1000, s_pulse_gap_ns / 1000);
    }
    return s_pulse_n > 0;
}

int openpete_host_input_pulse_count(void)
{
    (void)pulse_on();
    return s_pulse_n > 0 ? s_pulse_n : 0;
}

unsigned short openpete_host_input_pulse_bit(void)
{
    (void)pulse_on();
    return s_pulse_n > 0 ? s_pulse_bit : 0u;
}

int openpete_host_input_pulse_hold_us(void)
{
    (void)pulse_on();
    return s_pulse_n > 0 ? s_pulse_hold_ns / 1000 : 0;
}

int openpete_host_input_pulses_fired(void)
{
    return s_pulse_fired;
}

/** @brief Clear the pulse bit in bits while a scheduled pulse is held.
 *
 * t0 is the op_now() of the first call. Pulse k presses at
 * t0 + k * gap + jitter(k) and releases hold later; jitter(k) is a
 * multiplicative hash of k modulo 16715111 ns, about one VBL period, so the
 * press phases spread across the period instead of aliasing onto one
 * point. Also advances s_pulse_fired to the number of pulses whose press
 * instant has passed. Pulses past the configured count press nothing.
 * @param bits the active-low pad bits, updated in place. */
static void pulse_apply(unsigned short* bits)
{
    const int64_t now = (int64_t)op_now();
    if (!s_pulse_t0) s_pulse_t0 = now;

    const int64_t since = now - s_pulse_t0;
    if (since < 0) return;
    int64_t k = since / s_pulse_gap_ns;
    if (k >= s_pulse_n) return;

    /* A cheap deterministic hash of the index: reproducible across runs (the
     * whole log must replay identically) but uncorrelated with the grid. */
    unsigned h = (unsigned)k * 2654435761u;
    const int64_t jitter = (int64_t)(h % 16715111u);

    const int64_t start = s_pulse_t0 + k * (int64_t)s_pulse_gap_ns + jitter;
    if (now < start) return;
    if ((int)(k + 1) > s_pulse_fired) s_pulse_fired = (int)(k + 1);
    if (now >= start + s_pulse_hold_ns) return;

    *bits = (unsigned short)(*bits & ~s_pulse_bit);   /* active-low */
}

extern unsigned openpete_frame;

/* Parity instrument state. */
static int      s_parity_on = -1;   /* -1 = not yet resolved from the env */
static unsigned s_parity_frames;    /* frames where live decode owned the pad */
static unsigned s_parity_engaged;   /* ...of those, frames carrying real input */
static unsigned s_parity_mismatch;
static unsigned s_parity_stale;   /* pump produced nothing new: not a mismatch */
static unsigned s_parity_last_seq;

/* OPENPETE_INPUT_LEGACY, read once and cached. */
int openpete_host_input_legacy(void)
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("OPENPETE_INPUT_LEGACY");
        v = (e && *e && *e != '0') ? 1 : 0;
        if (v) OPL_INFO_F("input", "LEGACY input path (PsyCross owns pump+decode)");
    }
    return v;
}

/* Virtual gamepad (OPENPETE_INPUT_VGAMEPAD=1).
 *
 * The synthetic source above bypasses SDL entirely, which is what makes it
 * usable headless but also means it never exercises the real thing: hotplug
 * events reaching PsyCross's controller handlers through the host's pump, and
 * PsyCross's gamepad decode reading actual SDL axes and buttons. Attaching an
 * SDL virtual joystick drives that whole path for real — SDL emits a genuine
 * SDL_EVENT_GAMEPAD_ADDED, the pump routes it, PsyCross opens the pad, and
 * decode reads it like any other controller. Diagnostic only. */
/* Virtual pads. Index 0 is the original single pad the rumble recorder
 * probes; OPENPETE_INPUT_VGAMEPAD=N attaches N of them so the per-slot decode
 * and the N-port log can be exercised on a machine with no controllers, where
 * slots 1-3 would otherwise sit absent and make any verdict about them
 * vacuous. */
static SDL_Joystick* s_vpads[OPENPETE_PAD_SLOTS];
#define s_vpad (s_vpads[0])
static int s_vpad_state = -1;   /* -1 unresolved, 0 off, else the pad count */


/* ---- rumble recorder (the vibration oracle) -----------------------------
 *
 * Vibration has no pixels and no gameplay-state bytes, so the golden, oracle
 * and determinism gates are all structurally blind to it — reading them harder
 * proves nothing about whether a motor moved. The virtual pad closes that: a
 * virtual joystick's descriptor carries a Rumble callback, so attaching one
 * turns the whole chain (guest write -> registration -> per-tick sample ->
 * pump apply -> SDL -> device) into something a headless run can observe.
 *
 * What the recorder must NOT do is let a zero mean two things. A run that
 * records no motor commands looks identical whether the fix is broken or the
 * recorder was never wired up — and the pre-fix baseline is legitimately zero,
 * because SDL skips the driver entirely when the requested pair matches the
 * current one and a fresh joystick starts at {0,0} (SDL_joystick.c), so the
 * old code's single boot command of {0,0} never reached a callback at all.
 * The self-probe below is what separates the two: one deliberate non-zero
 * rumble at attach, asserted to arrive, before any later zero is believed. The
 * counters are zeroed behind it, so what they report is only ever the game. */
typedef struct {
    SDL_AtomicInt calls;        /* total Rumble callbacks seen */
    SDL_AtomicInt nonzero;      /* callbacks carrying a non-zero pair */
    SDL_AtomicInt last;         /* high<<16 | low, of the last callback */
    SDL_AtomicInt probe_seen;   /* the self-probe pair arrived */
} vpad_rumble_rec_t;

/* One recorder per virtual pad. Which device a motor command reached is the
 * question as soon as more than one slot can be driven, and a single set of
 * counters cannot answer it: the callback carries its pad index as userdata
 * so every command is attributed to the device that received it. */
static vpad_rumble_rec_t s_vpad_rumble[OPENPETE_PAD_SLOTS];

/* Distinctive enough that the probe cannot be confused with a game value: the
 * curve never produces equal low and high bytes at this magnitude. */
#define VPAD_PROBE_LOW   0x4000u
#define VPAD_PROBE_HIGH  0x4000u

/** @brief SDL virtual-joystick Rumble callback: record one motor command against its pad's counters.
 *
 * userdata carries the pad index; an index outside 0 .. OPENPETE_PAD_SLOTS - 1
 * is ignored. Increments calls, increments nonzero when either motor value is
 * non-zero, stores high << 16 | low as last, and sets probe_seen when the pair
 * equals the self-probe pair. The counters are atomic, so the thread SDL calls
 * from does not matter.
 * @param userdata the pad index, stored as an integer in the pointer.
 * @return true, so SDL treats the command as accepted. */
static bool SDLCALL vpad_rumble_cb(void* userdata, Uint16 low, Uint16 high)
{
    int v = (int)(intptr_t)userdata;
    if (v < 0 || v >= OPENPETE_PAD_SLOTS) return true;
    vpad_rumble_rec_t* r = &s_vpad_rumble[v];
    SDL_AddAtomicInt(&r->calls, 1);
    if (low || high)
        SDL_AddAtomicInt(&r->nonzero, 1);
    SDL_SetAtomicInt(&r->last, (int)(((Uint32)high << 16) | (Uint32)low));
    if (low == VPAD_PROBE_LOW && high == VPAD_PROBE_HIGH)
        SDL_SetAtomicInt(&r->probe_seen, 1);
    return true;
}

/** @brief Return whether a virtual pad index addresses a recorder. */
static int vpad_index_ok(int pad)
{
    return pad >= 0 && pad < OPENPETE_PAD_SLOTS;
}

int openpete_host_input_vpad_rumble_calls(int pad)
{
    return vpad_index_ok(pad) ? SDL_GetAtomicInt(&s_vpad_rumble[pad].calls) : 0;
}

int openpete_host_input_vpad_rumble_nonzero(int pad)
{
    return vpad_index_ok(pad) ? SDL_GetAtomicInt(&s_vpad_rumble[pad].nonzero) : 0;
}

int openpete_host_input_vpad_rumble_live(int pad)
{
    return vpad_index_ok(pad) ? SDL_GetAtomicInt(&s_vpad_rumble[pad].probe_seen) : 0;
}

void openpete_host_input_vpad_rumble_last(int pad, unsigned* low, unsigned* high)
{
    Uint32 p = vpad_index_ok(pad)
             ? (Uint32)SDL_GetAtomicInt(&s_vpad_rumble[pad].last) : 0u;
    if (low)  *low  = (unsigned)(p & 0xFFFFu);
    if (high) *high = (unsigned)(p >> 16);
}

int openpete_host_input_vpad_count(void)
{
    int n = 0;
    for (int v = 0; v < OPENPETE_PAD_SLOTS; v++)
        if (s_vpads[v]) n++;
    return n;
}

static void vpad_selfprobe(int v, SDL_JoystickID id);

/** @brief Attach the harness's virtual gamepads once, after SDL's gamepad subsystem is up.
 *
 * OPENPETE_INPUT_VGAMEPAD, resolved on first call, gives the pad count: unset
 * or 0 is none, a bare 1 is one pad, a larger integer is that many, clamped to
 * OPENPETE_PAD_SLOTS. Each pad is a virtual gamepad joystick whose Rumble
 * callback is vpad_rumble_cb with the pad index as userdata. Does nothing while
 * SDL_INIT_GAMEPAD is not initialised (the next call retries), once pad 0
 * exists, or when the count is 0. A failed attach or open of pad 0 disables the
 * source; a failure on a later pad stops the loop and keeps the pads already
 * open. Logs a warning when SDL does not expose pad 0 as a gamepad, then runs
 * vpad_selfprobe on every open pad. Called from openpete_host_input_sample on
 * the pump thread. */
static void vgamepad_maybe_attach(void)
{
    if (s_vpad_state < 0) {
        const char* e = getenv("OPENPETE_INPUT_VGAMEPAD");
        /* Bare "1" keeps the historical single-pad meaning. */
        s_vpad_state = (e && *e && *e != '0') ? atoi(e) : 0;
        if (s_vpad_state < 0) s_vpad_state = 0;
        if (s_vpad_state > OPENPETE_PAD_SLOTS) s_vpad_state = OPENPETE_PAD_SLOTS;
    }
    if (s_vpad_state < 1 || s_vpad)
        return;

    /* SDL_INIT_GAMEPAD is brought up by PsyX_Pad_InitSystem; attaching before
     * that leaves the pad invisible to the gamepad layer. */
    if (!SDL_WasInit(SDL_INIT_GAMEPAD))
        return;

    SDL_JoystickID id = 0;
    SDL_JoystickID vpad_ids[OPENPETE_PAD_SLOTS];
    memset(vpad_ids, 0, sizeof vpad_ids);
    const int want = s_vpad_state;
    for (int v = 0; v < want; v++) {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type       = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes      = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons   = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name       = "openpete virtual pad";
        desc.button_mask = 0xFFFFu;
        desc.axis_mask   = 0x3Fu;
        /* Every pad records, and the index it reports with is its userdata:
         * a command's destination device is the claim a per-slot actuator
         * path has to prove, and one shared counter cannot carry it. */
        desc.Rumble      = vpad_rumble_cb;
        desc.userdata    = (void*)(intptr_t)v;

        SDL_JoystickID vid = SDL_AttachVirtualJoystick(&desc);
        if (!vid) {
            OPL_WARN_F("input", "virtual gamepad %d attach failed: %s",
                       v, SDL_GetError());
            if (v == 0) { s_vpad_state = 0; return; }
            break;
        }
        s_vpads[v] = SDL_OpenJoystick(vid);
        if (!s_vpads[v]) {
            OPL_WARN_F("input", "virtual gamepad %d open failed: %s",
                       v, SDL_GetError());
            if (v == 0) { s_vpad_state = 0; return; }
            break;
        }
        vpad_ids[v] = vid;
        if (v == 0) id = vid;
    }
    /* Attaching is not the same as being usable: the pad only reaches decode
     * if SDL exposes it as a *gamepad* (it needs a mapping — on some platforms
     * a virtual joystick never gets one), and if PsyCross assigns it a slot,
     * which a real controller already occupying slot 0 will prevent. Say so
     * here, or a run that decodes nothing looks like a code failure. */
    if (!SDL_IsGamepad(id))
        OPL_WARN_F("input",
                   "virtual pad (id=%u) attached but SDL does not expose it as a "
                   "gamepad here — it will not reach decode", (unsigned)id);
    else
        OPL_INFO_F("input", "VIRTUAL gamepad attached (id=%u) — diagnostic only",
                   (unsigned)id);

    for (int v = 0; v < OPENPETE_PAD_SLOTS; v++)
        if (s_vpads[v]) vpad_selfprobe(v, vpad_ids[v]);
}

/** @brief Register one virtual pad as a rumble test device and check that its recorder receives commands.
 *
 * openpete_rumble_set_test_device() adds the pad's instance id to the set
 * the actuator path may drive headless and unfocused; the headless and
 * focus refusals protect a human's controller, which a harness-attached
 * virtual pad is not. The probe is one non-zero pair (VPAD_PROBE_LOW,
 * VPAD_PROBE_HIGH) followed by a stop; the callback latches probe_seen when
 * the pair arrives, and an error is logged when it did not, since a zero
 * motor count from such a run shows nothing. The calls, nonzero and last
 * counters are then zeroed, so every command they count afterwards came
 * through the actuator path; left in, the probe would make a run that
 * delivered nothing read the same as a run that delivered one command.
 * @param v Virtual pad index into s_vpads.
 * @param id SDL instance id of that pad. */
static void vpad_selfprobe(int v, SDL_JoystickID id)
{
    openpete_rumble_set_test_device((unsigned)id);

    SDL_RumbleJoystick(s_vpads[v], VPAD_PROBE_LOW, VPAD_PROBE_HIGH, 50);
    SDL_RumbleJoystick(s_vpads[v], 0, 0, 0);

    SDL_SetAtomicInt(&s_vpad_rumble[v].calls, 0);
    SDL_SetAtomicInt(&s_vpad_rumble[v].nonzero, 0);
    SDL_SetAtomicInt(&s_vpad_rumble[v].last, 0);

    if (!SDL_GetAtomicInt(&s_vpad_rumble[v].probe_seen))
        OPL_ERROR_F("input",
                    "virtual pad %d rumble recorder is NOT live — the probe "
                    "pair never reached the callback, so a zero motor count "
                    "from this run proves nothing", v);
    else
        OPL_INFO_F("input", "virtual pad %d rumble recorder live (self-probe ok)",
                   v);
}

/** @brief Set every attached virtual pad's buttons and left stick from the VBL-indexed synthetic pattern.
 *
 * The pattern matches synth_pad's (one of seven buttons per 8-VBL phase,
 * then a neutral phase) but is applied through SDL virtual-joystick state,
 * so the decode under test is the real gamepad path. The left stick sweeps
 * continuously, held or not. Pad v is shifted by 3 * v VBLs so no two
 * virtual pads hold the same button on the same VBL, which exposes a
 * routing defect that swaps two slots. Does nothing when no virtual pad is
 * attached.
 * @param frame the VBL counter (openpete_frame). */
static void vgamepad_drive(unsigned frame)
{
    if (!s_vpad) return;

    static const int kBtn[] = {
        SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
        SDL_GAMEPAD_BUTTON_WEST,  SDL_GAMEPAD_BUTTON_NORTH,
        SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        SDL_GAMEPAD_BUTTON_START,
    };
    const int n = (int)(sizeof(kBtn) / sizeof(kBtn[0]));

    for (int v = 0; v < OPENPETE_PAD_SLOTS; v++) {
        if (!s_vpads[v]) continue;
        /* Phase-shift per pad so no two virtual pads ever hold the same
         * button on the same VBL: a per-slot routing bug that crossed two
         * devices would otherwise still look correct. */
        unsigned f = frame + (unsigned)v * 3u;
        unsigned slot = (f / 8u) % (unsigned)(n + 1);

        for (int i = 0; i < n; i++)
            SDL_SetJoystickVirtualButton(s_vpads[v], kBtn[i],
                                         (unsigned)i == slot);

        Sint16 sweep = (Sint16)(((int)(f % 100u) - 50) * 600);
        SDL_SetJoystickVirtualAxis(s_vpads[v], SDL_GAMEPAD_AXIS_LEFTX, sweep);
        SDL_SetJoystickVirtualAxis(s_vpads[v], SDL_GAMEPAD_AXIS_LEFTY,
                                   (Sint16)-sweep);
    }
}

/* ---- host SDL input-state snapshot (see the header) --------------------- */

typedef struct {
    bool     buttons[SDL_GAMEPAD_BUTTON_COUNT];
    Sint16   axes[SDL_GAMEPAD_AXIS_COUNT];
    bool     have_pad;
} host_pad_state_t;

typedef struct {
    bool     keys[SDL_SCANCODE_COUNT];
    unsigned mods;
    host_pad_state_t pads[OPENPETE_PAD_SLOTS];
    openpete_mouse_state_t mouse;   /* event-derived, see openpete_host_mouse.h */
} host_sdl_state_t;

static host_sdl_state_t s_sdl_state[2];
static SDL_AtomicInt    s_sdl_state_cur;   /* index of the readable buffer */

/* Keys the injection script holds down. Pump thread only: the script steps
 * and the snapshot publishes on the same thread. `marked` keys come from the
 * script as such and reach the snapshot whatever the input lock says; `raw`
 * keys stand in for a physical keyboard and are merged into SDL's array
 * before the lock blanks it. */
static bool s_inj_keys_marked[SDL_SCANCODE_COUNT];
static bool s_inj_keys_raw[SDL_SCANCODE_COUNT];

void openpete_host_input_inject_key(int scancode, int down, int raw)
{
    if (scancode <= 0 || scancode >= SDL_SCANCODE_COUNT) return;
    (raw ? s_inj_keys_raw : s_inj_keys_marked)[scancode] = down != 0;
}

/** @brief Filter a slot's gamepad through the input lock: only an SDL virtual gamepad passes while the lock is active.
 *
 * A harness attaches the virtual gamepad; a physical pad reads as absent
 * under the lock. With the lock off, or gp NULL, gp is returned unchanged.
 * @param gp the slot's gamepad handle, or NULL.
 * @return gp, or NULL when the lock hides a physical pad. */
static SDL_Gamepad* lock_filter_gamepad(SDL_Gamepad* gp)
{
    if (!gp || !openpete_input_lock_active()) return gp;
    return SDL_IsJoystickVirtual(SDL_GetGamepadID(gp)) ? gp : NULL;
}

/* ---- controller mapping database ------------------------------------- */

/* A pad SDL has no mapping for is not a gamepad as far as SDL_IsGamepad is
 * concerned, so it never reaches the decode path at all. Mappings come from
 * files, and the whole point of loading them here is that a user can add one
 * without rebuilding or repacking anything.
 *
 * Precedence, lowest first. SDL's compiled-in database sits below every file;
 * inline SDL_GAMECONTROLLERCONFIG mappings sit above every file, because SDL
 * gives those PRIORITY_USER while every file load is PRIORITY_API. Among
 * files, SDL overwrites an existing GUID when the incoming priority is equal
 * or higher — so for files, load order alone is the precedence:
 *
 *   1  SDL's compiled-in database
 *   2  the file named by SDL_HINT_GAMECONTROLLERCONFIG_FILE, if the user
 *      exported it (SDL loads this itself during gamepad init)
 *   3  <exe dir>/gamecontrollerdb.txt, the copy shipped in the release
 *   4  <user config dir>/gamecontrollerdb.txt, the user's own additions
 *   5  --controller-db / OPENPETE_CONTROLLER_DB / controller_db
 *
 * Every file is merged into one table, so a user file holding a single line
 * for a single pad is enough; there is no need to copy the shipped database
 * to change one entry. Step 3 is skipped when the user set the hint, so an
 * explicit hint file means what it says rather than being half-overridden.
 *
 * Loading happens after the gamepad subsystem is up, which is also what makes
 * a late-added mapping usable: SDL emits an ADDED event for a joystick that
 * gains a mapping it did not have, and the pad opens on the next poll.
 *
 * Note for anyone adding a caller: SDL_ReloadGamepadMappings() rebuilds the
 * table from the compiled-in database plus the two hints only, dropping
 * everything loaded here. It must be followed by another call to this. */

#define MAP_PATH_MAX   1024
#define MAP_FILES_MAX  4

static struct {
    char path[MAP_PATH_MAX];
    const char* what;   /* the role, for the log: "shipped", "user", ... */
} s_map_file[MAP_FILES_MAX];
static int s_map_file_n;

/** @brief Return whether path names an existing regular file.
 *
 * Uses SDL_GetPathInfo; a directory or a missing path is false. */
static bool map_file_exists(const char* path)
{
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE;
}

/** @brief Load one controller-mapping database file and record it as a provenance source.
 *
 * A missing file logs at info level; a file SDL fails to parse logs a
 * warning. A loaded file is appended to s_map_file (up to MAP_FILES_MAX
 * entries) for map_source_for.
 * @param path the file path; NULL or empty loads nothing.
 * @param what the file's role for the log ("shipped", "user", ...).
 * @return whether the file loaded, so a caller can fall back to another
 *         location. */
static bool map_load_file(const char* path, const char* what)
{
    if (!path || !*path) return false;
    if (!map_file_exists(path)) {
        OPL_INFO_F("input", "mappings: no %s database at %s", what, path);
        return false;
    }
    int added = SDL_AddGamepadMappingsFromFile(path);
    if (added < 0) {
        /* Present but unreadable or malformed: the user meant to be heard,
         * so this is louder than a file that simply is not there. */
        OPL_WARN_F("input", "mappings: %s database %s failed to load: %s",
                   what, path, SDL_GetError());
        return false;
    }
    OPL_INFO_F("input", "mappings: %s database %s (+%d)", what, path, added);
    if (s_map_file_n < MAP_FILES_MAX) {
        SDL_strlcpy(s_map_file[s_map_file_n].path, path, MAP_PATH_MAX);
        s_map_file[s_map_file_n].what = what;
        s_map_file_n++;
    }
    return true;
}

/** @brief Return whether a mapping-database line maps guid on the platform this build runs on.
 *
 * The line must start with the 32 GUID characters (case-insensitive)
 * followed by a comma. A line without a platform field applies everywhere;
 * one whose platform field names another OS is discarded by SDL, so a file
 * that only mentions the GUID is not the file that supplied it.
 * @param line one NUL-terminated database line.
 * @param guid 32-hex-digit SDL GUID string.
 * @return true when the line supplies this platform's mapping for guid. */
static bool map_line_matches(const char* line, const char* guid)
{
    if (SDL_strncasecmp(line, guid, 32) != 0 || line[32] != ',') return false;
    const char* plat = SDL_strstr(line, "platform:");
    if (!plat) return true;   /* no platform field: applies everywhere */
    plat += 9;
    const char* self = SDL_GetPlatform();
    size_t n = SDL_strlen(self);
    return SDL_strncmp(plat, self, n) == 0 && (plat[n] == ',' || plat[n] == '\0' ||
                                               plat[n] == '\r' || plat[n] == '\n');
}

/** @brief Name the loaded mapping file that last supplied a line for @p guid.
 *
 * SDL exposes no provenance for a mapping, so the files are re-scanned back
 * to front and the first line matching the GUID and this platform wins, the
 * same order that decided the table. The rescan runs once per attached pad,
 * at boot and on each arrival, and tells whether the user's own file is the
 * one in use.
 * @param guid 32-hex-digit SDL GUID string.
 * @return The file's label, or NULL when the mapping can only have come from
 *         SDL's compiled-in database. */
static const char* map_source_for(const char* guid)
{
    for (int i = s_map_file_n - 1; i >= 0; i--) {
        SDL_IOStream* io = SDL_IOFromFile(s_map_file[i].path, "rb");
        if (!io) continue;
        char line[1024];
        size_t len = 0;
        bool hit = false;
        for (;;) {
            char c;
            if (SDL_ReadIO(io, &c, 1) != 1) c = '\n';
            else if (c != '\n') {
                if (len + 1 < sizeof line) line[len++] = c;
                continue;
            }
            line[len] = '\0';
            if (len && map_line_matches(line, guid)) { hit = true; break; }
            len = 0;
            if (SDL_GetIOStatus(io) == SDL_IO_STATUS_EOF) break;
        }
        SDL_CloseIO(io);
        if (hit) return s_map_file[i].what;
    }
    return NULL;
}

/** @brief Log one joystick's GUID, name, mapping source and resolved mapping, or a warning when it has no mapping.
 *
 * Called for each pad present at boot by map_log_pads() and for each later
 * arrival by openpete_host_input_pad_arrived().
 * @param id SDL instance id of the joystick. */
static void map_log_one(SDL_JoystickID id)
{
    char guid[33] = "";
    SDL_GUID g = SDL_GetJoystickGUIDForID(id);
    SDL_GUIDToString(g, guid, sizeof guid);
    const char* name = SDL_GetJoystickNameForID(id);
    char* mapping = SDL_GetGamepadMappingForGUID(g);
    if (mapping) {
        const char* src = map_source_for(guid);
        OPL_INFO_F("input", "pad %s \"%s\" mapped from %s",
                   guid, name ? name : "?", src ? src : "SDL's built-in database");
        /* The resolved mapping itself, not just its origin: a loaded file
         * and the mapping this pad uses are different facts, and only the
         * second shows which line drives the pad. */
        OPL_INFO_F("input", "pad %s mapping: %.240s", guid, mapping);
        SDL_free(mapping);
    } else {
        OPL_WARN_F("input", "pad %s \"%s\" has NO mapping — add a line for "
                            "this GUID to the user database",
                   guid, name ? name : "?");
    }
}

/** @brief Log each attached joystick's GUID, name and mapping source through map_log_one.
 *
 * Logs "no joysticks attached" when there are none. A per-file count of
 * loaded mappings cannot say which line won for a given pad, since SDL
 * keeps only the lines whose platform field matches, so the log is per
 * pad. */
static void map_log_pads(void)
{
    int n = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&n);
    if (!ids) return;
    if (n == 0) OPL_INFO_F("input", "mappings: no joysticks attached");
    for (int i = 0; i < n; i++) map_log_one(ids[i]);
    SDL_free(ids);
}

void openpete_host_input_pad_arrived(unsigned int joystick_id)
{
    /* Deliberately hung off JOYSTICK_ADDED rather than GAMEPAD_ADDED: the
     * latter fires only for a device SDL already has a mapping for, which is
     * exactly the device nobody needs a diagnostic about. */
    map_log_one((SDL_JoystickID)joystick_id);
}

void openpete_host_input_load_mappings(const char* cli_path)
{
    const char* hint = SDL_GetHint(SDL_HINT_GAMECONTROLLERCONFIG_FILE);
    /* Read, never written. An exported variable already outranks SDL_SetHint
     * (which fails outright rather than replacing it), so honouring the user
     * here is a matter of not fighting them for the shipped file's sake. */
    if (hint && *hint)
        OPL_INFO_F("input", "mappings: SDL_GAMECONTROLLERCONFIG_FILE=%s "
                            "(loaded by SDL; shipped database skipped)", hint);

    char exedir[MAP_PATH_MAX] = "";
    char shipped[MAP_PATH_MAX] = "";
    if (!(hint && *hint)) {
        bool got = false;
        if (openpete_exe_dir(exedir, sizeof exedir) == 0) {
            openpete_paths_join(shipped, sizeof shipped, exedir, "gamecontrollerdb.txt");
            got = map_load_file(shipped, "shipped");
        }
        /* A packaged build stages the database next to the executable; a build
         * tree keeps it at the repo root, which is where a dev run is launched
         * from. Falling back to the working directory keeps those runs on the
         * same mappings the release has. */
        if (!got) map_load_file("gamecontrollerdb.txt", "shipped (working directory)");
    }

    char userdir[MAP_PATH_MAX] = "";
    if (openpete_paths_user_config_dir(userdir, sizeof userdir, 0) == 0) {
        char user[MAP_PATH_MAX];
        openpete_paths_join(user, sizeof user, userdir, "gamecontrollerdb.txt");
        /* Under the portable layout the config directory IS the exe directory,
         * so this is the file already loaded above. Editing that one in place
         * is the supported route there; loading it twice would only produce a
         * second, confusing log line. */
        if (SDL_strcmp(user, shipped) != 0)
            map_load_file(user, "user");
    }

    if (!cli_path || !*cli_path) cli_path = SDL_getenv("OPENPETE_CONTROLLER_DB");
    if (cli_path && *cli_path) map_load_file(cli_path, "--controller-db");

    /* Steam exports this for its virtual pad, and SDL ranks inline mappings
     * above every file — so "my file is ignored" has exactly one explanation
     * that no amount of file logging would reveal. */
    const char* inline_cfg = SDL_GetHint(SDL_HINT_GAMECONTROLLERCONFIG);
    if (inline_cfg && *inline_cfg)
        OPL_INFO_F("input", "mappings: SDL_GAMECONTROLLERCONFIG is set — its "
                            "inline mappings outrank every file above");

    map_log_pads();
}

/** @brief Return the first gamepad in SDL's enumeration that already has an open handle, the device of pad slot 0.
 *
 * SDL_GetGamepadFromID resolves only devices some caller has opened;
 * enumerated gamepads nobody opened are skipped.
 * @return the gamepad handle, or NULL when none is open. */
static SDL_Gamepad* host_first_gamepad(void)
{
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    SDL_Gamepad* found = NULL;
    if (ids) {
        for (int i = 0; i < n && !found; i++)
            found = SDL_GetGamepadFromID(ids[i]);
        SDL_free(ids);
    }
    return found;
}

/* Gamepads openpete opened for slots 1-3.
 *
 * SDL_GetGamepadFromID resolves only devices somebody has already OPENED as a
 * gamepad, and PsyCross opens at most its own two. A third and fourth
 * controller are therefore invisible to enumeration until opened here — the
 * slots stay empty with the devices sitting right there. openpete owns these
 * handles because it owns those slots; PsyCross's own are never closed here. */
#define HOST_OWNED_PADS 8
static struct { SDL_JoystickID id; SDL_Gamepad* gp; } s_owned[HOST_OWNED_PADS];

/* Each slot's device, as an instance id, for readers on other threads. See
 * openpete_host_input_slot_device_id. */
static SDL_AtomicInt s_slot_dev_id[OPENPETE_PAD_SLOTS];

unsigned openpete_host_input_slot_device_id(int slot)
{
    if (slot < 1 || slot >= OPENPETE_PAD_SLOTS) return 0u;
    return (unsigned)SDL_GetAtomicInt(&s_slot_dev_id[slot]);
}

/** @brief Resolve a joystick instance id to a gamepad handle, opening it into s_owned when nothing else has.
 *
 * A device already open (by PsyCross or an earlier call) returns its
 * existing handle. A device with no mapping is not a gamepad and returns
 * NULL. Otherwise the device is opened into the first free s_owned entry;
 * a full table or a failed open (logged) returns NULL.
 * @param id SDL joystick instance id.
 * @return the gamepad handle, or NULL. */
static SDL_Gamepad* host_gamepad_for_id(SDL_JoystickID id)
{
    SDL_Gamepad* gp = SDL_GetGamepadFromID(id);
    if (gp) return gp;
    if (!SDL_IsGamepad(id)) return NULL;   /* no mapping: not a gamepad here */

    for (int i = 0; i < HOST_OWNED_PADS; i++)
        if (s_owned[i].id == id && s_owned[i].gp) return s_owned[i].gp;

    for (int i = 0; i < HOST_OWNED_PADS; i++) {
        if (s_owned[i].gp) continue;
        gp = SDL_OpenGamepad(id);
        if (!gp) {
            OPL_WARN_F("input", "could not open gamepad id=%u for a pad slot: %s",
                       (unsigned)id, SDL_GetError());
            return NULL;
        }
        s_owned[i].id = id;
        s_owned[i].gp = gp;
        return gp;
    }
    return NULL;
}

/** @brief Close the s_owned gamepad handles whose device is no longer enumerated.
 *
 * Keeps a replug from exhausting the HOST_OWNED_PADS table and leaving a
 * later slot unfillable.
 * @param ids the currently enumerated gamepad ids.
 * @param n the number of entries in ids. */
static void host_owned_prune(const SDL_JoystickID* ids, int n)
{
    for (int i = 0; i < HOST_OWNED_PADS; i++) {
        if (!s_owned[i].gp) continue;
        int live = 0;
        for (int k = 0; k < n; k++) if (ids[k] == s_owned[i].id) { live = 1; break; }
        if (live) continue;
        SDL_CloseGamepad(s_owned[i].gp);
        s_owned[i].gp = NULL;
        s_owned[i].id = 0;
    }
}

/** @brief Fill out with the gamepad handle that drives each pad slot.
 *
 * Slot 0 is host_first_gamepad, the first already-open gamepad, so player 1
 * does not depend on the other slots. Slots 1 .. OPENPETE_PAD_SLOTS - 1 take
 * the remaining enumerated gamepads in SDL enumeration order, skipping slot 0's
 * device, the handle PsyCross tracks as its own player 1 (decoded through
 * PsyCross, and a device in both would drive two players) and any handle
 * already assigned. A device with no open handle is opened into the owned table
 * by host_gamepad_for_id, and owned handles whose device has left are closed
 * first. When SDL_GetGamepads fails only slot 0 is filled. The order is stable
 * for a fixed device set but not across a replug.
 * @param out receives one handle per slot; NULL marks an unfilled slot. */
static void host_slot_gamepads(SDL_Gamepad* out[OPENPETE_PAD_SLOTS])
{
    for (int i = 0; i < OPENPETE_PAD_SLOTS; i++) out[i] = NULL;

    SDL_Gamepad* p1_host = host_first_gamepad();
    SDL_Gamepad* p1_psyx = (SDL_Gamepad*)PsyX_Pad_GetGamepad(0);
    out[0] = p1_host;

    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    if (!ids) return;
    host_owned_prune(ids, n);

    int slot = 1;
    for (int i = 0; i < n && slot < OPENPETE_PAD_SLOTS; i++) {
        SDL_Gamepad* gp = host_gamepad_for_id(ids[i]);
        if (!gp || gp == p1_host || gp == p1_psyx) continue;
        int dup = 0;
        for (int k = 1; k < slot; k++) if (out[k] == gp) dup = 1;
        if (dup) continue;
        out[slot++] = gp;
    }
    SDL_free(ids);
}

void openpete_host_input_publish_sdl(void)
{
    int cur  = SDL_GetAtomicInt(&s_sdl_state_cur);
    int next = cur ^ 1;
    host_sdl_state_t* d = &s_sdl_state[next];

    int nkeys = 0;
    const bool* keys = SDL_GetKeyboardState(&nkeys);
    if (keys) {
        if (nkeys > SDL_SCANCODE_COUNT) nkeys = SDL_SCANCODE_COUNT;
        memcpy(d->keys, keys, (size_t)nkeys * sizeof(bool));
        if (nkeys < SDL_SCANCODE_COUNT)
            memset(d->keys + nkeys, 0,
                   (size_t)(SDL_SCANCODE_COUNT - nkeys) * sizeof(bool));
    } else {
        memset(d->keys, 0, sizeof(d->keys));
    }
    for (int k = 0; k < SDL_SCANCODE_COUNT; k++)
        if (s_inj_keys_raw[k]) d->keys[k] = true;
    d->mods = (unsigned)SDL_GetModState();
    /* The input lock blanks everything a physical keyboard contributed; the
     * marked injected keys are merged after it and survive. */
    if (openpete_input_lock_active()) {
        memset(d->keys, 0, sizeof(d->keys));
        d->mods = 0;
    }
    for (int k = 0; k < SDL_SCANCODE_COUNT; k++)
        if (s_inj_keys_marked[k]) d->keys[k] = true;

    openpete_host_mouse_producer_read(&d->mouse);
    if (openpete_host_input_legacy() && openpete_host_mouse_accepting()) {
        /* PsyCross pumps on this path and openpete_handle_event never runs,
         * so the event-derived state above is empty: read SDL's own position
         * and mask instead. Presses, deltas and the wheel stay at zero. The
         * mouse switch gates this read too, since it is the one place the
         * pointer reaches a consumer without passing the event handler. */
        float mx = 0.0f, my = 0.0f;
        d->mouse.buttons = (unsigned)SDL_GetMouseState(&mx, &my);
        d->mouse.px = mx; d->mouse.py = my;
    }

    SDL_Gamepad* slot_gp[OPENPETE_PAD_SLOTS];
    host_slot_gamepads(slot_gp);
    for (int sl = 0; sl < OPENPETE_PAD_SLOTS; sl++) {
        host_pad_state_t* ps = &d->pads[sl];
        SDL_Gamepad* gp = lock_filter_gamepad(slot_gp[sl]);
        ps->have_pad = (gp != NULL);
        if (gp) {
            for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
                ps->buttons[b] = SDL_GetGamepadButton(gp, (SDL_GamepadButton)b);
            for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; a++)
                ps->axes[a] = SDL_GetGamepadAxis(gp, (SDL_GamepadAxis)a);
        } else {
            memset(ps->buttons, 0, sizeof(ps->buttons));
            memset(ps->axes, 0, sizeof(ps->axes));
        }
    }

    /* Release: the buffer's contents must be visible before the index that
     * publishes it. */
    SDL_MemoryBarrierRelease();
    SDL_SetAtomicInt(&s_sdl_state_cur, next);
}

/** @brief Return the published SDL snapshot buffer for reading.
 *
 * Loads the current-buffer index, then issues an acquire barrier that pairs
 * with the release barrier in openpete_host_input_publish_sdl. The pointer
 * addresses one of two buffers; the producer overwrites it two publishes later,
 * so a caller reads it briefly and calls again for a later sample.
 * @return the buffer, never NULL. */
static const host_sdl_state_t* host_sdl_read(void)
{
    const host_sdl_state_t* r = &s_sdl_state[SDL_GetAtomicInt(&s_sdl_state_cur)];
    SDL_MemoryBarrierAcquire();
    return r;
}

int openpete_host_input_key_down(int scancode)
{
    if (scancode <= 0 || scancode >= SDL_SCANCODE_COUNT) return 0;
    return host_sdl_read()->keys[scancode] ? 1 : 0;
}

int openpete_host_input_pad_button_slot(int slot, int button)
{
    if (slot < 0 || slot >= OPENPETE_PAD_SLOTS) return 0;
    if (button < 0 || button >= SDL_GAMEPAD_BUTTON_COUNT) return 0;
    const host_pad_state_t* ps = &host_sdl_read()->pads[slot];
    return (ps->have_pad && ps->buttons[button]) ? 1 : 0;
}

int openpete_host_input_pad_axis_slot(int slot, int axis)
{
    if (slot < 0 || slot >= OPENPETE_PAD_SLOTS) return 0;
    if (axis < 0 || axis >= SDL_GAMEPAD_AXIS_COUNT) return 0;
    const host_pad_state_t* ps = &host_sdl_read()->pads[slot];
    return ps->have_pad ? ps->axes[axis] : 0;
}

int openpete_host_input_pad_connected(int slot)
{
    if (slot < 0 || slot >= OPENPETE_PAD_SLOTS) return 0;
    return host_sdl_read()->pads[slot].have_pad ? 1 : 0;
}

int openpete_host_input_pad_button(int button)
{
    return openpete_host_input_pad_button_slot(0, button);
}

int openpete_host_input_pad_axis(int axis)
{
    return openpete_host_input_pad_axis_slot(0, axis);
}

unsigned openpete_host_input_modstate(void)
{
    return host_sdl_read()->mods;
}

void openpete_host_input_mouse(openpete_mouse_state_t* out)
{
    if (out) *out = host_sdl_read()->mouse;
}

int openpete_host_input_mouse_button(int button_index)
{
    if (button_index < 0 || button_index >= OPENPETE_MOUSE_BUTTONS) return 0;
    return (host_sdl_read()->mouse.buttons >> button_index) & 1u;
}

/** @brief Publish one whole pad sample under the seqlock.
 *
 * The sequence is made odd before the bytes are written and even after,
 * with release barriers between, so a reader never accepts a partial
 * sample. Slot 0 is always valid.
 * @param bits slot 0's active-low pad bits.
 * @param axes slot 0's RX, RY, LX, LY bytes.
 * @param extra slots 1 .. OPENPETE_PAD_SLOTS - 1, or NULL to publish them
 *        invalid.
 * @param now_ns the op_now() publish instant.
 * @return the sample's sequence number (even). */
static unsigned pad_publish(unsigned short bits, const unsigned char axes[4],
                            const openpete_pad_snapshot_t* extra,
                            int64_t now_ns)
{
    const unsigned seq = atomic_load_explicit(&s_snap_seq, memory_order_relaxed);
    atomic_store_explicit(&s_snap_seq, seq + 1u, memory_order_relaxed);
    SDL_MemoryBarrierRelease();          /* odd count visible before the bytes */

    s_snap_live.snap[0].bits = bits;
    memcpy(s_snap_live.snap[0].axes, axes, sizeof(s_snap_live.snap[0].axes));
    s_snap_live.snap[0].valid = true;
    for (int sl = 1; sl < OPENPETE_PAD_SLOTS; sl++)
        s_snap_live.snap[sl] = extra ? extra[sl - 1] : (openpete_pad_snapshot_t){0};
    s_snap_live.ns = now_ns;

    SDL_MemoryBarrierRelease();          /* bytes visible before the even count */
    atomic_store_explicit(&s_snap_seq, seq + 2u, memory_order_relaxed);
    return seq + 2u;
}

/** @brief Copy the current pad sample whole under the seqlock.
 *
 * Retries while a publish is in flight or the sequence changed across the
 * copy, up to PAD_SEQLOCK_TRIES times, yielding 1 us per try after the
 * first PAD_SEQLOCK_SPIN. Exhausting the bound increments the counter
 * openpete_host_input_seqlock_failures reads; out then holds an unverified
 * copy the callers discard.
 * @param out receives the sample.
 * @return the sequence number, or 0 when no sample has been published or
 *         the retry bound was exhausted. */
static unsigned pad_read_stable(host_pad_sample_t* out)
{
    for (int i = 0; i < PAD_SEQLOCK_TRIES; i++) {
        if (i >= PAD_SEQLOCK_SPIN) SDL_DelayNS(1000);
        const unsigned s0 =
            atomic_load_explicit(&s_snap_seq, memory_order_relaxed);
        if (s0 & 1u) continue;           /* publish in flight */
        SDL_MemoryBarrierAcquire();
        *out = s_snap_live;
        SDL_MemoryBarrierAcquire();
        if (atomic_load_explicit(&s_snap_seq, memory_order_relaxed) == s0)
            return s0;
    }
    /* The producer was not merely publishing but unavailable for the whole
     * retry window. The caller keeps the previous override bytes, which is the
     * right behaviour, but the census must say it happened rather than let a
     * tick silently replay an older sample as if it were fresh. */
    atomic_fetch_add_explicit(&s_snap_retry_fail, 1u, memory_order_relaxed);
    return 0u;
}

unsigned openpete_host_input_seqlock_failures(void)
{
    return atomic_load_explicit(&s_snap_retry_fail, memory_order_relaxed);
}

/* The kmr_* readers resolve bindings against the published SDL snapshot
 * rather than live SDL. The slot decode reads the same snapshot, so a
 * re-mapped player and a default-mapped one describe the same instant. */
/** @brief Keymap reader: return whether scancode is down in the published SDL snapshot.
 *
 * @param ctx unused.
 * @param scancode SDL scancode.
 * @return 1 when down, else 0. */
static int kmr_key(void* ctx, int scancode)
{
    (void)ctx; return openpete_host_input_key_down(scancode);
}
/** @brief Keymap reader: return whether a gamepad button is held on a slot in the SDL snapshot.
 *
 * @param ctx unused.
 * @param button SDL_GamepadButton index. */
static int kmr_btn(void* ctx, int slot, int button)
{
    (void)ctx; return openpete_host_input_pad_button_slot(slot, button);
}
/** @brief Keymap reader: return a gamepad axis value on a slot in the SDL snapshot.
 *
 * @param ctx unused.
 * @param axis SDL_GamepadAxis index.
 * @return the raw SDL axis value, or 0 when the slot has no device or an
 *        argument is out of range. */
static int kmr_axis(void* ctx, int slot, int axis)
{
    (void)ctx; return openpete_host_input_pad_axis_slot(slot, axis);
}

static const openpete_keymap_reader_t kSlotReader = {
    kmr_key, kmr_btn, kmr_axis, NULL
};

/* PSX pad bit per action, as offsets into a player's action block. The blocks
 * are generated from one list, so the same offsets address every player. Bit
 * values and the 16384 press threshold match the controller merge, so a
 * re-mapped player behaves like a default-mapped one. */
static const struct { int off; unsigned short mask; } kPadBits[] = {
    { OPK_PAD_CROSS      - OPK_PAD_CROSS, 0x4000 },
    { OPK_PAD_CIRCLE     - OPK_PAD_CROSS, 0x2000 },
    { OPK_PAD_SQUARE     - OPK_PAD_CROSS, 0x8000 },
    { OPK_PAD_TRIANGLE   - OPK_PAD_CROSS, 0x1000 },
    { OPK_PAD_L1         - OPK_PAD_CROSS, 0x0400 },
    { OPK_PAD_L2         - OPK_PAD_CROSS, 0x0100 },
    { OPK_PAD_L3         - OPK_PAD_CROSS, 0x0002 },
    { OPK_PAD_R1         - OPK_PAD_CROSS, 0x0800 },
    { OPK_PAD_R2         - OPK_PAD_CROSS, 0x0200 },
    { OPK_PAD_R3         - OPK_PAD_CROSS, 0x0004 },
    { OPK_PAD_START      - OPK_PAD_CROSS, 0x0008 },
    { OPK_PAD_SELECT     - OPK_PAD_CROSS, 0x0001 },
    { OPK_PAD_DPAD_UP    - OPK_PAD_CROSS, 0x0010 },
    { OPK_PAD_DPAD_DOWN  - OPK_PAD_CROSS, 0x0040 },
    { OPK_PAD_DPAD_LEFT  - OPK_PAD_CROSS, 0x0080 },
    { OPK_PAD_DPAD_RIGHT - OPK_PAD_CROSS, 0x0020 },
};

/* VBLs each slot decoded through its [keys.padN] bindings rather than the
 * shared merge. A re-map that silently failed to take would otherwise look
 * exactly like one that was never written. */
static unsigned long long s_slot_keymap_decodes[OPENPETE_PAD_SLOTS];

unsigned long long openpete_host_input_slot_keymap_decodes(int slot)
{
    return (slot >= 0 && slot < OPENPETE_PAD_SLOTS)
         ? s_slot_keymap_decodes[slot] : 0;
}

/** @brief Return whether any button, axis or digital-stick action is bound under the slot's [keys.padN] block.
 *
 * @param slot pad slot index.
 * @return 1 when at least one action is bound, else 0. */
static int keymap_slot_bound(int slot)
{
    const int base = OPK_PAD_BLOCK_FIRST(slot);
    for (size_t i = 0; i < sizeof(kPadBits) / sizeof(kPadBits[0]); i++)
        if (openpete_keymap_action_bound((openpete_action_t)(base + kPadBits[i].off)))
            return 1;
    static const int kAxisOff[] = {
        OPK_PAD_LEFT_X - OPK_PAD_CROSS,  OPK_PAD_LEFT_Y - OPK_PAD_CROSS,
        OPK_PAD_RIGHT_X - OPK_PAD_CROSS, OPK_PAD_RIGHT_Y - OPK_PAD_CROSS,
        OPK_PAD_STICK_UP - OPK_PAD_CROSS, OPK_PAD_STICK_DOWN - OPK_PAD_CROSS,
        OPK_PAD_STICK_LEFT - OPK_PAD_CROSS, OPK_PAD_STICK_RIGHT - OPK_PAD_CROSS,
    };
    for (size_t i = 0; i < sizeof(kAxisOff) / sizeof(kAxisOff[0]); i++)
        if (openpete_keymap_action_bound((openpete_action_t)(base + kAxisOff[i])))
            return 1;
    return 0;
}

/** @brief Apply one player's [keys.padN] bindings over an already-decoded pad.
 *
 * The controller-merge result is the base and only bound rows override it,
 * so re-mapping one button changes that button and leaves every unmentioned
 * row at its merged value. A bound button clears or sets its active-low bit;
 * a bound stick axis replaces the axis byte through the merge's conversion
 * (v / 256 + 128); a held digital-stick action forces its axis to 0 or 255.
 * Runs on the producer side against the sample being published, so the
 * bytes reach the input log and replay reproduces them.
 * @param slot pad slot index.
 * @param io_bits the pad bits, updated in place.
 * @param io_axes the four axis bytes (RX, RY, LX, LY), updated in place. */
static void keymap_decode_slot(int slot, unsigned short* io_bits,
                               unsigned char io_axes[4])
{
    const int base = OPK_PAD_BLOCK_FIRST(slot);
    unsigned short bits = *io_bits;

    for (size_t i = 0; i < sizeof(kPadBits) / sizeof(kPadBits[0]); i++) {
        openpete_action_t a = (openpete_action_t)(base + kPadBits[i].off);
        if (!openpete_keymap_action_bound(a)) continue;
        if (openpete_keymap_action_down_via(a, slot, &kSlotReader))
            bits = (unsigned short)(bits & ~kPadBits[i].mask);   /* active-low */
        else
            bits = (unsigned short)(bits | kPadBits[i].mask);
    }
    *io_bits = bits;

    /* Axes use the same fixed-point conversion the controller merge applies,
     * so a re-mapped stick and a default one share a centre and a scale. */
    static const struct { int off; int idx; } kAxes[] = {
        { OPK_PAD_RIGHT_X - OPK_PAD_CROSS, 0 },
        { OPK_PAD_RIGHT_Y - OPK_PAD_CROSS, 1 },
        { OPK_PAD_LEFT_X  - OPK_PAD_CROSS, 2 },
        { OPK_PAD_LEFT_Y  - OPK_PAD_CROSS, 3 },
    };
    for (size_t i = 0; i < sizeof(kAxes) / sizeof(kAxes[0]); i++) {
        openpete_action_t a = (openpete_action_t)(base + kAxes[i].off);
        if (!openpete_keymap_action_bound(a)) continue;
        int v = openpete_keymap_action_axis_raw_via(a, slot, &kSlotReader);
        io_axes[kAxes[i].idx] = (unsigned char)((v / 256) + 128);
    }

    /* Digital left stick, overriding the analog read only while held — the
     * keyboard-as-stick rule the merge applies for player 1. */
    {
        static const struct { int off; int idx; unsigned char val; } kStick[] = {
            { OPK_PAD_STICK_LEFT  - OPK_PAD_CROSS, 2, 0   },
            { OPK_PAD_STICK_RIGHT - OPK_PAD_CROSS, 2, 255 },
            { OPK_PAD_STICK_UP    - OPK_PAD_CROSS, 3, 0   },
            { OPK_PAD_STICK_DOWN  - OPK_PAD_CROSS, 3, 255 },
        };
        for (size_t i = 0; i < sizeof(kStick) / sizeof(kStick[0]); i++) {
            openpete_action_t a = (openpete_action_t)(base + kStick[i].off);
            if (!openpete_keymap_action_bound(a)) continue;
            if (openpete_keymap_action_down_via(a, slot, &kSlotReader))
                io_axes[kStick[i].idx] = kStick[i].val;
        }
    }
}

void openpete_host_input_sample(bool alt_held)
{
    unsigned short bits = PAD_NEUTRAL_BITS;
    unsigned char  axes[4] = { 128, 128, 128, 128 };

    /* PsyX_UpdateInput skips the pad update entirely while Alt is held, so
     * the Enter of Alt+Enter never reaches the game as Start. Driving the
     * halves separately means owning that gate here. Publishing neutral
     * (rather than skipping the publish) keeps the snapshot fresh: a stale
     * one would replay the last pre-Alt press for as long as Alt is down. */
    vgamepad_maybe_attach();
    vgamepad_drive(openpete_frame);

    if (synth_on())
        synth_pad(openpete_frame, &bits, axes);
    else if (openpete_input_lock_active())
        /* PsyX_Pad_DecodeP1 reads SDL's keyboard array and the player-1
         * gamepad directly, so the lock cannot filter inside it. Decode the
         * player-1 pad alone, and only when it is a virtual one; with none
         * this yields the neutral pad. */
        PsyX_Pad_DecodeGamepad(lock_filter_gamepad((SDL_Gamepad*)PsyX_Pad_GetGamepad(0)),
                               &bits, axes);
    else if (!alt_held)
        PsyX_Pad_DecodeP1(&bits, axes);

    /* Applied last so the pulse survives whatever the live decode produced:
     * the schedule is the thing under test, and a real key held at the same
     * moment must not cancel it. */
    if (pulse_on())
        pulse_apply(&bits);

    /* Slots 1-3, decoded from their own devices through the same merge slot 0
     * uses, so a mapping or centre-convention change cannot reach one player
     * and not the others. A slot with no device publishes invalid rather than
     * neutral, which is what lets the log distinguish "nobody plugged in"
     * from "holding nothing".
     *
     * The Alt gate deliberately does NOT extend here. It exists so the Enter
     * of Alt+Enter cannot reach the game as Start, which is a fact about
     * player 1's keyboard; these slots are decoded from their own gamepads.
     * Gating them would drop every other player's pad — recorded as
     * disconnected for those VBLs — because player 1 reached for a window
     * shortcut. */
    openpete_pad_snapshot_t extra[OPENPETE_PAD_SLOTS - 1];
    memset(extra, 0, sizeof(extra));
    {
        SDL_Gamepad* slot_gp[OPENPETE_PAD_SLOTS];
        host_slot_gamepads(slot_gp);
        for (int sl = 1; sl < OPENPETE_PAD_SLOTS; sl++) {
            openpete_pad_snapshot_t* d = &extra[sl - 1];
            slot_gp[sl] = lock_filter_gamepad(slot_gp[sl]);
            if (!slot_gp[sl]) continue;              /* leaves valid = false */
            unsigned short sb = PAD_NEUTRAL_BITS;
            unsigned char  sa[4] = { 128, 128, 128, 128 };
            /* [keys.padN] re-maps this player; with nothing bound the shared
             * controller merge gives them a working default pad. */
            PsyX_Pad_DecodeGamepad(slot_gp[sl], &sb, sa);
            if (keymap_slot_bound(sl)) {
                keymap_decode_slot(sl, &sb, sa);
                s_slot_keymap_decodes[sl]++;
            }
            d->bits = sb;
            memcpy(d->axes, sa, sizeof(d->axes));
            d->valid = true;
        }
        /* Publish each slot's device id for the rumble pump, which runs on
         * another thread and must not hold one of these handles across a
         * prune. Published for an empty slot too (as 0), so a pad going away
         * silences its slot instead of leaving the last id standing. */
        for (int sl = 1; sl < OPENPETE_PAD_SLOTS; sl++)
            SDL_SetAtomicInt(&s_slot_dev_id[sl],
                             slot_gp[sl] ? (int)SDL_GetGamepadID(slot_gp[sl]) : 0);
    }

    const int64_t now_ns = (int64_t)op_now();
    const unsigned seq = pad_publish(bits, axes, extra, now_ns);

    /* Pad edges go to the pacing log from HERE — the producer side — because
     * the quantity of interest is when the press was seen relative to the
     * tick that consumed it. Logging it at the latch would record the
     * consumer's own cadence and hide exactly the phase being measured. */
    {
        static unsigned short s_prev_bits = PAD_NEUTRAL_BITS;
        static unsigned char  s_prev_axes[4] = { 128, 128, 128, 128 };
        const int analog_delta = memcmp(s_prev_axes, axes, sizeof(axes)) != 0;
        if (bits != s_prev_bits || analog_delta) {
            openpete_pace_input_edge(now_ns, seq, bits,
                                     (unsigned short)(bits ^ s_prev_bits),
                                     analog_delta);
            s_prev_bits = bits;
            memcpy(s_prev_axes, axes, sizeof(axes));
        }
    }
}

void openpete_host_input_seed_neutral(void)
{
    /* Publish a neutral, valid snapshot before the consumer can run.
     *
     * Without one, the consumer's first latch finds nothing to stamp, leaves
     * the override inactive, and PsyX_Pad_InternalPadUpdates falls through to
     * its LIVE path — which calls SDL_PumpEvents (PsyX_pad.cpp). Under the
     * thread split that consumer is the worker, so this is an SDL event pump
     * from a thread that does not own video: a hard crash on Windows.
     *
     * Seeding neutral means the override is active from the first tick and
     * the live path is unreachable on the consumer thread by construction. */
    const unsigned char neutral[4] = { 128, 128, 128, 128 };
    pad_publish(PAD_NEUTRAL_BITS, neutral, NULL, (int64_t)op_now());
}

void openpete_host_input_latch(bool claimed)
{
    if (claimed) {
        /* A higher-priority writer (bk2 replay, the menu, devui) owns the pad
         * this iteration, so this tick consumes NO host sample. Clearing the
         * latch record rather than leaving the previous one standing is the
         * whole point: a stale publish instant quoted against this tick's
         * exec_start reads as ever-growing input staleness on a channel the
         * tick never touched — a plausible number about nothing, which on a
         * fully-claimed run (bk2) is every number the log reports. */
        s_latched_ns  = 0;
        s_latched_seq = 0;
        for (int sl = 0; sl < OPENPETE_PAD_SLOTS; sl++)
            s_latched_snap[sl].valid = false;
        return;
    }

    /* Copy bytes, publish instant and identity as ONE sample. Reading the
     * timestamp separately lets a tick be credited with a sample whose bytes
     * it never saw; the sequence number is what makes "which published sample
     * did this tick consume" answerable rather than inferred from ordering. */
    host_pad_sample_t cur;
    const unsigned seq = pad_read_stable(&cur);
    if (!seq || !cur.snap[0].valid) return;

    for (int sl = 0; sl < OPENPETE_PAD_SLOTS; sl++)
        s_latched_snap[sl] = cur.snap[sl];
    s_latched_ns   = cur.ns;
    s_latched_seq  = seq;

    /* Only slot 0 reaches the guest. Slots 1-3 are latched for the input log
     * and the mod API; nothing stamps them into a PSX pad buffer. */
    g_PsyX_Pad_InputOverride     = cur.snap[0].bits;
    g_PsyX_Pad_AnalogOverride[0] = cur.snap[0].axes[0];
    g_PsyX_Pad_AnalogOverride[1] = cur.snap[0].axes[1];
    g_PsyX_Pad_AnalogOverride[2] = cur.snap[0].axes[2];
    g_PsyX_Pad_AnalogOverride[3] = cur.snap[0].axes[3];
    g_PsyX_Pad_OverrideActive    = 1;
}

int openpete_host_input_latched_slot(int slot, openpete_pad_snapshot_t* out)
{
    if (!out) return 0;
    if (slot < 0 || slot >= OPENPETE_PAD_SLOTS) { out->valid = false; return 0; }
    *out = s_latched_snap[slot];
    return out->valid ? 1 : 0;
}

void openpete_host_input_last(openpete_pad_snapshot_t* out)
{
    host_pad_sample_t cur;
    if (!out) return;
    if (pad_read_stable(&cur)) *out = cur.snap[0];
    else                       out->valid = false;
}

int64_t openpete_host_input_last_sample_ns(void)
{
    host_pad_sample_t cur;
    return pad_read_stable(&cur) ? cur.ns : 0;
}

int64_t openpete_host_input_latched_sample_ns(void)
{
    return s_latched_ns;
}

unsigned openpete_host_input_latched_seq(void)
{
    return s_latched_seq;
}

unsigned short openpete_host_input_latched_bits(void)
{
    return s_latched_snap[0].valid ? s_latched_snap[0].bits : PAD_NEUTRAL_BITS;
}

void openpete_host_input_parity_enable(void)
{
    s_parity_on = 1;
    OPL_INFO_F("input", "parity instrument armed (pad buffer vs live decode)");
}

/** @brief Return whether the input parity instrument is armed.
 *
 * OPENPETE_INPUT_PARITY is read on the first call and cached, since a
 * getenv per iteration is a measurable cost on Windows; a non-empty value
 * not starting with '0' arms it. openpete_host_input_parity_enable arms it
 * regardless.
 * @return 1 when armed, else 0. */
static int parity_on(void)
{
    if (s_parity_on < 0) {
        const char* e = getenv("OPENPETE_INPUT_PARITY");
        s_parity_on = (e && *e && *e != '0') ? 1 : 0;
        if (s_parity_on)
            OPL_INFO_F("input", "parity instrument armed (pad buffer vs live decode)");
    }
    return s_parity_on;
}

void openpete_host_input_parity_check(bool claimed)
{
    if (!parity_on() || claimed)
        return;   /* another writer legitimately owns the pad this iteration */

    /* Nothing is injected before the game registers a pad buffer, so the
     * readbacks would report their no-buffer defaults and read as a mismatch. */
    if (!PsyX_Pad_P1Ready())
        return;

    /* Reference: what the tick actually LATCHED.
     *
     * The obvious reference is a fresh live decode, and that is what this used
     * to do — but a live decode reads SDL keyboard and gamepad state, and this
     * runs on the thread that consumes input, not the one that pumps it. Under
     * the thread split that is an SDL read from the non-owning thread: the
     * exact thing the producer/consumer seam exists to forbid, performed by
     * the instrument meant to verify it.
     *
     * The latched sample answers the same question without touching SDL. What
     * is under test is the PLUMBING — that the value this iteration latched is
     * the one that reached the PSX pad buffer, and not a stale sample, a
     * mis-ordered latch, an override writer leaking past its claim, or the
     * analog centre drifting (the override channel and the live decode both
     * centre at 128, a DualShock's rest value, and routing live input through
     * the channel must keep it). The decode itself needs no comparison: both
     * paths call PsyX_Pad_DecodeP1, so they agree by construction. */
    if (!s_latched_snap[0].valid)
        return;                       /* nothing was consumed this iteration */
    const unsigned short ref_bits = s_latched_snap[0].bits;
    const unsigned char* ref_axes = s_latched_snap[0].axes;

    /* Staleness probe: the producer must have published something new since
     * the last latch. Under the split it republishes on its own cadence, so
     * a repeat means the pump stopped — which is a different finding from a
     * value mismatch and is counted separately, or a parked pump would be
     * reported as a plumbing failure. */
    if (s_latched_seq == s_parity_last_seq) {
        if (s_parity_stale < 16)
            OPL_WARN_F("input",
                       "parity STALE: latched sample seq %u again — the "
                       "producer published nothing new since the last tick",
                       s_latched_seq);
        s_parity_stale++;
    }
    s_parity_last_seq = s_latched_seq;

    unsigned short got_bits = PsyX_Pad_GetLastP1Bits();
    unsigned char  got_axes[4];
    PsyX_Pad_GetLastP1Analog(got_axes);

    s_parity_frames++;

    /* "Engaged" must mean a button was actually pressed or a stick actually
     * deflected: idle centres at 128 on every path, and a gamepad at rest
     * reads within one of it, so a one-count wobble is not input. */
    bool axis_moved = false;
    for (int i = 0; i < 4; i++) {
        int d = (int)ref_axes[i] - 128;
        if (d < 0) d = -d;
        if (d > 1) { axis_moved = true; break; }
    }
    bool engaged = (ref_bits != PAD_NEUTRAL_BITS) || axis_moved;
    if (engaged) s_parity_engaged++;

    if (got_bits != ref_bits || memcmp(got_axes, ref_axes, 4) != 0) {
        if (s_parity_mismatch < 16)
            OPL_WARN_F("input",
                       "parity MISMATCH: f=%u pad=0x%04X [%u %u %u %u] live=0x%04X [%u %u %u %u]",
                       openpete_frame, got_bits, got_axes[0], got_axes[1], got_axes[2], got_axes[3],
                       ref_bits, ref_axes[0], ref_axes[1], ref_axes[2], ref_axes[3]);
        s_parity_mismatch++;
    }
}

int openpete_host_input_parity_report(void)
{
    if (!parity_on()) return 1;

    if (s_parity_engaged == 0) {
        OPL_WARN_F("input",
                   "parity VACUOUS: %u host-owned frames, 0 carried input — "
                   "nothing was pressed, so this run proves nothing",
                   s_parity_frames);
        return -1;
    }
    if (s_parity_mismatch) {
        OPL_ERROR_F("input", "parity FAIL: %u mismatches over %u frames (%u engaged)",
                  s_parity_mismatch, s_parity_frames, s_parity_engaged);
        return 0;
    }
    /* Reported next to the verdict, never folded into it: a stale sample means
     * the pump stopped producing, which is a real finding about the producer
     * and not a failure of the path this check covers. */
    if (s_parity_stale)
        OPL_WARN_F("input",
                   "parity: %u tick(s) latched a sample the producer had not "
                   "refreshed", s_parity_stale);
    OPL_INFO_F("input", "parity PASS: %u frames, %u engaged, 0 mismatches",
               s_parity_frames, s_parity_engaged);
    return 1;
}
