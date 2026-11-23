/* toy_counter.c - the lost-update toy's shared counters, compiled with
 * -fsanitize=thread so every access calls the scheduler's __tsan_* hooks. */
#include "toys.h"

int g_toy_counter_unseamed;
int g_toy_counter_seamed;

void toy_counter_inc(int *c)
{
    const int v = *c;
    *c = v + 1;
}
