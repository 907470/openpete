/* tests/unit/mod_arena_test.c - the guest arena's two pools
 * (core/mods/mod_arena.c): pool selection by OP_GALLOC_NO_DMA, the
 * DMA-reachable pool's upper-then-lower segment order (a small block lands
 * upper after a larger one went lower), per-segment exhaustion naming every
 * owner, a version-3 savestate section mapped onto the upper segment at the
 * vaddrs it was saved from, the version-4 round trip and in-memory
 * snapshot over the three ranges, KSEG2 and high-KUSEG refusal on region
 * resolution, and a bus read one byte past a segment's committed extent
 * answered by the ROM model; the rewind page tables over the three
 * ranges (core/record/rewind_pages.c): copy, compare-and-share against a
 * target, restore from an encoded and from a partly compared table, a
 * compare discarded when its target is released, and the store's release.
 *
 * Compiles mod_arena.c with the region table, the bus accessors, the
 * stream-handle table and the flat vector; the logger, the CD-ROM register
 * model and the BIOS ROM window are local stubs. The arena is process
 * state, so the checks run in one order. */

#include "openpete_psx.h"
#include "openpete_log.h"
#include "openpete_mods.h"
#include "openpete_mod_api.h"
#include "openpete_rewind_pages.h"
#include "psx_runtime.h"
#include "psx_runtime_internal.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned s_rom_reads;
/** @brief Stub CD-ROM register read: every register is unmapped here. */
int openpete_cdrom_mmio_read8(uint32_t phys, uint8_t* out) { (void)phys; (void)out; return 0; }
/** @brief Stub CD-ROM register write: every register is unmapped here. */
int openpete_cdrom_mmio_write8(uint32_t phys, uint8_t value) { (void)phys; (void)value; return 0; }
/** @brief Stub BIOS ROM window: every byte reads 0xAB and is counted, so a read the ROM model answers is visible. */
int openpete_psx_bios_rom_read8(uint32_t phys, uint8_t* out) {
    (void)phys;
    s_rom_reads++;
    *out = 0xABu;
    return 1;
}

#define LOG_LINES 64
static char s_log[LOG_LINES][512];
static unsigned s_log_n;
/** @brief Logger stub: keeps the last LOG_LINES lines for log_has. */
void openpete_logf(openpete_log_level_t level, const char* subsystem, const char* fmt, ...) {
    (void)level; (void)subsystem;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_log[s_log_n % LOG_LINES], sizeof s_log[0], fmt, ap);
    va_end(ap);
    s_log_n++;
}
/** @brief Report whether a logged line since the last log_clear contains @p needle. */
static int log_has(const char* needle) {
    const unsigned n = s_log_n < LOG_LINES ? s_log_n : LOG_LINES;
    for (unsigned i = 0; i < n; i++)
        if (strstr(s_log[i], needle)) return 1;
    return 0;
}
/** @brief Forget the logged lines. */
static void log_clear(void) { s_log_n = 0; }

static int s_fail;
#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); s_fail++; } } while (0)

#define MIB (1024u * 1024u)
static CPUState s_cpu;

/** @brief Allocate through the arena and return the vaddr, checking the host view against the bus. */
static uint32_t alloc(const char* owner, uint32_t size, uint32_t align, uint32_t flags) {
    void* host = NULL;
    const uint32_t va = openpete_mods_guest_alloc(size, align, flags, &host, owner);
    if (va) CHECK(openpete_psx_to_host_ptr(va) == host, "host view is the bus's host byte");
    return va;
}

/** @brief Checks that resolution refuses KSEG2 and KUSEG at or above 0x20000000 and accepts KUSEG, KSEG0 and KSEG1. */
static void test_segments_resolve(void) {
    CHECK(psx_runtime_resolve_region(0x00700000u, NULL, NULL) == 1, "KUSEG alias resolves");
    CHECK(psx_runtime_resolve_region(0x80700000u, NULL, NULL) == 1, "KSEG0 resolves");
    CHECK(psx_runtime_resolve_region(0xA0700000u, NULL, NULL) == 1, "KSEG1 resolves");
    CHECK(psx_runtime_resolve_region(0xC0700000u, NULL, NULL) == 0, "KSEG2 refused");
    CHECK(psx_runtime_resolve_region(0xE0700000u, NULL, NULL) == 0, "KSEG2 (upper half) refused");
    CHECK(psx_runtime_resolve_region(0x20700000u, NULL, NULL) == 0, "KUSEG at 0x20000000 and up refused");
    CHECK(psx_runtime_resolve_region(0xF1000000u, NULL, NULL) == 0, "a BIOS event handle never reaches the general pool");
    CHECK(s_cpu.read_word(0xC0700000u) == 0xABABABABu, "a KSEG2 read is answered by the ROM model");
}

