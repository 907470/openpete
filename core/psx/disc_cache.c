/* core/psx/disc_cache.c - see openpete_disc_cache.h. */
#include "openpete_disc_cache.h"
#include "openpete_disc_image.h"
#include "openpete_disc_trace.h"
#include "openpete_crash.h"
#include "openpete_log.h"
#include "openpete_time.h"
#include "openpete_pace_log.h"
#include "psx/libcd.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openpete_thread_roles.h"

/* The image is tracked per 2352-byte sector and filled by one loader thread
 * in runs of missing sectors. Run order: the chunk a game read last missed
 * on, the chunk an XA read last missed on, the game's boot-order profile
 * ranges in order, then every chunk in file order. A chunk is a whole number
 * of sectors, 8 MB rounded down; a chunk run never crosses its chunk and a
 * profile run never leaves its range. A read never waits on the loader; the
 * reader serves a miss from the file. */
#define DC_SECTOR        2352u
#define DC_CHUNK_SECTORS ((size_t)((1u << 23) / DC_SECTOR))   /* 3566 sectors */
#define DC_CHUNK         (DC_CHUNK_SECTORS * DC_SECTOR)

/* Steer slots, in the loader's priority order. Host paths (the woah NSF,
 * the library scan, the ingest copy, the sfx census, mods) have none: a
 * host read never reorders the loader ahead of the game. */
enum { DC_STEER_GAME = 0, DC_STEER_XA, DC_STEER_SLOTS };

/* The VBL rate the throttle's budget accrues on: 2250000/37609 Hz. */
#define DC_VBL_NUM 2250000.0
#define DC_VBL_DEN 37609.0

static uint8_t*       s_img;
static size_t         s_size;
static size_t         s_nchunks;
static size_t         s_nsec;
static uint8_t*       s_have;          /* per-chunk DC_MISSING / DC_RESIDENT / DC_UNREAD (under s_mx) */
static uint8_t*       s_sec;           /* per-sector DC_MISSING / DC_RESIDENT / DC_UNREAD (under s_mx) */
static uint8_t*       s_logged;        /* per-chunk: a miss on it was logged (under s_mx) */
static size_t         s_have_n;        /* chunks resident */
static long           s_want[DC_STEER_SLOTS]; /* chunk each slot steers to, -1 = none */
static size_t         s_seq;           /* next chunk in file order */
static const openpete_disc_range_t* s_order; /* boot-order profile, NULL = none */
static size_t         s_order_n;
static size_t         s_order_at;      /* next profile range (under s_mx) */
static uint64_t       s_bps;           /* OPENPETE_DISC_CACHE_BPS, 0 = unthrottled */
static uint64_t       s_acct;          /* bytes charged to the throttle (under s_mx) */
static SDL_Mutex*     s_mx;
static int            s_abort;
static int            s_active;
static SDL_Thread*    s_thr;
static FILE*          s_fp;
static char*          s_path;          /* the image path; a reader of the same file is served here */
static int64_t        s_t0, s_load_ns;
static uint64_t       s_misses;
static uint64_t       s_misses_by_tag[OPENPETE_DISC_TAG_COUNT];
static int64_t        s_direct_ns;

/* s_have and s_sec states. A DC_UNREAD sector came up short in the loader
 * after DC_READ_TRIES attempts; it is never served from memory, so every
 * read of it goes to the file. A chunk is DC_MISSING while any of its
 * sectors is, else DC_UNREAD while any is unread, else DC_RESIDENT. */
#define DC_MISSING   0u
#define DC_RESIDENT  1u
#define DC_UNREAD    2u
#define DC_READ_TRIES 3

/* Sectors [s0, s1) the loader reads in one go. */
typedef struct { size_t s0, s1; } dc_run_t;

void openpete_disc_cache_set_boot_order(const openpete_disc_range_t* ranges, size_t n) {
    s_order = n ? ranges : NULL;
    s_order_n = ranges ? n : 0;
}

/** @brief Read the sectors of run r from fp into s_img; bytes the read does not supply are zeroed.
 *
 * @param fp An open handle on the image.
 * @param r The run; its last sector may pass the end of the image.
 * @return 1 when the whole run was read, 0 on a seek failure or a short read. */
static int read_run_from(FILE* fp, dc_run_t r) {
    size_t at = r.s0 * DC_SECTOR;
    size_t end = r.s1 * DC_SECTOR; if (end > s_size) end = s_size;
    size_t want = end - at;
    size_t got = 0;
    if (fseek(fp, (long)at, SEEK_SET) == 0) got = fread(s_img + at, 1, want, fp);
    openpete_disc_trace_bytes(OPENPETE_DISC_TAG_CACHE_LOAD, at, got, OPENPETE_DISC_DIRECT);
    if (got < want) memset(s_img + at + got, 0, want - got);
    return got == want;
}

/** @brief Read run r through the loader's handle, retrying a short read up to DC_READ_TRIES times.
 *
 * @param r The run.
 * @return 1 when the run was read whole. */
static int read_run(dc_run_t r) {
    for (int t = 0; t < DC_READ_TRIES; t++)
        if (read_run_from(s_fp, r)) return 1;
    return 0;
}

/** @brief Find the first run of DC_MISSING sectors in [lo, hi), under s_mx.
 *
 * @param lo First sector.
 * @param hi One past the last sector; clipped to the image.
 * @param r Receives the run.
 * @return 1 when a run was found. */
static int missing_run(size_t lo, size_t hi, dc_run_t* r) {
    if (hi > s_nsec) hi = s_nsec;
    size_t s = lo;
    while (s < hi && s_sec[s] != DC_MISSING) s++;
    if (s >= hi) return 0;
    size_t e = s;
    while (e < hi && s_sec[e] == DC_MISSING) e++;
    r->s0 = s; r->s1 = e;
    return 1;
}

/** @brief Find the first run of missing sectors in chunk c, under s_mx.
 *
 * @param c Chunk index.
 * @param r Receives the run.
 * @return 1 when the chunk has a missing sector. */
static int chunk_run(size_t c, dc_run_t* r) {
    return s_have[c] == DC_MISSING &&
           missing_run(c * DC_CHUNK_SECTORS, (c + 1u) * DC_CHUNK_SECTORS, r);
}

/** @brief Pick the loader's next run under s_mx: each steer slot's chunk in slot order, the profile ranges in order, then file order.
 *
 * @param r Receives the run.
 * @return 1 when a run was chosen, 0 when no sector is missing. */
static int next_run(dc_run_t* r) {
    for (int k = 0; k < DC_STEER_SLOTS; k++) {
        long w = s_want[k];
        if (w >= 0 && chunk_run((size_t)w, r)) return 1;
        s_want[k] = -1;
    }
    for (; s_order_at < s_order_n; s_order_at++) {
        const openpete_disc_range_t* g = &s_order[s_order_at];
        if (missing_run(g->lba, (size_t)g->lba + g->count, r)) return 1;
    }
    for (; s_seq < s_nchunks; s_seq++)
        if (chunk_run(s_seq, r)) return 1;
    return 0;
}

/** @brief Mark run r's sectors resident or unread and recompute the state of every chunk it touches, under s_mx.
 *
 * @param r The run.
 * @param ok 1 when the loader read it whole. */
static void mark_run(dc_run_t r, int ok) {
    memset(s_sec + r.s0, ok ? DC_RESIDENT : DC_UNREAD, r.s1 - r.s0);
    for (size_t c = r.s0 / DC_CHUNK_SECTORS; c <= (r.s1 - 1u) / DC_CHUNK_SECTORS; c++) {
        size_t lo = c * DC_CHUNK_SECTORS, hi = lo + DC_CHUNK_SECTORS;
        if (hi > s_nsec) hi = s_nsec;
        uint8_t st = DC_RESIDENT;
        for (size_t s = lo; s < hi && st != DC_MISSING; s++)
            if (s_sec[s] != DC_RESIDENT) st = s_sec[s];
        if (st == DC_RESIDENT && s_have[c] != DC_RESIDENT) s_have_n++;
        s_have[c] = st;
    }
}

/** @brief Hold the loader until the throttle's VBL-clock budget covers every byte charged so far.
 *
 * The budget is OPENPETE_DISC_CACHE_BPS bytes per second of VBL time
 * (2250000/37609 Hz) since VBL 0; the charge is every byte the loader has
 * read plus every byte a miss read from the file. Returns at once when
 * unthrottled or on shutdown.
 * @param bytes The run's byte count, added to the charge first. */
