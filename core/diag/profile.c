/* openpete_profile.c - see openpete_profile.h.
 *
 * Streams a compact binary `.opp` trace: an interned name table plus fixed
 * 32-byte records for presents, published scenes, CPU spans, GPU zones, and
 * census counts.
 *
 * Producers are concurrent: the sim worker raises spans and closes presents
 * while the present thread renders. Every fixed record therefore carries a
 * `tid` (a small thread index interned in first-touch order) and an absolute
 * microsecond timestamp; consumers group by thread and use marker-interval
 * containment across threads. GPU zones carry the RHI frame serial of the
 * frame that submitted them, so they are self-identifying and need no shared
 * counter.
 *
 * Writer: each thread appends lock-free into its own `_Thread_local` chunk.
 * The chunk is written to the stream when it fills, at that thread's marker
 * record (a present, a published scene, or the end of a rendered frame), and
 * at close. The mutex
 * is taken once per chunk, not once per record, so the append path on the
 * pacing-critical present thread has no synchronisation point. Chunk-
 * granularity interleaving in the file carries no meaning: `tid` and absolute
 * timestamps make every record self-describing.
 *
 * All little-endian (producer and every consumer are LE x86-64 / ARM64).
 */
#include "openpete_profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

/* ---- record type tags ------------------------------------------------------ */
enum {
    OPP_REC_NAME    = 1,  /* variable length: type,u16 id,u16 len,bytes         */
    OPP_REC_PRESENT = 2,  /* fixed 32                                           */
    OPP_REC_SPAN    = 3,  /* fixed 32                                           */
    OPP_REC_GPUZ    = 4,  /* fixed 32                                           */
    OPP_REC_COUNT   = 5,  /* fixed 32                                           */
    OPP_REC_FUNC    = 6,  /* fixed 32: per-function aggregate (func trace)      */
    OPP_REC_SCENE   = 7,  /* fixed 32, PRESENT layout: a scene published (eager) */
};
#define OPP_REC_SIZE   32
#define OPP_REC_TID    28   /* u8 thread index, same offset in every fixed record */
#define OPP_MAGIC      "OPPR"
#define OPP_VERSION    3u   /* 3: OPP_REC_SCENE; PRESENT at the present itself */

/* ---- global state (mutex-guarded unless noted) ----------------------------- */
static FILE*       s_fp = NULL;
static int         s_on = 0;       /* plain load on the emitter fast path       */
static SDL_AtomicInt s_present_index;   /* written by the presenting thread, read by op_prof_count */
static uint32_t    s_scene_index = 0;     /* sim worker only */
static SDL_Mutex*  s_mu = NULL;    /* guards s_fp writes, the name table, s_threads */

#define OPP_MAX_NAMES 8192
static char* s_names[OPP_MAX_NAMES];
static int   s_name_count = 0;

#define OPP_STACK_MAX 32
struct opp_span_frame { int name_id; uint64_t t0_us; };

/* One bucket per interned name that has been accumulated since this thread's
 * last flush. Linear scan: the table holds a handful of names and the lookup
 * runs once per entry, which is cheap next to the two clock reads around it. */
#define OPP_ACCUM_MAX 64
struct opp_accum { int name_id; uint64_t first_t0_us, sum_us; uint32_t calls; };

/* Per-thread producer state. Sized so a busy frame's records fit without a
 * mid-frame flush; a full chunk flushes early and costs only one extra mutex
 * acquisition. */
#define OPP_CHUNK_CAP   (48 * 1024)
#define OPP_MAX_THREADS 16

struct opp_tls {
    int      tid;
    size_t   len;
    uint8_t  buf[OPP_CHUNK_CAP];
    struct opp_span_frame stack[OPP_STACK_MAX];
    int      sp;
    struct opp_accum accum[OPP_ACCUM_MAX];
    int      accum_count;
    uint32_t spans_since_flush;
    int      warned_overflow, warned_underflow, warned_accum_full;
};

/* Spelled `_Thread_local`: third_party/libco/settings.h defines `thread_local`
 * away to nothing unless LIBCO_MP is set. */