/** @brief Checks pool selection, the segment order and per-segment exhaustion. */
static void test_pools(void) {
    CHECK(s_cpu.read_word(0x80700000u) == 0xABABABABu, "before any block the upper segment is unmapped");
    const uint32_t a0 = alloc("a", 64, 0, 0);
    CHECK(a0 == 0x80700000u, "first DMA block at the former arena base");
    CHECK(log_has("guest arena live @ 0x80700000"), "upper segment registration logged");
    const uint32_t g0 = alloc("a", 4096, 16, OP_GALLOC_NO_DMA);
    CHECK(g0 == 0x81000000u, "NO_DMA block at the general pool base");
    const uint32_t g1 = alloc("a", 100, 0, OP_GALLOC_NO_DMA | OP_GALLOC_IMMUTABLE);
    CHECK(g1 == 0x81001000u, "NO_DMA|IMMUTABLE block in the general pool");
    CHECK(alloc("a", 16, 0, 0) == 0x80700040u, "a later DMA block follows in the upper segment");
    CHECK(openpete_mods_guest_alloc(9u * MIB + 1u, 0, 0, NULL, "a") == 0, "DMA per-call limit is 9 MiB");

    const uint32_t b0 = alloc("b", 4u * MIB, 0, 0);
    CHECK(b0 == 0x80700050u, "4 MiB fits upper");
    const uint32_t b1 = alloc("b", 5u * MIB, 0, 0);
    CHECK(b1 == 0x80200000u, "5 MiB does not fit upper and goes lower");
    CHECK(log_has("lower segment live @ 0x80200000"), "lower segment registration logged");
    const uint32_t b2 = alloc("b", 1024, 0, 0);
    CHECK(b2 == b0 + 4u * MIB, "a small block lands upper after a larger one went lower");

    log_clear();
    CHECK(openpete_mods_guest_alloc(5u * MIB, 0, 0, NULL, "c") == 0, "neither segment fits: refused");
    CHECK(log_has("guest arena exhausted: c wanted 5242880 B"), "exhaustion names the caller");
    CHECK(log_has("DMA-reachable pool held by a: 80 B"), "exhaustion lists owner a");
    CHECK(log_has("DMA-reachable pool held by b: 9438208 B"), "exhaustion lists owner b");
    CHECK(!log_has("pool held by c"), "the refused caller holds nothing");
    log_clear();
    CHECK(openpete_mods_guest_alloc(480u * MIB, 0, OP_GALLOC_NO_DMA, NULL, "c") == 0,
          "general pool: a block past its free bytes is refused");
    CHECK(log_has("in the general pool") && log_has("general pool held by a: 4196 B"),
          "general exhaustion lists the general pool's owners");
    CHECK(openpete_mods_guest_alloc(480u * MIB + 1u, 0, OP_GALLOC_NO_DMA, NULL, "c") == 0,
          "general per-call limit is 480 MiB");

    openpete_mods_arena_use_t u;
    CHECK(openpete_mods_arena_owner_use("a", &u) == 1 && u.dma_bytes == 80u &&
          u.general_bytes == 4196u && u.general_immutable == 100u && u.dma_immutable == 0u,
          "owner a's use per pool");
    CHECK(openpete_mods_arena_owner_use("c", &u) == 0, "owner c holds nothing");

    uint32_t used[OPENPETE_ARENA_RANGES];
    const uint32_t total = openpete_mods_arena_used(used);
    CHECK(used[OPENPETE_ARENA_RANGE_UPPER] == 0x50u + 4u * MIB + 1024u, "upper cursor");
    CHECK(used[OPENPETE_ARENA_RANGE_LOWER] == 5u * MIB, "lower cursor");
    CHECK(used[OPENPETE_ARENA_RANGE_GENERAL] == 0x1064u, "general cursor");
    CHECK(total == used[0] + used[1] + used[2], "total is the sum");
}

/** @brief Checks that a bus read past a segment's committed extent is answered by the ROM model and one inside is not. */
static void test_committed_extent(void) {
    void* host = NULL;
    uint32_t rem = 0;
    CHECK(psx_runtime_resolve_region(0x81000000u, &host, &rem) == 1, "general pool resolves");
    CHECK(rem == 0x10000u, "general pool registered at one 64 KiB granule");
    const unsigned before = s_rom_reads;
    CHECK(s_cpu.read_byte(0x81000000u + rem - 1u) == 0u && s_rom_reads == before,
          "last committed byte is mapped");
    CHECK(s_cpu.read_byte(0x81000000u + rem) == 0xABu && s_rom_reads == before + 1u,
          "one byte past the committed extent is answered by rom_read");
    CHECK(psx_runtime_resolve_region(0x81000000u + rem, NULL, NULL) == 0, "past the extent resolves to no region");
}

