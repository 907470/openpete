/* core/input/rumble_motor.c - actuator byte -> SDL motor value, scaled by the
 * --vibration strength.
 *
 * SDL-free and free of every other openpete header but its own, so the unit
 * tests link it with rumble_response.c alone.
 */

#include "openpete_rumble.h"

uint16_t openpete_rumble_motor_u16(int raw_byte, int is_small, int strength)
{
    float s;
    float scaled;

    /* 0 silences; -1 (unset, the flag's "leave the game alone" default) and
     * any out-of-range value apply the response unscaled. */
    if (strength == 0) return 0;
    if (strength < 0 || strength > 10) strength = 10;

    s = openpete_rumble_response(raw_byte, is_small);
    if (s <= 0.0f) return 0;

    /* Scaled in float and truncated toward zero, so a full-intensity byte at
     * strength 10 reaches 65535 and no nonzero intensity is rounded up. */
    scaled = s * ((float)strength / 10.0f) * 65535.0f;
    if (scaled < 0.0f)     scaled = 0.0f;
    if (scaled > 65535.0f) scaled = 65535.0f;

    return (uint16_t)scaled;
}
