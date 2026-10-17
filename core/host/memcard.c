/* memcard.c - host-backed PlayStation memory card. See openpete_memcard.h.
 *
 * Two raw 128 KiB card images (.mcr) with the standard PSX directory
 * structure, persisted write-through to disk. The layout, the XOR
 * checksums and the free-block chain follow the memory card format in the
 * nocash PSX specs.
 *
 * Card image layout (1024 sectors x 128 bytes = 128 KiB):
 *   Block 0 (sectors 0-63) = directory
 *     Sector 0     = header frame:  'M','C', zeros, XOR checksum at byte 127
 *     Sectors 1-15 = one directory frame per data block 1-15:
 *       +0x00 u32 state (0xA0 free, 0x51 in-use first-block, 0x52 mid, 0x53 last)
 *       +0x04 u32 file size in bytes (meaningful in the first-block frame)
 *       +0x08 u16 next-block link (0xFFFF = end of chain)
 *       +0x0A char[20] filename (ASCII)
 *       +0x7F u8 XOR checksum of bytes 0x00..0x7E
 *   Blocks 1-15 (sectors 64-1023) = data; block B at byte offset B*8192.
 */

#include "openpete_memcard.h"
#include "openpete_atomic_file.h"
#include "openpete_error_ids.h"
#include "openpete_log.h"
#include "openpete_paths.h"
#include "openpete_mods.h" /* openpete_mods_modset_tag - save safety */

extern int openpete_speculative; /* openpete_fiber.h - runahead lookahead */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
  #include <direct.h>
  #define OPENPETE_MKDIR(path) _mkdir(path)
  #define MC_SEP '\\'
#else
  #include <unistd.h>
  #define OPENPETE_MKDIR(path) mkdir((path), 0755)
  #define MC_SEP '/'
#endif

/* --- card geometry -------------------------------------------------------- */

#define MC_SECTOR_SIZE  128
#define MC_SECTORS      1024
#define MC_CARD_SIZE    (MC_SECTOR_SIZE * MC_SECTORS)   /* 131072 */
#define MC_BLOCK_SIZE   (MC_SECTOR_SIZE * 64)           /* 8192   */
#define MC_NAME_MAX     20                              /* on-card name field */

/* libmcrd result / sync / command codes (kept local so core needn't pull in
 * the PsyCross game header). Values are the PSYQ ABI constants. */
#define MC_ERR_NONE            0
#define MC_ERR_CARD_NOT_EXIST  1
#define MC_ERR_CARD_INVALID    2   /* communication error: the write did not land */
#define MC_ERR_NEW_CARD        3
#define MC_ERR_NOT_FORMAT      4
#define MC_ERR_FILE_NOT_EXIST  5
#define MC_ERR_ALREADY_EXIST   6
#define MC_ERR_BLOCK_FULL      7

#define MC_SYNC_NOTHING        (-1)
#define MC_SYNC_FINISHED       1

#define MC_FUNC_EXIST          1
#define MC_FUNC_ACCEPT         2
#define MC_FUNC_READ           3
#define MC_FUNC_WRITE          4

/* --- state ---------------------------------------------------------------- */

static uint8_t s_card[2][MC_CARD_SIZE];
static uint8_t s_disk[2][MC_CARD_SIZE];        /* the image as last persisted */
/* Writes to the slot are refused: its file is a headered image loaded
 * read-only (s_readonly), no card path resolved (s_mem_only), or a write
 * to its file failed earlier in the session (s_write_failed). */
static int     s_readonly[2];
static int     s_mem_only[2];
static int     s_write_failed[2];
static int     s_refuse_warned[2];
static int     s_present[2]      = { 1, 1 };   /* both cards inserted by default */
static int     s_loaded[2]       = { 0, 0 };
static int     s_new_card[2]     = { 1, 1 };   /* "new card" latch, clears on 1st probe */
static char    s_path[2][1280];                /* resolved image path (once loaded) */
static char    s_path_override[2][1024];       /* --card1 / --card2 */
static char    s_dir_override[1024];           /* --card-dir */
static int     s_unformatted     = 0;          /* --card-unformatted */

/* Per-modset save safety: a sidecar "<image>.modset"
 * records the modset tag the card's progress belongs to. Mismatch at
 * open warns (never blocks); --adopt-saves retags explicitly. */
static int  s_adopt = 0;                       /* --adopt-saves */
static char s_ms_tag[2][1024];                 /* sidecar tag at open */
static int  s_ms_write_warned[2];

/* Async-sync latch surfaced by MemCardSync. */
static int     s_mc_cmds   = -1;
static int     s_mc_rslt   = -1;
static int     s_mc_status = MC_SYNC_NOTHING;

/* --- directory / image helpers -------------------------------------------- */

/** @brief Write the directory holding the card images into out, creating it as needed.
 *
 * Precedence: the --card-dir override, then the OPENPETE_CARD_DIR
 * environment variable, then the `cards` leaf of openpete_paths_state_dir,
 * a sibling of the save-states' `states` directory. The directory is made
 * with its creation result ignored for the first two sources. */
static int mc_resolve_card_dir(char* out, size_t out_size)
{
    if (s_dir_override[0]) {
        (void)OPENPETE_MKDIR(s_dir_override);
        snprintf(out, out_size, "%s", s_dir_override);
        return 0;
    }
    const char* env = getenv("OPENPETE_CARD_DIR");
    if (env && *env) {
        (void)OPENPETE_MKDIR(env);
        snprintf(out, out_size, "%s", env);
        return 0;
    }
    return openpete_paths_state_dir("cards", out, out_size, 1);
}

