/* tests/unit/psx_region_test.c — verify PSX RAM virtualization region
 * table behaves correctly: registration, lookup, overlap rejection,
 * unregister, resolve_region. The table backs cold-path memory
 * resolvers (cpu.c mem_ptr / openpete_psx_to_host_ptr); a regression
 * here would silently corrupt presentation buffers or leak gameplay
 * accesses into renderer-only memory.
 *
 * Standalone — only compiles psx_region.c, doesn't pull in cpu.c or
 * the rest of openpete_core. */

#include "../../third_party/psxrecomp/runner/include/psx_runtime.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(cond, msg) do {                                          \
    if (!(cond)) {                                                     \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);  \
        return 1;                                                      \
    }                                                                  \
} while (0)

static void clear_table(void) {
    /* Walk the documented base addresses our test uses and unregister
     * any leftovers from earlier subtests. */
    psx_runtime_unregister_region(0x80300000u);
    psx_runtime_unregister_region(0x80400000u);
    psx_runtime_unregister_region(0x80500000u);
}

static int test_register_basic(void) {
    clear_table();
    static uint8_t buf[64 * 1024];
    memset(buf, 0xAA, sizeof(buf));

    int rc = psx_runtime_register_region(0x80300000u, sizeof(buf), buf);
    CHECK(rc == 0, "register at 0x80300000 should succeed");
    CHECK(psx_runtime_region_count() == 1u, "count == 1");

    void*    host = NULL;
    uint32_t rem  = 0;
    int hit = psx_runtime_resolve_region(0x80300100u, &host, &rem);
    CHECK(hit == 1, "resolve mid-region hits");
    CHECK(host == buf + 0x100, "host pointer offset correct");
    CHECK(rem == sizeof(buf) - 0x100, "remaining correct");

    /* KSEG1 mirror of same phys should resolve to the same region. */
    hit = psx_runtime_resolve_region(0xA0300100u, &host, NULL);
    CHECK(hit == 1, "KSEG1 mirror resolves");
    CHECK(host == buf + 0x100, "mirror host pointer correct");

    /* Outside-region addresses miss. */
    hit = psx_runtime_resolve_region(0x80000000u, NULL, NULL);
    CHECK(hit == 0, "canonical RAM addr does not resolve");
    hit = psx_runtime_resolve_region(0x80300000u + sizeof(buf), NULL, NULL);
    CHECK(hit == 0, "one byte past region end does not resolve");

    psx_runtime_unregister_region(0x80300000u);
    CHECK(psx_runtime_region_count() == 0u, "count == 0 after unregister");
    hit = psx_runtime_resolve_region(0x80300100u, NULL, NULL);
    CHECK(hit == 0, "after unregister, resolve misses");
    return 0;
}

static int test_overlap_rejected(void) {
    clear_table();
    static uint8_t buf_a[16 * 1024];
    static uint8_t buf_b[16 * 1024];

    /* Overlap with canonical 2 MB. */
    int rc = psx_runtime_register_region(0x80100000u, 16 * 1024, buf_a);
    CHECK(rc != 0, "overlap with canonical RAM rejected");

    /* Overlap with scratchpad. */
    rc = psx_runtime_register_region(0x9F800000u, 16 * 1024, buf_a);
    CHECK(rc != 0, "overlap with scratchpad rejected");

    /* First valid registration. */
    rc = psx_runtime_register_region(0x80300000u, 16 * 1024, buf_a);
    CHECK(rc == 0, "first region registers");

    /* Second registration overlapping the first. */
    rc = psx_runtime_register_region(0x80302000u, 16 * 1024, buf_b);
    CHECK(rc != 0, "second region overlapping first rejected");

    /* Adjacent (no overlap) succeeds. */
    rc = psx_runtime_register_region(0x80304000u, 16 * 1024, buf_b);
    CHECK(rc == 0, "adjacent region succeeds");
    CHECK(psx_runtime_region_count() == 2u, "count == 2");

    psx_runtime_unregister_region(0x80300000u);
    psx_runtime_unregister_region(0x80304000u);
    CHECK(psx_runtime_region_count() == 0u, "count == 0 after both unregistered");
    return 0;
}

static int test_resolve_with_null_outparams(void) {
    clear_table();
    static uint8_t buf[4096];
    int rc = psx_runtime_register_region(0x80400000u, sizeof(buf), buf);
    CHECK(rc == 0, "register succeeds");
    /* Both NULL — should still report hit/miss correctly. */
    int hit = psx_runtime_resolve_region(0x80400500u, NULL, NULL);
    CHECK(hit == 1, "resolve with NULL outparams reports hit");
    hit = psx_runtime_resolve_region(0x80500000u, NULL, NULL);
    CHECK(hit == 0, "resolve with NULL outparams reports miss");
    psx_runtime_unregister_region(0x80400000u);
    return 0;
}

static int test_canonicalize_release_path(void) {
    /* In release builds (no OPENPETE_DEBUG_REGION_GUARD), canonicalize
     * is a bare 0x1FFFFF mask regardless of registration state. */
    clear_table();
    uint32_t v = psx_runtime_canonicalize(0x80123456u);
    CHECK(v == 0x00123456u, "canonicalize masks KSEG0 phys");
    v = psx_runtime_canonicalize(0xA01F0000u);
    CHECK(v == 0x001F0000u, "canonicalize masks KSEG1 phys");
    return 0;
}

static int test_unregister_unknown_is_safe(void) {
    clear_table();
    /* Unregistering an address that was never registered is a no-op. */
    psx_runtime_unregister_region(0x80300000u);
    CHECK(psx_runtime_region_count() == 0u, "count stays 0");
    return 0;
}

int main(void) {
    int rc = 0;
    rc |= test_register_basic();
    rc |= test_overlap_rejected();
    rc |= test_resolve_with_null_outparams();
    rc |= test_canonicalize_release_path();
    rc |= test_unregister_unknown_is_safe();
    if (rc == 0) printf("psx_region_test: OK\n");
    return rc;
}
