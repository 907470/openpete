#ifndef OPENPETE_RUMBLE_H
#define OPENPETE_RUMBLE_H

/* Host-owned controller vibration.
 *
 * Real PSYQ libpad treats PadSetAct(port, table, len) as a REGISTRATION: the
 * game hands libpad the address of a small actuator buffer once, and libpad
 * reads that buffer and ships its bytes to the pad every vsync, forever. The
 * game never calls PadSetAct again — it just writes new bytes into its own
 * buffer.
 *
 * PsyCross implemented it as an immediate one-shot rumble command instead,
 * reading the bytes at call time. Spyro registers once at boot (gamepad.c,
 * PadVSync) while the buffer is still {0,0}, so the only actuator command the
 * port ever sent was a zero-amplitude one at startup and every later buffer
 * write went unread. That is why vibration never worked for anyone.
 *
 * This module restores the registration semantics, split across the two
 * threads the way the input path already is:
 *
 *   guest PadVSync   writes ---> the actuator buffer in PSX RAM
 *   libpad PadSetAct registers -> openpete_rumble_register()
 *   sim thread, per tick -------> openpete_rumble_sample()   reads + publishes
 *   main pump ------------------> openpete_rumble_apply()    drives SDL
 *
 * The sim thread samples so the bytes shipped belong to one whole tick; the
 * main thread applies because it owns the SDL gamepad handle (open and close
 * both happen in its event handler), which removes any question of a worker
 * racing a close.
 */

#include <stdint.h>

/* ---- actuator byte -> motor ------------------------------------------- */

/** @brief Convert an actuator byte to a motor intensity in 0..1 equal to the measured reference response.
 *
 * The response is a measurement of the oracle reference's motor output at
 * its default settings, taken with tools/rumble_response_measure.py at the
 * reference head recorded in the header of
 * tests/unit/data/rumble-response/reference.tsv; the function returns that
 * table's row for every byte of each motor, bit for bit, and
 * tests/unit/rumble_response_test.c checks all 512. The small motor is 1 on
 * an odd byte and 0 on an even one. The large motor is 0 at byte 0; for
 * any other byte it is the cubic from the Pokopom pad plugin (copyright
 * (c) 2012 KrossX, MIT licence), evaluated in double at byte + 8, divided
 * by 65535 and clamped to 1, an input offset and output scale fitted to
 * the measured table; it is nondecreasing and reaches 1 at byte 247.
 * @param raw_byte the actuator byte; clamped to 0..255.
 * @param is_small nonzero selects the small motor (actuator byte 0), zero
 *        the large motor (actuator byte 1).
 * @return the intensity, 0..1. */
float openpete_rumble_motor_strength(int raw_byte, int is_small);

/** @brief Convert an actuator byte to SDL's u16 motor range, scaled by a 0..10 host strength.
 *
 * The byte goes through openpete_rumble_motor_strength, is scaled by strength / 10
 * and by 65535 in float, and is truncated toward zero. A strength of 10
 * applies the measured response unscaled and 0 silences; -1 (unset) and any
 * value outside 0..10 read as 10.
 * @param raw_byte the actuator byte; clamped to 0..255.
 * @param is_small nonzero selects the small (on/off) motor.
 * @param strength the host strength.
 * @return the motor intensity, 0..65535. */
uint16_t openpete_rumble_motor_u16(int raw_byte, int is_small, int strength);

/* ---- registration and the per-tick / per-pump halves -------------------- */

/* Called from libpad's PadSetAct. `table` is a host pointer into PSX RAM and
 * stays valid for the life of the process; `len` is 0..2 in practice. A NULL
 * table or len <= 0 unregisters the slot. */
void openpete_rumble_register(int slot, unsigned char* table, int len);

/* Sim thread, once per committed tick: read the registered buffer and publish
 * the motor pair. Inert while speculating (runahead must not buzz the pad). */
void openpete_rumble_sample(void);

/* Main pump thread, once per pump iteration: drive SDL from the published
 * pair. Silent when headless, when the window has no input focus, or when the
 * strength is 0.
 *
 * Deliberately unconditional rather than change-gated. SDL_RumbleJoystick
 * already skips the driver when the pair is unchanged and merely refreshes the
 * expiry, resends the current pair every SDL_RUMBLE_RESEND_MS so a dropped
 * wireless packet heals itself, and auto-stops at expiry — so a custom rate
 * limiter would duplicate three things SDL ships. Worse, a change-gated
 * re-issue gets permanently stuck off: a host-side stop (focus loss, pause)
 * does not change the PUBLISHED pair, so the detector would never re-fire on
 * focus regain and the pad would stay dead until the game happened to change
 * value. Unconditional re-issue has no such state to get stuck in. */
void openpete_rumble_apply(void);

/* Immediate silence, from the pump thread. Used on focus loss, pause, exit and
 * controller removal. */
void openpete_rumble_stop(void);

/* ---- host-side gates ---------------------------------------------------- */

/* Headless is a refusal, not a default: no window means no player. */
void openpete_rumble_set_headless(int headless);

/* Name the harness's virtual pad (OPENPETE_INPUT_VGAMEPAD), by SDL instance id.
 *
 * The headless and focus refusals protect a human's controller; a virtual
 * device a harness attached is not one. Lifting them for that device — and
 * only that device, re-checked against the handle actually driven — is what
 * lets the vibration oracle observe the REAL apply path headlessly, instead of
 * testing a mock of it. A real pad that has taken slot 0 will not match, so a
 * diagnostic run still cannot buzz a controller on the desk. */
void openpete_rumble_set_test_device(unsigned instance_id);

/* 0..10 host strength, -1 = unset (parity). Mirrors --vibration. */
void openpete_rumble_set_strength(int strength);
int  openpete_rumble_get_strength(void);

/* Pause (F7 and friends): motors silent while held, restored on release
 * without needing the game to change value. */
void openpete_rumble_set_paused(int paused);

/* ---- diagnostics -------------------------------------------------------- */

/* PadSetAct calls seen, and whether any carried a non-zero actuator byte.
 * The second is the falsifiable half of the diagnosis: under the old one-shot
 * semantics every call carries {0,0}, because the game registers once at boot
 * and only writes bytes afterwards. */
unsigned openpete_rumble_register_calls(void);
unsigned openpete_rumble_register_nonzero(void);

/* SDL rumble calls actually issued, and the last pair driven. */
unsigned openpete_rumble_commands(void);
void     openpete_rumble_last_pair(uint16_t* low, uint16_t* high);

#endif /* OPENPETE_RUMBLE_H */
