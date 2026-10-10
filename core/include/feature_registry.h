// feature_registry.h - generic glue for openpete presentation features.
//
// Each feature (Hor+ widescreen, internal-resolution upscale, FPS unlock,
// ...) provides a `feature_module_t` and gets listed in the per-game
// `feature_list.c`, which the game's runtime descriptor carries. The registry walks that list at boot to apply
// command-line options and at every translated-function entry to
// dispatch per-vaddr overrides.
//
// Per-vertex GTE hooks do NOT go through a vtable here: an indirect call in
// the per-vertex hot path costs more than the hook's inline body.

#ifndef OPENPETE_FEATURE_REGISTRY_H
#define OPENPETE_FEATURE_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

#include "openpete.h"   /* openpete_options_t */

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;
typedef struct CPUState CPUState;

typedef int  (*feature_dispatch_fn)(CPUState* cpu, uint32_t addr);
typedef void (*feature_apply_options_fn)(const openpete_options_t* opts);

typedef struct {
    uint32_t            addr;        /* PSX vaddr; 0 terminates the table */
    feature_dispatch_fn fn;
    /* Overlay-id qualifier. Every overlay of a game loads at one vaddr, so a
     * function address inside that window names a different function in each
     * overlay. An entry with a nonzero id dispatches only while that overlay
     * is the resident one; 0 (the value every base-binary entry gets from a
     * two-field initializer) dispatches unconditionally. */
    uint32_t            overlay;
} feature_dispatch_entry_t;

/* Forward declarations dropped above - `openpete_options_t` comes
 * from openpete.h, `CPUState` from the typedef above. */

typedef struct {
    const char*                       name;
    feature_apply_options_fn          apply_options;   /* nullable */
    const feature_dispatch_entry_t*   dispatch_table;  /* nullable; addr=0 terminates */
} feature_module_t;

/* The modules walked are the registered game descriptor's feature list
 * (openpete_game_runtime.h), defined in games/<game>/feature_list.c; a
 * descriptor without one registers no module. */

/** @brief Hand the parsed options to every registered feature module.
 *
 * Calls each module's apply_options in list order; a null module or a module
 * without the callback is skipped.
 * @param opts The parsed options. */
void feature_registry_apply_options(const openpete_options_t* opts);

/** @brief Register the resident-overlay probe consulted for entries carrying a nonzero `overlay`.
 *
 * The overlay id is a per-game concept, so the registry reaches it through
 * this pointer instead of naming the game's table. The probe is called only
 * after an address match, never on the dispatch hot path. Unregistered (or
 * NULL), every qualified entry stays inert. A later call replaces the probe.
 * @param fn Returns the id of the overlay resident in guest RAM. */
void feature_registry_set_overlay_probe(uint32_t (*fn)(void));

/** @brief Return the registered resident-overlay probe.
 *
 * The mods dispatcher qualifies override links inside the overlay window
 * by the same probe the registry uses for its entries.
 * @return The probe, or NULL when none is registered. */
uint32_t (*feature_registry_overlay_probe(void))(void);

/** @brief Run the feature hooks registered for a guest function address.
 *
 * An address whose bit is clear in a prefilter built from every table on
 * the first call returns 0 without walking. Otherwise walks every module of
 * the registered feature list in order and every entry of its dispatch_table. A matching entry with a nonzero `overlay` runs only
 * while the probe reports that overlay resident. The walk stops at the
 * first hook that returns nonzero; a hook returning 0 lets later matches
 * run and the recompiled body run after them. Under --profile-out each
 * hook's time accumulates in a `hook.<module name>` bucket.
 * @param cpu Guest CPU state passed to the hooks.
 * @param addr PSX address of the function being entered.
 * @return 1 when a hook handled the call and the caller skips the
 *         recompiled body, else 0. */
int  feature_registry_dispatch(CPUState* cpu, uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif
