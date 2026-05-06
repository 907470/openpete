// feature_registry.h — generic glue for openpete presentation features.
//
// Each feature (Hor+ widescreen, internal-resolution upscale, FPS unlock,
// …) provides a `feature_module_t` and gets listed in the per-game
// `feature_list.inc`. The registry walks that list at boot to apply
// command-line options and at every translated-function entry to
// dispatch per-vaddr overrides.
//
// Per-vertex GTE hooks do NOT go through a vtable here — they're
// `static inline` in per-feature headers (`<feature>_gte_inline.h`)
// included directly from third_party/psxrecomp/runner/src/gte.cpp.
// Indirect calls in the per-vertex hot path would regress determinism-
// test runtime; inlines compile to byte-identical asm vs the open-coded
// state today.
//
// See notes/plans/openpete-feature-modules.md.

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
} feature_dispatch_entry_t;

/* Forward declarations dropped above — `openpete_options_t` comes
 * from openpete.h, `CPUState` from the typedef above. */

typedef struct {
    const char*                       name;
    feature_apply_options_fn          apply_options;   /* nullable */
    const feature_dispatch_entry_t*   dispatch_table;  /* nullable; addr=0 terminates */
} feature_module_t;

/* Per-game feature list. Defined in games/<game>/features/feature_list.c
 * (which #includes feature_list.inc). The pointer array form lets us
 * add/remove features by editing one file without touching the registry
 * core. */
extern const feature_module_t* const openpete_features[];
extern const size_t                  openpete_feature_count;

void feature_registry_apply_options(const openpete_options_t* opts);

/* Walks each registered feature's dispatch_table for a matching addr.
 * Returns 1 if a feature handled the call (caller should skip the
 * recompiled body), 0 otherwise. */
int  feature_registry_dispatch(CPUState* cpu, uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif
