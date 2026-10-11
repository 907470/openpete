// games/spyro-3/code_patch_guard.c - the entry guard over stubbed EXE
// functions (code_patch_guard.h).

#include "code_patch_guard.h"
#include "openpete_fiber.h"          /* openpete_speculative, openpete_vbl */
#include "openpete_log.h"
#include "psx_runtime_internal.h"    /* CPUState + its read callbacks */
#include "features/antipiracy_scan.h"

#include <stdio.h>
#include <stdlib.h>

#define MIPS_JR_RA 0x03E00008u
#define MIPS_NOP   0x00000000u

typedef struct {
    uint32_t    addr;
    uint32_t    word0, word1;  /* the EXE's first two instructions */
    const char* role;
    /* run counters */
    uint32_t    returns, entries;
    int         first_return_vbl, first_entry_vbl;
    uint32_t    bad0, bad1;    /* last unmodelled pair reported */
    int         bad_reported;
} guard_row_t;

/* The entry words are the decomp's (asm/nonmatchings/updatepause/
 * func_8005663C.s: addiu $sp,$sp,-0x18; lui $a1,%hi(D_8006FBC8) and
 * asm/nonmatchings/update/func_800512E4.s: addiu $sp,$sp,-0x40;
 * sw $s0,0x30($sp)). */
static guard_row_t s_rows[] = {
    { 0x8005663Cu, 0x27BDFFE8u, 0x3C058007u, "pause update", 0, 0, -1, -1, 0, 0, 0 },
    { 0x800512E4u, 0x27BDFFC0u, 0xAFB00030u, "update",       0, 0, -1, -1, 0, 0, 0 },
};

/** @brief Print each guarded function's guard returns and body entries. */
static void exit_report(void) {
    for (size_t i = 0; i < sizeof s_rows / sizeof s_rows[0]; i++) {
        const guard_row_t* r = &s_rows[i];
        fprintf(stderr, "[s3-patch-guard] func_%08X (%s): guard returns=%u (first vbl %d) "
                        "body entries=%u (first vbl %d)\n",
                (unsigned)r->addr, r->role, r->returns, r->first_return_vbl, r->entries,
                r->first_entry_vbl);
    }
}

void spyro3_code_patch_guard_boot(void) {
    static int s_atexit;
    if (!s_atexit) s_atexit = atexit(exit_report) == 0;
}

int spyro3_code_patch_guard(CPUState* cpu, uint32_t addr) {
    guard_row_t* r;
    if (addr == 0x8005663Cu) r = &s_rows[0];
    else if (addr == 0x800512E4u) r = &s_rows[1];
    else return 0;

    const uint32_t w0 = cpu->read_word(addr);
    const uint32_t w1 = cpu->read_word(addr + 4u);
    const int counted = !openpete_speculative;
    const int vbl = SDL_GetAtomicInt(&openpete_vbl);
    if (w0 == MIPS_JR_RA && w1 == MIPS_NOP) {
        if (counted && r->returns++ == 0u) r->first_return_vbl = vbl;
        return 1;
    }
    if (counted && r->entries++ == 0u) r->first_entry_vbl = vbl;
    if ((w0 != r->word0 || w1 != r->word1) && counted &&
        (!r->bad_reported || w0 != r->bad0 || w1 != r->bad1)) {
        char detail[160];
        snprintf(detail, sizeof detail,
                 "func_%08X entry words 0x%08X 0x%08X are neither the EXE's "
                 "(0x%08X 0x%08X) nor jr ra; nop at vbl %d; the body runs",
                 (unsigned)addr, (unsigned)w0, (unsigned)w1, (unsigned)r->word0,
                 (unsigned)r->word1, vbl);
        spyro3_antipiracy_report_fail("patch", detail);
        r->bad0 = w0;
        r->bad1 = w1;
        r->bad_reported = 1;
    }
    return 0;
}
