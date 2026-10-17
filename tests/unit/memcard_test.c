/* memcard_test - the host memory card's persistence against failed and
 * interrupted writes and against card files it did not create.
 *
 * Cases, each on a fresh card directory under the path given as argv[1]
 * (default: a directory under /tmp):
 *   kill    a child process starts replacing a 128 KiB card through the
 *           atomic-file helper, writes half of the new image and is killed
 *           before the commit; the old card must be byte-identical after.
 *   fail    OPENPETE_WRITE_FAIL injects a failure after 65536 bytes into
 *           the card write; the game-facing calls must return the libmcrd
 *           error McErrCardInvalid (2), the file on disk must be unchanged
 *           and the in-memory card must still read the old contents.
 *           With the injection removed, a later frame write is still
 *           refused: the slot stays failing for the session.
 *   folder  a --card1 path in a folder that does not exist is refused by
 *           the startup check, and one in an existing folder passes.
 *   bad     a truncated card, an unknown 131200-byte file and a 131200-
 *           byte image with the PSP/PS3 .vmp magic: the first two are
 *           renamed to <name>.bad-1 with their bytes intact and a fresh
 *           card is provisioned; the .vmp is loaded read-only, never
 *           rewritten, and a save to it is refused with McErrCardInvalid.
 *   frames  64 BIOS frame writes (one 8 KiB block); the card file must
 *           keep its inode (written in place) and hold the new frames.
 *           Run alone (`memcard_test DIR frames`) under strace to count
 *           the bytes written per frame.
 */

#include "openpete_atomic_file.h"
#include "openpete_memcard.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Link-time stand-ins for the runtime the card module reads. */
int openpete_speculative = 0;
const char* openpete_mods_modset_tag(void) { return ""; }

#define CARD_SIZE 131072
static char s_root[512];
static int  s_case = 0;

/** @brief Print a failure line and return 1. */
static int fail(const char* what) {
    fprintf(stderr, "FAIL: %s\n", what);
    return 1;
}

/** @brief Create a fresh card directory for the next case and point the card module at it with both slots reset.
 *
 * The module's per-slot state is static, so each case runs in its own
 * child process; this is called at the start of the child.
 * @param out Receives the directory path.
 * @param cap Capacity of out. */
static void fresh_dir(char* out, size_t cap) {
    snprintf(out, cap, "%s/case%d", s_root, ++s_case);
    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", out, out);
    if (system(cmd) != 0) { perror("mkdir"); exit(2); }
    openpete_memcard_set_dir_override(out);
    openpete_memcard_set_presence(1, 0);
    openpete_memcard_init();
}

/** @brief Read a whole file into a malloc'd buffer.
 *
 * @param path File path.
 * @param len Receives its length.
 * @return The buffer, or NULL when the file cannot be read. */
static unsigned char* slurp(const char* path, long* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    rewind(f);
    unsigned char* b = malloc((size_t)*len + 1);
    if (b && fread(b, 1, (size_t)*len, f) != (size_t)*len) { free(b); b = NULL; }
    fclose(f);
    return b;
}

/** @brief Write a card-image fixture: header bytes, then a formatted raw card, cut to len bytes.
 *
 * The header is magic followed by 0x7E filler up to hdr bytes; the card
 * after it has the "MC" header frame and 15 free directory frames.
 * @return 0 on success. */
static int write_fixture(const char* path, long len, long hdr, const char* magic, size_t magic_len) {
    static unsigned char card[CARD_SIZE];
    memset(card, 0, sizeof card);
    card[0] = 'M'; card[1] = 'C';
    for (int i = 1; i <= 15; i++) {
        unsigned char* de = &card[i * 128];
        de[0] = 0xA0; de[8] = 0xFF; de[9] = 0xFF;
    }
    for (int i = 0; i <= 15; i++) {
        unsigned char x = 0;
        for (int k = 0; k < 127; k++) x ^= card[i * 128 + k];
        card[i * 128 + 127] = x;
    }
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    for (long i = 0; i < len; i++)
        fputc(i < (long)magic_len ? magic[i] : i < hdr ? 0x7E : card[i - hdr], f);
    return fclose(f);
}

/** @brief Provision card 1 and save one 1-block file "TESTFILE" holding 0x11 bytes through the libmcrd calls.
 * @return 0 when both calls succeeded. */
static int seed_card(void) {
    unsigned char buf[8192];
    memset(buf, 0x11, sizeof buf);
    if (openpete_memcard_create(0x00, "TESTFILE", 1) != 0) return -1;
    return openpete_memcard_write(0x00, "TESTFILE", buf, 0, sizeof buf);
}