/** @brief Replace the slot's image path with the file a symbolic link at it names.
 *
 * The whole-image write replaces the directory entry at the path, which
 * would turn a link into a regular file and leave its target unwritten,
 * while the in-place frame write follows the link. Resolving the link once
 * sends both writers to the target. A path that is not a link, or whose
 * target does not resolve, is kept. No-op on Windows.
 * @param slot Card slot, 0 or 1. */
static void mc_follow_link(int slot)
{
#ifndef _WIN32
    struct stat st;
    char* real;
    if (lstat(s_path[slot], &st) != 0 || !S_ISLNK(st.st_mode)) return;
    real = realpath(s_path[slot], NULL);
    if (!real) return;
    if (strlen(real) < sizeof(s_path[slot]))
        snprintf(s_path[slot], sizeof(s_path[slot]), "%s", real);
    free(real);
#else
    (void)slot;
#endif
}

/** @brief Write the slot's image path into s_path[slot] and return 0, or -1 when no card directory resolves.
 *
 * An explicit --card1/--card2 path is used as given; otherwise the path is
 * card<slot+1>.mcr in the directory mc_resolve_card_dir returns. A symbolic
 * link at the path is resolved to its target. */
static int mc_resolve_path(int slot)
{
    if (s_path_override[slot][0]) {
        snprintf(s_path[slot], sizeof(s_path[slot]), "%s", s_path_override[slot]);
        mc_follow_link(slot);
        return 0;
    }
    char dir[1024];
    if (mc_resolve_card_dir(dir, sizeof(dir)) != 0) return -1;
    snprintf(s_path[slot], sizeof(s_path[slot]), "%s%ccard%d.mcr",
             dir, MC_SEP, slot + 1);
    mc_follow_link(slot);
    return 0;
}

/// @brief Return the XOR of the first 127 bytes of a 128-byte card frame, the value its last byte holds.
static uint8_t mc_frame_checksum(const uint8_t* frame)
{
    uint8_t x = 0;
    for (int i = 0; i < MC_SECTOR_SIZE - 1; i++) x ^= frame[i];
    return x;
}

/** @brief Write a freshly formatted card image into card.
 *
 * Zeroes the MC_CARD_SIZE bytes, writes the "MC" header frame, and marks
 * the 15 directory frames free (state 0xA0, next-block link 0xFFFF), each
 * frame with its XOR checksum in its last byte. */
static void mc_format_image(uint8_t* card)
{
    memset(card, 0, MC_CARD_SIZE);

    /* Header frame. */
    card[0] = 'M';
    card[1] = 'C';
    card[MC_SECTOR_SIZE - 1] = mc_frame_checksum(card);

    /* 15 directory frames, all free. */
    for (int i = 1; i <= 15; i++) {
        uint8_t* de = &card[i * MC_SECTOR_SIZE];
        de[0] = 0xA0;                 /* state = free / formatted */
        de[8] = 0xFF; de[9] = 0xFF;   /* next-block link = none  */
        de[MC_SECTOR_SIZE - 1] = mc_frame_checksum(de);
    }
}

/// @brief Return nonzero when the slot's loaded image starts with the "MC" header signature.
static int mc_is_formatted(int slot)
{
    return s_card[slot][0] == 'M' && s_card[slot][1] == 'C';
}

/* --- per-modset save safety ------------------------------------------ */

/// @brief Write the slot's modset sidecar path, the image path with ".modset" appended, into out.
static void mc_modset_sidecar_path(int slot, char* out, size_t cap) {
    snprintf(out, cap, "%s.modset", s_path[slot]);
}

/** @brief Load the slot's modset sidecar into s_ms_tag, leaving the tag empty when the file is absent.
 *
 * Reads at most 1023 bytes and strips trailing newline, carriage return and
 * space characters. */
static void mc_modset_read_sidecar(int slot) {
    char p[1300];
    mc_modset_sidecar_path(slot, p, sizeof p);
    s_ms_tag[slot][0] = '\0';
    FILE* f = fopen(p, "rb");
    if (!f) return;
    size_t got = fread(s_ms_tag[slot], 1, sizeof s_ms_tag[slot] - 1, f);
    fclose(f);
    s_ms_tag[slot][got] = '\0';
    /* trim trailing newline/space */
    while (got && (s_ms_tag[slot][got - 1] == '\n' ||
                   s_ms_tag[slot][got - 1] == '\r' ||
                   s_ms_tag[slot][got - 1] == ' '))
        s_ms_tag[slot][--got] = '\0';
}

/** @brief Replace the slot's modset sidecar with tag and a newline, and adopt tag as the slot's sidecar tag when the write lands.
 *
 * @param slot Card slot, 0 or 1.
 * @param tag The modset tag; "" is vanilla. */
static void mc_modset_write_sidecar(int slot, const char* tag) {
    char p[1300];
    char line[1100];
    mc_modset_sidecar_path(slot, p, sizeof p);
    int n = snprintf(line, sizeof line, "%s\n", tag);
    if (n < 0 || (size_t)n >= sizeof line) return;
    if (op_atomic_write_file(p, line, (size_t)n) != 0) return;
    snprintf(s_ms_tag[slot], sizeof s_ms_tag[slot], "%s", tag);
}

