/* tests/unit/psx_span_test.c - the span form of the guest-pointer resolver
 * and the width-checked bus accessors at the ends of main RAM and the
 * scratchpad, and the main-RAM mirror through physical 8 MB.
 *
 * Compiles cpu.c and psx_region.c with stubs for the CD-ROM register model
 * and the BIOS ROM window, so no SDL or game module is linked. */

#include "openpete_psx.h"
#include "openpete_log.h"
#include "psx_runtime.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/** @brief Stub CD-ROM register read: every register is unmapped here. */
int openpete_cdrom_mmio_read8(uint32_t phys, uint8_t* out) { (void)phys; (void)out; return 0; }
/** @brief Stub CD-ROM register write: every register is unmapped here. */
int openpete_cdrom_mmio_write8(uint32_t phys, uint8_t value) { (void)phys; (void)value; return 0; }
/** @brief Stub BIOS ROM window: no byte of it is mapped here. */
int openpete_psx_bios_rom_read8(uint32_t phys, uint8_t* out) { (void)phys; (void)out; return 0; }
/** @brief Stub logger sink. */
void openpete_logf(openpete_log_level_t level, const char* subsystem, const char* fmt, ...) { (void)level; (void)subsystem; (void)fmt; }

static int s_fail;
#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); s_fail++; } } while (0)

/** @brief Checks the span form at the main RAM boundary under each segment. */
static void test_ram_edges(void) {
    const uint32_t seg[3] = { 0x00000000u, 0x80000000u, 0xA0000000u };
    uint8_t* ram = openpete_psx_get_ram();
    for (int i = 0; i < 3; i++) {
        uint32_t top = seg[i] + OPENPETE_PSX_RAM_SIZE;
        CHECK(openpete_psx_to_host_span(top - 4u, 4u) == ram + OPENPETE_PSX_RAM_SIZE - 4u, "last word maps");
        CHECK(openpete_psx_to_host_span(top - 3u, 4u) == NULL, "word crossing the end refused");
        CHECK(openpete_psx_to_host_span(top - 1u, 1u) == ram + OPENPETE_PSX_RAM_SIZE - 1u, "last byte maps");
        CHECK(openpete_psx_to_host_span(top - 1u, 2u) == NULL, "half crossing the end refused");
        CHECK(openpete_psx_to_host_span(top - 0x58u, 0x58u) != NULL, "record ending at the top maps");
        CHECK(openpete_psx_to_host_span(top - 0x57u, 0x58u) == NULL, "record one past the top refused");
        CHECK(openpete_psx_to_host_span(top, 1u) == ram, "first byte past RAM is the first mirror of RAM offset 0");
        CHECK(openpete_psx_to_host_span(seg[i] + 0x100u, 0xFFFFFFF0u) == NULL, "huge length refused without wrap");
    }
    CHECK(openpete_psx_to_host_span(0x80000000u, OPENPETE_PSX_RAM_SIZE) == ram, "whole RAM as one span");
    CHECK(openpete_psx_to_host_span(0x80000000u, OPENPETE_PSX_RAM_SIZE + 1u) == NULL, "RAM plus one refused");
    CHECK(openpete_psx_to_host_span(0x80200000u, OPENPETE_PSX_RAM_SIZE) == ram, "a whole mirror copy as one span");
    CHECK(openpete_psx_to_host_span(0u, 4u) == NULL, "guest NULL stays NULL");
    CHECK(openpete_psx_to_host_ptr(0x801FFFFFu) == ram + OPENPETE_PSX_RAM_SIZE - 1u, "one-byte form unchanged");
}

