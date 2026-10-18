/* texture_pack.c - see texture_pack.h.
 *
 * Registry model: one entry per live upload rect. A new load that overlaps a
 * live entry kills it (exact-rect re-upload replaces in place, keeping the
 * slot). Moves duplicate identity to the destination (translated); clears
 * kill covered entries. CLUTs are ordinary entries (their rects are CLUT
 * rows); the texture<->CLUT pairing happens at resolve time, where the full
 * palette row is hashed from the live native VRAM model (authoritative by
 * construction - the draw samples that same model) on each resolve-cache
 * miss; results are cached against the registry generation.
 *
 * Hashing: XXH3-64 (vendored third_party/xxhash). The texture hash covers
 * the FULL upload source; sub-rect draws share it and carry their box in the
 * resolved id: the hash covers exactly what was uploaded, never a guessed
 * VRAM region. */
#include "texture_pack.h"
#include "openpete_crash.h"
#include "openpete_discontinuity.h"
#include "openpete_mods.h" /* asset-overlay roots */
#include "openpete_native_vram.h"
#include "openpete_profile.h"

#include "PsyX/PsyX_public.h"  /* PsyX_VRAM_Snapshot - restore-time rescan */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

/* Registry lock. The entry table, the resolve/match/fit caches and the pack
 * table are written by the guest's VRAM ops (on the thread running the
 * guest) and read by resolve/match from the bake AND from the compose,
 * which may run on a present thread. One recursive mutex around every
 * public entry that touches them; uncontended except at upload time. */
static SDL_Mutex *s_reg_mx;
/** @brief Return the registry mutex, creating it on first use.
 *
 * The lazy creation is not synchronized: two threads making the first call
 * concurrently can each create a mutex. */
static SDL_Mutex *reg_mx(void) { if (!s_reg_mx) s_reg_mx = SDL_CreateMutex(); return s_reg_mx; }

#define XXH_INLINE_ALL
#include "xxhash.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

#include <SDL3/SDL.h>  /* SDL_GetBasePath - same discovery music_override uses */
#include "openpete_paths.h" /* the writable user pack root */
#include "screenshot.h"     /* openpete_write_png_rgba: the dump's PNGs */
#include "mod_vec.h"
#include <dirent.h>

#define TP_MAX_ENTRIES   4096
#define TP_MAX_ANNOT     128
#define TP_RESOLVE_SLOTS 4096   /* open-addressed resolve cache (power of 2) */

typedef struct {
    int      alive;
    int      x, y, w, h;        /* VRAM 16-bit-pixel units */
    uint64_t thash;             /* XXH3-64 of the full upload source */
    const char *asset;          /* symbolic name or NULL */
} tp_entry;

typedef struct {
    int x, y, w, h;
    const char *name;
    uint64_t thash;             /* content the name is bound to; 0 = armed,
                                 * binds to the next upload landing on the rect */
} tp_annot;

typedef struct {                /* resolve cache slot */
    uint64_t key;               /* packed material+box; 0 = empty */
    uint32_t gen;
    uint32_t ver;               /* VRAM image version (openpete_vram_read_view) */
    texture_pack_id id;
    int      hit;               /* 1 = resolves, 0 = negative entry */
} tp_slot;

static tp_entry s_ent[TP_MAX_ENTRIES];
static int      s_ent_n = 0;          /* high-water mark */
static tp_annot s_annot[TP_MAX_ANNOT];
static int      s_annot_n = 0;
static uint32_t s_gen = 1;            /* bumped on any registry mutation */
/* One resolve cache per VRAM image a thread reads (openpete_vram_read_view):
 * [0] the model, [1] the composed image. The phash a slot holds is computed
 * from that image, and a present thread's composed image trails the model
 * by the scenes it has not yet presented, so one cache shared by both would
 * hand each thread the other's palette hashes. */
static tp_slot  s_cache[2][TP_RESOLVE_SLOTS];
static int      s_inited = 0;

/* ---- replacement pack state ---------------------------------------------- */
/* The pack INDEX is built at init by parsing every PNG filename in the dir
 * (identity is fully encoded in the name); images load lazily on first
 * match. Matching is containment-based - see texture_pack.h. */
/* Entries the first block holds; the table grows by whole blocks. */
#define TP_PACK_BLOCK 8192
typedef struct {
    uint64_t thash, phash;
    int bpp;
    int bx, by, bw, bh;            /* the file's upload-relative texel box */
    int symbolic;                  /* named asset file (<asset>.png) - matched by
                                    * the game layer's texture_pack_annotate name;
                                    * covers the FULL upload */
    unsigned char *px;             /* decoded by the worker; read only once
                                    * px_state is TP_PX_READY */
    int w, h;
    int load_failed;
    SDL_AtomicInt px_state;        /* TP_PX_* - publishes px across threads */
    unsigned char urgent;          /* a draw is waiting on this one */
    unsigned char src;             /* index into s_srcs (0 = base pack dir) */
    char name[96];                 /* filename; for symbolic: asset name (no .png) */
} tp_pack_entry;
/* Chunked: the decode worker dereferences an entry by index while a late
 * mod source appends on the sim thread, and a block never moves. */
static mod_cvec s_pack;   /* tp_pack_entry */

/** @brief Return pack entry i, or NULL when i is outside [0, pack_count()). */
static tp_pack_entry* pack_at(int i) {
    return (tp_pack_entry*)mod_cvec_at(&s_pack, (size_t)i);
}
/** @brief Return the number of entries in the pack table. */
static int pack_count(void) { return (int)mod_cvec_count(&s_pack); }

/** @brief Append a pack entry and return it, or return NULL and log to stderr when the allocation fails.
 *
 * Initializes the chunked table on first use. */
static tp_pack_entry* pack_push(void) {
    tp_pack_entry *e;
    if (!s_pack.elem_size)
        mod_cvec_init(&s_pack, sizeof(tp_pack_entry), TP_PACK_BLOCK,
                      "texpack_pack", TP_PACK_BLOCK);
    e = (tp_pack_entry*)mod_cvec_push(&s_pack);
    if (!e) fprintf(stderr, "%s", "[texpack] entry table allocation failed\n");
    return e;
}

static char s_repl_dir[512];
static int  s_repl_active = 0;
/* Layered sources (asset-overlay v1): [0] = the base pack dir; then one
 * per enabled mod with an assets/textures/ dir, in enabled order. A
 * later source shipping the SAME identity replaces the earlier
 * entry at scan time (last-writer-wins - the overlay's single priority
 * truth), logged for the resolved-asset assert. */
#define TP_SRC_BLOCK 17
typedef struct {
    char dir[560];
    char label[64];
    char owner[64];  /* mod id; "" = base pack (always enabled) */
} tp_source;
/* Chunked for the same reason as s_pack: the decode worker reads a
 * source's dir while a late root appends. */
static mod_cvec s_srcs;   /* tp_source */

/** @brief Return pack source i, or NULL when i is outside [0, src_count()). */
static tp_source* src_at(int i) {
    return (tp_source*)mod_cvec_at(&s_srcs, (size_t)i);
}
/** @brief Return the number of pack sources. */
static int src_count(void) { return (int)mod_cvec_count(&s_srcs); }

/** @brief Append a pack source and return it, or return NULL and log to stderr when the allocation fails.
 *
 * Initializes the chunked table on first use. */
static tp_source* src_push(void) {
    tp_source *sp;
    if (!s_srcs.elem_size)
        mod_cvec_init(&s_srcs, sizeof(tp_source), TP_SRC_BLOCK, "texpack_src",
                      TP_SRC_BLOCK);
    sp = (tp_source*)mod_cvec_push(&s_srcs);
    if (!sp) fprintf(stderr, "%s", "[texpack] source table allocation failed\n");
    return sp;
}

/** @brief Return 1 when source src serves: a source with no owner (the base and user packs) always, a mod source while its mod is enabled.
 *
 * The mod check reads the live enable state, so a toggle takes effect on
 * the next match. An index with no source also returns 1. */
static int tp_src_enabled(int src) {
    const tp_source *sp = src_at(src);
    return !sp || !sp->owner[0] || openpete_mods_is_enabled(sp->owner);
}

static int tp_add_root_source(int r);
static void tp_decode_prefetch_all(void);
static int s_seen_roots = 0; /* asset roots already turned into sources */

/** @brief Fold mod toggles and newly registered asset roots into the pack state.
 *
 * A change of openpete_mods_toggle_epoch() changes which sources serve, so
 * it bumps s_gen, which invalidates every memo slot keyed on the
 * generation (resolve, match, decode). A mod with a deferred entry
 * registers its asset root at its first in-game enable, after the boot
 * scan enumerated sources; every root index past s_seen_roots is turned
 * into a source by tp_add_root_source, and when any source was added the
 * generation is bumped, s_repl_active is recomputed from the pack count,
 * and the decode prefetch starts. Called from resolve, match, overlaps,
 * texture_pack_replacements_active and texture_pack_generation; the
 * common case costs one epoch read and one root-count read. */
static void tp_poll_modset(void) {
    static unsigned s_seen_epoch = 0;
    unsigned ep = openpete_mods_toggle_epoch();
    if (ep != s_seen_epoch) { s_seen_epoch = ep; s_gen++; }
    int n = openpete_mods_asset_root_count();
    if (n > s_seen_roots) {
        int grew = 0;
        for (int r = s_seen_roots; r < n; r++) grew |= tp_add_root_source(r);
        s_seen_roots = n;
        if (grew) {
            s_gen++;
            s_repl_active = pack_count() > 0;
            /* A mod's pack arrives here, not through tp_repl_scan - this is
             * the activation path that matters in practice, so it is the one
             * that must start the prefetch. */
            tp_decode_prefetch_all();
        }
    }
}
static char s_dir_override[512];   /* --texpack-dir; empty = unset (see set_dir_override) */

void texture_pack_set_dir_override(const char *dir)
{
    if (dir && *dir) snprintf(s_dir_override, sizeof s_dir_override, "%s", dir);
    else             s_dir_override[0] = '\0';
}

/* memo: identity key -> pack entry index (or -1 = no match). Slot count
 * is a power of two at least twice the pack, so the probe window stays
 * short however many entries the sources bring. */
#define TP_MATCH_SLOTS_MIN 8192
typedef struct { uint64_t key; uint32_t gen; int entry; } tp_match_slot;
static mod_fvec s_match;   /* tp_match_slot, power-of-two slot count */

