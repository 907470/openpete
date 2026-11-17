/* tests/unit/disc_build_test.c - disc build classification.
 *
 * Registers the three games' build rows (games/<game>/disc_builds.c), then
 * classifies synthetic SYSTEM.CNF texts and executables into every class
 * and warning: supported (the decomp checkouts' executables, under their
 * own boot name and under PSX.EXE), a supported serial with another
 * executable (a flipped byte, the shape of an unknown revision), a
 * supported executable under another game's serial, PAL and NTSC-J serials
 * on file and not, another NTSC-U serial, a non-Sony boot name, and no
 * SYSTEM.CNF, no BOOT line or no executable. Checks the BOOT-line parser and
 * the player message of each, then writes raw images (supported, altered,
 * PSX.EXE, a PAL boot path, no SYSTEM.CNF) and runs the image reader and
 * the boot check on them, with the fatal message stubbed. With
 * OPENPETE_DISC_BUILD_REAL=1 it also classifies the three checkouts' discs
 * and prints the time of each read; OPENPETE_DISC_BUILD_V11 names a Spyro:
 * Year of the Dragon revision 1.1 image to classify as the recognised
 * revision. */
#include "openpete_disc_build.h"
#include "openpete_disc_image.h"
#include "openpete_disc_trace.h"
#include "openpete_error_ids.h"
#include "openpete_log.h"
#include "openpete_psx.h"
#include "psx/libcd.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static int  s_fatals;
static char s_last_fatal[1024];

/** @brief Fatal message stub: counts the reports and keeps the last message. */
void openpete_fatal_message(openpete_error_t err, const char* title, const char* msg) {
    (void)err; (void)title;
    snprintf(s_last_fatal, sizeof s_last_fatal, "%s", msg);
    s_fatals++;
}

static int s_fail, s_checks;

/** @brief Set an environment variable, or unset it when val is NULL.
 *
 * @param var The variable.
 * @param val The value, or NULL. */
static void put_env(const char* var, const char* val) {
#ifdef _WIN32
    _putenv_s(var, val ? val : "");
#else
    if (val) setenv(var, val, 1);
    else unsetenv(var);
#endif
}

/** @brief Report a failed check.
 *
 * @param ok The check.
 * @param what Its description. */
static void check(int ok, const char* what) {
    s_checks++;
    if (!ok) { fprintf(stderr, "FAIL %s\n", what); s_fail++; }
}

/** @brief Read a whole file.
 *
 * @param path The file.
 * @param n Receives its size.
 * @return The bytes, freed by the caller; NULL when unreadable. */
static uint8_t* slurp(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* b = (uint8_t*)malloc(len > 0 ? (size_t)len : 1u);
    *n = fread(b, 1, (size_t)len, f);
    fclose(f);
    return b;
}

/** @brief Check one classification against the class, warning, region and game expected, and its texts.
 *
 * @param what The case's name.
 * @param cnf The SYSTEM.CNF text, or NULL.
 * @param exe The executable, or NULL.
 * @param n Its size.
 * @param cls The class expected.
 * @param warn The warning expected.
 * @param region The region expected.
 * @param game The game expected, or NULL. */
static void expect(const char* what, const char* cnf, const uint8_t* exe, size_t n,
                   openpete_disc_build_class_t cls, openpete_disc_warning_t warn,
                   openpete_disc_region_t region, const char* game) {
    openpete_disc_build_t b;
    openpete_disc_build_classify(cnf, exe, n, &b);
    char m[160];
    snprintf(m, sizeof m, "%s: class %s", what, openpete_disc_build_class_name(b.cls));
    check(b.cls == cls, m);
    snprintf(m, sizeof m, "%s: warning %d", what, (int)b.warning);
    check(b.warning == warn, m);
    snprintf(m, sizeof m, "%s: region %s", what, openpete_disc_region_name(b.region));
    check(b.region == region, m);
    snprintf(m, sizeof m, "%s: game", what);
    check(game ? (b.game && !strcmp(b.game, game)) : !b.game, m);

    char msg[768];
    openpete_disc_build_message(&b, 0, msg, sizeof msg);
    if (cls == OPENPETE_DISC_BUILD_SUPPORTED) {
        snprintf(m, sizeof m, "%s: no message", what);
        check(!msg[0], m);
        return;
    }
    snprintf(m, sizeof m, "%s: message names the crash", what);
    check(strstr(msg, "expected to crash") != NULL && strstr(msg, "OpenPete supports only") != NULL, m);
    openpete_disc_build_message(&b, 1, msg, sizeof msg);
    snprintf(m, sizeof m, "%s: refused message", what);
    check(strstr(msg, "cannot start it") != NULL, m);
}

