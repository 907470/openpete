/* tests/unit/stick_response_test.c - openpete_stick_byte over the whole SDL
 * axis range.
 *
 * The console response is checked against the measured table
 * tests/unit/data/stick-response/console.runs (rows `first last byte`,
 * `#` comments; tools/stick_response_measure.py regenerates it) at every
 * value -32768..32767; linear against v / 256 + 128. Usage:
 * stick_response_test <table>.
 *
 * Exit 0 when every check passes, 1 on any failure, and 2 when the only
 * failures are console values and console returns the linear byte at
 * every value: the unimplemented stub, which ctest reports as skipped.
 */
#include "openpete_stick_response.h"

#include <stdio.h>

static int s_fail;
static unsigned char s_table[65536];
static unsigned char s_covered[65536];

/** @brief Record a failed check.
 * @param what The check.
 * @param v The axis value.
 * @param got The byte the mapping returned.
 * @param want The expected byte. */
static void fail(const char* what, int v, int got, int want)
{
    if (s_fail++ < 20) fprintf(stderr, "FAIL %s: v=%d got %d want %d\n", what, v, got, want);
}

/** @brief Load the measured runs into s_table.
 * @param path The runs file.
 * @return The number of axis values the runs cover, or -1 when the file cannot be read or a row is malformed. */
static int load_table(const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "stick_response_test: cannot open %s\n", path);
        return -1;
    }
    char line[512];
    int covered = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        int lo, hi, b;
        if (sscanf(line, "%d %d %d", &lo, &hi, &b) != 3 || lo < -32768 || hi > 32767 || lo > hi || b < 0 ||
            b > 255) {
            fprintf(stderr, "stick_response_test: malformed row: %s", line);
            fclose(f);
            return -1;
        }
        for (int v = lo; v <= hi; v++) {
            if (s_covered[v + 32768]++) fail("table row overlaps", v, b, s_table[v + 32768]);
            else covered++;
            s_table[v + 32768] = (unsigned char)b;
        }
    }
    fclose(f);
    return covered;
}

/** @brief Check the selected response: console until set, linear once set, console for any other value.
 * @param console_ok Nonzero when the console mapping matched the table, so its bytes can be compared. */
static void check_selected(int console_ok)
{
    /* The selected response: console until set; any value other than
     * linear, the options' -1 unset sentinel among them, selects console. */
    if (openpete_stick_response_get() != OPENPETE_STICK_RESPONSE_CONSOLE)
        fail("default response", 0, (int)openpete_stick_response_get(), OPENPETE_STICK_RESPONSE_CONSOLE);
    if (console_ok && openpete_stick_byte_current(0) != s_table[32768])
        fail("current console", 0, openpete_stick_byte_current(0), s_table[32768]);
    openpete_stick_response_set(OPENPETE_STICK_RESPONSE_LINEAR);
    if (openpete_stick_response_get() != OPENPETE_STICK_RESPONSE_LINEAR)
        fail("set linear", 0, (int)openpete_stick_response_get(), OPENPETE_STICK_RESPONSE_LINEAR);
    if (openpete_stick_byte_current(0) != 128) fail("current linear", 0, openpete_stick_byte_current(0), 128);
    openpete_stick_response_set(7);
    if (openpete_stick_response_get() != OPENPETE_STICK_RESPONSE_CONSOLE)
        fail("unknown set", 7, (int)openpete_stick_response_get(), OPENPETE_STICK_RESPONSE_CONSOLE);
    openpete_stick_response_set(OPENPETE_STICK_RESPONSE_LINEAR);
    openpete_stick_response_set(-1);
    if (openpete_stick_response_get() != OPENPETE_STICK_RESPONSE_CONSOLE)
        fail("unset sentinel", -1, (int)openpete_stick_response_get(), OPENPETE_STICK_RESPONSE_CONSOLE);
}

/** @brief Run the checks; print a summary.
 * @param argc Argument count.
 * @param argv argv[1] is the measured runs file.
 * @return 0 when every check passed, else 1. */
int main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: stick_response_test <console.runs>\n");
        return 1;
    }
    const int covered = load_table(argv[1]);
    if (covered != 65536) {
        fprintf(stderr, "stick_response_test: the table covers %d of 65536 axis values\n", covered);
        return 1;
    }
    int console_bad = 0, console_linear = 0;
    for (int v = -32768; v <= 32767; v++) {
        const int want = s_table[v + 32768];
        const int got = openpete_stick_byte(v, OPENPETE_STICK_RESPONSE_CONSOLE);
        console_linear += got == (v / 256) + 128;
        if (got != want) {
            console_bad++;
            if (console_bad <= 20) fprintf(stderr, "FAIL console: v=%d got %d want %d\n", v, got, want);
        }
        const int lin = openpete_stick_byte(v, OPENPETE_STICK_RESPONSE_LINEAR);
        if (lin != (v / 256) + 128) fail("linear", v, lin, (v / 256) + 128);
    }
    /* Out of range clamps; an unknown response maps as linear. */
    if (console_bad == 0 && openpete_stick_byte(40000, OPENPETE_STICK_RESPONSE_CONSOLE) != s_table[65535])
        fail("clamp high", 40000, openpete_stick_byte(40000, OPENPETE_STICK_RESPONSE_CONSOLE), s_table[65535]);
    if (console_bad == 0 && openpete_stick_byte(-40000, OPENPETE_STICK_RESPONSE_CONSOLE) != s_table[0])
        fail("clamp low", -40000, openpete_stick_byte(-40000, OPENPETE_STICK_RESPONSE_CONSOLE), s_table[0]);
    if (openpete_stick_byte(1000, (openpete_stick_response_t)7) != (1000 / 256) + 128)
        fail("unknown response", 1000, openpete_stick_byte(1000, (openpete_stick_response_t)7), (1000 / 256) + 128);
    /* The selected response. */
    check_selected(console_bad == 0);
    printf("stick_response_test: %d axis values checked against the measured table, %d console "
           "mismatch(es), %d failure(s)\n",
           covered, console_bad, s_fail);
    if (s_fail == 0 && console_bad > 0 && console_linear == 65536) {
        printf("stick_response_test: console is the unimplemented stub (the linear byte at every value)\n");
        return 2;
    }
    return (s_fail == 0 && console_bad == 0) ? 0 : 1;
}
