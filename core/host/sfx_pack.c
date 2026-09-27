#include "sfx_pack.h"
#include "sfx_ident.h"

#include <SDL3/SDL.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../../third_party/PsyCross/src/audio/PsyX_SPUAL.h"
#include "openpete_fiber.h"  /* openpete_speculative */
#include "openpete_log.h"
#include "openpete_mods.h"
#include "openpete_paths.h"
#include "mod_vec.h"

/* Sources and entries the tables allocate up front; both grow past it. */
#define SP_SRC_INIT   17
#define SP_ENTRY_INIT 4096
#define SP_CACHE_MAX 1024
#define SP_SPU_SIZE  (512u * 1024u)

typedef struct {
    char dir[512];
    char label[80];
    char owner[64];     /* "" = base/user source, always serves */
} sp_source;

typedef struct {
    op_sfx_name id;
    int      src;        /* source index; -1 = registered from code (sfx_replace) */
    char     owner[64];  /* code entries: the registering mod ("" = engine) */
    int      raw;        /* code entries: the sample at `rate` under the game's
                          * pitch as written — no base-pitch fold */
    char     path[640];
    long     mtime;
    long     size;
    /* decoded by the decode worker, or by the callback when it reaches an
     * entry first; guarded by s_mx */
    int      decoded;    /* 1 = pcm valid, -1 = failed */
    int      claimed;    /* a thread is decoding this entry's file */
    int16_t* pcm;
    int      count;
    uint32_t rate;
    int      loop_start, loop_len;
} sp_entry;

typedef struct {
    int      valid;
    uint32_t addr;
    uint64_t hash;
    int      len;        /* ADPCM bytes */
} sp_ident;

static mod_fvec  s_srcs;      /* sp_source, index-stable within a rescan */
static mod_fvec  s_entries;   /* sp_entry */

static sp_source* src_at(int i) {
    return (sp_source*)mod_fvec_at(&s_srcs, (size_t)i);
}
static sp_entry* entry_at(int i) {
    return (sp_entry*)mod_fvec_at(&s_entries, (size_t)i);
}
static int src_count(void)   { return (int)mod_fvec_count(&s_srcs); }
static int entry_count(void) { return (int)mod_fvec_count(&s_entries); }

/** @brief Append a zeroed source slot, creating the table on first use; return it, or NULL when the allocation fails (logged). */
static sp_source* src_push(void) {
    if (!s_srcs.elem_size)
        mod_fvec_init(&s_srcs, sizeof(sp_source), SP_SRC_INIT, "sfx_src",
                      SP_SRC_INIT);
    sp_source* s = (sp_source*)mod_fvec_push(&s_srcs);
    if (!s) OPL_ERROR_F("sfxpack", "source table allocation failed");
    return s;
}

/** @brief Append a zeroed entry slot, creating the table on first use; return it, or NULL when the allocation fails (logged). */
static sp_entry* entry_push(void) {
    if (!s_entries.elem_size)
        mod_fvec_init(&s_entries, sizeof(sp_entry), SP_ENTRY_INIT,
                      "sfx_entry", SP_ENTRY_INIT);
    sp_entry* e = (sp_entry*)mod_fvec_push(&s_entries);
    if (!e) OPL_ERROR_F("sfxpack", "entry table allocation failed");
    return e;
}
static sp_ident  s_cache[SP_CACHE_MAX];
static unsigned  s_gen;
static char      s_dir_override[512];
static int       s_seen_roots;
static int       s_verify = -1;
static int       s_log = -1;     /* OPENPETE_SFXPACK_LOG: name the misses */
static sfx_pack_pitch_reader_fn  s_pitch_reader;
static sfx_pack_name_resolver_fn s_name_resolver;
static sfx_pack_census_t s_census;

/* Decode worker. Decodes every file entry after each scan so the override
 * callback finds its PCM resident. s_mx guards the entry table and every
 * entry's decode fields; only the thread that scans (the sim thread, which
 * also runs the callback) changes the table's shape, so an entry pointer
 * that thread holds stays valid across its own unlocks. */
static SDL_Mutex*     s_mx;
static SDL_Condition* s_cv;          /* worker work, and decode completion */
static SDL_Thread*    s_worker;
/* Callback wall time per committed key-on, 100 us buckets to 50 ms. */
#define SP_CB_BUCKETS 501
static uint32_t s_cb_hist[SP_CB_BUCKETS];

/* ── WAV in: whatever SDL reads, to S16 mono at the file's rate ───── */

/* Loop sample indices at or above this are refused (2^30 samples). */
#define WAV_LOOP_SAMPLE_MAX 0x40000000u