static int s_ms_checked[2];

/** @brief Compare the slot's sidecar tag against the running modset tag, once per slot per process.
 *
 * A mismatch warns and continues, so progress is never blocked. The tag
 * changes here only under --adopt-saves, which rewrites the sidecar with
 * the running tag. A card tagged with the running tag, or untagged with no
 * mods active, is silent. The first call reads the sidecar, so s_ms_tag
 * holds the file's tag afterwards.
 * @param slot Card slot, 0 or 1. */
static void mc_modset_check_open(int slot) {
    if (s_ms_checked[slot]) return; /* once per session per slot */
    s_ms_checked[slot] = 1;
    const char* cur = openpete_mods_modset_tag();
    mc_modset_read_sidecar(slot);
    const char* tag = s_ms_tag[slot];
    if (strcmp(tag, cur) == 0) return; /* matched (incl. vanilla+vanilla) */
    if (s_adopt) {
        mc_modset_write_sidecar(slot, cur);
        OPL_INFO_F("memcard", "card %d: modset tag adopted (\"%s\")",
                   slot, cur[0] ? cur : "vanilla");
        return;
    }
    if (!tag[0])
        OPL_WARN_F("memcard", "card %d holds VANILLA progress but mods "
                   "are active (%s) - saving will mix modded progress "
                   "into it; pass --adopt-saves to accept and tag",
                   slot, cur);
    else if (!cur[0])
        OPL_WARN_F("memcard", "card %d is tagged with modset \"%s\" but "
                   "no mods are active - its progress may depend on "
                   "them; pass --adopt-saves to re-tag as vanilla",
                   slot, tag);
    else
        OPL_WARN_F("memcard", "card %d modset MISMATCH: tagged \"%s\", "
                   "running \"%s\" - pass --adopt-saves to accept",
                   slot, tag, cur);
}

/** @brief Apply the modset tag rule to a write of the slot's card.
 *
 * A tag equal to the running modset tag is left alone. An untagged card
 * written while mods are active is tagged with the running modset and
 * logged. Any other mismatch, including a tagged card written with no mods
 * active, leaves the tag unchanged and warns once per slot, so the
 * mismatch keeps being reported until --adopt-saves retags the card. */
static void mc_modset_on_write(int slot) {
    const char* cur = openpete_mods_modset_tag();
    const char* tag = s_ms_tag[slot];
    if (strcmp(tag, cur) == 0) return;
    if (!tag[0] && cur[0]) {
        mc_modset_write_sidecar(slot, cur);
        OPL_INFO_F("memcard", "card %d: first save under modset \"%s\" - "
                   "tagged", slot, cur);
        return;
    }
    if (!s_ms_write_warned[slot]) {
        s_ms_write_warned[slot] = 1;
        OPL_WARN_F("memcard", "card %d written under a modset that does "
                   "not match its tag (\"%s\" vs running \"%s\"); tag "
                   "unchanged - pass --adopt-saves to re-tag",
                   slot, tag, cur[0] ? cur : "vanilla");
    }
}

/** @brief Report whether the slot refuses writes, logging the reason once per slot.
 *
 * @param slot Card slot, 0 or 1.
 * @return 1 when the slot's image is read-only or memory-only, else 0. */
static int mc_write_refused(int slot)
{
    if (!s_readonly[slot] && !s_mem_only[slot] && !s_write_failed[slot]) return 0;
    if (!s_refuse_warned[slot]) {
        s_refuse_warned[slot] = 1;
        OPL_ERROR_F("memcard", "card %d: save refused - %s %s", slot,
                    s_mem_only[slot] ? "no card file is usable (no card directory "
                                       "resolves, or the file could not be read)"
                                     : "the image is a headered format loaded read-only",
                    openpete_error_ref(s_mem_only[slot] ? OP_E_CARD_OPEN : OP_E_CARD_READONLY));
    }
    return 1;
}

/** @brief Mark the slot as failing writes after a write to its file failed, logging it once.
 *
 * Every later write is refused without touching the file and reports a
 * card error to the game, as a card that fails its writes does, so a game
 * that retries the write does not repeat the file I/O and its log lines.
 * @param slot Card slot, 0 or 1. */
static void mc_mark_write_failed(int slot)
{
    s_write_failed[slot] = 1;
    s_refuse_warned[slot] = 1;
    OPL_ERROR_F("memcard", "card %d: writing %s failed; the card reports a write "
                "error to the game for the rest of the session %s", slot, s_path[slot],
                openpete_error_ref(OP_E_CARD_WRITE));
}

/** @brief Report whether a slot write is suppressed because the tick is speculative (runahead).
 *
 * Runahead lookahead: the mutation belongs to a timeline that is about to
 * be rolled back, and persisting it would leak a phantom save to disk. The
 * real tick re-runs the same guest code and writes for real. s_card[]
 * itself is host state; a speculative mutation the real tick does not
 * repeat lingers in the in-memory card until the next real write. Card
 * writes happen at save points, which are not gameplay-live, and runahead
 * is gated on liveness.
 * @param slot Card slot, 0 or 1, named in the warning.
 * @return 1 when the write is suppressed. */
