// games/spyro-1/mods/mod_wad_overlay.c - the asset overlay's level-segment
// namespace.
//
// Spyro's loader pulls everything from WAD.WAD by sector range, so the
// overlay key is LOGICAL identity, not a path: per-level {overlay, data}
// entries from g_WadHeader's TOC. A mod ships
//   assets/wad/level<ID>.data.bin      (ID = logical level id, e.g. 10)
//   assets/wad/level<ID>.ovl.bin       (optional: the level's code overlay)
// and every CdRead that lands inside that entry is served the mod's
// bytes instead of the disc's. Overlay overrides must match the stock
// size (code loads into a fixed buffer); DATA overrides may be any size
// - the serve range follows the file, and a resizing mod declares the
// new length in the guest WAD TOC so the game actually streams it. The
// highest-priority ENABLED mod wins, same single priority truth as
// every other namespace.
//
// Mechanism: the shared psyq thunks call psyq_game_cd_setloc on CdlSetloc to
// track the head position, and psyq_game_cd_apply_buf from psyq_CdRead after
// the read drains (games/spyro-1/psyq_hooks.cpp forwards them to
// mod_wad_setloc and mod_wad_apply), so the guest buffer holds the override
// bytes before the game's loader parses them. The TOC is read host-side
// from the user's own disc image (cue -> bin, WAD at the game's wad_sector),
// lazily on first use.
//
// Live enable/disable: the winning override is resolved against the
// current enabled set, but only ONCE PER LEVEL LOAD - see the latch in
// mod_wad_apply(). A toggle therefore lands at the next level load and
// can never splice stock and override bytes into one level image.
//
// Legal posture note: mods may NOT ship derived game data. This
// namespace exists for (a) locally-derived blobs (generated on the
// user's machine from their own disc - the CI fixtures do exactly this)
// and (b) genuinely novel content (custom levels).

#include "mods/mod_wad_overlay.h"

#include "game_extras.h"
#include "openpete_disc_image.h"
#include "openpete_disc_trace.h"
#include "openpete_log.h"
#include "openpete_str.h"
#include "openpete_mods.h"
#include "openpete_psx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define WAD_LEVELS      36
#define WAD_HDR_LEVELS  72   /* byte offset of m_LevelEntry[0] in g_WadHeader */
#define SEC_DATA        2048

/* One servable WAD segment (a level's Overlay or Data entry). Data
 * overrides may be a DIFFERENT size than the stock entry: the serve
 * range extends to the override file (a level-resize mod patches the
 * guest TOC length so the game actually asks for it - the overlay just
 * answers whatever span the game reads). Overlay overrides stay
 * same-size (code loaded into a fixed-size buffer). */
typedef struct {
    int      present;       /* TOC entry valid */
    uint32_t start_sector;  /* absolute disc sector of the entry */
    uint32_t n_sectors;     /* stock entry sectors */
    uint32_t length;        /* stock entry byte length */
    int      latched;       /* live_root decided for the current load */
    int      live_root;     /* asset root serving this load, -1 = the disc */
    int      serve_id;      /* logical level id this load resolved to */
    uint32_t serve_len;     /* latched override byte length (file size) */
    char     serve_path[1088]; /* latched override file */
} wad_seg_t;

/* Guest-side words the latch consults (Spyro 1). The in-RAM WAD TOC is
 * the serving CONTRACT: a segment serves the override of whichever
 * LOGICAL level id is streaming it - a registered level slot's cloned
 * entry points at its donor's sectors, so the same physical range can
 * be one level stock and another level modded. */
#define GVA_NEXT_LEVEL 0x800758B4u
#define GVA_LEVEL_ID   0x8007596Cu
#define GVA_WAD_HEADER 0x8007A6D0u

/** @brief Read a guest word through a bounds-checked span; 0 when it does not map. */
static uint32_t guest_u32(uint32_t va) {
    const uint8_t* p = (const uint8_t*)openpete_psx_to_host_span(va, 4u);
    uint32_t v = 0u;
    if (p) memcpy(&v, p, 4);
    return v;
}

