/* div_semantics_test - the recompiled MIPS div and divu against R3000
 * results. div_site and divu_site are the first div and divu statements of
 * one game's generated PSX.EXE_full.c, copied verbatim by
 * extract_div_sites.py. The R3000 never traps on division: x/0 leaves
 * LO = (x >= 0) ? -1 : 1 (div) or 0xFFFFFFFF (divu) and HI = x, and
 * 0x80000000 / -1 leaves LO = 0x80000000, HI = 0. A host division of that
 * pair raises SIGFPE on x86, which ends this process with a failure. */

#include "psx_runtime.h"

#include <stdint.h>
#include <stdio.h>

void div_site(CPUState* cpu, uint32_t n, uint32_t d);
void divu_site(CPUState* cpu, uint32_t n, uint32_t d);

typedef struct {
    uint32_t n, d, lo, hi;
} div_case_t;

static const div_case_t k_div_cases[] = {
    {0x80000000u, 0xFFFFFFFFu, 0x80000000u, 0},
    {0x80000000u, 1, 0x80000000u, 0},
    {7, (uint32_t)-2, (uint32_t)-3, 1},
    {(uint32_t)-7, 2, (uint32_t)-3, (uint32_t)-1},
    {5, 0, 0xFFFFFFFFu, 5},
    {0, 0, 0xFFFFFFFFu, 0},
    {(uint32_t)-5, 0, 1, (uint32_t)-5},
    {0x80000000u, 0, 1, 0x80000000u},
};

static const div_case_t k_divu_cases[] = {
    {0x80000000u, 0xFFFFFFFFu, 0, 0x80000000u},
    {0x80000000u, 1, 0x80000000u, 0},
    {7, 2, 3, 1},
    {0xFFFFFFF9u, 2, 0x7FFFFFFCu, 1},
    {5, 0, 0xFFFFFFFFu, 5},
    {0x80000000u, 0, 0xFFFFFFFFu, 0x80000000u},
};

/** @brief Run every case through one site and count the results that differ from the R3000's.
 *
 * @param name label printed with each mismatch
 * @param site the copied statement's wrapper
 * @param cases expected LO and HI per dividend and divisor
 * @param count number of cases
 * @return number of mismatching cases */
static int run_cases(const char* name, void (*site)(CPUState*, uint32_t, uint32_t),
                     const div_case_t* cases, size_t count)
{
    int fails = 0;
    for (size_t i = 0; i < count; i++) {
        CPUState cpu = {0};
        cpu.lo = 0xDEADBEEFu;
        cpu.hi = 0xDEADBEEFu;
        site(&cpu, cases[i].n, cases[i].d);
        if (cpu.lo != cases[i].lo || cpu.hi != cases[i].hi) {
            fprintf(stderr, "FAIL %s 0x%08X / 0x%08X: LO=0x%08X HI=0x%08X, want LO=0x%08X HI=0x%08X\n",
                    name, cases[i].n, cases[i].d, cpu.lo, cpu.hi, cases[i].lo, cases[i].hi);
            fails++;
        }
    }
    return fails;
}

/** @brief Check the copied div and divu sites against the R3000 table.
 *
 * @return 0 when every case matches, 1 otherwise */
int main(void)
{
    int fails = run_cases("div", div_site, k_div_cases, sizeof k_div_cases / sizeof k_div_cases[0]);
    fails += run_cases("divu", divu_site, k_divu_cases, sizeof k_divu_cases / sizeof k_divu_cases[0]);
    if (fails) {
        fprintf(stderr, "div_semantics_test: %d case(s) failed\n", fails);
        return 1;
    }
    printf("div_semantics_test: PASS (%zu div, %zu divu cases)\n",
           sizeof k_div_cases / sizeof k_div_cases[0], sizeof k_divu_cases / sizeof k_divu_cases[0]);
    return 0;
}