/** @brief Checks the in-memory snapshot copy and re-anchor restore over the three ranges. */
static void test_mem_snapshot(void) {
    uint32_t used[OPENPETE_ARENA_RANGES];
    const uint32_t total = openpete_mods_arena_used(used);
    uint8_t* buf = malloc(total);
    CHECK(buf != NULL, "snapshot buffer");
    if (!buf) return;
    s_cpu.write_word(0x80700000u, 0x11111111u);
    s_cpu.write_word(0x80200000u, 0x22222222u);
    s_cpu.write_word(0x81000000u, 0x33333333u);
    uint32_t saved[OPENPETE_ARENA_RANGES];
    openpete_mods_arena_mem_save(buf, saved);
    CHECK(memcmp(saved, used, sizeof saved) == 0, "save reports the used ranges");
    s_cpu.write_word(0x80700000u, 0u);
    s_cpu.write_word(0x80200000u, 0u);
    s_cpu.write_word(0x81000000u, 0u);
    openpete_mods_arena_mem_restore(buf, saved);
    CHECK(s_cpu.read_word(0x80700000u) == 0x11111111u, "upper range restored");
    CHECK(s_cpu.read_word(0x80200000u) == 0x22222222u, "lower range restored");
    CHECK(s_cpu.read_word(0x81000000u) == 0x33333333u, "general range restored");
    free(buf);
}

/** @brief Checks the version-4 section round trip and the version-3 reader's mapping onto the upper segment. */
static void test_sections(void) {
    const uint32_t sz = openpete_mods_arena_state_size();
    uint8_t* v4 = malloc(sz);
    CHECK(v4 != NULL, "section buffer");
    if (!v4) return;
    openpete_mods_arena_state_save(v4);
    uint32_t hdr[5];
    memcpy(hdr, v4, sizeof hdr);
    CHECK(hdr[0] == 4u, "section version 4");
    CHECK(openpete_mods_arena_state_verdict(v4, sz) == 0, "own v4 section passes the verdict");

    /* A version-3 section as the single 8 MiB arena wrote it: owner a's
     * first call, 64 B at arena offset 0, bytes holding a pattern. */
    struct {
        uint32_t ver, count, used;
        char owner[OPENPETE_ARENA_OWNER_MAX];
        uint32_t kind, call_idx, size, align, flags, off, superseded;
        uint8_t bytes[64];
    } v3;
    memset(&v3, 0, sizeof v3);
    v3.ver = 3u; v3.count = 1u; v3.used = 64u;
    snprintf(v3.owner, sizeof v3.owner, "a");
    v3.size = 64u; v3.align = 8u;
    for (int i = 0; i < 64; i++) v3.bytes[i] = (uint8_t)(0x40 + i);
    CHECK(openpete_mods_arena_state_verdict(&v3, sizeof v3) == 0, "v3 row maps onto the live upper row");
    log_clear();
    openpete_mods_arena_state_apply(&v3, sizeof v3);
    CHECK(s_cpu.read_byte(0x80700000u) == 0x40u && s_cpu.read_byte(0x8070003Fu) == 0x7Fu,
          "v3 bytes land at the vaddrs they were saved from");
    CHECK(log_has("section v3"), "apply names the section version");
    v3.off = 8u;
    CHECK(openpete_mods_arena_state_verdict(&v3, sizeof v3) == -1, "v3 row at another offset refused");
    v3.off = 0u; v3.flags = OP_GALLOC_NO_DMA;
    CHECK(openpete_mods_arena_state_verdict(&v3, sizeof v3) == -1, "v3 row carrying NO_DMA is malformed");
    v3.flags = 0u; v3.size = 128u;
    CHECK(openpete_mods_arena_state_verdict(&v3, sizeof v3) == -1, "v3 row past its saved bytes is malformed");

    /* The v4 section restores every range. */
    s_cpu.write_word(0x80200000u, 0u);
    s_cpu.write_word(0x81000000u, 0u);
    openpete_mods_arena_state_apply(v4, sz);
    CHECK(s_cpu.read_word(0x80200000u) == 0x22222222u, "v4 apply restores the lower range");
    CHECK(s_cpu.read_word(0x81000000u) == 0x33333333u, "v4 apply restores the general range");
    hdr[0] = 5u;
    memcpy(v4, hdr, sizeof hdr);
    CHECK(openpete_mods_arena_state_verdict(v4, sz) == -1, "unknown section version refused");
    free(v4);
}

