/* core/include/openpete_stick_response.h - host stick axis value -> PSX
 * analog stick byte (--stick-response). SDL-free, so the unit test links the
 * mapping alone. */
#ifndef OPENPETE_STICK_RESPONSE_H
#define OPENPETE_STICK_RESPONSE_H

#ifdef __cplusplus
extern "C" {
#endif

/* How a host axis value (-32768..32767, SDL's range) becomes the byte a
 * DualShock reports for that stick axis (0..255). */
typedef enum openpete_stick_response {
    /* v / 256 + 128, truncated toward zero: linear over the whole range,
     * rest 128. */
    OPENPETE_STICK_RESPONSE_LINEAR = 0,
    /* The default. The ends of the byte range are reached before the
     * stick's physical limit; equal, for every axis value, to the measured
     * table tests/unit/data/stick-response/console.runs, the oracle
     * reference emulator's live input path at its default settings. */
    OPENPETE_STICK_RESPONSE_CONSOLE = 1,
} openpete_stick_response_t;

/** @brief Map one host stick axis value to the PSX stick byte under a response.
 *
 * OPENPETE_STICK_RESPONSE_LINEAR is v / 256 + 128 in C integer division.
 * OPENPETE_STICK_RESPONSE_CONSOLE returns, for every v, the byte the
 * measured table tests/unit/data/stick-response/console.runs lists.
 * An unknown response maps as LINEAR.
 * @param value The host axis value; values outside -32768..32767 are clamped.
 * @param response The response.
 * @return The stick byte, 0..255. */
unsigned char openpete_stick_byte(int value, openpete_stick_response_t response);

/** @brief Select the response the live pad decode applies from the next sample on.
 *
 * Thread-safe; read on the thread that samples the pad.
 * @param response An openpete_stick_response_t; any other value (the options'
 *        -1 unset sentinel among them) selects CONSOLE, the default. */
void openpete_stick_response_set(int response);

/** @brief Return the response the live pad decode applies.
 *
 * @return OPENPETE_STICK_RESPONSE_CONSOLE until openpete_stick_response_set selects another. */
openpete_stick_response_t openpete_stick_response_get(void);

/** @brief Map one host stick axis value under the selected response.
 *
 * openpete_stick_byte with openpete_stick_response_get(); the decode hook
 * PsyCross calls for each gamepad stick axis, and the keymap's stick re-map.
 * @param value The host axis value.
 * @return The stick byte. */
unsigned char openpete_stick_byte_current(int value);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_STICK_RESPONSE_H */
