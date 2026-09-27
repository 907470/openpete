/* music_override — play user-supplied PCM WAVs from assets/music/ in
 * place of the game's XA-ADPCM music streams. The XA pump never reads
 * the disc image while an override is active.
 *
 * Game-agnostic: the caller supplies the track index (read from game
 * RAM at the game layer — core/ carries no game addresses). */
#ifndef OPENPETE_MUSIC_OVERRIDE_H
#define OPENPETE_MUSIC_OVERRIDE_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Resolve the base music roots and parse each root's optional manifest.txt.
 *
 * The roots are assets/music/ under SDL_GetBasePath() (shipped) and
 * music/ under the user's writable assets data directory, the second
 * dropped when it equals the first. Each root's manifest.txt maps
 * "<index> <relative path>" lines; the user root is parsed second, so its
 * entries replace the shipped ones track by track. Runs once until
 * music_override_shutdown; the start functions call it lazily, and boot
 * code calls it to log the search path early. */
void music_override_init(void);
/** @brief Stop playback, release the AL objects, join the streaming worker and forget the roots and manifests.
 *
 * Logs a stream census line (starts, bytes read, resident ring bytes,
 * longest start on the calling thread) when any stream started. */
void music_override_shutdown(void);

/** @brief Start WAV playback in place of a starting music stream when an override file exists for the track.
 *
 * The file is looked up in the enabled mods' asset roots first, the
 * later-enabled mod first, each by its music/manifest.txt entry and then
 * music/track_NN.wav; then in the user and shipped base roots by their
 * manifest entry, else track_NN.wav. A missing file is not latched, so a
 * file added mid-process is found at the next start; a file whose header
 * is rejected is latched as failed until the modset toggle epoch changes.
 * Returns 0 without probing the mod roots when neither base root resolved.
 * Only the RIFF header and the first 8192 sample frames are read on the
 * calling thread; a streaming worker reads the rest into a fixed ring of
 * 131072 frames (512 KiB), so neither the start cost nor the memory held
 * grows with the track's length, and nothing stays resident after the
 * stream is replaced. The playback position the game reads back belongs
 * to the game layer's virtual CD head, not to this module.
 * @param track_index Music table index, 0 to 63.
 * @param start_sector The stream's LBA, used only in the log line.
 * @return 1 when playback started, 0 to fall back to the XA path. */
int  music_override_try_start(int track_index, int start_sector);

/** @brief Start WAV playback of an explicit file in place of a starting music stream.
 *
 * Used for content with no stock track index, such as a custom level's
 * own song. Streamed as music_override_try_start streams; a rejected
 * header is latched until the path changes.
 * @param path WAV file path; NULL or "" returns 0.
 * @param start_sector The stream's LBA, used only in the log line.
 * @return 1 when playback started, 0 to fall back to the XA path. */
int  music_override_try_start_path(const char* path, int start_sector);
/** @brief End override playback: the streaming worker drops its stream and the AL source is stopped and emptied.
 *
 * No-op while no override is active. */
void music_override_stop(void);
int  music_override_is_active(void);

/** @brief Refill the four-buffer AL ring with the next PCM chunks and keep the source playing.
 *
 * Processed buffers are unqueued and refilled with up to 8192 sample
 * frames each from what the streaming worker has read ahead, and the
 * worker is woken to read more; once the WAV is exhausted the source
 * drains and stops. Looping is the game's: it re-issues the stream read when the CD
 * head passes the track end, which restarts playback from frame 0. No-op
 * while no override is active. Called on every CdSync thunk, beside
 * PsyX_XA_Pump. */
void music_override_pump(void);

/** @brief Set the game-driven music gain, applied unless a forced gain is set.
 *
 * Mirrors PsyX_XA_SetVolume; the game layer passes the CD volume times
 * the SPU master volume.
 * @param linear_gain Linear AL source gain. */
void music_override_set_gain(float linear_gain);
/** @brief Set or release a gain that overrides the game-driven one.
 *
 * Mirrors PsyX_XA_ForceGain; --mute passes 0.
 * @param linear_gain Forced linear gain when >= 0; a negative value
 *        releases the override. */
void music_override_set_forced_gain(float linear_gain);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_MUSIC_OVERRIDE_H */