static void throttle(size_t bytes) {
    if (!s_bps) return;
    SDL_LockMutex(s_mx); s_acct += bytes; SDL_UnlockMutex(s_mx);
    for (;;) {
        SDL_LockMutex(s_mx);
        uint64_t acct = s_acct;
        int abort_now = s_abort;
        SDL_UnlockMutex(s_mx);
        int vbl = openpete_disc_trace_vbl();
        double budget = (double)s_bps * (vbl > 0 ? vbl : 0) * DC_VBL_DEN / DC_VBL_NUM;
        if (abort_now || budget >= (double)acct) return;
        SDL_Delay(1);
    }
}

/** @brief Loader thread: read every missing sector once, run by run in next_run order, then close the image.
 *
 * A run that stays short after DC_READ_TRIES attempts is marked DC_UNREAD
 * with its missing bytes zeroed; reads of it go to the file.
 * @param arg Unused.
 * @return 0. */
static int loader_main(void* arg) {
    OP_ROLE_BIND(disc);
    (void)arg;
    openpete_crash_thread_attach();
    for (;;) {
        dc_run_t r;
        SDL_LockMutex(s_mx);
        int more = !s_abort && next_run(&r);
        SDL_UnlockMutex(s_mx);
        if (!more) break;                       /* everything resident, or shutting down */
        int ok = read_run(r);
        throttle((r.s1 - r.s0) * DC_SECTOR);
        SDL_LockMutex(s_mx);
        mark_run(r, ok);
        SDL_UnlockMutex(s_mx);
        if (!ok)
            OPL_WARN_F("cd", "disc cache: short read of sectors [%zu,%zu) after %d attempts; "
                       "reads of them go to the file", r.s0, r.s1, DC_READ_TRIES);
    }
    fclose(s_fp); s_fp = NULL;
    s_load_ns = op_now() - s_t0;
    OPL_INFO_F("cd", "disc cache: %zu MB resident in %.2f s", s_size >> 20,
               (double)s_load_ns / 1e9);
    return 0;
}

/** @brief Read the loader's harness knobs: OPENPETE_DISC_CACHE_BPS (VBL-clock throttle) and OPENPETE_DISC_BOOT_ORDER=0 (profile off). */
static void read_loader_env(void) {
    const char* bps = getenv("OPENPETE_DISC_CACHE_BPS");
    s_bps = bps && bps[0] ? strtoull(bps, NULL, 10) : 0;
    const char* order = getenv("OPENPETE_DISC_BOOT_ORDER");
    if (order && strcmp(order, "0") == 0) s_order_n = 0;
    s_order_at = 0; s_acct = 0;
}

int openpete_disc_cache_attach(const char* bin_path) {
    if (s_active || !bin_path) return s_active;
    FILE* fp = fopen(bin_path, "rb");
    if (!fp) { OPL_WARN_F("cd", "disc cache: cannot open %s", bin_path); return 0; }
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return 0; }
    long long sz = ftell(fp);
    if (sz <= 0 || sz > 0x7FFFFFFFLL) {   /* PsyX_CDFS_Init_Mem takes an int size */
        OPL_WARN_F("cd", "disc cache: image size %lld unusable", sz); fclose(fp); return 0;
    }
    fseek(fp, 0, SEEK_SET);
    uint8_t* img = (uint8_t*)malloc((size_t)sz);
    if (!img) { OPL_WARN_F("cd", "disc cache: cannot allocate %lld bytes", sz); fclose(fp); return 0; }
    s_img = img; s_size = (size_t)sz; s_fp = fp; s_abort = 0;
    free(s_path);
    s_path = (char*)malloc(strlen(bin_path) + 1u);
    if (s_path) memcpy(s_path, bin_path, strlen(bin_path) + 1u);
    s_nsec = (s_size + DC_SECTOR - 1u) / DC_SECTOR;
    s_nchunks = (s_nsec + DC_CHUNK_SECTORS - 1u) / DC_CHUNK_SECTORS;
    s_have = (uint8_t*)calloc(s_nchunks, 1); s_have_n = 0; s_seq = 0;
    s_sec = (uint8_t*)calloc(s_nsec, 1);
    s_logged = (uint8_t*)calloc(s_nchunks, 1);
    for (int k = 0; k < DC_STEER_SLOTS; k++) s_want[k] = -1;
    read_loader_env();
    s_mx = SDL_CreateMutex();
    s_t0 = op_now();
    /* Readers consult s_active only after the loader state above exists. */
    s_active = 1;
    s_thr = SDL_CreateThread(loader_main, "openpete-disc", NULL);
    if (!s_thr) {
        /* No thread: load synchronously so the image is complete before use. */
        OPL_WARN_F("cd", "disc cache: SDL_CreateThread failed (%s); loading inline", SDL_GetError());
        loader_main(NULL);
    }
    OPL_INFO_F("boot", "disc cache: %s (%zu MB) loading in the background, boot order %zu "
               "range(s)%s, throttle %llu B/s", bin_path, s_size >> 20, s_order_n,
               s_order_n ? "" : " (file order)", (unsigned long long)s_bps);
    return 1;
}