/** @brief Size the match memo to the smallest power of two at least 8192 and at least twice the pack count, and zero it.
 *
 * A failed resize leaves the memo empty, which costs the match its
 * memoization and nothing else. */
static void tp_match_reset(void) {
    size_t want = TP_MATCH_SLOTS_MIN;
    while (want < (size_t)pack_count() * 2u) want *= 2u;
    if (!s_match.elem_size)
        mod_fvec_init(&s_match, sizeof(tp_match_slot), TP_MATCH_SLOTS_MIN,
                      "texpack_match", TP_MATCH_SLOTS_MIN);
    if (mod_fvec_count(&s_match) != want &&
        mod_fvec_resize(&s_match, want) != 0)
        mod_fvec_truncate(&s_match, 0);
    if (mod_fvec_count(&s_match))
        memset(mod_fvec_data(&s_match), 0,
               mod_fvec_count(&s_match) * sizeof(tp_match_slot));
}

/** @brief Fill out from one pack file name, as a hash-form entry or a symbolic entry.
 *
 * The hash form is <thash>-<bx>x<by>+<bw>x<bh>-<phash>-<bpp>bpp.png with
 * both hashes in hex. A name that does not open with 16 hex digits is
 * taken to carry a leading `<label>-` (the dumper's scene-category slug,
 * organisational only), and the parse starts after its first '-'. A hash
 * entry stores the whole file name. Any other name is symbolic,
 * `<asset>.png`, matched against a texture_pack_annotate name and covering
 * the full upload; it stores the name without the extension. out is
 * zeroed first. */
static void tp_parse_name(const char *fname, const char *dot,
                          tp_pack_entry *out)
{
    unsigned long long th, ph;
    int bx, by, bw, bh, bpp;
    const char *idpart = fname;
    const char *dash = strchr(fname, '-');
    int hexrun = 0;
    memset(out, 0, sizeof *out);
    while (isxdigit((unsigned char)fname[hexrun])) hexrun++;
    if (dash && hexrun < 16) idpart = dash + 1;
    if (sscanf(idpart, "%16llx-%dx%d+%dx%d-%16llx-%dbpp.png",
               &th, &bx, &by, &bw, &bh, &ph, &bpp) == 7) {
        out->thash = th; out->phash = ph; out->bpp = bpp;
        out->bx = bx; out->by = by; out->bw = bw; out->bh = bh;
        snprintf(out->name, sizeof out->name, "%s", fname);
    } else {
        out->symbolic = 1;
        snprintf(out->name, sizeof out->name, "%.*s",
                 (int)(dot - fname), fname);
    }
}

/** @brief Return the index of the first pack entry with the same identity as q, or -1.
 *
 * Symbolic entries compare by name; hash entries by thash, phash, bpp and
 * box. */
static int tp_find_identity(const tp_pack_entry *q)
{
    for (int i = 0; i < pack_count(); i++) {
        const tp_pack_entry *pe = pack_at(i);
        if (pe->symbolic != q->symbolic) continue;
        if (q->symbolic ? (strcmp(pe->name, q->name) == 0)
                        : (pe->thash == q->thash &&
                           pe->phash == q->phash &&
                           pe->bpp == q->bpp &&
                           pe->bx == q->bx &&
                           pe->by == q->by &&
                           pe->bw == q->bw &&
                           pe->bh == q->bh))
            return i;
    }
    return -1;
}

/** @brief Append one scanned file as a pack entry; return 0 when the table could not grow.
 *
 * A duplicate identity from an earlier source is logged to stderr and both
 * entries stay resident: the match loops pick the later (higher-priority)
 * enabled one, so a live toggle falls back to the earlier source's file.
 * The log is the resolved-asset table: with both sources enabled, the
 * later one wins. */
static int tp_entry_add(int src, const tp_pack_entry *tmp)
{
    int hit = tp_find_identity(tmp);
    tp_pack_entry *slot;
    if (hit >= 0)
        fprintf(stderr, "[texpack] %s overrides \"%s\" (was %s)\n",
                src_at(src)->label, tmp->name,
                src_at(pack_at(hit)->src)->label);
    slot = pack_push();
    if (!slot) return 0;
    *slot = *tmp;
    return 1;
}

/** @brief Append every `*.png` file in source src's directory to s_pack and return the number added.
 *
 * The scan is flat, so subdirectories under the same textures/ root (such
 * as own/, the declarative own-texture ids) are skipped. A name of 96
 * bytes or more is skipped; the scan stops when the table cannot grow. A
 * missing directory adds nothing. */
static int tp_scan_dir(int src)
{
    DIR *d = opendir(src_at(src)->dir);
    if (!d) return 0;
    int touched = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *dot = strrchr(de->d_name, '.');
        tp_pack_entry tmp;
        if (!dot || strcmp(dot, ".png") != 0) continue;
        if (strlen(de->d_name) >= sizeof tmp.name) continue;
        tp_parse_name(de->d_name, dot, &tmp);
        tmp.src = (unsigned char)src;
        if (!tp_entry_add(src, &tmp)) break;
        touched++;
    }
    closedir(d);
    return touched;
}

/** @brief Add mod asset root r's `textures` directory as a pack source and scan it; return nonzero when any entry was added.
 *
 * The source is labelled and owned by the root's mod, so it serves only
 * while that mod is enabled. It is appended even when its directory holds
 * no file. A nonempty scan is logged to stderr. */
static int tp_add_root_source(int r)
{
    const char* owner = NULL;
    const char* root = openpete_mods_asset_root(r, &owner);
    if (!root) return 0;
    tp_source *sp = src_push();
    if (!sp) return 0;
    int si = src_count() - 1;
    snprintf(sp->dir, sizeof sp->dir, "%s/textures", root);
    snprintf(sp->label, sizeof sp->label, "mod %s", owner ? owner : "?");
    snprintf(sp->owner, sizeof sp->owner, "%s", owner ? owner : "");
    int before = pack_count();
    int added = tp_scan_dir(si);
    if (added > 0 || pack_count() != before)
        fprintf(stderr, "[texpack] + %s: %d entr%s (%s)\n",
                src_at(si)->label, added, added == 1 ? "y" : "ies",
                src_at(si)->dir);
    return added > 0 || pack_count() != before;
}

/* ---- Background replacement decode ---------------------------------------
 *
 * Decoding a replacement is a blocking PNG read off disk. Doing it inline in
 * texture_pack_match is doing it during draw-time texture resolve - on the
 * game fiber, inside the tick - so the host loop stops for as long as the
 * read takes. A level's worth arriving at once (first render of a scene the
 * pack covers) has stalled it for 228-275 ms, far past the pacer's slip
 * clamp, which silently discards that much wall-clock from the game clock.
 *
 * Decodes therefore run on a worker. The whole enabled pack is prefetched
 * when it goes active, so by the time a draw wants an entry it is normally
 * already resident; anything the prefetch has not reached is queued at the
 * front on first use, and that draw falls back to the base texture for as
 * long as it takes. The tick never blocks on disk either way.
 *
 * Prefetch stops adding to resident memory past TP_PREFETCH_BUDGET; entries
 * beyond it stay on the on-demand path rather than being dropped, so a pack
 * larger than the budget still renders fully, just with brief fallback the
 * first time each texture is drawn. */
#define TP_PX_NONE   0
#define TP_PX_QUEUED 1
#define TP_PX_READY  2
#define TP_PX_FAILED 3

/* Decoded-pixel ceiling for *prefetch*. On-demand decodes ignore it - a
 * texture a draw is actually waiting for is always worth the memory. */
#define TP_PREFETCH_BUDGET ((size_t)256 * 1024 * 1024)

/* Prefetch ring depth, one slot of which stays empty. A push into a full
 * ring is refused and tp_decode_request returns the entry to TP_PX_NONE, so
 * the next draw that wants it requests it again. */
#define TP_Q_CAP 8193

static struct {
    SDL_Thread    *th;
    SDL_Mutex     *mu;
    SDL_Condition *cv;
    int            q[TP_Q_CAP];
    int            head, tail;     /* ring: head = next out, tail = next in */
    int            busy;           /* a decode is in flight */
    int            quit;
    size_t         resident;       /* decoded bytes currently held */
    int            pf_outstanding; /* prefetch items still queued/in flight */
    Uint64         pf_t0;          /* prefetch start, for the completion log */
    unsigned       pf_done;        /* decoded by the prefetch */
    unsigned       budget_skips;   /* prefetch items deferred to on-demand */
    int            budget_logged;
} s_dec;

/** @brief Return nonzero when the decode ring holds no entry.
 *
 * The caller holds s_dec.mu. */
static int tp_q_empty(void) { return s_dec.head == s_dec.tail; }

/** @brief Push pack entry ei onto the decode ring, at the head when front is set; refuse it when the ring is full.
 *
 * The caller holds s_dec.mu and has already set the entry to
 * TP_PX_QUEUED; on a refusal it owns returning the entry to TP_PX_NONE.
 * @return 1 when the entry is on the ring, 0 when the ring was full. */
static int tp_q_push(int ei, int front) {
    int nxt = (s_dec.tail + 1) % TP_Q_CAP;
    if (nxt == s_dec.head) return 0;          /* full */
    if (front) {
        s_dec.head = (s_dec.head + TP_Q_CAP - 1) % TP_Q_CAP;
        s_dec.q[s_dec.head] = ei;
    } else {
        s_dec.q[s_dec.tail] = ei;
        s_dec.tail = nxt;
    }
    return 1;
}

/** @brief Decode pack entry ei's PNG to RGBA8 and publish it as READY.
 *
 * Reads `<source dir>/<name>` (with `.png` appended for a symbolic entry)
 * through stb_image. A failed load sets load_failed and the FAILED state.
 * On success it stores w, h and px, issues a release barrier, then sets
 * READY, so a reader that sees READY sees all three, and adds the decoded
 * byte count to s_dec.resident and one to pf_done. Runs on the decode worker,
 * or on the requesting thread when no worker could start. */