/** @brief Fill @p n bytes at guest @p va with the word sequence seed, seed+1, ... */
static void fill_words(uint32_t va, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i + 4u <= n; i += 4u) s_cpu.write_word(va + i, seed + i);
}

/** @brief Checks the rewind page tables: capture, compare-and-share, restore (encoded and pending), discard on target release, store release. */
static void test_rewind_pages(void) {
    uint32_t used[OPENPETE_ARENA_RANGES];
    (void)openpete_mods_arena_used(used);
    /* Distinct page contents, so only the same page of another table can match. */
    fill_words(0x80700000u, used[OPENPETE_ARENA_RANGE_UPPER], 0x10000000u);
    fill_words(0x80200000u, used[OPENPETE_ARENA_RANGE_LOWER], 0x20000000u);
    fill_words(0x81000000u, used[OPENPETE_ARENA_RANGE_GENERAL], 0x30000000u);
    rwp_table_t a, b, c;
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b); memset(&c, 0, sizeof c);
    CHECK(rwp_capture(&a) == 0, "capture a");
    const uint32_t np = a.npages;
    CHECK(np > 4u, "the ranges span several pages");
    rwp_compare_begin(&a, NULL);
    rwp_stats_t st = rwp_stats();
    CHECK(st.compares_no_target == 1u && rwp_pending() == NULL, "no target: encoded at once");
    CHECK(st.pages_live == np, "one store page per table page");

    s_cpu.write_word(0x80200000u + 4096u, 0xDEADBEEFu);   /* one lower page changes */
    CHECK(rwp_capture(&b) == 0, "capture b");
    rwp_compare_begin(&b, &a);
    CHECK(rwp_compare_remaining() == np, "compare covers every page");
    CHECK(rwp_compare_step(3u) == 3u && rwp_compare_remaining() == np - 3u, "slice of three pages");
    while (rwp_compare_remaining()) (void)rwp_compare_step(5u);
    st = rwp_stats();
    CHECK(st.pages_shared == np - 1u, "every unchanged page shared");
    CHECK(st.pages_live == np + 1u, "store holds a's pages plus b's changed page");
    CHECK(b.flat == NULL && b.nruns <= 4u, "b encodes as a few runs");

    s_cpu.write_word(0x80700000u, 0u);
    s_cpu.write_word(0x80200000u + 4096u, 0u);
    s_cpu.write_word(0x81000000u, 0u);
    rwp_restore(&a);
    CHECK(s_cpu.read_word(0x80700000u) == 0x10000000u, "a restores the upper range");
    CHECK(s_cpu.read_word(0x80200000u + 4096u) == 0x20000000u + 4096u, "a restores its own lower page");
    CHECK(s_cpu.read_word(0x81000000u) == 0x30000000u, "a restores the general range");
    rwp_restore(&b);
    CHECK(s_cpu.read_word(0x80200000u + 4096u) == 0xDEADBEEFu, "b restores its changed page");
    CHECK(s_cpu.read_word(0x81000000u) == 0x30000000u, "b restores a shared page");

    s_cpu.write_word(0x81000000u, 0x77777777u);
    CHECK(rwp_capture(&c) == 0, "capture c");
    rwp_compare_begin(&c, &b);
    (void)rwp_compare_step(1u);
    s_cpu.write_word(0x81000000u, 0u);
    rwp_restore(&c);
    st = rwp_stats();
    CHECK(st.restores_pending == 1u, "restore of a partly compared table counted");
    CHECK(s_cpu.read_word(0x81000000u) == 0x77777777u, "partly compared c restores its bytes");
    rwp_release(&b);
    st = rwp_stats();
    CHECK(st.discards == 1u && rwp_pending() == NULL, "releasing the target discards the compare");
    rwp_restore(&c);
    CHECK(s_cpu.read_word(0x80200000u + 4096u) == 0xDEADBEEFu, "c keeps a page it shared with released b");
    CHECK(rwp_store_trim() == 0u, "a referenced store is not released");
    rwp_release(&a);
    rwp_release(&c);
    st = rwp_stats();
    CHECK(st.pages_live == 0u, "every page returned");
    CHECK(rwp_store_trim() > 0u && rwp_stats().slabs == 0u, "an empty store unmaps its slabs");
    rwp_table_free(&a); rwp_table_free(&b); rwp_table_free(&c);
}

/** @brief Runs the arena checks in order.
 * @return 0 when every check passes. */
int main(void) {
    openpete_psx_runtime_init(&s_cpu);
    test_pools();
    test_segments_resolve();
    test_committed_extent();
    test_mem_snapshot();
    test_sections();
    test_rewind_pages();
    if (s_fail) { fprintf(stderr, "mod_arena_test: %d FAILED\n", s_fail); return 1; }
    printf("mod_arena_test: OK\n");
    return 0;
}
