/* games/common/renderer/interp_regions_gen.c — the interp region table
 * generator. See renderer/interp_regions_gen.h. */

#include "renderer/interp_regions_gen.h"

#include "openpete_interp.h"
#include "openpete_psx.h"          /* openpete_psx_to_host_ptr */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#define census_write(fd, buf, n) _write((fd), (buf), (unsigned)(n))
#else
#include <unistd.h>
#define census_write(fd, buf, n) write((fd), (buf), (size_t)(n))
#endif

/* The ring captures the table by pointer, so the built table is the storage it
 * reads from for the process's life. */
static openpete_interp_region_t s_regions[OPENPETE_INTERP_MAX_REGIONS];
static unsigned                 s_count;

/* OPENPETE_INTERP_REGION_CENSUS=1: per region, how many ticks the capture ran
 * and on how many of them the region's bytes differed from the previous tick's.
 * A row that never changes is capturing the wrong span (or a span the game does
 * not advance); a row one word off a struct base changes on a different set of
 * ticks than the base does. Sampled from guest RAM independently of the ring, so
 * the ring's whole-snapshot dedupe cannot hide a per-region result. Its hook is
 * the per-object capture, which the ring runs only while interpolation capture
 * is on, so a headless census run passes --render-fps above the tick rate. */
static struct {
    unsigned char* prev;
    int            have_prev;
    unsigned long  ticks;
    unsigned long  changed;
} s_census[OPENPETE_INTERP_MAX_REGIONS];

static openpete_interp_obj_capture_fn s_game_obj_capture;

static int census_on(void) {
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("OPENPETE_INTERP_REGION_CENSUS");
        on = (e && *e && *e != '0');
    }
    return on;
}

int game_interp_rows_add_one(const char* name, uint32_t addr, uint32_t length,
                             int ra_commit) {
    if (!name || !addr || !length) return -1;
    if (s_count >= OPENPETE_INTERP_MAX_REGIONS) return -1;
    unsigned i = s_count++;
    s_regions[i].name      = name;
    s_regions[i].psx_addr  = addr;
    s_regions[i].length    = length;
    s_regions[i].ra_commit = ra_commit;
    return (int)i;
}

int game_interp_rows_add(const game_interp_row_t* rows, unsigned count) {
    int first = -1;
    for (unsigned r = 0; rows && r < count; ++r) {
        int i = game_interp_rows_add_one(rows[r].name, rows[r].addr,
                                         rows[r].length, rows[r].ra_commit);
        if (first < 0) first = i;
    }
    return first;
}

int game_interp_row_index(const char* name) {
    if (!name) return -1;
    for (unsigned i = 0; i < s_count; ++i)
        if (s_regions[i].name && strcmp(s_regions[i].name, name) == 0) return (int)i;
    return -1;
}

/* One raw write to the stderr descriptor: the process mirrors stderr to a log
 * file and tears that stream down in its own exit hook, which can run before
 * this one and swallow anything written through the FILE*. */
static void census_report(void) {
    char buf[128 * OPENPETE_INTERP_MAX_REGIONS + 128];
    int n = snprintf(buf, sizeof buf, "[interp-region-census] %-14s %-10s %6s %3s %10s %10s\n",
            "region", "addr", "len", "rac", "ticks", "changed");
    for (unsigned i = 0; i < s_count && n > 0 && (size_t)n < sizeof buf; ++i) {
        n += snprintf(buf + n, sizeof buf - (size_t)n,
                      "[interp-region-census] %-14s 0x%08X %6u %3d %10lu %10lu\n",
                      s_regions[i].name ? s_regions[i].name : "?",
                      s_regions[i].psx_addr, s_regions[i].length,
                      s_regions[i].ra_commit, s_census[i].ticks, s_census[i].changed);
    }
    fflush(stderr);
    (void)census_write(2, buf, n > 0 ? n : 0);
}

static void census_sample(void) {
    for (unsigned i = 0; i < s_count; ++i) {
        const void* host = openpete_psx_to_host_ptr(s_regions[i].psx_addr);
        if (!host) continue;
        uint32_t len = s_regions[i].length;
        if (!s_census[i].prev) {
            s_census[i].prev = (unsigned char*)malloc(len);
            if (!s_census[i].prev) continue;
        }
        s_census[i].ticks++;
        if (s_census[i].have_prev && memcmp(s_census[i].prev, host, len) != 0)
            s_census[i].changed++;
        memcpy(s_census[i].prev, host, len);
        s_census[i].have_prev = 1;
    }
}

/* The ring's own capture (openpete_interp_capture) runs immediately before the
 * per-object capture, so sampling here reads the same tick-end RAM the ring
 * just captured. */
static void census_obj_capture(void) {
    census_sample();
    if (s_game_obj_capture) s_game_obj_capture();
}

void game_interp_rows_obj_capture(openpete_interp_obj_capture_fn fn,
                                  uint32_t blob_bytes, uint32_t slots) {
    s_game_obj_capture = fn;
    openpete_interp_register_obj_capture(census_on() ? census_obj_capture : fn,
                                         blob_bytes, slots);
}

void game_interp_rows_commit(void) {
    openpete_interp_register_regions(s_regions, s_count);
    if (census_on()) {
        static int armed = 0;
        if (!armed) { armed = 1; atexit(census_report); }
    }
}
