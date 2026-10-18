/* core/input/input_log.c - see openpete_input_log.h. */

#include "openpete_input_log.h"
#include "openpete_log.h"

#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_mutex.h>

#include <stdlib.h>
#include <string.h>

static openpete_input_rec_t* s_log = NULL;
static int s_count = 0;
static int s_capacity = 0;
static int s_alloc_failed = 0;

/* The record for the tick about to run; see the staging note in the header. */
static openpete_input_rec_t s_staged;
static unsigned s_staged_vbl;

void input_port_clear(openpete_input_port_t* p) {
    p->bits = 0xFFFF;
    p->axes[0] = p->axes[1] = p->axes[2] = p->axes[3] = 128;
    p->present = 0;
}

void input_log_stage_reset(unsigned vbl) {
    for (int i = 0; i < OPENPETE_PAD_SLOTS; i++)
        input_port_clear(&s_staged.port[i]);
    s_staged_vbl = vbl;
}

void input_log_stage_port(int slot, uint16_t bits, const uint8_t axes[4]) {
    if (slot < 0 || slot >= OPENPETE_PAD_SLOTS) return;
    openpete_input_port_t* p = &s_staged.port[slot];
    p->bits = bits;
    if (axes) memcpy(p->axes, axes, 4);
    else      p->axes[0] = p->axes[1] = p->axes[2] = p->axes[3] = 128;
    p->present = 1;
}

void input_log_stage_p1_readback(uint16_t bits, const uint8_t axes[4]) {
    openpete_input_port_t* p = &s_staged.port[0];
    p->bits = bits;
    if (axes) memcpy(p->axes, axes, 4);
    else      p->axes[0] = p->axes[1] = p->axes[2] = p->axes[3] = 128;
    p->present = 1;
}

const openpete_input_rec_t* input_log_staged(void) { return &s_staged; }

unsigned input_log_staged_vbl(void) { return s_staged_vbl; }

int input_log_read_slot(int slot, openpete_input_port_t* out) {
    if (!out) return 0;
    if (slot < 0 || slot >= OPENPETE_PAD_SLOTS) {
        input_port_clear(out);
        return 0;
    }
    *out = s_staged.port[slot];
    return 1;
}

int input_log_append_staged(void) {
    if (s_alloc_failed) return -1;
    if (s_count == s_capacity) {
        int new_cap = s_capacity ? s_capacity * 2 : 16 * 1024;
        openpete_input_rec_t* g = (openpete_input_rec_t*)
            realloc(s_log, (size_t)new_cap * sizeof(*s_log));
        if (!g) {
            /* Stop growing rather than crash; consumers see a truncated
             * log (bk2_record already had this drop-the-frame policy). */
            OPL_ERROR_F("input-log", "realloc failed at %d entries; log frozen",
                        s_count);
            s_alloc_failed = 1;
            return -1;
        }
        s_log = g;
        s_capacity = new_cap;
    }
    s_log[s_count] = s_staged;
    return s_count++;
}

int input_log_count(void) { return s_count; }

const openpete_input_rec_t* input_log_get(int idx) {
    return (idx >= 0 && idx < s_count) ? &s_log[idx] : NULL;
}

void input_log_truncate(int count) {
    if (count < 0) count = 0;
    if (count < s_count) s_count = count;
}

void input_log_reset(void) {
    free(s_log);
    s_log = NULL;
    s_count = s_capacity = 0;
    s_alloc_failed = 0;
    input_log_stage_reset(0);
}

/* The writer lock (input_log_lock), published once by compare-and-swap. */
static SDL_Mutex* s_lock_mx;

/** @brief Return the writer lock, creating it on first use; a compare-and-swap publishes exactly one.
 *
 * @return The mutex; NULL when creation failed, which SDL_LockMutex treats as a no-op. */
static SDL_Mutex* input_log_mx(void) {
    SDL_Mutex* m = (SDL_Mutex*)SDL_GetAtomicPointer((void**)&s_lock_mx);
    if (m) return m;
    SDL_Mutex* n = SDL_CreateMutex();
    if (n && !SDL_CompareAndSwapAtomicPointer((void**)&s_lock_mx, NULL, n)) SDL_DestroyMutex(n);
    return (SDL_Mutex*)SDL_GetAtomicPointer((void**)&s_lock_mx);
}

void input_log_lock(void) { SDL_LockMutex(input_log_mx()); }

void input_log_unlock(void) { SDL_UnlockMutex(input_log_mx()); }