/** @brief The BOOT-line parser on the spellings real SYSTEM.CNF files use. */
static void boot_line_cases(void) {
    static const struct { const char* text; const char* path; const char* serial; } k[] = {
        { "BOOT = cdrom:\\SCUS_942.28;1\r\nTCB = 4\r\n", "SCUS_942.28", "SCUS-94228" },
        { "BOOT=cdrom:\\SCES_028.35;1\n", "SCES_028.35", "SCES-02835" },
        { "BOOT = cdrom:/SLPS_000.01;1", "SLPS_000.01", "SLPS-00001" },
        { "BOOT\t=\tcdrom:\\\\SCPS_101.28;1", "SCPS_101.28", "SCPS-10128" },
        { "BOOT = cdrom:\\DIR\\SLUS_005.55;1", "DIR\\SLUS_005.55", "SLUS-00555" },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        char path[64], serial[32];
        const int r = openpete_psx_parse_boot_line(k[i].text, path, sizeof path, serial, sizeof serial);
        char m[128];
        snprintf(m, sizeof m, "boot line %zu: %s %s", i, path, serial);
        check(r == 0 && !strcmp(path, k[i].path) && !strcmp(serial, k[i].serial), m);
    }
    char path[64];
    check(openpete_psx_parse_boot_line("TCB = 4\r\n", path, sizeof path, NULL, 0) != 0, "no BOOT line");
    check(openpete_psx_parse_boot_line("BOOT = cdrom:\\;1", path, sizeof path, NULL, 0) != 0, "empty BOOT value");
    check(openpete_psx_parse_boot_line(NULL, path, sizeof path, NULL, 0) != 0, "NULL text");
}

#define CNF(p) "BOOT = cdrom:\\" p ";1\r\nTCB = 4\r\nEVENT = 16\r\nSTACK = 801FFFF0\r\n"

/* The checkouts' executables: Spyro 1 and Spyro 2's are the discs' own;
 * Spyro 3's is the decomp build, the disc's with exe_check patched. */
static uint8_t* s_exe1;
static uint8_t* s_exe2;
static uint8_t* s_exe3;
static size_t   s_n1, s_n2, s_n3;