/** @brief Checks the span form at the scratchpad boundary. */
static void test_scratch_edges(void) {
    uint8_t* sp = openpete_psx_get_scratch();
    const uint32_t base = 0x1F800000u;
    CHECK(openpete_psx_to_host_span(base, OPENPETE_PSX_SCRATCH_SIZE) == sp, "whole scratchpad maps");
    CHECK(openpete_psx_to_host_span(base, OPENPETE_PSX_SCRATCH_SIZE + 1u) == NULL, "scratchpad plus one refused");
    CHECK(openpete_psx_to_host_span(0x9F8003FCu, 4u) == sp + 0x3FCu, "last scratch word maps (KSEG0 mirror)");
    CHECK(openpete_psx_to_host_span(0x9F8003FEu, 4u) == NULL, "scratch word crossing the end refused");
    CHECK(openpete_psx_to_host_span(0x1F800400u, 1u) == NULL, "first byte past the scratchpad unmapped");
}

/** @brief Checks a presentation region: a span ending inside maps, one crossing its end does not. */
static void test_region_edges(void) {
    static uint8_t buf[256];
    CHECK(psx_runtime_register_region(0x80300000u, sizeof buf, buf) == 0, "region registers");
    CHECK(openpete_psx_to_host_span(0x80300000u + 252u, 4u) == buf + 252, "last region word maps");
    CHECK(openpete_psx_to_host_span(0x80300000u + 253u, 4u) == NULL, "word crossing the region end refused");
    psx_runtime_unregister_region(0x80300000u);
}

/** @brief Checks the bus accessors: an unaligned word or half at the top of RAM or the scratchpad touches no host byte past the array. */
static void test_bus_accessors(void) {
    static CPUState cpu;
    openpete_psx_runtime_init(&cpu);
    uint8_t* ram = openpete_psx_get_ram();
    uint8_t* sp = openpete_psx_get_scratch();
    memset(ram + OPENPETE_PSX_RAM_SIZE - 4u, 0xAB, 4);
    memset(sp + OPENPETE_PSX_SCRATCH_SIZE - 4u, 0xCD, 4);
    CHECK(cpu.read_word(0x801FFFFCu) == 0xABABABABu, "aligned last RAM word reads");
    CHECK(cpu.read_word(0x801FFFFDu) == 0u, "unaligned word crossing RAM end reads 0");
    CHECK(cpu.read_half(0x801FFFFFu) == 0u, "half crossing RAM end reads 0");
    cpu.write_word(0x801FFFFDu, 0x11223344u);
    CHECK(memcmp(ram + OPENPETE_PSX_RAM_SIZE - 4u, "\xAB\xAB\xAB\xAB", 4) == 0, "crossing word write dropped");
    cpu.write_half(0x801FFFFFu, 0x5566u);
    CHECK(ram[OPENPETE_PSX_RAM_SIZE - 1u] == 0xABu, "crossing half write dropped");
    CHECK(cpu.read_word(0x1F8003FCu) == 0xCDCDCDCDu, "aligned last scratch word reads");
    CHECK(cpu.read_word(0x1F8003FEu) == 0u, "word crossing scratch end reads 0");
    cpu.write_word(0x1F8003FEu, 0x11223344u);
    CHECK(sp[OPENPETE_PSX_SCRATCH_SIZE - 1u] == 0xCDu, "crossing scratch write dropped");
    CHECK(cpu.read_byte(0x801FFFFFu) == 0xABu, "last RAM byte reads");
}