typedef struct {
    wad_seg_t data;
    wad_seg_t ovl;
} wad_level_t;

static char        s_disc_path[1024];
static int         s_state = 0;     /* 0 unconfigured, 1 pending, 2 ready, -1 failed */
static wad_level_t s_levels[WAD_LEVELS];
static disc_image_t* s_disc = NULL;
static uint32_t    s_next_sector = 0;
static uint32_t    s_wad_sector = 37;

/** @brief Returns the tracked disc head sector for the savestate extra block.
 *
 * The sector is set by each CdlSetloc and advanced by every CdRead that
 * mod_wad_apply sees; the overlay compares it against each WAD segment's
 * sector range to decide which reads it serves. The game-extra savestate
 * block saves and restores it beside guest RAM, so a restored state resumes
 * a multi-read load at the head position the guest's CdRead sequence
 * expects.
 * @return The absolute disc sector the next read starts at. */
uint32_t spyro1_mod_wad_cursor_get(void)        { return s_next_sector; }
/** @brief Restores the tracked disc head sector from the savestate extra block.
 * @param s The absolute disc sector the next read starts at. */
void     spyro1_mod_wad_cursor_set(uint32_t s)  { s_next_sector = s; }

void spyro1_mod_wad_configure(const char* disc_path) {
    if (!disc_path || !disc_path[0]) return;
    if (s_state == 2 && strcmp(s_disc_path, disc_path) == 0) return;
    snprintf(s_disc_path, sizeof s_disc_path, "%s", disc_path);
    s_state = 1;
}

/** @brief Reads the 2048-byte user data of one absolute disc sector through the disc reader.
 *
 * @param sector The absolute disc sector.
 * @param out Receives 2048 bytes.
 * @return 0 on success, -1 on a read past the image or a short read. */
static int read_sector(uint32_t sector, uint8_t* out) {
    return disc_image_read_user(s_disc, sector, out);
}

/** @brief Maps a WAD TOC index to its logical level id.
 *
 * @param idx The TOC index (0 to 35).
 * @return (idx / 6 + 1) * 10 + idx % 6, such as 10 for index 0 and 65 for
 *     index 35. */
static int level_index_to_id(int idx) {
    return (idx / 6 + 1) * 10 + idx % 6;
}

/** @brief Builds the override file path logical level `id` would take from one asset root.
 *
 * The path is `ROOT/wad/levelID.KIND.bin`: the root's directory, `id` in
 * two digits, and `kind`.
 * Root indices are stable for the session (the loader only appends roots;
 * a disabled mod keeps its slot), so a caller may latch an index.
 * @param id The logical level id.
 * @param kind "data" or "ovl".
 * @param root The asset root index.
 * @param out Receives the path.
 * @param cap The capacity of `out`.
 * @param owner_out When non-NULL, receives the root's owning mod id.
 * @return 0 on success, -1 for an index with no root. */
static int wad_override_path(int id, const char* kind, int root, char* out,
                             size_t cap, const char** owner_out) {
    const char* owner = NULL;
    const char* dir = openpete_mods_asset_root(root, &owner);
    if (!dir) return -1;
    snprintf(out, cap, "%s/wad/level%02d.%s.bin", dir, id, kind);
    if (owner_out) *owner_out = owner;
    return 0;
}

/** @brief Resolves the winning WAD override for logical level `id` against the current enabled set.
 *
 * Walks the asset roots from the highest priority (last registered) down
 * and takes the first whose owner is enabled (or unowned) and which ships
 * `wad/levelID.KIND.bin` of exactly `want_len` bytes, the length the
 * guest WAD TOC declares for the entry. A resize mod declares its new
 * length in that table; a file of any other size never serves. A level's
 * files serve as a set: an ovl serves only when the same root's data file,
 * if present, is `want_data_len` bytes, so a root shipping a whole level
 * never leaks its code overlay into a stock load of the same sectors.
 * @param id The logical level id.
 * @param kind "data" or "ovl".
 * @param want_len The byte length the guest TOC declares for this entry.
 * @param want_data_len The byte length the guest TOC declares for the
 *     level's data entry.
 * @param path_out When non-NULL, receives the winning file's path.
 * @param cap The capacity of `path_out`.
 * @param owner_out When non-NULL, receives the owning mod id ("?" when
 *     unowned).
 * @param len_out When non-NULL, receives the file's byte length.
 * @return The winning root index, or -1 to serve the disc. */
