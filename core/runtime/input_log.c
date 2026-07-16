/* core/runtime/input_log.c — see openpete_input_log.h. */

#include "openpete_input_log.h"
#include "openpete_log.h"

#include <stdlib.h>
#include <string.h>

static openpete_input_rec_t* s_log = NULL;
static int s_count = 0;
static int s_capacity = 0;
static int s_alloc_failed = 0;

int input_log_append(uint16_t bits, const uint8_t axes[4]) {
    if (s_alloc_failed) return -1;
    if (s_count == s_capacity) {
        int new_cap = s_capacity ? s_capacity * 2 : 16 * 1024;
        openpete_input_rec_t* g = (openpete_input_rec_t*)
            realloc(s_log, (size_t)new_cap * sizeof(*s_log));
        if (!g) {
            /* Stop growing rather than crash; consumers see a truncated
             * log (bk2_record already had this drop-the-frame policy). */
            OPL_ERROR_F("input-log", "realloc failed at %d entries; log frozen",
                        s_count);
            s_alloc_failed = 1;
            return -1;
        }
        s_log = g;
        s_capacity = new_cap;
    }
    openpete_input_rec_t* r = &s_log[s_count];
    r->bits = bits;
    if (axes) memcpy(r->axes, axes, 4);
    else      r->axes[0] = r->axes[1] = r->axes[2] = r->axes[3] = 128;
    return s_count++;
}

int input_log_count(void) { return s_count; }

const openpete_input_rec_t* input_log_get(int idx) {
    return (idx >= 0 && idx < s_count) ? &s_log[idx] : NULL;
}

void input_log_truncate(int count) {
    if (count < 0) count = 0;
    if (count < s_count) s_count = count;
}

void input_log_reset(void) {
    free(s_log);
    s_log = NULL;
    s_count = s_capacity = 0;
    s_alloc_failed = 0;
}
