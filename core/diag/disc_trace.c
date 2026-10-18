/* core/diag/disc_trace.c - see openpete_disc_trace.h. */
#include "openpete_disc_trace.h"
#include "openpete_log.h"
#include "openpete_time.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#define DT_SECTOR 2352u

int openpete_disc_trace_state = -1;

static SDL_SpinLock     s_lock;
static FILE*            s_fp;
static int            (*s_vbl_fn)(void);
static _Thread_local int s_scope = OPENPETE_DISC_TAG_NONE;

/* A row waiting for a contiguous successor from the same thread. Each
 * reading thread holds its own, so a row's extent does not depend on how
 * other threads' reads interleave with it. */
typedef struct {
    int            live;
    int64_t        ns;
    int            vbl;
    int            tag;
    int            result;
    SDL_ThreadID   thread;
    uint64_t       offset;
    uint64_t       end;
} dt_row_t;

#define DT_PENDING 8
static dt_row_t s_pend[DT_PENDING];

static const char* const k_tag_names[OPENPETE_DISC_TAG_COUNT] = {
    "none", "libcd", "xa", "exe", "serial", "extract", "woah", "sfx",
    "wad", "ingest", "cache-load", "iso", "mod", "font", "terrain", "icon",
};

static const char* const k_result_names[] = { "hit", "miss", "direct" };

const char* openpete_disc_trace_tag_name(int tag) {
    return (tag >= 0 && tag < OPENPETE_DISC_TAG_COUNT) ? k_tag_names[tag] : "?";
}

/** @brief Write a live row to the trace file and clear it, under s_lock.
 *
 * @param r The row. */
static void emit_row(dt_row_t* r) {
    if (!r->live || !s_fp) return;
    uint64_t lba = r->offset / DT_SECTOR;
    uint64_t lba_end = (r->end + DT_SECTOR - 1u) / DT_SECTOR;
    fprintf(s_fp, "%lld\t%d\t%s\t%llu\t%llu\t%s\t%llu\t%llu\n",
            (long long)r->ns, r->vbl, openpete_disc_trace_tag_name(r->tag),
            (unsigned long long)lba, (unsigned long long)(lba_end - lba),
            k_result_names[r->result], (unsigned long long)r->offset,
            (unsigned long long)(r->end - r->offset));
    r->live = 0;
}

/** @brief Write every pending row, under s_lock. */
static void emit_pending(void) {
    for (int i = 0; i < DT_PENDING; i++) emit_row(&s_pend[i]);
}

/** @brief The pending slot of thread th, under s_lock: its live row, else a free slot, else the oldest row after writing it.
 *
 * @param th The reading thread.
 * @return The slot. */
static dt_row_t* pending_slot(SDL_ThreadID th) {
    dt_row_t* free_slot = NULL;
    dt_row_t* oldest = &s_pend[0];
    for (int i = 0; i < DT_PENDING; i++) {
        dt_row_t* r = &s_pend[i];
        if (r->live && r->thread == th) return r;
        if (!r->live && !free_slot) free_slot = r;
        if (r->live && oldest->live && r->ns < oldest->ns) oldest = r;
    }
    if (free_slot) return free_slot;
    emit_row(oldest);
    return oldest;
}

void openpete_disc_trace_flush(void) {
    if (openpete_disc_trace_state != 1) return;
    SDL_LockSpinlock(&s_lock);
    emit_pending();
    if (s_fp) fflush(s_fp);
    SDL_UnlockSpinlock(&s_lock);
}

/** @brief Exit handler: write the pending row and close the trace file. */
static void disc_trace_close(void) {
    SDL_LockSpinlock(&s_lock);
    emit_pending();
    if (s_fp) { fclose(s_fp); s_fp = NULL; }
    SDL_UnlockSpinlock(&s_lock);
}

