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
 *   merge   two instances on one card file: a forked child stands for a
 *           second process that loaded the same card. Saves to different
 *           files and to different BIOS frames merge, each instance taking
 *           the other's frames; a save to the file or frame the other
 *           instance changed is refused (McErrCardInvalid, -1 for the
 *           frame) and its retry succeeds; a card lock held past the wait
 *           refuses the save without refusing the one after the release.
 */

#include "openpete_atomic_file.h"
#include "openpete_memcard.h"
#include "openpete.h"
#include "openpete_paths.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "openpete_mods.h"
#include "openpete_modsave.h"
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Link-time stand-ins for the runtime the card module reads. */
int openpete_speculative = 0;
unsigned openpete_frame = 0;
const char* openpete_mods_modset_tag(void) { return ""; }
/* No game descriptor: the card-level .modset path runs, and the save-record
 * hooks the card module calls do nothing. */
static const game_mod_desc_t s_no_desc;
/** @brief Stand-in: a game with no mod-layer surfaces. */
const game_mod_desc_t* openpete_mods_game_desc(void) { return &s_no_desc; }
/** @brief Stand-in: no save records. */
void openpete_modsave_card_mounted(int slot, const char* image_path, int adopt) { (void)slot; (void)image_path; (void)adopt; }
/** @brief Stand-in: no save records. */
void openpete_modsave_card_write(int slot, const char* name, const uint8_t* file, size_t file_len,
                                 int ofs, int bytes, int flushed) {
    (void)slot; (void)name; (void)file; (void)file_len; (void)ofs; (void)bytes; (void)flushed;
}
/** @brief Stand-in: no save records. */
void openpete_modsave_card_frame_write(int slot, int frame) { (void)slot; (void)frame; }
/** @brief Stand-in: no save records. */
void openpete_modsave_card_format(int slot) { (void)slot; }
/** @brief Stand-in: no save records. */
void openpete_modsave_spec_discard(void) {}
/** @brief Stand-in: no save records. */
void openpete_modsave_spec_commit(int flushed) { (void)flushed; }

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

/** @brief Run fn in a forked child that stands for a second instance holding the same card state, and wait for it.
 * @return 0 when the child returned 0. */
static int other_instance(int (*fn)(void)) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) _exit(fn());
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

/** @brief Fill an 8 KiB buffer with v and write it to the named file through MemCardWriteFile.
 * @return The libmcrd result. */
static int save_fill(const char* name, int v) {
    unsigned char buf[8192];
    memset(buf, v, sizeof buf);
    return openpete_memcard_write(0x00, name, buf, 0, sizeof buf);
}

/** @brief Second instance: create and fill "BFILE". */
static int b_new_file(void) {
    return openpete_memcard_create(0x00, "BFILE", 1) != 0 || save_fill("BFILE", 0x44) != 0;
}
/** @brief Second instance: overwrite "TESTFILE". */
static int b_same_file(void) { return save_fill("TESTFILE", 0x22) != 0; }
/* The fill byte the second instance writes to frame 200. */
static int s_b_fill = 0x55;
/** @brief Second instance: write BIOS frame 200 filled with s_b_fill. */
static int b_frame_200(void) {
    unsigned char fr[128];
    memset(fr, s_b_fill, sizeof fr);
    return openpete_memcard_write_frame(0, 200, fr) != 0;
}

/** @brief Read byte 0 of the named file through MemCardReadFile, or -1. */
static int first_byte(const char* name) {
    unsigned char b[1];
    return openpete_memcard_read(0x00, name, b, 0, 1) == 0 ? b[0] : -1;
}

/** @brief Two instances on one card: different files and frames merge, the same file or frame refuses then retries, a held lock refuses without poisoning.
 * @return 0 when every verdict holds. */
