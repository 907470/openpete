/**
 * @file r10_libc.c
 * @brief r10-libc: one call into each of string.h, stdio.h, math.h,
 *        stdlib.h and stdatomic.h plus an __int128 division, checked
 *        against known results at mod entry.
 *
 * The operands pass through volatile objects so the compiler emits the
 * library calls and the runtime division instead of folding them.
 */

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openpete_mod_api.h>

static volatile double   g_two   = 2.0;     /**< sqrt and pow operand. */
static volatile __int128 g_num   = ((__int128)1 << 100) + 12345; /**< Dividend. */
static volatile __int128 g_den   = ((__int128)1 << 64) + 3;      /**< Divisor. */
static atomic_int        g_count = 0;       /**< stdatomic counter. */

/** @brief qsort comparator ordering ints ascending.
 *
 * @param a First element.
 * @param b Second element.
 * @return Negative, zero or positive as *a is below, equal to or above *b. */
static int cmp_int(const void* a, const void* b) {
    const int x = *(const int*)a, y = *(const int*)b;
    return (x > y) - (x < y);
}

/** @brief Run each library call, log one result line, and fail the entry on any mismatch.
 *
 * The __int128 quotient and remainder are checked by recombination
 * (q * den + r == num, r < den), which needs no second division.
 * @param api The engine table.
 * @param self This mod's handle.
 * @return 0 when every check holds, -1 otherwise. */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    int fails = 0;

    char buf[64];
    int n = snprintf(buf, sizeof buf, "%d-%s-%.3f", 42, "libc", 1.5);
    if (n != 13 || strcmp(buf, "42-libc-1.500") != 0) fails |= 1 << 0;
    char copy[64];
    memcpy(copy, buf, strlen(buf) + 1);
    if (strncmp(copy, "42-", 3) != 0 || strchr(copy, 'c') == NULL) fails |= 1 << 1;

    double s = sqrt(g_two), p = pow(g_two, 10.0);
    if (fabs(s * s - 2.0) > 1e-12 || p != 1024.0 || fabs(sin(0.0)) != 0.0)
        fails |= 1 << 2;

    int v[5] = { 9, 3, 7, 1, 5 };
    qsort(v, 5, sizeof v[0], cmp_int);
    if (v[0] != 1 || v[4] != 9 || strtol("-123", NULL, 10) != -123) fails |= 1 << 3;
    int* heap = (int*)malloc(16 * sizeof *heap);
    if (!heap) fails |= 1 << 4;
    free(heap);

    for (int i = 0; i < 3; i++) atomic_fetch_add(&g_count, 2);
    if (atomic_load(&g_count) != 6) fails |= 1 << 5;

    const __int128 num = g_num, den = g_den;
    const __int128 q = num / den, r = num % den;
    if (q * den + r != num || r < 0 || r >= den) fails |= 1 << 6;
    const unsigned __int128 uq = (unsigned __int128)num / (unsigned __int128)den;
    if ((__int128)uq != q) fails |= 1 << 7;

    api->log(self, fails ? OP_MOD_LOG_ERROR : OP_MOD_LOG_INFO,
             "r10-libc: libc probe %s (fail mask 0x%02x, \"%s\", sqrt2=%.6f, q=%llu)",
             fails ? "FAILED" : "OK", (unsigned)fails, buf, s,
             (unsigned long long)q);
    return fails ? -1 : 0;
}