/** @brief Read a little-endian 32-bit word. */
static uint32_t wav_le32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/** @brief The size of an open file in bytes, or 0 when it cannot be measured. */
static uint64_t wav_file_size(FILE* f) {
    if (fseek(f, 0, SEEK_END) != 0) return 0;
    const long e = ftell(f);
    return e > 0 ? (uint64_t)e : 0u;
}

/** @brief Parse the first loop of a 60-byte `smpl` chunk body into start and len.
 *
 * The loop count sits at offset 28 and the first loop's start and end
 * sample at 44 and 48, the end inclusive. The end bound keeps start + len
 * inside int for the consumer.
 * @return 1 when the count is nonzero and start <= end < WAV_LOOP_SAMPLE_MAX. */
static int wav_smpl_first_loop(const uint8_t* b, int* start, int* len) {
    const uint32_t s0 = wav_le32(b + 44), e0 = wav_le32(b + 48);
    if (wav_le32(b + 28) < 1u || e0 < s0 || e0 >= WAV_LOOP_SAMPLE_MAX) return 0;
    *start = (int)s0;
    *len = (int)(e0 - s0 + 1);
    return 1;
}

/** @brief Read the first loop of a WAV file's `smpl` chunk into start and len; return 1 when one is found.
 *
 * Walks the RIFF/WAVE chunks, each size padded to even, to the first
 * `smpl` chunk of at least 60 bytes. The loop count sits at offset 28 and
 * the first loop's start and end sample at offsets 44 and 48; the end is
 * inclusive, so len = end - start + 1. A missing file, a non-RIFF/WAVE
 * header, no `smpl` chunk, a zero loop count or an end before the start
 * returns 0 and leaves start and len unwritten, as does a chunk that runs
 * past the end of the file or a loop end at or above WAV_LOOP_SAMPLE_MAX. */
static int wav_read_smpl(const char* path, int* start, int* len)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t h[12];
    int found = 0;
    const uint64_t fsize = wav_file_size(f);
    uint64_t pos = 12;
    if (fseek(f, 0, SEEK_SET) == 0 &&
        fread(h, 1, 12, f) == 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) {
        uint8_t ch[8];
        while (fread(ch, 1, 8, f) == 8) {
            const uint32_t sz = wav_le32(ch + 4);
            if (!memcmp(ch, "smpl", 4) && sz >= 36 + 24) {
                uint8_t b[36 + 24];
                found = fread(b, 1, sizeof b, f) == sizeof b && wav_smpl_first_loop(b, start, len);
                break;
            }
            /* Chunk offsets are tracked in 64 bits against the file's size and
             * every seek is absolute: a chunk size near 2^32 ends the walk
             * instead of wrapping to a backward seek (a 32-bit long on LLP64)
             * that re-reads an earlier header forever. */
            pos += 8u + (((uint64_t)sz + 1u) & ~(uint64_t)1u);
            if (pos > fsize || pos > (uint64_t)LONG_MAX || fseek(f, (long)pos, SEEK_SET) != 0) break;
        }
    }
    fclose(f);
    return found;
}

/* One decoded file: S16 mono PCM at the file's rate, and its first loop. */
typedef struct {
    int16_t* pcm;
    int      count;
    uint32_t rate;
    int      loop_start, loop_len;
} sp_pcm;

/** @brief Decode a WAV file to S16 mono PCM at its own rate and read its first smpl loop.
 *
 * Touches no shared state beyond the log, so any thread may call it.
 * @param path WAV file path.
 * @param out Receives the PCM (SDL-allocated) and loop on success.
 * @return 1 on success; 0 when SDL cannot read or convert the file (logged). */
static int decode_file(const char* path, sp_pcm* out)
{
    SDL_AudioSpec spec;
    Uint8* buf = NULL;
    Uint32 len = 0;
    if (!SDL_LoadWAV(path, &spec, &buf, &len)) {
        OPL_WARN_F("sfxpack", "%s: %s", path, SDL_GetError());
        return 0;
    }
    SDL_AudioSpec want = { SDL_AUDIO_S16LE, 1, spec.freq };
    Uint8* conv = NULL;
    int outlen = 0;
    int ok = SDL_ConvertAudioSamples(&spec, buf, (int)len, &want, &conv, &outlen);
    SDL_free(buf);
    if (!ok || !conv) {
        OPL_WARN_F("sfxpack", "%s: cannot convert (%s)", path, SDL_GetError());
        return 0;
    }
    if (spec.channels != 1)
        OPL_WARN_F("sfxpack", "%s: %d channels downmixed to mono (the SPU "
                   "voice is mono; pan is the game's)", path, spec.channels);
    out->pcm   = (int16_t*)conv;
    out->count = outlen / 2;
    out->rate  = (uint32_t)spec.freq;
    if (!wav_read_smpl(path, &out->loop_start, &out->loop_len)) {
        out->loop_start = 0; out->loop_len = 0;
    }
    return 1;
}

