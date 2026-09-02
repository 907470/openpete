/* sfx_pack — drop-in sound-effect replacement, the texture pack's twin.
 *
 * Sources (later wins): the base dir (`--sfxpack-dir` / OPENPETE_SFXPACK_DIR
 * / <exe>/assets/sfx), the user's data-dir assets/sfx, then one source
 * per enabled mod asset root (<root>/sfx) in enabled order. Files are
 * named by identity (core/host/sfx_ident.h); `own/` is not scanned.
 *
 * Substitution happens inside PsyCross at the voice's PCM upload
 * (PsyX_SPUAL_SetSampleOverride): the guest, the SPU RAM image, the
 * attr mirror and the one-shot duration scan all see the stock sample;
 * only the audible buffer changes. Everything here runs on the thread
 * that keys voices and blasts banks (the sim thread): the scan, the WAV
 * decode, the identity cache and the override callback, so no lock is
 * needed beyond the SPU mutex PsyCross already holds at the callback.
 *
 * Identity at runtime is a (generation, address) cache filled on miss
 * by hashing the sample straight out of SPU RAM — the level bank blast
 * bumps the generation, and any other SPU write invalidates the cached
 * addresses it overlaps (dragon speech lands mid-level). */
#ifndef OPENPETE_SFX_PACK_H
#define OPENPETE_SFX_PACK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void sfx_pack_init(void);
void sfx_pack_set_dir_override(const char* dir); /* before init */

/* The level bank was (re)blasted: new generation — rescan sources so an
 * edited WAV lands on the next level entry, drop every cached identity
 * and the decoded PCM of files that changed. */
void sfx_pack_generation_bump(void);
/* Any other SPU RAM write: forget identities inside [addr, addr+len). */
void sfx_pack_invalidate_range(uint32_t addr, uint32_t len);

/* Game hook: the base pitch the game will keep refreshing voice `voice`
 * with (its ActiveSound pitch — the def's, or the per-play override).
 * <= 0 = unknown; tagged files then cannot match. */
typedef int (*sfx_pack_pitch_reader_fn)(int voice);
void sfx_pack_set_pitch_reader(sfx_pack_pitch_reader_fn fn);

/* Game hook for the symbolic form: the SoundTable slot names that map
 * to the def keyed at `addr` in the current tables. Returns the count
 * written (<= max). */
typedef int (*sfx_pack_name_resolver_fn)(uint32_t addr, const char** names,
                                         int max);
void sfx_pack_set_name_resolver(sfx_pack_name_resolver_fn fn);

typedef struct {
    uint64_t keyed;       /* override callbacks (voice key-ons with a decode) */
    uint64_t resolved;    /* identity known (END flag found) */
    uint64_t replaced;    /* a pack file served */
    uint64_t rings_out;   /* served file outlasts stock; its tail rings out */
    uint64_t verify_ok;   /* OPENPETE_SFXPACK_VERIFY: PCM identical to stock */
    uint64_t verify_bad;
    int      active;      /* any file in any source */
    int      files;
} sfx_pack_census_t;
void sfx_pack_census(sfx_pack_census_t* out);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_SFX_PACK_H */
