/* games/common/write_census.c — see write_census.h. */
#include "write_census.h"
#include "openpete_fiber.h"
#include "spyro_overlay_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CENSUS_MAX_RANGES 8

static struct { uint32_t lo, hi; } s_ranges[CENSUS_MAX_RANGES];
static int      s_nranges = 0;
static FILE*    s_out = NULL;
static int      s_active = 0;
static uint32_t s_entry = 0;
static uint32_t s_phase_entry = 0;
static int      s_phase = 0;
static unsigned s_seq = 0;
static unsigned s_rows = 0;

static void (*s_orig_write_word)(uint32_t, uint32_t) = NULL;
static void (*s_orig_write_half)(uint32_t, uint16_t) = NULL;
static void (*s_orig_write_byte)(uint32_t, uint8_t)  = NULL;
static void (*s_orig_swl)(uint32_t, uint32_t)        = NULL;
static void (*s_orig_swr)(uint32_t, uint32_t)        = NULL;
static CPUState* s_cpu = NULL;

static int in_range(uint32_t addr, unsigned size) {
    for (int i = 0; i < s_nranges; ++i)
        if (addr + size - 1u >= s_ranges[i].lo && addr <= s_ranges[i].hi) return 1;
    return 0;
}

static void row(uint32_t addr, unsigned size, uint32_t value) {
    if (!s_out || !in_range(addr, size)) return;
    uint32_t magic = spyro_get_active_overlay_magic();
    const spyro_overlay_entry_t* e = magic ? spyro_overlay_find(magic) : NULL;
    fprintf(s_out, "%u,%u,0x%08X,%u,0x%08X,0x%08X,0x%08X,%d,%s\n",
            openpete_frame, s_seq, addr, size, value,
            s_cpu ? s_cpu->ra : 0u, s_entry, s_phase, e ? e->name : "none");
    s_rows++;
}

static void wc_write_word(uint32_t a, uint32_t v) { row(a, 4, v); s_orig_write_word(a, v); }
static void wc_write_half(uint32_t a, uint16_t v) { row(a, 2, v); s_orig_write_half(a, v); }
static void wc_write_byte(uint32_t a, uint8_t v)  { row(a, 1, v); s_orig_write_byte(a, v); }
/** @brief Logs an swl store as a row on the aligned word containing its address, then performs it.
 *
 * swl and swr cover the aligned word containing the address; the merged
 * value is not known here, so the register value is logged.
 * @param a The unaligned guest address.
 * @param v The register value. */
static void wc_swl(uint32_t a, uint32_t v) { row(a & ~3u, 4, v); s_orig_swl(a, v); }
static void wc_swr(uint32_t a, uint32_t v) { row(a & ~3u, 4, v); s_orig_swr(a, v); }

static void census_close(void) {
    if (!s_out) return;
    fflush(s_out);
    fclose(s_out);
    s_out = NULL;
    fprintf(stderr, "[write-census] %u rows, %u phase marks\n", s_rows, s_seq);
}

void write_census_install(CPUState* cpu) {
    const char* spec = getenv("OPENPETE_WRITE_CENSUS");
    if (!spec || !*spec || !cpu) return;
    const char* p = spec;
    while (*p && s_nranges < CENSUS_MAX_RANGES) {
        char* end = NULL;
        uint32_t lo = (uint32_t)strtoul(p, &end, 16);
        if (end == p || strncmp(end, "..", 2) != 0) break;
        p = end + 2;
        uint32_t hi = (uint32_t)strtoul(p, &end, 16);
        if (end == p || hi < lo) break;
        s_ranges[s_nranges].lo = lo;
        s_ranges[s_nranges].hi = hi;
        s_nranges++;
        p = end;
        if (*p == ',') p++;
    }
    if (!s_nranges) {
        fprintf(stderr, "[write-census] OPENPETE_WRITE_CENSUS=%s: no range parsed\n", spec);
        return;
    }
    const char* path = getenv("OPENPETE_WRITE_CENSUS_OUT");
    if (!path || !*path) path = "write-census.csv";
    s_out = fopen(path, "w");
    if (!s_out) {
        fprintf(stderr, "[write-census] cannot open %s\n", path);
        return;
    }
    fprintf(s_out, "vbl,seq,addr,size,value,ra,entry,phase,overlay\n");
    s_cpu = cpu;
    s_orig_write_word = cpu->write_word;
    s_orig_write_half = cpu->write_half;
    s_orig_write_byte = cpu->write_byte;
    s_orig_swl        = cpu->swl;
    s_orig_swr        = cpu->swr;
    cpu->write_word = wc_write_word;
    cpu->write_half = wc_write_half;
    cpu->write_byte = wc_write_byte;
    cpu->swl        = wc_swl;
    cpu->swr        = wc_swr;
    const char* ph = getenv("OPENPETE_WRITE_CENSUS_PHASE");
    if (ph && *ph) s_phase_entry = (uint32_t)strtoul(ph, NULL, 16);
    s_active = 1;
    atexit(census_close);
    fprintf(stderr, "[write-census] %d range(s) -> %s\n", s_nranges, path);
}

int  write_census_active(void)           { return s_active; }
void write_census_note_entry(uint32_t a) {
    s_entry = a;
    if (a == s_phase_entry) write_census_set_phase(1);
}
void write_census_set_phase(int phase)   { if (phase && !s_phase) s_seq++; s_phase = phase; }

void write_census_orig_word(uint32_t a, uint32_t v) { if (s_orig_write_word) s_orig_write_word(a, v); }
void write_census_orig_half(uint32_t a, uint16_t v) { if (s_orig_write_half) s_orig_write_half(a, v); }
