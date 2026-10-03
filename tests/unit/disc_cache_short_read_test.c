/* tests/unit/disc_cache_short_read_test.c - a chunk the loader cannot read
 * whole is never served as uninitialised heap bytes.
 *
 * disc_cache.c is compiled into this TU with fread and malloc replaced: the
 * allocation is filled with 0xCD, and reads of chunk 1 come up short on
 * demand. Case A: the first read of chunk 1 is short and the retry reads it
 * whole; the image matches the file. Case B: every loader read of chunk 1
 * is short; a guest read of it triggers the re-read, and when that is short
 * too the missing bytes read as zero, never 0xCD. */
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

static void (*s_wait)(size_t, size_t);
static const uint8_t* s_mem;

/** @brief PsyCross stub: records the miss-wait callback. */
void PsyX_CDFS_SetMemWait(void (*cb)(size_t, size_t)) { s_wait = cb; }
/** @brief PsyCross stub: records the image pointer. */
void PsyX_CDFS_Init_Mem(const u_int* data, int size, int track, int sector_size) {
    (void)size; (void)track; (void)sector_size; s_mem = (const uint8_t*)data;
}
/** @brief PsyCross stub: the read observer is not exercised here. */
void PsyX_CDFS_SetReadObserver(void (*cb)(size_t, size_t, int)) { (void)cb; }
/** @brief Pace log stub. */
void openpete_pace_cd_miss(int64_t waited_ns, uint64_t end_offset) { (void)waited_ns; (void)end_offset; }
void openpete_crash_thread_attach(void) {}
/** @brief Logger stub. */
void openpete_logf(openpete_log_level_t level, const char* subsystem, const char* fmt, ...) {
    (void)level; (void)subsystem; (void)fmt;
}

/** @brief Resets the module's statics between cases. */
static void dc_reset(void) {
    openpete_disc_cache_shutdown();
    s_active = 0; s_img = NULL; s_have = NULL; s_thr = NULL;
}

/** @brief Runs the retry case and the persistent short-read case.
 * @return 0 when both pass. */
int main(void) {
    const char* tmp = getenv("TMPDIR");
    char path[512];
    snprintf(path, sizeof path, "%s/disc_cache_short_read_test.bin", tmp && *tmp ? tmp : "/tmp");
    const size_t size = 2u * DC_CHUNK + 4096u;
    uint8_t* file = (uint8_t*)calloc(1, size);
    for (size_t i = 0; i < size; i++) file[i] = (uint8_t)(i * 7u + 1u);
    FILE* f = fopen(path, "wb");
    fwrite(file, 1, size, f); fclose(f);
    s_short_at = DC_CHUNK;
    int fail = 0;

    s_short_left = 1;
    if (!openpete_disc_cache_attach(path)) { fprintf(stderr, "FAIL attach A\n"); return 1; }
    s_wait(0, size);
    if (memcmp(s_mem, file, size) != 0) { fprintf(stderr, "FAIL A: image differs after a retried short read\n"); fail++; }
    dc_reset();

    s_short_left = -1;              /* every read of chunk 1 is short */
    if (!openpete_disc_cache_attach(path)) { fprintf(stderr, "FAIL attach B\n"); return 1; }
    s_wait(0, size);
    const size_t half = DC_CHUNK / 2u;
    if (memcmp(s_mem + DC_CHUNK, file + DC_CHUNK, half) != 0) { fprintf(stderr, "FAIL B: read half wrong\n"); fail++; }
    size_t cd = 0;
    for (size_t i = DC_CHUNK + half; i < 2u * DC_CHUNK; i++) cd += (s_mem[i] == 0xCD);
    int zero = 1;
    for (size_t i = DC_CHUNK + half; i < 2u * DC_CHUNK && zero; i++) zero = (s_mem[i] == 0);
    if (cd || !zero) { fprintf(stderr, "FAIL B: unread half not zero (%zu stale 0xCD bytes)\n", cd); fail++; }
    if (memcmp(s_mem + 2u * DC_CHUNK, file + 2u * DC_CHUNK, 4096u) != 0) { fprintf(stderr, "FAIL B: chunk 2\n"); fail++; }

    s_short_left = 0;               /* the file reads whole again: the re-read recovers */
    s_wait(DC_CHUNK, DC_CHUNK + 16u);
    if (memcmp(s_mem + DC_CHUNK, file + DC_CHUNK, DC_CHUNK) != 0) { fprintf(stderr, "FAIL B: re-read did not recover\n"); fail++; }
    dc_reset();
    remove(path);
    free(file);
    if (fail) return 1;
    printf("disc_cache_short_read_test: OK\n");
    return 0;
}
