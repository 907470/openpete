/* wav_loader — thin SDL3 SDL_LoadWAV wrapper returning an interleaved
 * int16 PCM blob, used by the music WAV override, mod sounds and the mod
 * registry's custom-level music probe. */
#ifndef OPENPETE_WAV_LOADER_H
#define OPENPETE_WAV_LOADER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t* samples;     /* interleaved; SDL-owned, release via op_wav_free */
    uint32_t frame_count; /* samples per channel */
    int      channels;    /* 1 or 2 */
    int      sample_rate; /* Hz */
} op_wav_pcm;

/** @brief Load a WAV file as interleaved int16 PCM, without any format conversion.
 *
 * Accepts SDL_AUDIO_S16LE with 1 or 2 channels at 11025, 22050, 32000,
 * 44100 or 48000 Hz. A file SDL_LoadWAV cannot read, or any other format,
 * logs one warning under the "music" tag and fails. The samples buffer is
 * SDL-allocated; release it with op_wav_free.
 * @param path WAV file path.
 * @param out Receives the PCM on success; untouched on failure.
 * @return 0 on success, -1 on failure. */
int  op_wav_load(const char* path, op_wav_pcm* out);
void op_wav_free(op_wav_pcm* pcm);

/** @brief Return the duration in seconds of a WAV that op_wav_load accepts, without keeping the PCM.
 *
 * Loads the whole file through op_wav_load, divides the per-channel
 * sample count by the sample rate, and frees the buffer.
 * @param path WAV file path.
 * @return Duration in seconds; 0.0 when op_wav_load fails, which logs the
 *         reason. */
double op_wav_probe_seconds(const char* path);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_WAV_LOADER_H */
