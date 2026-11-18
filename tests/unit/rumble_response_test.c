/* rumble_response_test - the motor intensity per actuator byte against the
 * measured reference table (data/rumble-response/reference.tsv, written by
 * tools/rumble_response_measure.py).
 *
 * Every byte of each motor of openpete_rumble_response, the intensity the
 * program ships, is compared with the table bit for bit, as IEEE-754
 * singles; all 512 entries must match.
 *
 * Before either check the comparator is shown to reject two known-wrong
 * functions: byte / 255 on both motors, and the table itself with one
 * entry's bits moved by one. A comparator that passes either is broken and
 * fails the test.
 *
 * usage: rumble_response_test TABLE
 */

#include "openpete_rumble.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define N_BYTES 256

static uint32_t s_table[2][N_BYTES]; /* [is_small ? 0 : 1][byte] */

/** @brief Return the IEEE-754 bits of a float. */
static uint32_t bits_of(float f)
{
    uint32_t b;
    memcpy(&b, &f, sizeof b);
    return b;
}

/** @brief Return the float an IEEE-754 bit pattern holds. */
static float float_of(uint32_t b)
{
    float f;
    memcpy(&f, &b, sizeof f);
    return f;
}

/** @brief Load the table's 256 rows; return 0 on success, nonzero on a malformed or short file. */
static int load_table(const char *path)
{
    FILE *fp = fopen(path, "r");
    char line[256];
    int seen[N_BYTES] = {0};
    int rows = 0;
    int i;
    if (!fp) {
        printf("FAIL: cannot open %s\n", path);
        return 1;
    }
    while (fgets(line, sizeof line, fp)) {
        unsigned byte;
        unsigned small_bits;
        unsigned large_bits;
        if (line[0] == '#' || line[0] == '\n')
            continue;
        if (sscanf(line, "%u %x %x", &byte, &small_bits, &large_bits) != 3 || byte >= N_BYTES ||
            seen[byte]) {
            printf("FAIL: malformed table row: %s", line);
            fclose(fp);
            return 1;
        }
        seen[byte] = 1;
        s_table[0][byte] = small_bits;
        s_table[1][byte] = large_bits;
        rows++;
    }
    fclose(fp);
    for (i = 0; i < N_BYTES; i++) {
        if (!seen[i]) {
            printf("FAIL: table has %d rows, byte %d missing\n", rows, i);
            return 1;
        }
    }
    return 0;
}

typedef float (*curve_fn)(int raw_byte, int is_small);

/** @brief Count the entries where a curve's bits differ from the table, printing the first few. */
static int mismatches(const char *name, curve_fn fn, int quiet)
{
    int bad = 0;
    int m;
    int b;
    for (m = 0; m < 2; m++) {
        for (b = 0; b < N_BYTES; b++) {
            const uint32_t want = s_table[m][b];
            const uint32_t got = bits_of(fn(b, m == 0));
            if (got != want) {
                if (!quiet && bad < 8)
                    printf("  %s: %s motor byte %d: got %08X (%.9g), want %08X (%.9g)\n", name,
                           m == 0 ? "small" : "large", b, got, (double)float_of(got), want,
                           (double)float_of(want));
                bad++;
            }
        }
    }
    return bad;
}

/** @brief A known-wrong curve: the byte over 255 on both motors. */
static float wrong_linear(int raw_byte, int is_small)
{
    (void)is_small;
    return (float)raw_byte / 255.0f;
}

/** @brief A known-wrong curve: the table with large-motor byte 120's bits moved by one. */
static float wrong_one_ulp(int raw_byte, int is_small)
{
    uint32_t b = s_table[is_small ? 0 : 1][raw_byte];
    if (!is_small && raw_byte == 120)
        b += 1;
    return float_of(b);
}

int main(int argc, char **argv)
{
    int rc = 0;
    int bad;
    if (argc != 2) {
        printf("usage: rumble_response_test TABLE\n");
        return 2;
    }
    if (load_table(argv[1]))
        return 1;

    /* The comparator rejects known-wrong curves. */
    bad = mismatches("byte/255", wrong_linear, 1);
    printf("control byte/255: %d of 512 entries differ\n", bad);
    if (bad == 0) {
        printf("FAIL: the comparator accepted byte/255\n");
        rc = 1;
    }
    bad = mismatches("one ulp", wrong_one_ulp, 1);
    printf("control one-ulp: %d of 512 entries differ\n", bad);
    if (bad != 1) {
        printf("FAIL: the comparator did not reject exactly the one moved entry\n");
        rc = 1;
    }

    bad = mismatches("openpete_rumble_response", openpete_rumble_response, 0);
    printf("openpete_rumble_response: %d of 512 entries differ\n", bad);
    if (bad)
        rc = 1;

    printf("%s\n", rc ? "FAIL" : "PASS");
    return rc;
}
