// games/spyro-1/mod_wad_overlay.c — the asset overlay's level-segment
// namespace (PoC ladder rung 9, notes/plans/openpete-modding-framework.md).
//
// Spyro's loader pulls everything from WAD.WAD by sector range, so the
// overlay key is LOGICAL identity, not a path: per-level {overlay, data}
// entries from g_WadHeader's TOC. A mod ships
//   assets/wad/level<ID>.data.bin      (ID = logical level id, e.g. 10)
// and every CdRead that lands inside that level's Data entry is served
// the mod's bytes instead of the disc's (same size required in v1 —
// resizing is P3 capacity work). Later-enabled mods win, same single
// priority truth as every other namespace.
//
// Mechanism: psyq_thunks snoops CdlSetloc to track the head position,
// psyq_CdRead calls mod_wad_apply() after the drain — the guest buffer
// gets the override bytes before the game's loader parses them, i.e.
// the CdRead chokepoint exactly as the plan specified. The TOC is read
// host-side from the user's own disc image (cue -> bin, WAD at the
// game's wad_sector), lazily on first use.
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
    char     path[1088];    /* override file ("" = none) */
    char     owner[64];
    int      announced;     /* one log per load */
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
 * A .bin/.img/.iso path is used as-is. */
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
        if (!name[0]) return -1;
        const char* slash = strrchr(s_disc_path, '/');
        if (slash && name[0] != '/')
            snprintf(out, cap, "%.*s%s", (int)(slash - s_disc_path + 1),
                     s_disc_path, name);
        else
            snprintf(out, cap, "%s", name);
        return 0;
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

static void scan_mod_roots(void) {
    int n = openpete_mods_asset_root_count();
    for (int r = 0; r < n; r++) {
        const char* owner = NULL;
        const char* root = openpete_mods_asset_root(r, &owner);
        if (!root) continue;
        for (int i = 0; i < WAD_LEVELS; i++) {
            if (!s_levels[i].present) continue;
            char p[1088];
            snprintf(p, sizeof p, "%s/wad/level%02d.data.bin", root,
                     level_index_to_id(i));
            struct stat st;
            if (stat(p, &st) != 0) continue;
            if ((uint32_t)st.st_size != s_levels[i].length) {
                OPL_WARN_F("wadmod", "%s: %s is %ld bytes but the level %d "
                           "Data entry is %u — size must match (v1); "
                           "skipped", owner ? owner : "?", p,
                           (long)st.st_size, level_index_to_id(i),
                           s_levels[i].length);
                continue;
            }
            if (s_levels[i].path[0])
                OPL_INFO_F("wadmod", "%s overrides level %d data (was %s)",
                           owner ? owner : "?", level_index_to_id(i),
                           s_levels[i].owner);
            snprintf(s_levels[i].path, sizeof s_levels[i].path, "%s", p);
            snprintf(s_levels[i].owner, sizeof s_levels[i].owner, "%s",
                     owner ? owner : "?");
        }
    }
}

static void wad_init(void) {
    if (s_state != 1) return;
    s_state = -1; /* until proven otherwise */

    if (openpete_mods_asset_root_count() == 0) return; /* nothing to do */

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
        usable++;
    }
    scan_mod_roots();
    int overrides = 0;
    for (int i = 0; i < WAD_LEVELS; i++)
        if (s_levels[i].path[0]) overrides++;
    OPL_INFO_F("wadmod", "WAD TOC: %d level data entries; %d override%s "
               "registered", usable, overrides, overrides == 1 ? "" : "s");
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
        if (!L->path[0]) continue;
        uint32_t lo = L->start_sector, hi = L->start_sector + L->n_sectors;
        uint32_t a = start > lo ? start : lo;
        uint32_t b = (start + (uint32_t)sectors) < hi
                         ? (start + (uint32_t)sectors) : hi;
        if (a >= b) continue;
        FILE* f = fopen(L->path, "rb");
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
        if (!L->announced) {
            L->announced = 1;
            OPL_INFO_F("wadmod", "wad overlay: level %d data served from "
                       "%s (%s)", level_index_to_id(i), L->owner, L->path);
        }
    }
}
