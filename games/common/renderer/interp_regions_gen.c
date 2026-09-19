/* games/common/renderer/interp_regions_gen.c — the interp region table
 * generator. See renderer/interp_regions_gen.h. */

#include "renderer/interp_regions_gen.h"

#include "openpete_interp.h"
#include "openpete_interp_objs.h"
#include "openpete_psx.h"          /* openpete_psx_to_host_ptr */
#include "game_layout.h"
#include "renderer/env_knob.h"

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
    ENV_FLAG_LATCH(on, "OPENPETE_INTERP_REGION_CENSUS");
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

extern unsigned openpete_frame;

static uint32_t rd32(uint32_t addr) {
    const void* p = openpete_psx_to_host_ptr(addr);
    uint32_t v = 0;
    if (p) memcpy(&v, p, 4);
    return v;
}

void game_interp_capture_moby_array(void) {
    const uint32_t head = game_layout.moby.array_head;
    if (!head) return;
    uint32_t arr = game_layout.moby.array_direct ? head : rd32(head);
    if (arr < 0x80000000u) return;

    const uint32_t stride = game_layout.moby.array_stride;
    const uint32_t blob   = openpete_interp_objs_blob_bytes();
    /* OPENPETE_CAPTRACE_CLS=<class|0xFFFF>: the per-TICK canonical position of
     * every moby of that class, the capture side of the staircase-vs-smooth
     * comparison against the sub-tick trace. */
    static long s_ctc = -2;
    if (s_ctc == -2) { const char* e = getenv("OPENPETE_CAPTRACE_CLS"); s_ctc = e ? strtol(e, NULL, 0) : -1; }

    /* Bound the walk below the store capacity, leaving room for the pools
     * captured after it so a full array can never crowd them out; the array's
     * sentinel stops the walk after the live prefix anyway. */
    const long bound = (long)openpete_interp_objs_slots() - 512;
    uint32_t m = arr;
    for (long guard = 0; guard < bound; ++guard) {
        const void* host = openpete_psx_to_host_ptr(m);
        if (!host) break;
        uint8_t st = ((const uint8_t*)host)[game_layout.moby.state_off];
        if (st == (uint8_t)game_layout.moby.state_term) break;   /* end of the array */
        if (st < 0x80) {                                         /* the rest are dormant */
            openpete_interp_objs_capture(m, host, blob);
            if (s_ctc >= 0) {
                uint16_t c2; memcpy(&c2, (const uint8_t*)host + game_layout.moby.class_off, 2);
                if (s_ctc == 0xFFFF || c2 == (uint16_t)s_ctc) {
                    int32_t pos[3]; memcpy(pos, (const uint8_t*)host + game_layout.moby.pos_off, 12);
                    fprintf(stderr, "[cap-tick f=%u] m=%08x cls=%03x pos=(%d,%d,%d)\n",
                            openpete_frame, m, c2, pos[0], pos[1], pos[2]);
                }
            }
        }
        m += stride;
    }
}

/* The particle pool of a game whose particle renderer links its own primitives
 * (game_layout.particles). The renderer projects each record under the tick's
 * camera, so a sub-tick present that replays those primitives needs the
 * record's previous state to rebase them onto the pose it shows; the pool sits
 * outside the moby array, so the shared walk above never reaches it.
 *
 * The walk is the routine's own: from the pool base, one record per
 * `rec_stride`, reading the selector halfword whose sign marks an inactive
 * slot and whose arithmetic shift by 8 is -1 at the end-of-pool sentinel. The
 * key is the record's address, which is its slot; a slot a new record reuses
 * is the consumer's pairing verdict, not this walk's. Read-only against guest
 * RAM. */
void game_interp_capture_particle_pool(void) {
    if (!game_layout.particles.array_ptr || !game_layout.particles.rec_stride) return;
    const uint32_t base = rd32(game_layout.particles.array_ptr);
    if (base < 0x80000000u) return;
    const uint32_t stride = game_layout.particles.rec_stride;
    const uint32_t len = openpete_interp_objs_blob_bytes() < stride
                             ? openpete_interp_objs_blob_bytes() : stride;
    /* The pool is walked inside the room the moby walk leaves free above its
     * own bound, so a full array can never crowd it out. */
    for (uint32_t i = 0, rec = base; i < 512u; ++i, rec += stride) {
        const void* host = openpete_psx_to_host_ptr(rec);
        if (!host) break;
        int16_t sel;
        memcpy(&sel, (const uint8_t*)host + game_layout.particles.sel_off, 2);
        if (sel < 0) {
            if ((sel >> 8) == -1) break;              /* end of the pool */
            continue;                                 /* inactive slot */
        }
        openpete_interp_objs_capture(rec, host, len);
    }
}

