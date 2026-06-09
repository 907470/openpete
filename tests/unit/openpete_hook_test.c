/* openpete_hook_test.c — verifies the OPENPETE_HOOK decomp-seam macro's two
 * invariants (hook-anchor plan step 4):
 *   - matching build (OPENPETE_BUILD undefined): the macro is a no-op, so a
 *     function carrying it behaves identically to the un-hooked source (the
 *     byte-match-preserving property, observed here as "the hook never runs").
 *   - openpete build (OPENPETE_BUILD defined): the macro calls the named host
 *     hook with its arguments.
 *
 * Built TWICE by CMake — once with -DOPENPETE_BUILD and once without — so a
 * single source asserts both expansions. See tests/unit/CMakeLists.txt.
 */
#include "openpete_hook.h"
#include <assert.h>
#include <stdio.h>

static int s_calls = 0;
static int s_sum = 0;

/* The host hook the macro resolves to in the OPENPETE_BUILD expansion. */
void openpete_hook_demo(int a, int b) {
    s_calls++;
    s_sum += a + b;
}

/* A function carrying the seam, exactly as a decomped game function would. */
static int hooked_function(int x) {
    int r = x * 2;
    OPENPETE_HOOK(demo, x, r);   /* seam: no-op in matching, call in openpete */
    return r;
}

int main(void) {
    s_calls = 0;
    s_sum = 0;

    int r = hooked_function(21);
    assert(r == 42);   /* the hook must never alter the function's own result */

#ifdef OPENPETE_BUILD
    /* openpete build: the seam called openpete_hook_demo(21, 42) exactly once. */
    assert(s_calls == 1);
    assert(s_sum == 21 + 42);
    printf("openpete_hook_test[openpete]: hook fired (calls=%d sum=%d) OK\n",
           s_calls, s_sum);
#else
    /* matching build: the seam expanded to a no-op; the hook never ran. */
    assert(s_calls == 0);
    assert(s_sum == 0);
    printf("openpete_hook_test[matching]: hook is a no-op (calls=%d) OK\n",
           s_calls);
#endif
    return 0;
}