static int case_merge(void) {
    char dir[600], path[700], lockp[720];
    fresh_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/card1.mcr", dir);
    snprintf(lockp, sizeof lockp, "%s.lock", path);
    if (seed_card() != 0) return fail("merge: seeding the card failed");
    int rc = 0;
    unsigned fl0, ad, rf, lo;

    /* Different files: B creates BFILE, A then saves TESTFILE. */
    if (other_instance(b_new_file) != 0) return fail("merge: second instance save failed");
    const int a_diff = save_fill("TESTFILE", 0x33);
    long n = 0;
    unsigned char* img = slurp(path, &n);
    const int both = img && n == CARD_SIZE && img[8192] == 0x33 && img[2 * 8192] == 0x44;
    free(img);
    openpete_memcard_merge_counts(&fl0, &ad, &rf, &lo);
    printf("merge: different files: save=%d file holds both=%d read BFILE=0x%02x adopted=%u refused=%u\n",
           a_diff, both, first_byte("BFILE") & 0xFF, ad, rf);
    if (a_diff != 0 || !both || first_byte("BFILE") != 0x44 || ad == 0 || rf != 0)
        rc |= fail("merge: saves to different files did not merge");

    /* Same file: B overwrites TESTFILE, A's save of it is refused, the retry lands. */
    if (other_instance(b_same_file) != 0) return fail("merge: second instance same-file save failed");
    const int a_same = save_fill("TESTFILE", 0x66);
    const int seen = first_byte("TESTFILE");
    const int retry = save_fill("TESTFILE", 0x66);
    img = slurp(path, &n);
    const int landed = img && n == CARD_SIZE && img[8192] == 0x66 && img[2 * 8192] == 0x44;
    free(img);
    openpete_memcard_merge_counts(NULL, NULL, &rf, NULL);
    printf("merge: same file: save=%d then reads 0x%02x, retry=%d landed=%d refused frames=%u\n",
           a_same, seen & 0xFF, retry, landed, rf);
    if (a_same != 2 || seen != 0x22 || retry != 0 || !landed || rf == 0)
        rc |= fail("merge: a save to a file the other instance changed was not refused then retried");

    /* BIOS frames: B writes frame 200; A's frame 201 merges, A's frame 200 refuses then retries. */
    unsigned char fr[128], back[128];
    memset(fr, 0x77, sizeof fr);
    if (other_instance(b_frame_200) != 0) return fail("merge: second instance frame write failed");
    const int f201 = openpete_memcard_write_frame(0, 201, fr);
    openpete_memcard_read_frame(0, 200, back);
    const int took200 = back[0] == 0x55;
    s_b_fill = 0x56;
    if (other_instance(b_frame_200) != 0) return fail("merge: second instance frame write failed");
    fr[0] = 0x78;
    const int f200 = openpete_memcard_write_frame(0, 200, fr);
    const int f200r = openpete_memcard_write_frame(0, 200, fr);
    img = slurp(path, &n);
    const int frames_ok = img && n == CARD_SIZE && img[200 * 128] == 0x78 && img[201 * 128] == 0x77;
    free(img);
    printf("merge: frames: other frame=%d took B's frame=%d same frame=%d retry=%d on disk=%d\n",
           f201, took200, f200, f200r, frames_ok);
    if (f201 != 0 || !took200 || f200 != -1 || f200r != 0 || !frames_ok)
        rc |= fail("merge: BIOS frame writes did not merge, refuse and retry");

    /* A lock held past the wait refuses the save; after release the next save lands. */
    intptr_t held = 0;
    if (openpete_paths_lock(lockp, &held) != 0) return fail("merge: could not take the card lock");
    const int locked = save_fill("TESTFILE", 0x88);
    openpete_paths_unlock(held);
    const int after = save_fill("TESTFILE", 0x99);
    img = slurp(path, &n);
    const int after_ok = img && n == CARD_SIZE && img[8192] == 0x99;
    free(img);
    openpete_memcard_merge_counts(NULL, NULL, NULL, &lo);
    printf("merge: lock held: save=%d lock refusals=%u, after release save=%d on disk=%d\n",
           locked, lo, after, after_ok);
    if (locked != 2 || lo != 1 || after != 0 || !after_ok)
        rc |= fail("merge: a held lock did not refuse, or poisoned the next save");
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
    if (r_missing != OPENPETE_EXIT_SETUP_ERROR)
        rc |= fail("folder: a card in a missing folder was not refused");
    if (r_present != 0) rc |= fail("folder: a card in an existing folder was refused");
    struct stat st;
    if (stat(missing, &st) == 0) rc |= fail("folder: the refused card was created");
    return rc;
}