static void tp_decode_one(int ei) {
    tp_pack_entry *e = pack_at(ei);
    char path[704];
    if (!e) return;
    snprintf(path, sizeof path, "%s/%s%s", src_at(e->src)->dir, e->name,
             e->symbolic ? ".png" : "");
    int comp = 0, w = 0, h = 0;
    unsigned char *px = stbi_load(path, &w, &h, &comp, 4);
    if (!px) {
        e->load_failed = 1;
        SDL_SetAtomicInt(&e->px_state, TP_PX_FAILED);
        return;
    }
    e->w = w; e->h = h;
    e->px = px;
    /* Publish w/h/px before the state flip, so a reader that sees READY sees
     * all three. */
    SDL_MemoryBarrierRelease();
    SDL_SetAtomicInt(&e->px_state, TP_PX_READY);

    SDL_LockMutex(s_dec.mu);
    s_dec.resident += (size_t)w * (size_t)h * 4u;
    s_dec.pf_done++;
    SDL_UnlockMutex(s_dec.mu);
}

/** @brief Body of the texture-pack decode thread: decode queued pack images until shutdown. */
static int SDLCALL tp_decode_main(void *unused) {
    (void)unused;
    openpete_crash_thread_attach();
    for (;;) {
        SDL_LockMutex(s_dec.mu);
        while (!s_dec.quit && tp_q_empty())
            SDL_WaitCondition(s_dec.cv, s_dec.mu);
        if (s_dec.quit) { SDL_UnlockMutex(s_dec.mu); return 0; }
        int ei = s_dec.q[s_dec.head];
        s_dec.head = (s_dec.head + 1) % TP_Q_CAP;
        s_dec.busy = 1;
        int over = s_dec.resident >= TP_PREFETCH_BUDGET;
        SDL_UnlockMutex(s_dec.mu);

        tp_pack_entry *e = pack_at(ei);
        if (!e) { SDL_LockMutex(s_dec.mu); s_dec.busy = 0; SDL_UnlockMutex(s_dec.mu); continue; }
        if (over && !e->urgent) {
            /* Back to NONE, not FAILED: the first draw that wants it will
             * re-queue it as urgent and it decodes then. */
            SDL_SetAtomicInt(&e->px_state, TP_PX_NONE);
            SDL_LockMutex(s_dec.mu);
            s_dec.budget_skips++;
            int say = !s_dec.budget_logged;
            s_dec.budget_logged = 1;
            size_t res = s_dec.resident;
            SDL_UnlockMutex(s_dec.mu);
            if (say)
                fprintf(stderr, "[texpack] prefetch budget reached (%zu MiB "
                        "resident); remaining entries decode on first draw, "
                        "which shows the base texture briefly\n",
                        res / (1024u * 1024u));
        } else {
            tp_decode_one(ei);
        }

        SDL_LockMutex(s_dec.mu);
        s_dec.busy = 0;
        if (s_dec.pf_outstanding > 0 && --s_dec.pf_outstanding == 0) {
            double ms = (double)(SDL_GetTicksNS() - s_dec.pf_t0) / 1e6;
            unsigned n = s_dec.pf_done;
            size_t res = s_dec.resident;
            SDL_UnlockMutex(s_dec.mu);
            fprintf(stderr, "[texpack] prefetch complete: %u decoded, "
                    "%zu KiB resident, %.0f ms\n", n, res / 1024u, ms);
            SDL_LockMutex(s_dec.mu);
        }
        SDL_BroadcastCondition(s_dec.cv);
        SDL_UnlockMutex(s_dec.mu);
    }
}

/** @brief Start the decode worker thread if it is not running.
 *
 * Creates the worker's mutex and condition variable on first use and clears
 * the quit flag. Leaves s_dec.th NULL when creation fails, which makes
 * tp_decode_request decode inline. */
static void tp_decode_start(void) {
    if (s_dec.th) return;
    if (!s_dec.mu) s_dec.mu = SDL_CreateMutex();
    if (!s_dec.cv) s_dec.cv = SDL_CreateCondition();
    if (!s_dec.mu || !s_dec.cv) return;
    s_dec.quit = 0;
    s_dec.th = SDL_CreateThread(tp_decode_main, "openpete-texdec", NULL);
}

/** @brief Empty the decode queue and wait for any in-flight decode.
 *
 * tp_repl_scan calls it before clearing s_pack, which the worker indexes;
 * shutdown stops the worker through tp_decode_stop instead. A no-op before
 * the worker's mutex exists. */
static void tp_decode_quiesce(void) {
    if (!s_dec.mu) return;
    SDL_LockMutex(s_dec.mu);
    s_dec.head = s_dec.tail = 0;
    while (s_dec.busy) SDL_WaitCondition(s_dec.cv, s_dec.mu);
    SDL_UnlockMutex(s_dec.mu);
}

/** @brief Stop the decode worker, discarding queued entries, and join it.
 *
 * Sets quit, empties the ring and wakes the worker; a decode in flight
 * finishes first. Entries dropped from the ring keep their QUEUED state, and
 * s_dec.resident and pf_outstanding are not reset. A no-op when no worker
 * runs. */
static void tp_decode_stop(void) {
    if (!s_dec.th) return;
    SDL_LockMutex(s_dec.mu);
    s_dec.quit = 1;
    s_dec.head = s_dec.tail = 0;
    SDL_BroadcastCondition(s_dec.cv);
    SDL_UnlockMutex(s_dec.mu);
    SDL_WaitThread(s_dec.th, NULL);
    s_dec.th = NULL;
}

/** @brief Queue pack entry ei for decode unless it is already decoded, failed, or queued.
 *
 * `urgent` marks a draw as waiting: the entry goes to the head of the
 * queue and the worker decodes it past the prefetch memory budget; the
 * mark is set even when the entry is already queued. The worker thread is
 * started on demand; when it cannot start, the entry decodes inline.
 * @return 1 when this call put the entry on the ring, 0 otherwise (already
 *         ready, failed or queued, decoded inline, or refused by a full ring). */
static int tp_decode_request(int ei, int urgent) {
    tp_pack_entry *e = pack_at(ei);
    if (!e) return 0;
    if (urgent) e->urgent = 1;
    int st = SDL_GetAtomicInt(&e->px_state);
    if (st != TP_PX_NONE) return 0;            /* ready, failed, or queued */
    if (!s_dec.th) tp_decode_start();
    if (!s_dec.th) {                           /* no worker - decode inline */
        tp_decode_one(ei);
        return 0;
    }
    if (!SDL_CompareAndSwapAtomicInt(&e->px_state, TP_PX_NONE, TP_PX_QUEUED))
        return 0;                              /* another thread queued it */
    SDL_LockMutex(s_dec.mu);
    /* A full ring leaves the entry at NONE, the state the worker's budget
     * skip also returns to, so a later draw re-requests it. */
    int queued = tp_q_push(ei, urgent);
    if (!queued) SDL_SetAtomicInt(&e->px_state, TP_PX_NONE);
    SDL_SignalCondition(s_dec.cv);
    SDL_UnlockMutex(s_dec.mu);
    return queued;
}

/** @brief Queue every entry of an enabled source for decode, so a draw normally finds its replacement already resident.
 *
 * Returns without queueing when no pack is active or
 * OPENPETE_TEXPACK_PREFETCH starts with '0'. The count of entries that
 * reached the ring is added to the outstanding prefetch count, whose
 * completion the worker logs; an entry the full ring refused is not
 * outstanding. */
static void tp_decode_prefetch_all(void) {
    if (!s_repl_active) return;
    /* OPENPETE_TEXPACK_PREFETCH=0 leaves every entry on the on-demand path -
     * a diagnostic leg for pricing the prefetch against the contention it
     * causes while it runs. */
    const char *pf = getenv("OPENPETE_TEXPACK_PREFETCH");
    if (pf && *pf == '0') {
        fprintf(stderr, "%s", "[texpack] prefetch disabled; replacements "
                "decode on first draw\n");
        return;
    }
    tp_decode_start();
    SDL_LockMutex(s_dec.mu);
    s_dec.pf_t0 = SDL_GetTicksNS();
    s_dec.pf_done = 0;
    SDL_UnlockMutex(s_dec.mu);
    int n = 0;
    for (int i = 0; i < pack_count(); i++)
        if (tp_src_enabled(pack_at(i)->src)) n += tp_decode_request(i, 0);
    SDL_LockMutex(s_dec.mu);
    s_dec.pf_outstanding += n;
    SDL_UnlockMutex(s_dec.mu);
    if (n)
        fprintf(stderr, "[texpack] prefetching %d replacement%s off-tick\n",
                n, n == 1 ? "" : "s");
}

/** @brief Rebuild the replacement pack index from every source directory.
 *
 * Quiesces the decode worker, clears the pack table and source table, then
 * scans the base directory (--texpack-dir, else OPENPETE_TEXPACK_DIR, else
 * `<exe dir>/assets/textures`), the user pack directory when neither override
 * is set and it differs from the base, and every mod asset root registered
 * so far. Decoded pixels of the cleared entries are not freed here; the
 * caller must have none. Sets s_repl_active from the pack count and starts
 * the prefetch. Returns early, leaving the pack inactive, when the exe base
 * path is unavailable or the base source cannot be added. */
static void tp_repl_scan(void)
{
    /* The worker indexes s_pack; this rebuilds it. */
    tp_decode_quiesce();
    s_repl_active = 0;
    mod_cvec_clear(&s_pack);
    tp_match_reset();
    /* Precedence: --texpack-dir override > OPENPETE_TEXPACK_DIR env > built-in dir. */
    const char *env = getenv("OPENPETE_TEXPACK_DIR");
    if (s_dir_override[0]) {
        snprintf(s_repl_dir, sizeof s_repl_dir, "%s", s_dir_override);
    } else if (env && *env) {
        snprintf(s_repl_dir, sizeof s_repl_dir, "%s", env);
    } else {
        const char *base = SDL_GetBasePath();
        if (!base) return;
        snprintf(s_repl_dir, sizeof s_repl_dir, "%sassets/textures", base);
    }
    mod_cvec_clear(&s_srcs);
    {
        tp_source *base_src = src_push();
        if (!base_src) return;
        snprintf(base_src->dir, sizeof base_src->dir, "%s", s_repl_dir);
        snprintf(base_src->label, sizeof base_src->label, "base pack");
        tp_scan_dir(0);
    }
    /* The shipped pack sits inside the install, which may be read-only -
     * so the user's own drop-in dir is a SECOND base root, not a
     * replacement, and outranks the shipped one. An explicit
     * --texpack-dir / OPENPETE_TEXPACK_DIR names exactly one directory
     * and is not joined by anything. */
    if (!s_dir_override[0] && !(env && *env)) {
        char user[512];
        if (openpete_paths_data_dir("assets", user, sizeof user, 0) == 0) {
            char dir[560];
            snprintf(dir, sizeof dir, "%s/textures", user);
            if (strcmp(dir, s_repl_dir) != 0) {
                tp_source *us = src_push();
                if (us) {
                    snprintf(us->dir, sizeof us->dir, "%s", dir);
                    snprintf(us->label, sizeof us->label, "user pack");
                    tp_scan_dir(src_count() - 1);
                }
            }
        }
    }
    /* Mod asset roots, enabled order = priority (later wins). */
    int n_roots = openpete_mods_asset_root_count();
    for (int r = 0; r < n_roots; r++) (void)tp_add_root_source(r);
    s_seen_roots = n_roots;
    s_repl_active = pack_count() > 0;
    tp_match_reset();
    if (s_repl_active)
        fprintf(stderr, "[texpack] replacement pack active: %d entries "
                "(%d source%s)\n", pack_count(), src_count(),
                src_count() == 1 ? "" : "s");
    tp_decode_prefetch_all();
}

