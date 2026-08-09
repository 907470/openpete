// games/spyro-1/mod_wad_overlay.c — the asset overlay's level-segment
// namespace (PoC ladder rung 9, notes/plans/openpete-modding-framework.md).
//
// Spyro's loader pulls everything from WAD.WAD by sector range, so the
// overlay key is LOGICAL identity, not a path: per-level {overlay, data}
// entries from g_WadHeader's TOC. A mod ships
//   assets/wad/level<ID>.data.bin      (ID = logical level id, e.g. 10)
// and every CdRead that lands inside that level's Data entry is served
// the mod's bytes instead of the disc's (same size required in v1 —
// resizing is P3 capacity work). The highest-priority ENABLED mod wins,
// same single priority truth as every other namespace.
//
// Mechanism: psyq_thunks snoops CdlSetloc to track the head position,
// psyq_CdRead calls mod_wad_apply() after the drain — the guest buffer
// gets the override bytes before the game's loader parses them, i.e.
// the CdRead chokepoint exactly as the plan specified. The TOC is read
// host-side from the user's own disc image (cue -> bin, WAD at the
// game's wad_sector), lazily on first use.
//
// Live enable/disable: the winning override is resolved against the
// current enabled set, but only ONCE PER LEVEL LOAD — see the latch in
// mod_wad_apply(). A toggle therefore lands at the next level load and
// can never splice stock and override bytes into one level image.
//
// Legal posture note: mods may NOT ship derived game data. This
// namespace exists for (a) locally-derived blobs (generated on the
// user's machine from their own disc — the CI fixtures do exactly this)
// and (b) future genuinely-novel content (P3 custom levels).

#include "mod_wad_overlay.h"

#include "game_extras.h"
#include "openpete_log.h"
#include "openpete_mods.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define WAD_LEVELS      36
#define WAD_HDR_LEVELS  72   /* byte offset of m_LevelEntry[0] in g_WadHeader */
#define SEC_DATA        2048
#define SEC_RAW         2352
#define SEC_RAW_SKEW    24   /* mode2/form1 user data offset in a raw sector */

typedef struct {
    int      present;       /* TOC entry valid */
    uint32_t start_sector;  /* absolute disc sector of the Data entry */
    uint32_t n_sectors;
    uint32_t length;        /* entry byte length */
    int      latched;       /* live_root decided for the current load */
    int      live_root;     /* asset root serving this load, -1 = the disc */
} wad_level_t;

static char        s_disc_path[1024];
static int         s_state = 0;     /* 0 unconfigured, 1 pending, 2 ready, -1 failed */
static wad_level_t s_levels[WAD_LEVELS];
static FILE*       s_bin = NULL;
static int         s_raw = 0;       /* 1 = 2352-byte raw sectors */
static uint32_t    s_next_sector = 0;

/* Savestate participation (game_state_extra): the append cursor is host
 * state the guest depends on — TOC entries written into restored guest RAM
 * point at sectors this cursor allocated. Restoring RAM without the cursor
 * would let the next mod-asset append reuse sectors the restored TOC still
 * references. */
uint32_t spyro1_mod_wad_cursor_get(void)        { return s_next_sector; }
void     spyro1_mod_wad_cursor_set(uint32_t s)  { s_next_sector = s; }

void spyro1_mod_wad_configure(const char* disc_path) {
    if (!disc_path || !disc_path[0]) return;
    if (s_state == 2 && strcmp(s_disc_path, disc_path) == 0) return;
    snprintf(s_disc_path, sizeof s_disc_path, "%s", disc_path);
    s_state = 1;
}

/* cue -> first BINARY file, resolved relative to the cue's directory.
 * A .bin/.img/.iso path is used as-is. When the cue's FILE entry names a
 * bin that isn't on disk (renamed bin, stale cue), fall back to the
 * sibling same-basename .bin — the same tolerance the runtime's own
 * disc loader applies, so a disc that boots also serves WAD reads. */
static int resolve_bin(char* out, size_t cap) {
    const char* dot = strrchr(s_disc_path, '.');
    if (dot && (strcasecmp(dot, ".cue") == 0)) {
        FILE* f = fopen(s_disc_path, "r");
        if (!f) return -1;
        char line[512], name[400] = "";
        while (fgets(line, sizeof line, f)) {
            char* q0 = strchr(line, '"');
            if (!q0 || !strstr(line, "FILE")) continue;
            char* q1 = strchr(q0 + 1, '"');
            if (!q1) continue;
            snprintf(name, sizeof name, "%.*s", (int)(q1 - q0 - 1), q0 + 1);
            break;
        }
        fclose(f);
        if (name[0]) {
            const char* slash = strrchr(s_disc_path, '/');
            if (slash && name[0] != '/')
                snprintf(out, cap, "%.*s%s", (int)(slash - s_disc_path + 1),
                         s_disc_path, name);
            else
                snprintf(out, cap, "%s", name);
            FILE* b = fopen(out, "rb");
            if (b) { fclose(b); return 0; }
        }
        char sib[1200];
        size_t n = strlen(s_disc_path);
        if (n >= 4 && n + 1 <= sizeof sib) {
            memcpy(sib, s_disc_path, n + 1);
            memcpy(sib + n - 4, ".bin", 4);
            FILE* b = fopen(sib, "rb");
            if (b) { fclose(b); snprintf(out, cap, "%s", sib); return 0; }
        }
        return name[0] ? 0 : -1;   /* neither exists: report the cue-named path */
    }
    snprintf(out, cap, "%s", s_disc_path);
    return 0;
}

