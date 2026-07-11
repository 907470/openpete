/* mod_sound — one-shot mod sound effects (sfx_play, the modding
 * framework's engine-services contract).
 *
 * HOST-SIDE ADDITIVE audio: OpenAL static buffers played through a small
 * fire-and-forget source pool on the context PsyCross owns (the same
 * piggyback as music_override.c). Never touches the SPU or guest state —
 * playing a sound from any hook context is sim-neutral by construction,
 * so it is legal from tick AND present hooks (audio is presentation, not
 * render output; the render-purity contract concerns sim effects).
 * --mute at the listener gain covers these too.
 *
 * WAVs load through op_wav_load (PCM int16 mono/stereo) and are cached
 * by path — repeated plays reuse the AL buffer. All failures are
 * warn-and-return; a missing file or dead AL context can never brick a
 * mod, let alone the launch. */

#include "wav_loader.h"
#include "openpete_log.h"

#include <AL/al.h>

#include <stdio.h>
#include <string.h>

#define SND_CACHE_MAX 16
#define SND_SOURCES   8

typedef struct {
    char   path[1024];
    ALuint buf;
} snd_cache_t;

static snd_cache_t s_cache[SND_CACHE_MAX];
static int         s_cache_count = 0;
static ALuint      s_sources[SND_SOURCES];
static int         s_inited = 0; /* 0 = not yet, 1 = ok, -1 = AL dead */

static int snd_init(void) {
    if (s_inited) return s_inited;
    alGetError();
    alGenSources(SND_SOURCES, s_sources);
    if (alGetError() != AL_NO_ERROR) {
        OPL_WARN_F("mods", "sfx_play: no OpenAL context — mod "
                   "sounds disabled this session");
        s_inited = -1;
        return s_inited;
    }
    for (int i = 0; i < SND_SOURCES; i++)
        alSourcei(s_sources[i], AL_SOURCE_RELATIVE, AL_TRUE);
    s_inited = 1;
    return s_inited;
}

static ALuint snd_buffer_for(const char* path) {
    for (int i = 0; i < s_cache_count; i++)
        if (strcmp(s_cache[i].path, path) == 0) return s_cache[i].buf;
    if (s_cache_count >= SND_CACHE_MAX) {
        OPL_WARN_F("mods", "sfx_play: sound cache full (%d)",
                   SND_CACHE_MAX);
        return 0;
    }
    op_wav_pcm pcm;
    if (op_wav_load(path, &pcm) != 0) return 0; /* loader logged why */
    ALuint buf = 0;
    alGetError();
    alGenBuffers(1, &buf);
    alBufferData(buf,
                 pcm.channels == 2 ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16,
                 pcm.samples,
                 (ALsizei)(pcm.frame_count * (uint32_t)pcm.channels * 2u),
                 pcm.sample_rate);
    op_wav_free(&pcm);
    if (alGetError() != AL_NO_ERROR) {
        OPL_WARN_F("mods", "sfx_play: AL buffer upload failed for %s",
                   path);
        if (buf) alDeleteBuffers(1, &buf);
        return 0;
    }
    snd_cache_t* c = &s_cache[s_cache_count++];
    snprintf(c->path, sizeof c->path, "%s", path);
    c->buf = buf;
    return buf;
}

int openpete_mod_sound_play(const char* path, float gain) {
    if (!path || snd_init() < 0) return -1;
    ALuint buf = snd_buffer_for(path);
    if (!buf) return -1;
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 4.0f) gain = 4.0f;
    /* First stopped/fresh source wins; all busy = steal the oldest slot
     * (round-robin) — one-shots are short, the clank beats silence. */
    static int s_rr = 0;
    ALuint src = 0;
    for (int i = 0; i < SND_SOURCES; i++) {
        ALint st = 0;
        alGetSourcei(s_sources[i], AL_SOURCE_STATE, &st);
        if (st != AL_PLAYING) { src = s_sources[i]; break; }
    }
    if (!src) src = s_sources[s_rr++ % SND_SOURCES];
    alSourceStop(src);
    alSourcei(src, AL_BUFFER, (ALint)buf);
    alSourcef(src, AL_GAIN, gain);
    alSourcePlay(src);
    return 0;
}
