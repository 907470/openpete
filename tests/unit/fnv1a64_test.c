/* tests/unit/fnv1a64_test.c — verify openpete_fnv1a64.h against
 * published FNV1a-64 test vectors and confirm streaming-update
 * equals one-shot hashing. The determinism harness's per-frame
 * gameplay-state hash is built on top; a wrong hash here would
 * silently let the harness pass mismatched runs. */

#include "../../core/include/openpete_fnv1a64.h"

#include <stdio.h>
#include <string.h>

#define CHECK(cond, msg) do {                                      \
    if (!(cond)) {                                                 \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,    \
                msg);                                              \
        return 1;                                                  \
    }                                                              \
} while (0)

static int check_vector(const char* s, uint64_t expected) {
    uint64_t got = openpete_fnv1a64(s, strlen(s));
    if (got != expected) {
        fprintf(stderr, "FAIL fnv1a64(\"%s\") = 0x%016llx, expected 0x%016llx\n",
                s, (unsigned long long)got, (unsigned long long)expected);
        return 1;
    }
    return 0;
}

int main(void) {
    /* Published vectors — see http://www.isthe.com/chongo/src/fnv/test_fnv.c */
    if (check_vector("",       0xcbf29ce484222325ULL)) return 1;
    if (check_vector("a",      0xaf63dc4c8601ec8cULL)) return 1;
    if (check_vector("foobar", 0x85944171f73967e8ULL)) return 1;

    /* Streaming update over arbitrary chunk boundaries must match a
     * one-shot hash. The runtime hashes one region per call and
     * relies on this equivalence. */
    const char* msg = "the quick brown fox jumps over the lazy dog";
    size_t n = strlen(msg);
    uint64_t one_shot = openpete_fnv1a64(msg, n);
    for (size_t split = 0; split <= n; split++) {
        uint64_t h = openpete_fnv1a64_init();
        h = openpete_fnv1a64_update(h, msg, split);
        h = openpete_fnv1a64_update(h, msg + split, n - split);
        CHECK(h == one_shot, "streaming update diverges from one-shot");
    }

    /* Empty update is identity. */
    uint64_t h0 = openpete_fnv1a64_init();
    CHECK(openpete_fnv1a64_update(h0, NULL, 0) == h0, "empty update mutates");

    printf("PASS fnv1a64_test\n");
    return 0;
}