static int mc_speculative_suppressed(int slot)
{
    if (!openpete_speculative) return 0;
    OPL_WARN_F("memcard", "flush during speculative tick suppressed (slot %d)", slot);
    return 1;
}

/** @brief Write the slot's card image to its file atomically and apply the modset tag rule for a write.
 *
 * Called after every mutation, so the file is written through. No-op when
 * the slot is not loaded or the tick is speculative. On a refused or failed
 * write the in-memory image is restored to the last persisted one, so the
 * card the game sees stays the card on disk.
 * @param slot Card slot, 0 or 1.
 * @return 0 when the image is on disk (or nothing was due), -1 when the write failed. */
static int mc_flush(int slot)
{
    if (!s_loaded[slot] || mc_speculative_suppressed(slot)) return 0;
    int failed = mc_write_refused(slot);
    if (!failed && op_atomic_write_file(s_path[slot], s_card[slot], MC_CARD_SIZE) != 0) {
        mc_mark_write_failed(slot);
        failed = 1;
    }
    if (failed) {
        memcpy(s_card[slot], s_disk[slot], MC_CARD_SIZE);
        return -1;
    }
    memcpy(s_disk[slot], s_card[slot], MC_CARD_SIZE);
    mc_modset_on_write(slot);
    return 0;
}

/** @brief Write one 128-byte frame of the slot's card image in place and apply the modset tag rule for a write.
 *
 * The BIOS card path changes the image one frame per command; a frame is
 * smaller than a device sector, so it is written where it lies instead of
 * rewriting the 128 KiB image. A file that cannot take the in-place write
 * (missing, or replaced by another program) falls back to the whole-image
 * atomic write. On a refused or failed write the frame is restored from
 * the last persisted image.
 * @param slot Card slot, 0 or 1.
 * @param frame Frame number, 0 to 1023.
 * @return 0 when the frame is on disk (or nothing was due), -1 when the write failed. */
static int mc_flush_frame(int slot, int frame)
{
    if (!s_loaded[slot] || mc_speculative_suppressed(slot)) return 0;
    const size_t at = (size_t)frame * MC_SECTOR_SIZE;
    if (mc_write_refused(slot)) {
        memcpy(s_card[slot] + at, s_disk[slot] + at, MC_SECTOR_SIZE);
        return -1;
    }
    if (op_file_write_at(s_path[slot], (long)at, s_card[slot] + at, MC_SECTOR_SIZE) != 0)
        return mc_flush(slot);
    memcpy(s_disk[slot] + at, s_card[slot] + at, MC_SECTOR_SIZE);
    mc_modset_on_write(slot);
    return 0;
}

/* Headered card-image containers: a fixed header, then the raw 128 KiB
 * image. Detected by total size and magic, loaded read-only. */
typedef struct {
    const char* name;
    long        size;       /* header + MC_CARD_SIZE */
    long        header;
    const char* magic;
    size_t      magic_len;
} mc_container_t;

static const mc_container_t k_containers[] = {
    { "DexDrive .gme",  3904 + MC_CARD_SIZE, 3904, "123-456-STD", 11 },
    { "VGS .mem",       64 + MC_CARD_SIZE,   64,   "VgsM",        4 },
    { "PSP/PS3 .vmp",   128 + MC_CARD_SIZE,  128,  "\0PMV",       4 },
};

/** @brief Load the payload of a headered image of a known container format into the slot, read-only.
 *
 * @param slot Card slot, 0 or 1.
 * @param f The image, open for reading.
 * @param size Its size in bytes.
 * @return 1 when a container matched and its payload was read, else 0. */
static int mc_load_container(int slot, FILE* f, long size)
{
    for (size_t i = 0; i < sizeof k_containers / sizeof k_containers[0]; i++) {
        const mc_container_t* c = &k_containers[i];
        char magic[16];
        if (size != c->size || fseek(f, 0, SEEK_SET) != 0 ||
            fread(magic, 1, c->magic_len, f) != c->magic_len ||
            memcmp(magic, c->magic, c->magic_len) != 0)
            continue;
        if (fseek(f, c->header, SEEK_SET) != 0 ||
            fread(s_card[slot], 1, MC_CARD_SIZE, f) != MC_CARD_SIZE)
            return 0;
        s_readonly[slot] = 1;
        OPL_WARN_F("memcard", "card %d: %s is a %s image; loaded read-only - "
                   "saves are refused until it is converted to a raw 128 KiB .mcr %s",
                   slot, s_path[slot], c->name, openpete_error_ref(OP_E_CARD_READONLY));
        return 1;
    }
    return 0;
}

/** @brief Rename the slot's image aside to the first free "<path>.bad-<n>", n from 1 to 999.
 *
 * @param slot Card slot, 0 or 1.
 * @return 0 when renamed, -1 when no free name exists or the rename fails. */
static int mc_set_aside(int slot)
{
    char aside[1300];
    for (int n = 1; n < 1000; n++) {
        snprintf(aside, sizeof aside, "%s.bad-%d", s_path[slot], n);
        FILE* probe = fopen(aside, "rb");
        if (probe) { fclose(probe); continue; }
        if (rename(s_path[slot], aside) != 0) break;
        OPL_ERROR_F("memcard", "card %d: %s is not a 128 KiB card image; moved to %s "
                    "and a new card provisioned %s", slot, s_path[slot], aside,
                    openpete_error_ref(OP_E_CARD_BAD));
        return 0;
    }
    OPL_ERROR_F("memcard", "card %d: %s is not a 128 KiB card image and cannot be "
                "moved aside (%s); the slot runs in memory and saves are refused %s",
                slot, s_path[slot], strerror(errno), openpete_error_ref(OP_E_CARD_BAD));
    return -1;
}

enum { MC_FILE_LOADED, MC_FILE_ABSENT, MC_FILE_UNUSABLE };

/** @brief Read the slot's image file into s_card, classifying what the file holds.
 *
 * A raw 131072-byte image loads as is and a known headered container loads
 * read-only. A file that exists but cannot be opened or read is unusable,
 * so provisioning never replaces a card it could not read. Any other file
 * (truncated, oversized, unknown header) is renamed aside so that
 * provisioning never writes over it.
 * @param slot Card slot, 0 or 1.
 * @return MC_FILE_LOADED, MC_FILE_ABSENT (no file, or moved aside), or
 *         MC_FILE_UNUSABLE (a bad file that could not be moved aside). */
static int mc_load_file(int slot)
{
    FILE* f = fopen(s_path[slot], "rb");
    if (!f) {
        if (errno == ENOENT) return MC_FILE_ABSENT;
        OPL_ERROR_F("memcard", "card %d: %s cannot be opened (%s); the slot runs in "
                    "memory and saves are refused %s", slot, s_path[slot], strerror(errno),
                    openpete_error_ref(OP_E_CARD_OPEN));
        return MC_FILE_UNUSABLE;
    }
    long size = (fseek(f, 0, SEEK_END) == 0) ? ftell(f) : -1;
    int loaded = 0;
    int read_failed = size < 0;
    if (size == MC_CARD_SIZE) {
        loaded = fseek(f, 0, SEEK_SET) == 0 &&
                 fread(s_card[slot], 1, MC_CARD_SIZE, f) == MC_CARD_SIZE;
        read_failed = !loaded;
    } else if (size > 0) {
        loaded = mc_load_container(slot, f, size);
        read_failed = !loaded && ferror(f);
    }
    fclose(f);
    if (loaded) return MC_FILE_LOADED;
    if (read_failed) {
        OPL_ERROR_F("memcard", "card %d: %s could not be read; the slot runs in memory "
                    "and saves are refused %s", slot, s_path[slot],
                    openpete_error_ref(OP_E_CARD_OPEN));
        return MC_FILE_UNUSABLE;
    }
    return mc_set_aside(slot) == 0 ? MC_FILE_ABSENT : MC_FILE_UNUSABLE;
}

/** @brief Load the slot's card image on first access, or provision one when the file is absent or unusable.
 *
 * A loaded image runs the modset tag check. A provisioned image is
 * formatted, or zeroed under --card-unformatted, and written to the file at
 * once, which tags it under the active modset. When no image path
 * resolves, or a bad file cannot be moved aside, the slot gets an in-memory
 * card, formatted or zeroed under --card-unformatted, whose writes are
 * refused, so a save reports a card error to the game instead of being lost
 * at exit.
 * @param slot Card slot, 0 or 1. */
static void mc_ensure_loaded(int slot)
{
    if (s_loaded[slot]) return;
    int st = MC_FILE_UNUSABLE;
    if (mc_resolve_path(slot) != 0)
        OPL_ERROR_F("memcard", "card %d: no card directory resolves; the slot runs "
                    "in memory and saves are refused %s", slot,
                    openpete_error_ref(OP_E_CARD_OPEN));
    else
        st = mc_load_file(slot);
    if (st == MC_FILE_LOADED) {
        memcpy(s_disk[slot], s_card[slot], MC_CARD_SIZE);
        s_loaded[slot] = 1;
        OPL_INFO_F("memcard", "loaded card %d from %s", slot, s_path[slot]);
        mc_modset_check_open(slot);
        return;
    }

    if (s_unformatted) memset(s_card[slot], 0, MC_CARD_SIZE);
    else               mc_format_image(s_card[slot]);
    memcpy(s_disk[slot], s_card[slot], MC_CARD_SIZE);
    s_loaded[slot] = 1;
    if (st == MC_FILE_UNUSABLE) {
        s_mem_only[slot] = 1;
        return;
    }
    /* First run for this slot: materialize on disk. The flush birth-tags
     * the fresh card under the active modset. */
    if (mc_flush(slot) == 0)
        OPL_INFO_F("memcard", "provisioned %s card %d at %s",
                   s_unformatted ? "unformatted" : "formatted", slot, s_path[slot]);
}

/** @brief Return the slot's directory frame for data block block, sector block of the image.
 *
 * The pointer aims into s_card and is not range-checked; block is 1 to 15.
 * @param slot Card slot, 0 or 1.
 * @param block Data block number, 1 to 15. */
static uint8_t* mc_dir_frame(int slot, int block)
{
    return &s_card[slot][block * MC_SECTOR_SIZE];
}

/// @brief Return the little-endian u32 state word at the start of a directory frame.
static uint32_t mc_frame_state(const uint8_t* de)
{
    return (uint32_t)de[0] | ((uint32_t)de[1] << 8) |
           ((uint32_t)de[2] << 16) | ((uint32_t)de[3] << 24);
}