/** @brief Every class and warning from synthetic SYSTEM.CNF texts and executables. */
static void classify_cases(void) {
    static const uint8_t junk[] = "PS-X EXE not a real program";
    const size_t nj = sizeof junk;
    if (s_exe1) {
        expect("S1 supported", CNF("SCUS_942.28"), s_exe1, s_n1, OPENPETE_DISC_BUILD_SUPPORTED,
               OPENPETE_DISC_WARN_NONE, OPENPETE_DISC_REGION_NTSC_U, "spyro-1");
        s_exe1[0x1000] ^= 0x01;
        expect("S1 one flipped byte", CNF("SCUS_942.28"), s_exe1, s_n1, OPENPETE_DISC_BUILD_UNKNOWN,
               OPENPETE_DISC_WARN_MODIFIED, OPENPETE_DISC_REGION_NTSC_U, "spyro-1");
        s_exe1[0x1000] ^= 0x01;
    } else {
        fprintf(stderr, "note: no Spyro 1 decomp PSX.EXE; the supported S1 case did not run\n");
    }
    if (s_exe2)
        expect("S2 supported", CNF("SCUS_944.25"), s_exe2, s_n2, OPENPETE_DISC_BUILD_SUPPORTED,
               OPENPETE_DISC_WARN_NONE, OPENPETE_DISC_REGION_NTSC_U, "spyro-2");
    if (s_exe1)
        expect("S1 program under the S2 serial", CNF("SCUS_944.25"), s_exe1, s_n1,
               OPENPETE_DISC_BUILD_UNKNOWN, OPENPETE_DISC_WARN_MODIFIED,
               OPENPETE_DISC_REGION_NTSC_U, "spyro-2");
    if (s_exe3) {
        expect("S3 decomp build", CNF("SCUS_944.67"), s_exe3, s_n3, OPENPETE_DISC_BUILD_SUPPORTED,
               OPENPETE_DISC_WARN_NONE, OPENPETE_DISC_REGION_NTSC_U, "spyro-3");
        s_exe3[0x2000] ^= 0x01;
        expect("S3 one flipped byte (unknown revision shape)", CNF("SCUS_944.67"), s_exe3, s_n3,
               OPENPETE_DISC_BUILD_UNKNOWN, OPENPETE_DISC_WARN_MODIFIED,
               OPENPETE_DISC_REGION_NTSC_U, "spyro-3");
        s_exe3[0x2000] ^= 0x01;
    }
    if (s_exe1)
        expect("S1 program booted as PSX.EXE", CNF("PSX.EXE"), s_exe1, s_n1, OPENPETE_DISC_BUILD_SUPPORTED,
               OPENPETE_DISC_WARN_NONE, OPENPETE_DISC_REGION_NTSC_U, "spyro-1");
    expect("S3 serial, other program", CNF("SCUS_944.67"), junk, nj, OPENPETE_DISC_BUILD_UNKNOWN,
           OPENPETE_DISC_WARN_MODIFIED, OPENPETE_DISC_REGION_NTSC_U, "spyro-3");
    expect("S1 PAL", CNF("SCES_014.38"), junk, nj, OPENPETE_DISC_BUILD_RECOGNISED,
           OPENPETE_DISC_WARN_PAL, OPENPETE_DISC_REGION_PAL, "spyro-1");
    expect("S2 PAL", CNF("SCES_021.04"), junk, nj, OPENPETE_DISC_BUILD_RECOGNISED,
           OPENPETE_DISC_WARN_PAL, OPENPETE_DISC_REGION_PAL, "spyro-2");
    expect("S3 PAL", CNF("SCES_028.35"), junk, nj, OPENPETE_DISC_BUILD_RECOGNISED,
           OPENPETE_DISC_WARN_PAL, OPENPETE_DISC_REGION_PAL, "spyro-3");
    expect("S1 NTSC-J", CNF("SCPS_100.85"), junk, nj, OPENPETE_DISC_BUILD_RECOGNISED,
           OPENPETE_DISC_WARN_NTSC_J, OPENPETE_DISC_REGION_NTSC_J, "spyro-1");
    expect("S2 NTSC-J", CNF("SCPS_101.28"), junk, nj, OPENPETE_DISC_BUILD_RECOGNISED,
           OPENPETE_DISC_WARN_NTSC_J, OPENPETE_DISC_REGION_NTSC_J, "spyro-2");
    expect("unlisted PAL", CNF("SLES_012.34"), junk, nj, OPENPETE_DISC_BUILD_UNKNOWN,
           OPENPETE_DISC_WARN_PAL, OPENPETE_DISC_REGION_PAL, NULL);
    expect("unlisted NTSC-J", CNF("SLPM_860.00"), junk, nj, OPENPETE_DISC_BUILD_UNKNOWN,
           OPENPETE_DISC_WARN_NTSC_J, OPENPETE_DISC_REGION_NTSC_J, NULL);
    expect("other NTSC-U", CNF("SCUS_944.26"), junk, nj, OPENPETE_DISC_BUILD_UNKNOWN,
           OPENPETE_DISC_WARN_OTHER, OPENPETE_DISC_REGION_NTSC_U, NULL);
    expect("letters only, PAL-like third letter", CNF("XXEXEC.BIN"), junk, nj, OPENPETE_DISC_BUILD_UNKNOWN,
           OPENPETE_DISC_WARN_OTHER, OPENPETE_DISC_REGION_UNKNOWN, NULL);
    expect("non-Sony boot name", CNF("PSX.EXE"), junk, nj, OPENPETE_DISC_BUILD_UNKNOWN,
           OPENPETE_DISC_WARN_OTHER, OPENPETE_DISC_REGION_UNKNOWN, NULL);
    expect("no SYSTEM.CNF", NULL, junk, nj, OPENPETE_DISC_BUILD_UNREADABLE,
           OPENPETE_DISC_WARN_UNREADABLE, OPENPETE_DISC_REGION_UNKNOWN, NULL);
    expect("no BOOT line", "TCB = 4\r\n", junk, nj, OPENPETE_DISC_BUILD_UNREADABLE,
           OPENPETE_DISC_WARN_UNREADABLE, OPENPETE_DISC_REGION_UNKNOWN, NULL);
    expect("PAL serial on file, no executable", CNF("SCES_028.35"), NULL, 0, OPENPETE_DISC_BUILD_RECOGNISED,
           OPENPETE_DISC_WARN_PAL, OPENPETE_DISC_REGION_PAL, "spyro-3");
    expect("supported serial, no executable", CNF("SCUS_942.28"), NULL, 0, OPENPETE_DISC_BUILD_UNREADABLE,
           OPENPETE_DISC_WARN_UNREADABLE, OPENPETE_DISC_REGION_NTSC_U, NULL);
    expect("unlisted serial, no executable", CNF("SLES_012.34"), NULL, 0, OPENPETE_DISC_BUILD_UNREADABLE,
           OPENPETE_DISC_WARN_UNREADABLE, OPENPETE_DISC_REGION_PAL, NULL);
}

