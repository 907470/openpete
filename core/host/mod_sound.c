/* mod_sound — one-shot mod sound effects (sfx_play, the modding
 * framework's engine-services contract).
 *
 * HOST-SIDE ADDITIVE audio: OpenAL static buffers played through a small
 * fire-and-forget source pool on the context PsyCross owns (the same
 * piggyback as music_override.c). Never touches the SPU or guest state —
 * playing a sound from any hook context is sim-neutral by construction,
 * so it is legal from tick AND present hooks (audio is presentation, not
 * render output; the render-purity contract concerns sim effects).
 *
 * These sources sit on PsyCross's AL context but outside its SPU mixer, so
 * the SPU's own mute flag never reaches them. --mute arrives here through
 * openpete_mod_sound_set_mute() instead — the same push music_override
 * takes — and stops whatever is already in flight.
 *
 * WAVs load through op_wav_load (PCM int16 mono/stereo) and are cached
 * by path — repeated plays reuse the AL buffer. Callers that already hold
 * decoded PCM (a synthesized tone, a sample pulled out of game data) take
 * the _pcm entry point and are cached by a caller-chosen key instead, so
 * nothing has to be staged on disk to be playable. All failures are
 * warn-and-return; a missing file or dead AL context can never brick a
 * mod, let alone the launch. */

#include "wav_loader.h"
#include "openpete_log.h"
#include "mod_vec.h"

#include <AL/al.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Buffers the cache allocates up front; it grows past this. */
#define SND_CACHE_INIT 16
#define SND_SOURCES    8

typedef struct {
    char   key[1024];   /* WAV path, or a caller-chosen label for raw PCM */
    ALuint buf;
} snd_cache_t;

static mod_fvec    s_cache;      /* snd_cache_t, one per uploaded key */
static ALuint      s_sources[SND_SOURCES];
static int         s_inited = 0; /* 0 = not yet, 1 = ok, -1 = AL dead */
static int         s_muted  = 0;

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

/* Cached AL buffer for `key`, or 0 if this key has not been uploaded. */
static ALuint snd_cache_find(const char* key) {
    for (size_t i = 0; i < mod_fvec_count(&s_cache); i++) {
        const snd_cache_t* c = (const snd_cache_t*)mod_fvec_at(&s_cache, i);
        if (strcmp(c->key, key) == 0) return c->buf;
    }
    return 0;
}

/* Upload interleaved s16 PCM under `key`. Returns the AL buffer, 0 on
 * failure (logged). AL copies the samples, so the caller may free them. */
static ALuint snd_cache_upload(const char* key, const int16_t* pcm,
                               uint32_t frame_count, int channels,
                               uint32_t sample_rate) {
    ALuint buf = 0;
    alGetError();
    alGenBuffers(1, &buf);
    alBufferData(buf,
                 channels == 2 ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16,
                 pcm,
                 (ALsizei)(frame_count * (uint32_t)channels * 2u),
                 (ALsizei)sample_rate);
    if (alGetError() != AL_NO_ERROR) {
        OPL_WARN_F("mods", "sfx_play: AL buffer upload failed for %s", key);
        if (buf) alDeleteBuffers(1, &buf);
        return 0;
    }
    if (!s_cache.elem_size)
        mod_fvec_init(&s_cache, sizeof(snd_cache_t), SND_CACHE_INIT,
                      "snd_cache", SND_CACHE_INIT);
    snd_cache_t* c = (snd_cache_t*)mod_fvec_push(&s_cache);
    if (!c) {
        OPL_ERROR_F("mods", "sfx_play: sound cache allocation failed — "
                    "%s not cached", key);
        alDeleteBuffers(1, &buf);
        return 0;
    }
    snprintf(c->key, sizeof c->key, "%s", key);
    c->buf = buf;
    return buf;
}

static ALuint snd_buffer_for(const char* path) {
    ALuint cached = snd_cache_find(path);
    if (cached) return cached;
    op_wav_pcm pcm;
    if (op_wav_load(path, &pcm) != 0) return 0; /* loader logged why */
    ALuint buf = snd_cache_upload(path, pcm.samples, pcm.frame_count,
                                  pcm.channels, pcm.sample_rate);
    op_wav_free(&pcm);
    return buf;
}

/* Play `buf` on the first free source; all busy = steal the oldest slot
 * (round-robin) — one-shots are short, the clank beats silence. */
static int snd_play_buffer(ALuint buf, float gain) {
    if (s_muted) return 0; /* muted is a silence, not a failure */
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 4.0f) gain = 4.0f;
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

void openpete_mod_sound_set_mute(int on) {
    s_muted = on ? 1 : 0;
    if (s_muted && s_inited > 0)
        for (int i = 0; i < SND_SOURCES; i++) alSourceStop(s_sources[i]);
}

int openpete_mod_sound_play(const char* path, float gain) {
    if (!path || snd_init() < 0) return -1;
    ALuint buf = snd_buffer_for(path);
    if (!buf) return -1;
    return snd_play_buffer(buf, gain);
}

int openpete_mod_sound_play_pcm(const char* key, const int16_t* pcm,
                                uint32_t frame_count, int channels,
                                uint32_t sample_rate, float gain) {
    if (!key || snd_init() < 0) return -1;
    ALuint buf = snd_cache_find(key);
    if (!buf) {
        if (!pcm || !frame_count || (channels != 1 && channels != 2))
            return -1;
        buf = snd_cache_upload(key, pcm, frame_count, channels, sample_rate);
        if (!buf) return -1;
    }
    return snd_play_buffer(buf, gain);
}
