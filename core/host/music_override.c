#include "music_override.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <AL/al.h>
#include <AL/alc.h>
#include <SDL3/SDL.h>

#include "openpete_log.h"
#include "openpete_mods.h" /* asset-overlay roots */
#include "openpete_time.h"
#include "wav_loader.h"

#define MO_MAX_TRACKS   64    /* m_MusicTable capacity; 48 used in Spyro 1 */
#define MO_RING         4
#define MO_CHUNK_FRAMES 8192u /* ~0.19 s @ 44.1 kHz per AL buffer */
#define MO_SECTOR_HZ    75    /* LBA rate CdlGetlocP reports */

enum { MO_UNLOADED = 0, MO_LOADED, MO_FAILED };

static struct {
    int   discovered;
    char  dir[1024];                /* "" → no override directory */
    char* manifest[MO_MAX_TRACKS];  /* malloc'd paths relative to dir */

    op_wav_pcm cache[MO_MAX_TRACKS];
    uint8_t    cache_state[MO_MAX_TRACKS];

    int    al_ready;
    ALuint source;
    ALuint buffers[MO_RING];
    ALuint free_bufs[MO_RING];
    int    n_free;

    int       active;
    int       track;
    int       start_sector;
    op_time_t start_ns;
    uint32_t  cursor;               /* frames consumed from pcm */
    const op_wav_pcm* pcm;

    float gain;
    float forced_gain;              /* >= 0 → overrides `gain` */
} s = { .gain = 1.0f, .forced_gain = -1.0f };

static void apply_gain(void)
{
    if (!s.al_ready) return;
    alSourcef(s.source, AL_GAIN,
              s.forced_gain >= 0.0f ? s.forced_gain : s.gain);
}

static void parse_manifest(void)
{
    char path[sizeof(s.dir) + 16];
    snprintf(path, sizeof(path), "%smanifest.txt", s.dir);
    FILE* f = fopen(path, "r");
    if (!f) return;

    char line[1100];
    int  n_entries = 0;
    while (fgets(line, sizeof(line), f)) {
        int  idx;
        char rel[1024];
        if (line[0] == '#') continue;
        if (sscanf(line, "%d %1023s", &idx, rel) != 2) continue;
        if (idx < 0 || idx >= MO_MAX_TRACKS) {
            OPL_WARN_F("music", "manifest.txt: track %d out of range", idx);
            continue;
        }
        free(s.manifest[idx]);
        s.manifest[idx] = (char*)malloc(strlen(rel) + 1);
        if (s.manifest[idx]) {
            strcpy(s.manifest[idx], rel);
            n_entries++;
        }
    }
    fclose(f);
    OPL_INFO_F("music", "wav override manifest: %d entr%s from %s",
               n_entries, n_entries == 1 ? "y" : "ies", path);
}

void music_override_init(void)
{
    if (s.discovered) return;
    s.discovered = 1;

    const char* base = SDL_GetBasePath();
    if (!base) {
        OPL_WARN_F("music", "wav override disabled: SDL_GetBasePath: %s",
                   SDL_GetError());
        return;
    }
    snprintf(s.dir, sizeof(s.dir), "%sassets/music/", base);
    OPL_INFO_F("music", "wav override search path: %s", s.dir);
    parse_manifest();
}

static const op_wav_pcm* load_track(int track)
{
    if (s.cache_state[track] == MO_LOADED) return &s.cache[track];
    if (s.cache_state[track] == MO_FAILED) return NULL;

    char path[sizeof(s.dir) + 1040];
    /* Asset overlay: mod roots outrank the base dir; later-enabled mods
     * outrank earlier ones (single priority truth with the texture
     * namespace). First hit wins. */
    int found = 0;
    for (int r = openpete_mods_asset_root_count() - 1; r >= 0 && !found;
         r--) {
        const char* root = openpete_mods_asset_root(r, NULL);
        if (!root) continue;
        snprintf(path, sizeof(path), "%s/music/track_%02d.wav", root,
                 track);
        FILE* mf = fopen(path, "rb");
        if (mf) { fclose(mf); found = 1; }
    }
    if (!found) {
        if (s.manifest[track])
            snprintf(path, sizeof(path), "%s%s", s.dir, s.manifest[track]);
        else
            snprintf(path, sizeof(path), "%strack_%02d.wav", s.dir, track);

        /* Missing file is the common no-override case — probe quietly and
         * don't latch, so files dropped in mid-session are picked up. */
        FILE* f = fopen(path, "rb");
        if (!f) return NULL;
        fclose(f);
    }

    if (op_wav_load(path, &s.cache[track]) != 0) {
        s.cache_state[track] = MO_FAILED;  /* op_wav_load warned */
        return NULL;
    }
    s.cache_state[track] = MO_LOADED;
    OPL_INFO_F("music", "wav override: track %02d <- %s (%u frames, %d ch, %d Hz)",
               track, path, s.cache[track].frame_count,
               s.cache[track].channels, s.cache[track].sample_rate);
    return &s.cache[track];
}