/* Read the 2048-byte user data of one absolute disc sector. */
static int read_sector(uint32_t sector, uint8_t* out) {
    long off = s_raw ? (long)sector * SEC_RAW + SEC_RAW_SKEW
                     : (long)sector * SEC_DATA;
    if (fseek(s_bin, off, SEEK_SET) != 0) return -1;
    return fread(out, 1, SEC_DATA, s_bin) == SEC_DATA ? 0 : -1;
}

static int level_index_to_id(int idx) {
    return (idx / 6 + 1) * 10 + idx % 6;
}

/* Build the override path level `idx` would take from asset root `root`.
 * Root indices are stable for the session (the loader only ever appends
 * roots; a disabled mod keeps its slot), so an index may be latched. */
static int wad_override_path(int idx, int root, char* out, size_t cap,
                             const char** owner_out) {
    const char* owner = NULL;
    const char* dir = openpete_mods_asset_root(root, &owner);
    if (!dir) return -1;
    snprintf(out, cap, "%s/wad/level%02d.data.bin", dir,
             level_index_to_id(idx));
    if (owner_out) *owner_out = owner;
    return 0;
}

/* Resolve the winning override for level `idx` against the CURRENT enabled
 * set: the highest-priority (= last-registered) asset root whose owner is
 * enabled and which ships a correctly-sized assets/wad/level<ID>.data.bin.
 * Returns that root index, or -1 for "no override — serve the disc".
 * path_out/owner_out are optional. */
static int wad_find_override(int idx, char* path_out, size_t cap,
                             const char** owner_out) {
    for (int r = openpete_mods_asset_root_count() - 1; r >= 0; r--) {
        const char* owner = NULL;
        char p[1088];
        if (wad_override_path(idx, r, p, sizeof p, &owner) != 0) continue;
        /* A live-disabled mod's assets must not reach the guest; the slot
         * falls through to the next enabled mod that supplies this level. */
        if (owner && owner[0] && !openpete_mods_is_enabled(owner)) continue;
        if (!owner || !owner[0]) owner = "?";
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if ((uint32_t)st.st_size != s_levels[idx].length) {
            OPL_WARN_F("wadmod", "%s: %s is %ld bytes but the level %d "
                       "Data entry is %u — size must match (v1); skipped",
                       owner, p, (long)st.st_size, level_index_to_id(idx),
                       s_levels[idx].length);
            continue;
        }
        if (path_out) snprintf(path_out, cap, "%s", p);
        if (owner_out) *owner_out = owner;
        return r;
    }
    return -1;
}

static void wad_init(void) {
    if (s_state != 1) return;

    /* No mod has an assets/ root yet: stay PENDING rather than failing.
     * A boot-disabled mod registers its root only when it is first
     * enabled (deferred entry), and the TOC still has to be parsed then. */
    if (openpete_mods_asset_root_count() == 0) return;

    s_state = -1; /* until proven otherwise */

    char bin[1200];
    if (resolve_bin(bin, sizeof bin) != 0) {
        OPL_WARN_F("wadmod", "cannot resolve disc image from %s",
                   s_disc_path);
        return;
    }
    s_bin = fopen(bin, "rb");
    if (!s_bin) {
        OPL_WARN_F("wadmod", "cannot open disc image %s", bin);
        return;
    }
    /* Raw (2352) vs cooked (2048): raw sectors open with the 12-byte
     * sync pattern 00 FF*10 00. */
    uint8_t probe[12] = {0};
    (void)fread(probe, 1, 12, s_bin);
    s_raw = (probe[0] == 0x00 && probe[1] == 0xFF && probe[11] == 0x00);

    const game_disc_layout_t* dl = game_get_disc_layout();
    uint32_t wad_sector = dl ? dl->wad_sector : 37;

    uint8_t hdr[SEC_DATA];
    if (read_sector(wad_sector, hdr) != 0) {
        OPL_WARN_F("wadmod", "cannot read WAD header sector %u", wad_sector);
        return;
    }
    int usable = 0;
    for (int i = 0; i < WAD_LEVELS; i++) {
        uint32_t off, len;
        memcpy(&off, hdr + WAD_HDR_LEVELS + i * 16 + 8, 4);  /* m_Data.m_Offset */
        memcpy(&len, hdr + WAD_HDR_LEVELS + i * 16 + 12, 4); /* m_Data.m_Length */
        if (len == 0 || (off & (SEC_DATA - 1)) != 0) continue;
        s_levels[i].present = 1;
        s_levels[i].start_sector = wad_sector + off / SEC_DATA;
        s_levels[i].n_sectors = (len + SEC_DATA - 1) / SEC_DATA;
        s_levels[i].length = len;
        s_levels[i].live_root = -1;
        usable++;
    }
    int overrides = 0;
    for (int i = 0; i < WAD_LEVELS; i++)
        if (s_levels[i].present &&
            wad_find_override(i, NULL, 0, NULL) >= 0) overrides++;
    OPL_INFO_F("wadmod", "WAD TOC: %d level data entries; %d override%s "
               "available", usable, overrides, overrides == 1 ? "" : "s");
    s_state = 2;
}

