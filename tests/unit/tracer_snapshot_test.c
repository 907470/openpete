/* tests/unit/tracer_snapshot_test.c — the tracer set copy trusts the guest
 * set count only up to the game's four-entry tables.
 *
 * A fake bus serves a set count of 0xFFFFFFFF over zeroed tables; the copy
 * must read the count and at most four table rows. */
#include "renderer/tracer_snapshot.h"

#include <stdio.h>
#include <string.h>

#define COUNT_VA   0x80075684u
#define CNTTAB_VA  0x800772C8u
#define BASETAB_VA 0x80078658u
#define POINTS_VA  0x80100000u

static uint32_t s_count;
static unsigned long s_reads;

/** @brief Fake bus word read: the set count, one live set of three points, zero elsewhere. */
static uint32_t fake_read_word(uint32_t a) {
    s_reads++;
    if (a == COUNT_VA) return s_count;
    if (a == CNTTAB_VA) return 3u;
    if (a == BASETAB_VA) return POINTS_VA;
    if (a >= POINTS_VA && a < POINTS_VA + 3u * 28u) return a - POINTS_VA;
    return 0u;
}

/** @brief Runs the crafted-count and valid-count cases.
 * @return 0 when both pass. */
int main(void) {
    static CPUState cpu;
    static tracer_set_t sets[TRACER_MAX_SETS];
    int fail = 0;
    cpu.read_word = fake_read_word;

    s_count = 0xFFFFFFFFu; s_reads = 0;
    int n = tracer_snapshot_take(&cpu, COUNT_VA, CNTTAB_VA, BASETAB_VA, sets, TRACER_MAX_SETS);
    /* count + 4 rows x (point count, base) + 3 points x 4 words */
    if (s_reads != 1u + 4u * 2u + 3u * 4u || n != 1) {
        fprintf(stderr, "FAIL garbage count: reads=%lu sets=%d\n", s_reads, n); fail++;
    }
    s_count = 1u; s_reads = 0;
    n = tracer_snapshot_take(&cpu, COUNT_VA, CNTTAB_VA, BASETAB_VA, sets, TRACER_MAX_SETS);
    if (n != 1 || sets[0].count != 3 || sets[0].pt[2].x != 56 || sets[0].pt[1].f != 28 + 24) {
        fprintf(stderr, "FAIL valid set: sets=%d count=%d\n", n, sets[0].count); fail++;
    }
    if (fail) return 1;
    printf("tracer_snapshot_test: OK\n");
    return 0;
}
