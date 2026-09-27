/* tests/unit/psx_span_test.c — the span form of the guest-pointer resolver
 * and the width-checked bus accessors at the ends of main RAM and the
 * scratchpad.
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
        CHECK(openpete_psx_to_host_span(top, 1u) == NULL, "first byte past RAM unmapped");
        CHECK(openpete_psx_to_host_span(seg[i] + 0x100u, 0xFFFFFFF0u) == NULL, "huge length refused without wrap");
    }
    CHECK(openpete_psx_to_host_span(0x80000000u, OPENPETE_PSX_RAM_SIZE) == ram, "whole RAM as one span");
    CHECK(openpete_psx_to_host_span(0x80000000u, OPENPETE_PSX_RAM_SIZE + 1u) == NULL, "RAM plus one refused");
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

/** @brief Runs the span and accessor checks.
 * @return 0 when every check passes. */
int main(void) {
    test_ram_edges();
    test_scratch_edges();
    test_region_edges();
    test_bus_accessors();
    test_thunk_refusals();
    if (s_fail) { fprintf(stderr, "psx_span_test: %d FAILED\n", s_fail); return 1; }
    printf("psx_span_test: OK\n");
    return 0;
}
