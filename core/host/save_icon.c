/* core/host/save_icon.c - see openpete_save_icon.h. */
#include "openpete_save_icon.h"

#include "openpete_disc_image.h"
#include "openpete_disc_trace.h"
#include "openpete_log.h"
#include "openpete_psx.h"
#include "sha256.h"

#include <stdio.h>
#include <string.h>

enum {
    SI_SEC = DISC_IMAGE_USER_SECTOR,
    SI_CLUT = 0x60,        /* 16 BGR555 entries */
    SI_FRAME0 = 0x80       /* 16 rows of 8 bytes, 4bpp */
};

static const openpete_save_icon_loc_t* s_loc;

void openpete_save_icon_set_location(const openpete_save_icon_loc_t* loc) { s_loc = loc; }

/** @brief Reads a little-endian word from a byte buffer.
 * @param p The buffer.
 * @param o The byte offset.
 * @return The word. */
static uint32_t si_le32(const uint8_t* p, uint32_t o) {
    return (uint32_t)p[o] | ((uint32_t)p[o + 1] << 8) | ((uint32_t)p[o + 2] << 16) |
           ((uint32_t)p[o + 3] << 24);
}

/** @brief Finds the extent of the file the location names: the executable, or a WAD.WAD entry.
 *
 * A WAD entry is checked against the WAD extent and must start on a sector
 * boundary, as the format guarantees.
 * @param img The open disc.
 * @param loc The location.
 * @param lba Receives the file's first sector.
 * @param size Receives the file's byte count.
 * @return NULL on success, else the failure's reason. */
static const char* si_extent(disc_image_t* img, const openpete_save_icon_loc_t* loc,
                             uint32_t* lba, uint32_t* size) {
    disc_image_entry_t f;
    if (loc->wad_entry < 0) {
        const openpete_game_runtime_t* rt = openpete_game_runtime();
        const char* exe = rt && rt->identity ? rt->identity->exe_filename : NULL;
        if (!exe || disc_image_find(img, exe, &f) != 0) return "no executable";
        *lba = f.lba;
        *size = f.size;
        return NULL;
    }
    uint8_t table[2 * SI_SEC];
    const uint32_t e = (uint32_t)loc->wad_entry;
    if (disc_image_find(img, "WAD.WAD", &f) != 0) return "no WAD.WAD";
    if (8u * e + 8u > sizeof table) return "WAD entry index";
    if (disc_image_read_user(img, f.lba, table) != 0 ||
        disc_image_read_user(img, f.lba + 1u, table + SI_SEC) != 0) return "WAD table read";
    const uint32_t off = si_le32(table, 8u * e), len = si_le32(table, 8u * e + 4u);
    if (len == 0u || off % SI_SEC || off > f.size || len > f.size - off) return "WAD entry extent";
    *lba = f.lba + off / SI_SEC;
    *size = len;
    return NULL;
}

/** @brief Reads the 512 header bytes at the location's offset in its file.
 *
 * @param img The open disc.
 * @param loc The location.
 * @param hdr Receives the header.
 * @return NULL on success, else the failure's reason. */
static const char* si_read_header(disc_image_t* img, const openpete_save_icon_loc_t* loc,
                                  uint8_t* hdr) {
    uint32_t lba = 0, size = 0;
    const char* why = si_extent(img, loc, &lba, &size);
    if (why) return why;
    if (loc->offset > size || size - loc->offset < OPENPETE_SAVE_ICON_HEADER_BYTES)
        return "header past the end of the file";
    uint8_t sec[2 * SI_SEC];
    const uint32_t first = loc->offset / SI_SEC, within = loc->offset % SI_SEC;
    const uint32_t n = (within + OPENPETE_SAVE_ICON_HEADER_BYTES + SI_SEC - 1u) / SI_SEC;
    for (uint32_t i = 0; i < n; i++)
        if (disc_image_read_user(img, lba + first + i, sec + i * SI_SEC) != 0) return "sector read";
    memcpy(hdr, sec + within, OPENPETE_SAVE_ICON_HEADER_BYTES);
    return hdr[0] == 'S' && hdr[1] == 'C' ? NULL : "no SC magic";
}

