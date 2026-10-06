/* games/common/renderer/wad_subfile.c - see wad_subfile.h. */
#include "renderer/wad_subfile.h"

#include "openpete_disc_image.h"
#include "openpete_log.h"

#include <stdlib.h>
#include <string.h>

/** @brief Reads a little-endian word from a byte buffer.
 * @param p The buffer.
 * @param o The byte offset.
 * @return The word. */
static uint32_t ws_le32(const uint8_t* p, uint32_t o) {
    return (uint32_t)p[o] | ((uint32_t)p[o + 1] << 8) | ((uint32_t)p[o + 2] << 16) | ((uint32_t)p[o + 3] << 24);
}

/** @brief Locates sub-file @p k of entry @p e inside WAD.WAD on an open disc handle.
 *
 * Reads the WAD's two-sector entry table and the entry's first sector, and
 * checks each (offset, size) pair against its container: the entry against
 * the WAD extent, the sub-file against the entry. Both offsets must be
 * sector-aligned, which the format guarantees and the sector reads need.
 * @param img The open handle.
 * @param e The entry index.
 * @param k The sub-file index.
 * @param lba Receives the sub-file's first sector.
 * @param size Receives the sub-file's byte count.
 * @return NULL on success, else the failure's reason. */
static const char* ws_locate(disc_image_t* img, uint32_t e, uint32_t k, uint32_t* lba, uint32_t* size) {
    enum { SEC = DISC_IMAGE_USER_SECTOR };
    uint8_t hdr[2 * SEC], sec[SEC];
    disc_image_entry_t wad;
    if (disc_image_find(img, "WAD.WAD", &wad) != 0)                    return "no WAD.WAD";
    if (8u * e + 8u > sizeof hdr || 8u * k + 8u > SEC)                 return "descriptor index";
    if (disc_image_read_user(img, wad.lba, hdr) != 0 ||
        disc_image_read_user(img, wad.lba + 1u, hdr + SEC) != 0)       return "WAD header read";
    const uint32_t eoff = ws_le32(hdr, 8u * e), esz = ws_le32(hdr, 8u * e + 4u);
    if (esz == 0u || eoff % SEC || eoff > wad.size || esz > wad.size - eoff) return "WAD entry";
    if (disc_image_read_user(img, wad.lba + eoff / SEC, sec) != 0)     return "entry header read";
    const uint32_t soff = ws_le32(sec, 8u * k), ssz = ws_le32(sec, 8u * k + 4u);
    if (ssz == 0u || soff % SEC || soff > esz || ssz > esz - soff)     return "sub-file";
    *lba = wad.lba + (eoff + soff) / SEC;
    *size = ssz;
    return NULL;
}

uint32_t wad_subfile_read(int e, int k, int tag, uint8_t* buf, uint32_t cap, const char* who) {
    enum { SEC = DISC_IMAGE_USER_SECTOR };
    disc_image_t* game = disc_image_game();
    if (!game) { OPL_WARN_F(who, "WAD entry %d sub-file %d not read: no disc open", e, k); return 0; }
    if (e < 0 || k < 0) return 0;
    disc_image_t* img = disc_image_open(disc_image_bin_path(game), DISC_READ_CACHED, tag);
    if (!img) return 0;
    uint32_t lba = 0, size = 0;
    const char* why = ws_locate(img, (uint32_t)e, (uint32_t)k, &lba, &size);
    if (!why && buf && size > cap) why = "sub-file larger than the buffer";
    uint8_t sec[SEC];
    for (uint32_t i = 0; !why && buf && i * SEC < size; i++) {
        if (disc_image_read_user(img, lba + i, sec) != 0) { why = "sub-file read"; break; }
        const uint32_t n = (size - i * SEC < SEC) ? size - i * SEC : SEC;
        memcpy(buf + i * SEC, sec, n);
    }
    disc_image_close(img);
    if (why) {
        OPL_WARN_F(who, "WAD entry %d sub-file %d not read from the disc (%s)", e, k, why);
        return 0;
    }
    return size;
}

uint8_t* wad_subfile_load(int e, int k, int tag, uint32_t* size, const char* who) {
    *size = 0;
    const uint32_t n = wad_subfile_read(e, k, tag, NULL, 0, who);
    if (!n) return NULL;
    uint8_t* buf = (uint8_t*)malloc(n);
    if (!buf) return NULL;
    if (wad_subfile_read(e, k, tag, buf, n, who) != n) { free(buf); return NULL; }
    *size = n;
    return buf;
}
