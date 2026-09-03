// games/spyro-1/features/vibration_native.c — --vibration: preset the game's
// own controller-vibration setting.
//
// Spyro's options menu (the sound/options submenu, D_800757C8 == 1) carries a
// VIBRATION row at cursor index 3, toggling one flag:
//
//   g_ActEnabled    0x800757A4   0 = off, 1 = on
//
// That flag is upstream of everything: PadVSync zeroes all four vibration
// timers while it is clear (gamepad.c), so with it off the game never writes a
// non-zero actuator byte and there is nothing for the host to ship.
//
// PRESET. A positive --vibration writes the flag so a launch starts with
// vibration enabled even if the player's save has it off. A one-shot write at
// boot would not survive — initialization.c sets it to 1 on its own, and
// loading a save file restores whatever the memory card holds (save_file.c) —
// so the preset is re-stamped whenever the guest value drifts, but only while
// the player is not sitting on that options row. Toggling the row by hand is
// how the player takes the setting back: the preset is dropped (host request
// cleared to -1) and the menu owns it from then on.
//
// --vibration 0 WRITES NOTHING. Silence is achieved entirely host-side — the
// strength scale forces both motors to zero — so clearing this flag would be a
// second, gameplay-visible effect the goal does not need. It is not free:
// with the flag clear PadVSync zeroes four timers it would otherwise be
// counting down, so the guest's RAM stops matching what the same input
// produces on hardware. That matters more than it sounds, because 0 is the
// value every unattended launcher sets (scripts/fences/rumble_guard.py): if it
// wrote guest state, the oracle and determinism gates would be running against
// a game that had been quietly modified, and the first symptom would be the
// oracle failing for a reason nobody would connect to vibration.
//
// So the in-game options row keeps reading whatever the player or their save
// chose, while the motors stay silent. The row is inert in that state —
// nothing it enables can reach a motor at strength 0.
//
// Every guest write happens here, on the tick hook. The dev-overlay and O-menu
// rows only move the host request, so a UI-thread edit never lands in gameplay
// RAM off-schedule.
//
// One thing deliberately NOT done here: g_ActAvailable (0x800756D8) is left
// alone. The game derives it from PadInfoMode, which PsyCross answers with a
// constant, so it reads 1 even with no pad attached — the options row is
// offered when nothing can rumble. Deriving it from host controller presence
// would fix the cosmetic lie by writing host state into gameplay-readable RAM,
// where it would then feed the four timers PadVSync clears, and diverge from
// hardware the moment a run has no pad. Pad presence stays a host-side
// concern.
//
// Addresses from manifest/globals.toml.

#include "features/vibration_native.h"

#include "openpete.h"        /* openpete_get/set_vibration */
#include "psx_runtime_internal.h"     /* CPUState + its read/write callbacks */

#include <stdint.h>

/* Runahead speculation flag (openpete_fiber.h). Declared rather than included:
 * that header pulls in SDL, which would drag a window library into this
 * module's console unit test. */
extern int openpete_speculative;

#define ADDR_ACT_ENABLED     0x800757A4u /* g_ActEnabled, 0/1 */
#define ADDR_OPTIONS_SUBMENU 0x800757C8u /* D_800757C8, 1 = options submenu open */
#define ADDR_OPTIONS_ROW     0x80075720u /* D_80075720, menu cursor row */

#define OPTIONS_SUBMENU_SOUND 1u
#define OPTIONS_ROW_VIBRATION 3u

/* The request we are currently serving (-1 = none) and the value we last
 * wrote (-1 = never). Two variables rather than one because "we have not
 * stamped yet" and "we stamped a 0" are different states, and conflating them
 * would make the release check below fire on the first tick of a fresh
 * request. */
static int s_requested = -1;
static int s_stamped   = -1;

/* The host request is a 0..10 strength; the guest flag is a bit. */
static int want_enabled(int strength) { return strength > 0 ? 1 : 0; }

void spyro1_vibration_on_tick(CPUState* cpu)
{
    int want, enabled, menu_open, on_row;

    /* Runahead: a speculative tick's guest writes roll back, but a release —
     * which clears the host-side request — would not. Let the real tick
     * decide. */
    if (openpete_speculative) return;

    want = openpete_get_vibration();

    /* A new request — the boot seed, or a dev-overlay / O-menu edit — starts
     * over, so a re-enable after a release takes effect again. */
    if (want != s_requested) {
        s_requested = want;
        s_stamped   = -1;
    }
    /* -1 = leave the game alone; 0 = host-side silence, also without touching
     * guest RAM (see the header comment — every unattended launcher sets 0,
     * and a guest write there would put the gates on a modified game). */
    if (want <= 0) return;

    enabled   = want_enabled(want);
    menu_open = (cpu->read_word(ADDR_OPTIONS_SUBMENU) == OPTIONS_SUBMENU_SOUND);
    on_row    = (cpu->read_word(ADDR_OPTIONS_ROW) == OPTIONS_ROW_VIBRATION);

    /* While the player is on the vibration row the setting is theirs: stamping
     * would fight their input. Moving it away from our value while they are
     * there is them taking it back for good — and that row is the only writer
     * that runs with this submenu open, so a drift here cannot be the game
     * reseeding.
     *
     * The row check matters: the submenu's other rows (sound, music, mono,
     * camera) do not touch this flag, so releasing on any in-menu drift would
     * hand the setting back for an edit the player never made to it. */
    if (menu_open && on_row) {
        if (s_stamped >= 0 &&
            (int)cpu->read_word(ADDR_ACT_ENABLED) != s_stamped) {
            s_requested = -1;
            openpete_set_vibration(-1);
        }
        return;
    }

    /* Re-stamp only on drift, so the common case is a read and nothing more. */
    if ((int)cpu->read_word(ADDR_ACT_ENABLED) == enabled) {
        s_stamped = enabled;
        return;
    }
    cpu->write_word(ADDR_ACT_ENABLED, (uint32_t)enabled);
    s_stamped = enabled;
}
