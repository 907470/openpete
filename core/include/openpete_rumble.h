#ifndef OPENPETE_RUMBLE_H
#define OPENPETE_RUMBLE_H

/* Host-owned controller vibration.
 *
 * Real PSYQ libpad treats PadSetAct(port, table, len) as a REGISTRATION: the
 * game hands libpad the address of a small actuator buffer once, and libpad
 * reads that buffer and ships its bytes to the pad every vsync, forever. The
 * game never calls PadSetAct again - it just writes new bytes into its own
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
 * an odd byte and 0 on an even one; the large motor reads a 256-entry table
 * of the measured bit patterns, 0 at byte 0 and nondecreasing to 1.
 * @param raw_byte the actuator byte; clamped to 0..255.
 * @param is_small nonzero selects the small motor (actuator byte 0), zero
 *        the large motor (actuator byte 1).
 * @return the intensity, 0..1. */
float openpete_rumble_response(int raw_byte, int is_small);

/** @brief Convert an actuator byte to SDL's u16 motor range, scaled by a 0..10 host strength.
 *
 * The byte goes through openpete_rumble_response, is scaled by strength / 10
 * and by 65535 in float, and is truncated toward zero. A strength of 10
 * applies the measured response unscaled and 0 silences; -1 (unset) and any
 * value outside 0..10 read as 10.
 * @param raw_byte the actuator byte; clamped to 0..255.
 * @param is_small nonzero selects the small (on/off) motor.
 * @param strength the host strength.
 * @return the motor intensity, 0..65535. */
uint16_t openpete_rumble_motor_u16(int raw_byte, int is_small, int strength);

/* ---- registration and the per-tick / per-pump halves -------------------- */

/** @brief Register a slot's actuator buffer; installed as PsyCross's PadSetAct hook.
 *
 * Every call is counted, and a call whose bytes include a non-zero one is
 * counted separately. Only the first two bytes ({small, large}) are
 * sampled. Under OPENPETE_RUMBLE_LOG the registration is logged.
 * @param slot pad slot; only 0 and 1 are stored, others are ignored
 *        uncounted.
 * @param table host pointer into PSX RAM, valid for the life of the
 *        process; NULL unregisters the slot.
 * @param len buffer length, 0..2 in practice; len <= 0 unregisters the
 *        slot. */
void openpete_rumble_register(int slot, unsigned char* table, int len);

/** @brief Read slot 0's registered actuator buffer and publish the motor pair; sim thread, once per committed tick.
 *
 * The bytes are converted with openpete_rumble_motor_u16 at the current
 * strength and published as one 32-bit word (large motor low, small motor
 * high). An unregistered slot publishes {0,0}. Does nothing while
 * speculating, so a runahead tick never reaches a motor. Under
 * OPENPETE_RUMBLE_LOG each change in the bytes is logged. */
void openpete_rumble_sample(void);

/** @brief Drive the pads from the published motor pairs; main pump thread, once per pump iteration.
 *
 * Mod-driven slots 1..OPENPETE_PAD_SLOTS-1 are driven first, then player
 * one's pad from the game's pair with a 1000 ms expiry. A pad stays silent
 * (stopped, not skipped) while paused, and while headless or without
 * keyboard focus unless it is a registered test device. Strength 0
 * publishes a zero pair, which silences the motors.
 *
 * The drive is unconditional rather than change-gated. SDL_RumbleJoystick
 * already skips the driver when the pair is unchanged and only refreshes
 * the expiry, resends the current pair every SDL_RUMBLE_RESEND_MS so a
 * dropped wireless packet recovers, and stops at expiry. A change-gated
 * re-issue would stay off after a host-side stop (focus loss, pause),
 * since the stop does not change the published pair and the detector
 * would not fire again on focus regain. */
void openpete_rumble_apply(void);

/** @brief Silence player one's pad immediately; pump thread.
 *
 * Issues a zero pair with duration 0 to the gamepad holding slot 0, which
 * clears the expiry, and zeroes the last-driven pair. Does nothing while no
 * pad holds slot 0. openpete_rumble_apply calls it every pump iteration
 * while a gate (headless, focus loss, pause) blocks slot 0; SDL skips the
 * driver when the pad is already silent. */
void openpete_rumble_stop(void);

/* ---- host-side gates ---------------------------------------------------- */

/** @brief Set whether the run is headless, which silences every pad except registered test devices.
 *
 * Headless is a refusal, not a default: no window means no player. Set
 * once from the options at host init.
 * @param headless nonzero for a headless run. */
void openpete_rumble_set_headless(int headless);

