/* core/input/stick_response.c - host stick axis value -> PSX analog stick
 * byte, per --stick-response.
 *
 * Kept free of SDL and of every other openpete header so the unit test links
 * the mapping alone. */

#include "openpete_stick_response.h"

#include <stdatomic.h>

static _Atomic int s_response = OPENPETE_STICK_RESPONSE_CONSOLE;

/* Console gain in bytes per full half-axis, as a 16-bit fixed-point
 * fraction: 11113270 / 65536 = 169.57504 bytes. Every gain in
 * 169.5750077..169.5750668 reproduces the measured table; this value sits
 * inside that interval. */
#define CONSOLE_GAIN_Q16 11113270LL

/** @brief Map one axis value under the console response.
 *
 * Each half of the axis is a line through its own centre offset, floored and
 * clamped to 0..255, in integer arithmetic only:
 * v >= 0: floor(127.5 + G * v / 32767); v < 0: floor(128 + G * v / 32768),
 * G = CONSOLE_GAIN_Q16 / 65536. The result rests at 127 for
 * -193..96, steps every 193 or 194 values, reads 0 at and below -24541 and
 * 255 at and above 24637, and equals
 * tests/unit/data/stick-response/console.runs for every value
 * -32768..32767 (stick_response_test checks all of them).
 * @param v The axis value, -32768..32767.
 * @return The stick byte. */
static unsigned char console_byte(int v)
{
    long long b;
    if (v >= 0) {
        b = (255LL * 32767 * 32768 + CONSOLE_GAIN_Q16 * v) / (32767LL * 65536);
    } else {
        const long long num = 128LL * 32768 * 65536 + CONSOLE_GAIN_Q16 * v;
        b = num < 0 ? 0 : num / (32768LL * 65536);
    }
    return (unsigned char)(b > 255 ? 255 : b);
}

unsigned char openpete_stick_byte(int value, openpete_stick_response_t response)
{
    const int v = value < -32768 ? -32768 : (value > 32767 ? 32767 : value);
    if (response == OPENPETE_STICK_RESPONSE_CONSOLE)
        return console_byte(v);
    return (unsigned char)((v / 256) + 128);
}

void openpete_stick_response_set(int response)
{
    atomic_store_explicit(&s_response,
                          response == OPENPETE_STICK_RESPONSE_LINEAR
                              ? OPENPETE_STICK_RESPONSE_LINEAR
                              : OPENPETE_STICK_RESPONSE_CONSOLE,
                          memory_order_relaxed);
}

openpete_stick_response_t openpete_stick_response_get(void)
{
    return (openpete_stick_response_t)atomic_load_explicit(&s_response, memory_order_relaxed);
}

unsigned char openpete_stick_byte_current(int value)
{
    return openpete_stick_byte(value, openpete_stick_response_get());
}
