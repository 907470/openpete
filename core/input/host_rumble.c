/* core/input/host_rumble.c — the actuator path openpete owns.
 *
 * See openpete_rumble.h for why this exists at all (libpad registers a buffer;
 * PsyCross one-shotted it, so the only command ever sent was a zero at boot).
 *
 * Shape of the seam:
 *
 *   PadSetAct        -> openpete_rumble_register()  stores {ptr, len} per slot
 *   sim thread /tick -> openpete_rumble_sample()    reads the buffer, publishes
 *   main pump        -> openpete_rumble_apply()     drives SDL from the pair
 *
 * The published pair is a single 32-bit atomic rather than a seqlock: it is
 * two u16s that are only ever written together, so packing them into one word
 * makes "both halves belong to the same tick" a property of the type instead
 * of a protocol. That matters because the alternative failure — a large-motor
 * value from one tick paired with a small-motor value from the next — is
 * exactly the kind of thing nobody would ever notice by feel.
 */

#include "openpete_rumble.h"
#include "openpete_input_log.h"   /* OPENPETE_PAD_SLOTS */
#include "openpete_log.h"

#include <SDL3/SDL.h>

#include <stdlib.h>
#include <string.h>

/* PsyCross owns the SDL_Gamepad handles (opened and closed in its
 * controller-added / -removed handlers, which run on the pump thread). Asking
 * it for the slot's handle keeps the slot mapping, the cold-plug enumeration
 * and the hotplug re-claim in one place rather than growing a second, subtly
 * different notion of "which pad is player one". */
extern void* PsyX_Pad_GetGamepad(int slot);

/* Runahead speculation flag (openpete_fiber.h). Declared rather than included
 * to keep this TU's include surface to SDL + our own header. */
extern int openpete_speculative;

/* The device driving pad slot 1..3, as an SDL instance id (host_input.c).
 * Declared for the same reason: an id, not a handle, because those handles are
 * opened and pruned on the sampling thread while this file runs on the pump. */
extern unsigned openpete_host_input_slot_device_id(int slot);

/* Only slot 0 is wired: Spyro registers one actuator buffer, for player one,
 * and a second slot has nothing to drive. The array exists so a game that
 * registers slot 1 records the fact instead of silently aliasing slot 0. */
#define RUMBLE_SLOTS 2

typedef struct {
    unsigned char* table;
    int            len;
} rumble_reg_t;

static rumble_reg_t s_reg[RUMBLE_SLOTS];

/* Published motor pair: low 16 bits = large motor (SDL low frequency), high 16
 * bits = small motor (SDL high frequency). Written by the sim thread, read by
 * the pump thread. */
static SDL_AtomicInt s_pair;

/* The harness's own virtual pad, when one is attached (OPENPETE_INPUT_VGAMEPAD).
 *
 * The headless and focus refusals below exist to protect a HUMAN's controller,
 * and a virtual device attached by a test harness is not one — nobody is
 * holding it. Left as an unconditional refusal, headless would also blind the
 * only instrument that can observe this path at all, and the vibration oracle
 * would be reduced to testing a mock of the code instead of the code.
 *
 * So the refusals are lifted for exactly the devices the harness attached:
 * virtual pads, identified by instance id and re-checked against the handle
 * actually being driven. If a real controller is also plugged in and has taken
 * a slot, its id is not in the set and the normal gates apply — a diagnostic
 * run must not buzz a pad on the desk just because a test flag is set.
 *
 * A set rather than one id, because the harness attaches one pad per slot and
 * a per-slot claim is only observable when every one of them can be driven.
 * Each entry is stored as id+1 so zero means "empty". */
static SDL_AtomicInt s_test_device[OPENPETE_PAD_SLOTS];

/* Mod-driven motor pairs for slots 1..OPENPETE_PAD_SLOTS-1, packed the same
 * way as s_pair: low 16 bits the large motor, high 16 the small. Written by
 * the sim thread through openpete_rumble_mod_set, read by the pump. Index 0 is
 * never used: slot 0 is the game's own actuator table. */
static SDL_AtomicInt s_mod_pair[OPENPETE_PAD_SLOTS];
static SDL_AtomicInt s_mod_commands[OPENPETE_PAD_SLOTS];

/* Set by the first publish for a slot and never cleared: a slot no mod has
 * ever driven is left alone entirely, so its command count stays a count of
 * what a mod asked for. Clearing stays armed, because the stop has to reach
 * the device. */
static SDL_AtomicInt s_mod_armed[OPENPETE_PAD_SLOTS];