/* A raw image: PVD at 16, terminator at 17, root at 18, SYSTEM.CNF at 19,
 * the executable from 20. */
#define FIRST_EXE_LBA 20u

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
 * @param name Identifier.
 * @param lba Extent sector.
 * @param size Extent length.
 * @param flags File flags (2 = directory).
 * @return The record length. */
static int dir_record(uint8_t* p, const char* name, uint32_t lba, uint32_t size, uint8_t flags) {
    const int nl = name[0] ? (int)strlen(name) : 1;
    const int len = 33 + nl + ((33 + nl) & 1);
    p[0] = (uint8_t)len;
    put_both32(p + 2, lba);
    put_both32(p + 10, size);
    p[25] = flags;
    p[32] = (uint8_t)nl;
    memcpy(p + 33, name, (size_t)nl);
    return len;
}

/** @brief Write a raw image holding SYSTEM.CNF (unless @p cnf is NULL) and one executable.
 *
 * @param path Output path.
 * @param cnf The SYSTEM.CNF text, or NULL for none.
 * @param exe_name The executable's ISO 9660 name ("SCUS_942.28;1").
 * @param exe Its bytes.
 * @param n Their count. */
static void write_image(const char* path, const char* cnf, const char* exe_name, const uint8_t* exe, size_t n) {
    const uint32_t exe_sectors = (uint32_t)((n + 2047) / 2048);
    const uint32_t sectors = FIRST_EXE_LBA + exe_sectors;
    uint8_t* user = (uint8_t*)calloc(sectors, 2048);
    uint8_t* pvd = user + 16 * 2048;
    pvd[0] = 1; memcpy(pvd + 1, "CD001", 5); pvd[6] = 1;
    dir_record(pvd + 156, "", 18, 2048, 2);
    uint8_t* term = user + 17 * 2048;
    term[0] = 255; memcpy(term + 1, "CD001", 5); term[6] = 1;
    uint8_t* root = user + 18 * 2048;
    int o = dir_record(root, "", 18, 2048, 2);
    o += dir_record(root + o, "\1", 18, 2048, 2);
    o += dir_record(root + o, exe_name, FIRST_EXE_LBA, (uint32_t)n, 0);
    if (cnf) {
        dir_record(root + o, "SYSTEM.CNF;1", 19, (uint32_t)strlen(cnf), 0);
        memcpy(user + 19 * 2048, cnf, strlen(cnf));
    }
    memcpy(user + FIRST_EXE_LBA * 2048, exe, n);
    FILE* f = fopen(path, "wb");
    uint8_t raw[2352];
    for (uint32_t i = 0; i < sectors; i++) {
        memset(raw, 0, sizeof raw);
        memset(raw + 1, 0xFF, 10);
        raw[15] = 2;
        memcpy(raw + 24, user + (size_t)i * 2048, 2048);
        fwrite(raw, 1, sizeof raw, f);
    }
    fclose(f);
    free(user);
}

/** @brief The image reader and the boot check on written images.
 *
 * @param dir The directory the images are written in. */