/** @brief Returns whether a header's SHA-256 equals the 64-hex-digit digest.
 * @param hdr The 512 header bytes.
 * @param hex The expected digest, lowercase.
 * @return 1 on a match, else 0. */
static int si_digest_matches(const uint8_t* hdr, const char* hex) {
    openpete_sha256_ctx c;
    uint8_t d[32];
    char got[65];
    openpete_sha256_init(&c);
    openpete_sha256_update(&c, hdr, OPENPETE_SAVE_ICON_HEADER_BYTES);
    openpete_sha256_final(&c, d);
    openpete_sha256_hex(d, got);
    return hex && strcmp(got, hex) == 0;
}

/** @brief Decodes frame 0 of a header's icon through its CLUT into RGBA8.
 * @param hdr The 512 header bytes.
 * @param rgba Receives 16x16 RGBA8 pixels, row-major from the top. */
static void si_decode_frame0(const uint8_t* hdr, uint8_t* rgba) {
    uint8_t clut[16][4];
    for (int i = 0; i < 16; i++) {
        const unsigned c = (unsigned)hdr[SI_CLUT + 2 * i] | ((unsigned)hdr[SI_CLUT + 2 * i + 1] << 8);
        const unsigned r = c & 0x1Fu, g = (c >> 5) & 0x1Fu, b = (c >> 10) & 0x1Fu;
        clut[i][0] = (uint8_t)((r << 3) | (r >> 2));
        clut[i][1] = (uint8_t)((g << 3) | (g >> 2));
        clut[i][2] = (uint8_t)((b << 3) | (b >> 2));
        clut[i][3] = i == 0 ? 0 : 255;
    }
    for (int p = 0; p < OPENPETE_SAVE_ICON_WIDTH * OPENPETE_SAVE_ICON_HEIGHT; p++) {
        const uint8_t byte = hdr[SI_FRAME0 + p / 2];
        memcpy(rgba + 4 * p, clut[(p & 1) ? byte >> 4 : byte & 0x0F], 4);
    }
}

int openpete_save_icon_decode(const char* disc_path, uint8_t* rgba) {
    const openpete_save_icon_loc_t* loc = s_loc;
    if (!loc) {
        OPL_INFO_F("icon", "%s", "the game records no save-header location; the window keeps the OpenPete mark");
        return -1;
    }
    char src[64];
    if (loc->wad_entry < 0) snprintf(src, sizeof src, "the executable");
    else snprintf(src, sizeof src, "WAD.WAD entry %d", loc->wad_entry);
    disc_image_t* img = disc_path ? disc_image_open(disc_path, DISC_READ_CACHED, OPENPETE_DISC_TAG_ICON)
                                  : NULL;
    if (!img) {
        OPL_WARN_F("icon", "%s", "save-header icon not read (no disc); the window keeps the OpenPete mark");
        return -1;
    }
    uint8_t hdr[OPENPETE_SAVE_ICON_HEADER_BYTES];
    const char* why = si_read_header(img, loc, hdr);
    disc_image_close(img);
    if (why) {
        OPL_WARN_F("icon", "save-header icon not read from %s +0x%X (%s); the window keeps the OpenPete mark",
                   src, loc->offset, why);
        return -1;
    }
    const int retail = si_digest_matches(hdr, loc->sha256);
    if (!retail)
        OPL_WARN_F("icon", "save header at %s +0x%X differs from the retail disc's; decoding it as found",
                   src, loc->offset);
    si_decode_frame0(hdr, rgba);
    OPL_INFO_F("icon", "window icon: save header at %s +0x%X (%s), decoded %dx%d",
               src, loc->offset, retail ? "retail digest" : "modified", OPENPETE_SAVE_ICON_WIDTH,
               OPENPETE_SAVE_ICON_HEIGHT);
    return 0;
}