/* Scrolling-texture env-anim descriptors (flowing water and waterfalls). The
 * update keeps each scroller's whole animation state in a 7-bit accumulator at
 * desc+6 and commits it to the tiledef V bytes as whole texels on a keyframe
 * advance, so a scrolled texture steps at the sim rate while the world around
 * it interpolates. Capture just the (tile, accumulator) pair at desc+4, not the
 * descriptor head, whose countdown byte at +3 decrements every tick: including
 * it would make the blob differ every tick and defeat the table's distinct-roll
 * dedupe, collapsing a slow scroller's real advance interval to a single tick.
 * terrain.c (tsc_scroll_refresh) reads the pair back and interpolates the
 * accumulator at the slot's own alpha, so the scroll stays smooth even when the
 * camera is still, where the camera ring dedupes and the shared bracket alpha
 * never engages. */
static void capture_env_anim_scroll(void) {
    if (!game_layout.env_anim.scroll) return;
    uint32_t cnt  = rd32(game_layout.env_anim.scroll);
    uint32_t base = rd32(game_layout.env_anim.scroll + 4u);
    if (!base || !cnt || cnt > 1024u) return;
    for (uint32_t i = 0; i < cnt; ++i) {
        uint32_t desc = rd32(base + i * 4u);
        if (!desc) continue;
        const void* host = openpete_psx_to_host_ptr(desc + 4u);
        if (host) openpete_interp_objs_capture(desc, host, 4u);
    }
}

static void (*s_extra_pools)(void);

static void capture_objs_common(void) {
    openpete_interp_objs_begin_tick();
    game_interp_capture_moby_array();
    if (s_extra_pools) s_extra_pools();
    capture_env_anim_scroll();
    openpete_interp_objs_end_tick();

    /* OPENPETE_INTERP_OBJDBG=1: the keyed table's population every 120 ticks.
     * A spawned/despawned pair that never settles is a key that is not stable
     * across ticks, which shows on screen as objects snapping at the tick rate. */
    static int dbg = -1;
    ENV_FLAG_LATCH(dbg, "OPENPETE_INTERP_OBJDBG");
    if (dbg && (openpete_frame % 120u) == 0) {
        int tracked, paired, spawned, despawned;
        openpete_interp_objs_stats(&tracked, &paired, &spawned, &despawned);
        fprintf(stderr, "[interp-objs f=%u] tracked=%d paired=%d spawned=%d despawned=%d\n",
                openpete_frame, tracked, paired, spawned, despawned);
    }
}

void game_interp_register_common(const game_interp_row_t* rows, unsigned count,
                                 uint32_t cam_length, void (*extra_pools)(void)) {
    /* ra_commit = 1 on the camera: it presents from the committed timeline under
     * runahead, so a misprediction never shows as a pan that un-shows. The
     * follow camera is where misprediction is catastrophic (it is the view
     * transform) and where the extra ticks of latency are invisible (it trails
     * by design); the player and the mobys stay led, keeping the input-lag win
     * where it is felt. This picks which timeline the presentation lerps; the
     * game's own camera state is untouched. */
    game_interp_rows_add_one(GAME_INTERP_CAM_ROW, game_layout.cam.base, cam_length, 1);
    game_interp_rows_add(rows, count);
    game_interp_rows_commit();
    s_extra_pools = extra_pools;
    /* One whole Moby record per object, so every consumer reads its fields at
     * their natural offsets, and the store's default capacity. */
    game_interp_rows_obj_capture(capture_objs_common,
                                 game_layout.moby.array_stride,
                                 OPENPETE_INTERP_OBJ_SLOTS);
}

void game_interp_rows_commit(void) {
    openpete_interp_register_regions(s_regions, s_count);
    if (census_on()) {
        static int armed = 0;
        if (!armed) { armed = 1; atexit(census_report); }
    }
}