static void image_cases(const char* dir) {
    static const uint8_t junk[] = "PS-X EXE not a real program";
    char sup[512], alt[512], pal[512], none[512];
    snprintf(sup, sizeof sup, "%s/disc_build_test_supported.bin", dir);
    snprintf(alt, sizeof alt, "%s/disc_build_test_altered.bin", dir);
    snprintf(pal, sizeof pal, "%s/disc_build_test_pal.bin", dir);
    snprintf(none, sizeof none, "%s/disc_build_test_nocnf.bin", dir);
    remove(sup); remove(alt); remove(pal); remove(none);
    openpete_disc_build_t b;

    if (s_exe1) {
        write_image(sup, CNF("SCUS_942.28"), "SCUS_942.28;1", s_exe1, s_n1);
        check(openpete_disc_build_identify(sup, &b) == 0 && b.cls == OPENPETE_DISC_BUILD_SUPPORTED &&
              !strcmp(b.serial, "SCUS-94228"), "image: supported");
        s_fatals = 0;
        check(openpete_disc_build_boot_check(sup, "SCUS_942.28") == 0 && !s_fatals,
              "boot check: supported proceeds silently");
        s_exe1[0x1000] ^= 0x01;
        write_image(alt, CNF("SCUS_942.28"), "SCUS_942.28;1", s_exe1, s_n1);
        s_exe1[0x1000] ^= 0x01;
        check(openpete_disc_build_identify(alt, &b) == 0 && b.cls == OPENPETE_DISC_BUILD_UNKNOWN &&
              b.warning == OPENPETE_DISC_WARN_MODIFIED, "image: altered executable");
        s_fatals = 0;
        check(openpete_disc_build_boot_check(alt, "SCUS_942.28;1") == 0 && !s_fatals,
              "boot check: altered warns and proceeds");
        put_env("OPENPETE_DISC_BUILD_REFUSE", "1");
        s_fatals = 0;
        check(openpete_disc_build_boot_check(alt, "SCUS_942.28") != 0 && s_fatals == 1 &&
              strstr(s_last_fatal, "cannot start it"), "boot check: OPENPETE_DISC_BUILD_REFUSE refuses");
        put_env("OPENPETE_DISC_BUILD_REFUSE", "0");
        s_fatals = 0;
        check(openpete_disc_build_boot_check(alt, "SCUS_942.28") == 0 && !s_fatals,
              "boot check: OPENPETE_DISC_BUILD_REFUSE=0 warns");
        put_env("OPENPETE_DISC_BUILD_REFUSE", NULL);
    }
    if (s_exe1) {
        write_image(pal, CNF("PSX.EXE"), "PSX.EXE;1", s_exe1, s_n1);
        check(openpete_disc_build_identify(pal, &b) == 0 && b.cls == OPENPETE_DISC_BUILD_SUPPORTED,
              "image: the supported program booted as PSX.EXE");
    }
    write_image(pal, CNF("SCES_028.35"), "SCES_028.35;1", junk, sizeof junk);
    check(openpete_disc_build_identify(pal, &b) == 0 && b.cls == OPENPETE_DISC_BUILD_RECOGNISED &&
          b.warning == OPENPETE_DISC_WARN_PAL, "image: PAL");
    s_fatals = 0;
    check(openpete_disc_build_boot_check(pal, "SCUS_944.67") != 0 && s_fatals == 1 &&
          strstr(s_last_fatal, "PAL") && strstr(s_last_fatal, "SCUS-94467"),
          "boot check: a disc booting another executable is refused with its message");
    write_image(none, NULL, "SCUS_942.28;1", junk, sizeof junk);
    check(openpete_disc_build_identify(none, &b) == 0 && b.cls == OPENPETE_DISC_BUILD_UNREADABLE,
          "image: no SYSTEM.CNF");
    s_fatals = 0;
    check(openpete_disc_build_boot_check(none, "SCUS_942.28") == 0 && !s_fatals,
          "boot check: no SYSTEM.CNF, the game's executable present: generic warning, proceeds");
    s_fatals = 0;
    check(openpete_disc_build_boot_check(none, "SCUS_944.25") != 0 && s_fatals == 1,
          "boot check: no SYSTEM.CNF and not the game's executable: refused");
    check(openpete_disc_build_identify("/nonexistent/disc.cue", &b) != 0 &&
          b.cls == OPENPETE_DISC_BUILD_UNREADABLE, "image: missing file");

    /* A rewritten image is read again. */
    write_image(pal, CNF("SCES_021.04"), "SCES_021.04;1", junk, sizeof junk);
    check(openpete_disc_build_identify(pal, &b) == 0 && !strcmp(b.serial, "SCES-02104"),
          "a rewritten image is read again");
    remove(sup); remove(alt); remove(pal); remove(none);
}

