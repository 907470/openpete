/* tests/unit/boot_memfill_test.c — the EXE header's memfill range is bounded
 * without a 32-bit sum that can wrap.
 *
 * Boots a crafted EXE whose memfill_start + memfill_size wraps below 2 MiB
 * (0x80000100 + 0xFFFFFF80); the boot must skip the fill and leave RAM
 * above the fill start untouched. A valid fill still zeroes its range. */
#include "openpete_psx.h"
#include "openpete_log.h"
#include "psx_runtime.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/** @brief Stub CD-ROM register read: unmapped. */
int openpete_cdrom_mmio_read8(uint32_t phys, uint8_t* out) { (void)phys; (void)out; return 0; }
/** @brief Stub CD-ROM register write: unmapped. */
int openpete_cdrom_mmio_write8(uint32_t phys, uint8_t value) { (void)phys; (void)value; return 0; }
/** @brief Stub BIOS ROM window: unmapped. */
int openpete_psx_bios_rom_read8(uint32_t phys, uint8_t* out) { (void)phys; (void)out; return 0; }
/** @brief Stub kernel low-RAM seed: nothing to seed here. */
void openpete_psx_bios_seed_ram(uint8_t* ram) { (void)ram; }
/** @brief Stub logger sink. */
void openpete_logf(openpete_log_level_t level, const char* subsystem, const char* fmt, ...) {
    (void)level; (void)subsystem; (void)fmt;
}

/** @brief Store a little-endian word into a header buffer. */
static void put32(uint8_t* b, size_t off, uint32_t v) { memcpy(b + off, &v, 4); }

/** @brief Builds a 2048-byte header plus a 4-byte body with the given memfill range. */
static void make_exe(uint8_t* exe, uint32_t fill_start, uint32_t fill_size) {
    memset(exe, 0, 2048 + 4);
    memcpy(exe, "PS-X EXE", 8);
    put32(exe, 0x10, 0x80010000u);      /* pc */
    put32(exe, 0x18, 0x80010000u);      /* load address */
    put32(exe, 0x1C, 4u);               /* file size */
    put32(exe, 0x28, fill_start);
    put32(exe, 0x2C, fill_size);
}

/** @brief Runs the wrapping and valid memfill cases.
 * @return 0 when both pass. */
int main(void) {
    static CPUState cpu;
    static uint8_t exe[2048 + 4];
    int fail = 0;
    openpete_psx_runtime_init(&cpu);
    uint8_t* ram = openpete_psx_get_ram();

    memset(ram + 0x100, 0x5A, 0x1000);
    make_exe(exe, 0x80000100u, 0xFFFFFF80u);
    openpete_psx_boot(&cpu, exe, sizeof exe);
    if (ram[0x100] != 0x5A || ram[0x10FF] != 0x5A) { fprintf(stderr, "FAIL wrapping fill ran\n"); fail++; }

    make_exe(exe, 0x80000100u, 0x100u);
    openpete_psx_boot(&cpu, exe, sizeof exe);
    if (ram[0x100] != 0 || ram[0x1FF] != 0 || ram[0x200] != 0x5A) { fprintf(stderr, "FAIL valid fill\n"); fail++; }

    make_exe(exe, 0x801FFF00u, 0x101u);
    memset(ram + 0x1FFF00, 0x5A, 0x100);
    openpete_psx_boot(&cpu, exe, sizeof exe);
    if (ram[0x1FFF00] != 0x5A) { fprintf(stderr, "FAIL fill past RAM end ran\n"); fail++; }
    if (fail) return 1;
    printf("boot_memfill_test: OK\n");
    return 0;
}