int openpete_disc_cache_active(void) { return s_active; }

/** @brief Map a read path to its steer slot.
 *
 * The EXE read (with its volume descriptor and directory sectors) runs once
 * as the cache attaches and is never re-read, so it steers nothing.
 * @param tag An OPENPETE_DISC_TAG_* value.
 * @return DC_STEER_GAME for libcd, the extract and the WAD overlay;
 *         DC_STEER_XA for XA; -1 (no steer) for the EXE and every host
 *         path. */
static int steer_slot(int tag) {
    switch (tag) {
    case OPENPETE_DISC_TAG_LIBCD:
    case OPENPETE_DISC_TAG_EXTRACT:
    case OPENPETE_DISC_TAG_WAD:     return DC_STEER_GAME;
    case OPENPETE_DISC_TAG_XA:      return DC_STEER_XA;
    default:                        return -1;
    }
}

/** @brief Find the first sector of [s0, s1] not resident, under s_mx.
 *
 * Chunks wholly resident are skipped without a sector scan.
 * @param s0 First sector.
 * @param s1 Last sector (inclusive).
 * @return The sector, or -1 when every sector is resident. */
static long first_absent(size_t s0, size_t s1) {
    for (size_t s = s0; s <= s1;) {
        size_t c = s / DC_CHUNK_SECTORS;
        size_t c_end = (c + 1u) * DC_CHUNK_SECTORS;
        if (s_have[c] == DC_RESIDENT) { s = c_end; continue; }
        for (; s <= s1 && s < c_end; s++)
            if (s_sec[s] != DC_RESIDENT) return (long)s;
    }
    return -1;
}

int openpete_disc_cache_copy(const char* bin_path, uint64_t offset, void* dst,
                             size_t len, int tag) {
    if (!s_active || !s_path || !bin_path || strcmp(bin_path, s_path) != 0) return -1;
    if (offset >= s_size || len > s_size - offset) return -1;
    if (len == 0) return 1;
    SDL_LockMutex(s_mx);
    long miss = first_absent((size_t)(offset / DC_SECTOR), (size_t)((offset + len - 1u) / DC_SECTOR));
    if (miss >= 0) {
        size_t c = (size_t)miss / DC_CHUNK_SECTORS;
        int slot = steer_slot(tag);
        if (slot >= 0 && s_have[c] == DC_MISSING && !s_abort) s_want[slot] = (long)c;
        SDL_UnlockMutex(s_mx);
        return 0;
    }
    SDL_UnlockMutex(s_mx);
    /* A resident sector is never written again, and the mutex orders the
     * loader's fill before its DC_RESIDENT mark. */
    memcpy(dst, s_img + offset, len);
    return 1;
}