/** @brief Store a decode result in an entry and clear its claim.
 *
 * Caller holds s_mx.
 * @param e The entry.
 * @param ok The decode_file result.
 * @param r The decoded PCM when @p ok. */
static void entry_store(sp_entry* e, int ok, const sp_pcm* r)
{
    e->claimed = 0;
    if (!ok) { e->decoded = -1; return; }
    e->pcm = r->pcm; e->count = r->count; e->rate = r->rate;
    e->loop_start = r->loop_start; e->loop_len = r->loop_len;
    e->decoded = 1;
}

/** @brief Return 1 when an entry's PCM is resident, decoding it on this thread when no worker has claimed it.
 *
 * Caller holds s_mx. An entry the worker is decoding is waited for rather than decoded twice.
 * An unclaimed entry is claimed and decoded here with s_mx released, which
 * counts in sync_decodes on a committed tick. Must run on the thread that
 * scans, so @p e cannot move while s_mx is released.
 * @param e The entry.
 * @return 1 when the PCM is valid, 0 when the file failed to decode. */
static int entry_decode(sp_entry* e)
{
    while (e->claimed) SDL_WaitCondition(s_cv, s_mx);
    if (e->decoded) return e->decoded > 0;
    if (!openpete_speculative) s_census.sync_decodes++;
    char path[sizeof e->path];
    memcpy(path, e->path, sizeof path);
    e->claimed = 1;
    SDL_UnlockMutex(s_mx);
    sp_pcm r;
    const int ok = decode_file(path, &r);
    SDL_LockMutex(s_mx);
    entry_store(e, ok, &r);
    SDL_BroadcastCondition(s_cv);
    return ok;
}

/** @brief Claim the first file entry with no PCM and no claim.
 *
 * Caller holds s_mx.
 * @param path Receives the entry's path.
 * @param mtime Receives the entry's mtime.
 * @param size Receives the entry's size.
 * @return 1 when an entry was claimed, else 0. */
static int worker_claim(char* path, long* mtime, long* size)
{
    for (int i = 0; i < entry_count(); i++) {
        sp_entry* e = entry_at(i);
        if (e->src < 0 || e->decoded || e->claimed) continue;
        e->claimed = 1;
        memcpy(path, e->path, sizeof e->path);
        *mtime = e->mtime; *size = e->size;
        return 1;
    }
    return 0;
}

/** @brief Hand a worker decode back to the entry it was claimed from, or free it.
 *
 * Caller holds s_mx. The entry is found again by path, since a scan may have moved it. The
 * PCM is kept only when that entry still describes the same file (mtime and
 * size) and has none yet; the entry's claim is cleared either way.
 * @param path Path the decode read.
 * @param mtime The entry's mtime at claim.
 * @param size The entry's size at claim.
 * @param ok The decode_file result.
 * @param r The decoded PCM when @p ok. */
static void worker_publish(const char* path, long mtime, long size, int ok, sp_pcm* r)
{
    for (int i = 0; i < entry_count(); i++) {
        sp_entry* e = entry_at(i);
        if (e->src < 0 || !e->claimed || strcmp(e->path, path) != 0) continue;
        if (e->mtime == mtime && e->size == size && !e->decoded) {
            entry_store(e, ok, r);
            return;
        }
        e->claimed = 0;
        break;
    }
    if (ok) SDL_free(r->pcm);
}

/** @brief Decode worker: decode every file entry without PCM, then wait for the next scan.
 * @param ud Unused.
 * @return 0 (never returns while the process runs). */
static int SDLCALL sfx_decode_worker(void* ud)
{
    (void)ud;
    char path[sizeof(((sp_entry*)0)->path)];
    long mtime = 0, size = 0;
    SDL_LockMutex(s_mx);
    for (;;) {
        if (!worker_claim(path, &mtime, &size)) {
            SDL_WaitCondition(s_cv, s_mx);
            continue;
        }
        SDL_UnlockMutex(s_mx);
        sp_pcm r;
        const int ok = decode_file(path, &r);
        SDL_LockMutex(s_mx);
        worker_publish(path, mtime, size, ok, &r);
        SDL_BroadcastCondition(s_cv);
    }
    return 0;
}

/** @brief Create the table lock on first use.
 * @return 1 when s_mx and s_cv exist. */
static int ensure_lock(void)
{
    if (!s_mx) s_mx = SDL_CreateMutex();
    if (!s_cv) s_cv = SDL_CreateCondition();
    return s_mx && s_cv;
}