/* Gates. All are pump-thread reads; the setters are called from wherever the
 * corresponding state changes, and each is a single int, so no locking. */
static SDL_AtomicInt s_headless;
static SDL_AtomicInt s_paused;
static SDL_AtomicInt s_strength;      /* 0..10, or -1 for unset (reads as 10) */
static int           s_strength_init;

/* Diagnostics. */
static SDL_AtomicInt s_reg_calls;
static SDL_AtomicInt s_reg_nonzero;
static SDL_AtomicInt s_commands;
static SDL_AtomicInt s_last_pair;

/** @brief Return whether OPENPETE_RUMBLE_LOG is set to a value other than empty or 0.
 *
 * The log traces every PadSetAct registration, every change in the sampled
 * actuator bytes, and every mod slot publish. The variable is read once and
 * cached for the life of the process.
 * @return 1 when logging is on, 0 otherwise. */
static int rumble_log_on(void)
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("OPENPETE_RUMBLE_LOG");
        v = (e && *e && *e != '0') ? 1 : 0;
    }
    return v;
}

/* ---- registration -------------------------------------------------------- */

void openpete_rumble_register(int slot, unsigned char* table, int len)
{
    if (slot < 0 || slot >= RUMBLE_SLOTS)
        return;

    SDL_AddAtomicInt(&s_reg_calls, 1);

    /* Whether any registration ever carried a non-zero byte is the falsifiable
     * half of the diagnosis, so it is counted rather than merely logged: a
     * one-shot implementation is only wrong BECAUSE the bytes are always zero
     * at call time. */
    if (table && len > 0) {
        int i, nonzero = 0;
        for (i = 0; i < len; i++)
            if (table[i]) nonzero = 1;
        if (nonzero) SDL_AddAtomicInt(&s_reg_nonzero, 1);
    }

    if (!table || len <= 0) {
        s_reg[slot].table = NULL;
        s_reg[slot].len   = 0;
        return;
    }

    if (len > 2) len = 2;   /* DualShock actuator buffer: {small, large} */
    s_reg[slot].table = table;
    s_reg[slot].len   = len;

    if (rumble_log_on()) {
        OPL_INFO_F("rumble",
                   "PadSetAct: slot %d buffer registered, len %d, bytes {%u, %u} "
                   "(call #%d)",
                   slot, len, (unsigned)table[0],
                   len > 1 ? (unsigned)table[1] : 0u,
                   SDL_GetAtomicInt(&s_reg_calls));
    }
}

/* ---- sim side ------------------------------------------------------------ */

void openpete_rumble_sample(void)
{
    unsigned char small_byte = 0, large_byte = 0;
    uint16_t low, high;
    int strength;

    /* Runahead: a speculative tick's guest writes roll back, and its actuator
     * bytes are a timeline the player never lived. Buzzing the pad for one
     * would be a side effect escaping speculation, exactly like audio. */
    if (openpete_speculative)
        return;

    if (s_reg[0].table && s_reg[0].len > 0) {
        small_byte = s_reg[0].table[0];
        if (s_reg[0].len > 1) large_byte = s_reg[0].table[1];
    }

    if (rumble_log_on()) {
        static int prev_small = -1, prev_large = -1;
        if ((int)small_byte != prev_small || (int)large_byte != prev_large) {
            OPL_INFO_F("rumble", "actuator bytes -> {%u, %u}",
                       (unsigned)small_byte, (unsigned)large_byte);
            prev_small = (int)small_byte;
            prev_large = (int)large_byte;
        }
    }

    strength = openpete_rumble_get_strength();
    low  = openpete_rumble_motor_u16((int)large_byte, /*is_small=*/0, strength);
    high = openpete_rumble_motor_u16((int)small_byte, /*is_small=*/1, strength);

    SDL_SetAtomicInt(&s_pair, (int)(((uint32_t)high << 16) | (uint32_t)low));
}

/* ---- pump side ----------------------------------------------------------- */

/* SDL's own maximum is 65535 ms ("never expires"). We pass a bounded duration and refresh it every pump iteration
 * instead, which makes the effect a dead-man switch: if the sim thread stalls
 * or the process wedges, the motors stop on their own within a second rather
 * than running until someone unplugs the pad. Refreshing is free — an
 * unchanged pair never reaches the driver, it only pushes the expiry out. */
#define RUMBLE_DURATION_MS 1000u

