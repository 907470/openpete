/* core/runtime/rumble_curve.c - actuator byte to motor intensity, and its --vibration scaling.
 *
 * The large-motor cubic is taken from Pokopom
 * (https://github.com/KrossX/Pokopom, Pokopom/Input_XInput.cpp),
 * copyright (c) 2012 KrossX, MIT licence; the licence text ships as
 * licences/Pokopom.txt and NOTICE.md lists it.
 *
 * Pokopom's constants are the cubic coefficients POKOPOM_C3..POKOPOM_C0 and
 * its evaluation: double precision, pow(x, 3.0) and pow(x, 2.0) terms summed
 * in that order. RUMBLE_LARGE_INPUT_OFFSET, RUMBLE_LARGE_OUTPUT_SCALE and the
 * clamp at 1.0 are fitted to the measured intensity table
 * (tests/unit/data/rumble-response/reference.tsv); with them the function
 * reproduces the table bit for bit on all 256 bytes of both motors.
 */
#include "openpete_rumble.h"

#include <math.h>

/* Pokopom's large-motor cubic. */
#define POKOPOM_C3 0.006474549734772402
#define POKOPOM_C2 1.258165252213538
#define POKOPOM_C1 156.82454281087692
#define POKOPOM_C0 3.637978807091713e-11

/* Fitted to the measured table: the curve is evaluated at byte + 8 and divided
 * by 65535; results above 1 clamp to 1 (bytes 247..255). */
#define RUMBLE_LARGE_INPUT_OFFSET 8.0
#define RUMBLE_LARGE_OUTPUT_SCALE 65535.0

/* The byte is clamped to 0..255. Small motor: 1.0 when bit 0 of the byte is
 * set, 0.0 otherwise. Large motor: byte 0 is 0.0; otherwise x = byte + 8, and
 * c3 * x^3 - c2 * x^2 + c1 * x + c0 in double (Pokopom's term order, pow for
 * the powers) is divided by 65535, clamped to 1.0 and converted to float once.
 * Documented in openpete_rumble.h. */
float openpete_rumble_motor_strength(int raw_byte, int is_small)
{
    int b = raw_byte < 0 ? 0 : (raw_byte > 255 ? 255 : raw_byte);

    if (is_small)
        return (b & 1) ? 1.0f : 0.0f;
    if (b == 0)
        return 0.0f;

    double x = (double)b + RUMBLE_LARGE_INPUT_OFFSET;
    double v = POKOPOM_C3 * pow(x, 3.0) - POKOPOM_C2 * pow(x, 2.0) + POKOPOM_C1 * x + POKOPOM_C0;
    v /= RUMBLE_LARGE_OUTPUT_SCALE;
    if (v > 1.0)
        v = 1.0;
    return (float)v;
}

uint16_t openpete_rumble_motor_u16(int raw_byte, int is_small, int strength)
{
    float s;
    float scaled;

    /* 0 silences; -1 (unset, the flag's "leave the game alone" default) and
     * any out-of-range value apply the response unscaled. */
    if (strength == 0) return 0;
    if (strength < 0 || strength > 10) strength = 10;

    s = openpete_rumble_motor_strength(raw_byte, is_small);
    if (s <= 0.0f) return 0;

    /* Scaled in float and truncated toward zero, so a full-intensity byte at
     * strength 10 reaches 65535 and no nonzero intensity is rounded up. */
    scaled = s * ((float)strength / 10.0f) * 65535.0f;
    if (scaled < 0.0f)     scaled = 0.0f;
    if (scaled > 65535.0f) scaled = 65535.0f;

    return (uint16_t)scaled;
}