int openpete_disc_trace_init(void) {
    if (openpete_disc_trace_state >= 0) return openpete_disc_trace_state;
    SDL_LockSpinlock(&s_lock);
    if (openpete_disc_trace_state < 0) {
        const char* path = getenv("OPENPETE_DISC_TRACE");
        int on = 0;
        if (path && path[0]) {
            s_fp = fopen(path, "w");
            if (s_fp) {
                setvbuf(s_fp, NULL, _IOFBF, 1u << 20);
                fputs("#ns\tvbl\ttag\tlba\tcount\tresult\toffset\tlength\n", s_fp);
                atexit(disc_trace_close);
                on = 1;
            } else {
                OPL_WARN_F("cd", "OPENPETE_DISC_TRACE: cannot open %s; disc trace off", path);
            }
        }
        openpete_disc_trace_state = on;
    }
    SDL_UnlockSpinlock(&s_lock);
    if (openpete_disc_trace_state == 1)
        OPL_INFO_F("cd", "disc trace: writing %s", getenv("OPENPETE_DISC_TRACE"));
    return openpete_disc_trace_state;
}

void openpete_disc_trace_reload(void) {
    const char* path = getenv("OPENPETE_DISC_TRACE");
    if (!path || !path[0]) return;
    SDL_LockSpinlock(&s_lock);
    if (openpete_disc_trace_state == 0) openpete_disc_trace_state = -1;
    SDL_UnlockSpinlock(&s_lock);
}

void openpete_disc_trace_set_vbl_source(int (*fn)(void)) { s_vbl_fn = fn; }

int openpete_disc_trace_vbl(void) { return s_vbl_fn ? s_vbl_fn() : -1; }

void openpete_disc_trace_bytes_slow(int tag, uint64_t offset, uint64_t length, int result) {
    if (!openpete_disc_trace_init()) return;
    if (tag == OPENPETE_DISC_TAG_NONE)
        tag = s_scope != OPENPETE_DISC_TAG_NONE ? s_scope : OPENPETE_DISC_TAG_ISO;
    if (result < OPENPETE_DISC_HIT || result > OPENPETE_DISC_DIRECT) result = OPENPETE_DISC_DIRECT;
    int64_t ns = op_now();
    int vbl = s_vbl_fn ? s_vbl_fn() : -1;
    SDL_ThreadID th = SDL_GetCurrentThreadID();
    uint64_t end = offset + length;
    dt_row_t row = { 1, ns, vbl, tag, result, th, offset, end };
    SDL_LockSpinlock(&s_lock);
    if (tag == OPENPETE_DISC_TAG_CACHE_LOAD) {
        emit_row(&row);                 /* one row per loader read */
        SDL_UnlockSpinlock(&s_lock);
        return;
    }
    dt_row_t* p = pending_slot(th);
    /* A read continues the thread's pending row when it starts where that
     * row ends, byte for byte or at the next sector (a caller that records
     * a byte range inside each raw sector). */
    int contiguous = p->live && (offset == p->end ||
                     offset / DT_SECTOR == (p->end + DT_SECTOR - 1u) / DT_SECTOR);
    if (contiguous && tag == p->tag && result == p->result && vbl == p->vbl) {
        p->end = end;
    } else {
        emit_row(p);
        *p = row;
    }
    SDL_UnlockSpinlock(&s_lock);
}

void openpete_disc_trace_sectors_slow(int tag, uint32_t lba, uint32_t count, int result) {
    openpete_disc_trace_bytes_slow(tag, (uint64_t)lba * DT_SECTOR, (uint64_t)count * DT_SECTOR, result);
}

int openpete_disc_trace_scope_begin_slow(int tag) {
    if (!openpete_disc_trace_init()) return -1;
    int prev = s_scope;
    if (prev == OPENPETE_DISC_TAG_NONE) s_scope = tag;
    return prev;
}

void openpete_disc_trace_scope_end_slow(int token) {
    if (token >= OPENPETE_DISC_TAG_NONE && token < OPENPETE_DISC_TAG_COUNT) s_scope = token;
}
