/* psx_runtime_layout_test.c - CPUState layout equality between
 * core/include/psxrt/psx_runtime.h and the recompiler runner's header.
 *
 * The savestate cpu section copies CPUState byte for byte up to
 * offsetof(CPUState, read_word), and mods compiled against an earlier SDK
 * read the struct by offset, so every field keeps its offset, size and
 * type. The runner header is included first with its struct tag and
 * typedef renamed through a macro; its include guard is then undefined so
 * the engine header, which uses the same guard name, is read in full.
 * Every check is a static assertion: a layout difference fails the build
 * of this test, and the executable only reports how many fields it
 * compared. This file is the only one that includes the runner header and
 * it is linked into no shipped target. */
#define CPUState RunnerCPUState
#include "../../third_party/psxrecomp/runner/include/psx_runtime.h"
#undef CPUState
#undef PSX_RUNTIME_H

#include "psx_runtime.h"

#include <stddef.h>
#include <stdio.h>

#if defined(__GNUC__) || defined(__clang__)
#define SAME_TYPE(a, b) __builtin_types_compatible_p(__typeof__(a), __typeof__(b))
#else
#define SAME_TYPE(a, b) 1
#endif

/* Every CPUState member in declaration order. */
#define CPUSTATE_FIELDS(X)                                                  \
    X(zero) X(at) X(v0) X(v1) X(a0) X(a1) X(a2) X(a3)                       \
    X(t0) X(t1) X(t2) X(t3) X(t4) X(t5) X(t6) X(t7)                         \
    X(s0) X(s1) X(s2) X(s3) X(s4) X(s5) X(s6) X(s7)                         \
    X(t8) X(t9) X(k0) X(k1) X(gp) X(sp) X(fp) X(ra)                         \
    X(pc) X(hi) X(lo) X(cop0) X(gte_data) X(gte_ctrl)                       \
    X(read_word) X(write_word) X(read_half) X(write_half)                   \
    X(read_byte) X(write_byte) X(lwl) X(lwr) X(swl) X(swr)

#define CHECK_FIELD(f)                                                      \
    _Static_assert(offsetof(CPUState, f) == offsetof(RunnerCPUState, f),    \
                   "CPUState." #f " offset differs");                       \
    _Static_assert(sizeof(((CPUState*)0)->f) ==                             \
                   sizeof(((RunnerCPUState*)0)->f),                         \
                   "CPUState." #f " size differs");                         \
    _Static_assert(SAME_TYPE(((CPUState*)0)->f, ((RunnerCPUState*)0)->f),   \
                   "CPUState." #f " type differs");
CPUSTATE_FIELDS(CHECK_FIELD)

_Static_assert(sizeof(CPUState) == sizeof(RunnerCPUState), "CPUState size differs");
_Static_assert(_Alignof(CPUState) == _Alignof(RunnerCPUState), "CPUState alignment differs");

/* The field list covers the whole struct: the last member ends at its size. */
_Static_assert(offsetof(CPUState, swr) + sizeof(((CPUState*)0)->swr) == sizeof(CPUState),
               "CPUSTATE_FIELDS misses a trailing member");

#define COUNT_FIELD(f) +1
enum { CPUSTATE_FIELD_COUNT = 0 CPUSTATE_FIELDS(COUNT_FIELD) };

/** @brief Report the number of CPUState fields whose layout the static assertions compared.
 *
 * @return 0; a layout difference fails compilation instead. */
int main(void) {
    printf("psx_runtime_layout_test: %d fields, sizeof %zu: PASS\n",
           (int)CPUSTATE_FIELD_COUNT, sizeof(CPUState));
    return 0;
}