/** @brief Start the decode worker when file entries exist, and wake it to decode the current table.
 *
 * Caller holds s_mx. Without a worker (creation failed) the callback
 * decodes each entry on first use. */
static void worker_kick(void)
{
    if (!s_worker && entry_count() > 0) {
        s_worker = SDL_CreateThread(sfx_decode_worker, "sfx-decode", NULL);
        if (s_worker) SDL_DetachThread(s_worker);
        else OPL_WARN_F("sfxpack", "decode worker unavailable (%s); decoding on first use",
                        SDL_GetError());
    }
    SDL_BroadcastCondition(s_cv);
}

static void entry_release(sp_entry* e)
{
    if (e->pcm) SDL_free(e->pcm);
    e->pcm = NULL; e->count = 0; e->decoded = 0;
}

/* ── sources + scan ──────────────────────────────────────────────── */

/** @brief Insert or refresh the entry for path in source si, and return it or NULL when the table cannot grow.
 *
 * An entry whose size and mtime match keeps its decoded PCM; a changed
 * file's PCM is released and decoded again on its next match. The parsed
 * identity, size and mtime are rewritten either way. */
static sp_entry* entry_upsert(int si, const char* path, op_sfx_name id,
                              const struct stat* st)
{
    sp_entry* e = NULL;
    for (int i = 0; i < entry_count(); i++) {
        sp_entry* o = entry_at(i);
        if (o->src == si && strcmp(o->path, path) == 0) { e = o; break; }
    }
    if (!e) {
        e = entry_push();
        if (!e) return NULL;
        e->src = si;
        snprintf(e->path, sizeof e->path, "%s", path);
    } else if (e->mtime != (long)st->st_mtime ||
               e->size != (long)st->st_size) {
        entry_release(e);
    }
    e->id    = id;
    e->mtime = (long)st->st_mtime;
    e->size  = (long)st->st_size;
    return e;
}

/** @brief Log the first entry of an earlier source that entry e of source si overrides, if any.
 *
 * Entries match on kind and on hash and pitch for the hash form, or on
 * name for the symbolic form. Both entries stay in the table: the later
 * source wins the match, and disabling its mod falls back to the earlier
 * one. */
static void note_override(int si, const sp_entry* e, const char* fname)
{
    for (int i = 0; i < entry_count(); i++) {
        const sp_entry* o = entry_at(i);
        int same;
        if (o == e || o->src < 0 || o->src >= si) continue;
        same = o->id.kind == e->id.kind &&
               (e->id.kind == OP_SFX_NAME_HASH
                    ? (o->id.hash == e->id.hash && o->id.pitch == e->id.pitch)
                    : strcmp(o->id.name, e->id.name) == 0);
        if (same) {
            OPL_INFO_F("sfxpack", "%s overrides \"%s\" (was %s)",
                       src_at(si)->label, fname, src_at(o->src)->label);
            break;
        }
    }
}

static void scan_dir(int si)
{
    DIR* d = opendir(src_at(si)->dir);
    if (!d) return;
    struct dirent* de;
    int added = 0;
    while ((de = readdir(d)) != NULL) {
        op_sfx_name id;
        char path[640];
        struct stat st;
        sp_entry* e;
        if (op_sfx_name_parse(de->d_name, &id) == OP_SFX_NAME_NONE) continue;
        snprintf(path, sizeof path, "%s/%s", src_at(si)->dir, de->d_name);
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        e = entry_upsert(si, path, id, &st);
        if (!e) break;
        added++;
        note_override(si, e, de->d_name);
    }
    closedir(d);
    if (added)
        OPL_INFO_F("sfxpack", "%s: %d file%s (%s)", src_at(si)->label, added,
                   added == 1 ? "" : "s", src_at(si)->dir);
}

static void add_root_source(int r)
{
    const char* owner = NULL;
    const char* root = openpete_mods_asset_root(r, &owner);
    if (!root) return;
    sp_source* src = src_push();
    if (!src) return;
    int si = src_count() - 1;
    snprintf(src->dir, sizeof src->dir, "%s/sfx", root);
    snprintf(src->label, sizeof src->label, "mod %s", owner ? owner : "?");
    snprintf(src->owner, sizeof src->owner, "%s", owner ? owner : "");
    scan_dir(si);
}

