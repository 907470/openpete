/* tests/unit/disc_image_test.c - the disc reader on synthetic images.
 *
 * Writes a raw (2352-byte sector) ISO 9660 image: the primary volume
 * descriptor at sector 16, the terminator at 17, the root directory at 18
 * with SYSTEM.CNF, a file whose name has no extension ("NOEXT.;1") and a
 * subdirectory S0 at 19 holding S000000D.NSF, a two-sector file whose
 * second sector is Mode 1 (user data at byte 16, not 24). Checks: lookup at
 * the root and one level down, case folding, the ";1" suffix, separators
 * '/' and '\', absent names; file and sector reads against the bytes
 * written; the serial reader and the file loader the woah NSF uses on the
 * same image; a 2048-byte image and an odd-sized one refused; with the disc
 * cache attached, a cached handle hits and a direct handle reads direct;
 * the booted-disc read_sector the mod API forwards to, and that slot's
 * place at the end of the api table. */
#include "openpete_disc_cache.h"
#include "openpete_disc_image.h"
#include "openpete_disc_trace.h"
#include "openpete_log.h"
#include "openpete_mod_api.h"
#include "openpete_psx.h"
#include "psx/libcd.h"

#include <SDL3/SDL.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define SECTORS 24u
#define NSF_SIZE 3000u
static const char k_cnf[] = "BOOT = cdrom:\\SCUS_942.28;1\r\nTCB = 4\r\n";

static uint8_t s_user[SECTORS][2048];   /* user data per sector */
static uint8_t s_mode[SECTORS];         /* 1 or 2 */
static int s_fail;

/** @brief Report a failed check.
 *
 * @param ok The check.
 * @param what Its description. */
static void check(int ok, const char* what) {
    if (!ok) { fprintf(stderr, "FAIL %s\n", what); s_fail++; }
}

/** @brief Store a both-endian 32-bit field (ECMA-119 7.3.3).
 *
 * @param p Eight bytes.
 * @param v The value. */
static void put_both32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) { p[i] = (uint8_t)(v >> (8 * i)); p[7 - i] = (uint8_t)(v >> (8 * i)); }
}

/** @brief Write one directory record.
 *
 * @param p Destination.
 * @param name Identifier bytes.
 * @param nl Identifier length.
 * @param lba Extent sector.
 * @param size Extent length.
 * @param flags File flags (2 = directory).
 * @return The record length. */
static int dir_record(uint8_t* p, const char* name, int nl, uint32_t lba, uint32_t size, uint8_t flags) {
    const int len = 33 + nl + ((33 + nl) & 1);
    p[0] = (uint8_t)len;
    put_both32(p + 2, lba);
    put_both32(p + 10, size);
    p[25] = flags;
    p[32] = (uint8_t)nl;
    memcpy(p + 33, name, (size_t)nl);
    return len;
}

/** @brief Fill s_user with the volume: descriptors, root, S0, SYSTEM.CNF, the NSF and NOEXT. */
static void build_volume(void) {
    memset(s_user, 0, sizeof s_user);
    for (uint32_t i = 0; i < SECTORS; i++) s_mode[i] = 2;
    uint8_t* pvd = s_user[16];
    pvd[0] = 1; memcpy(pvd + 1, "CD001", 5); pvd[6] = 1;
    dir_record(pvd + 156, "\0", 1, 18, 2048, 2);
    uint8_t* term = s_user[17];
    term[0] = 255; memcpy(term + 1, "CD001", 5); term[6] = 1;
    uint8_t* root = s_user[18];
    int o = dir_record(root, "\0", 1, 18, 2048, 2);
    o += dir_record(root + o, "\1", 1, 18, 2048, 2);
    o += dir_record(root + o, "NOEXT.;1", 8, 23, 5, 0);
    o += dir_record(root + o, "S0", 2, 19, 2048, 2);
    dir_record(root + o, "SYSTEM.CNF;1", 12, 20, (uint32_t)strlen(k_cnf), 0);
    uint8_t* s0 = s_user[19];
    o = dir_record(s0, "\0", 1, 19, 2048, 2);
    o += dir_record(s0 + o, "\1", 1, 18, 2048, 2);
    dir_record(s0 + o, "S000000D.NSF;1", 14, 21, NSF_SIZE, 0);
    memcpy(s_user[20], k_cnf, strlen(k_cnf));
    for (uint32_t i = 0; i < NSF_SIZE; i++) s_user[21 + i / 2048][i % 2048] = (uint8_t)(i * 13u + 5u);
    s_mode[22] = 1;
    memcpy(s_user[23], "hello", 5);
}