/** @brief Add one of the harness's virtual pads (OPENPETE_INPUT_VGAMEPAD) to the test-device set.
 *
 * The headless and focus refusals protect a connected physical controller and are
 * lifted for a device in the set, checked against the instance id of the
 * handle being driven, so the vibration harness observes the real apply
 * path headlessly. The set holds OPENPETE_PAD_SLOTS ids, one per slot the
 * harness attaches; a duplicate id is ignored and an id arriving when the
 * set is full is dropped. A real pad is never in the set, so the refusals
 * still apply to it.
 * @param instance_id the virtual pad's SDL instance id. */
void openpete_rumble_set_test_device(unsigned instance_id);

/* ---- mod-driven slots --------------------------------------------------- */

/** @brief Publish a mod-driven slot's motor pair from libpad's actuator encoding.
 *
 * The bytes are the two the game itself writes to its actuator buffer.
 * They go through openpete_rumble_motor_u16 with the current strength, so
 * one byte-to-motor conversion and the user's strength setting serve every
 * slot. The first call arms the slot; openpete_rumble_apply leaves an
 * unarmed slot alone. The pair is level-triggered: it is re-driven every
 * pump iteration with the 1000 ms expiry until the next call for that
 * slot or openpete_rumble_mod_clear. Publish {0,0} to stop.
 * @param slot 1 .. OPENPETE_PAD_SLOTS - 1; slot 0 is the game's own
 *        actuator table and is ignored, as is any out-of-range slot.
 * @param small_byte the small motor's on/off byte (bit 0).
 * @param large_byte the large motor's 0..255 byte. */
void openpete_rumble_mod_set(int slot, int small_byte, int large_byte);

/** @brief Silence one mod-driven slot by publishing a zero pair (mod disabled, unloaded, or reloaded).
 *
 * The slot stays armed, so the stop reaches the device on the next pump
 * iteration.
 * @param slot 1 .. OPENPETE_PAD_SLOTS - 1; any other value is ignored. */
void openpete_rumble_mod_clear(int slot);

/** @brief Return how many SDL rumble calls drove a mod-driven slot's published pair.
 *
 * The zero-pair stops a refusal issues are not counted.
 * @param slot 1 .. OPENPETE_PAD_SLOTS - 1.
 * @return the count, or 0 for any other slot. */
unsigned openpete_rumble_mod_commands(int slot);
/** @brief Report the motor pair last published for a mod-driven slot.
 *
 * Zero for a slot no mod has driven and after openpete_rumble_mod_clear.
 * @param slot 1 .. OPENPETE_PAD_SLOTS - 1; any other slot reports zero.
 * @param low receives the large-motor value; NULL is ignored.
 * @param high receives the small-motor value; NULL is ignored. */
void     openpete_rumble_mod_pair(int slot, uint16_t* low, uint16_t* high);

/** @brief Set the host vibration strength that scales every motor pair (--vibration).
 *
 * 10 applies the measured response unscaled, 0 silences, and -1 is
 * unset and reads as 10. The value is clamped to -1..10. Before the first call
 * openpete_rumble_get_strength returns -1.
 * @param strength 0..10, or -1 for unset. */
void openpete_rumble_set_strength(int strength);
/** @brief Return the host vibration strength.
 *
 * @return 0..10, or -1 before openpete_rumble_set_strength has been called. */
int  openpete_rumble_get_strength(void);

/** @brief Set the host pause gate: motors are silent while it is set and resume on release without the game changing value.
 *
 * Set by the host loop's pause toggle.
 * @param paused nonzero while the host has paused the game. */
void openpete_rumble_set_paused(int paused);

/* ---- diagnostics -------------------------------------------------------- */

/* PadSetAct calls seen, and how many carried a non-zero actuator byte. The
 * game registers once at boot, while its buffer is still {0,0}, and only
 * writes bytes afterwards, so a one-shot implementation sees only zero
 * bytes. */
/** @brief Return how many openpete_rumble_register calls for slot 0 or 1 have been seen.
 *
 * @return the call count since start. */
unsigned openpete_rumble_register_calls(void);
/** @brief Return how many openpete_rumble_register calls carried a non-zero actuator byte.
 *
 * Only calls with a non-NULL table and a positive length are scanned, over the
 * whole length given.
 * @return the count since start. */
unsigned openpete_rumble_register_nonzero(void);

/* SDL rumble calls issued for player one's pad, and the last pair driven. */
/** @brief Return how many SDL rumble calls openpete_rumble_apply issued for player one's pad.
 *
 * Stops are not counted; nothing is issued or counted while no pad holds
 * slot 0.
 * @return the command count since start. */
unsigned openpete_rumble_commands(void);
/** @brief Report the motor pair openpete_rumble_apply last drove on player one's pad.
 *
 * Zero before any drive and after openpete_rumble_stop.
 * @param low receives the large-motor value; NULL is ignored.
 * @param high receives the small-motor value; NULL is ignored. */
void     openpete_rumble_last_pair(uint16_t* low, uint16_t* high);

#endif /* OPENPETE_RUMBLE_H */