/** @brief A replacement killed between its writes and its commit leaves the old card whole. */
static int case_kill(void) {
    char dir[600], path[700];
    fresh_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/card1.mcr", dir);
    if (seed_card() != 0) return fail("kill: seeding the card failed");
    long n0 = 0;
    unsigned char* before = slurp(path, &n0);
    if (!before || n0 != CARD_SIZE) return fail("kill: seeded card missing");

    pid_t pid = fork();
    if (pid == 0) {
        op_atomic_file_t af;
        static unsigned char img[CARD_SIZE];
        memset(img, 0x5A, sizeof img);
        if (op_atomic_file_open(&af, path) != 0) _exit(3);
        fwrite(img, 1, CARD_SIZE / 2, af.f);
        fflush(af.f);
        raise(SIGKILL);   /* killed mid-write: no commit, no cleanup */
        _exit(4);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGKILL) return fail("kill: child was not killed");

    long n1 = 0;
    unsigned char* after = slurp(path, &n1);
    int rc = 0;
    if (!after || n1 != CARD_SIZE || memcmp(before, after, CARD_SIZE) != 0)
        rc = fail("kill: the card changed under an uncommitted write");
    printf("kill: card after kill-at-65536-bytes: %ld bytes, %s\n", n1,
           rc ? "CHANGED" : "identical to before");
    free(before);
    free(after);
    return rc;
}

/** @brief An injected write failure reaches the game as McErrCardInvalid and leaves disk and memory on the old card. */
static int case_fail(void) {
    char dir[600], path[700];
    fresh_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/card1.mcr", dir);
    if (seed_card() != 0) return fail("fail: seeding the card failed");
    long n0 = 0;
    unsigned char* before = slurp(path, &n0);

    setenv("OPENPETE_WRITE_FAIL", "card1.mcr:65536", 1);
    unsigned char buf[8192];
    memset(buf, 0x22, sizeof buf);
    int w = openpete_memcard_write(0x00, "TESTFILE", buf, 0, sizeof buf);
    int cmds = -1, rslt = -1;
    openpete_memcard_sync(0, &cmds, &rslt);
    int c = openpete_memcard_create(0x00, "SECOND", 1);
    int f = openpete_memcard_format(0x00);
    unsigned char frame[128];
    memset(frame, 0x33, sizeof frame);
    int fw = openpete_memcard_write_frame(0, 70, frame);
    unsetenv("OPENPETE_WRITE_FAIL");
    int later = openpete_memcard_write_frame(0, 71, frame);
    printf("fail: write=%d sync rslt=%d create=%d format=%d write_frame=%d later=%d\n",
           w, rslt, c, f, fw, later);

    int rc = 0;
    if (w != 2 || rslt != 2) rc |= fail("fail: MemCardWriteFile did not report McErrCardInvalid");
    if (c != 2) rc |= fail("fail: MemCardCreateFile did not report McErrCardInvalid");
    if (f != 2) rc |= fail("fail: MemCardFormat did not report McErrCardInvalid");
    if (fw != -1) rc |= fail("fail: the BIOS frame write did not report failure");
    if (later != -1) rc |= fail("fail: a write after a failed one was not refused");

    long n1 = 0;
    unsigned char* after = slurp(path, &n1);
    if (!after || n1 != n0 || memcmp(before, after, (size_t)n0) != 0)
        rc |= fail("fail: the card file changed");
    unsigned char back[8192];
    if (openpete_memcard_read(0x00, "TESTFILE", back, 0, sizeof back) != 0 || back[0] != 0x11)
        rc |= fail("fail: the in-memory card kept the failed write");
    free(before);
    free(after);
    return rc;
}

/** @brief Load one card file of the given shape and report what the module made of it.
 *
 * @param label Case label for the output.
 * @param len File length.
 * @param hdr Header length before the card image.
 * @param magic Leading bytes, or NULL.
 * @param magic_len Their count.
 * @param want_aside 1 when the file must be moved to .bad-1, 0 when it must stay in place read-only.
 * @return 0 on the expected outcome. */
static int bad_one(const char* label, long len, long hdr, const char* magic, size_t magic_len, int want_aside) {
    char dir[600], path[700], aside[720];
    fresh_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/card1.mcr", dir);
    snprintf(aside, sizeof aside, "%s.bad-1", path);
    if (write_fixture(path, len, hdr, magic, magic_len) != 0) return fail("bad: fixture write");
    long n0 = 0;
    unsigned char* orig = slurp(path, &n0);

    int ex = openpete_memcard_exist(0x00, NULL, NULL);
    unsigned char buf[128] = {0};
    int save = openpete_memcard_create(0x00, "TESTFILE", 1);
    (void)buf;

    struct stat st_path, st_aside;
    int have_aside = stat(aside, &st_aside) == 0;
    int have_path = stat(path, &st_path) == 0;
    long na = 0;
    unsigned char* kept = slurp(want_aside ? aside : path, &na);
    int intact = kept && na == n0 && memcmp(kept, orig, (size_t)n0) == 0;
    printf("bad %-26s %6ld bytes: exist=%d create=%d aside=%s card=%ld bytes original %s\n",
           label, len, ex, save, have_aside ? "yes" : "no",
           have_path ? (long)st_path.st_size : -1L, intact ? "intact" : "LOST");
    int rc = 0;
    if (!intact) rc |= fail("bad: the original bytes were not kept");
    if (want_aside) {
        if (!have_aside) rc |= fail("bad: not moved aside");
        if (!have_path || st_path.st_size != CARD_SIZE) rc |= fail("bad: no fresh card provisioned");
        if (save != 0) rc |= fail("bad: the fresh card refused a save");
    } else {
        if (have_aside) rc |= fail("bad: a recognised image was moved aside");
        if (save != 2) rc |= fail("bad: a save to a read-only image was not refused");
    }
    free(orig);
    free(kept);
    return rc;
}

