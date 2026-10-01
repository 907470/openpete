/* games/common/dispatch_census.c — see dispatch_census.h. */
#include "dispatch_census.h"

#include <stdio.h>
#include <stdlib.h>

static FILE* s_out = NULL;

int dispatch_census_open(void) {
    const char* path = getenv("OPENPETE_DISPATCH_CENSUS");
    if (!s_out && path && path[0]) s_out = fopen(path, "w");
    return s_out != NULL;
}

void dispatch_census_append(const char* overlay, uint32_t addr, uint32_t ra,
                            const char* cls, int resolved) {
    if (!s_out) return;
    fprintf(s_out, "%s %08X %08X %s %s\n", overlay, (unsigned)addr,
            (unsigned)ra, cls, resolved ? "resolved" : "missed");
}