static int ensure_al(void)
{
    if (s.al_ready) return 1;
    if (!alcGetCurrentContext()) {
        OPL_WARN_F("music", "wav override: no OpenAL context; using XA path");
        return 0;
    }
    alGenSources(1, &s.source);
    alGenBuffers(MO_RING, s.buffers);
    alSourcei(s.source, AL_SOURCE_RELATIVE, AL_TRUE);
    alSourcef(s.source, AL_PITCH, 1.0f);
    for (int i = 0; i < MO_RING; ++i) s.free_bufs[i] = s.buffers[i];
    s.n_free   = MO_RING;
    s.al_ready = 1;
    apply_gain();
    return 1;
}

void music_override_stop(void)
{
    if (!s.active) return;
    s.active = 0;
    s.pcm    = NULL;
    if (s.al_ready) {
        alSourceStop(s.source);
        ALint queued = 0;
        alGetSourcei(s.source, AL_BUFFERS_QUEUED, &queued);
        while (queued-- > 0) {
            ALuint b = 0;
            alSourceUnqueueBuffers(s.source, 1, &b);
            if (s.n_free < MO_RING) s.free_bufs[s.n_free++] = b;
        }
    }
}

int music_override_try_start(int track_index, int start_sector)
{
    music_override_init();
    if (!s.dir[0]) return 0;
    if (track_index < 0 || track_index >= MO_MAX_TRACKS) return 0;

    const op_wav_pcm* pcm = load_track(track_index);
    if (!pcm) return 0;
    if (!ensure_al()) return 0;

    music_override_stop();
    s.track        = track_index;
    s.start_sector = start_sector;
    s.start_ns     = op_now();
    s.cursor       = 0;
    s.pcm          = pcm;
    s.active       = 1;
    apply_gain();
    music_override_pump();  /* prime the ring so playback starts now */
    OPL_INFO_F("music", "wav override active: track %02d (LBA %d)",
               track_index, start_sector);
    return 1;
}

int music_override_is_active(void) { return s.active; }

int music_override_get_sector(void)
{
    if (!s.active) return 0;
    int64_t elapsed_ns = (int64_t)(op_now() - s.start_ns);
    return s.start_sector + (int)(elapsed_ns * MO_SECTOR_HZ / 1000000000LL);
}

void music_override_pump(void)
{
    if (!s.active || !s.al_ready) return;

    ALint processed = 0;
    alGetSourcei(s.source, AL_BUFFERS_PROCESSED, &processed);
    while (processed-- > 0) {
        ALuint b = 0;
        alSourceUnqueueBuffers(s.source, 1, &b);
        if (s.n_free < MO_RING) s.free_bufs[s.n_free++] = b;
    }

    const op_wav_pcm* p = s.pcm;
    while (s.n_free > 0 && s.cursor < p->frame_count) {
        uint32_t n = p->frame_count - s.cursor;
        if (n > MO_CHUNK_FRAMES) n = MO_CHUNK_FRAMES;
        ALuint b = s.free_bufs[--s.n_free];
        alBufferData(b,
                     p->channels == 2 ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16,
                     p->samples + (size_t)s.cursor * (size_t)p->channels,
                     (ALsizei)(n * (uint32_t)p->channels * sizeof(int16_t)),
                     p->sample_rate);
        alSourceQueueBuffers(s.source, 1, &b);
        s.cursor += n;
    }

    /* WAV exhausted → ring drains and the source stops; the synthetic
     * playhead keeps advancing so the game's track-end check fires. */
    ALint queued = 0;
    alGetSourcei(s.source, AL_BUFFERS_QUEUED, &queued);
    if (queued > 0) {
        ALint state = 0;
        alGetSourcei(s.source, AL_SOURCE_STATE, &state);
        if (state != AL_PLAYING) alSourcePlay(s.source);
    }
}

void music_override_set_gain(float linear_gain)
{
    s.gain = linear_gain;
    if (s.al_ready && s.forced_gain < 0.0f)
        alSourcef(s.source, AL_GAIN, linear_gain);
}

void music_override_set_forced_gain(float linear_gain)
{
    s.forced_gain = linear_gain;
    apply_gain();
}

void music_override_shutdown(void)
{
    music_override_stop();
    if (s.al_ready) {
        alDeleteSources(1, &s.source);
        alDeleteBuffers(MO_RING, s.buffers);
        s.source   = 0;
        s.al_ready = 0;
        s.n_free   = 0;
    }
    for (int i = 0; i < MO_MAX_TRACKS; ++i) {
        free(s.manifest[i]);
        s.manifest[i] = NULL;
        if (s.cache_state[i] == MO_LOADED) op_wav_free(&s.cache[i]);
        s.cache_state[i] = MO_UNLOADED;
    }
    s.discovered = 0;
    s.dir[0]     = '\0';
}