/** @brief Checks the refusal classes a PSYQ thunk argument meets: a struct past the RAM end, a word copy past it, and a string with no NUL before its region's end. */
static void test_thunk_refusals(void) {
    uint8_t* ram = openpete_psx_get_ram();
    uint8_t* sp = openpete_psx_get_scratch();
    /* A struct argument (a 20-byte DISPENV) whose last byte passes the top of RAM. */
    CHECK(openpete_psx_to_host_span(0x801FFFECu, 20u) == ram + OPENPETE_PSX_RAM_SIZE - 20u, "struct ending at the top maps");
    CHECK(openpete_psx_to_host_span(0x801FFFEDu, 20u) == NULL, "struct one byte past the top refused");
    CHECK(openpete_psx_to_host_ptr(0x801FFFEDu) != NULL, "one-byte form still maps the struct's first byte");
    /* A word copy of n words (CdGetSector, ClearImage's fill packet). */
    const uint32_t words = 13u;
    CHECK(openpete_psx_to_host_span(0x80200000u - 4u * words, 4u * words) != NULL, "word copy ending at the top maps");
    CHECK(openpete_psx_to_host_span(0x80200000u - 4u * words + 4u, 4u * words) == NULL, "word copy one word past the top refused");
    CHECK(openpete_psx_to_host_span(0x80100000u, 0x40001u * 4u) == NULL, "word count one word past RAM refused");
    /* Strings: the NUL scan stops at the region's end. */
    uint32_t n = 0xFFu;
    memcpy(ram + OPENPETE_PSX_RAM_SIZE - 4u, "ab\0z", 4);
    CHECK(openpete_psx_to_host_str(0x801FFFFCu, 0xFFFFFFFFu, &n) == (const char*)ram + OPENPETE_PSX_RAM_SIZE - 4u && n == 2u, "terminated string at the top maps with its length");
    memcpy(ram + OPENPETE_PSX_RAM_SIZE - 4u, "abcd", 4);
    CHECK(openpete_psx_to_host_str(0x801FFFFCu, 0xFFFFFFFFu, &n) == NULL, "string with no NUL before RAM end refused");
    CHECK(openpete_psx_to_host_str(0x801FFFFCu, 4u, &n) != NULL && n == 4u, "bounded read of the last four bytes maps at the bound");
    CHECK(openpete_psx_to_host_str(0x801FFFFCu, 5u, &n) == NULL, "bounded read past RAM end with no NUL refused");
    CHECK(openpete_psx_to_host_str(0x801FFFFCu, 0u, &n) != NULL && n == 0u, "zero-byte bounded read maps");
    memset(sp + OPENPETE_PSX_SCRATCH_SIZE - 8u, 'x', 8);
    CHECK(openpete_psx_to_host_str(0x1F8003F8u, 0xFFFFFFFFu, NULL) == NULL, "string with no NUL before scratchpad end refused");
    static uint8_t buf[16];
    memset(buf, 'y', sizeof buf);
    CHECK(psx_runtime_register_region(0x80300000u, sizeof buf, buf) == 0, "region registers");
    CHECK(openpete_psx_to_host_str(0x80300008u, 0xFFFFFFFFu, NULL) == NULL, "string with no NUL before region end refused");
    buf[15] = 0;
    CHECK(openpete_psx_to_host_str(0x80300008u, 0xFFFFFFFFu, &n) == (const char*)buf + 8 && n == 7u, "string ending at the region's last byte maps");
    psx_runtime_unregister_region(0x80300000u);
    CHECK(openpete_psx_to_host_str(0u, 0xFFFFFFFFu, NULL) == NULL, "guest NULL string stays NULL");
}

/* Physical bases of the three 2 MB copies above main RAM, and the segments. */
static const uint32_t k_mirror[3] = { 0x00200000u, 0x00400000u, 0x00600000u };
static const uint32_t k_seg[3] = { 0x00000000u, 0x80000000u, 0xA0000000u };

/** @brief Checks that a store through each mirror copy under each segment, at every width, lands on the main-RAM byte and reads back through the base address and every other alias. */
static void test_mirror_widths(void) {
    static CPUState cpu;
    openpete_psx_runtime_init(&cpu);
    uint8_t* ram = openpete_psx_get_ram();
    const uint32_t off = 0x1659E4u;
    uint32_t v = 0x10203040u;
    for (int m = 0; m < 3; m++) {
        for (int g = 0; g < 3; g++) {
            const uint32_t a = k_seg[g] | (k_mirror[m] + off);
            v += 0x01010101u;
            cpu.write_word(a, v);
            uint32_t got;
            memcpy(&got, ram + off, 4);
            CHECK(got == v, "mirror word store lands on the main-RAM word");
            CHECK(cpu.read_word(0x80000000u | off) == v, "base address reads the mirror store");
            for (int m2 = 0; m2 < 3; m2++)
                for (int g2 = 0; g2 < 3; g2++)
                    CHECK(cpu.read_word(k_seg[g2] | (k_mirror[m2] + off)) == v, "every mirror alias reads the word");
            cpu.write_half(a + 2u, (uint16_t)(v >> 7));
            CHECK(cpu.read_half(0xA0000000u | (off + 2u)) == (uint16_t)(v >> 7), "mirror half store reads back through KSEG1 base");
            CHECK(cpu.read_half(k_seg[2 - g] | (k_mirror[2 - m] + off + 2u)) == (uint16_t)(v >> 7), "mirror half reads through another copy");
            cpu.write_byte(a + 1u, (uint8_t)m);
            CHECK(ram[off + 1u] == (uint8_t)m, "mirror byte store lands on the main-RAM byte");
            CHECK(cpu.read_byte(k_seg[(g + 1) % 3] | (k_mirror[(m + 1) % 3] + off + 1u)) == (uint8_t)m, "mirror byte reads through another copy");
            cpu.swl(a + 1u, 0xAABBCCDDu);
            cpu.swr(a + 1u, 0xAABBCCDDu);
            CHECK(cpu.lwr(0x80000000u | (off + 1u), 0u) == cpu.lwr(a + 1u, 0u), "unaligned pair agrees between base and mirror");
        }
    }
    /* Host lookups agree with the bus. */
    CHECK(openpete_psx_to_host_ptr(0x803659E4u) == ram + off, "host pointer of a mirror address is the main-RAM byte");
    CHECK(openpete_psx_to_host_span(0x007FFFFCu, 4u) == ram + OPENPETE_PSX_RAM_SIZE - 4u, "last word of the last copy maps");
    uint32_t n = 0;
    memcpy(ram + 0x100u, "mir\0", 4);
    CHECK(openpete_psx_to_host_str(0x80600100u, 0xFFFFFFFFu, &n) == (const char*)ram + 0x100u && n == 3u, "string through a mirror resolves");
}

/** @brief Checks that a span or access crossing a 2 MB copy boundary is refused, as one crossing the top of main RAM is. */
static void test_mirror_straddle(void) {
    static CPUState cpu;
    openpete_psx_runtime_init(&cpu);
    uint8_t* ram = openpete_psx_get_ram();
    memset(ram, 0x5A, 8);
    memset(ram + OPENPETE_PSX_RAM_SIZE - 8u, 0xA5, 8);
    for (int m = 0; m < 3; m++) {
        const uint32_t end = 0x80000000u | (k_mirror[m] + OPENPETE_PSX_RAM_SIZE);
        CHECK(openpete_psx_to_host_span(end - 2u, 4u) == NULL, "span crossing a copy boundary refused");
        CHECK(cpu.read_word(end - 2u) == 0u, "word crossing a copy boundary reads 0");
        cpu.write_word(end - 2u, 0x11111111u);
        CHECK(ram[0] == 0x5Au && ram[OPENPETE_PSX_RAM_SIZE - 1u] == 0xA5u, "word crossing a copy boundary dropped");
    }
    CHECK(openpete_psx_to_host_span(0x007FFFFEu, 4u) == NULL, "span crossing physical 8 MB refused");
}