/** @brief Build the raw sector the image holds at lba.
 *
 * @param lba The sector.
 * @param raw Receives 2352 bytes: sync, header with the mode byte, a zero
 *        subheader for Mode 2, the user data, zero EDC/ECC. */
static void raw_sector(uint32_t lba, uint8_t raw[2352]) {
    memset(raw, 0, 2352);
    memset(raw + 1, 0xFF, 10);
    raw[12] = (uint8_t)lba;     /* address bytes: not read by the reader */
    raw[15] = s_mode[lba];
    memcpy(raw + (s_mode[lba] == 1 ? 16 : 24), s_user[lba], 2048);
}

/** @brief Write the volume as a raw image.
 *
 * @param path Output path. */
static void write_raw(const char* path) {
    FILE* f = fopen(path, "wb");
    uint8_t raw[2352];
    for (uint32_t i = 0; i < SECTORS; i++) { raw_sector(i, raw); fwrite(raw, 1, sizeof raw, f); }
    fclose(f);
}

/** @brief Write @p bytes zero bytes.
 *
 * @param path Output path.
 * @param bytes Size. */
static void write_zero(const char* path, size_t bytes) {
    FILE* f = fopen(path, "wb");
    uint8_t* z = (uint8_t*)calloc(1, bytes);
    fwrite(z, 1, bytes, f);
    fclose(f);
    free(z);
}

/** @brief Look up path and compare the record found.
 *
 * @param img The handle.
 * @param path Lookup path.
 * @param lba Expected sector, or 0 for an expected miss.
 * @param size Expected size. */
static void expect_find(disc_image_t* img, const char* path, uint32_t lba, uint32_t size) {
    disc_image_entry_t e;
    int r = disc_image_find(img, path, &e);
    char what[160];
    snprintf(what, sizeof what, "find %s", path);
    if (!lba) { check(r != 0, what); return; }
    check(r == 0 && e.lba == lba && e.size == size, what);
}

/** @brief Lookup cases on the volume.
 *
 * @param img The handle. */
static void lookup_cases(disc_image_t* img) {
    expect_find(img, "SYSTEM.CNF", 20, (uint32_t)strlen(k_cnf));
    expect_find(img, "system.cnf;1", 20, (uint32_t)strlen(k_cnf));
    expect_find(img, "\\SYSTEM.CNF", 20, (uint32_t)strlen(k_cnf));
    expect_find(img, "S0/S000000D.NSF", 21, NSF_SIZE);
    expect_find(img, "s0\\s000000d.nsf;1", 21, NSF_SIZE);
    expect_find(img, "NOEXT", 23, 5);
    expect_find(img, "S0", 19, 2048);
    expect_find(img, "MISSING.BIN", 0, 0);
    expect_find(img, "S0/MISSING", 0, 0);
    expect_find(img, "SYSTEM.CNF/X", 0, 0);
}

/** @brief Read cases: file across a Mode 2 and a Mode 1 sector, raw sectors, a sector past the end.
 *
 * @param img The handle. */
static void read_cases(disc_image_t* img) {
    disc_image_entry_t e;
    uint8_t buf[NSF_SIZE];
    uint8_t want[NSF_SIZE];
    for (uint32_t i = 0; i < NSF_SIZE; i++) want[i] = (uint8_t)(i * 13u + 5u);
    check(disc_image_find(img, "S0/S000000D.NSF", &e) == 0 &&
          disc_image_read_file(img, &e, buf, sizeof buf) == NSF_SIZE &&
          memcmp(buf, want, NSF_SIZE) == 0, "read_file across Mode 2 and Mode 1 sectors");
    check(disc_image_read_file(img, &e, buf, 100) == 100 && memcmp(buf, want, 100) == 0,
          "read_file stops at max");
    uint8_t raw[2352], ref[2352];
    raw_sector(20, ref);
    check(disc_image_read_sector(img, 20, raw) == 0 && memcmp(raw, ref, 2352) == 0, "read_sector");
    check(disc_image_read_sector(img, SECTORS, raw) != 0, "read_sector past the end fails");
    check(disc_image_sector_bytes(img) == 2352, "sector_bytes");
}

/** @brief The cache: a cached handle hits once the loader is done, a direct handle reads direct.
 *
 * @param path The image path. */
