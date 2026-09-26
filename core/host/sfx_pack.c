#include "sfx_pack.h"
#include "sfx_ident.h"

#include <SDL3/SDL.h>
#include <dirent.h>
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
    /* decoded (lazily, on the sim thread) */
    int      decoded;    /* 1 = pcm valid, -1 = failed */
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

/* ── WAV in: whatever SDL reads, to S16 mono at the file's rate ───── */

/** @brief Read the first loop of a WAV file's `smpl` chunk into start and len; return 1 when one is found.
 *
 * Walks the RIFF/WAVE chunks, each size padded to even, to the first
 * `smpl` chunk of at least 60 bytes. The loop count sits at offset 28 and
 * the first loop's start and end sample at offsets 44 and 48; the end is
 * inclusive, so len = end - start + 1. A missing file, a non-RIFF/WAVE
 * header, no `smpl` chunk, a zero loop count or an end before the start
 * returns 0 and leaves start and len unwritten. */
static int wav_read_smpl(const char* path, int* start, int* len)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t h[12];
    int found = 0;
    if (fread(h, 1, 12, f) == 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) {
        uint8_t ch[8];
        while (fread(ch, 1, 8, f) == 8) {
            uint32_t sz = (uint32_t)ch[4] | (uint32_t)ch[5] << 8 |
                          (uint32_t)ch[6] << 16 | (uint32_t)ch[7] << 24;
            if (!memcmp(ch, "smpl", 4) && sz >= 36 + 24) {
                uint8_t b[36 + 24];
                if (fread(b, 1, sizeof b, f) == sizeof b) {
                    uint32_t nloops = (uint32_t)b[28] | (uint32_t)b[29] << 8 |
                                      (uint32_t)b[30] << 16 | (uint32_t)b[31] << 24;
                    if (nloops >= 1) {
                        uint32_t s0 = (uint32_t)b[44] | (uint32_t)b[45] << 8 |
                                      (uint32_t)b[46] << 16 | (uint32_t)b[47] << 24;
                        uint32_t e0 = (uint32_t)b[48] | (uint32_t)b[49] << 8 |
                                      (uint32_t)b[50] << 16 | (uint32_t)b[51] << 24;
                        if (e0 >= s0) { *start = (int)s0; *len = (int)(e0 - s0 + 1); found = 1; }
                    }
                }
                break;
            }
            if (fseek(f, (long)((sz + 1) & ~1u), SEEK_CUR) != 0) break;
        }
    }
    fclose(f);
    return found;
}

static int entry_decode(sp_entry* e)
{
    if (e->decoded) return e->decoded > 0;
    SDL_AudioSpec spec;
    Uint8* buf = NULL;
    Uint32 len = 0;
    if (!SDL_LoadWAV(e->path, &spec, &buf, &len)) {
        OPL_WARN_F("sfxpack", "%s: %s", e->path, SDL_GetError());
        e->decoded = -1;
        return 0;
    }
    SDL_AudioSpec want = { SDL_AUDIO_S16LE, 1, spec.freq };
    Uint8* out = NULL;
    int outlen = 0;
    int ok = SDL_ConvertAudioSamples(&spec, buf, (int)len, &want, &out, &outlen);
    SDL_free(buf);
    if (!ok || !out) {
        OPL_WARN_F("sfxpack", "%s: cannot convert (%s)", e->path, SDL_GetError());
        e->decoded = -1;
        return 0;
    }
    if (spec.channels != 1)
        OPL_WARN_F("sfxpack", "%s: %d channels downmixed to mono (the SPU "
                   "voice is mono; pan is the game's)", e->path, spec.channels);
    e->pcm   = (int16_t*)out;
    e->count = outlen / 2;
    e->rate  = (uint32_t)spec.freq;
    if (!wav_read_smpl(e->path, &e->loop_start, &e->loop_len)) {
        e->loop_start = 0; e->loop_len = 0;
    }
    e->decoded = 1;
    return 1;
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
    if (openpete_mods_asset_root_count() > s_seen_roots) rescan();
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
    sp_entry* e = NULL;
    for (int i = 0; i < entry_count(); i++) {
        sp_entry* o = entry_at(i);
        if (o->src < 0 && strcmp(o->owner, own) == 0 &&
            strcmp(o->id.name, name) == 0) { e = o; break; }
    }
    if (!e) {
        e = entry_push();
        if (!e) { SDL_free(pcm); return -1; }
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
    rescan();
    PsyX_SPUAL_SetSampleOverride(override_cb);
    if (entry_count())
        OPL_INFO_F("sfxpack", "sfx pack active: %d file%s (%d source%s)",
                   entry_count(), entry_count() == 1 ? "" : "s", src_count(),
                   src_count() == 1 ? "" : "s");
}

void sfx_pack_generation_bump(void)
{
    s_gen++;
    memset(s_cache, 0, sizeof s_cache);
    rescan();
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

void sfx_pack_census(sfx_pack_census_t* out) { *out = s_census; }
