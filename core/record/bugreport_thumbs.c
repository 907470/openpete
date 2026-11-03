/* core/record/bugreport_thumbs.c - the bug-report thumbnail ring's host
 * store and cost counters. See openpete_bugthumbs.h. */
#include "openpete_bugthumbs.h"

#include "openpete_log.h"
#include "openpete_mutex_once.h"

#include <SDL3/SDL.h>

#include <stdlib.h>
#include <string.h>

#define THUMB_BYTES ((size_t)OPENPETE_BUGTHUMB_W * OPENPETE_BUGTHUMB_H * 4u)

static void*    s_mx;                 /* openpete_mutex_once slot */
static uint8_t* s_pix;                /* SLOTS * THUMB_BYTES, allocated at the first put */
static openpete_bugthumb_t s_slot[OPENPETE_BUGTHUMB_SLOTS];
static unsigned s_head, s_count;      /* next slot written; slots held */
/* Counters, under the mutex. */
static uint64_t s_puts, s_dropped;
static uint64_t s_cycles, s_queued, s_cycle_ns, s_cycle_max_ns;

int openpete_bugthumbs_enabled(void)
{
    static int s_on = -1;
    if (s_on < 0) {
        const char* e = getenv("OPENPETE_BUGREPORT_THUMBS");
        s_on = (e && *e && *e == '0') ? 0 : 1;
    }
    return s_on;
}

void openpete_bugthumbs_put(const void* rgba, int w, int h, int64_t present_ns, uint32_t feed_key)
{
    SDL_Mutex* mx = openpete_mutex_once(&s_mx);
    if (!mx || !rgba || w <= 0 || h <= 0) return;
    SDL_LockMutex(mx);
    if (!s_pix) s_pix = (uint8_t*)malloc(THUMB_BYTES * OPENPETE_BUGTHUMB_SLOTS);
    if (!s_pix || w > OPENPETE_BUGTHUMB_W || h > OPENPETE_BUGTHUMB_H) {
        s_dropped++;
        SDL_UnlockMutex(mx);
        return;
    }
    openpete_bugthumb_t* t = &s_slot[s_head];
    t->rgba = s_pix + (size_t)s_head * THUMB_BYTES;
    memcpy(t->rgba, rgba, (size_t)w * (size_t)h * 4u);
    t->w = w;
    t->h = h;
    t->present_ns = present_ns;
    t->feed_key = feed_key;
    s_head = (s_head + 1u) % OPENPETE_BUGTHUMB_SLOTS;
    if (s_count < OPENPETE_BUGTHUMB_SLOTS) s_count++;
    s_puts++;
    SDL_UnlockMutex(mx);
}

size_t openpete_bugthumbs_copy(openpete_bugthumb_t** out)
{
    *out = NULL;
    SDL_Mutex* mx = openpete_mutex_once(&s_mx);
    if (!mx) return 0;
    SDL_LockMutex(mx);
    const unsigned n = s_count;
    openpete_bugthumb_t* v = n ? (openpete_bugthumb_t*)calloc(n, sizeof *v) : NULL;
    size_t got = 0;
    for (unsigned i = 0; v && i < n; i++) {
        const openpete_bugthumb_t* t =
            &s_slot[(s_head + OPENPETE_BUGTHUMB_SLOTS - n + i) % OPENPETE_BUGTHUMB_SLOTS];
        const size_t bytes = (size_t)t->w * (size_t)t->h * 4u;
        uint8_t* px = (uint8_t*)malloc(bytes);
        if (!px) break;
        memcpy(px, t->rgba, bytes);
        v[got] = *t;
        v[got].rgba = px;
        got++;
    }
    SDL_UnlockMutex(mx);
    if (!got) { free(v); v = NULL; }
    *out = v;
    return got;
}

void openpete_bugthumbs_free(openpete_bugthumb_t* v, size_t n)
{
    if (!v) return;
    for (size_t i = 0; i < n; i++) free(v[i].rgba);
    free(v);
}

void openpete_bugthumbs_note_cycle(uint64_t ns, int queued)
{
    SDL_Mutex* mx = openpete_mutex_once(&s_mx);
    if (!mx) return;
    SDL_LockMutex(mx);
    s_cycles++;
    s_queued += queued ? 1u : 0u;
    s_cycle_ns += ns;
    if (ns > s_cycle_max_ns) s_cycle_max_ns = ns;
    SDL_UnlockMutex(mx);
}

void openpete_bugthumbs_report(void)
{
    SDL_Mutex* mx = openpete_mutex_once(&s_mx);
    if (!mx) return;
    SDL_LockMutex(mx);
    if (s_cycles || s_puts)
        OPL_INFO_F("bugreport", "thumbnails: %llu present(s) cycled, %llu downscale(s) queued, "
                   "%llu stored, %llu dropped, %u held; cycle mean %.2f us max %.2f us on the "
                   "display thread; host store %zu bytes",
                   (unsigned long long)s_cycles, (unsigned long long)s_queued,
                   (unsigned long long)s_puts, (unsigned long long)s_dropped, s_count,
                   s_cycles ? (double)s_cycle_ns / (double)s_cycles / 1000.0 : 0.0,
                   (double)s_cycle_max_ns / 1000.0,
                   s_pix ? THUMB_BYTES * OPENPETE_BUGTHUMB_SLOTS : (size_t)0);
    SDL_UnlockMutex(mx);
}
