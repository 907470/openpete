/* music_override — play user-supplied PCM WAVs from assets/music/ in
 * place of the game's XA-ADPCM music streams. The XA pump never reads
 * the disc image while an override is active.
 *
 * Game-agnostic: the caller supplies the track index (read from game
 * RAM at the game layer — core/ carries no game addresses). See
 * notes/plans/openpete-music-wav-override.md. */
#ifndef OPENPETE_MUSIC_OVERRIDE_H
#define OPENPETE_MUSIC_OVERRIDE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Discover assets/music/ next to the executable and parse the optional
 * manifest.txt. Idempotent; try_start calls it lazily — exposing it
 * lets boot code log the search path early. */
void music_override_init(void);
void music_override_shutdown(void);

/* Attempt to take ownership of a starting music stream. Returns 1 and
 * starts WAV playback if an override file exists for `track_index`;
 * returns 0 to fall back to the XA path. `start_sector` is the stream's
 * LBA — logged for identification; the playback position the game reads
 * back is owned by the game layer's virtual CD head, not by us. */
int  music_override_try_start(int track_index, int start_sector);
void music_override_stop(void);
int  music_override_is_active(void);

/* Schedule pending PCM into the AL ring; call once per game frame,
 * same cadence as PsyX_XA_Pump. */
void music_override_pump(void);

/* Mirror PsyX_XA_SetVolume / PsyX_XA_ForceGain semantics: set_gain is
 * the game-driven CD volume (applied unless forced); a forced gain
 * >= 0 overrides it (used by --mute), -1 releases the override. */
void music_override_set_gain(float linear_gain);
void music_override_set_forced_gain(float linear_gain);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_MUSIC_OVERRIDE_H */
