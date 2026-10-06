/* games/common/renderer/wad_subfile.h - host reads of a WAD.WAD sub-file.
 *
 * WAD.WAD on every Spyro disc is a table of (offset, size) word pairs, one
 * per entry, in its first two sectors; each entry's first sector holds one
 * (offset, size) pair per sub-file, offsets relative to the entry. The reads
 * here go through a disc handle of their own, traced under the caller's host
 * tag, so they never steer the disc cache loader and never enter a boot
 * profile. Presentation only: nothing here writes guest state. */
#ifndef OPENPETE_COMMON_RENDERER_WAD_SUBFILE_H
#define OPENPETE_COMMON_RENDERER_WAD_SUBFILE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Reads sub-file @p k of WAD entry @p e from the booted disc into a host buffer.
 *
 * Every offset and size is checked against its container and against
 * @p cap; a failure logs its reason under @p who and reads nothing.
 * @param e The WAD entry index.
 * @param k The sub-file index inside the entry.
 * @param tag The OPENPETE_DISC_TAG_* host path the reads are traced under.
 * @param buf Receives the sub-file, or NULL to read nothing and return the size.
 * @param cap The buffer's capacity in bytes; ignored when @p buf is NULL.
 * @param who The log subsystem naming the caller.
 * @return The sub-file's byte count, or 0 on any failure. */
uint32_t wad_subfile_read(int e, int k, int tag, uint8_t* buf, uint32_t cap, const char* who);

/** @brief Reads sub-file @p k of WAD entry @p e into a buffer it allocates.
 *
 * @param e The WAD entry index.
 * @param k The sub-file index inside the entry.
 * @param tag The OPENPETE_DISC_TAG_* host path the reads are traced under.
 * @param size Receives the byte count; 0 on failure.
 * @param who The log subsystem naming the caller.
 * @return The buffer, which the caller frees, or NULL on failure. */
uint8_t* wad_subfile_load(int e, int k, int tag, uint32_t* size, const char* who);

#ifdef __cplusplus
}
#endif
#endif /* OPENPETE_COMMON_RENDERER_WAD_SUBFILE_H */
