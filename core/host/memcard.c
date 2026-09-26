/* memcard.c — host-backed PlayStation memory card. See openpete_memcard.h.
 *
 * Two raw 128 KiB card images (.mcr) with the standard PSX directory
 * structure, persisted write-through to disk. The layout / checksum /
 * free-block logic mirrors the (unlinked) reference in
 * third_party/psxrecomp/runner/src/runtime.c and the nocash PSX specs.
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
#include "openpete_log.h"
#include "openpete_paths.h"
#include "openpete_mods.h" /* openpete_mods_modset_tag — save safety */

extern int openpete_speculative; /* openpete_fiber.h — runahead lookahead */

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
static int     s_present[2]      = { 1, 1 };   /* both cards inserted by default */
static int     s_loaded[2]       = { 0, 0 };
static int     s_new_card[2]     = { 1, 1 };   /* "new card" latch, clears on 1st probe */
static char    s_path[2][1280];                /* resolved image path (once loaded) */
static char    s_path_override[2][1024];       /* --card1 / --card2 */
static char    s_dir_override[1024];           /* --card-dir */
static int     s_unformatted     = 0;          /* --card-unformatted */

/* Per-modset save safety (modding plan P3): a sidecar "<image>.modset"
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

/* Resolve the cards directory, creating it as needed. `cards` is a sibling
 * of savestate.c's `states` under whichever layout openpete_paths.h
 * resolved — the game folder for a portable install, the per-user state
 * directory otherwise. Returns 0 ok. */
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

/* Fill s_path[slot] with the image path for this slot. */
static int mc_resolve_path(int slot)
{
    if (s_path_override[slot][0]) {
        snprintf(s_path[slot], sizeof(s_path[slot]), "%s", s_path_override[slot]);
        return 0;
    }
    char dir[1024];
    if (mc_resolve_card_dir(dir, sizeof(dir)) != 0) return -1;
    snprintf(s_path[slot], sizeof(s_path[slot]), "%s%ccard%d.mcr",
             dir, MC_SEP, slot + 1);
    return 0;
}

static uint8_t mc_frame_checksum(const uint8_t* frame)
{
    uint8_t x = 0;
    for (int i = 0; i < MC_SECTOR_SIZE - 1; i++) x ^= frame[i];
    return x;
}

/* Write a freshly-formatted card image into `card`. */
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

static int mc_is_formatted(int slot)
{
    return s_card[slot][0] == 'M' && s_card[slot][1] == 'C';
}

/* --- per-modset save safety ------------------------------------------ */

static void mc_modset_sidecar_path(int slot, char* out, size_t cap) {
    snprintf(out, cap, "%s.modset", s_path[slot]);
}

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

static void mc_modset_write_sidecar(int slot, const char* tag) {
    char p[1300];
    mc_modset_sidecar_path(slot, p, sizeof p);
    FILE* f = fopen(p, "wb");
    if (!f) {
        OPL_ERROR_F("memcard", "cannot write %s: %s", p, strerror(errno));
        return;
    }
    fprintf(f, "%s\n", tag);
    fclose(f);
    snprintf(s_ms_tag[slot], sizeof s_ms_tag[slot], "%s", tag);
}

/* At card open: compare the sidecar tag against the running modset.
 * Mismatch WARNS and continues — progress is never blocked; the tag
 * only changes through the explicit --adopt-saves flow (or the
 * birth-tagging of a card that has no vanilla progress recorded). */
static int s_ms_checked[2];

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
                   "are active (%s) — saving will mix modded progress "
                   "into it; pass --adopt-saves to accept and tag",
                   slot, cur);
    else if (!cur[0])
        OPL_WARN_F("memcard", "card %d is tagged with modset \"%s\" but "
                   "no mods are active — its progress may depend on "
                   "them; pass --adopt-saves to re-tag as vanilla",
                   slot, tag);
    else
        OPL_WARN_F("memcard", "card %d modset MISMATCH: tagged \"%s\", "
                   "running \"%s\" — pass --adopt-saves to accept",
                   slot, tag, cur);
}

/* At each persisted write: birth-tag an untagged card under the active
 * modset (recorded truth, with a log); never silently CHANGE an
 * existing tag — a mismatched write warns once and leaves the tag, so
 * the mismatch keeps surfacing until explicitly adopted. */
static void mc_modset_on_write(int slot) {
    const char* cur = openpete_mods_modset_tag();
    const char* tag = s_ms_tag[slot];
    if (strcmp(tag, cur) == 0) return;
    if (!tag[0] && cur[0]) {
        mc_modset_write_sidecar(slot, cur);
        OPL_INFO_F("memcard", "card %d: first save under modset \"%s\" — "
                   "tagged", slot, cur);
        return;
    }
    if (!s_ms_write_warned[slot]) {
        s_ms_write_warned[slot] = 1;
        OPL_WARN_F("memcard", "card %d written under a modset that does "
                   "not match its tag (\"%s\" vs running \"%s\"); tag "
                   "unchanged — pass --adopt-saves to re-tag",
                   slot, tag, cur[0] ? cur : "vanilla");
    }
}

