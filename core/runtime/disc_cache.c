/* core/runtime/disc_cache.c — see openpete_disc_cache.h. */
#include "openpete_disc_cache.h"
#include "openpete_log.h"
#include "openpete_time.h"
#include "openpete_pace_log.h"
#include "psx/libcd.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The image is filled in chunks. The loader walks them in file order, but
 * a chunk a guest read is blocked on is served next: a boot-time read far
 * into the image (the EXE, the first WAD entries) then waits for one chunk,
 * not for everything before it. */
#define DC_CHUNK_SHIFT 23u                    /* 8 MB */
#define DC_CHUNK       ((size_t)1 << DC_CHUNK_SHIFT)

static uint8_t*       s_img;
static size_t         s_size;
static size_t         s_nchunks;
static uint8_t*       s_have;          /* per-chunk resident flag (under s_mx) */
static size_t         s_have_n;        /* chunks resident */
static long           s_want;          /* chunk a reader is blocked on, -1 = none */
static size_t         s_seq;           /* next chunk in file order */
static SDL_Mutex*     s_mx;
static SDL_Condition* s_cv;            /* readers wait; loader broadcasts */
static SDL_Condition* s_cv_loader;     /* loader waits for a want / abort */
static int            s_abort;
static int            s_active;
static SDL_Thread*    s_thr;
static FILE*          s_fp;
static int64_t        s_t0, s_load_ns;
static uint64_t       s_misses;
static int64_t        s_waited_ns;

static int read_chunk(size_t c) {
    size_t at = c << DC_CHUNK_SHIFT;
    size_t want = s_size - at; if (want > DC_CHUNK) want = DC_CHUNK;
    if (fseek(s_fp, (long)at, SEEK_SET) != 0) return 0;
    size_t got = fread(s_img + at, 1, want, s_fp);
    return got == want;
}

static int loader_main(void* arg) {
    (void)arg;
    for (;;) {
        SDL_LockMutex(s_mx);
        long c = -1;
        if (s_abort) { SDL_UnlockMutex(s_mx); break; }
        if (s_want >= 0 && !s_have[s_want]) c = s_want;
        else {
            while (s_seq < s_nchunks && s_have[s_seq]) s_seq++;
            if (s_seq < s_nchunks) c = (long)s_seq;
        }
        SDL_UnlockMutex(s_mx);
        if (c < 0) break;                       /* everything resident */
        int ok = read_chunk((size_t)c);
        SDL_LockMutex(s_mx);
        if (ok) { s_have[c] = 1; s_have_n++; }
        else {
            OPL_WARN_F("cd", "disc cache: short read of chunk %ld; falling back to the file for it", c);
            s_have[c] = 1; s_have_n++;         /* do not spin; the bytes are whatever the read left */
        }
        if (s_want == c) s_want = -1;
        SDL_BroadcastCondition(s_cv);
        SDL_UnlockMutex(s_mx);
    }
    fclose(s_fp); s_fp = NULL;
    s_load_ns = op_now() - s_t0;
    OPL_INFO_F("cd", "disc cache: %zu MB resident in %.2f s", s_size >> 20,
               (double)s_load_ns / 1e9);
    return 0;
}

static int range_resident(size_t b, size_t e, size_t* first_missing) {
    size_t c0 = b >> DC_CHUNK_SHIFT, c1 = (e ? e - 1 : 0) >> DC_CHUNK_SHIFT;
    for (size_t c = c0; c <= c1 && c < s_nchunks; c++)
        if (!s_have[c]) { *first_missing = c; return 0; }
    return 1;
}

/* The wait: runs on whichever thread reads the image (the sim thread). */
static void mem_wait(size_t b, size_t e) {
    if (e > s_size) e = s_size;
    if (b >= e) return;
    SDL_LockMutex(s_mx);
    size_t miss;
    if (range_resident(b, e, &miss) || s_abort) { SDL_UnlockMutex(s_mx); return; }
    int64_t t0 = op_now();
    while (!s_abort && !range_resident(b, e, &miss)) {
        s_want = (long)miss;                    /* steer the loader here */
        SDL_BroadcastCondition(s_cv_loader);
        SDL_WaitCondition(s_cv, s_mx);
    }
    int64_t waited = op_now() - t0;
    size_t have = s_have_n;
    s_misses++; s_waited_ns += waited;
    SDL_UnlockMutex(s_mx);
    openpete_pace_cd_miss(waited, (uint64_t)e);
    OPL_WARN_F("cd", "disc cache miss: read [%zu,%zu) waited %.2f ms (%zu/%zu chunks resident)",
               b, e, (double)waited / 1e6, have, s_nchunks);
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
    s_nchunks = (s_size + DC_CHUNK - 1) >> DC_CHUNK_SHIFT;
    s_have = (uint8_t*)calloc(s_nchunks, 1); s_have_n = 0; s_want = -1; s_seq = 0;
    s_mx = SDL_CreateMutex(); s_cv = SDL_CreateCondition(); s_cv_loader = SDL_CreateCondition();
    s_t0 = op_now();
    PsyX_CDFS_SetMemWait(mem_wait);
    PsyX_CDFS_Init_Mem((const u_int*)img, (int)sz, /*track=*/0, /*sectorSize=*/0);
    s_thr = SDL_CreateThread(loader_main, "openpete-disc", NULL);
    if (!s_thr) {
        /* No thread: load synchronously so the image is complete before use. */
        OPL_WARN_F("cd", "disc cache: SDL_CreateThread failed (%s); loading inline", SDL_GetError());
        loader_main(NULL);
    }
    s_active = 1;
    OPL_INFO_F("boot", "disc cache: %s (%zu MB) loading in the background", bin_path, s_size >> 20);
    return 1;
}

int openpete_disc_cache_active(void) { return s_active; }

void openpete_disc_cache_shutdown(void) {
    if (!s_active) return;
    SDL_LockMutex(s_mx); s_abort = 1;
    SDL_BroadcastCondition(s_cv); SDL_BroadcastCondition(s_cv_loader);
    SDL_UnlockMutex(s_mx);
    if (s_thr) { SDL_WaitThread(s_thr, NULL); s_thr = NULL; }
    PsyX_CDFS_SetMemWait(NULL);
    /* The image itself stays: PsyCross still holds the pointer until the
     * process ends, and the OS reclaims it. */
}

void openpete_disc_cache_census(uint64_t* misses, int64_t* waited_ns,
                                size_t* bytes, int64_t* load_ns) {
    if (misses)    *misses    = s_misses;
    if (waited_ns) *waited_ns = s_waited_ns;
    if (bytes)     *bytes     = s_size;
    if (load_ns)   *load_ns   = s_load_ns;
}
