/* tests/unit/moby_patch_list_test.c - the keyframe patch-list walk
 * (games/common/renderer/moby_patch_list.h) on a host array standing for
 * guest RAM.
 *
 * Under the Spyro 2/3 copy loop (body first) an entry with count 0 copies
 * one word and consumes one list word, so a list whose only header is
 * 0x80000000 replaces face word 0 with the word after it; under the Spyro 1
 * loop (count first) the same list copies nothing. A two-entry list whose
 * first entry has count 0 reads its second header one word further on under
 * body first. An oversized list is rolled back, and the span walk covers the
 * words the apply writes. */

#include "moby_patch_list.h"

#include <stdio.h>
#include <string.h>

#define RAM_BASE 0x80010000u
#define RAM_WORDS 256u
#define FB   (RAM_BASE + 0x100u)    /* the faceblock the offsets address */
#define LIST (RAM_BASE + 0x200u)    /* the patch list */

static uint32_t s_ram[RAM_WORDS];

/** @brief Read one word of the test RAM.
 * @param ctx Unused.
 * @param addr The address, inside the test RAM.
 * @return The word. */
static uint32_t rd(void* ctx, uint32_t addr) {
    (void)ctx;
    return s_ram[(addr - RAM_BASE) / 4u];
}

/** @brief Write one word of the test RAM.
 * @param ctx Unused.
 * @param addr The address, inside the test RAM.
 * @param val The word. */
static void wr(void* ctx, uint32_t addr, uint32_t val) {
    (void)ctx;
    s_ram[(addr - RAM_BASE) / 4u] = val;
}

/** @brief The test RAM word at an address.
 * @param addr The address.
 * @return The word. */
static uint32_t at(uint32_t addr) { return s_ram[(addr - RAM_BASE) / 4u]; }

/** @brief Set the test RAM word at an address.
 * @param addr The address.
 * @param val The word. */
static void put(uint32_t addr, uint32_t val) { s_ram[(addr - RAM_BASE) / 4u] = val; }

/** @brief Fill the faceblock with 0xF0000000 + word index and clear the list. */
static void reset(void) {
    memset(s_ram, 0, sizeof s_ram);
    for (uint32_t k = 0; k < 16u; k++) put(FB + 4u * k, 0xF0000000u + k);
}

/** @brief Report one failed expectation.
 * @param ok The expectation.
 * @param what Its description.
 * @return 0 when it holds, else 1. */
static int expect(int ok, const char* what) {
    if (!ok) fprintf(stderr, "FAIL %s\n", what);
    return !ok;
}

/** @brief Run the cases.
 * @return 0 when every case passes. */
int main(void) {
    const moby_patch_mem_t m = { NULL, rd, wr };
    moby_patch_save_t sv[8];
    int fails = 0;

    /* A list whose only (last) header is 0x80000000: count 0, offset 0. */
    reset();
    put(LIST, 0x80000000u);
    put(LIST + 4u, 0x12345678u);
    int n = moby_patch_list_apply(&m, FB, LIST, 1, sv, 8, 8);
    fails += expect(n == 1, "body first: count 0 copies one word");
    fails += expect(at(FB) == 0x12345678u, "body first: face word 0 replaced");
    fails += expect(at(FB + 4u) == 0xF0000001u, "body first: face word 1 kept");
    fails += expect(sv[0].addr == FB && sv[0].val == 0xF0000000u, "body first: original saved");
    reset();
    put(LIST, 0x80000000u);
    put(LIST + 4u, 0x12345678u);
    n = moby_patch_list_apply(&m, FB, LIST, 0, sv, 8, 8);
    fails += expect(n == 0, "count first: count 0 copies nothing");
    fails += expect(at(FB) == 0xF0000000u, "count first: face word 0 kept");

    /* Count 0 at offset 8, then a final entry of count 2 at offset 0x20: the
     * second header sits one word further on under body first. */
    reset();
    put(LIST, 0x00000008u);
    put(LIST + 4u, 0xAAAA0000u);
    put(LIST + 8u, 0x82000020u);
    put(LIST + 12u, 0xBBBB0000u);
    put(LIST + 16u, 0xBBBB0001u);
    n = moby_patch_list_apply(&m, FB, LIST, 1, sv, 8, 8);
    fails += expect(n == 3, "body first: two entries copy 1 + 2 words");
    fails += expect(at(FB + 8u) == 0xAAAA0000u, "body first: count-0 entry's word");
    fails += expect(at(FB + 0x20u) == 0xBBBB0000u && at(FB + 0x24u) == 0xBBBB0001u,
                    "body first: second entry's words");
    uint32_t lo = 0xFFFFFFFFu, hi = 0u;
    reset();
    put(LIST, 0x00000008u);
    put(LIST + 4u, 0xAAAA0000u);
    put(LIST + 8u, 0x82000020u);
    moby_patch_list_span(&m, FB, LIST, 1, 8, 8, &lo, &hi);
    fails += expect(lo == FB + 8u && hi == FB + 0x28u, "body first: span covers both entries");

    /* Count first: the same first header is a count-0 entry that does not
     * end the list, so the next header is the word right after it. */
    reset();
    put(LIST, 0x00000008u);
    put(LIST + 4u, 0x81000010u);
    put(LIST + 8u, 0xCCCC0000u);
    n = moby_patch_list_apply(&m, FB, LIST, 0, sv, 8, 8);
    fails += expect(n == 1 && at(FB + 0x10u) == 0xCCCC0000u && at(FB + 8u) == 0xF0000002u,
                    "count first: count-0 entry consumes no word");

    /* A list past the word cap is rolled back: every word restored. */
    reset();
    put(LIST, 0x02000000u);
    put(LIST + 4u, 0x11111111u);
    put(LIST + 8u, 0x22222222u);
    put(LIST + 12u, 0x80000010u);
    n = moby_patch_list_apply(&m, FB, LIST, 1, sv, 2, 8);
    fails += expect(n == -1, "oversized: rolled back");
    fails += expect(at(FB) == 0xF0000000u && at(FB + 4u) == 0xF0000001u, "oversized: words restored");

    if (fails) return 1;
    printf("moby_patch_list_test: PASS\n");
    return 0;
}
