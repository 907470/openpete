/* core/include/openpete_disc_cache.h — the disc image resident in host memory.
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
/* Attach `bin_path` as the memory image and start the loader. Returns 1 on
 * success (PsyCross is now reading from memory), 0 if the file could not
 * be opened or sized — the caller falls back to PsyX_CDFS_Init. */
int  openpete_disc_cache_attach(const char* bin_path);
int  openpete_disc_cache_active(void);
/* Stop the loader (joins the thread); the image stays mapped for PsyCross. */
void openpete_disc_cache_shutdown(void);
/* Exit census: misses and total blocked time. */
void openpete_disc_cache_census(uint64_t* misses, int64_t* waited_ns,
                                size_t* bytes, int64_t* load_ns);
#ifdef __cplusplus
}
#endif
#endif
