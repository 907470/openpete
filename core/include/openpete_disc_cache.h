/* core/include/openpete_disc_cache.h - the disc image resident in host memory.
 *
 * PsyCross reads the disc through a VFILE; on the file path every guest
 * CdRead drain, every XA pump and CdInit's re-open is an fread on the sim
 * thread, and the wall-clock cost of that read (tens of ms per level-load
 * burst over a network share) lands as a late tick that no interpolation
 * can hide. This attaches the image as PsyCross's memory image instead,
 * filled by a background thread in file order behind a watermark: a read
 * below the watermark is a memcpy; one above it blocks until the loader
 * passes it and is counted as a miss (logged, and a pacelog CD_MISS event)
 * so the stall is attributed rather than guessed at. Bytes are identical
 * to the file path. */
#ifndef OPENPETE_DISC_CACHE_H
#define OPENPETE_DISC_CACHE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/** @brief Attach a disc image as PsyCross's memory image and start the background loader.
 *
 * Allocates the whole image, installs the miss wait with
 * PsyX_CDFS_SetMemWait, hands the buffer to PsyX_CDFS_Init_Mem, and starts
 * the loader thread; when the thread cannot be created the image loads
 * inline before the call returns. A call while already attached changes
 * nothing.
 * @param bin_path Path to the disc image.
 * @return 1 when PsyCross now reads from memory (or already did); 0 when
 *         @p bin_path is NULL or the file cannot be opened, sized (larger
 *         than 0x7FFFFFFF bytes) or allocated, and the caller falls back
 *         to PsyX_CDFS_Init. */
int  openpete_disc_cache_attach(const char* bin_path);
/** @brief Report whether an image is attached as PsyCross's memory image.
 *
 * Stays 1 after openpete_disc_cache_shutdown, since PsyCross keeps the buffer.
 * @return 1 after a successful attach, else 0. */
int  openpete_disc_cache_active(void);
/** @brief Stop the loader, join its thread, and remove the miss wait.
 *
 * The image buffer stays allocated because PsyCross holds the pointer
 * until the process ends; chunks the loader had not read stay unfilled. No
 * effect when the cache was never attached. */
void openpete_disc_cache_shutdown(void);
/** @brief Report the cache's counters for the exit census.
 *
 * Each output pointer may be NULL.
 * @param misses Receives the number of reads that blocked on the loader.
 * @param waited_ns Receives the total time those reads blocked, in ns.
 * @param bytes Receives the image size in bytes.
 * @param load_ns Receives the loader's wall time from attach to the last
 *        chunk, in ns; 0 until the loader finishes. */
void openpete_disc_cache_census(uint64_t* misses, int64_t* waited_ns,
                                size_t* bytes, int64_t* load_ns);
#ifdef __cplusplus
}
#endif
#endif