/* Draws texture_pack_match served, by the matched file's form; reported at
 * shutdown so a harness can require a replacement to have engaged. */
static uint64_t s_served[2]; /* [0] by content hash, [1] symbolic */

void texture_pack_init(void)
{
    if (s_inited) return;
    s_inited = 1;
    (void)openpete_on_guest_discontinuity(texture_pack_on_restore);
    tp_repl_scan();
}

/** @brief Body of texture_pack_on_restore; the caller holds the registry lock.
 *
 * Snapshots the PsyX VRAM mirror and re-hashes every live entry's rect
 * against its upload hash; a mismatch or an out-of-VRAM rect kills the
 * entry. A failed snapshot kills every live entry. Bumps s_gen when any
 * entry died and logs the counts to stderr. */
static void texture_pack_on_restore_locked(void) {
    if (!s_inited) return;
    unsigned bytes = PsyX_VRAM_GetSize();
    uint16_t *snap = (uint16_t *)malloc(bytes);
    if (!snap || PsyX_VRAM_Snapshot(snap, bytes) != 0) {
        /* Can't verify - kill everything rather than risk serving a stale
         * replacement over restored content. */
        free(snap);
        int killed = 0;
        for (int i = 0; i < s_ent_n; i++)
            if (s_ent[i].alive) { s_ent[i].alive = 0; killed++; }
        if (killed) s_gen++;
        fprintf(stderr, "[texpack] guest restore: VRAM snapshot failed; "
                "%d entries killed unverified\n", killed);
        return;
    }

    /* The PsyX mirror is the authoritative post-restore content (the native
     * model refreshes lazily at the next present). Re-hash each live rect;
     * identity survives iff the content still matches. thash covers the
     * clamped rect only (stride skips the clamped columns), so a clamped
     * upload re-hashes equal to its thash. */
    const int vw = OPENPETE_VRAM_W;
    const int vh = (int)(bytes / ((unsigned)vw * 2u));
    int kept = 0, killed = 0;
    for (int i = 0; i < s_ent_n; i++) {
        tp_entry *e = &s_ent[i];
        if (!e->alive) continue;
        if (e->x < 0 || e->y < 0 || e->x + e->w > vw || e->y + e->h > vh) {
            e->alive = 0; killed++;
            continue;
        }
        XXH3_state_t st;
        XXH3_64bits_reset(&st);
        for (int r = 0; r < e->h; r++)
            XXH3_64bits_update(&st, snap + (size_t)(e->y + r) * (size_t)vw + e->x,
                               (size_t)e->w * 2);
        if (XXH3_64bits_digest(&st) == e->thash) {
            kept++;
        } else {
            e->alive = 0; killed++;
        }
    }
    free(snap);
    if (killed) s_gen++;
    fprintf(stderr, "[texpack] guest restore: %d entries verified live, "
            "%d stale (killed)\n", kept, killed);
}
void texture_pack_on_restore(void) { SDL_LockMutex(reg_mx()); texture_pack_on_restore_locked(); SDL_UnlockMutex(s_reg_mx); }

void texture_pack_shutdown(void)
{
    if (getenv("OPENPETE_TEXPACK_DUMP_REGISTRY")) texture_pack_debug_dump();
    if (s_repl_active)
        fprintf(stderr, "[texpack] served=%llu draws (symbolic=%llu hash=%llu)\n",
                (unsigned long long)(s_served[0] + s_served[1]),
                (unsigned long long)s_served[1],
                (unsigned long long)s_served[0]);
    tp_decode_stop();
    for (int i = 0; i < pack_count(); i++) {
        tp_pack_entry *e = pack_at(i);
        if (e->px) { stbi_image_free(e->px); e->px = NULL; }
    }
    mod_cvec_clear(&s_pack);
    s_ent_n = 0; s_annot_n = 0; s_gen++; s_inited = 0; s_repl_active = 0;
}

int texture_pack_replacements_active(void)
{
    if (s_inited) tp_poll_modset(); /* late roots can activate the pack */
    return s_repl_active;
}

/** @brief Return nonzero when live registry entry e intersects the rect (x, y, w, h). */
static int tp_overlap(const tp_entry *e, int x, int y, int w, int h)
{
    return e->alive && x < e->x + e->w && e->x < x + w &&
                       y < e->y + e->h && e->y < y + h;
}

/** @brief Return a registry slot: the first dead one, else a fresh one, else slot 0.
 *
 * A fresh slot extends the high-water mark s_ent_n up to TP_MAX_ENTRIES.
 * With the pool full it returns &s_ent[0] whether or not that entry is
 * alive, so the caller overwrites a live identity, and logs once to
 * stderr. The returned slot is not initialized. */
static tp_entry *tp_alloc(void)
{
    for (int i = 0; i < s_ent_n; i++) if (!s_ent[i].alive) return &s_ent[i];
    if (s_ent_n < TP_MAX_ENTRIES) return &s_ent[s_ent_n++];
    /* Pool exhausted: drop the oldest slot (index 0 heuristic). Should not
     * happen in practice (level loads clear-invalidate); log once. */
    static int warned = 0;
    if (!warned) { warned = 1; fprintf(stderr, "[texpack] entry pool full\n"); }
    return &s_ent[0];
}

/** @brief Body of texture_pack_on_load; the caller holds the registry lock.
 *
 * Ignores an empty rect or a NULL source. Bumps s_gen. An entry the rect
 * overlaps dies unless its rect is identical, in which case it is reused in
 * place; otherwise a slot comes from tp_alloc. Hashes the source rows
 * (through src_stride) with XXH3-64 into thash, then binds the entry to an
 * annotation on the same rect: an armed annotation takes this hash, and an
 * annotation whose hash equals it names the entry. */
static void texture_pack_on_load_locked(int x, int y, int w, int h,
                          const uint16_t *src, int src_stride) {
    if (w <= 0 || h <= 0 || !src) return;
    s_gen++;
    tp_entry *reuse = NULL;
    for (int i = 0; i < s_ent_n; i++) {
        tp_entry *e = &s_ent[i];
        if (!tp_overlap(e, x, y, w, h)) continue;
        if (e->x == x && e->y == y && e->w == w && e->h == h) reuse = e;
        else e->alive = 0;                 /* partial overwrite: identity gone */
    }
    tp_entry *e = reuse ? reuse : tp_alloc();
    e->alive = 1; e->x = x; e->y = y; e->w = w; e->h = h;
    if (src_stride == w) {
        e->thash = XXH3_64bits(src, (size_t)w * h * 2);
    } else {                               /* hash rows through the stride */
        XXH3_state_t st;
        XXH3_64bits_reset(&st);
        for (int i = 0; i < h; i++)
            XXH3_64bits_update(&st, src + (size_t)i * src_stride, (size_t)w * 2);
        e->thash = XXH3_64bits_digest(&st);
    }
    e->asset = NULL;
    for (int i = 0; i < s_annot_n; i++) {
        tp_annot *a = &s_annot[i];
        if (a->x != x || a->y != y || a->w != w || a->h != h) continue;
        /* The rect is shared: the title UI atlas and every level's texture
         * page land on the same VRAM block. The name belongs to one content,
         * so it binds to the first upload's hash and later uploads to the rect
         * carry it only when their bytes match. */
        if (a->thash == 0) {
            a->thash = e->thash;
            fprintf(stderr, "[texpack] annotate %s: bound to thash=%016llx "
                    "(%d,%d %dx%d, on upload)\n", a->name,
                    (unsigned long long)a->thash, x, y, w, h);
        }
        if (a->thash == e->thash) e->asset = a->name;
    }
}
void texture_pack_on_load(int x, int y, int w, int h,
                          const uint16_t *src, int src_stride) { SDL_LockMutex(reg_mx()); texture_pack_on_load_locked(x, y, w, h, src, src_stride); SDL_UnlockMutex(s_reg_mx); }


/** @brief Body of texture_pack_on_move; the caller holds the registry lock.
 *
 * Ignores an empty rect. Bumps s_gen. Entries overlapping the destination
 * die, then each live entry contained in the source rect is copied to the
 * translated position in a slot from tp_alloc. The walk stops at the entry
 * count taken before the copies, but a copy placed in a reused dead slot
 * below that count is walked as well. */
static void texture_pack_on_move_locked(int src_x, int src_y, int w, int h,
                          int dst_x, int dst_y) {
    if (w <= 0 || h <= 0) return;
    s_gen++;
    /* Kill identities the destination overwrites, then duplicate any entry
     * fully contained in the source rect, translated. (Partial moves shear a
     * texture - no stable identity; those entries just die at the dest.) */
    for (int i = 0; i < s_ent_n; i++)
        if (tp_overlap(&s_ent[i], dst_x, dst_y, w, h)) s_ent[i].alive = 0;
    int n = s_ent_n;                       /* don't walk entries we add */
    for (int i = 0; i < n; i++) {
        tp_entry *e = &s_ent[i];
        if (!e->alive) continue;
        if (e->x >= src_x && e->y >= src_y &&
            e->x + e->w <= src_x + w && e->y + e->h <= src_y + h) {
            tp_entry *d = tp_alloc();
            *d = *e;
            d->x = e->x - src_x + dst_x;
            d->y = e->y - src_y + dst_y;
        }
    }
}
void texture_pack_on_move(int src_x, int src_y, int w, int h,
                          int dst_x, int dst_y) { SDL_LockMutex(reg_mx()); texture_pack_on_move_locked(src_x, src_y, w, h, dst_x, dst_y); SDL_UnlockMutex(s_reg_mx); }


