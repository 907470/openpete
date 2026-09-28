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
 * only the audible buffer changes. The scan, the identity cache and the
 * override callback run on the thread that keys voices and blasts banks
 * (the sim thread). WAV decoding runs on a decode worker, which decodes
 * every file entry after each scan; the callback serves resident PCM,
 * waits for an entry the worker is decoding, and decodes on its own
 * thread only an entry the worker has not reached. One mutex guards the
 * entry table and the decode results.
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

/** @brief Scan the sources, start the decode worker on the files found, and install the override callback.
 *
 * Without the table lock the pack stays off (logged). */
void sfx_pack_init(void);
/** @brief Name the base sfx directory in place of OPENPETE_SFXPACK_DIR and `assets/sfx`.
 *
 * Also suppresses the user data directory source. Read at each rescan,
 * so it is set before sfx_pack_init.
 * @param dir Directory path; NULL or "" clears the override. */
void sfx_pack_set_dir_override(const char* dir);

/** @brief Make the next voice key-on play pcm instead of the sample it keyed.
 *
 * The substitution is served by the sample-override callback at the
 * voice's PCM upload, ahead of any pack file and whether or not a pack is
 * installed; it plays at rate with the game's pitch writes ignored. As
 * with a pack file only the audible buffer changes: the voice, the music
 * and the SPU state run as stock. The arm is spent by the first
 * non-speculative key-on; a speculative (runahead) key-on is served
 * without spending it. pcm must outlive the arm, and the caller disarms
 * with sfx_pack_disarm_oneshot when the targeted play does not happen. A
 * call with NULL pcm, count <= 0 or rate 0 is ignored.
 * @param pcm Mono s16 samples.
 * @param count Sample count.
 * @param rate Playback rate in Hz. */
void sfx_pack_arm_oneshot(const int16_t* pcm, int count, unsigned int rate);
/** @brief Cancel a pending one-shot substitution so the next key-on plays as stock. */
void sfx_pack_disarm_oneshot(void);
/** @brief Report whether a one-shot substitution is armed and unspent.
 * @return Nonzero while the arm from sfx_pack_arm_oneshot is pending. */
int  sfx_pack_oneshot_armed(void);
/** @brief Register a symbolic replacement from an in-memory SPU-ADPCM body (the mod API's sfx_replace).
 *
 * The entry provides what a symbolic <name>.wav in the owner's
 * assets/sfx would provide from disk; the body is decoded here, up to its
 * END flag. The substitution point and the audible-only contract are a
 * file's: the SPU RAM image, the voice mirror and the one-shot end tick
 * the guest observes stay stock, and a longer body rings out on an orphan
 * source. Within the symbolic match tier a registration outranks a file.
 * The entry serves only while its owner is enabled, checked at each
 * key-on; an empty owner always serves. Registering the same (owner,
 * name) again replaces the entry. Sim thread only.
 * @param owner Registering mod id; NULL is the engine ("").
 * @param name SoundTable slot name, under 64 bytes.
 * @param adpcm SPU-ADPCM body.
 * @param len Body bytes, 16 to 1 MiB.
 * @param pitch SPU pitch (0x1000 = 44.1 kHz) that pins the playback rate
 *        and ignores the game's pitch writes; 0 or less plays the sample at
 *        44.1 kHz under the game's pitch writes.
 * @return 0 on success; -1 for a bad name or length, a body that decodes
 *         to no samples, or an allocation failure. */
int  sfx_pack_register_symbolic_adpcm(const char* owner, const char* name,
                                      const void* adpcm, uint32_t len,
                                      int pitch);

/** @brief Start a new identity generation after the level bank blast.
 *
 * Drops every cached sample identity and rescans the sources, so an
 * edited WAV takes effect at the next level entry: entries whose file
 * vanished are dropped, entries whose file changed size or mtime release
 * their decoded PCM, and unchanged entries keep it; the decode worker is
 * then woken for the entries without PCM. */
void sfx_pack_generation_bump(void);
/** @brief Forget every cached sample identity whose ADPCM bytes overlap [addr, addr + len).
 *
 * Called for any SPU RAM write other than the level bank blast.
 * @param addr SPU RAM byte address of the write.
 * @param len Bytes written. */
void sfx_pack_invalidate_range(uint32_t addr, uint32_t len);

/* Game hook: the base pitch the game will keep refreshing voice `voice`
 * with (its ActiveSound pitch — the def's, or the per-play override).
 * <= 0 = unknown; tagged files then cannot match. */
typedef int (*sfx_pack_pitch_reader_fn)(int voice);
/** @brief Install the hook that reports a voice's base pitch to the matcher.
 * @param fn Hook; NULL leaves the pitch unknown, so tagged files never match. */
void sfx_pack_set_pitch_reader(sfx_pack_pitch_reader_fn fn);

/* Game hook for the symbolic form: the SoundTable slot names that map
 * to the def keyed at `addr` in the current tables. Returns the count
 * written (<= max). */
typedef int (*sfx_pack_name_resolver_fn)(uint32_t addr, const char** names,
                                         int max);
/** @brief Install the hook that maps a sample address to its SoundTable slot names.
 * @param fn Hook; NULL leaves no slot names, so symbolic files never match by name. */
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
    /* Time the override callback spent on the calling (sim) thread, over
     * committed key-ons: the longest call, the 99th percentile (upper edge
     * of a 100 us bucket), and the pack decodes the callback ran itself. */
    uint64_t cb_max_us;
    uint64_t cb_p99_us;
    uint64_t sync_decodes;
} sfx_pack_census_t;
/** @brief Copy the sfx pack census, computing the callback-time percentile from its histogram.
 * @param out Receives the census. */
void sfx_pack_census(sfx_pack_census_t* out);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_SFX_PACK_H */