/** @brief Return the directory block (1 to 15) whose first-block frame names file name, or -1.
 *
 * Only frames in state 0x51, the first block of a file, are considered; the
 * name is compared over the MC_NAME_MAX bytes at offset 0x0A. */
static int mc_find_file(int slot, const char* name)
{
    for (int blk = 1; blk <= 15; blk++) {
        const uint8_t* de = mc_dir_frame(slot, blk);
        if (mc_frame_state(de) != 0x51) continue;   /* first-block frames only */
        if (strncmp((const char*)&de[0x0A], name, MC_NAME_MAX) == 0)
            return blk;
    }
    return -1;
}

/** @brief Return the libmcrd result code for a slot's presence, format and new-card state.
 *
 * Checks in order: slot absent gives MC_ERR_CARD_NOT_EXIST; otherwise the
 * image is loaded and an unformatted image gives MC_ERR_NOT_FORMAT; a set
 * new-card flag gives MC_ERR_NEW_CARD once and is cleared, so the next
 * call on the same card returns MC_ERR_NONE. The caller (mc_probe) latches
 * the result.
 * @param slot Card slot, 0 or 1.
 * @return One of MC_ERR_CARD_NOT_EXIST, MC_ERR_NOT_FORMAT, MC_ERR_NEW_CARD, MC_ERR_NONE. */
static int mc_verdict(int slot)
{
    if (!s_present[slot]) return MC_ERR_CARD_NOT_EXIST;
    mc_ensure_loaded(slot);
    if (!mc_is_formatted(slot)) return MC_ERR_NOT_FORMAT;
    if (s_new_card[slot]) { s_new_card[slot] = 0; return MC_ERR_NEW_CARD; }
    return MC_ERR_NONE;
}

/** @brief Record a finished command and its result for the game's MemCardSync poll.
 *
 * Host I/O completes inside the call, so the status is always finished.
 * @param cmds Command code, one of the MC_FUNC values.
 * @param rslt Result code, an MC_ERR value. */
static void mc_latch(int cmds, int rslt)
{
    s_mc_cmds   = cmds;
    s_mc_rslt   = rslt;
    s_mc_status = MC_SYNC_FINISHED;   /* host I/O is instant */
}

/* --- public API ----------------------------------------------------------- */

void openpete_memcard_set_dir_override(const char* dir)
{
    if (dir && *dir) snprintf(s_dir_override, sizeof(s_dir_override), "%s", dir);
    else             s_dir_override[0] = '\0';
}

void openpete_memcard_set_paths(const char* card_a, const char* card_b)
{
    const char* p[2] = { card_a, card_b };
    for (int slot = 0; slot < 2; slot++) {
        if (p[slot] && *p[slot]) {
            snprintf(s_path_override[slot], sizeof(s_path_override[slot]), "%s", p[slot]);
            s_present[slot] = 1;   /* an explicit path forces the slot present */
        } else {
            s_path_override[slot][0] = '\0';
        }
    }
}

void openpete_memcard_set_presence(int slot, int present)
{
    if (slot == 0 || slot == 1) s_present[slot] = present ? 1 : 0;
}

void openpete_memcard_set_adopt(int on)
{
    s_adopt = on ? 1 : 0;
}

/** @brief Report whether the folder holding the slot's resolved image path exists, naming it in folder.
 *
 * The folder is the path up to its last separator; a path with none lies
 * in the working directory, which exists.
 * @param slot Card slot, 0 or 1.
 * @param folder Receives the folder, or "." for a bare file name.
 * @param cap Size of folder in bytes.
 * @return 1 when the folder exists and is a directory, else 0. */
