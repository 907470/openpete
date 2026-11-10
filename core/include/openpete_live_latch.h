/* openpete_live_latch.h - a live setting written by one thread and latched
 * once per present by the thread that reads it.
 *
 * A live setter (settings menu, dev overlay, test hook) may run on a thread
 * other than the one that reads the setting: the settings menu runs on the
 * host loop while, under eager production, the present thread composes. A
 * reader that loads the setter's variable several times in one present can
 * see two values. Each such setting is a pair: an atomic wanted value, which
 * the setter stores and the getters return, and a plain latched value, which
 * only the reading thread writes, once per present, before the first reader
 * of that present runs. Every read inside the present sees the latched value.
 *
 * The wanted value starts at OP_LIVE_NONE (no setter has run); the latch
 * then leaves the latched value as its initializer set it. Header-only, no
 * state. */
#ifndef OPENPETE_LIVE_LATCH_H
#define OPENPETE_LIVE_LATCH_H

#include <limits.h>
#include <stdatomic.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The wanted value of a setting no setter has written. */
#define OP_LIVE_NONE INT_MIN

/** @brief Store a wanted value for the next latch.
 *
 * @param want The setting's wanted value.
 * @param v The value; OP_LIVE_NONE is not a legal value. */
static inline void op_live_set(_Atomic int *want, int v)
{
    atomic_store_explicit(want, v, memory_order_release);
}

/** @brief Return the value the setting will hold after the next latch.
 *
 * @param want The setting's wanted value.
 * @param cur The latched value, returned when no setter has run.
 * @return The wanted value when a setter has run, else @p cur. */
static inline int op_live_wanted(_Atomic int *want, int cur)
{
    const int v = atomic_load_explicit(want, memory_order_acquire);
    return v == OP_LIVE_NONE ? cur : v;
}

/** @brief Copy the wanted value into the latched value; called by the reading thread only.
 *
 * @param want The setting's wanted value.
 * @param cur The latched value, written when a setter has run.
 * @return 1 when the latched value changed, else 0. */
static inline int op_live_latch(_Atomic int *want, int *cur)
{
    const int v = atomic_load_explicit(want, memory_order_acquire);
    if (v == OP_LIVE_NONE || v == *cur) return 0;
    *cur = v;
    return 1;
}

/** @brief Return the int whose bits are a float's, with -0.0f taken as +0.0f.
 *
 * -0.0f has the bits of OP_LIVE_NONE, so it is stored as +0.0f.
 * @param f The value.
 * @return Its bits. */
static inline int op_live_f2i(float f)
{
    int i;
    if (f == 0.0f) f = 0.0f;
    memcpy(&i, &f, sizeof i);
    return i;
}

/** @brief Return the float whose bits an int holds.
 *
 * @param i The bits.
 * @return The value. */
static inline float op_live_i2f(int i)
{
    float f;
    memcpy(&f, &i, sizeof f);
    return f;
}

/** @brief Store a wanted float value for the next latch.
 *
 * @param want The setting's wanted value, as float bits.
 * @param v The value. */
static inline void op_live_set_f(_Atomic int *want, float v)
{
    op_live_set(want, op_live_f2i(v));
}

/** @brief Return the float the setting will hold after the next latch.
 *
 * @param want The setting's wanted value, as float bits.
 * @param cur The latched value, returned when no setter has run.
 * @return The wanted value when a setter has run, else @p cur. */
static inline float op_live_wanted_f(_Atomic int *want, float cur)
{
    const int v = atomic_load_explicit(want, memory_order_acquire);
    return v == OP_LIVE_NONE ? cur : op_live_i2f(v);
}

/** @brief Copy the wanted float into the latched value; called by the reading thread only.
 *
 * @param want The setting's wanted value, as float bits.
 * @param cur The latched value, written when a setter has run.
 * @return 1 when the latched value changed, else 0. */
static inline int op_live_latch_f(_Atomic int *want, float *cur)
{
    const int v = atomic_load_explicit(want, memory_order_acquire);
    if (v == OP_LIVE_NONE || v == op_live_f2i(*cur)) return 0;
    *cur = op_live_i2f(v);
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_LIVE_LATCH_H */
