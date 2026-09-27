/* core/include/openpete_mutex_once.h — a mutex created on first use without
 * a creation race.
 *
 * A `static SDL_Mutex* m; if (!m) m = SDL_CreateMutex();` accessor reached
 * by two threads at once can create two mutexes, and each thread then locks
 * its own. The slot here is an SDL atomic pointer: the first caller to
 * compare-and-swap its fresh mutex in wins, and a loser destroys its own
 * and takes the winner's. */
#ifndef OPENPETE_MUTEX_ONCE_H
#define OPENPETE_MUTEX_ONCE_H

#include <SDL3/SDL.h>

/** @brief Return the mutex held in slot, creating it on first use; concurrent first calls agree on one.
 *
 * @param slot A zero-initialised pointer slot, accessed only through this call.
 * @return The mutex, or NULL when creation failed (SDL treats a NULL mutex as a no-op lock). */
static inline SDL_Mutex *openpete_mutex_once(void **slot)
{
    SDL_Mutex *m = (SDL_Mutex *)SDL_GetAtomicPointer(slot);
    if (m) return m;
    SDL_Mutex *fresh = SDL_CreateMutex();
    if (!fresh) return (SDL_Mutex *)SDL_GetAtomicPointer(slot);
    if (SDL_CompareAndSwapAtomicPointer(slot, NULL, fresh)) return fresh;
    SDL_DestroyMutex(fresh);
    return (SDL_Mutex *)SDL_GetAtomicPointer(slot);
}

#endif