/** @brief Body of texture_pack_on_clear; the caller holds the registry lock.
 *
 * Ignores an empty rect. Bumps s_gen and kills every entry the rect
 * overlaps. */
static void texture_pack_on_clear_locked(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    s_gen++;
    for (int i = 0; i < s_ent_n; i++)
        if (tp_overlap(&s_ent[i], x, y, w, h)) s_ent[i].alive = 0;
}
void texture_pack_on_clear(int x, int y, int w, int h) { SDL_LockMutex(reg_mx()); texture_pack_on_clear_locked(x, y, w, h); SDL_UnlockMutex(s_reg_mx); }


/** @brief Body of texture_pack_annotate; the caller holds the registry lock.
 *
 * Appends an annotation (dropped silently at TP_MAX_ANNOT) that keeps the
 * name pointer. Every live entry with exactly this rect supplies the hash
 * and takes the name; with none live the annotation stays armed for the
 * next upload on the rect. Logs which case applied to stderr. */
static void texture_pack_annotate_locked(int x, int y, int w, int h, const char *name) {
    if (s_annot_n >= TP_MAX_ANNOT) return;
    tp_annot *a = &s_annot[s_annot_n++];
    *a = (tp_annot){ x, y, w, h, name, 0 };
    /* An exact-rect entry live now is the content being named: bind the
     * annotation to its hash and name it. With none live the annotation
     * stays armed and binds to the next upload on the rect. */
    for (int i = 0; i < s_ent_n; i++) {
        tp_entry *e = &s_ent[i];
        if (e->alive && e->x == x && e->y == y && e->w == w && e->h == h) {
            a->thash = e->thash;
            e->asset = name;
        }
    }
    if (a->thash)
        fprintf(stderr, "[texpack] annotate %s: bound to thash=%016llx "
                "(%d,%d %dx%d, live entry)\n", name,
                (unsigned long long)a->thash, x, y, w, h);
    else
        fprintf(stderr, "[texpack] annotate %s: armed for the next upload on "
                "%d,%d %dx%d\n", name, x, y, w, h);
}
void texture_pack_annotate(int x, int y, int w, int h, const char *name) { SDL_LockMutex(reg_mx()); texture_pack_annotate_locked(x, y, w, h, name); SDL_UnlockMutex(s_reg_mx); }


/* ---- resolve -------------------------------------------------------------- */

/// @brief Return the first live entry whose rect contains the rect (x, y, w, h), or NULL.
static const tp_entry *tp_cover(int x, int y, int w, int h)
{
    for (int i = 0; i < s_ent_n; i++) {
        const tp_entry *e = &s_ent[i];
        if (e->alive && x >= e->x && y >= e->y &&
            x + w <= e->x + e->w && y + h <= e->y + e->h) return e;
    }
    return NULL;
}

/** @brief Hash the full CLUT row a texture's bpp addresses, read from the VRAM model.
 *
 * XXH3 over 16 entries for 4bpp and 256 for 8bpp, starting at
 * (clut_x, clut_y); 16bpp has no palette and returns 0. The row is not
 * trimmed to the indices the query box uses: the used range depends on
 * the box, and each clipped triangle of one tile scans a different range,
 * so a trimmed hash gives one tile several identities and containment
 * matching fails. Entries past the used range are stable because the VRAM
 * model is written only by the game's uploads, which are deterministic.
 * The row is read through openpete_vram_read: the present thread hashes the
 * composed image of the scene it draws, never the model the guest writes.
 * @param bpp Texture depth: 4, 8 or 16.
 * @param clut_x CLUT X in VRAM halfwords.
 * @param clut_y CLUT row in VRAM.
 * @return The palette hash, 0 for 16bpp. */
static uint64_t tp_clut_hash(int bpp, int clut_x, int clut_y)
{
    if (bpp == 16) return 0;
    const uint16_t *vr = openpete_vram_read();
    const uint16_t *clut = vr + (size_t)clut_y * OPENPETE_VRAM_W + clut_x;
    return XXH3_64bits(clut, (bpp == 8 ? 256 : 16) * 2);
}

/** @brief Body of texture_pack_resolve; the caller holds the registry lock.
 *
 * Returns 0 before init or for an inverted box. Otherwise it looks up a
 * direct-mapped slot of the calling thread's VRAM image's cache, keyed on
 * the packed material and box, valid for the current s_gen and image
 * version, including negative results. On a miss it maps the box
 * to VRAM halfword columns by bpp, finds a live entry containing them
 * (tp_cover), and fills out from that entry's hash, the CLUT hash and the
 * box relative to the upload origin. */
static int texture_pack_resolve_locked(int tpage_x, int tpage_y, int clut_x, int clut_y,
                         int bpp, int u0, int v0, int u1, int v1,
                         texture_pack_id *out) {
    if (!s_inited || u1 < u0 || v1 < v0) return 0;
    tp_poll_modset();
    /* Cache key: material + box XOR-folded into one word. The fields
     * overlap, so distinct inputs can share a key; a slot hit compares the
     * key and the generation only. */
    uint64_t key = ((uint64_t)(uint16_t)tpage_x << 48) ^ ((uint64_t)(uint16_t)tpage_y << 32) ^
                   ((uint64_t)(uint16_t)clut_x  << 16) ^  (uint64_t)(uint16_t)clut_y ^
                   ((uint64_t)(uint8_t)bpp << 56) ^
                   ((uint64_t)(uint8_t)u0 << 8) ^ ((uint64_t)(uint8_t)v0 << 16) ^
                   ((uint64_t)(uint8_t)(u1 - u0) << 24) ^ ((uint64_t)(uint8_t)(v1 - v0) << 40);
    if (!key) key = 1;
    uint32_t ver;
    const int view = openpete_vram_read_view(&ver);
    tp_slot *sl = &s_cache[view][(key * 0x9E3779B97F4A7C15ull >> 32) & (TP_RESOLVE_SLOTS - 1)];
    if (sl->key == key && sl->gen == s_gen && sl->ver == ver) {
        if (!sl->hit) return 0;
        *out = sl->id; return 1;
    }
    sl->key = key; sl->gen = s_gen; sl->ver = ver; sl->hit = 0;

    /* The VRAM columns the texel box occupies depend on bpp packing. */
    int px0 = tpage_x, px1 = tpage_x;
    if (bpp == 16)     { px0 += u0;      px1 += u1; }
    else if (bpp == 8) { px0 += u0 >> 1; px1 += u1 >> 1; }
    else               { px0 += u0 >> 2; px1 += u1 >> 2; }
    const tp_entry *e = tp_cover(px0, tpage_y + v0, px1 - px0 + 1, v1 - v0 + 1);
    if (!e) return 0;

    texture_pack_id id;
    id.thash = e->thash;
    id.phash = tp_clut_hash(bpp, clut_x, clut_y);
    id.bpp   = bpp;
    /* Box relative to the upload origin, in TEXEL units of this bpp. */
    int shift = (bpp == 16) ? 0 : (bpp == 8) ? 1 : 2;
    id.box_x = ((tpage_x - e->x) << shift) + u0;   /* page origin offset + u */
    id.box_y = (tpage_y - e->y) + v0;
    id.box_w = u1 - u0 + 1;
    id.box_h = v1 - v0 + 1;
    id.up_w  = e->w << shift;
    id.up_h  = e->h;
    id.asset = e->asset;
    sl->id = id; sl->hit = 1;
    *out = id;
    return 1;
}
int texture_pack_resolve(int tpage_x, int tpage_y, int clut_x, int clut_y,
                         int bpp, int u0, int v0, int u1, int v1,
                         texture_pack_id *out) { SDL_LockMutex(reg_mx()); int r_ = texture_pack_resolve_locked(tpage_x, tpage_y, clut_x, clut_y, bpp, u0, v0, u1, v1, out); SDL_UnlockMutex(s_reg_mx); return r_; }


/* ---- identity key (shared: census + replacement lookup) ------------------- */

/** @brief Mix thash, phash, box and bpp into a nonzero identity key.
 *
 * Keys the match memo, the category census and texture_pack_id_hashkey.
 * The mix is not collision-free, and both tables compare only the key. */
static uint64_t tp_id_key(const texture_pack_id *id)
{
    uint64_t key = id->thash ^ (id->phash * 0x9E3779B97F4A7C15ull) ^
                   ((uint64_t)(uint32_t)((id->box_x << 20) ^ (id->box_y << 8) ^
                                         (id->box_w << 26) ^ id->box_h ^ id->bpp) << 1);
    return key ? key : 1;
}

uint64_t texture_pack_id_hashkey(const texture_pack_id *id) { return tp_id_key(id); }

/* ---- replacement lookup --------------------------------------------------- */

