/* env_knob.h — the boot-latched OPENPETE_* readers the renderer feeders share.
 *
 * These knobs select a code path, arm a census or inject a fault, so each is
 * read once, at the first call of the getter that owns it, and the value is
 * latched for the life of the process: a later change to the environment
 * would otherwise describe a run that never happened. A knob a person changes
 * while the game runs is a registry row instead (core/config/knob_registry.c)
 * and must not use these.
 *
 * The latch slot stays the caller's own static, so each getter keeps its own
 * state and the sentinel it already used: -1 for a flag, -2 for an integer
 * that may legitimately be negative.
 */
#ifndef OPENPETE_RENDERER_ENV_KNOB_H
#define OPENPETE_RENDERER_ENV_KNOB_H

#include <stdlib.h>
#include <string.h>

/* Boolean knob: true when the variable is set to anything but "0".
 * `slot` is a static int seeded to -1. */
#define ENV_FLAG_LATCH(slot, var)                                           \
    do {                                                                    \
        if ((slot) < 0) {                                                   \
            const char* env_knob_e_ = getenv(var);                          \
            (slot) = (env_knob_e_ && *env_knob_e_ && *env_knob_e_ != '0');  \
        }                                                                   \
    } while (0)

/* Integer knob with a fallback for "unset or empty".
 * `slot` is a static int seeded to -2. */
#define ENV_INT_LATCH(slot, var, dflt)                                      \
    do {                                                                    \
        if ((slot) == -2) {                                                 \
            const char* env_knob_e_ = getenv(var);                          \
            (slot) = (env_knob_e_ && *env_knob_e_) ? atoi(env_knob_e_)      \
                                                   : (dflt);                \
        }                                                                   \
    } while (0)

/* A census knob spelled LO-HI, or a single frame meaning LO == HI. Unset or
 * empty leaves the range empty, which no frame is inside. */
typedef struct { int lo, hi; } env_frame_range_t;

#define ENV_FRAME_RANGE_INIT { -2, -2 }   /* -2 = not read yet */

static inline int env_frame_range_active(env_frame_range_t* r, const char* var,
                                         unsigned frame) {
    if (r->lo == -2) {
        const char* e = getenv(var);
        r->lo = r->hi = -1;
        if (e && *e) {
            const char* dash = strchr(e, '-');
            r->lo = atoi(e);
            r->hi = dash ? atoi(dash + 1) : r->lo;
        }
    }
    return r->lo >= 0 && (int)frame >= r->lo && (int)frame <= r->hi;
}

#endif /* OPENPETE_RENDERER_ENV_KNOB_H */
