/* tests/unit/disc_cache_short_read_test.c - a chunk the loader cannot read
 * whole is never served as uninitialised heap bytes.
 *
 * disc_cache.c is compiled into this TU with fread and malloc replaced: the
 * allocation is filled with 0xCD, and the loader's reads of chunk 1 come up
 * short on demand. Case A: the first read of chunk 1 is short and the retry
 * reads it whole; every byte of the image is then served from memory and
 * matches the file. Case B: every loader read of chunk 1 is short; the chunk
 * is never served from memory, and a disc_image read of it is a miss whose
 * bytes come from the file, never 0xCD. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int s_short_left;       /* short reads still to inject on chunk 1 */
static size_t s_short_at;      /* file offset of chunk 1 */

/** @brief fread that returns half the request for a read starting at chunk 1 while s_short_left > 0. */
static size_t dc_test_fread(void* p, size_t sz, size_t n, FILE* f) {
    if (s_short_left != 0 && (size_t)ftell(f) == s_short_at && sz == 1) {
        if (s_short_left > 0) s_short_left--;
        return fread(p, 1, n / 2, f);
    }
    return fread(p, sz, n, f);
}
/** @brief malloc that fills the block with 0xCD, standing in for stale heap contents. */
static void* dc_test_malloc(size_t n) {
    void* p = malloc(n);
    if (p) memset(p, 0xCD, n);
    return p;
}
#define fread dc_test_fread
#define malloc dc_test_malloc
#include "../../core/psx/disc_cache.c"
#undef fread
#undef malloc

/** @brief PsyCross stub: the read callback is not exercised here. */
void PsyX_CDFS_SetMemRead(int (*cb)(void*, size_t, size_t, int)) { (void)cb; }
/** @brief PsyCross stub: the image declaration is not exercised here. */
void PsyX_CDFS_Init_Mem(const u_int* data, int size, int track, int sector_size) {
    (void)data; (void)size; (void)track; (void)sector_size;
}
/** @brief PsyCross stub: the read observer is not exercised here. */
void PsyX_CDFS_SetReadObserver(void (*cb)(size_t, size_t, int)) { (void)cb; }
/** @brief Pace log stub. */
void openpete_pace_cd_miss(int64_t waited_ns, uint64_t end_offset) { (void)waited_ns; (void)end_offset; }
/** @brief Crash handler stub. */
void openpete_crash_thread_attach(void) {}
/** @brief Logger stub. */
void openpete_logf(openpete_log_level_t level, const char* subsystem, const char* fmt, ...) {
    (void)level; (void)subsystem; (void)fmt;
}

/** @brief Wait for the loader to finish every chunk. */
static void dc_join(void) {
    if (s_thr) { SDL_WaitThread(s_thr, NULL); s_thr = NULL; }
}

/** @brief Resets the module's statics between cases. */
static void dc_reset(void) {
    openpete_disc_cache_shutdown();
    s_active = 0; s_img = NULL; s_have = NULL; s_thr = NULL; s_misses = 0;
}

/** @brief Runs the retry case and the persistent short-read case.
 * @return 0 when both pass. */
int main(void) {
    const char* tmp = getenv("TMPDIR");
    char path[512];
    snprintf(path, sizeof path, "%s/disc_cache_short_read_test.bin", tmp && *tmp ? tmp : "/tmp");
    /* A whole number of 2352-byte sectors past two chunks, so disc_image opens it. */
    const size_t size = ((2u * DC_CHUNK) / 2352u + 4u) * 2352u;
    uint8_t* file = (uint8_t*)calloc(1, size);
    uint8_t* got = (uint8_t*)calloc(1, size);
    for (size_t i = 0; i < size; i++) file[i] = (uint8_t)(i * 7u + 1u);
    FILE* f = fopen(path, "wb");
    fwrite(file, 1, size, f); fclose(f);
    s_short_at = DC_CHUNK;
    int fail = 0;

    disc_image_t* img = disc_image_open(path, DISC_READ_CACHED, OPENPETE_DISC_TAG_LIBCD);
    if (!img) { fprintf(stderr, "FAIL open\n"); return 1; }

    s_short_left = 1;
    if (!openpete_disc_cache_attach(disc_image_bin_path(img))) { fprintf(stderr, "FAIL attach A\n"); return 1; }
    dc_join();
    if (openpete_disc_cache_copy(disc_image_bin_path(img), 0, got, size, OPENPETE_DISC_TAG_LIBCD) != 1)
        { fprintf(stderr, "FAIL A: image not resident after a retried short read\n"); fail++; }
    else if (memcmp(got, file, size) != 0)
        { fprintf(stderr, "FAIL A: image differs after a retried short read\n"); fail++; }
    dc_reset();

    s_short_left = -1;              /* every loader read of chunk 1 is short */
    if (!openpete_disc_cache_attach(disc_image_bin_path(img))) { fprintf(stderr, "FAIL attach B\n"); return 1; }
    dc_join();
    if (s_have[1] != DC_UNREAD) { fprintf(stderr, "FAIL B: chunk 1 state %u\n", s_have[1]); fail++; }
    if (openpete_disc_cache_copy(disc_image_bin_path(img), DC_CHUNK, got, 16u, OPENPETE_DISC_TAG_LIBCD) != 0)
        { fprintf(stderr, "FAIL B: an unread chunk was served from memory\n"); fail++; }
    memset(got, 0xCD, size);
    int r = disc_image_read_bytes(img, 0, got, size, OPENPETE_DISC_TAG_LIBCD);
    if (r != OPENPETE_DISC_MISS) { fprintf(stderr, "FAIL B: read result %d, want miss\n", r); fail++; }
    if (memcmp(got, file, size) != 0) { fprintf(stderr, "FAIL B: bytes differ from the file\n"); fail++; }
    if (s_misses != 1) { fprintf(stderr, "FAIL B: %llu misses counted, want 1\n", (unsigned long long)s_misses); fail++; }
    r = disc_image_read_bytes(img, 0, got, 4096u, OPENPETE_DISC_TAG_LIBCD);
    if (r != OPENPETE_DISC_HIT) { fprintf(stderr, "FAIL B: chunk 0 read result %d, want hit\n", r); fail++; }
    dc_reset();
    disc_image_close(img);
    remove(path);
    free(file); free(got);
    if (fail) return 1;
    printf("disc_cache_short_read_test: OK\n");
    return 0;
}
