/* psx_bios_kernel_seed.h - the SCPH-1001 kernel tables the boot leaves in
 * low RAM (psx_bios_kernel_seed.c), applied by openpete_psx_bios_seed_ram. */
#ifndef PSX_BIOS_KERNEL_SEED_H
#define PSX_BIOS_KERNEL_SEED_H

#include <stdint.h>

/* A run of consecutive words starting at a physical RAM address. */
typedef struct {
    uint32_t addr;
    uint32_t count;
    const uint32_t* words;
} psx_kernel_seed_run_t;

extern const psx_kernel_seed_run_t g_psx_kernel_seed[];
extern const unsigned g_psx_kernel_seed_count;

#endif