static _Thread_local struct opp_tls* s_tls = NULL;
static struct opp_tls* s_threads[OPP_MAX_THREADS];   /* worker-line: written once per thread at registration, under s_mu */
static int s_thread_count = 0;
static int s_warned_threads_full = 0;

/* ---- little-endian packers ------------------------------------------------- */
/// @brief Store v at p as two little-endian bytes.
static inline void le_u16(uint8_t* p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
/// @brief Store v at p as four little-endian bytes.
static inline void le_u32(uint8_t* p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
/// @brief Store v at p as eight little-endian bytes, low word first.
static inline void le_u64(uint8_t* p, uint64_t v) {
    le_u32(p, (uint32_t)v); le_u32(p+4, (uint32_t)(v>>32));
}

/* ---- clock ----------------------------------------------------------------- */
static double s_us_per_tick = 0.0;   /* set in op_prof_open, before any producer */

uint64_t op_prof_now_us(void) {
    double k = s_us_per_tick;
    if (k == 0.0) k = 1e6 / (double)SDL_GetPerformanceFrequency();
    return (uint64_t)((double)SDL_GetPerformanceCounter() * k);
}

int op_prof_on(void) { return s_on; }

/* ---- per-thread state ------------------------------------------------------ */
/** @brief Return the calling thread's producer block, registering one on first use.
 *
 * Registration takes the writer mutex and assigns the next thread index.
 * Blocks live for the process and are never freed: a producer thread may
 * still hold its pointer when the trace closes, and the cost is bounded by
 * OPP_MAX_THREADS chunks. Returns NULL before the first op_prof_open, when
 * OPP_MAX_THREADS threads are registered (warned once), or when allocation
 * fails. */
static struct opp_tls* tls_get(void) {
    struct opp_tls* t = s_tls;
    if (t) return t;
    if (!s_mu) return NULL;
    SDL_LockMutex(s_mu);
    if (s_thread_count >= OPP_MAX_THREADS) {
        if (!s_warned_threads_full) {
            s_warned_threads_full = 1;
            fprintf(stderr, "[profile] more than %d producer threads; dropping records\n",
                    OPP_MAX_THREADS);
        }
        SDL_UnlockMutex(s_mu);
        return NULL;
    }
    t = (struct opp_tls*)calloc(1, sizeof(*t));
    if (!t) { SDL_UnlockMutex(s_mu); return NULL; }
    t->tid = s_thread_count;
    s_threads[s_thread_count++] = t;
    SDL_UnlockMutex(s_mu);
    s_tls = t;
    return t;
}

/** @brief Write the thread's chunk to the stream under the writer mutex and empty it.
 *
 * The append path never takes a lock; this write is the only
 * synchronisation per chunk. The bytes are dropped when no stream is
 * open. */
static void chunk_flush(struct opp_tls* t) {
    if (!t->len) return;
    SDL_LockMutex(s_mu);
    if (s_fp) fwrite(t->buf, 1, t->len, s_fp);
    SDL_UnlockMutex(s_mu);
    t->len = 0;
}

/** @brief Append n bytes to the thread's chunk, flushing first when they do not fit.
 *
 * The record is dropped when it still does not fit after the flush, which
 * happens only when n exceeds OPP_CHUNK_CAP; the record writers here pass
 * OPP_REC_SIZE bytes. */
static void chunk_put(struct opp_tls* t, const void* p, size_t n) {
    if (t->len + n > OPP_CHUNK_CAP) chunk_flush(t);
    if (t->len + n > OPP_CHUNK_CAP) return;
    memcpy(t->buf + t->len, p, n);
    t->len += n;
}

/* ---- lifecycle ------------------------------------------------------------- */
int op_prof_open(const char* path) {
    if (!path || !*path) return 0;
    if (!s_mu) s_mu = SDL_CreateMutex();   /* never destroyed: see op_prof_close */
    if (!s_mu) { fprintf(stderr, "[profile] mutex create failed\n"); return -1; }
    s_fp = fopen(path, "wb");
    if (!s_fp) {
        fprintf(stderr, "[profile] fopen %s failed\n", path);
        return -1;
    }
    static char buf[256 * 1024];
    setvbuf(s_fp, buf, _IOFBF, sizeof(buf));

    uint8_t hdr[OPP_REC_SIZE];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, OPP_MAGIC, 4);
    le_u32(hdr + 4, OPP_VERSION);
    le_u64(hdr + 8, 1000000u);   /* clock_hz: timestamps are already µs */
    le_u64(hdr + 16, 0u);        /* created_unix_ns: Date unavailable here */
    fwrite(hdr, 1, sizeof(hdr), s_fp);

    s_us_per_tick = 1e6 / (double)SDL_GetPerformanceFrequency();
    SDL_SetAtomicInt(&s_present_index, 0);
    s_scene_index = 0;
    s_name_count = 0;
    /* Reuse the registered blocks so a live s_tls pointer stays valid; no other
     * producer can be running, since s_on is still 0. */
    SDL_LockMutex(s_mu);
    for (int i = 0; i < s_thread_count; i++) {
        struct opp_tls* t = s_threads[i];
        t->len = 0; t->sp = 0; t->accum_count = 0; t->spans_since_flush = 0;
    }
    SDL_UnlockMutex(s_mu);
    s_on = 1;
    fprintf(stderr, "[profile] tracing to %s\n", path);
    return 0;
}

static void accum_flush(struct opp_tls* t);

void op_prof_close(void) {
    if (!s_fp) return;
    s_on = 0;   /* producers past this point return before touching any state */
    SDL_LockMutex(s_mu);
    /* Every producer's pending buckets and chunk, not only the closing
     * thread's: each still holds what it raised after its last marker. */
    for (int i = 0; i < s_thread_count; i++) {
        struct opp_tls* t = s_threads[i];
        accum_flush(t);
        if (t->len) { fwrite(t->buf, 1, t->len, s_fp); t->len = 0; }
    }
    fflush(s_fp);
    fclose(s_fp);
    s_fp = NULL;
    for (int i = 0; i < s_name_count; i++) { free(s_names[i]); s_names[i] = NULL; }
    s_name_count = 0;
    SDL_UnlockMutex(s_mu);
    /* s_mu is kept: op_prof_intern and the emitters may still be entered from
     * another thread, and locking a destroyed mutex is undefined. */
}

/* ---- name interning -------------------------------------------------------- */
int op_prof_intern(const char* name) {
    if (!s_on || !name || !s_mu) return -1;
    SDL_LockMutex(s_mu);
    int id = -1;
    for (int i = 0; i < s_name_count; i++)
        if (strcmp(s_names[i], name) == 0) { id = i; break; }
    if (id < 0 && s_name_count < OPP_MAX_NAMES && s_fp) {
        id = s_name_count++;
        s_names[id] = strdup(name);
        size_t len = strlen(name);
        if (len > 65535) len = 65535;
        uint8_t hdr[5];
        hdr[0] = OPP_REC_NAME;
        le_u16(hdr + 1, (uint16_t)id);
        le_u16(hdr + 3, (uint16_t)len);
        /* Written straight to the stream, not into a chunk: the NAME record
         * must precede every record that cites the id, and a chunk carrying
         * such a record is always flushed later, under this same mutex. */
        fwrite(hdr, 1, sizeof(hdr), s_fp);
        fwrite(name, 1, len, s_fp);
    }
    SDL_UnlockMutex(s_mu);
    return id;
}

/* ---- CPU spans ------------------------------------------------------------- */
void op_prof_begin(int name_id) {
    if (!s_on) return;
    struct opp_tls* t = tls_get();
    if (!t) return;
    if (t->sp >= OPP_STACK_MAX) {
        if (!t->warned_overflow) { t->warned_overflow = 1;
            fprintf(stderr, "[profile] span stack overflow on tid %d (depth>%d); dropping\n",
                    t->tid, OPP_STACK_MAX); }
        return;
    }
    t->stack[t->sp].name_id = name_id;
    t->stack[t->sp].t0_us   = op_prof_now_us();
    t->sp++;
}

/** @brief Append one span record to the thread's chunk.
 *
 * The record carries the depth byte, the name id truncated to 16 bits, the
 * start time, the duration truncated to 32 bits, aux, and the thread index
 * at OPP_REC_TID, and counts toward the thread's spans since the last flush
 * marker. */
static void emit_span(struct opp_tls* t, int depth, int name_id,
                      uint64_t t0_us, uint32_t dur_us, uint32_t aux) {
    uint8_t r[OPP_REC_SIZE];
    memset(r, 0, sizeof(r));
    r[0] = OPP_REC_SPAN;
    r[1] = (uint8_t)depth;
    le_u16(r + 2, (uint16_t)name_id);
    le_u64(r + 8, t0_us);
    le_u32(r + 16, dur_us);
    le_u32(r + 20, aux);
    r[OPP_REC_TID] = (uint8_t)t->tid;
    chunk_put(t, r, sizeof(r));
    t->spans_since_flush++;
}

void op_prof_end(void) {
    if (!s_on) return;
    struct opp_tls* t = tls_get();
    if (!t) return;
    if (t->sp <= 0) {
        if (!t->warned_underflow) { t->warned_underflow = 1;
            fprintf(stderr, "[profile] unbalanced op_prof_end on tid %d; ignoring\n", t->tid); }
        return;
    }
    t->sp--;
    uint64_t now = op_prof_now_us();
    uint64_t t0  = t->stack[t->sp].t0_us;
    emit_span(t, t->sp, t->stack[t->sp].name_id, t0, (uint32_t)(now - t0), 0);
}

void op_prof_add_span(int name_id, uint64_t dur_us) {
    if (!s_on || name_id < 0) return;
    struct opp_tls* t = tls_get();
    if (!t) return;
    uint64_t now = op_prof_now_us();
    uint64_t t0  = (dur_us <= now) ? (now - dur_us) : now;
    /* depth = current stack depth so a phase span forwarded mid-scope nests
     * correctly under whatever is open (normally depth 0). */
    emit_span(t, t->sp, name_id, t0, (uint32_t)dur_us, 0);
}

/* ---- accumulating scopes --------------------------------------------------- */
void op_prof_accum(int name_id, uint64_t us) {
    if (!s_on || name_id < 0) return;
    struct opp_tls* t = tls_get();
    if (!t) return;
    struct opp_accum* a = NULL;
    for (int i = 0; i < t->accum_count; i++)
        if (t->accum[i].name_id == name_id) { a = &t->accum[i]; break; }
    if (!a) {
        if (t->accum_count >= OPP_ACCUM_MAX) {
            if (!t->warned_accum_full) { t->warned_accum_full = 1;
                fprintf(stderr, "[profile] accum table full on tid %d (>%d names); dropping\n",
                        t->tid, OPP_ACCUM_MAX); }
            return;
        }
        a = &t->accum[t->accum_count++];
        a->name_id = name_id; a->sum_us = 0; a->calls = 0;
        /* Anchor at this flush interval's first entry, not at flush time: the
         * emitted interval then lands inside the region the entries ran in. */
        a->first_t0_us = op_prof_now_us() - us;
    }
    a->sum_us += us;
    a->calls++;
}

/** @brief Emit one depth-0 span per bucket with a nonzero sum and empty the bucket table.
 *
 * Each span starts at the bucket's first entry, lasts the summed time, and
 * carries the call count in aux. Called at the thread's marker (a present,
 * a published scene, the end of a rendered frame) and at close. */
static void accum_flush(struct opp_tls* t) {
    for (int i = 0; i < t->accum_count; i++) {
        struct opp_accum* a = &t->accum[i];
        if (a->sum_us) emit_span(t, 0, a->name_id, a->first_t0_us,
                                 (uint32_t)a->sum_us, a->calls);
    }
    t->accum_count = 0;
}

void op_prof_thread_flush(void) {
    if (!s_on) return;
    struct opp_tls* t = tls_get();
    if (!t) return;
    accum_flush(t);
    chunk_flush(t);
}

/* ---- census counts --------------------------------------------------------- */
void op_prof_count(int name_id, uint64_t value) {
    if (!s_on || name_id < 0) return;
    struct opp_tls* t = tls_get();
    if (!t) return;
    uint8_t r[OPP_REC_SIZE];
    memset(r, 0, sizeof(r));
    r[0] = OPP_REC_COUNT;
    le_u16(r + 2, (uint16_t)name_id);
    le_u32(r + 4, (uint32_t)SDL_GetAtomicInt(&s_present_index));   /* informational: the reader bins by thread */
    le_u64(r + 8, value);
    r[OPP_REC_TID] = (uint8_t)t->tid;
    chunk_put(t, r, sizeof(r));
}

/* ---- GPU zones ------------------------------------------------------------- */
void op_prof_gpu_zone(int name_id, int depth, uint64_t frame_serial,
                      uint32_t t0_rel_us, uint32_t dur_us) {
    if (!s_on || name_id < 0) return;
    struct opp_tls* t = tls_get();
    if (!t) return;
    uint8_t r[OPP_REC_SIZE];
    memset(r, 0, sizeof(r));
    r[0] = OPP_REC_GPUZ;
    r[1] = (uint8_t)depth;
    le_u16(r + 2, (uint16_t)name_id);
    le_u32(r + 8, t0_rel_us);
    le_u32(r + 12, dur_us);
    le_u64(r + 16, frame_serial);
    r[OPP_REC_TID] = (uint8_t)t->tid;
    chunk_put(t, r, sizeof(r));
}

/* ---- per-function aggregate (function-trace builds) ------------------------ */
void op_prof_func(int name_id, uint32_t addr, uint64_t calls,
                  uint64_t self_us, uint64_t total_us) {
    if (!s_on || name_id < 0) return;
    struct opp_tls* t = tls_get();
    if (!t) return;
    uint8_t r[OPP_REC_SIZE];
    memset(r, 0, sizeof(r));
    r[0] = OPP_REC_FUNC;
    le_u16(r + 2, (uint16_t)name_id);
    le_u32(r + 4, addr);
    le_u64(r + 8, self_us);
    le_u64(r + 16, total_us);
    le_u32(r + 24, (uint32_t)(calls > 0xFFFFFFFFu ? 0xFFFFFFFFu : calls));
    r[OPP_REC_TID] = (uint8_t)t->tid;
    chunk_put(t, r, sizeof(r));
}

/* ---- markers: present, scene ---------------------------------------------- */
/** @brief Flush the thread's buckets, append one PRESENT-layout marker record, and flush its chunk.
 *
 * The buckets are emitted first, so they precede the marker they bind to in
 * the thread's stream. The record carries the type, kind, index, canon_tick,
 * the thread's span count since its last marker (then reset), the time and
 * the thread index. */
static void emit_marker(struct opp_tls* t, uint8_t type, int kind,
                        uint32_t index, uint32_t canon_tick) {
    accum_flush(t);
    uint8_t r[OPP_REC_SIZE];
    memset(r, 0, sizeof(r));
    r[0] = type;
    r[1] = (uint8_t)kind;
    le_u32(r + 4, index);
    le_u32(r + 8, canon_tick);
    le_u32(r + 12, t->spans_since_flush);
    le_u64(r + 16, op_prof_now_us());
    r[OPP_REC_TID] = (uint8_t)t->tid;
    chunk_put(t, r, sizeof(r));
    chunk_flush(t);
    t->spans_since_flush = 0;
}

uint32_t op_prof_present(uint32_t canon_tick, int kind) {
    if (!s_on) return (uint32_t)SDL_GetAtomicInt(&s_present_index);
    struct opp_tls* t = tls_get();
    if (!t) return (uint32_t)SDL_GetAtomicInt(&s_present_index);
    const uint32_t idx = (uint32_t)SDL_AddAtomicInt(&s_present_index, 1);
    emit_marker(t, OPP_REC_PRESENT, kind, idx, canon_tick);
    return idx;
}

uint32_t op_prof_scene(uint32_t canon_tick, int kind) {
    if (!s_on) return s_scene_index;
    struct opp_tls* t = tls_get();
    if (!t) return s_scene_index;
    const uint32_t idx = s_scene_index++;
    emit_marker(t, OPP_REC_SCENE, kind, idx, canon_tick);
    return idx;
}
