/* core/include/openpete_disc_image.h - the disc image reader every host path uses.
 *
 * One reader for the sector image behind --disc: PsyCross's libcd and XA
 * reads, the EXE load, the disc serial, the recomp extract, the woah NSF,
 * the sfx census, the WAD overlay, the library ingest copy and the mod API.
 * Addressing is by LBA over raw 2352-byte sectors; an image of 2048-byte
 * sectors is refused, since it has lost the Mode 2 Form 2 sectors the games
 * stream XA audio from.
 *
 * A handle opened DISC_READ_CACHED reads through the disc cache when the
 * cache holds the same file: a byte range in resident chunks is copied from
 * memory (a hit); any other range is read from the file at once (a miss)
 * while the cache loader is steered to its chunk. A handle opened
 * DISC_READ_DIRECT, or any handle while no cache holds its file, reads the
 * file (direct). File reads are positioned (pread, or ReadFile with an
 * offset on Windows), so handles and threads share no cursor.
 *
 * Name lookup follows ISO 9660 (ECMA-119): the primary volume descriptor
 * from sector 16, its root directory record, and directory records walked
 * one path component at a time, compared case-insensitively with the
 * version suffix (";1") and a trailing '.' removed. */
#ifndef OPENPETE_DISC_IMAGE_H
#define OPENPETE_DISC_IMAGE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Bytes in one raw sector and in its Mode 1 / Mode 2 Form 1 user data. */
#define DISC_IMAGE_RAW_SECTOR  2352u
#define DISC_IMAGE_USER_SECTOR 2048u

/* Open modes. */
enum {
    DISC_READ_CACHED = 0,   /* through the disc cache when it holds the file */
    DISC_READ_DIRECT = 1    /* always the file: ingest copy, offline tools */
};

typedef struct disc_image disc_image_t;

/** @brief A file or directory found by disc_image_find. */
typedef struct {
    uint32_t lba;      /**< First sector of the extent. */
    uint32_t size;     /**< Extent length in bytes. */
    int      is_dir;   /**< 1 for a directory record. */
} disc_image_entry_t;

/** @brief Open the sector image behind a disc path (a cue or the image itself).
 *
 * The path resolves through openpete_psx_resolve_disc_bin. The sector size
 * is read off the file size once: a multiple of 2352 is a raw image; a
 * multiple of 2048 is refused with an error naming the lost XA sectors;
 * any other size is refused.
 * @param path Disc path.
 * @param mode DISC_READ_CACHED or DISC_READ_DIRECT.
 * @param tag The OPENPETE_DISC_TAG_* path this handle's reads are traced
 *        and steered under; OPENPETE_DISC_TAG_NONE traces under the calling
 *        thread's trace scope and never steers the cache loader.
 * @return The handle, or NULL with an error logged. */
disc_image_t* disc_image_open(const char* path, int mode, int tag);

/** @brief Close a handle; NULL is a no-op.
 *
 * @param img The handle. */
void disc_image_close(disc_image_t* img);

/** @brief Return the resolved sector image path.
 *
 * @param img The handle.
 * @return The path, owned by the handle. */
const char* disc_image_bin_path(const disc_image_t* img);

/** @brief Return the image size in bytes.
 *
 * @param img The handle.
 * @return The size. */
uint64_t disc_image_size(const disc_image_t* img);

/** @brief Return the image's sector size, read off the file size at open.
 *
 * @param img The handle.
 * @return DISC_IMAGE_RAW_SECTOR; an open handle has no other. */
uint32_t disc_image_sector_bytes(const disc_image_t* img);

/** @brief Read bytes [offset, offset + len) of the image, untraced.
 *
 * Bytes past the end of the image, or that the file fails to supply, read
 * as zero.
 * @param img The handle.
 * @param offset First byte.
 * @param dst Receives @p len bytes.
 * @param len Byte count.
 * @param tag The path issuing the read, for the cache's steer and miss
 *        count; OPENPETE_DISC_TAG_NONE never steers.
 * @return OPENPETE_DISC_HIT, _MISS or _DIRECT; -1 when any byte read as
 *         zero for want of the file. */
int disc_image_read_bytes(disc_image_t* img, uint64_t offset, void* dst,
                          size_t len, int tag);

/** @brief Read one raw 2352-byte sector, traced under the handle's tag.
 *
 * @param img The handle.
 * @param lba Sector.
 * @param out2352 Receives the sector.
 * @return 0, or -1 when the sector lies past the end of the image or the
 *         file came up short. */
int disc_image_read_sector(disc_image_t* img, uint32_t lba, uint8_t* out2352);

/** @brief Read the 2048 user-data bytes of one sector, traced under the handle's tag.
 *
 * The user data starts 16 bytes into a Mode 1 sector and 24 bytes into a
 * Mode 2 sector (after the subheader), per the mode byte of the header.
 * @param img The handle.
 * @param lba Sector.
 * @param out2048 Receives the user data.
 * @return 0, or -1 as for disc_image_read_sector. */
int disc_image_read_user(disc_image_t* img, uint32_t lba, uint8_t* out2048);

/** @brief Look up a file or directory by its ISO 9660 path.
 *
 * Components split at '/' or '\'; leading separators are skipped. Each
 * component matches a directory record case-insensitively with the ";1"
 * version suffix and a trailing '.' removed from both names.
 * @param img The handle.
 * @param path Such as "SYSTEM.CNF" or "S0/S000000D.NSF".
 * @param entry Receives the record.
 * @return 0 when found, -1 when absent or the volume descriptor or a
 *         directory cannot be read. */
int disc_image_find(disc_image_t* img, const char* path, disc_image_entry_t* entry);

/** @brief Read a file's bytes, at most @p max, from its extent's user data.
 *
 * @param img The handle.
 * @param entry A file found by disc_image_find.
 * @param buf Receives the bytes.
 * @param max Capacity of @p buf.
 * @return Bytes copied: min(entry->size, max) on success, fewer when a
 *         sector read fails. */
size_t disc_image_read_file(disc_image_t* img, const disc_image_entry_t* entry,
                            void* buf, size_t max);

/** @brief Read at most @p len bytes of a file starting @p offset bytes into it, from its extent's user data.
 *
 * Reads only the sectors the range covers, so a sample inside a large
 * archive costs its own bytes rather than the whole file.
 * @param img The handle.
 * @param entry A file found by disc_image_find.
 * @param offset First byte, relative to the file's start.
 * @param buf Receives the bytes.
 * @param len Capacity of @p buf.
 * @return Bytes copied: min(entry->size - offset, len) on success, 0 when
 *         offset lies at or past the end, fewer when a sector read fails. */
size_t disc_image_read_file_range(disc_image_t* img, const disc_image_entry_t* entry,
                                  uint32_t offset, void* buf, size_t len);

/** @brief Record the handle of the disc this session booted, for readers that hold no handle of their own.
 *
 * @param img The handle, or NULL. */
void disc_image_set_game(disc_image_t* img);

/** @brief Return the handle disc_image_set_game recorded.
 *
 * @return The handle, or NULL before a disc is open. */
disc_image_t* disc_image_game(void);

/** @brief Read one raw sector of the booted disc under the mod tag (the mod API's read_sector).
 *
 * @param lba Sector.
 * @param out2352 Receives the sector.
 * @return 0, or -1 when no disc is open or the read fails. */
int disc_image_game_read_sector(uint32_t lba, uint8_t* out2352);

#ifdef __cplusplus
}
#endif
#endif
