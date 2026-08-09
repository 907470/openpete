/* wav_loader — thin SDL3 SDL_LoadWAV wrapper returning an interleaved
 * int16 PCM blob. Part of the music WAV override; see
 * notes/plans/openpete-music-wav-override.md (Phase 3). */
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

/* Load `path`. Accepts PCM int16 mono/stereo at 11025/22050/32000/
 * 44100/48000 Hz; anything needing conversion is rejected with a
 * one-line warn. Returns 0 on success; on failure returns -1 and
 * leaves `out` untouched. */
int  op_wav_load(const char* path, op_wav_pcm* out);
void op_wav_free(op_wav_pcm* pcm);

/* Duration in seconds of a WAV op_wav_load would accept, without keeping
 * the PCM. 0.0 on missing/unloadable/unsupported file. */
double op_wav_probe_seconds(const char* path);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_WAV_LOADER_H */
