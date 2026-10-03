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

/* The image is filled in chunks. The loader walks them in file order, but a
 * chunk a read missed on is loaded next: the game's own chunk first, then
 * the XA stream's, then file order. A read never waits on the loader; the
 * reader serves a miss from the file. */
#define DC_CHUNK_SHIFT 23u                    /* 8 MB */
#define DC_CHUNK       ((size_t)1 << DC_CHUNK_SHIFT)

/* Steer slots, in the loader's priority order. Host paths (the woah NSF,
 * the library scan, the ingest copy, the sfx census, mods) have none: a
 * host read never reorders the loader ahead of the game. */
enum { DC_STEER_GAME = 0, DC_STEER_XA, DC_STEER_SLOTS };

static uint8_t*       s_img;
static size_t         s_size;
static size_t         s_nchunks;
static uint8_t*       s_have;          /* per-chunk DC_MISSING / DC_RESIDENT / DC_UNREAD (under s_mx) */
static uint8_t*       s_logged;        /* per-chunk: a miss on it was logged (under s_mx) */
static size_t         s_have_n;        /* chunks resident */
static long           s_want[DC_STEER_SLOTS]; /* chunk each slot steers to, -1 = none */
static size_t         s_seq;           /* next chunk in file order */
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

/* s_have states. A DC_UNREAD chunk came up short in the loader after
 * DC_READ_TRIES attempts; it is never served from memory, so every read of
 * it goes to the file. */
#define DC_MISSING   0u
#define DC_RESIDENT  1u
#define DC_UNREAD    2u
#define DC_READ_TRIES 3

/** @brief Read chunk c of the image from fp into s_img; bytes the read does not supply are zeroed.
 *
 * @param fp An open handle on the image.
 * @param c Chunk index.
 * @return 1 when the whole chunk was read, 0 on a seek failure or a short read. */
static int read_chunk_from(FILE* fp, size_t c) {
    size_t at = c << DC_CHUNK_SHIFT;
    size_t want = s_size - at; if (want > DC_CHUNK) want = DC_CHUNK;
    size_t got = 0;
    if (fseek(fp, (long)at, SEEK_SET) == 0) got = fread(s_img + at, 1, want, fp);
    openpete_disc_trace_bytes(OPENPETE_DISC_TAG_CACHE_LOAD, at, got, OPENPETE_DISC_DIRECT);
    if (got < want) memset(s_img + at + got, 0, want - got);
    return got == want;
}

/** @brief Read chunk c through the loader's handle, retrying a short read up to DC_READ_TRIES times.
 *
 * @param c Chunk index.
 * @return 1 when the chunk was read whole. */
static int read_chunk(size_t c) {
    for (int t = 0; t < DC_READ_TRIES; t++)
        if (read_chunk_from(s_fp, c)) return 1;
    return 0;
}

/** @brief Pick the loader's next chunk under s_mx: each steer slot's chunk in slot order, then file order.
 *
 * @return The chunk, or -1 when every chunk is done. */
static long next_chunk(void) {
    for (int k = 0; k < DC_STEER_SLOTS; k++) {
        long w = s_want[k];
        if (w >= 0 && s_have[w] == DC_MISSING) return w;
        s_want[k] = -1;
    }
    while (s_seq < s_nchunks && s_have[s_seq] != DC_MISSING) s_seq++;
    return s_seq < s_nchunks ? (long)s_seq : -1;
}

/** @brief Loader thread: read every chunk once, steered chunks first, then close the image.
 *
 * A chunk that stays short after DC_READ_TRIES attempts is marked DC_UNREAD
 * with its missing bytes zeroed; reads of it go to the file.
 * @param arg Unused.
 * @return 0. */