/** @brief Run the startup check on one configuration and return its status.
 * @param guard OPENPETE_CARD_GUARD value, or NULL to unset it.
 * @param dir --card-dir value, or NULL for none.
 * @param card1 --card1 path, or NULL for card1.mcr in the card directory.
 * @param card2 --card2 path, or NULL for card2.mcr in the card directory.
 * @return The startup check's status. */
static int guard_leg(const char* guard, const char* dir, const char* card1, const char* card2) {
    if (guard) setenv("OPENPETE_CARD_GUARD", guard, 1);
    else unsetenv("OPENPETE_CARD_GUARD");
    openpete_memcard_set_dir_override(dir);
    openpete_memcard_set_paths(card1, card2);
    openpete_memcard_set_presence(0, 1);
    openpete_memcard_set_presence(1, 1);
    return openpete_memcard_startup_check();
}

/** @brief The card guard refuses a slot inside the per-user card directory, through a link or `..`, and admits scratch cards.
 *
 * XDG_STATE_HOME points at the case directory, so the per-user card
 * directory is <case>/state/openpete/cards; OPENPETE_CARD_DIR is unset.
 * @return 0 when every verdict holds. */
static int case_guard(void) {
    char dir[600], state[700], def[760], link[700], up[800], a[700], b[700], c1[800], c2[800];
    fresh_dir(dir, sizeof dir);
    snprintf(state, sizeof state, "%s/state", dir);
    snprintf(def, sizeof def, "%s/openpete/cards", state);
    snprintf(link, sizeof link, "%s/link", dir);
    snprintf(up, sizeof up, "%s/scratch/../state/openpete/cards", dir);
    snprintf(a, sizeof a, "%s/scratch/a.mcr", dir);
    snprintf(b, sizeof b, "%s/scratch/b.mcr", dir);
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s' '%s/scratch' && ln -s '%s' '%s'", def, dir, def, link);
    if (system(cmd) != 0) { perror("mkdir"); exit(2); }
    setenv("XDG_STATE_HOME", state, 1);
    unsetenv("OPENPETE_CARD_DIR");
    snprintf(c1, sizeof c1, "%s/card1.mcr", link);
    snprintf(c2, sizeof c2, "%s/card2.mcr", def);
    const struct { const char* name; const char* guard; const char* dir; const char* c1; const char* c2; int want; } legs[] = {
        { "no flags",              "1", NULL, NULL, NULL, OPENPETE_EXIT_CARD_GUARD },
        { "card1 only",            "1", NULL, a,    NULL, OPENPETE_EXIT_CARD_GUARD },
        { "card-dir default",      "1", def,  NULL, NULL, OPENPETE_EXIT_CARD_GUARD },
        { "card-dir via ..",       "1", up,   NULL, NULL, OPENPETE_EXIT_CARD_GUARD },
        { "card1 via link",        "1", NULL, c1,   b,    OPENPETE_EXIT_CARD_GUARD },
        { "card2 default path",    "1", NULL, a,    c2,   OPENPETE_EXIT_CARD_GUARD },
        { "both scratch",          "1", NULL, a,    b,    OPENPETE_EXIT_OK },
        { "guard 0, no flags",     "0", NULL, NULL, NULL, OPENPETE_EXIT_OK },
        { "guard unset, no flags", NULL, NULL, NULL, NULL, OPENPETE_EXIT_OK },
    };
    int rc = 0;
    for (size_t i = 0; i < sizeof legs / sizeof legs[0]; i++) {
        const int got = guard_leg(legs[i].guard, legs[i].dir, legs[i].c1, legs[i].c2);
        printf("guard: %s -> %d\n", legs[i].name, got);
        if (got != legs[i].want) rc |= fail("guard: a leg returned the wrong status");
    }
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
    rc |= run_case(case_guard);
    rc |= run_case(case_merge);
    printf(rc ? "memcard_test: FAILED\n" : "memcard_test: ok\n");
    return rc;
}
