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

/** @brief Locates entry @p e of WAD.WAD on an open disc handle.
 *
 * Reads the WAD's two-sector entry table and checks the entry's (offset,
 * size) pair against the WAD extent; the offset must be sector-aligned,
 * which the format guarantees and the sector reads need.
 * @param img The open handle.
 * @param e The entry index.
 * @param wad Receives WAD.WAD's extent.
 * @param eoff Receives the entry's byte offset in WAD.WAD.
 * @param esz Receives the entry's byte count.
 * @return NULL on success, else the failure's reason. */
static const char* ws_entry(disc_image_t* img, uint32_t e, disc_image_entry_t* wad,
                            uint32_t* eoff, uint32_t* esz) {
    enum { SEC = DISC_IMAGE_USER_SECTOR };
    uint8_t hdr[2 * SEC];
    if (disc_image_find(img, "WAD.WAD", wad) != 0)                     return "no WAD.WAD";
    if (8u * e + 8u > sizeof hdr)                                      return "descriptor index";
    if (disc_image_read_user(img, wad->lba, hdr) != 0 ||
        disc_image_read_user(img, wad->lba + 1u, hdr + SEC) != 0)      return "WAD header read";
    *eoff = ws_le32(hdr, 8u * e);
    *esz = ws_le32(hdr, 8u * e + 4u);
    if (*esz == 0u || *eoff % SEC || *eoff > wad->size || *esz > wad->size - *eoff) return "WAD entry";
    return NULL;
}

/** @brief Locates sub-file @p k of entry @p e inside WAD.WAD on an open disc handle.
 *
 * Locates the entry (ws_entry), reads its first sector and checks the
 * sub-file's (offset, size) pair against the entry; the offset must be
 * sector-aligned.
 * @param img The open handle.
 * @param e The entry index.
 * @param k The sub-file index.
 * @param lba Receives the sub-file's first sector.
 * @param size Receives the sub-file's byte count.
 * @return NULL on success, else the failure's reason. */
static const char* ws_locate(disc_image_t* img, uint32_t e, uint32_t k, uint32_t* lba, uint32_t* size) {
    enum { SEC = DISC_IMAGE_USER_SECTOR };
    uint8_t sec[SEC];
    disc_image_entry_t wad;
    uint32_t eoff = 0, esz = 0;
    const char* why = ws_entry(img, e, &wad, &eoff, &esz);
    if (why) return why;
    if (8u * k + 8u > SEC)                                             return "descriptor index";
    if (disc_image_read_user(img, wad.lba + eoff / SEC, sec) != 0)     return "entry header read";
    const uint32_t soff = ws_le32(sec, 8u * k), ssz = ws_le32(sec, 8u * k + 4u);
    if (ssz == 0u || soff % SEC || soff > esz || ssz > esz - soff)     return "sub-file";
    *lba = wad.lba + (eoff + soff) / SEC;
    *size = ssz;
    return NULL;
}

/** @brief Reads @p size bytes of consecutive sectors from @p lba into @p buf.
 * @param img The open handle.
 * @param lba The first sector.
 * @param size The byte count.
 * @param buf Receives the bytes.
 * @return NULL on success, else the failure's reason. */
static const char* ws_read_span(disc_image_t* img, uint32_t lba, uint32_t size, uint8_t* buf) {
    enum { SEC = DISC_IMAGE_USER_SECTOR };
    uint8_t sec[SEC];
    for (uint32_t i = 0; i * SEC < size; i++) {
        if (disc_image_read_user(img, lba + i, sec) != 0) return "sub-file read";
        const uint32_t n = (size - i * SEC < SEC) ? size - i * SEC : SEC;
        memcpy(buf + i * SEC, sec, n);
    }
    return NULL;
}

uint32_t wad_subfile_read(int e, int k, int tag, uint8_t* buf, uint32_t cap, const char* who) {
    disc_image_t* game = disc_image_game();
    if (!game) { OPL_WARN_F(who, "WAD entry %d sub-file %d not read: no disc open", e, k); return 0; }
    if (e < 0 || k < 0) return 0;
    disc_image_t* img = disc_image_open(disc_image_bin_path(game), DISC_READ_CACHED, tag);
    if (!img) return 0;
    uint32_t lba = 0, size = 0;
    const char* why = ws_locate(img, (uint32_t)e, (uint32_t)k, &lba, &size);
    if (!why && buf && size > cap) why = "sub-file larger than the buffer";
    if (!why && buf) why = ws_read_span(img, lba, size, buf);
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

uint8_t* wad_disc_subfile_load(disc_image_t* img, int e, int k, uint32_t* size, const char* who) {
    *size = 0;
    if (!img || e < 0 || k < 0) return NULL;
    uint32_t lba = 0, n = 0;
    const char* why = ws_locate(img, (uint32_t)e, (uint32_t)k, &lba, &n);
    uint8_t* buf = why ? NULL : (uint8_t*)malloc(n);
    if (!why && !buf) why = "allocation";
    if (!why) why = ws_read_span(img, lba, n, buf);
    if (why) {
        OPL_WARN_F(who, "WAD entry %d sub-file %d not read from %s (%s)", e, k,
                   disc_image_bin_path(img), why);
        free(buf);
        return NULL;
    }
    *size = n;
    return buf;
}

uint8_t* wad_disc_entry_load(disc_image_t* img, int e, uint32_t off, uint32_t len, uint32_t* size,
                             const char* who) {
    *size = 0;
    if (!img || e < 0) return NULL;
    disc_image_entry_t wad;
    uint32_t eoff = 0, esz = 0;
    const char* why = ws_entry(img, (uint32_t)e, &wad, &eoff, &esz);
    if (!why && off > esz) why = "range past the entry";
    if (!why && len == 0u) len = esz - off;
    if (!why && len > esz - off) why = "range past the entry";
    uint8_t* buf = (why || len == 0u) ? NULL : (uint8_t*)malloc(len);
    if (!why && !buf) why = len ? "allocation" : "empty range";
    if (!why && disc_image_read_file_range(img, &wad, eoff + off, buf, len) != len) why = "entry read";
    if (why) {
        OPL_WARN_F(who, "WAD entry %d bytes +%u..+%u not read from %s (%s)", e, (unsigned)off,
                   (unsigned)(off + len), disc_image_bin_path(img), why);
        free(buf);
        return NULL;
    }
    *size = len;
    return buf;
}
