#include "music_override.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <AL/al.h>
#include <AL/alc.h>
#include <SDL3/SDL.h>

#include "openpete_log.h"
#include "openpete_mods.h" /* asset-overlay roots */
#include "openpete_paths.h" /* the writable user assets root */
#include "wav_loader.h"

#define MO_MAX_TRACKS   64    /* m_MusicTable capacity; 48 used in Spyro 1 */
#define MO_RING         4
#define MO_CHUNK_FRAMES 8192u /* ~0.19 s @ 44.1 kHz per AL buffer */
/* Frames the streaming worker keeps decoded ahead of the AL ring: about
 * 2.7 s at 48 kHz, 512 KiB of stereo PCM, whatever the track length. */
#define MO_STREAM_FRAMES (1u << 17)

enum { MO_UNLOADED = 0, MO_FAILED = 2 };

/* Two base roots, lowest priority first: the shipped assets/music/ inside
 * the install (which may be read-only) and the user's own writable one.
 * Mod asset roots outrank both. */
enum { MO_DIR_SHIPPED = 0, MO_DIR_USER, MO_DIR_MAX };

static struct {
    int   discovered;
    char  dir[MO_DIR_MAX][1024];    /* "" -> that root is not in play */
    char* manifest[MO_MAX_TRACKS];  /* malloc'd paths relative to a dir */

    /* MO_FAILED latches a file whose header was rejected until the modset
     * epoch it was stamped under changes. */
    uint8_t    cache_state[MO_MAX_TRACKS];
    unsigned   cache_epoch[MO_MAX_TRACKS]; /* modset epoch at cache time */

    int    al_ready;
    ALuint source;
    ALuint buffers[MO_RING];
    ALuint free_bufs[MO_RING];
    int    n_free;

    int       active;
    int       track;
    int       channels;             /* of the active stream */
    int       sample_rate;

    float gain;
    float forced_gain;              /* >= 0 -> overrides `gain` */
} s = { .gain = 1.0f, .forced_gain = -1.0f };

/* The streaming worker. A start opens the file and reads its header and
 * first AL chunk on the calling thread, then hands the open stream to the
 * worker, which reads the rest into `ring` ahead of music_override_pump.
 * Everything below is guarded by `mx`; the worker does its file reads
 * outside it, into its own buffer, and copies them into the ring only if
 * `gen` has not moved meanwhile. The worker owns every stream it adopted
 * and closes it; a stream still in `next` belongs to the thread that put
 * it there. */
static struct {
    SDL_Thread*    th;
    SDL_Mutex*     mx;
    SDL_Condition* cv;
    int            quit;
    unsigned       gen;        /* bumped by every start and stop */
    FILE*          next;       /* stream awaiting adoption (NULL: none) */
    int            have_next;  /* the worker must drop its stream and adopt next */
    uint32_t       next_left;  /* frames of next not yet in the ring */
    int            channels;   /* of the stream in the ring */
    int16_t*       ring;       /* MO_STREAM_FRAMES frames of 2 samples */
    uint64_t       wr, rd;     /* frames written / consumed, monotonic */
    int            eof;        /* every frame of the stream is in the ring */
    /* census */
    uint32_t       starts;
    uint64_t       bytes_read;
    uint64_t       start_max_ns; /* longest start on the calling thread */
} st;

static int16_t s_chunk[MO_CHUNK_FRAMES * 2]; /* caller-thread staging */

/** @brief Set the AL source gain to the forced gain when one is set, else the linear gain.
 *
 * No-op until the AL source exists. */
static void apply_gain(void)
{
    if (!s.al_ready) return;
    alSourcef(s.source, AL_GAIN,
              s.forced_gain >= 0.0f ? s.forced_gain : s.gain);
}

/** @brief Parse one manifest line "<index> <relative path>" and return 1 when both fields parse.
 *
 * A line starting with '#' is a comment and returns 0. The path is one
 * whitespace-free token of at most 1023 bytes; cap below 1024 returns 0.
 * Shared by the base-root parse and the per-mod-root lookup. */