/** @brief Seconds on the monotonic clock.
 *
 * @return The time. */
static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/** @brief Classify the three checkouts' discs, timing two reads of each. */
static void real_disc_cases(void) {
    static const char* const k[] = { "spyro-1/spyro-1/build/SCUS-94228.cue",
                                     "spyro-2/spyro-2/build/SCUS-94425.cue",
                                     "spyro-3/spyro-3/build/SCUS-94467.cue" };
    for (size_t i = 0; i < 3; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/games/%s", OPENPETE_SOURCE_DIR, k[i]);
        openpete_disc_build_t b, c;
        const double t0 = now_s();
        const int r = openpete_disc_build_identify(path, &b);
        const double t1 = now_s();
        (void)openpete_disc_build_identify(path, &c);
        const double t2 = now_s();
        printf("real %s: %s serial %s exe %s first %.2f ms second %.2f ms\n", k[i],
               openpete_disc_build_class_name(b.cls), b.serial, b.exe_sha256,
               (t1 - t0) * 1e3, (t2 - t1) * 1e3);
        check(r == 0 && b.cls == OPENPETE_DISC_BUILD_SUPPORTED, path);
    }
}

/** @brief Classify a Spyro: Year of the Dragon revision 1.1 image named by OPENPETE_DISC_BUILD_V11 (a player's own dump, never a fixture).
 *
 * @param path The image's .cue or .bin. */
static void revision_disc_case(const char* path) {
    openpete_disc_build_t b;
    const double t0 = now_s();
    const int r = openpete_disc_build_identify(path, &b);
    const double t1 = now_s();
    char msg[1024];
    openpete_disc_build_message(&b, 0, msg, sizeof msg);
    printf("v1.1 %s: %s warning %d serial %s exe %s %.2f ms\n  message: %s\n", path,
           openpete_disc_build_class_name(b.cls), (int)b.warning, b.serial, b.exe_sha256,
           (t1 - t0) * 1e3, msg);
    check(r == 0 && b.cls == OPENPETE_DISC_BUILD_RECOGNISED && b.warning == OPENPETE_DISC_WARN_REVISION &&
          b.revision && !strcmp(b.revision, "1.1"), "v1.1 image: recognised revision 1.1");
    check(strstr(msg, "revision 1.1") && strstr(msg, "original") &&
          strstr(msg, "9df489e2420988494691c9193fe74dec4b1dbfcd") && strstr(msg, "expected to crash"),
          "v1.1 message: revision, the original pressing's .bin SHA-1, the crash");
    s_fatals = 0;
    check(openpete_disc_build_boot_check(path, "SCUS_944.67") == 0 && !s_fatals,
          "v1.1 boot check: warns and proceeds");
}

extern const openpete_disc_build_row_t spyro1_disc_builds[], spyro2_disc_builds[], spyro3_disc_builds[];

int main(void) {
    openpete_disc_build_add_rows(spyro1_disc_builds);
    openpete_disc_build_add_rows(spyro2_disc_builds);
    openpete_disc_build_add_rows(spyro3_disc_builds);
    openpete_disc_build_add_rows(spyro3_disc_builds);   /* ignored */
    const char* tmp = getenv("TMPDIR");
    const char* dir = tmp && *tmp ? tmp : "/tmp";
    s_exe1 = slurp(OPENPETE_SOURCE_DIR "/games/spyro-1/spyro-1/build/PSX.EXE", &s_n1);
    s_exe2 = slurp(OPENPETE_SOURCE_DIR "/games/spyro-2/spyro-2/build/PSX.EXE", &s_n2);
    s_exe3 = slurp(OPENPETE_SOURCE_DIR "/games/spyro-3/spyro-3/build/PSX.EXE", &s_n3);
    boot_line_cases();
    classify_cases();
    image_cases(dir);
    const char* real = getenv("OPENPETE_DISC_BUILD_REAL");
    if (real && *real && strcmp(real, "0") != 0) real_disc_cases();
    const char* v11 = getenv("OPENPETE_DISC_BUILD_V11");
    if (v11 && *v11) revision_disc_case(v11);
    free(s_exe1); free(s_exe2); free(s_exe3);
    printf("disc_build_test: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