void mod_wad_setloc(uint32_t sector) {
    if (getenv("OPENPETE_WADMOD_TRACE"))
        OPL_INFO_F("wadmod", "setloc -> sector %u", sector);
    s_next_sector = sector;
}

void mod_wad_apply(uint8_t* host_buf, int sectors) {
    if (s_state == 1) wad_init();
    if (s_state != 2 || sectors <= 0) {
        s_next_sector += (uint32_t)(sectors > 0 ? sectors : 0);
        return;
    }
    uint32_t start = s_next_sector;
    s_next_sector += (uint32_t)sectors;
    if (getenv("OPENPETE_WADMOD_TRACE"))
        OPL_INFO_F("wadmod", "read [%u..%u)", start,
                   start + (uint32_t)sectors);

    for (int i = 0; i < WAD_LEVELS; i++) {
        wad_level_t* L = &s_levels[i];
        if (!L->present) continue;
        uint32_t lo = L->start_sector, hi = L->start_sector + L->n_sectors;
        uint32_t a = start > lo ? start : lo;
        uint32_t b = (start + (uint32_t)sectors) < hi
                         ? (start + (uint32_t)sectors) : hi;
        if (a >= b) continue;

        /* Per-load serve latch. The game streams one level's Data entry
         * across a dozen separate CdReads spread over many ticks
         * (LoadLevel stages 3..11) and keeps reading the same entry
         * in-level for the dragon cutscenes, so resolving the override
         * per read would splice stock and mod bytes into one level image
         * whenever an enable/disable landed between two of them. Resolve
         * once instead, at the read that starts at the entry's FIRST
         * sector — the level-header read that begins every load of that
         * level — and hold it for every later read of the entry. A read
         * that lands mid-entry with nothing latched (savestate restore,
         * level-slot clone) latches there. */
        char path[1088];
        const char* owner = NULL;
        if (!L->latched || start == lo) {
            L->latched = 1;
            L->live_root = wad_find_override(i, path, sizeof path, &owner);
            if (L->live_root >= 0)
                OPL_INFO_F("wadmod", "wad overlay: level %d data served "
                           "from %s (%s)", level_index_to_id(i), owner,
                           path);
        }
        if (L->live_root < 0) continue;
        if (wad_override_path(i, L->live_root, path, sizeof path, NULL) != 0)
            continue;
        if (getenv("OPENPETE_WADMOD_TRACE"))
            OPL_INFO_F("wadmod", "level %d serve [%u..%u)",
                       level_index_to_id(i), a, b);
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        for (uint32_t s = a; s < b; s++) {
            uint32_t rel = (s - lo) * SEC_DATA;
            uint32_t n = L->length - rel < SEC_DATA ? L->length - rel
                                                    : SEC_DATA;
            if (fseek(f, (long)rel, SEEK_SET) != 0) break;
            size_t got = fread(host_buf + (size_t)(s - start) * SEC_DATA,
                               1, n, f);
            (void)got;
        }
        fclose(f);
    }
}

/* Read a level's raw Data WAD entry host-side from the user's own disc image
 * (the same TOC the overlay uses). buf=NULL just probes the length. Returns the
 * entry byte length (>=0) or -1 if unavailable / buffer too small. Used by mods
 * that borrow another level's data (e.g. its cyclorama) at runtime, so no
 * derived game data is shipped. */
int spyro1_mod_wad_read_level_data(int level_id, void* buf, uint32_t cap) {
    if (s_state == 1) wad_init();
    if (s_state != 2) return -1;
    int idx = (level_id / 10 - 1) * 6 + level_id % 10;
    if (idx < 0 || idx >= WAD_LEVELS) return -1;
    wad_level_t* L = &s_levels[idx];
    if (!L->present) return -1;
    uint32_t len = L->length;
    if (buf) {
        if (cap < len) return -1;
        uint8_t sec[SEC_DATA];
        uint32_t done = 0;
        for (uint32_t i = 0; i < L->n_sectors && done < len; i++) {
            if (read_sector(L->start_sector + i, sec) != 0) return -1;
            uint32_t n = len - done < SEC_DATA ? len - done : SEC_DATA;
            memcpy((uint8_t*)buf + done, sec, n);
            done += n;
        }
    }
    return (int)len;
}