static void rescan(void)
{
    /* Entries whose file vanished are dropped; the rest keep their PCM.
     * Code-registered entries have no file and live until re-registered. */
    int keep = 0;
    for (int i = 0; i < entry_count(); i++) {
        struct stat st;
        sp_entry* e = entry_at(i);
        if (e->src < 0 || stat(e->path, &st) == 0) *entry_at(keep++) = *e;
        else entry_release(e);
    }
    mod_fvec_truncate(&s_entries, (size_t)keep);

    /* Sources are rebuilt in the same order every rescan, so the `src`
     * index a surviving entry carries still names its source. */
    mod_fvec_clear(&s_srcs);
    const char* env = getenv("OPENPETE_SFXPACK_DIR");
    char base[512];
    if (s_dir_override[0]) snprintf(base, sizeof base, "%s", s_dir_override);
    else if (env && *env) snprintf(base, sizeof base, "%s", env);
    else {
        const char* bp = SDL_GetBasePath();
        snprintf(base, sizeof base, "%sassets/sfx", bp ? bp : "./");
    }
    sp_source* base_src = src_push();
    if (!base_src) return;
    snprintf(base_src->dir, sizeof base_src->dir, "%s", base);
    snprintf(base_src->label, sizeof base_src->label, "base pack");
    scan_dir(0);
    if (!s_dir_override[0] && !(env && *env)) {
        char user[512];
        if (openpete_paths_data_dir("assets", user, sizeof user, 0) == 0) {
            sp_source* us = src_push();
            if (us) {
                snprintf(us->dir, sizeof us->dir, "%s/sfx", user);
                snprintf(us->label, sizeof us->label, "user pack");
                scan_dir(src_count() - 1);
            }
        }
    }
    int n_roots = openpete_mods_asset_root_count();
    for (int r = 0; r < n_roots; r++) add_root_source(r);
    s_seen_roots = n_roots;
    s_census.active = entry_count() > 0;
    s_census.files  = entry_count();
}

/* ── identity cache ──────────────────────────────────────────────── */

/** @brief Return the identity of the sample at SPU address addr, hashing it from SPU RAM on a cache miss, or NULL.
 *
 * The sample runs from addr through its first END-flagged 16-byte block,
 * as measured by PsyX_SPUAL_SampleByteLength. NULL when addr is outside
 * SPU RAM or no END flag is found. A miss takes the first invalid cache
 * slot, else evicts slot addr % SP_CACHE_MAX. */
static const sp_ident* ident_lookup(uint32_t addr)
{
    for (int i = 0; i < SP_CACHE_MAX; i++)
        if (s_cache[i].valid && s_cache[i].addr == addr) return &s_cache[i];
    unsigned int size = 0;
    const unsigned char* mem = PsyX_SPUAL_MemPtr(&size);
    if (!mem || addr >= size) return NULL;
    int len = PsyX_SPUAL_SampleByteLength(mem + addr, (int)(size - addr));
    if (len < 16 || !(mem[addr + len - 15] & 0x01)) return NULL; /* no END */
    sp_ident* slot = NULL;
    for (int i = 0; i < SP_CACHE_MAX; i++)
        if (!s_cache[i].valid) { slot = &s_cache[i]; break; }
    if (!slot) slot = &s_cache[addr % SP_CACHE_MAX];
    slot->valid = 1;
    slot->addr  = addr;
    slot->len   = len;
    slot->hash  = op_sfx_hash(mem + addr, (size_t)len);
    return slot;
}

/* ── match ───────────────────────────────────────────────────────── */

/** @brief Return 1 when entry e may serve: its owner (the registering mod, or its source's mod) is empty or enabled. */
static int entry_enabled(const sp_entry* e)
{
    const char* owner = e->src < 0 ? e->owner : src_at(e->src)->owner;
    return !owner[0] || openpete_mods_is_enabled(owner);
}

/** @brief Return the enabled entry that best matches the sample identity id keyed at addr, or NULL.
 *
 * Tiers, highest first: a tagged hash file for exactly this def's base
 * pitch (4); the untagged hash form (3); a tagged hash file for another
 * def sharing the sample (2), which still plays at the right pitch through
 * the callback's rate folding; a symbolic entry whose name is one of the
 * SoundTable slot names the name resolver returns for addr (1). Among
 * equal tiers the later entry wins, except that a code registration is
 * not displaced by a file. id may be NULL, which leaves only the symbolic
 * tier. */
static sp_entry* match(const sp_ident* id, uint32_t addr, int base_pitch)
{
    sp_entry* best = NULL;
    int best_tier = 0;
    const char* names[8];
    int n_names = s_name_resolver ? s_name_resolver(addr, names, 8) : 0;
    for (int i = 0; i < entry_count(); i++) {
        sp_entry* e = entry_at(i);
        int tier = 0;
        if (e->id.kind == OP_SFX_NAME_HASH && id && e->id.hash == id->hash)
            tier = e->id.pitch < 0 ? 3 : (base_pitch > 0 && e->id.pitch == base_pitch ? 4 : 2);
        else if (e->id.kind == OP_SFX_NAME_SYMBOLIC)
            for (int k = 0; k < n_names; k++)
                if (strcmp(e->id.name, names[k]) == 0) { tier = 1; break; }
        if (!tier || !entry_enabled(e)) continue;
        /* later entry (later source) wins ties; code beats file at tier 1 */
        if (tier > best_tier ||
            (tier == best_tier && (e->src < 0 || best->src >= 0))) {
            best = e; best_tier = tier;
        }
    }
    return best;
}

