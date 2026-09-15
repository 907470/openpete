// core/runtime/boot.c — PSX EXE boot sequence.
//
// See notes/plans/openpete-pc-port.md for the spec. Real hardware
// runs a small CRT from BIOS that copies .text/.data from the disc-loaded
// EXE into 2MB RAM, zeroes BSS, sets sp/gp/pc, then transfers control.
// We do exactly the same here against g_ram.

#include "openpete_psx.h"
#include "openpete_log.h"
#include "openpete_memcard.h"

#include <stdint.h>
#include <string.h>

/* PSX-EXE header is 2048 bytes. Layout matches psxrecomp's PS1ExeHeader
 * (vendored at third_party/psxrecomp/recompiler/include/ps1_exe_parser.h)
 * but we redefine it here so core/ doesn't depend on the recompiler's
 * C++ parser at link time. */
#pragma pack(push, 1)
typedef struct {
    char     magic[8];          /* "PS-X EXE" */
    uint32_t pad0;
    uint32_t pad1;
    uint32_t initial_pc;
    uint32_t initial_gp;
    uint32_t load_address;
    uint32_t file_size;
    uint32_t unknown0;
    uint32_t unknown1;
    uint32_t memfill_start;
    uint32_t memfill_size;
    uint32_t initial_sp_base;
    uint32_t initial_sp_offset;
    /* remainder is reserved/marker; we don't care */
} psx_exe_header_t;
#pragma pack(pop)

#define PSX_EXE_HEADER_SIZE 2048u

int openpete_psx_boot(CPUState* cpu, const uint8_t* bytes, size_t len) {
    if (!cpu || !bytes) {
        OPL_ERROR_F("boot", "openpete_psx_boot: NULL arg (cpu=%p bytes=%p)",
                    (void*)cpu, (const void*)bytes);
        return 1;
    }
    if (len < PSX_EXE_HEADER_SIZE) {
        OPL_ERROR_F("boot", "PSX EXE too small: %zu < %u", len, PSX_EXE_HEADER_SIZE);
        return 1;
    }

    psx_exe_header_t h;
    memcpy(&h, bytes, sizeof(h));
    if (memcmp(h.magic, "PS-X EXE", 8) != 0) {
        OPL_ERROR_F("boot", "Bad magic: not a PSX EXE");
        return 1;
    }
    if ((size_t)h.file_size + PSX_EXE_HEADER_SIZE > len) {
        OPL_ERROR_F("boot",
                    "PSX EXE truncated: header file_size=%u + 2048 > buf %zu",
                    h.file_size, len);
        return 1;
    }

    OPL_INFO_F("boot",
               "PSX EXE: pc=0x%08X gp=0x%08X load=0x%08X size=0x%X bss=[0x%08X+0x%X] sp=0x%08X+0x%X",
               h.initial_pc, h.initial_gp, h.load_address, h.file_size,
               h.memfill_start, h.memfill_size,
               h.initial_sp_base, h.initial_sp_offset);

    /* 0) The kernel's fixed words in low RAM, present under every EXE. */
    openpete_psx_bios_seed_ram(openpete_psx_get_ram());

    /* 1) Copy .text + .data from the EXE body into g_ram at load_address. */
    openpete_psx_load(h.load_address, bytes + PSX_EXE_HEADER_SIZE, h.file_size);

    /* 2) Zero memfill (BSS) if specified. Many PSYQ EXEs leave this 0/0
     *    because their CRT (`__main`) zeroes BSS dynamically. We do whatever
     *    the header asks; the runtime CRT will handle the rest. */
    if (h.memfill_size > 0) {
        uint8_t* ram = openpete_psx_get_ram();
        uint32_t phys = h.memfill_start & 0x1FFFFFFFu;
        if (phys + h.memfill_size <= 2u * 1024u * 1024u) {
            memset(ram + phys, 0, h.memfill_size);
        } else {
            OPL_WARN_F("boot",
                       "memfill range out of bounds: 0x%08X+0x%X — skipping",
                       h.memfill_start, h.memfill_size);
        }
    }

    /* 3) Seed registers. SP is base+offset (typically 0x801FFFF0+0). When
     *    base is 0 (common for PSYQ-built EXEs) we fall back to 0x801FFFF0. */
    uint32_t sp_base = h.initial_sp_base ? h.initial_sp_base : 0x801FFFF0u;
    cpu->sp = sp_base + h.initial_sp_offset;
    cpu->gp = h.initial_gp;
    cpu->pc = h.initial_pc;
    /* The BIOS's Exec sets fp to the same stack address and jumps with ra
     * at its own return, 0xBFC03D60 in SCPH-1001; the crt0 stores that ra
     * in a .sbss word before it calls main. */
    cpu->fp = cpu->sp;
    cpu->ra = 0xBFC03D60u;

    OPL_INFO_F("boot", "BSS zeroed (%u bytes); sp=0x%08X fp=0x%08X gp=0x%08X ra=0x%08X pc=0x%08X",
               h.memfill_size, cpu->sp, cpu->fp, cpu->gp, cpu->ra, cpu->pc);
    return 0;
}