static int mc_folder_exists(int slot, char* folder, size_t cap)
{
    const char* p = s_path[slot];
    const char* sep = strrchr(p, '/');
#ifdef _WIN32
    const char* bs = strrchr(p, '\\');
    if (bs && (!sep || bs > sep)) sep = bs;
#endif
    if (!sep) { snprintf(folder, cap, "."); return 1; }
    size_t n = (size_t)(sep - p);
    if (n == 0) n = 1;                       /* "/card.mcr": the root */
#ifdef _WIN32
    if (n == 2 && p[1] == ':') n = 3;        /* "C:\card.mcr": the drive root */
#endif
    snprintf(folder, cap, "%.*s", (int)n, p);
    struct stat st;
    return stat(folder, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

int openpete_memcard_startup_check(void)
{
    /* Spyro only touches the card from its save UI, but the modset
     * warning is about the LAUNCH configuration - surface it at startup.
     * Existing images only: a fresh dir has no progress to protect (the
     * game's first real write birth-tags via mc_flush). */
    int rc = 0;
    for (int slot = 0; slot < 2; slot++) {
        if (!s_present[slot]) continue;
        if (mc_resolve_path(slot) != 0) continue;
        char folder[1280];
        if (!mc_folder_exists(slot, folder, sizeof folder)) {
            if (s_path_override[slot][0])
                OPL_ERROR_F("memcard", "--card%d %s: the folder %s does not exist; create it "
                            "or name a card file in an existing folder %s",
                            slot + 1, s_path_override[slot], folder,
                            openpete_error_ref(OP_E_CARD_OPEN));
            else
                OPL_ERROR_F("memcard", "card %d: the card folder %s does not exist and "
                            "could not be created %s", slot, folder,
                            openpete_error_ref(OP_E_CARD_OPEN));
            rc = -1;
            continue;
        }
        FILE* f = fopen(s_path[slot], "rb");
        if (!f) continue;
        fclose(f);
        mc_modset_check_open(slot);
    }
    return rc;
}

void openpete_memcard_set_unformatted(int on)
{
    s_unformatted = on ? 1 : 0;
}

void openpete_memcard_init(void)
{
    s_mc_cmds   = -1;
    s_mc_rslt   = -1;
    s_mc_status = MC_SYNC_NOTHING;
    s_new_card[0] = s_new_card[1] = 1;
}

void openpete_memcard_start(void) { /* comms always ready */ }

void openpete_memcard_stop(void)
{
    s_mc_cmds   = -1;
    s_mc_rslt   = -1;
    s_mc_status = MC_SYNC_NOTHING;
}

/** @brief Map a channel in either encoding to its slot, 0 or 1.
 *
 * libmcrd passes the raw PSX port (port A = 0x00, port B = 0x10) and the
 * BIOS card API passes a plain 0 or 1; both reach the same slot. A value
 * with low-nibble bits takes bit 0, else bit 4. */
static int mc_chan_slot(int chan)
{
    return (chan & 0x0F) ? (chan & 1) : ((chan >> 4) & 1);
}

/** @brief Probe the card in port chan for command func and latch the result.
 *
 * The slot is bit 4 of chan, the libmcrd port encoding. The result comes
 * from mc_verdict and is latched with func.
 * @param chan Raw PSX port, 0x00 for slot 0 or 0x10 for slot 1.
 * @param func MC_FUNC_EXIST or MC_FUNC_ACCEPT.
 * @param out_cmds Receives func; may be NULL.
 * @param out_rslt Receives the result code; may be NULL.
 * @return 0 when the slot holds no card, else 1. */
static int mc_probe(int chan, int func, int* out_cmds, int* out_rslt)
{
    int slot = (chan >> 4) & 1;
    int rslt = mc_verdict(slot);
    mc_latch(func, rslt);
    if (out_cmds) *out_cmds = func;
    if (out_rslt) *out_rslt = rslt;
    return (rslt == MC_ERR_CARD_NOT_EXIST) ? 0 : 1;   /* v0: 1 = present */
}

int openpete_memcard_exist(int chan, int* out_cmds, int* out_rslt)
{
    return mc_probe(chan, MC_FUNC_EXIST, out_cmds, out_rslt);
}

int openpete_memcard_accept(int chan, int* out_cmds, int* out_rslt)
{
    return mc_probe(chan, MC_FUNC_ACCEPT, out_cmds, out_rslt);
}

/** @brief Return 1 when bytes bytes at offset ofs of block block lie inside the card image, else 0.
 *
 * Negative ofs or bytes fail. The range is checked against the whole
 * 128 KiB image, not the file's own blocks, so a range may run past the
 * file's first block into the blocks that follow it in the image. */
static int mc_range_ok(int block, int ofs, int bytes)
{
    if (ofs < 0 || bytes < 0) return 0;
    long start = (long)block * MC_BLOCK_SIZE + ofs;
    return start >= 0 && start + bytes <= MC_CARD_SIZE;
}

int openpete_memcard_read(int chan, const char* name, void* dst_host,
                          int ofs, int bytes)
{
    int slot = (chan >> 4) & 1;
    if (!s_present[slot]) { mc_latch(MC_FUNC_READ, MC_ERR_CARD_NOT_EXIST); return MC_ERR_CARD_NOT_EXIST; }
    mc_ensure_loaded(slot);

    int blk = mc_find_file(slot, name);
    if (blk < 0)  { mc_latch(MC_FUNC_READ, MC_ERR_FILE_NOT_EXIST); return MC_ERR_FILE_NOT_EXIST; }
    if (!dst_host || !mc_range_ok(blk, ofs, bytes)) {
        OPL_ERROR_F("memcard", "read %s ofs=%d bytes=%d out of range", name, ofs, bytes);
        mc_latch(MC_FUNC_READ, MC_ERR_FILE_NOT_EXIST);
        return MC_ERR_FILE_NOT_EXIST;
    }
    memcpy(dst_host, &s_card[slot][blk * MC_BLOCK_SIZE + ofs], (size_t)bytes);
    mc_latch(MC_FUNC_READ, MC_ERR_NONE);
    return MC_ERR_NONE;
}

int openpete_memcard_write(int chan, const char* name, const void* src_host,
                           int ofs, int bytes)
{
    int slot = (chan >> 4) & 1;
    if (!s_present[slot]) { mc_latch(MC_FUNC_WRITE, MC_ERR_CARD_NOT_EXIST); return MC_ERR_CARD_NOT_EXIST; }
    mc_ensure_loaded(slot);

    int blk = mc_find_file(slot, name);
    if (blk < 0)  { mc_latch(MC_FUNC_WRITE, MC_ERR_FILE_NOT_EXIST); return MC_ERR_FILE_NOT_EXIST; }
    if (!src_host || !mc_range_ok(blk, ofs, bytes)) {
        OPL_ERROR_F("memcard", "write %s ofs=%d bytes=%d out of range", name, ofs, bytes);
        mc_latch(MC_FUNC_WRITE, MC_ERR_FILE_NOT_EXIST);
        return MC_ERR_FILE_NOT_EXIST;
    }
    memcpy(&s_card[slot][blk * MC_BLOCK_SIZE + ofs], src_host, (size_t)bytes);
    int rslt = mc_flush(slot) == 0 ? MC_ERR_NONE : MC_ERR_CARD_INVALID;
    mc_latch(MC_FUNC_WRITE, rslt);
    return rslt;
}

int openpete_memcard_create(int chan, const char* name, int blocks)
{
    int slot = (chan >> 4) & 1;
    if (!s_present[slot]) return MC_ERR_CARD_NOT_EXIST;
    mc_ensure_loaded(slot);
    if (!mc_is_formatted(slot)) return MC_ERR_NOT_FORMAT;
    if (blocks < 1) blocks = 1;
    if (mc_find_file(slot, name) >= 0) return MC_ERR_ALREADY_EXIST;

    /* Gather `blocks` free blocks. */
    int chain[15], n = 0;
    for (int blk = 1; blk <= 15 && n < blocks; blk++) {
        uint32_t st = mc_frame_state(mc_dir_frame(slot, blk));
        if (st == 0xA0 || st == 0x00) chain[n++] = blk;
    }
    if (n < blocks) return MC_ERR_BLOCK_FULL;

    for (int i = 0; i < blocks; i++) {
        uint8_t* de = mc_dir_frame(slot, chain[i]);
        memset(de, 0, MC_SECTOR_SIZE);
        uint32_t state = (blocks == 1) ? 0x51u
                       : (i == 0)       ? 0x51u          /* first */
                       : (i == blocks-1)? 0x53u          /* last  */
                       :                  0x52u;         /* middle */
        de[0] = (uint8_t)(state & 0xFF);
        de[1] = (uint8_t)((state >> 8) & 0xFF);
        de[2] = (uint8_t)((state >> 16) & 0xFF);
        de[3] = (uint8_t)((state >> 24) & 0xFF);
        if (i == 0) {
            uint32_t size = (uint32_t)blocks * MC_BLOCK_SIZE;
            de[4] = (uint8_t)(size & 0xFF);
            de[5] = (uint8_t)((size >> 8) & 0xFF);
            de[6] = (uint8_t)((size >> 16) & 0xFF);
            de[7] = (uint8_t)((size >> 24) & 0xFF);
            strncpy((char*)&de[0x0A], name, MC_NAME_MAX);
        }
        uint16_t next = (i == blocks - 1) ? 0xFFFFu : (uint16_t)chain[i + 1];
        de[8] = (uint8_t)(next & 0xFF);
        de[9] = (uint8_t)((next >> 8) & 0xFF);
        de[MC_SECTOR_SIZE - 1] = mc_frame_checksum(de);
    }
    return mc_flush(slot) == 0 ? MC_ERR_NONE : MC_ERR_CARD_INVALID;
}

int openpete_memcard_format(int chan)
{
    int slot = (chan >> 4) & 1;
    if (!s_present[slot]) return MC_ERR_CARD_NOT_EXIST;
    mc_ensure_loaded(slot);
    mc_format_image(s_card[slot]);
    return mc_flush(slot) == 0 ? MC_ERR_NONE : MC_ERR_CARD_INVALID;
}

int openpete_memcard_sync(int mode, int* out_cmds, int* out_rslt)
{
    (void)mode;   /* sync and async both complete instantly */
    /* The game's libmcrd MemCardSync reports a finished
     * command once: it clears the completion flag as it returns 1, and a
     * later call with nothing pending returns -1 and leaves the out-slots
     * alone. */
    if (s_mc_status != MC_SYNC_FINISHED) return MC_SYNC_NOTHING;
    if (out_cmds && s_mc_cmds != -1) *out_cmds = s_mc_cmds;
    if (out_rslt && s_mc_rslt != -1) *out_rslt = s_mc_rslt;
    s_mc_status = MC_SYNC_NOTHING;
    return MC_SYNC_FINISHED;
}

/* --- raw frame access ----------------------------------------------------
 *
 * The BIOS card API addresses the card as 1024 numbered 128-byte frames and
 * knows nothing of the directory the libmcrd surface above works through.
 * Both reach the same image, so a save written through either is readable
 * through the other and by any emulator or real card.
 */

int openpete_memcard_present(int chan)
{
    int slot = mc_chan_slot(chan);
    return s_present[slot] ? 1 : 0;
}

int openpete_memcard_read_frame(int chan, int frame, void* dst_host)
{
    int slot = mc_chan_slot(chan);
    if (!s_present[slot] || !dst_host) return -1;
    if (frame < 0 || frame >= MC_CARD_SIZE / MC_SECTOR_SIZE) return -1;
    mc_ensure_loaded(slot);
    memcpy(dst_host, s_card[slot] + (size_t)frame * MC_SECTOR_SIZE,
           MC_SECTOR_SIZE);
    return 0;
}

int openpete_memcard_write_frame(int chan, int frame, const void* src_host)
{
    int slot = mc_chan_slot(chan);
    if (!s_present[slot] || !src_host) return -1;
    if (frame < 0 || frame >= MC_CARD_SIZE / MC_SECTOR_SIZE) return -1;
    mc_ensure_loaded(slot);
    memcpy(s_card[slot] + (size_t)frame * MC_SECTOR_SIZE, src_host,
           MC_SECTOR_SIZE);
    return mc_flush_frame(slot, frame);
}