/* Persist a card image to disk (write-through after every mutation). */
static void mc_flush(int slot)
{
    if (!s_loaded[slot]) return;
    if (openpete_speculative) {
        /* Runahead lookahead: this mutation belongs to a timeline that is
         * about to be rolled back — persisting it would leak a phantom
         * save to disk. The real tick re-runs the same guest code and
         * flushes for real. (Residual: s_card[] itself is host state; a
         * speculative mutation the real tick doesn't repeat would linger
         * in the in-memory card until the next real write. Card writes
         * happen at save points, which are not gameplay-live, and
         * runahead is gated on liveness — accepted.) */
        OPL_WARN_F("memcard", "flush during speculative tick suppressed "
                   "(slot %d)", slot);
        return;
    }
    FILE* f = fopen(s_path[slot], "wb");
    if (!f) {
        OPL_ERROR_F("memcard", "fopen %s for write failed: %s",
                    s_path[slot], strerror(errno));
        return;
    }
    if (fwrite(s_card[slot], 1, MC_CARD_SIZE, f) != MC_CARD_SIZE)
        OPL_ERROR_F("memcard", "short write to %s", s_path[slot]);
    fclose(f);
    mc_modset_on_write(slot);
}

/* Load a card image on first access, or provision one if absent. */
static void mc_ensure_loaded(int slot)
{
    if (s_loaded[slot]) return;
    if (mc_resolve_path(slot) != 0) {
        /* No usable path — fall back to an in-memory formatted card so the
         * game still sees a working (if non-persistent) card. */
        mc_format_image(s_card[slot]);
        s_loaded[slot] = 1;
        return;
    }

    FILE* f = fopen(s_path[slot], "rb");
    if (f) {
        size_t got = fread(s_card[slot], 1, MC_CARD_SIZE, f);
        fclose(f);
        if (got == MC_CARD_SIZE) {
            s_loaded[slot] = 1;
            OPL_INFO_F("memcard", "loaded card %d from %s", slot, s_path[slot]);
            mc_modset_check_open(slot);
            return;
        }
        OPL_ERROR_F("memcard", "%s: short read (%zu/%d) — reprovisioning",
                    s_path[slot], got, MC_CARD_SIZE);
    }

    /* First run for this slot: provision + materialize on disk. The
     * flush birth-tags the fresh card under the active modset. */
    if (s_unformatted) memset(s_card[slot], 0, MC_CARD_SIZE);
    else               mc_format_image(s_card[slot]);
    s_loaded[slot] = 1;
    mc_flush(slot);
    OPL_INFO_F("memcard", "provisioned %s card %d at %s",
               s_unformatted ? "unformatted" : "formatted", slot, s_path[slot]);
}

static uint8_t* mc_dir_frame(int slot, int block)   /* block 1..15 */
{
    return &s_card[slot][block * MC_SECTOR_SIZE];
}

static uint32_t mc_frame_state(const uint8_t* de)
{
    return (uint32_t)de[0] | ((uint32_t)de[1] << 8) |
           ((uint32_t)de[2] << 16) | ((uint32_t)de[3] << 24);
}

/* Return the first (in-use) directory block holding file `name`, or -1. */
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

void openpete_memcard_startup_check(void)
{
    /* Spyro only touches the card from its save UI, but the modset
     * warning is about the LAUNCH configuration — surface it at startup.
     * Existing images only: a fresh dir has no progress to protect (the
     * game's first real write birth-tags via mc_flush). */
    for (int slot = 0; slot < 2; slot++) {
        if (!s_present[slot]) continue;
        if (mc_resolve_path(slot) != 0) continue;
        FILE* f = fopen(s_path[slot], "rb");
        if (!f) continue;
        fclose(f);
        mc_modset_check_open(slot);
    }
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

/* Both channel encodings reach the same slot: libmcrd passes the raw PSX port
 * (port A = 0x00, port B = 0x10) and the BIOS card API passes a plain 0 or 1. */
static int mc_chan_slot(int chan)
{
    return (chan & 0x0F) ? (chan & 1) : ((chan >> 4) & 1);
}

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

/* Bounds-check a (block, ofs, bytes) range against the card image. */
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
    mc_flush(slot);
    mc_latch(MC_FUNC_WRITE, MC_ERR_NONE);
    return MC_ERR_NONE;
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
    mc_flush(slot);
    return MC_ERR_NONE;
}

int openpete_memcard_format(int chan)
{
    int slot = (chan >> 4) & 1;
    if (!s_present[slot]) return MC_ERR_CARD_NOT_EXIST;
    if (mc_resolve_path(slot) != 0) { /* keep going with in-memory image */ }
    mc_format_image(s_card[slot]);
    s_loaded[slot] = 1;
    mc_flush(slot);
    return MC_ERR_NONE;
}

int openpete_memcard_sync(int mode, int* out_cmds, int* out_rslt)
{
    (void)mode;   /* sync and async both complete instantly */
    if (out_cmds && s_mc_cmds != -1) *out_cmds = s_mc_cmds;
    if (out_rslt && s_mc_rslt != -1) *out_rslt = s_mc_rslt;
    return s_mc_status;
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
    mc_modset_on_write(slot);
    memcpy(s_card[slot] + (size_t)frame * MC_SECTOR_SIZE, src_host,
           MC_SECTOR_SIZE);
    mc_flush(slot);
    return 0;
}
