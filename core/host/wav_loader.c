#include "wav_loader.h"

#include <SDL3/SDL.h>

#include "openpete_log.h"

int op_wav_load(const char* path, op_wav_pcm* out)
{
    SDL_AudioSpec spec;
    Uint8* buf = NULL;
    Uint32 len = 0;

    if (!SDL_LoadWAV(path, &spec, &buf, &len)) {
        OPL_WARN_F("music", "wav: %s: %s", path, SDL_GetError());
        return -1;
    }

    int format_ok = (spec.format == SDL_AUDIO_S16LE)
                 && (spec.channels == 1 || spec.channels == 2)
                 && (spec.freq == 11025 || spec.freq == 22050
                     || spec.freq == 32000 || spec.freq == 44100
                     || spec.freq == 48000);
    if (!format_ok) {
        OPL_WARN_F("music",
                   "wav: %s: unsupported format (fmt=0x%X ch=%d rate=%d); "
                   "need PCM int16 mono/stereo @ 11025/22050/32000/44100/48000",
                   path, (unsigned)spec.format, (int)spec.channels,
                   (int)spec.freq);
        SDL_free(buf);
        return -1;
    }

    out->samples     = (int16_t*)buf;
    out->frame_count = len / (uint32_t)(sizeof(int16_t) * spec.channels);
    out->channels    = (int)spec.channels;
    out->sample_rate = (int)spec.freq;
    return 0;
}

double op_wav_probe_seconds(const char* path)
{
    /* SDL_LoadWAV reads the whole file; fine for a boot-time probe. */
    op_wav_pcm pcm;
    if (op_wav_load(path, &pcm) != 0) return 0.0;
    double sec = pcm.sample_rate > 0
                     ? (double)pcm.frame_count / (double)pcm.sample_rate
                     : 0.0;
    op_wav_free(&pcm);
    return sec;
}

void op_wav_free(op_wav_pcm* pcm)
{
    if (!pcm) return;
    SDL_free(pcm->samples);
    pcm->samples     = NULL;
    pcm->frame_count = 0;
}