/* ── the PsyCross callback ───────────────────────────────────────── */

/* One-shot substitution (sfx_pack_arm_oneshot). Armed and spent on the
 * sim thread, the same thread that keys voices and runs the callback. */
static const int16_t* s_oneshot_pcm;
static int            s_oneshot_count;
static unsigned int   s_oneshot_rate;

static void override_done(PsyX_SPUAL_SampleOverride* ov)
{
    if (ov->rings_out) s_census.rings_out++;
}

/** @brief Answer a voice key-on with the pack sample or one-shot PCM that replaces it, or decline.
 *
 * A one-shot arm outranks the pack. Otherwise the sample's identity is
 * looked up (hashed from SPU RAM on a miss), matched against the enabled
 * entries, and the entry's PCM is served with its rate folded for the
 * file's pitch tag, decoding it first when it is not resident. Caller
 * holds s_mx (override_cb_timed). With OPENPETE_SFXPACK_VERIFY a hash-form
 * match is compared against the stock sample decoded from SPU RAM.
 * @param voice SPU voice index.
 * @param addr SPU address of the keyed sample.
 * @param stock_samples Decoded length of the stock sample.
 * @param stock_loops Nonzero when the stock sample loops (unused).
 * @param out Receives the replacement.
 * @return 1 when @p out holds a replacement, else 0. */
static int override_cb(int voice, unsigned int addr, int stock_samples,
                       int stock_loops, PsyX_SPUAL_SampleOverride* out)
{
    (void)stock_loops;
    if (!openpete_speculative) s_census.keyed++;
    /* A one-shot arm outranks the pack, and is checked before the
     * "no files" early-out so it works with no pack installed. */
    if (s_oneshot_pcm) {
        out->pcm        = s_oneshot_pcm;
        out->count      = s_oneshot_count;
        out->rate       = s_oneshot_rate;
        out->loop_start = 0;
        out->loop_len   = 0;
        out->flat       = 1; /* the caller's rate is the sound; ignore pitch */
        out->done       = NULL;
        if (!openpete_speculative) {
            s_oneshot_pcm = NULL;   /* spent */
            s_census.replaced++;
        }
        return 1;
    }
    if (s_log < 0) {
        const char* v = getenv("OPENPETE_SFXPACK_LOG");
        s_log = (v && *v && *v != '0') ? 1 : 0;
    }
    if (entry_count() == 0) return 0;
    /* Late asset roots (a mod enabled mid-session) become sources here,
     * the way the texture pack does it. */
    if (openpete_mods_asset_root_count() > s_seen_roots) { rescan(); worker_kick(); }
    const sp_ident* id = ident_lookup(addr);
    if (!openpete_speculative && id) s_census.resolved++;
    int base_pitch = s_pitch_reader ? s_pitch_reader(voice) : 0;
    sp_entry* e = match(id, addr, base_pitch);
    if (!e || !entry_decode(e)) {
        if (id && !openpete_speculative && s_log > 0) {
            static uint64_t s_logged[64]; static int s_nlogged;
            int seen = 0;
            for (int i = 0; i < s_nlogged; i++) if (s_logged[i] == id->hash) seen = 1;
            if (!seen && s_nlogged < 64) {
                s_logged[s_nlogged++] = id->hash;
                OPL_INFO_F("sfxpack", "no file for %016llx (addr %#x, base pitch "
                           "%#x, %d bytes)", (unsigned long long)id->hash, addr,
                           base_pitch, id->len);
            }
        }
        return 0;
    }

    uint32_t rate = e->rate;
    int fold = 0;
    if (e->id.flat) fold = 0;                 /* pinned: the file's own rate, always */
    else if (e->id.kind == OP_SFX_NAME_HASH && e->id.pitch > 0) fold = e->id.pitch;
    else if (e->id.kind == OP_SFX_NAME_SYMBOLIC && !e->raw && base_pitch > 0) fold = base_pitch;
    if (fold > 0) {
        /* Tagged: "at its own rate this file is the sound at base pitch
         * P" (the FILE's tag). Fold P into the buffer rate; the game's
         * pitch write (p / 0x1000) then lands at rate * p / P, which is
         * the file's own rate at that base pitch and the stock
         * transposition for any other def sharing the sample. Round to
         * nearest, the inverse of the dump's rounding. */
        /* A symbolic file has no tag: it "sounds as-is at THIS def's
         * base pitch" (the natural DAW export), so the def's own pitch
         * folds instead. An untagged hash file is the raw sample at the
         * decoder rate and folds nothing. */
        rate = (uint32_t)(((uint64_t)e->rate * 0x1000u + (uint32_t)fold / 2) /
                          (uint32_t)fold);
    }
    out->pcm        = e->pcm;
    out->count      = e->count;
    out->rate       = rate;
    out->loop_start = e->loop_start;
    out->loop_len   = e->loop_len;
    out->flat       = e->id.flat;
    out->done       = override_done;
    if (openpete_speculative) return 1;
    s_census.replaced++;

    if (s_verify < 0) {
        const char* v = getenv("OPENPETE_SFXPACK_VERIFY");
        s_verify = (v && *v && *v != '0') ? 1 : 0;
    }
    if (s_verify && id && e->id.kind == OP_SFX_NAME_HASH) {
        /* Decode the stock sample from LIVE SPU RAM and compare. Only a
         * hash-form file claims to BE this sample; a symbolic file
         * replaces a role and may legitimately differ. */
        unsigned int size = 0;
        const unsigned char* mem = PsyX_SPUAL_MemPtr(&size);
        int cap = (id->len / 16) * 28;
        short* tmp = (short*)malloc((size_t)cap * sizeof(short));
        int ls = 0, ll = 0;
        int n = tmp ? PsyX_SPUAL_DecodeSample(mem + addr, id->len, tmp, &ls, &ll) : 0;
        /* The reconstructed rate must be stock (44100) to within what an
         * integer WAV header can carry for this base pitch: one header
         * Hz is 0x1000/pitch SPU Hz, so a 0x1ff sample legitimately
         * lands 2 Hz off. */
        uint32_t tol = fold > 0 ? (0x1000u + (uint32_t)fold - 1u) / (uint32_t)fold : 1u;
        int rate_ok = rate + tol >= 44100u && rate <= 44100u + tol;
        int pcm_ok = tmp && n == e->count && n == stock_samples &&
                     memcmp(tmp, e->pcm, (size_t)n * sizeof(short)) == 0;
        if (pcm_ok && rate_ok) s_census.verify_ok++;
        else {
            if (s_census.verify_bad < 8)
                OPL_WARN_F("sfxpack", "verify: %s: stock %d samples (loop %d+%d) "
                           "vs file %d @ %u Hz — %s", e->path, n, ls, ll, e->count,
                           rate, !pcm_ok ? (tmp && n == e->count ? "PCM differs"
                                                                : "length differs")
                                         : "rate off");
            s_census.verify_bad++;
        }
        free(tmp);
    }
    return 1;
}