static int wad_find_override(int id, const char* kind, uint32_t want_len,
                             uint32_t want_data_len, char* path_out,
                             size_t cap, const char** owner_out,
                             uint32_t* len_out) {
    int is_ovl = kind[0] == 'o';
    for (int r = openpete_mods_asset_root_count() - 1; r >= 0; r--) {
        const char* owner = NULL;
        char p[1088];
        if (wad_override_path(id, kind, r, p, sizeof p, &owner) != 0)
            continue;
        /* A live-disabled mod's assets must not reach the guest; the slot
         * falls through to the next enabled mod that supplies this level. */
        if (owner && owner[0] && !openpete_mods_is_enabled(owner)) continue;
        if (!owner || !owner[0]) owner = "?";
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if ((uint32_t)st.st_size != want_len) {
            if (getenv("OPENPETE_WADMOD_TRACE"))
                OPL_INFO_F("wadmod", "%s: level %d %s is %ld bytes, guest "
                           "TOC declares %u - not serving", owner, id, kind,
                           (long)st.st_size, want_len);
            continue;
        }
        if (is_ovl) {
            char dp[1088];
            struct stat ds;
            if (wad_override_path(id, "data", r, dp, sizeof dp, NULL) == 0 &&
                stat(dp, &ds) == 0 &&
                (uint32_t)ds.st_size != want_data_len)
                continue; /* whole-level set: data mismatched -> hold ovl */
        }
        if (path_out) snprintf(path_out, cap, "%s", p);
        if (owner_out) *owner_out = owner;
        if (len_out) *len_out = (uint32_t)st.st_size;
        return r;
    }
    return -1;
}

/** @brief Parses the disc's WAD TOC into the per-level segment table, once the overlay is pending.
 *
 * Does nothing unless configured and pending, and stays pending while no
 * mod asset root exists. Otherwise opens the disc image through the disc
 * reader (WAD tag), reads the header sector at the game's wad_sector, and records the sector range and byte length of each
 * of 36 levels' data and overlay entries whose offsets are sector aligned.
 * Ends ready (state 2) with a logged entry and override-file count, or
 * failed (state -1) with a warning; a failed parse is not retried. */
