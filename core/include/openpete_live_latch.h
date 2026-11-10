/* openpete_live_latch.h - a live setting written by one thread and latched
 * by the thread that reads it.
 *
 * A live setter (settings menu, dev overlay, test hook) may run on a thread
 * other than the one that reads the setting: the settings menu runs on the
 * host loop while, under eager production, the present thread composes, and
 * the dev overlay runs on the present thread while the sim thread feeds. A
 * reader that loads the setter's variable several times in one present or
 * tick can see two values. Each such setting is a pair: an atomic wanted
 * value, which the setters store and the getters return, and a plain latched
 * value, which only the reading thread writes, at its latch point, before
 * the first reader of that present or tick. Every read there sees the
 * latched value.
 *
 * The wanted value always holds the setting: it is initialised to the
 * latched value's initializer, and every write of the latched value outside
 * the latch (init from the options) stores the wanted value too. A getter
 * therefore reads only the atomic, from any thread. Header-only, no state. */
#ifndef OPENPETE_LIVE_LATCH_H
#define OPENPETE_LIVE_LATCH_H

#include <stdatomic.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Store a setting's value: from a setter, or from init beside the latched value.
 *
 * @param want The setting's wanted value.
 * @param v The value. */
static inline void op_live_set(_Atomic int *want, int v)
{
    atomic_store_explicit(want, v, memory_order_release);
}

/** @brief Return the setting's value, the one the next latch applies; safe on any thread.
 *
 * @param want The setting's wanted value.
 * @return The value. */
static inline int op_live_get(_Atomic int *want)
{
    return atomic_load_explicit(want, memory_order_acquire);
}

/** @brief Copy the wanted value into the latched value; called by the reading thread only.
 *
 * @param want The setting's wanted value.
 * @param cur The latched value.
 * @return 1 when the latched value changed, else 0. */
static inline int op_live_latch(_Atomic int *want, int *cur)
{
    const int v = op_live_get(want);
    if (v == *cur) return 0;
    *cur = v;
    return 1;
}

/** @brief Return the int whose bits are a float's.
 *
 * @param f The value.
 * @return Its bits. */
static inline int op_live_f2i(float f)
{
    int i;
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

/** @brief Store a float setting's value.
 *
 * @param want The setting's wanted value, as float bits.
 * @param v The value. */
static inline void op_live_set_f(_Atomic int *want, float v)
{
    op_live_set(want, op_live_f2i(v));
}

/** @brief Return a float setting's value, the one the next latch applies; safe on any thread.
 *
 * @param want The setting's wanted value, as float bits.
 * @return The value. */
static inline float op_live_get_f(_Atomic int *want)
{
    return op_live_i2f(op_live_get(want));
}

/** @brief Copy the wanted float into the latched value; called by the reading thread only.
 *
 * Compares bits, so a change of sign of zero counts as a change.
 * @param want The setting's wanted value, as float bits.
 * @param cur The latched value.
 * @return 1 when the latched value changed, else 0. */
static inline int op_live_latch_f(_Atomic int *want, float *cur)
{
    const int v = op_live_get(want);
    if (v == op_live_f2i(*cur)) return 0;
    *cur = op_live_i2f(v);
    return 1;
}

/* The bits of 1.0f, for a static initializer of a float wanted value. */
#define OP_LIVE_F_ONE 0x3F800000

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_LIVE_LATCH_H */
