#include "openpete_host_input.h"
#include "openpete_log.h"

#include "PsyX/PsyX_public.h"

#include <SDL3/SDL.h>

#include <string.h>
#include <stdlib.h>

/* Neutral pad: all buttons released (active-low), sticks centred. */
#define PAD_NEUTRAL_BITS 0xFFFF

static openpete_pad_snapshot_t s_snap;
static unsigned s_snap_seq;      /* bumped on every publish — staleness probe */

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

/* A deterministic function of the VBL counter: a rotating single button held
 * for 8 frames at a time, with the left stick sweeping, separated by neutral
 * gaps so both engaged and idle frames occur. */
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

extern unsigned openpete_frame;

/* Parity instrument state. */
static int      s_parity_on = -1;   /* -1 = not yet resolved from the env */
static unsigned s_parity_frames;    /* frames where live decode owned the pad */
static unsigned s_parity_engaged;   /* ...of those, frames carrying real input */
static unsigned s_parity_mismatch;
static unsigned s_parity_last_seq;

/* OPENPETE_INPUT_LEGACY=1 restores the pre-ownership input path (PsyCross
 * pumps, decodes and injects on its own). Kept only while the host-owned path
 * is being proven: it is the A/B that shows the two produce identical gameplay,
 * and the bisect switch if they ever stop doing so. */
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
static SDL_Joystick* s_vpad;
static int s_vpad_state = -1;   /* -1 unresolved, 0 off, 1 requested */

static void vgamepad_maybe_attach(void)
{
    if (s_vpad_state < 0) {
        const char* e = getenv("OPENPETE_INPUT_VGAMEPAD");
        s_vpad_state = (e && *e && *e != '0') ? 1 : 0;
    }
    if (s_vpad_state != 1 || s_vpad)
        return;

    /* SDL_INIT_GAMEPAD is brought up by PsyX_Pad_InitSystem; attaching before
     * that leaves the pad invisible to the gamepad layer. */
    if (!SDL_WasInit(SDL_INIT_GAMEPAD))
        return;

    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type       = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes      = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons   = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name       = "openpete virtual pad";
    desc.button_mask = 0xFFFFu;
    desc.axis_mask   = 0x3Fu;

    SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    if (!id) {
        OPL_WARN_F("input", "virtual gamepad attach failed: %s", SDL_GetError());
        s_vpad_state = 0;
        return;
    }
    s_vpad = SDL_OpenJoystick(id);
    if (!s_vpad) {
        OPL_WARN_F("input", "virtual gamepad open failed: %s", SDL_GetError());
        s_vpad_state = 0;
        return;
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
}

/* Drive the virtual pad from the same frame-indexed pattern the synthetic
 * source uses, but through real SDL button/axis state so the decode under test
 * is the genuine gamepad path. */
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
    unsigned slot = (frame / 8u) % (unsigned)(n + 1);

    for (int i = 0; i < n; i++)
        SDL_SetJoystickVirtualButton(s_vpad, kBtn[i], (unsigned)i == slot);

    Sint16 sweep = (Sint16)(((int)(frame % 100u) - 50) * 600);
    SDL_SetJoystickVirtualAxis(s_vpad, SDL_GAMEPAD_AXIS_LEFTX, sweep);
    SDL_SetJoystickVirtualAxis(s_vpad, SDL_GAMEPAD_AXIS_LEFTY, (Sint16)-sweep);
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
    else if (!alt_held)
        PsyX_Pad_DecodeP1(&bits, axes);

    s_snap.bits = bits;
    memcpy(s_snap.axes, axes, sizeof(s_snap.axes));
    s_snap.valid = true;
    s_snap_seq++;
}

void openpete_host_input_latch(bool claimed)
{
    if (claimed || !s_snap.valid)
        return;

    g_PsyX_Pad_InputOverride     = s_snap.bits;
    g_PsyX_Pad_AnalogOverride[0] = s_snap.axes[0];
    g_PsyX_Pad_AnalogOverride[1] = s_snap.axes[1];
    g_PsyX_Pad_AnalogOverride[2] = s_snap.axes[2];
    g_PsyX_Pad_AnalogOverride[3] = s_snap.axes[3];
    g_PsyX_Pad_OverrideActive    = 1;
}

void openpete_host_input_last(openpete_pad_snapshot_t* out)
{
    if (out) *out = s_snap;
}

void openpete_host_input_parity_enable(void)
{
    s_parity_on = 1;
    OPL_INFO_F("input", "parity instrument armed (pad buffer vs live decode)");
}

/* Resolved once — getenv per frame is a measurable cost on Windows. */
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

    /* Reference: decode live SDL again, now, in the same iteration. This is
     * the value the pad WOULD have carried on the pre-ownership path. The
     * comparison is against what actually landed in the pad buffer, so it
     * catches the plumbing rather than the decode: a stale snapshot, a
     * mis-ordered latch, an override writer leaking past its claim, or the
     * analog centre drifting (live no-controller centres at 127; the override
     * channel's own default is 128 — routing live input through the channel
     * must not quietly adopt 128). */
    unsigned short ref_bits = PAD_NEUTRAL_BITS;
    unsigned char  ref_axes[4] = { 128, 128, 128, 128 };
    if (synth_on())
        synth_pad(openpete_frame, &ref_bits, ref_axes);
    else
        PsyX_Pad_DecodeP1(&ref_bits, ref_axes);

    /* Staleness probe: the sample must have been republished this iteration.
     * A latch that reads a snapshot the producer did not refresh is the
     * failure mode the thread split will make possible, so catch it now. */
    if (s_snap_seq == s_parity_last_seq) {
        if (s_parity_mismatch < 16)
            OPL_WARN_F("input", "parity STALE: snapshot seq %u not refreshed", s_snap_seq);
        s_parity_mismatch++;
    }
    s_parity_last_seq = s_snap_seq;

    unsigned short got_bits = PsyX_Pad_GetLastP1Bits();
    unsigned char  got_axes[4];
    PsyX_Pad_GetLastP1Analog(got_axes);

    s_parity_frames++;

    /* "Engaged" must mean a button was actually pressed or a stick actually
     * deflected. Idle is not a single value: the override channel centres at
     * 128, while live decode with no controller connected centres at 127
     * (PsyX_Pad_UpdateGameControllerInput's no-controller case), so treating
     * "!= 128" as input would score every idle headless frame as engaged and
     * hand back a confident PASS for a run that pressed nothing. */
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
    OPL_INFO_F("input", "parity PASS: %u frames, %u engaged, 0 mismatches",
               s_parity_frames, s_parity_engaged);
    return 1;
}