static int manifest_line(const char* line, int* idx, char* rel, size_t cap)
{
    if (line[0] == '#') return 0;
    if (cap < 1024) return 0;
    return sscanf(line, "%d %1023s", idx, rel) == 2;
}

/** @brief Read `<dir>/music/manifest.txt` and report whether any line maps `track`.
 *
 * The file is read on every call, not cached, because the set of mod roots
 * changes with the enabled mods; probe_mod_roots calls it once per enabled
 * mod root on every track start that reaches it. Each line parses into a local
 * buffer that is copied to `rel` only when its index is `track`, so the
 * last matching line wins and `rel` is untouched on a miss.
 * @param dir Mod asset root, without the `music/` suffix.
 * @param track Music table index.
 * @param rel Receives the relative path of the last line mapping `track`.
 * @param cap Capacity of `rel`; below 1024 no line parses.
 * @return 1 when a line maps `track`, else 0 (also when the file is absent). */
static int manifest_lookup(const char* dir, int track, char* rel, size_t cap)
{
    char path[1100];
    snprintf(path, sizeof path, "%s/music/manifest.txt", dir);
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[1100];
    int  hit = 0;
    while (fgets(line, sizeof line, f)) {
        int  idx;
        char cand[1024];
        /* Later lines win, matching the base-root parse. */
        if (manifest_line(line, &idx, cand, cap < sizeof cand ? cap : sizeof cand)
            && idx == track) {
            memcpy(rel, cand, strlen(cand) + 1);
            hit = 1;
        }
    }
    fclose(f);
    return hit;
}

/** @brief Load `<dir>manifest.txt` into the base-root manifest table, one malloc'd path per track.
 *
 * Lines parse through manifest_line; an index outside 0..MO_MAX_TRACKS-1
 * is logged and skipped. A later line or a later call replaces the
 * earlier path for the same index, and a failed allocation leaves the
 * slot NULL. A missing file is silent; the entry count is logged.
 * @param dir Base music root ending in '/'. */