static void rumble_drive(uint16_t low, uint16_t high)
{
    SDL_Gamepad* gp = (SDL_Gamepad*)PsyX_Pad_GetGamepad(0);
    if (!gp)
        return;

    SDL_RumbleGamepad(gp, low, high, RUMBLE_DURATION_MS);
    SDL_AddAtomicInt(&s_commands, 1);
    SDL_SetAtomicInt(&s_last_pair, (int)(((uint32_t)high << 16) | (uint32_t)low));
}

/** @brief Return whether a gamepad's instance id is in the registered test-device set.
 *
 * The set holds the harness's virtual pads (openpete_rumble_set_test_device).
 * @param gp the gamepad about to be driven, or NULL.
 * @return 1 when gp is a test device, 0 otherwise or for NULL. */
static int driving_test_device(SDL_Gamepad* gp)
{
    if (!gp) return 0;
    const int id = (int)SDL_GetGamepadID(gp);
    for (int i = 0; i < OPENPETE_PAD_SLOTS; i++) {
        int stored = SDL_GetAtomicInt(&s_test_device[i]);
        if (stored && stored - 1 == id) return 1;
    }
    return 0;
}

/** @brief Return whether a pad must stay silent this pump iteration.
 *
 * The refusals shared by every slot: headless and missing keyboard focus
 * protect a human's controller and are lifted for a registered test device;
 * pause silences every pad. Focus is polled with SDL_GetKeyboardFocus rather
 * than tracked through focus events.
 * @param gp the gamepad about to be driven, or NULL (never a test device).
 * @return 1 when the pad must stay silent, 0 otherwise. */
static int rumble_gates_block(SDL_Gamepad* gp)
{
    if (!driving_test_device(gp)) {
        if (SDL_GetAtomicInt(&s_headless))
            return 1;
        if (!SDL_GetKeyboardFocus())
            return 1;
    }
    return SDL_GetAtomicInt(&s_paused) ? 1 : 0;
}

/** @brief Drive one mod-published slot's pair against the device currently holding the slot.
 *
 * An unarmed slot, or a slot with no device, is left alone. The device is
 * resolved from its instance id at drive time rather than held, so a device
 * pruned by the sampling thread reads as NULL instead of a closed handle.
 * When rumble_gates_block refuses, a zero pair with duration 0 is sent so a
 * pair already live in the driver stops; otherwise the pair is driven with
 * the RUMBLE_DURATION_MS expiry and the slot's command count is incremented.
 * @param slot 1 .. OPENPETE_PAD_SLOTS - 1. */
static void rumble_apply_mod_slot(int slot)
{
    if (!SDL_GetAtomicInt(&s_mod_armed[slot])) return;
    const unsigned id = openpete_host_input_slot_device_id(slot);
    if (!id) return;
    SDL_Gamepad* gp = SDL_GetGamepadFromID((SDL_JoystickID)id);
    if (!gp) return;

    const uint32_t pair = (uint32_t)SDL_GetAtomicInt(&s_mod_pair[slot]);
    const uint16_t low  = (uint16_t)(pair & 0xFFFFu);
    const uint16_t high = (uint16_t)(pair >> 16);

    if (rumble_gates_block(gp)) {
        /* A refusal has to reach the device, not just skip the drive: a pair
         * published before focus was lost is still live in the driver until
         * its expiry. */
        SDL_RumbleGamepad(gp, 0, 0, 0);
        return;
    }
    SDL_RumbleGamepad(gp, low, high, RUMBLE_DURATION_MS);
    SDL_AddAtomicInt(&s_mod_commands[slot], 1);
}

void openpete_rumble_apply(void)
{
    uint32_t pair;
    uint16_t low, high;
    SDL_Gamepad* gp = (SDL_Gamepad*)PsyX_Pad_GetGamepad(0);

    for (int slot = 1; slot < OPENPETE_PAD_SLOTS; slot++)
        rumble_apply_mod_slot(slot);

    /* Headless, focus and pause: see rumble_gates_block. Headless is a
     * refusal, not a default — a run with no window has no player at the desk,
     * and every gate battery and scripted capture is one. Focus is the layer
     * that protects a human from a background window buzzing a pad on their
     * desk, polled rather than tracked through FOCUS_LOST / FOCUS_GAINED so
     * there is no state to get out of step with reality. */
    if (rumble_gates_block(gp)) {
        openpete_rumble_stop();
        return;
    }

    pair = (uint32_t)SDL_GetAtomicInt(&s_pair);
    low  = (uint16_t)(pair & 0xFFFFu);
    high = (uint16_t)(pair >> 16);

    /* Unconditional — see the header for why a change gate deadlocks against
     * the stop paths above. */
    rumble_drive(low, high);
}