/// @brief Body of texture_pack_match; the caller holds the registry lock.
static int texture_pack_match_locked(const texture_pack_id *id, texture_pack_repl *out) {
    if (!id) return 0;
    tp_poll_modset(); /* before the active check: a late root activates */
    if (!s_repl_active) return 0;
    uint64_t key = tp_id_key(id);
    uint64_t base = key * 0xff51afd7ed558ccdull >> 32;
    tp_match_slot *m = NULL;
    for (int p = 0; p < 16; p++) {
        size_t slots = mod_fvec_count(&s_match);
        tp_match_slot *c;
        if (!slots) break;                      /* memo unavailable */
        c = (tp_match_slot*)mod_fvec_at(&s_match,
                                        (size_t)(base + (uint64_t)p) & (slots - 1));
        if (c->key == key || c->key == 0) { m = c; break; }
    }
    int ei;
    if (m && m->key == key && m->gen == s_gen) {
        ei = m->entry;                          /* memoized (incl. -1 = no match) */
    } else {
        /* Symbolic first, since the asset name is the primary identity: an
         * annotated upload whose name matches a <asset>.png entry. Falls
         * back to hash containment:
         * same thash/bpp/phash, file box contains the query box; smallest
         * containing file wins. Only sources whose owning mod is enabled
         * serve; same-identity duplicates across sources resolve to the
         * later (higher-priority) source, so toggling one off falls back
         * to the other. */
        ei = -1;
        long best = 0;
        if (id->asset) {
            for (int i = 0; i < pack_count(); i++) {
                const tp_pack_entry *pe = pack_at(i);
                if (pe->symbolic && tp_src_enabled(pe->src) &&
                    strcmp(pe->name, id->asset) == 0)
                    ei = i;                     /* keep last = later source */
            }
        }
        if (ei < 0) for (int i = 0; i < pack_count(); i++) {
            const tp_pack_entry *e = pack_at(i);
            if (e->symbolic) continue;
            if (!tp_src_enabled(e->src)) continue;
            if (e->thash != id->thash || e->bpp != id->bpp || e->phash != id->phash)
                continue;
            if (id->box_x < e->bx || id->box_y < e->by ||
                id->box_x + id->box_w > e->bx + e->bw ||
                id->box_y + id->box_h > e->by + e->bh) continue;
            long area = (long)e->bw * e->bh;
            if (ei < 0 || area < best ||
                (area == best && i > ei)) { ei = i; best = area; }
        }
        if (m) { m->key = key; m->gen = s_gen; m->entry = ei; }
    }
    if (ei < 0) return 0;
    tp_pack_entry *e = pack_at(ei);
    if (!e) return 0;
    /* Not resident yet: ask for it and draw the base texture on this draw
     * rather than blocking the tick on a disk read. */
    if (SDL_GetAtomicInt(&e->px_state) != TP_PX_READY) {
        tp_decode_request(ei, 1);
        if (SDL_GetAtomicInt(&e->px_state) != TP_PX_READY) return 0;
    }
    SDL_MemoryBarrierAcquire();                 /* pairs with the worker */
    if (!e->px) return 0;
    out->pixels = e->px; out->w = e->w; out->h = e->h;
    s_served[e->symbolic != 0]++;
    if (e->symbolic) {
        /* A symbolic file covers the FULL upload in this draw's bpp texels. */
        out->box_x = 0; out->box_y = 0;
        out->box_w = id->up_w; out->box_h = id->up_h;
    } else {
        out->box_x = e->bx; out->box_y = e->by;
        out->box_w = e->bw; out->box_h = e->bh;
    }
    return 1;
}
int texture_pack_match(const texture_pack_id *id, texture_pack_repl *out) { SDL_LockMutex(reg_mx()); int r_ = texture_pack_match_locked(id, out); SDL_UnlockMutex(s_reg_mx); return r_; }


/** @brief Body of texture_pack_overlaps; the caller holds the registry lock.
 *
 * Returns 0 for a NULL id or an inactive pack. Otherwise it returns 1 when
 * an enabled symbolic file has the id's asset name, or an enabled hash file
 * matches thash, bpp and phash with a box that intersects the id's box. */
static int texture_pack_overlaps_locked(const texture_pack_id *id) {
    if (!id) return 0;
    tp_poll_modset(); /* a late root / toggle can activate the pack */
    if (!s_repl_active) return 0;
    for (int i = 0; i < pack_count(); i++) {
        const tp_pack_entry *e = pack_at(i);
        if (!tp_src_enabled(e->src)) continue;
        if (e->symbolic) {
            if (id->asset && strcmp(e->name, id->asset) == 0) return 1;
            continue;
        }
        if (e->thash != id->thash || e->bpp != id->bpp || e->phash != id->phash)
            continue;
        if (id->box_x < e->bx + e->bw && id->box_x + id->box_w > e->bx &&
            id->box_y < e->by + e->bh && id->box_y + id->box_h > e->by)
            return 1;
    }
    return 0;
}
int texture_pack_overlaps(const texture_pack_id *id) { SDL_LockMutex(reg_mx()); int r_ = texture_pack_overlaps_locked(id); SDL_UnlockMutex(s_reg_mx); return r_; }


/* ---- pack fog shift (see header) ------------------------------------------ */

static int   s_fogshift_on;
static float s_fogshift_s[3], s_fogshift_o[3];

void texture_pack_fog_shift_set(const float scale_rgb[3], const float offset_rgb[3])
{
    if (!scale_rgb || !offset_rgb) { s_fogshift_on = 0; return; }
    for (int c = 0; c < 3; c++) {
        s_fogshift_s[c] = scale_rgb[c];
        s_fogshift_o[c] = offset_rgb[c];
    }
    s_fogshift_on = 1;
}

int texture_pack_fog_shift_get(float scale_rgb[3], float offset_rgb[3])
{
    if (!s_fogshift_on) return 0;
    for (int c = 0; c < 3; c++) {
        scale_rgb[c] = s_fogshift_s[c];
        offset_rgb[c] = s_fogshift_o[c];
    }
    return 1;
}

/* ---- terrain fog-ladder ramp (see header) --------------------------------- */

static int   s_fogladder_on;
static float s_fogladder_ts, s_fogladder_tb;

void texture_pack_fog_ladder_set(float t_scale, float t_bias)
{
    s_fogladder_ts = t_scale;
    s_fogladder_tb = t_bias;
    s_fogladder_on = (t_scale != 0.0f || t_bias != 0.0f);
}

int texture_pack_fog_ladder_get(float *t_scale, float *t_bias)
{
    if (!s_fogladder_on) return 0;
    *t_scale = s_fogladder_ts;
    *t_bias  = s_fogladder_tb;
    return 1;
}

unsigned texture_pack_generation(void) {
    /* Poll here too: generation-latched callers (the terrain cache's claim
     * scopes) may see no resolve/match traffic at all while everything is
     * unclaimed, so without this a mid-session mod toggle would never fold
     * into the generation they key on. */
    tp_poll_modset();
    return s_gen;
}

/* ---- per-texture stock->pack grade fit (see header) ----------------------- */

typedef struct {
    uint64_t key; unsigned gen;
    uint32_t ver;                        /* VRAM image version (openpete_vram_read_view) */
    float p[3], q[3];
    uint8_t state;                       /* 0 empty, 1 fit, 2 no-signal */
} tp_fit_slot;
#define TP_FIT_SLOTS 512
static tp_fit_slot s_fit[2][TP_FIT_SLOTS];   /* per VRAM image, as s_cache */

/** @brief Return the 16-bit VRAM word a texel (u, v) of a texture page samples.
 *
 * 16bpp reads the page word directly; 8bpp and 4bpp unpack the index from
 * the page word and read the CLUT entry at (clut_x + index, clut_y). Page
 * row and column wrap at 512 and 1024. The CLUT address is not wrapped.
 * @param vr VRAM model, OPENPETE_VRAM_W halfwords per row.
 * @param tpage_x Page origin X in halfwords.
 * @param tpage_y Page origin Y.
 * @param clut_x CLUT X in halfwords.
 * @param clut_y CLUT row.
 * @param bpp 4, 8 or 16.
 * @param u Texel X within the page.
 * @param v Texel Y within the page.
 * @return The texel's 15-bit colour word. */
static uint16_t tp_vram_texel(const uint16_t *vr, int tpage_x, int tpage_y,
                              int clut_x, int clut_y, int bpp, int u, int v)
{
    int row = (tpage_y + v) & 0x1FF;
    uint16_t w;
    if (bpp == 16)
        return vr[(size_t)row * OPENPETE_VRAM_W + ((tpage_x + u) & 0x3FF)];
    if (bpp == 8) {
        w = vr[(size_t)row * OPENPETE_VRAM_W + ((tpage_x + (u >> 1)) & 0x3FF)];
        return vr[(size_t)clut_y * OPENPETE_VRAM_W + clut_x + ((w >> (8 * (u & 1))) & 0xFF)];
    }
    w = vr[(size_t)row * OPENPETE_VRAM_W + ((tpage_x + (u >> 2)) & 0x3FF)];
    return vr[(size_t)clut_y * OPENPETE_VRAM_W + clut_x + ((w >> (4 * (u & 3))) & 0xF)];
}

/** @brief Find a fit memo slot for a key: its live entry, else the slot to overwrite.
 *
 * Probes eight consecutive slots from the key's hash. A slot is live when
 * its generation is s_gen and its version is @p ver. The first live slot
 * holding the key is returned; failing that, the first empty slot, then a
 * stale slot, preferring the key's own. Stale slots must be reusable:
 * s_gen bumps on every VRAM upload, so each epoch would otherwise strand a
 * fresh slot per live key, and once all eight filled every call re-ran
 * the per-texel fit.
 * @param memo The calling thread's VRAM image's memo.
 * @param key Memo key.
 * @param ver The image version.
 * @return The slot, or NULL when all eight hold other live keys. */
static tp_fit_slot *tp_fit_probe(tp_fit_slot *memo, uint64_t key, uint32_t ver)
{
    tp_fit_slot *empty = NULL, *stale = NULL;
    const uint64_t base = key * 0xff51afd7ed558ccdull >> 32;
    for (int pr = 0; pr < 8; pr++) {
        tp_fit_slot *c = &memo[(base + (uint64_t)pr) & (TP_FIT_SLOTS - 1)];
        const int cur = c->gen == s_gen && c->ver == ver;
        if (c->key == key && cur) return c;
        if (c->state == 0) { if (!empty) empty = c; }
        /* Prefer evicting this key's own stale slot - it is the one entry in
         * the window guaranteed not to be some other texture's live fit. */
        else if (!cur && (!stale || c->key == key)) stale = c;
    }
    return empty ? empty : stale;
}

/** @brief Body of texture_pack_grade_fit; the caller holds the registry lock.
 *
 * Looks the key (pixel buffer, phash) up in the calling thread's VRAM
 * image's fit memo (tp_fit_probe), and on a miss runs the per-texel
 * regression over the file box and stores the result in the probed slot.
 * @param id Resolved identity.
 * @param repl Match result.
 * @param tpage_x Texture page X in VRAM halfwords.
 * @param tpage_y Texture page Y in VRAM rows.
 * @param clut_x CLUT X in VRAM halfwords.
 * @param clut_y The base CLUT row.
 * @param bpp 4, 8 or 16.
 * @param pu0 File box origin U in page texels.
 * @param pv0 File box origin V in page texels.
 * @param p Receives the per-channel slope.
 * @param q Receives the per-channel offset.
 * @return 1 on a fit, 0 otherwise (texture_pack_grade_fit). */