void openpete_disc_cache_miss_served(int tag, uint64_t offset, size_t len, int64_t ns) {
    if (!s_active) return;
    size_t c = (size_t)(offset / DC_CHUNK);
    if (c >= s_nchunks) return;
    uint64_t s0 = offset / DC_SECTOR, s1 = (offset + len + DC_SECTOR - 1u) / DC_SECTOR;
    SDL_LockMutex(s_mx);
    s_misses++;
    if (tag >= 0 && tag < OPENPETE_DISC_TAG_COUNT) s_misses_by_tag[tag]++;
    s_direct_ns += ns;
    s_acct += (s1 - s0) * DC_SECTOR;           /* a direct read shares the throttled device */
    int first = !s_logged[c];
    s_logged[c] = 1;
    size_t have = s_have_n;
    SDL_UnlockMutex(s_mx);
    openpete_pace_cd_miss(ns, offset + len);
    if (first)
        OPL_WARN_F("cd", "disc cache miss: %s read [%llu,%llu) in chunk %zu served from the file "
                   "in %.2f ms (%zu/%zu chunks resident)", openpete_disc_trace_tag_name(tag),
                   (unsigned long long)offset, (unsigned long long)(offset + len), c,
                   (double)ns / 1e6, have, s_nchunks);
}

void openpete_disc_cache_shutdown(void) {
    if (!s_active) return;
    SDL_LockMutex(s_mx); s_abort = 1; SDL_UnlockMutex(s_mx);
    if (s_thr) { SDL_WaitThread(s_thr, NULL); s_thr = NULL; }
    /* The image stays: a resident sector still serves a late read, and the OS
     * reclaims it at exit. */
}

/* PsyCross's image reads, bound to one disc_image handle. The result of
 * the last read is kept for the read observer, which runs right after it
 * on the same thread. */
static disc_image_t* s_psx_img;
static int           s_psx_result = OPENPETE_DISC_DIRECT;

/** @brief PsyCross image read: serve [b, e) into dst through the disc_image handle.
 *
 * @param dst Receives e - b bytes.
 * @param b First byte offset.
 * @param e One past the last byte offset.
 * @param source PSYX_CDFS_READ_DATA or PSYX_CDFS_READ_XA.
 * @return 1: the bytes are always served here. */
static int psycross_read(void* dst, size_t b, size_t e, int source) {
    int tag = source == PSYX_CDFS_READ_XA ? OPENPETE_DISC_TAG_XA : OPENPETE_DISC_TAG_LIBCD;
    int r = disc_image_read_bytes(s_psx_img, b, dst, e > b ? e - b : 0, tag);
    s_psx_result = r < 0 ? OPENPETE_DISC_DIRECT : r;
    return 1;
}

int openpete_disc_psycross_bind(disc_image_t* img) {
    uint64_t size = img ? disc_image_size(img) : 0;
    if (!size || size > 0x7FFFFFFFu) return 0;   /* PsyX_CDFS_Init_Mem takes an int size */
    s_psx_img = img;
    PsyX_CDFS_SetMemRead(psycross_read);
    PsyX_CDFS_Init_Mem(NULL, (int)size, /*track=*/0, /*sectorSize=*/0);
    return 1;
}

/** @brief PsyCross read observer: trace one image read as libcd or xa with the result psycross_read recorded.
 *
 * @param b First byte offset.
 * @param e One past the last byte offset.
 * @param source PSYX_CDFS_READ_DATA or PSYX_CDFS_READ_XA. */
static void trace_psycross_read(size_t b, size_t e, int source) {
    int tag = source == PSYX_CDFS_READ_XA ? OPENPETE_DISC_TAG_XA : OPENPETE_DISC_TAG_LIBCD;
    openpete_disc_trace_bytes(tag, b, e > b ? e - b : 0, s_psx_result);
}

void openpete_disc_cache_trace_install(void) {
    if (openpete_disc_trace_enabled()) PsyX_CDFS_SetReadObserver(trace_psycross_read);
}

uint64_t openpete_disc_cache_tag_misses(int tag) {
    return tag >= 0 && tag < OPENPETE_DISC_TAG_COUNT ? s_misses_by_tag[tag] : 0;
}

void openpete_disc_cache_census(uint64_t* misses, int64_t* direct_ns,
                                size_t* bytes, int64_t* load_ns) {
    if (misses)    *misses    = s_misses;
    if (direct_ns) *direct_ns = s_direct_ns;
    if (bytes)     *bytes     = s_size;
    if (load_ns)   *load_ns   = s_load_ns;
}