static void cache_cases(const char* path) {
    disc_image_t* cached = disc_image_open(path, DISC_READ_CACHED, OPENPETE_DISC_TAG_LIBCD);
    disc_image_t* direct = disc_image_open(path, DISC_READ_DIRECT, OPENPETE_DISC_TAG_INGEST);
    check(cached && direct, "open cached and direct");
    if (!cached || !direct) return;
    check(openpete_disc_cache_attach(disc_image_bin_path(cached)) == 1, "cache attach");
    uint8_t a[2352], b[2352];
    int r = OPENPETE_DISC_MISS;
    for (int i = 0; i < 2000 && r != OPENPETE_DISC_HIT; i++) {
        r = disc_image_read_bytes(cached, 20u * 2352u, a, sizeof a, OPENPETE_DISC_TAG_LIBCD);
        if (r != OPENPETE_DISC_HIT) SDL_Delay(1);
    }
    check(r == OPENPETE_DISC_HIT, "cached handle hits once the chunk is resident");
    check(disc_image_read_bytes(direct, 20u * 2352u, b, sizeof b, OPENPETE_DISC_TAG_INGEST) ==
          OPENPETE_DISC_DIRECT, "direct handle reads direct");
    check(memcmp(a, b, sizeof a) == 0, "hit bytes equal direct bytes");

    /* The booted disc's read_sector, the mod API slot's target. */
    check(disc_image_game_read_sector(20, a) != 0, "read_sector with no booted disc fails");
    disc_image_set_game(cached);
    uint8_t ref[2352];
    raw_sector(20, ref);
    check(disc_image_game_read_sector(20, a) == 0 && memcmp(a, ref, 2352) == 0, "booted-disc read_sector");
    check(disc_image_game_read_sector(SECTORS + 5u, a) != 0, "booted-disc read_sector past the end");
    disc_image_close(cached);
    check(disc_image_game() == NULL, "closing the booted disc clears it");
    disc_image_close(direct);
    openpete_disc_cache_shutdown();
}

/** @brief Runs every case.
 *
 * @return 0 when all pass. */
int main(void) {
    const char* tmp = getenv("TMPDIR");
    const char* dir = tmp && *tmp ? tmp : "/tmp";
    char path[512], cooked[512], odd[512];
    snprintf(path, sizeof path, "%s/disc_image_test.bin", dir);
    snprintf(cooked, sizeof cooked, "%s/disc_image_test_2048.iso", dir);
    snprintf(odd, sizeof odd, "%s/disc_image_test_odd.bin", dir);
    build_volume();
    write_raw(path);
    write_zero(cooked, 20u * 2048u);
    write_zero(odd, 20u * 2352u + 7u);

    disc_image_t* img = disc_image_open(path, DISC_READ_DIRECT, OPENPETE_DISC_TAG_NONE);
    check(img != NULL, "open raw image");
    if (img) {
        lookup_cases(img);
        read_cases(img);
        disc_image_close(img);
    }
    check(disc_image_open(cooked, DISC_READ_DIRECT, OPENPETE_DISC_TAG_NONE) == NULL,
          "a 2048-byte image is refused");
    check(disc_image_open(odd, DISC_READ_DIRECT, OPENPETE_DISC_TAG_NONE) == NULL,
          "an image of no whole sector count is refused");

    char serial[32];
    check(openpete_psx_read_disc_serial(path, serial, sizeof serial) == 0 &&
          strcmp(serial, "SCUS-94228") == 0, "disc serial");
    size_t n = 0;
    uint8_t* nsf = openpete_psx_load_file_from_disc(path, "S0/S000000D.NSF", &n);
    check(nsf && n == NSF_SIZE && nsf[2047] == (uint8_t)(2047u * 13u + 5u) &&
          nsf[2048] == (uint8_t)(2048u * 13u + 5u), "file loader (the woah NSF path)");
    free(nsf);

    cache_cases(path);

    /* The api table's read_sector slot is appended after disc_path, last. */
    check(offsetof(openpete_mod_api_t, read_sector) > offsetof(openpete_mod_api_t, disc_path) &&
          offsetof(openpete_mod_api_t, read_sector) + sizeof(void*) == sizeof(openpete_mod_api_t),
          "read_sector is the api table's last slot");
    check(OPENPETE_MOD_API_VERSION >= 13u, "api version covers read_sector");

    remove(path); remove(cooked); remove(odd);
    if (s_fail) return 1;
    printf("disc_image_test: OK\n");
    return 0;
}