static int texture_pack_grade_fit_locked(const texture_pack_id *id, const texture_pack_repl *repl,
                                         int tpage_x, int tpage_y, int clut_x, int clut_y,
                                         int bpp, int pu0, int pv0, float p[3], float q[3])
{
    if (!repl->pixels || repl->box_w <= 0 || repl->box_h <= 0) return 0;
    uint64_t key = (uint64_t)(uintptr_t)repl->pixels
                 ^ (id->phash * 0x9E3779B97F4A7C15ull);
    uint32_t ver;
    tp_fit_slot *sl = tp_fit_probe(s_fit[openpete_vram_read_view(&ver)], key, ver);
    if (sl && sl->key == key && sl->gen == s_gen && sl->ver == ver && sl->state) {
        if (sl->state != 1) return 0;
        for (int c = 0; c < 3; c++) { p[c] = sl->p[c]; q[c] = sl->q[c]; }
        return 1;
    }
    const uint16_t *vr = openpete_vram_read();
    if (!vr) return 0;
    /* Cache miss: the per-texel regression below is the expensive path, and
     * a level's worth of first-use fits lands in one tick. */
    OP_PROF_ACCUM_SCOPE("tex.gradefit");
    double sx[3] = {0}, sy[3] = {0}, sxx[3] = {0}, sxy[3] = {0};
    int cnt = 0;
    for (int ty = 0; ty < repl->box_h; ty++) {
        int v = pv0 + ty;
        int hy = (int)(((float)ty + 0.5f) * (float)repl->h / (float)repl->box_h);
        for (int tx = 0; tx < repl->box_w; tx++) {
            uint16_t word = tp_vram_texel(vr, tpage_x, tpage_y, clut_x, clut_y,
                                          bpp, pu0 + tx, v);
            if (word == 0) continue;                 /* transparency sentinel */
            int hx = (int)(((float)tx + 0.5f) * (float)repl->w / (float)repl->box_w);
            const unsigned char *hp = repl->pixels + ((size_t)hy * repl->w + hx) * 4;
            if (hp[3] < 128) continue;               /* pack cut this texel out */
            for (int c = 0; c < 3; c++) {
                double x = (double)((word >> (c * 5)) & 0x1F) / 31.0;
                double y = (double)hp[c] / 255.0;
                sx[c] += x; sy[c] += y; sxx[c] += x * x; sxy[c] += x * y;
            }
            cnt++;
        }
    }
    int ok = cnt >= 64;
    if (ok) {
        for (int c = 0; c < 3; c++) {
            double den = cnt * sxx[c] - sx[c] * sx[c];
            if (den > 1e-6) {
                double a = (cnt * sxy[c] - sx[c] * sy[c]) / den;
                p[c] = (float)a;
                q[c] = (float)((sy[c] - a * sx[c]) / cnt);
            } else {                                 /* flat stock: offset model */
                p[c] = 1.0f;
                q[c] = (float)((sy[c] - sx[c]) / cnt);
            }
        }
    }
    if (sl) {
        sl->key = key; sl->gen = s_gen; sl->ver = ver; sl->state = ok ? 1 : 2;
        if (ok) for (int c = 0; c < 3; c++) { sl->p[c] = p[c]; sl->q[c] = q[c]; }
    }
    return ok;
}
int texture_pack_grade_fit(const texture_pack_id *id, const texture_pack_repl *repl,
                           int tpage_x, int tpage_y, int clut_x, int clut_y,
                           int bpp, int pu0, int pv0, float p[3], float q[3])
{
    SDL_LockMutex(reg_mx());
    const int r = texture_pack_grade_fit_locked(id, repl, tpage_x, tpage_y, clut_x, clut_y,
                                                bpp, pu0, pv0, p, q);
    SDL_UnlockMutex(s_reg_mx);
    return r;
}

/* ---- identity/provenance census (temporary instrument) -------------------- */

#define TP_CENSUS_SLOTS 16384

typedef struct {
    uint64_t key;          /* full identity (tp_id_key) */
    uint64_t thash, phash;
    int      bx, by, bw, bh, bpp;
    uint32_t cat_mask;     /* bit per OPENPETE_SCENE_* category */
    uint32_t tag;          /* first provenance tag seen */
    int      tag_mixed;    /* a second distinct tag was seen */
    uint64_t draws;
} tp_census_ent;

static tp_census_ent *s_census;
static int s_census_on = -1;
static int s_census_full;

/** @brief Return the name of scene category c, or a placeholder outside the table.
 *
 * The placeholder is the string "?". */
static const char *tp_cat_name(int c)
{
    static const char *n[] = { "WORLD", "ACTOR", "HUD", "PLAYER", "OVERLAY",
                               "HUD_PANEL", "HUD3D", "PORTAL_CYCLORAMA",
                               "PORTAL_MASK", "PORTAL_GUARD", "SKY",
                               "WORLD_FILL", "SHADOW", "ACTOR_FRONT" };
    return (c >= 0 && c < (int)(sizeof n / sizeof *n)) ? n[c] : "?";
}

/** @brief Key a census entry by its texel box, ignoring the CLUT.
 *
 * XORs the texel hash with a shifted mix of the box origin, size and bpp.
 * tp_census_report groups entries with equal keys to count how many
 * distinct palettes (fog ladder rows) share one texture. Distinct boxes
 * can collide under the XOR mix; the census is an instrument and tolerates
 * a merged group.
 * @param e Census entry.
 * @return The box key. */
static uint64_t tp_box_key(const tp_census_ent *e)
{
    return e->thash ^ ((uint64_t)(uint32_t)((e->bx << 20) ^ (e->by << 8) ^
                                            (e->bw << 26) ^ e->bh ^ e->bpp) << 1);
}

/** @brief Print the category-census summary to stderr; registered with atexit.
 *
 * Reports the identity count, per-category identity and draw counts, the
 * identities drawn under more than one category or tag (first 20 listed),
 * and the CLUT fan-out: distinct palettes per texel box, with the first 20
 * boxes that have more than one. Does nothing when the census table was
 * never allocated. */
static void tp_census_report(void)
{
    if (!s_census) return;
    int n = 0, multi_cat = 0, multi_tag = 0;
    for (int i = 0; i < TP_CENSUS_SLOTS; i++) if (s_census[i].key) n++;
    fprintf(stderr, "\n[texpack/census] %d distinct identities%s\n",
            n, s_census_full ? " (TABLE FULL - counts truncated)" : "");

    /* Per-category coverage. Without this a "0 multi-category" result is
     * ambiguous: it can mean no overlap, or that only one category ever ran. */
    {
        int cat_ids[32] = {0};
        uint64_t cat_draws[32] = {0};
        for (int i = 0; i < TP_CENSUS_SLOTS; i++) {
            tp_census_ent *e = &s_census[i];
            if (!e->key) continue;
            for (int c = 0; c < 32; c++)
                if (e->cat_mask & (1u << c)) { cat_ids[c]++; cat_draws[c] += e->draws; }
        }
        fprintf(stderr, "[texpack/census] category coverage:\n");
        for (int c = 0; c < 32; c++)
            if (cat_ids[c])
                fprintf(stderr, "    %-18s %4d identities, %llu draws\n",
                        tp_cat_name(c), cat_ids[c], (unsigned long long)cat_draws[c]);
    }

    /* Is any single identity drawn under more than one category? */
    for (int i = 0; i < TP_CENSUS_SLOTS; i++) {
        tp_census_ent *e = &s_census[i];
        if (!e->key) continue;
        int bits = 0;
        for (int c = 0; c < 32; c++) if (e->cat_mask & (1u << c)) bits++;
        if (bits > 1) {
            multi_cat++;
            if (multi_cat <= 20) {
                fprintf(stderr, "  MULTI-CAT %016llx %dx%d+%dx%d %dbpp draws=%llu cats:",
                        (unsigned long long)e->thash, e->bx, e->by, e->bw, e->bh,
                        e->bpp, (unsigned long long)e->draws);
                for (int c = 0; c < 32; c++)
                    if (e->cat_mask & (1u << c)) fprintf(stderr, " %s", tp_cat_name(c));
                fprintf(stderr, "\n");
            }
        }
        if (e->tag_mixed) multi_tag++;
    }
    fprintf(stderr, "[texpack/census] identities drawn under >1 category: %d\n", multi_cat);
    fprintf(stderr, "[texpack/census] identities drawn under >1 tag:      %d\n", multi_tag);

    /* CLUT fan-out - distinct palettes per texel box, and whether the
     * variants of one box ever span categories. */
    int boxes = 0, worst = 0, fan_total = 0;
    for (int i = 0; i < TP_CENSUS_SLOTS; i++) {
        if (!s_census[i].key) continue;
        uint64_t bk = tp_box_key(&s_census[i]);
        int seen_earlier = 0, fan = 0;
        uint32_t union_cat = 0;
        for (int j = 0; j < TP_CENSUS_SLOTS; j++) {
            if (!s_census[j].key || tp_box_key(&s_census[j]) != bk) continue;
            if (j < i) { seen_earlier = 1; break; }
            fan++; union_cat |= s_census[j].cat_mask;
        }
        if (seen_earlier) continue;
        boxes++; fan_total += fan;
        if (fan > worst) worst = fan;
        if (fan > 1 && boxes <= 20) {
            fprintf(stderr, "  CLUT-FANOUT %016llx %dx%d+%dx%d %dbpp -> %d palettes cats:",
                    (unsigned long long)s_census[i].thash, s_census[i].bx, s_census[i].by,
                    s_census[i].bw, s_census[i].bh, s_census[i].bpp, fan);
            for (int c = 0; c < 32; c++)
                if (union_cat & (1u << c)) fprintf(stderr, " %s", tp_cat_name(c));
            fprintf(stderr, "\n");
        }
    }
    fprintf(stderr, "[texpack/census] %d distinct texel boxes, %d identities "
                    "(avg %.2f palettes/box, worst %d)\n",
            boxes, n, boxes ? (double)fan_total / boxes : 0.0, worst);
}

