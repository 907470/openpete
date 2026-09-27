/* wav_loader — thin SDL3 SDL_LoadWAV wrapper returning an interleaved
 * int16 PCM blob, used by the music WAV override, mod sounds and the mod
 * registry's custom-level music probe. */
#ifndef OPENPETE_WAV_LOADER_H
#define OPENPETE_WAV_LOADER_H

#include <stdint.h>
#include <stdio.h>

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

/** @brief Release the samples op_wav_load returned and zero the frame count.
 * @param pcm PCM to release; NULL is ignored. */
void op_wav_free(op_wav_pcm* pcm);

/* The layout of a WAV file's PCM data, read from its RIFF header. */
typedef struct {
    uint32_t frame_count; /* samples per channel in the data chunk, clamped
                           * to the bytes the file holds */
    int      channels;    /* 1 or 2 */
    int      sample_rate; /* Hz */
    long     data_offset; /* byte offset of the first sample */
} op_wav_info;

/** @brief Read a WAV file's RIFF header and leave the stream at its first sample, without reading the samples.
 *
 * Walks the RIFF/WAVE chunks (each padded to an even size) to the first
 * `data` chunk, which must follow a `fmt ` chunk describing 16-bit integer
 * PCM (format tag 1, or 0xFFFE with the PCM sub-format) with 1 or 2
 * channels at 11025, 22050, 32000, 44100 or 48000 Hz: the formats
 * op_wav_load accepts. The frame count is the data chunk's size, clamped to
 * the bytes after the chunk start, over the block size. A rejected file
 * logs one warning under the "music" tag.
 * @param f Stream opened in binary mode at offset 0.
 * @param path File path, for the warning.
 * @param out Receives the layout on success; untouched on failure.
 * @return 0 with @p f positioned at the first sample, or -1. */
int  op_wav_read_info(FILE* f, const char* path, op_wav_info* out);

/** @brief Return the duration in seconds of a WAV that op_wav_load accepts, from its header alone.
 *
 * Reads the RIFF header through op_wav_read_info and divides the frame
 * count by the sample rate; no sample is read.
 * @param path WAV file path.
 * @return Duration in seconds; 0.0 when the file cannot be opened or its
 *         header is rejected, which logs the reason. */
double op_wav_probe_seconds(const char* path);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_WAV_LOADER_H */
