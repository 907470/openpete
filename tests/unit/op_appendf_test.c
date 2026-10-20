/* tests/unit/op_appendf_test.c - op_appendf (openpete_str.h): the tracked
 * length never exceeds the bytes the buffer holds, the buffer stays
 * NUL-terminated, and appends past a truncation write nothing. The buffer
 * sits between guard bytes that must survive every call. */

#include "openpete_str.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL ");                                       \
            fprintf(stderr, __VA_ARGS__);                                   \
            fprintf(stderr, "\n     at %s:%d\n", __FILE__, __LINE__);       \
            g_fail++;                                                       \
        }                                                                   \
    } while (0)

/** @brief Appends that fit advance the length by the text's length. */
static void fits(void) {
    char buf[16];
    size_t n = 0;
    CHECK(op_appendf(buf, sizeof buf, &n, "ab%d", 12) == 0, "ab12 should fit");
    CHECK(op_appendf(buf, sizeof buf, &n, "%s", "cd") == 0, "cd should fit");
    CHECK(n == 6 && strcmp(buf, "ab12cd") == 0, "n=%zu buf='%s'", n, buf);
}

/** @brief An overflowing append clamps the length to cap - 1, keeps the terminator, and later appends write nothing. */
static void overflows(void) {
    struct { char lo[8]; char buf[10]; char hi[8]; } g;
    memset(&g, 0x5A, sizeof g);
    size_t n = 0;
    CHECK(op_appendf(g.buf, sizeof g.buf, &n, "%s", "12345") == 0, "12345 should fit");
    CHECK(op_appendf(g.buf, sizeof g.buf, &n, "%s", "6789abcdef") == -1,
          "the overflow should report truncation");
    CHECK(n == sizeof g.buf - 1, "n=%zu after overflow, want %zu", n, sizeof g.buf - 1);
    CHECK(strcmp(g.buf, "123456789") == 0, "buf='%s'", g.buf);
    CHECK(op_appendf(g.buf, sizeof g.buf, &n, "%s", "x") == -1, "a full buffer should refuse");
    CHECK(n == sizeof g.buf - 1 && g.buf[n] == '\0', "n=%zu after the full-buffer append", n);
    for (size_t i = 0; i < sizeof g.lo; i++)
        CHECK(g.lo[i] == 0x5A && g.hi[i] == 0x5A, "guard byte %zu overwritten", i);
}

/** @brief A zero-capacity buffer is never written and reports truncation. */
static void zero_cap(void) {
    char c = 'q';
    size_t n = 0;
    CHECK(op_appendf(&c, 0, &n, "%s", "x") == -1 && n == 0 && c == 'q',
          "cap 0: n=%zu c=%c", n, c);
}

int main(void) {
    fits();
    overflows();
    zero_cap();
    if (g_fail) printf("op_appendf_test: FAILED (%d)\n", g_fail);
    else        printf("op_appendf_test: ok\n");
    return g_fail ? 1 : 0;
}