/* ---- mod-driven slots ---------------------------------------------------- */

void openpete_rumble_mod_set(int slot, int small_byte, int large_byte)
{
    if (slot < 1 || slot >= OPENPETE_PAD_SLOTS)
        return;

    const int strength = openpete_rumble_get_strength();
    const uint16_t low  = openpete_rumble_motor_u16(large_byte, /*is_small=*/0,
                                                    strength);
    const uint16_t high = openpete_rumble_motor_u16(small_byte, /*is_small=*/1,
                                                    strength);
    SDL_SetAtomicInt(&s_mod_pair[slot],
                     (int)(((uint32_t)high << 16) | (uint32_t)low));
    SDL_SetAtomicInt(&s_mod_armed[slot], 1);

    if (rumble_log_on())
        OPL_INFO_F("rumble", "mod slot %d pair: bytes {%d, %d} -> "
                   "{low %u, high %u}", slot, small_byte, large_byte,
                   (unsigned)low, (unsigned)high);
}

void openpete_rumble_mod_clear(int slot)
{
    if (slot < 1 || slot >= OPENPETE_PAD_SLOTS)
        return;
    SDL_SetAtomicInt(&s_mod_pair[slot], 0);
}

unsigned openpete_rumble_mod_commands(int slot)
{
    if (slot < 1 || slot >= OPENPETE_PAD_SLOTS) return 0u;
    return (unsigned)SDL_GetAtomicInt(&s_mod_commands[slot]);
}

void openpete_rumble_mod_pair(int slot, uint16_t* low, uint16_t* high)
{
    uint32_t p = (slot >= 1 && slot < OPENPETE_PAD_SLOTS)
               ? (uint32_t)SDL_GetAtomicInt(&s_mod_pair[slot]) : 0u;
    if (low)  *low  = (uint16_t)(p & 0xFFFFu);
    if (high) *high = (uint16_t)(p >> 16);
}

void openpete_rumble_stop(void)
{
    SDL_Gamepad* gp = (SDL_Gamepad*)PsyX_Pad_GetGamepad(0);
    if (!gp)
        return;
    /* Duration 0 with a zero pair clears both the expiry and the resend, which
     * is what "stop" should mean; SDL still skips the driver if it was already
     * silent, so calling this every pump iteration while unfocused is cheap. */
    SDL_RumbleGamepad(gp, 0, 0, 0);
    SDL_SetAtomicInt(&s_last_pair, 0);
}

/* ---- gates --------------------------------------------------------------- */

void openpete_rumble_set_test_device(unsigned instance_id)
{
    const int want = (int)instance_id + 1;
    for (int i = 0; i < OPENPETE_PAD_SLOTS; i++) {
        int stored = SDL_GetAtomicInt(&s_test_device[i]);
        if (stored == want) return;
        if (!stored) { SDL_SetAtomicInt(&s_test_device[i], want); return; }
    }
}

void openpete_rumble_set_headless(int headless)
{
    SDL_SetAtomicInt(&s_headless, headless ? 1 : 0);
}

void openpete_rumble_set_paused(int paused)
{
    SDL_SetAtomicInt(&s_paused, paused ? 1 : 0);
}

void openpete_rumble_set_strength(int strength)
{
    if (strength < -1) strength = -1;
    if (strength > 10) strength = 10;
    SDL_SetAtomicInt(&s_strength, strength);
    s_strength_init = 1;
}

int openpete_rumble_get_strength(void)
{
    if (!s_strength_init) return -1;
    return SDL_GetAtomicInt(&s_strength);
}

/* ---- diagnostics --------------------------------------------------------- */

unsigned openpete_rumble_register_calls(void)
{
    return (unsigned)SDL_GetAtomicInt(&s_reg_calls);
}

unsigned openpete_rumble_register_nonzero(void)
{
    return (unsigned)SDL_GetAtomicInt(&s_reg_nonzero);
}

unsigned openpete_rumble_commands(void)
{
    return (unsigned)SDL_GetAtomicInt(&s_commands);
}

void openpete_rumble_last_pair(uint16_t* low, uint16_t* high)
{
    uint32_t p = (uint32_t)SDL_GetAtomicInt(&s_last_pair);
    if (low)  *low  = (uint16_t)(p & 0xFFFFu);
    if (high) *high = (uint16_t)(p >> 16);
}
