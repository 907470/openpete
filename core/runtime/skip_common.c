// core/runtime/skip_common.c - plumbing shared by the games' skip modules.
// See core/include/openpete_skip.h.

#include "openpete_skip.h"

#include <stdlib.h>

unsigned openpete_skip_failsafe_vbls(void) {
    static long s_vbls = -1;
    if (s_vbls < 0) {
        const char* e = getenv("OPENPETE_SKIP_FAILSAFE_VBL");
        s_vbls = (e && *e) ? strtol(e, NULL, 10) : 4000;
        if (s_vbls <= 0) s_vbls = 4000;
    }
    return (unsigned)s_vbls;
}