/** @brief PsyCross sample-override callback: run override_cb under the table lock and time it for the census.
 *
 * On a committed (non-speculative) tick the call's wall time enters the
 * callback-time histogram and maximum.
 * @param voice SPU voice index.
 * @param addr SPU address of the keyed sample.
 * @param stock_samples Decoded length of the stock sample.
 * @param stock_loops Nonzero when the stock sample loops.
 * @param out Receives the replacement.
 * @return 1 when @p out holds a replacement, else 0. */
static int override_cb_timed(int voice, unsigned int addr, int stock_samples,
                             int stock_loops, PsyX_SPUAL_SampleOverride* out)
{
    const Uint64 t0 = SDL_GetTicksNS();
    if (s_mx) SDL_LockMutex(s_mx);
    const int rc = override_cb(voice, addr, stock_samples, stock_loops, out);
    if (s_mx) SDL_UnlockMutex(s_mx);
    if (!openpete_speculative) {
        const uint64_t us = (SDL_GetTicksNS() - t0) / 1000u;
        uint64_t b = us / 100u;
        if (b >= SP_CB_BUCKETS) b = SP_CB_BUCKETS - 1;
        s_cb_hist[b]++;
        if (us > s_census.cb_max_us) s_census.cb_max_us = us;
    }
    return rc;
}

/* ── public ──────────────────────────────────────────────────────── */

void sfx_pack_arm_oneshot(const int16_t* pcm, int count, unsigned int rate)
{
    if (!pcm || count <= 0 || rate == 0) return;
    s_oneshot_count = count;
    s_oneshot_rate  = rate;
    s_oneshot_pcm   = pcm;   /* published last: the callback reads it first */
}

void sfx_pack_disarm_oneshot(void) { s_oneshot_pcm = NULL; }

