/* games/spyro-1/renderer/tracer_snapshot.c - see tracer_snapshot.h. */
#include "renderer/tracer_snapshot.h"

int tracer_snapshot_take(CPUState* cpu, uint32_t count_va, uint32_t cnttab_va,
                         uint32_t basetab_va, tracer_set_t* out, int max) {
    int n = 0;
    uint32_t nsets = cpu->read_word(count_va);
    if (nsets > TRACER_TABLE_SLOTS) nsets = TRACER_TABLE_SLOTS;
    for (uint32_t s = 0; s < nsets && n < max; s++) {
        int count = (int)cpu->read_word(cnttab_va + s * 4u);
        uint32_t base = cpu->read_word(basetab_va + s * 4u);
        if (count <= 0 || base < 0x80000000u) continue;
        if (count > TRACER_MAX_PTS) count = TRACER_MAX_PTS;
        tracer_set_t* ts = &out[n++];
        ts->count = count;
        for (int i = 0; i < count; i++) {
            uint32_t p = base + (uint32_t)i * 28u;
            ts->pt[i].x = (int)cpu->read_word(p + 0u);
            ts->pt[i].y = (int)cpu->read_word(p + 4u);
            ts->pt[i].z = (int)cpu->read_word(p + 8u);
            ts->pt[i].f = (int)cpu->read_word(p + 24u);
        }
    }
    return n;
}