/** @brief A truncated card is moved aside and replaced. */
static int bad_truncated(void) { return bad_one("truncated .mcr", 70000, 0, NULL, 0, 1); }
/** @brief A 131200-byte file with no known header is moved aside and replaced. */
static int bad_unknown(void) { return bad_one("unknown header (.gme)", 131200, 128, "XXXX", 4, 1); }
/** @brief A PSP/PS3 .vmp image is loaded read-only in place. */
static int bad_vmp(void) { return bad_one(".vmp (\\0PMV header)", 131200, 128, "\0PMV", 4, 0); }
/** @brief A DexDrive .gme image is loaded read-only in place. */
static int bad_dexdrive(void) { return bad_one("DexDrive .gme", 3904 + CARD_SIZE, 3904, "123-456-STD", 11, 0); }

/** @brief 64 BIOS frame writes update the card in place and land on disk. */
static int case_frames(void) {
    char dir[600], path[700];
    fresh_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/card1.mcr", dir);
    if (openpete_memcard_exist(0x00, NULL, NULL) != 1) return fail("frames: no card");
    struct stat st0, st1;
    if (stat(path, &st0) != 0) return fail("frames: card not provisioned");
    unsigned char frame[128];
    for (int i = 0; i < 64; i++) {
        memset(frame, 0x40 + i, sizeof frame);
        if (openpete_memcard_write_frame(0, 64 + i, frame) != 0) return fail("frames: write failed");
    }
    long n = 0;
    unsigned char* img = slurp(path, &n);
    int rc = 0;
    if (!img || n != CARD_SIZE) rc |= fail("frames: card size changed");
    for (int i = 0; img && i < 64; i++)
        if (img[(64 + i) * 128] != 0x40 + i || img[(64 + i) * 128 + 127] != 0x40 + i) {
            rc |= fail("frames: a frame did not reach the file");
            break;
        }
    if (stat(path, &st1) != 0 || st1.st_ino != st0.st_ino)
        printf("frames: card file replaced (whole-image writes)\n");
    else
        printf("frames: card file updated in place (same inode)\n");
    free(img);
    return rc;
}

/** @brief A --card1 path in a missing folder fails the startup check; one in an existing folder passes.
 * @return 0 when both verdicts hold. */
static int case_folder(void) {
    char dir[600], missing[700], present[700];
    fresh_dir(dir, sizeof dir);
    snprintf(missing, sizeof missing, "%s/no-such-folder/card1.mcr", dir);
    snprintf(present, sizeof present, "%s/card1.mcr", dir);
    openpete_memcard_set_paths(missing, NULL);
    openpete_memcard_set_presence(1, 0);
    int r_missing = openpete_memcard_startup_check();
    openpete_memcard_set_paths(present, NULL);
    openpete_memcard_set_presence(1, 0);
    int r_present = openpete_memcard_startup_check();
    printf("folder: missing=%d present=%d\n", r_missing, r_present);
    int rc = 0;
    if (r_missing != -1) rc |= fail("folder: a card in a missing folder was not refused");
    if (r_present != 0) rc |= fail("folder: a card in an existing folder was refused");
    struct stat st;
    if (stat(missing, &st) == 0) rc |= fail("folder: the refused card was created");
    return rc;
}

/** @brief Run one case in a child process, since the card module's slot state is process-wide.
 * @return The case's result. */
static int run_case(int (*fn)(void)) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        int r = fn();
        fflush(stdout);
        _exit(r);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    s_case += 10;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

int main(int argc, char** argv) {
    snprintf(s_root, sizeof s_root, "%s", argc > 1 ? argv[1] : "/tmp/openpete-memcard-test");
    if (argc > 2 && strcmp(argv[2], "frames") == 0) return case_frames();
    int rc = 0;
    rc |= run_case(case_kill);
    rc |= run_case(case_fail);
    rc |= run_case(bad_truncated);
    rc |= run_case(bad_unknown);
    rc |= run_case(bad_vmp);
    rc |= run_case(bad_dexdrive);
    rc |= run_case(case_frames);
    rc |= run_case(case_folder);
    printf(rc ? "memcard_test: FAILED\n" : "memcard_test: ok\n");
    return rc;
}