int sfx_pack_register_symbolic_adpcm(const char* owner, const char* name,
                                     const void* adpcm, uint32_t len,
                                     int pitch)
{
    if (!name || !*name || strlen(name) >= sizeof(((op_sfx_name*)0)->name) ||
        !adpcm || len < 16 || len > (1u << 20))
        return -1;
    /* One block of 16 bytes decodes to 28 samples; the decoder stops at
     * the END flag, so a body carrying one decodes to at most this. */
    int cap = (int)(len / 16u) * 28;
    int16_t* pcm = (int16_t*)SDL_malloc((size_t)cap * sizeof(int16_t));
    if (!pcm) return -1;
    int ls = 0, ll = 0;
    int n = PsyX_SPUAL_DecodeSample((const unsigned char*)adpcm, (int)len,
                                    pcm, &ls, &ll);
    if (n <= 0) { SDL_free(pcm); return -1; }
    const char* own = owner ? owner : "";
    if (s_mx) SDL_LockMutex(s_mx);
    sp_entry* e = NULL;
    for (int i = 0; i < entry_count(); i++) {
        sp_entry* o = entry_at(i);
        if (o->src < 0 && strcmp(o->owner, own) == 0 &&
            strcmp(o->id.name, name) == 0) { e = o; break; }
    }
    if (!e) {
        e = entry_push();
        if (!e) {
            if (s_mx) SDL_UnlockMutex(s_mx);
            SDL_free(pcm);
            return -1;
        }
        e->src = -1;
        snprintf(e->owner, sizeof e->owner, "%s", own);
    } else {
        entry_release(e);
    }
    e->id.kind = OP_SFX_NAME_SYMBOLIC;
    snprintf(e->id.name, sizeof e->id.name, "%s", name);
    /* pitch 0: the raw sample at the decoder rate, pitched by the game as
     * stock. Otherwise the caller's pitch is the sound: pin the buffer
     * rate to it and ignore the game's pitch writes. */
    e->id.flat    = pitch > 0;
    e->raw        = pitch <= 0;
    e->rate       = pitch > 0 ? op_sfx_rate_for_pitch(pitch) : 44100u;
    e->pcm        = pcm;
    e->count      = n;
    e->loop_start = ls;
    e->loop_len   = ll;
    e->decoded    = 1;
    s_census.active = 1;
    if (s_mx) SDL_UnlockMutex(s_mx);
    return 0;
}

int sfx_pack_oneshot_armed(void) { return s_oneshot_pcm != NULL; }

void sfx_pack_set_dir_override(const char* dir)
{
    snprintf(s_dir_override, sizeof s_dir_override, "%s", dir ? dir : "");
}

void sfx_pack_set_pitch_reader(sfx_pack_pitch_reader_fn fn)   { s_pitch_reader = fn; }
void sfx_pack_set_name_resolver(sfx_pack_name_resolver_fn fn) { s_name_resolver = fn; }

void sfx_pack_init(void)
{
    if (!ensure_lock()) {
        OPL_ERROR_F("sfxpack", "cannot create the table lock (%s); sfx pack off",
                    SDL_GetError());
        return;
    }
    SDL_LockMutex(s_mx);
    rescan();
    worker_kick();
    SDL_UnlockMutex(s_mx);
    PsyX_SPUAL_SetSampleOverride(override_cb_timed);
    if (entry_count())
        OPL_INFO_F("sfxpack", "sfx pack active: %d file%s (%d source%s)",
                   entry_count(), entry_count() == 1 ? "" : "s", src_count(),
                   src_count() == 1 ? "" : "s");
}

void sfx_pack_generation_bump(void)
{
    s_gen++;
    memset(s_cache, 0, sizeof s_cache);
    if (!s_mx) { rescan(); return; }
    SDL_LockMutex(s_mx);
    rescan();
    worker_kick();
    SDL_UnlockMutex(s_mx);
}

void sfx_pack_invalidate_range(uint32_t addr, uint32_t len)
{
    uint64_t end = (uint64_t)addr + len;
    for (int i = 0; i < SP_CACHE_MAX; i++) {
        if (!s_cache[i].valid) continue;
        uint64_t a = s_cache[i].addr, b = a + (uint64_t)s_cache[i].len;
        if (a < end && addr < b) s_cache[i].valid = 0;
    }
}

void sfx_pack_census(sfx_pack_census_t* out)
{
    uint64_t total = 0, run = 0;
    for (int i = 0; i < SP_CB_BUCKETS; i++) total += s_cb_hist[i];
    s_census.cb_p99_us = 0;
    for (int i = 0; i < SP_CB_BUCKETS && total; i++) {
        run += s_cb_hist[i];
        if (run * 100 >= total * 99) { s_census.cb_p99_us = (uint64_t)(i + 1) * 100u; break; }
    }
    *out = s_census;
}