/** @brief Checks that a registered region inside the mirror window takes its addresses, and the rest of the window still mirrors. */
static void test_mirror_region_priority(void) {
    static CPUState cpu;
    static uint8_t buf[4096];
    openpete_psx_runtime_init(&cpu);
    uint8_t* ram = openpete_psx_get_ram();
    memset(buf, 0, sizeof buf);
    CHECK(psx_runtime_register_region(0x80700000u, sizeof buf, buf) == 0, "region in the mirror window registers");
    cpu.write_word(0x80700010u, 0xCAFEF00Du);
    uint32_t got;
    memcpy(&got, buf + 0x10, 4);
    CHECK(got == 0xCAFEF00Du, "store inside the region lands in the region");
    memcpy(&got, ram + 0x100010u, 4);
    CHECK(got != 0xCAFEF00Du, "store inside the region does not reach RAM");
    CHECK(openpete_psx_to_host_ptr(0xA0700010u) == buf + 0x10, "host lookup of a region address under KSEG1 is the region");
    CHECK(openpete_psx_to_host_span(0x80700000u + sizeof buf - 2u, 4u) == NULL, "span past the region's end refused, not mirrored");
    CHECK(openpete_psx_to_host_ptr(0x80700000u + sizeof buf) == ram + 0x100000u + sizeof buf, "first byte past the region mirrors RAM");
    psx_runtime_unregister_region(0x80700000u);
    CHECK(openpete_psx_to_host_ptr(0x80700010u) == ram + 0x100010u, "after unregistering, the address mirrors RAM again");
}

/** @brief Checks that physical 8 MB and above behave as unmapped: loads read 0, stores are dropped, host lookups refuse, and every bus access is counted once with the first ones kept. */
static void test_above_mirror(void) {
    static CPUState cpu;
    openpete_psx_runtime_init(&cpu);
    uint8_t* ram = openpete_psx_get_ram();
    memset(ram, 0x77, 16);
    uint32_t kept = 0;
    const uint64_t n0 = openpete_psx_unmapped_accesses(NULL, 0u, NULL);
    const uint32_t a[4] = { 0x00800000u, 0x80800004u, 0xA0800008u, 0x81000000u };
    for (int i = 0; i < 4; i++) {
        CHECK(cpu.read_word(a[i]) == 0u, "word at or above 8 MB reads 0");
        CHECK(cpu.read_half(a[i]) == 0u, "half at or above 8 MB reads 0");
        CHECK(cpu.read_byte(a[i]) == 0u, "byte at or above 8 MB reads 0");
        cpu.write_word(a[i], 0x12345678u);
        cpu.write_half(a[i], 0x1234u);
        cpu.write_byte(a[i], 0x12u);
        CHECK(openpete_psx_to_host_ptr(a[i]) == NULL, "host lookup at or above 8 MB refused");
    }
    CHECK(memcmp(ram, "\x77\x77\x77\x77\x77\x77\x77\x77\x77\x77\x77\x77\x77\x77\x77\x77", 16) == 0, "no store above 8 MB reaches RAM");
    openpete_psx_unmapped_t first[8];
    const uint64_t n1 = openpete_psx_unmapped_accesses(first, 8u, &kept);
    CHECK(n1 - n0 == 24u, "every bus access at or above 8 MB counted once");
    CHECK(kept == 8u && first[0].addr == 0x00800000u && first[0].width == 4u && first[0].write == 0u,
          "first unmapped access kept with its width and direction");
    const uint64_t n2 = openpete_psx_unmapped_accesses(NULL, 0u, NULL);
    (void)cpu.read_word(0x80200000u);
    (void)cpu.read_word(0x1F801814u);
    CHECK(openpete_psx_unmapped_accesses(NULL, 0u, NULL) == n2, "mirror and device addresses are not unmapped accesses");
}

/** @brief Runs the span and accessor checks.
 * @return 0 when every check passes. */
int main(void) {
    test_ram_edges();
    test_scratch_edges();
    test_region_edges();
    test_bus_accessors();
    test_thunk_refusals();
    test_mirror_widths();
    test_mirror_straddle();
    test_mirror_region_priority();
    test_above_mirror();
    if (s_fail) { fprintf(stderr, "psx_span_test: %d FAILED\n", s_fail); return 1; }
    printf("psx_span_test: OK\n");
    return 0;
}