static void wad_init(void) {
    if (s_state != 1) return;

    /* No mod has an assets/ root yet: stay PENDING rather than failing.
     * A boot-disabled mod registers its root only when it is first
     * enabled (deferred entry), and the TOC still has to be parsed then. */
    if (openpete_mods_asset_root_count() == 0) return;

    s_state = -1; /* until proven otherwise */

    s_disc = disc_image_open(s_disc_path, DISC_READ_CACHED, OPENPETE_DISC_TAG_WAD);
    if (!s_disc) {
        OPL_WARN_F("wadmod", "cannot open disc image %s", s_disc_path);
        return;
    }

    const game_disc_layout_t* dl = game_get_disc_layout();
    uint32_t wad_sector = dl ? dl->wad_sector : 37;
    s_wad_sector = wad_sector;

    uint8_t hdr[SEC_DATA];
    if (read_sector(wad_sector, hdr) != 0) {
        OPL_WARN_F("wadmod", "cannot read WAD header sector %u", wad_sector);
        return;
    }
    int usable = 0;
    for (int i = 0; i < WAD_LEVELS; i++) {
        uint32_t oo, ol, off, len;
        memcpy(&oo,  hdr + WAD_HDR_LEVELS + i * 16 + 0, 4);  /* m_Overlay.m_Offset */
        memcpy(&ol,  hdr + WAD_HDR_LEVELS + i * 16 + 4, 4);  /* m_Overlay.m_Length */
        memcpy(&off, hdr + WAD_HDR_LEVELS + i * 16 + 8, 4);  /* m_Data.m_Offset */
        memcpy(&len, hdr + WAD_HDR_LEVELS + i * 16 + 12, 4); /* m_Data.m_Length */
        if (len && (off & (SEC_DATA - 1)) == 0) {
            wad_seg_t* d = &s_levels[i].data;
            d->present = 1;
            d->start_sector = wad_sector + off / SEC_DATA;
            d->n_sectors = (len + SEC_DATA - 1) / SEC_DATA;
            d->length = len;
            d->live_root = -1;
            usable++;
        }
        if (ol && (oo & (SEC_DATA - 1)) == 0) {
            wad_seg_t* o = &s_levels[i].ovl;
            o->present = 1;
            o->start_sector = wad_sector + oo / SEC_DATA;
            o->n_sectors = (ol + SEC_DATA - 1) / SEC_DATA;
            o->length = ol;
            o->live_root = -1;
        }
    }
    /* Boot summary: count override FILES present for any level id the
     * roots ship (any size - whether one serves is decided per load
     * against the guest TOC). Purely informational. */
    int overrides = 0;
    for (int id = 10; id <= 69; id++) {
        for (int r = openpete_mods_asset_root_count() - 1; r >= 0; r--) {
            char p[1088];
            struct stat st;
            if (wad_override_path(id, "data", r, p, sizeof p, NULL) == 0 &&
                stat(p, &st) == 0) { overrides++; break; }
        }
    }
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

    for (int i = 0; i < 2 * WAD_LEVELS; i++) {
        const char* kind = (i & 1) ? "ovl" : "data";
        wad_seg_t* L = (i & 1) ? &s_levels[i >> 1].ovl
                               : &s_levels[i >> 1].data;
        int lid = level_index_to_id(i >> 1);
        if (!L->present) continue;
        /* Serve range: an override may be LARGER than the stock entry
         * (a level-resize mod patched the guest TOC length) - once one
         * is latched, its file size defines the range the game may read. */
        uint32_t range = L->n_sectors;
        if (L->latched && L->live_root >= 0 && L->serve_len > L->length)
            range = (L->serve_len + SEC_DATA - 1) / SEC_DATA;
        uint32_t lo = L->start_sector, hi = L->start_sector + range;
        uint32_t a = start > lo ? start : lo;
        uint32_t b = (start + (uint32_t)sectors) < hi
                         ? (start + (uint32_t)sectors) : hi;
        if (a >= b) continue;

        /* Per-load serve latch. The game streams one level's entry
         * across a dozen separate CdReads spread over many ticks
         * (LoadLevel stages 3..11) and keeps reading the same entry
         * in-level for the dragon cutscenes, so resolving the override
         * per read would splice stock and mod bytes into one level image
         * whenever an enable/disable landed between two of them. Resolve
         * once instead, at the read that starts at the entry's FIRST
         * sector - the level-header read that begins every load of that
         * level - and hold it for every later read of the entry. A read
         * that lands mid-entry with nothing latched (savestate restore,
         * level-slot clone) latches there. */
        char path[1088];
        const char* owner = NULL;
        if (!L->latched || start == lo) {
            /* Which LOGICAL level is streaming this segment? Ask the
             * guest: the id in flight (g_NextLevelId during a load,
             * g_LevelId for in-level re-reads) names a TOC entry, and
             * that entry pointing its offset at THIS segment makes it
             * the identity of the load - a registered slot's cloned
             * entry aims at its donor's sectors, so the same range can
             * be one level stock and another level modded. The entry's
             * length is the serve contract. Falls back to the retail
             * identity + stock length while the guest table isn't up
             * (boot: the TOC itself is still loading). */
            uint32_t seg_woff = (L->start_sector - s_wad_sector) * SEC_DATA;
            int serve_id = lid;
            uint32_t want_len = L->length, want_dlen = 0;
            int cand[2] = { (int)guest_u32(GVA_NEXT_LEVEL),
                            (int)guest_u32(GVA_LEVEL_ID) };
            for (int c = 0; c < 2; c++) {
                int id = cand[c];
                if (id < 10) continue;
                int gi = (id / 10 - 1) * 6 + id % 10;
                if (gi < 0 || gi >= WAD_LEVELS) continue;
                uint32_t e = GVA_WAD_HEADER + 72u + (uint32_t)gi * 16u;
                if (guest_u32(e + ((i & 1) ? 0u : 8u)) != seg_woff)
                    continue;
                serve_id = id;
                want_len = guest_u32(e + ((i & 1) ? 4u : 12u));
                want_dlen = guest_u32(e + 12u);
                break;
            }
            if (!want_dlen) {
                /* No guest resolve: the data contract is the stock len. */
                want_dlen = s_levels[i >> 1].data.length;
            }
            uint32_t flen = 0;
            L->latched = 1;
            L->serve_id = serve_id;
            L->live_root = -1;
            /* Declarative custom levels first: the registry names the
             * files serving this logical id (active slot row / declared
             * replace row); same guest-TOC size contract. */
            const char* cdir = NULL;
            const char* cstem = NULL;
            if (openpete_mods_custom_files_for(serve_id, &cdir, &cstem)) {
                struct stat st;
                char dp[1088];
                openpete_mods_custom_asset_path(cdir, cstem, kind[0] == 'o',
                                                path, sizeof path);
                openpete_mods_custom_asset_path(cdir, cstem, 0, dp, sizeof dp);
                struct stat ds;
                if (stat(path, &st) == 0 && (uint32_t)st.st_size == want_len &&
                    stat(dp, &ds) == 0 && (uint32_t)ds.st_size == want_dlen) {
                    L->live_root = 1000; /* custom row, not an asset root */
                    flen = (uint32_t)st.st_size;
                    owner = "custom level";
                }
            }
            if (L->live_root < 0)
                L->live_root = wad_find_override(serve_id, kind, want_len,
                                                 want_dlen, path,
                                                 sizeof path, &owner,
                                                 &flen);
            L->serve_len = flen;
            if (L->live_root >= 0) {
                snprintf(L->serve_path, sizeof L->serve_path, "%s", path);
                OPL_INFO_F("wadmod", "wad overlay: level %d %s served "
                           "from %s (%s)", serve_id, kind, owner, path);
            }
        }
        if (L->live_root < 0) continue;
        snprintf(path, sizeof path, "%s", L->serve_path);
        if (!path[0]) continue;
        uint32_t slen = L->serve_len ? L->serve_len : L->length;
        if (getenv("OPENPETE_WADMOD_TRACE"))
            OPL_INFO_F("wadmod", "level %d %s serve [%u..%u)", L->serve_id,
                       kind, a, b);
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        for (uint32_t s = a; s < b; s++) {
            uint32_t rel = (s - lo) * SEC_DATA;
            if (rel >= slen) break;
            uint32_t n = slen - rel < SEC_DATA ? slen - rel : SEC_DATA;
            if (fseek(f, (long)rel, SEEK_SET) != 0) break;
            size_t got = fread(host_buf + (size_t)(s - start) * SEC_DATA,
                               1, n, f);
            (void)got;
        }
        fclose(f);
    }
}

/* Reads a level's stock data WAD entry from the disc image's TOC. */
int spyro1_mod_wad_read_level_data(int level_id, void* buf, uint32_t cap) {
    if (s_state == 1) wad_init();
    if (s_state != 2) return -1;
    int idx = (level_id / 10 - 1) * 6 + level_id % 10;
    if (idx < 0 || idx >= WAD_LEVELS) return -1;
    wad_seg_t* L = &s_levels[idx].data;
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