void texture_pack_cat_census(const texture_pack_id *id, int category, unsigned tag)
{
    if (s_census_on < 0) {
        const char *e = getenv("OPENPETE_TEXPACK_CATCENSUS");
        s_census_on = (e && *e && *e != '0');
        if (s_census_on) {
            s_census = calloc(TP_CENSUS_SLOTS, sizeof *s_census);
            if (!s_census) s_census_on = 0;
            else atexit(tp_census_report);
        }
    }
    if (!s_census_on || !id) return;

    uint64_t key = tp_id_key(id);
    uint64_t base = key * 0xff51afd7ed558ccdull >> 32;
    for (int p = 0; p < 32; p++) {
        tp_census_ent *e = &s_census[(base + (uint64_t)p) & (TP_CENSUS_SLOTS - 1)];
        if (e->key == 0) {
            e->key = key; e->thash = id->thash; e->phash = id->phash;
            e->bx = id->box_x; e->by = id->box_y;
            e->bw = id->box_w; e->bh = id->box_h; e->bpp = id->bpp;
            e->tag = tag;
        } else if (e->key != key) {
            continue;
        }
        if (category >= 0 && category < 32) e->cat_mask |= 1u << category;
        if (tag != e->tag) e->tag_mixed = 1;
        e->draws++;
        return;
    }
    s_census_full = 1;
}

/* ---- dump tool ------------------------------------------------------------ */


/* Dump granularity: per-tri query boxes explode into clipped variants
 * (11k files/run measured), so boxes accumulate rather than dumping per draw.
 * Accumulation is per CLUSTER, not per (thash, phash, bpp): a single bounding
 * box over everything sharing a palette spans DISJOINT tiles and swallows
 * whatever page content lies between them (a 160x64 dump holding grass +
 * foliage + rock). Boxes therefore merge only on strict OVERLAP, transitively
 * as new ones arrive, and each connected component writes its own file.
 * Clipped fragments of one tile still coalesce - they all overlap the eventual
 * full-tile draw - and the LP full-tile box still absorbs its HP 2x2 sub-quad
 * boxes, which containment matching depends on. Adjacent-but-disjoint tiles
 * stay separate. */
#define TP_UNION_SLOTS 16384   /* power of 2 */
#define TP_UNION_PROBE 32      /* clusters sharing a key live within this window */
typedef struct {
    uint64_t key;
    int bx, by, bw, bh;        /* cluster box, upload-relative texels */
    char name[96];             /* last written filename (unlinked on growth) */
} tp_union;
static tp_union s_union[TP_UNION_SLOTS];

/** @brief Remove a cluster's written file from dir and free its slot.
 *
 * Freeing a slot mid-window is safe because every lookup scans the whole
 * TP_UNION_PROBE window instead of stopping at the first empty slot. */
static void tp_union_drop(tp_union *c, const char *dir)
{
    if (c->name[0]) {
        char old[640];
        snprintf(old, sizeof old, "%s/%s", dir, c->name);
        remove(old);
    }
    c->key = 0; c->name[0] = 0;
}

/** @brief Return the file-name label for a scene category, or "other" outside the table.
 *
 * The label is organisational; the pack parser skips it by starting after
 * the first '-', so the labels are hyphen-free. They are lowercase. */
static const char *tp_cat_slug(int c)
{
    static const char *n[] = { "world", "actor", "hud", "player", "overlay",
                               "hudpanel", "hud3d", "cyclorama", "portalmask",
                               "portalguard", "sky", "worldfill", "shadow",
                               "actorfront" };
    return (c >= 0 && c < (int)(sizeof n / sizeof *n)) ? n[c] : "other";
}

void texture_pack_dump_draw(const texture_pack_id *id,
                            int tpage_x, int tpage_y, int clut_x, int clut_y,
                            int u0, int v0, int category, int sx, int sy)
{
    static const char *dir = NULL;
    static int checked = 0;
    if (!checked) { checked = 1; dir = getenv("OPENPETE_TEXPACK_DUMP_DIR"); }
    if (!dir || !*dir || !id) return;

    uint64_t key = id->thash ^ (id->phash * 0x9E3779B97F4A7C15ull) ^ (uint64_t)id->bpp;
    if (!key) key = 1;
    uint64_t base = key * 0xff51afd7ed558ccdull >> 32;

    const int qx0 = id->box_x, qy0 = id->box_y;
    const int qx1 = qx0 + id->box_w, qy1 = qy0 + id->box_h;

    /* Collect every same-key cluster the query box strictly overlaps. More
     * than one means this draw bridges them: they become one component. */
    tp_union *hit[TP_UNION_PROBE];
    int nhit = 0;
    tp_union *freeslot = NULL;
    for (int p = 0; p < TP_UNION_PROBE; p++) {
        tp_union *c = &s_union[(base + (uint64_t)p) & (TP_UNION_SLOTS - 1)];
        if (c->key == 0) { if (!freeslot) freeslot = c; continue; }
        if (c->key != key) continue;
        int cx1 = c->bx + c->bw, cy1 = c->by + c->bh;
        if (qx0 < cx1 && c->bx < qx1 && qy0 < cy1 && c->by < qy1)
            hit[nhit++] = c;
    }

    tp_union *u;
    int grew = 0;
    if (nhit == 0) {
        if (!freeslot) return;                  /* neighbourhood full */
        u = freeslot;
        u->key = key; u->name[0] = 0;
        u->bx = qx0; u->by = qy0; u->bw = id->box_w; u->bh = id->box_h;
        grew = 1;
    } else {
        u = hit[0];
        int x0 = u->bx, y0 = u->by, x1 = u->bx + u->bw, y1 = u->by + u->bh;
        if (qx0 < x0) x0 = qx0;
        if (qy0 < y0) y0 = qy0;
        if (qx1 > x1) x1 = qx1;
        if (qy1 > y1) y1 = qy1;
        for (int k = 1; k < nhit; k++) {        /* absorb the bridged clusters */
            tp_union *c = hit[k];
            if (c->bx < x0) x0 = c->bx;
            if (c->by < y0) y0 = c->by;
            if (c->bx + c->bw > x1) x1 = c->bx + c->bw;
            if (c->by + c->bh > y1) y1 = c->by + c->bh;
            tp_union_drop(c, dir);
            grew = 1;
        }
        if (x0 != u->bx || y0 != u->by || x1 - x0 != u->bw || y1 - y0 != u->bh)
            grew = 1;
        u->bx = x0; u->by = y0; u->bw = x1 - x0; u->bh = y1 - y0;
    }
    if (!grew) return;

    /* Decode the union box. This draw anchors page<->upload coords:
     * page coord of union origin = query page origin - (query - union) box
     * offset. u may walk beyond this tpage's 256-texel window - that is
     * correct: pages are x-offsets into the same VRAM rows, and the upload
     * is contiguous there. */
    int pu0 = u0 - (id->box_x - u->bx);
    int pv0 = v0 - (id->box_y - u->by);
    int W = u->bw, H = u->bh;
    const uint16_t *vr = openpete_vram_read();
    unsigned char *px = malloc((size_t)W * H * 4);
    if (!px) return;
    for (int y = 0; y < H; y++) {
        const uint16_t *row = vr + (size_t)(tpage_y + pv0 + y) * OPENPETE_VRAM_W;
        for (int x = 0; x < W; x++) {
            int uu = pu0 + x;
            uint16_t p;
            if (id->bpp == 16) {
                p = row[tpage_x + uu];
            } else if (id->bpp == 8) {
                unsigned idx = (row[tpage_x + (uu >> 1)] >> (8 * (uu & 1))) & 0xFFu;
                p = vr[(size_t)clut_y * OPENPETE_VRAM_W + clut_x + idx];
            } else {
                unsigned idx = (row[tpage_x + (uu >> 2)] >> (4 * (uu & 3))) & 0xFu;
                p = vr[(size_t)clut_y * OPENPETE_VRAM_W + clut_x + idx];
            }
            unsigned char *o = px + ((size_t)y * W + x) * 4;
            unsigned r5 = p & 31u, g5 = (p >> 5) & 31u, b5 = (p >> 10) & 31u;
            o[0] = (unsigned char)((r5 << 3) | (r5 >> 2));
            o[1] = (unsigned char)((g5 << 3) | (g5 >> 2));
            o[2] = (unsigned char)((b5 << 3) | (b5 >> 2));
            /* Alpha encoding: 0x0000 sentinel -> transparent;
             * STP-set texels -> 128 (on the HD alpha pipe, src-over at 0.5
             * reproduces the PSX (B+F)/2; the cutout pipe keeps them: 128
             * passes its 0.5 discard); plain texels -> opaque. */
            o[3] = (p == 0) ? 0 : ((p & 0x8000u) ? 128 : 255);
        }
    }
    /* Pixel-aspect correction (screen-space 2D only; see the header). Whole
     * texels replicated into sx-by-sy blocks: no source texel is lost and the
     * block edges stay aligned to texel edges, which is what preserves the
     * bit-identical round-trip. */
    int Wout = W, Hout = H;
    if (sx > 1 || sy > 1) {
        Wout = W * sx; Hout = H * sy;
        unsigned char *sp = malloc((size_t)Wout * Hout * 4);
        if (sp) {
            for (int y = 0; y < Hout; y++) {
                const unsigned char *srow = px + (size_t)(y / sy) * W * 4;
                unsigned char *drow = sp + (size_t)y * Wout * 4;
                for (int x = 0; x < Wout; x++)
                    memcpy(drow + (size_t)x * 4, srow + (size_t)(x / sx) * 4, 4);
            }
            free(px);
            px = sp;
        } else {
            Wout = W; Hout = H;                 /* out of memory: write raw */
        }
    }

    if (u->name[0]) {                           /* superseded smaller union */
        char old[640];
        snprintf(old, sizeof old, "%s/%s", dir, u->name);
        remove(old);
    }
    snprintf(u->name, sizeof u->name, "%s-%016llx-%dx%d+%dx%d-%016llx-%dbpp.png",
             tp_cat_slug(category),
             (unsigned long long)id->thash, u->bx, u->by, u->bw, u->bh,
             (unsigned long long)id->phash, id->bpp);
    char path[740];
    snprintf(path, sizeof path, "%s/%s", dir, u->name);
    (void)openpete_write_png_rgba(path, px, Wout, Hout);
    free(px);
}

void texture_pack_debug_dump(void)
{
    int live = 0;
    for (int i = 0; i < s_ent_n; i++) {
        const tp_entry *e = &s_ent[i];
        if (!e->alive) continue;
        live++;
        fprintf(stderr, "[texpack] entry %d,%d %dx%d thash=%016llx%s%s\n",
                e->x, e->y, e->w, e->h, (unsigned long long)e->thash,
                e->asset ? " asset=" : "", e->asset ? e->asset : "");
    }
    fprintf(stderr, "[texpack] registry: %d live / %d slots, gen %u\n",
            live, s_ent_n, s_gen);
}
