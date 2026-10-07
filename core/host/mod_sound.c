/* mod_sound - one-shot mod sound effects (sfx_play, the modding
 * framework's engine-services contract).
 *
 * HOST-SIDE ADDITIVE audio: clips played on the SPU render's host inputs
 * (PsyX_SPUAL_HostPlay), added to the console's output after its main
 * volume and advanced in guest time, so the device, --record and the
 * audio dump carry them in one mix. Never touches SPU registers or guest
 * state - playing a sound from any hook context is sim-neutral by
 * construction, so it is legal from tick AND present hooks (audio is
 * presentation, not render output; the render-purity contract concerns
 * sim effects). A play from a speculated tick is rewound with the
 * speculation and made again by the committed tick; a play from a present
 * hook persists.
 *
 * --mute arrives through openpete_mod_sound_set_mute(), which stops
 * whatever is in flight and plays nothing while set.
 *
 * WAVs load through op_wav_load (PCM int16 mono/stereo) and are cached by
 * path - repeated plays reuse the clip. Callers that already hold decoded
 * PCM (a synthesized tone, a sample pulled out of game data) take the _pcm
 * entry point and are cached by a caller-chosen key instead, so nothing
 * has to be staged on disk to be playable. All failures are
 * warn-and-return; a missing file can never brick a mod, let alone the
 * launch. */

#include "openpete_mods.h"
#include "wav_loader.h"
#include "openpete_log.h"
#include "mod_vec.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The SPU render's host inputs (PsyCross src/audio/PsyX_SPUAL.h). */
extern int  PsyX_SPUAL_HostBufferCreate(const short* pcm, int frames, int channels, unsigned int rate);
extern int  PsyX_SPUAL_HostPlay(int buffer, float gain, int persist);
extern void PsyX_SPUAL_HostStopAll(void);

/* Clips the cache allocates up front; it grows past this. */
#define SND_CACHE_INIT 16

typedef struct {
    char key[1024];   /* WAV path, or a caller-chosen label for raw PCM */
    int  clip;        /* host-input clip id */
} snd_cache_t;

static mod_fvec s_cache;      /* snd_cache_t, one per uploaded key */
static int      s_muted = 0;

/** @brief Return the cached clip for key, or 0 when key has not been uploaded. */
static int snd_cache_find(const char* key) {
    for (size_t i = 0; i < mod_fvec_count(&s_cache); i++) {
        const snd_cache_t* c = (const snd_cache_t*)mod_fvec_at(&s_cache, i);
        if (strcmp(c->key, key) == 0) return c->clip;
    }
    return 0;
}

/** @brief Copy interleaved s16 PCM into a new host-input clip cached under key, and return the clip or 0 on failure (logged).
 *
 * A channels value other than 2 uploads as mono. The render copies the
 * samples, so the caller may free them. The cache is created on first use. */
static int snd_cache_upload(const char* key, const int16_t* pcm,
                            uint32_t frame_count, int channels,
                            uint32_t sample_rate) {
    const int clip = PsyX_SPUAL_HostBufferCreate(pcm, (int)frame_count,
                                                 channels == 2 ? 2 : 1, sample_rate);
    if (!clip) {
        OPL_WARN_F("mods", "sfx_play: clip upload failed for %s", key);
        return 0;
    }
    if (!s_cache.elem_size)
        mod_fvec_init(&s_cache, sizeof(snd_cache_t), SND_CACHE_INIT,
                      "snd_cache", SND_CACHE_INIT);
    snd_cache_t* c = (snd_cache_t*)mod_fvec_push(&s_cache);
    if (!c) {
        OPL_ERROR_F("mods", "sfx_play: sound cache allocation failed - "
                    "%s not cached", key);
        return clip;
    }
    snprintf(c->key, sizeof c->key, "%s", key);
    c->clip = clip;
    return clip;
}

/** @brief Return the clip for the WAV at path, loading and caching it on first use, or 0 on failure.
 *
 * The decoded samples are freed once the render holds its copy. A load
 * failure is logged by the loader and an upload failure by snd_cache_upload. */
static int snd_clip_for(const char* path) {
    int cached = snd_cache_find(path);
    if (cached) return cached;
    op_wav_pcm pcm;
    if (op_wav_load(path, &pcm) != 0) return 0; /* loader logged why */
    int clip = snd_cache_upload(path, pcm.samples, pcm.frame_count,
                                pcm.channels, pcm.sample_rate);
    op_wav_free(&pcm);
    return clip;
}

/** @brief Play clip once at gain and return 0, or -1 when the render refused it.
 *
 * The gain is clamped to [0, 4]. While muted nothing plays and 0 is
 * returned. A play from a present hook persists through a discarded
 * speculation. */
static int snd_play_clip(int clip, float gain) {
    if (s_muted) return 0; /* muted is a silence, not a failure */
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 4.0f) gain = 4.0f;
    return PsyX_SPUAL_HostPlay(clip, gain, openpete_mods_in_present ? 1 : 0);
}

void openpete_mod_sound_set_mute(int on) {
    s_muted = on ? 1 : 0;
    if (s_muted) PsyX_SPUAL_HostStopAll();
}

int openpete_mod_sound_play(const char* path, float gain) {
    if (!path) return -1;
    int clip = snd_clip_for(path);
    if (!clip) return -1;
    return snd_play_clip(clip, gain);
}

int openpete_mod_sound_play_pcm(const char* key, const int16_t* pcm,
                                uint32_t frame_count, int channels,
                                uint32_t sample_rate, float gain) {
    if (!key) return -1;
    int clip = snd_cache_find(key);
    if (!clip) {
        if (!pcm || !frame_count || (channels != 1 && channels != 2))
            return -1;
        clip = snd_cache_upload(key, pcm, frame_count, channels, sample_rate);
        if (!clip) return -1;
    }
    return snd_play_clip(clip, gain);
}