static int loader_main(void* arg) {
    (void)arg;
    openpete_crash_thread_attach();
    for (;;) {
        SDL_LockMutex(s_mx);
        long c = s_abort ? -1 : next_chunk();
        SDL_UnlockMutex(s_mx);
        if (c < 0) break;                       /* everything resident, or shutting down */
        int ok = read_chunk((size_t)c);
        SDL_LockMutex(s_mx);
        if (ok) { s_have[c] = DC_RESIDENT; s_have_n++; }
        else {
            OPL_WARN_F("cd", "disc cache: short read of chunk %ld after %d attempts; reads of it "
                       "go to the file", c, DC_READ_TRIES);
            s_have[c] = DC_UNREAD;
        }
        SDL_UnlockMutex(s_mx);
    }
    fclose(s_fp); s_fp = NULL;
    s_load_ns = op_now() - s_t0;
    OPL_INFO_F("cd", "disc cache: %zu MB resident in %.2f s", s_size >> 20,
               (double)s_load_ns / 1e9);
    return 0;
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
    s_nchunks = (s_size + DC_CHUNK - 1) >> DC_CHUNK_SHIFT;
    s_have = (uint8_t*)calloc(s_nchunks, 1); s_have_n = 0; s_seq = 0;
    s_logged = (uint8_t*)calloc(s_nchunks, 1);
    for (int k = 0; k < DC_STEER_SLOTS; k++) s_want[k] = -1;
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
    OPL_INFO_F("boot", "disc cache: %s (%zu MB) loading in the background", bin_path, s_size >> 20);
    return 1;
}

int openpete_disc_cache_active(void) { return s_active; }

/** @brief Map a read path to its steer slot.
 *
 * @param tag An OPENPETE_DISC_TAG_* value.
 * @return DC_STEER_GAME for libcd, the EXE, the extract and the WAD
 *         overlay; DC_STEER_XA for XA; -1 (no steer) for every host path. */
static int steer_slot(int tag) {
    switch (tag) {
    case OPENPETE_DISC_TAG_LIBCD:
    case OPENPETE_DISC_TAG_EXE:
    case OPENPETE_DISC_TAG_EXTRACT:
    case OPENPETE_DISC_TAG_WAD:     return DC_STEER_GAME;
    case OPENPETE_DISC_TAG_XA:      return DC_STEER_XA;
    default:                        return -1;
    }
}

int openpete_disc_cache_copy(const char* bin_path, uint64_t offset, void* dst,
                             size_t len, int tag) {
    if (!s_active || !s_path || !bin_path || strcmp(bin_path, s_path) != 0) return -1;
    if (offset >= s_size || len > s_size - offset) return -1;
    if (len == 0) return 1;
    size_t c0 = (size_t)(offset >> DC_CHUNK_SHIFT);
    size_t c1 = (size_t)((offset + len - 1u) >> DC_CHUNK_SHIFT);
    SDL_LockMutex(s_mx);
    long miss = -1;
    for (size_t c = c0; c <= c1; c++)
        if (s_have[c] != DC_RESIDENT) { miss = (long)c; break; }
    if (miss >= 0) {
        int slot = steer_slot(tag);
        if (slot >= 0 && s_have[miss] == DC_MISSING && !s_abort) s_want[slot] = miss;
        SDL_UnlockMutex(s_mx);
        return 0;
    }
    SDL_UnlockMutex(s_mx);
    /* A resident chunk is never written again, and the mutex orders the
     * loader's fill before its DC_RESIDENT mark. */
    memcpy(dst, s_img + offset, len);
    return 1;
}

void openpete_disc_cache_miss_served(int tag, uint64_t offset, size_t len, int64_t ns) {
    if (!s_active) return;
    size_t c = (size_t)(offset >> DC_CHUNK_SHIFT);
    if (c >= s_nchunks) return;
    SDL_LockMutex(s_mx);
    s_misses++;
    if (tag >= 0 && tag < OPENPETE_DISC_TAG_COUNT) s_misses_by_tag[tag]++;
    s_direct_ns += ns;
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
    /* The image stays: a resident chunk still serves a late read, and the OS
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