static void parse_manifest(const char* dir)
{
    char path[sizeof(s.dir[0]) + 16];
    snprintf(path, sizeof(path), "%smanifest.txt", dir);
    FILE* f = fopen(path, "r");
    if (!f) return;

    char line[1100];
    int  n_entries = 0;
    while (fgets(line, sizeof(line), f)) {
        int  idx;
        char rel[1024];
        if (!manifest_line(line, &idx, rel, sizeof rel)) continue;
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
    if (base)
        snprintf(s.dir[MO_DIR_SHIPPED], sizeof(s.dir[0]), "%sassets/music/",
                 base);
    else
        OPL_WARN_F("music", "shipped wav overrides disabled: SDL_GetBasePath: %s",
                   SDL_GetError());

    char user[1024];
    if (openpete_paths_data_dir("assets", user, sizeof user, 0) == 0) {
        char dir[sizeof(s.dir[0])];
        snprintf(dir, sizeof dir, "%s/music/", user);
        if (strcmp(dir, s.dir[MO_DIR_SHIPPED]) != 0)
            snprintf(s.dir[MO_DIR_USER], sizeof(s.dir[0]), "%s", dir);
    }

    if (!s.dir[MO_DIR_SHIPPED][0] && !s.dir[MO_DIR_USER][0]) return;
    OPL_INFO_F("music", "wav override search path: %s%s%s",
               s.dir[MO_DIR_SHIPPED],
               s.dir[MO_DIR_USER][0] ? ", " : "", s.dir[MO_DIR_USER]);
    /* Later parse wins per track, so the user's manifest overrides the
     * shipped one entry by entry rather than all-or-nothing. */
    for (int d = 0; d < MO_DIR_MAX; d++)
        if (s.dir[d][0]) parse_manifest(s.dir[d]);
}

/** @brief Probe the enabled mods' asset roots for a track's file, the later-enabled mod first.
 *
 * Each root is probed by its music/manifest.txt entry, then by
 * music/track_NN.wav, so a mod can ship either. A disabled mod's root is
 * skipped, so a toggle lands at the next track start.
 * @param track Music table index.
 * @param path Receives the file path of the first hit.
 * @param cap Capacity of @p path.
 * @return 1 when a file exists, else 0. */
static int probe_mod_roots(int track, char* path, size_t cap)
{
    for (int r = openpete_mods_asset_root_count() - 1; r >= 0; r--) {
        const char* owner = NULL;
        const char* root = openpete_mods_asset_root(r, &owner);
        if (!root) continue;
        if (owner && *owner && !openpete_mods_is_enabled(owner)) continue;
        char rel[1024];
        if (manifest_lookup(root, track, rel, sizeof rel)) {
            snprintf(path, cap, "%s/music/%s", root, rel);
            FILE* mf = fopen(path, "rb");
            if (mf) { fclose(mf); return 1; }
        }
        snprintf(path, cap, "%s/music/track_%02d.wav", root, track);
        FILE* mf = fopen(path, "rb");
        if (mf) { fclose(mf); return 1; }
    }
    return 0;
}

/** @brief Probe the user then the shipped base root for a track's file, by its manifest entry, else track_NN.wav.
 * @param track Music table index.
 * @param path Receives the file path of the first hit.
 * @param cap Capacity of @p path.
 * @return 1 when a file exists, else 0. */
static int probe_base_roots(int track, char* path, size_t cap)
{
    for (int d = MO_DIR_MAX - 1; d >= 0; d--) {
        if (!s.dir[d][0]) continue;
        if (s.manifest[track])
            snprintf(path, cap, "%s%s", s.dir[d], s.manifest[track]);
        else
            snprintf(path, cap, "%strack_%02d.wav", s.dir[d], track);
        FILE* f = fopen(path, "rb");
        if (f) { fclose(f); return 1; }
    }
    return 0;
}

/** @brief Find the override file for a track, highest-priority root first.
 *
 * Clears a failure latch stamped under an older modset epoch, then probes
 * the mod roots (the asset overlay, single priority truth with the texture
 * namespace) and below them the base roots. A missing file latches
 * nothing, so a file added mid-session is found at the next start.
 * @param track Music table index.
 * @param path Receives the file path.
 * @param cap Capacity of @p path.
 * @return 1 when a file exists and is not latched failed, else 0. */
static int resolve_track(int track, char* path, size_t cap)
{
    unsigned ep = openpete_mods_toggle_epoch();
    if (s.cache_state[track] != MO_UNLOADED && s.cache_epoch[track] != ep)
        s.cache_state[track] = MO_UNLOADED;
    s.cache_epoch[track] = ep;
    if (s.cache_state[track] == MO_FAILED) return 0;
    return probe_mod_roots(track, path, cap) || probe_base_roots(track, path, cap);
}

/** @brief Copy frames into the worker ring at write position wr, wrapping at its end.
 *
 * Caller holds st.mx and has checked the ring has room for @p n frames.
 * @param src Interleaved samples, st.channels per frame.
 * @param n Frame count. */
static void ring_put(const int16_t* src, uint32_t n)
{
    const uint32_t ch = (uint32_t)st.channels;
    const uint32_t at = (uint32_t)(st.wr % MO_STREAM_FRAMES);
    const uint32_t first = n < MO_STREAM_FRAMES - at ? n : MO_STREAM_FRAMES - at;
    memcpy(st.ring + (size_t)at * ch, src, (size_t)first * ch * sizeof(int16_t));
    if (n > first)
        memcpy(st.ring, src + (size_t)first * ch, (size_t)(n - first) * ch * sizeof(int16_t));
    st.wr += n;
}

/** @brief Copy frames out of the worker ring at read position rd, wrapping at its end.
 *
 * Caller holds st.mx and has checked @p n frames are available.
 * @param dst Receives interleaved samples, st.channels per frame.
 * @param n Frame count. */
static void ring_get(int16_t* dst, uint32_t n)
{
    const uint32_t ch = (uint32_t)st.channels;
    const uint32_t at = (uint32_t)(st.rd % MO_STREAM_FRAMES);
    const uint32_t first = n < MO_STREAM_FRAMES - at ? n : MO_STREAM_FRAMES - at;
    memcpy(dst, st.ring + (size_t)at * ch, (size_t)first * ch * sizeof(int16_t));
    if (n > first)
        memcpy(dst + (size_t)first * ch, st.ring, (size_t)(n - first) * ch * sizeof(int16_t));
    st.rd += n;
}

/** @brief Read one block of the worker's stream into the ring, outside the lock for the file read.
 *
 * Caller holds st.mx; it is released for the fread and re-taken. A block
 * read while a start or stop moved st.gen is discarded. A short read or
 * the last frame sets st.eof.
 * @param f The worker's stream.
 * @param left Frames of @p f not yet read; decremented by the frames kept.
 * @param gen The generation @p f belongs to.
 * @param buf Worker buffer of MO_CHUNK_FRAMES stereo frames. */
static void worker_read_block(FILE* f, uint32_t* left, unsigned gen, int16_t* buf)
{
    uint64_t space = MO_STREAM_FRAMES - (st.wr - st.rd);
    uint32_t n = MO_CHUNK_FRAMES;
    if (space < n) n = (uint32_t)space;
    if (*left < n) n = *left;
    const size_t frame = (size_t)st.channels * sizeof(int16_t);
    SDL_UnlockMutex(st.mx);
    const size_t got = n ? fread(buf, frame, n, f) : 0;
    SDL_LockMutex(st.mx);
    if (gen != st.gen) return;
    ring_put(buf, (uint32_t)got);
    st.bytes_read += got * frame;
    *left -= (uint32_t)got;
    if (got < n || *left == 0) st.eof = 1;
}

/** @brief Streaming worker: adopt each handed-over stream and keep the ring filled from it.
 *
 * Waits on st.cv while it has no stream, the stream is fully read, or the
 * ring is full; music_override_pump signals it after consuming. Exits on
 * st.quit, closing the stream it holds.
 * @param ud Unused.
 * @return 0. */
static int SDLCALL mo_worker(void* ud)
{
    (void)ud;
    static int16_t buf[MO_CHUNK_FRAMES * 2];
    FILE*    f = NULL;
    uint32_t left = 0;
    unsigned gen = 0;
    SDL_LockMutex(st.mx);
    while (!st.quit) {
        if (st.have_next) {
            if (f) fclose(f);
            f = st.next; left = st.next_left; gen = st.gen;
            st.next = NULL; st.have_next = 0;
        }
        if (f && st.eof) { fclose(f); f = NULL; }
        if (!f || st.wr - st.rd >= MO_STREAM_FRAMES) {
            SDL_WaitCondition(st.cv, st.mx);
            continue;
        }
        worker_read_block(f, &left, gen, buf);
    }
    SDL_UnlockMutex(st.mx);
    if (f) fclose(f);
    return 0;
}

/** @brief Create the worker thread, its lock and its ring on first use.
 * @return 1 when the worker runs, 0 when a resource could not be created (logged). */
static int ensure_worker(void)
{
    if (st.th) return 1;
    if (!st.ring) st.ring = (int16_t*)malloc((size_t)MO_STREAM_FRAMES * 2 * sizeof(int16_t));
    if (!st.mx) st.mx = SDL_CreateMutex();
    if (!st.cv) st.cv = SDL_CreateCondition();
    if (st.ring && st.mx && st.cv) {
        st.quit = 0;
        st.th = SDL_CreateThread(mo_worker, "music-stream", NULL);
    }
    if (!st.th)
        OPL_WARN_F("music", "wav override: stream worker unavailable (%s)", SDL_GetError());
    return st.th != NULL;
}

/** @brief Replace the stream the worker serves: none when f is NULL, else f with its first chunk primed.
 *
 * A stream handed over earlier and not yet adopted is closed here. The
 * ring restarts empty, then takes the primed frames, which count in
 * st.bytes_read here under st.mx, since the worker adds its reads to the
 * same counter.
 * @param f Open stream positioned after the primed frames, or NULL.
 * @param ch Channels of @p f.
 * @param primed Frames already read from @p f into s_chunk.
 * @param left Frames of @p f after the primed ones. */
static void stream_hand_over(FILE* f, int ch, uint32_t primed, uint32_t left)
{
    SDL_LockMutex(st.mx);
    if (st.have_next && st.next) fclose(st.next);
    st.gen++;
    st.next = f; st.have_next = 1; st.next_left = left;
    st.rd = st.wr = 0;
    st.eof = (f == NULL || left == 0);
    if (f) {
        st.channels = ch;
        ring_put(s_chunk, primed);
        st.bytes_read += (uint64_t)primed * (uint64_t)ch * sizeof(int16_t);
    }
    SDL_SignalCondition(st.cv);
    SDL_UnlockMutex(st.mx);
}

/** @brief Open a WAV for streaming, prime its first AL chunk here and hand the rest to the worker.
 *
 * Only the RIFF header and the first MO_CHUNK_FRAMES frames are read on
 * the calling thread.
 * @param path WAV file path.
 * @return 0 with the stream installed and s.channels / s.sample_rate set;
 *         -1 when the file cannot be opened, its header is rejected
 *         (logged), or the worker is unavailable. */
static int stream_open(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    op_wav_info info;
    if (op_wav_read_info(f, path, &info) != 0 || !ensure_worker()) {
        fclose(f);
        return -1;
    }
    uint32_t n = info.frame_count < MO_CHUNK_FRAMES ? info.frame_count : MO_CHUNK_FRAMES;
    const size_t got = fread(s_chunk, (size_t)info.channels * sizeof(int16_t), n, f);
    const uint32_t left = got < n ? 0u : info.frame_count - (uint32_t)got;
    s.channels = info.channels;
    s.sample_rate = info.sample_rate;
    st.starts++;
    stream_hand_over(f, info.channels, (uint32_t)got, left);
    OPL_INFO_F("music", "wav override: streaming %s (%u frames, %d ch, %d Hz)",
               path, info.frame_count, info.channels, info.sample_rate);
    return 0;
}

/** @brief Record how long a start ran on the calling thread for the census.
 * @param t0 SDL_GetTicksNS() at the start's entry.
 * @return The start's duration in microseconds. */
static double note_start_time(Uint64 t0)
{
    const Uint64 d = SDL_GetTicksNS() - t0;
    if (d > st.start_max_ns) st.start_max_ns = d;
    return (double)d / 1e3;
}

/** @brief Create the AL source and the four-buffer ring on first use.
 *
 * The source is listener-relative at unit pitch, every buffer starts
 * free, and the current gain is applied.
 * @return 1 when the ring exists, 0 (logged) when no AL context is
 *         current, which sends the caller to the XA path. */
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
    if (st.th) stream_hand_over(NULL, 0, 0, 0);
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
    if (!s.dir[MO_DIR_SHIPPED][0] && !s.dir[MO_DIR_USER][0]) return 0;
    if (track_index < 0 || track_index >= MO_MAX_TRACKS) return 0;

    const Uint64 t0 = SDL_GetTicksNS();
    char path[sizeof(s.dir[0]) + 1040];
    if (!resolve_track(track_index, path, sizeof path)) return 0;
    /* Logged before the AL check: the selection does not depend on the
     * audio device, and a run without one still names the file it chose. */
    OPL_INFO_F("music", "wav override selected: track %02d -> %s", track_index, path);
    if (!ensure_al()) return 0;

    music_override_stop();
    if (stream_open(path) != 0) {
        s.cache_state[track_index] = MO_FAILED;  /* stream_open logged */
        return 0;
    }
    s.track  = track_index;
    s.active = 1;
    apply_gain();
    music_override_pump();  /* queue the primed chunk so playback starts now */
    const double us = note_start_time(t0);
    OPL_INFO_F("music", "wav override active: track %02d (LBA %d, start %.0f us)",
               track_index, start_sector, us);
    return 1;
}

/* Explicit-path start: a path whose header was rejected is latched failed
 * until a different path is started. s.track = -1 marks path-owned
 * playback (no table slot to invalidate). */
static struct {
    char path[1088];
    int  failed;
} s_path;

int music_override_try_start_path(const char* path, int start_sector)
{
    if (!path || !path[0]) return 0;
    music_override_init();
    if (strcmp(s_path.path, path) != 0) {
        s_path.failed = 0;
        snprintf(s_path.path, sizeof(s_path.path), "%s", path);
    }
    if (s_path.failed) return 0;
    OPL_INFO_F("music", "wav override selected: %s", path);
    if (!ensure_al()) return 0;

    const Uint64 t0 = SDL_GetTicksNS();
    music_override_stop();
    if (stream_open(path) != 0) {
        s_path.failed = 1;
        return 0;
    }
    s.track  = -1;
    s.active = 1;
    apply_gain();
    music_override_pump();
    const double us = note_start_time(t0);
    OPL_INFO_F("music", "wav override active: %s (LBA %d, start %.0f us)", path,
               start_sector, us);
    return 1;
}

int music_override_is_active(void) { return s.active; }

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

    while (s.n_free > 0) {
        SDL_LockMutex(st.mx);
        uint64_t avail = st.wr - st.rd;
        uint32_t n = avail < MO_CHUNK_FRAMES ? (uint32_t)avail : MO_CHUNK_FRAMES;
        ring_get(s_chunk, n);
        SDL_SignalCondition(st.cv);
        SDL_UnlockMutex(st.mx);
        if (n == 0) break;
        ALuint b = s.free_bufs[--s.n_free];
        alBufferData(b,
                     s.channels == 2 ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16,
                     s_chunk,
                     (ALsizei)(n * (uint32_t)s.channels * sizeof(int16_t)),
                     s.sample_rate);
        alSourceQueueBuffers(s.source, 1, &b);
    }

    /* WAV exhausted -> ring drains and the source stops. Looping is the
     * game's job: it polls CdlGetlocP and re-issues CdlReadS when the
     * head passes the track's end, which restarts us from frame 0. The
     * head is driven by the game layer (psyq_CdSync), not from here, so
     * a WAV shorter than the original XA track trails silence and a
     * longer one is cut off at the original's length. */
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
        s.cache_state[i] = MO_UNLOADED;
    }
    s_path.failed  = 0;
    s_path.path[0] = '\0';
    if (st.th) {
        SDL_LockMutex(st.mx);
        st.quit = 1;
        if (st.have_next && st.next) fclose(st.next);
        st.next = NULL; st.have_next = 0;
        SDL_SignalCondition(st.cv);
        SDL_UnlockMutex(st.mx);
        SDL_WaitThread(st.th, NULL);
        st.th = NULL;
    }
    if (st.starts)
        OPL_INFO_F("music", "stream census: starts=%u bytes_read=%llu "
                   "resident_bytes=%u start_max_us=%.1f",
                   st.starts, (unsigned long long)st.bytes_read,
                   (unsigned)(MO_STREAM_FRAMES * 2 * sizeof(int16_t)),
                   (double)st.start_max_ns / 1e3);
    if (st.cv) { SDL_DestroyCondition(st.cv); st.cv = NULL; }
    if (st.mx) { SDL_DestroyMutex(st.mx); st.mx = NULL; }
    free(st.ring);
    st.ring = NULL;
    st.starts = 0; st.bytes_read = 0; st.start_max_ns = 0;
    s.discovered = 0;
    for (int d = 0; d < MO_DIR_MAX; d++) s.dir[d][0] = '\0';
}
