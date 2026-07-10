// core/mods/mod_chain.c — per-address override chains + base() fallthrough.
//
// The ONE override primitive of the modding framework
// (notes/plans/openpete-modding-framework.md): N mods stack on a guest
// function; the highest-priority override runs first; base() calls the
// next link down and bottoms out at the recompiled original.
//
// Mechanism: every recompiled function's prologue calls
// psx_override_dispatch(cpu, addr) (emitted by the recompiler for all
// functions — the freecam freeze rides the same chokepoint), which
// reaches openpete_mods_dispatch() below. To fall through to the real
// body, base() re-calls the function through call_by_address() with a
// one-shot bypass armed for that addr, so the prologue's mod arm returns
// 0 and the recompiled body runs — the generalization of the in-tree
// re-entrancy-guard pattern in game_extras_impl.c (func_80023AC4 trace).
//
// Chain contents are runtime data (plan Round 7): registering and
// removing links needs no re-link or code patching. All chain execution
// happens on the sim thread (guest-call semantics); the TLS frame stack
// is belt-and-braces.
//
// Overlay caveat (documented, raw tier): all level overlays share the
// same vaddr window, so a chain keyed on an overlay-window addr fires
// for whichever overlay is resident. Named-tier symbols avoid this
// (main-EXE addresses are unique).

#include "openpete_mods.h"
#include "openpete_mod_api.h"
#include "openpete_log.h"

#include <string.h>

/* Recompiler runtime: dispatch-by-address (psx_runtime.h). */
void call_by_address(CPUState* cpu, uint32_t addr);

int openpete_mods_active = 0;

#define MOD_CHAIN_MAX        64   /* distinct overridden addresses */
#define MOD_CHAIN_LINKS_MAX  16   /* stacked overrides per address  */
#define MOD_FRAME_STACK_MAX  32   /* nested chain invocations       */

typedef struct {
    openpete_mod_override_fn fn;
    const char*              owner_id; /* for diagnostics/attribution */
} mod_link_t;

typedef struct {
    uint32_t   addr;
    int        count;
    mod_link_t links[MOD_CHAIN_LINKS_MAX]; /* [0] = highest priority */
} mod_chain_t;

static mod_chain_t s_chains[MOD_CHAIN_MAX];
static int         s_chain_count = 0;

/* Per-call frame: which link of which chain is currently executing, so
 * base() knows its continuation. Sim-thread only; TLS for safety. */
typedef struct {
    mod_chain_t* chain;
    int          link;
} mod_frame_t;

static _Thread_local mod_frame_t s_frames[MOD_FRAME_STACK_MAX];
static _Thread_local int         s_frame_depth = 0;

/* One-shot bypass: armed by base() bottoming out, consumed by the very
 * next prologue hit on that addr so the recompiled body runs. */
static _Thread_local uint32_t s_bypass_addr = 0;
static _Thread_local int      s_bypass_armed = 0;

static mod_chain_t* chain_find(uint32_t addr) {
    for (int i = 0; i < s_chain_count; i++) {
        if (s_chains[i].addr == addr) return &s_chains[i];
    }
    return NULL;
}

/* Run link `idx` of `chain`, or the recompiled original when the chain
 * is exhausted. base() is "run_link(current+1)". */
static void run_link(CPUState* cpu, mod_chain_t* chain, int idx) {
    if (idx < chain->count) {
        if (s_frame_depth >= MOD_FRAME_STACK_MAX) {
            OPL_ERROR_F("mods", "override frame stack overflow at 0x%08X "
                        "(recursion through %d chained calls?) — running "
                        "original body", chain->addr, s_frame_depth);
        } else {
            mod_frame_t* f = &s_frames[s_frame_depth++];
            f->chain = chain;
            f->link  = idx;
            chain->links[idx].fn(cpu);
            s_frame_depth--;
            return;
        }
    }
    /* Chain exhausted (or overflow fallback): call the original through
     * the dispatch switch. The callee's prologue re-enters
     * openpete_mods_dispatch, sees the armed bypass, and returns 0 so
     * the recompiled body executes. */
    s_bypass_addr  = chain->addr;
    s_bypass_armed = 1;
    call_by_address(cpu, chain->addr);
    /* Normally consumed at the prologue; clear defensively in case
     * another dispatch arm (freecam freeze, native decomp) swallowed the
     * call before our arm saw it. */
    s_bypass_armed = 0;
}

int openpete_mods_dispatch(CPUState* cpu, uint32_t addr) {
    if (s_bypass_armed && s_bypass_addr == addr) {
        s_bypass_armed = 0; /* base() bottoming out: run the real body */
        return 0;
    }
    mod_chain_t* chain = chain_find(addr);
    if (!chain || chain->count == 0) return 0;
    run_link(cpu, chain, 0);
    return 1;
}

void openpete_mods_base(CPUState* cpu) {
    if (s_frame_depth <= 0) {
        OPL_ERROR_F("mods", "base() called outside an override — ignored");
        return;
    }
    mod_frame_t f = s_frames[s_frame_depth - 1];
    run_link(cpu, f.chain, f.link + 1);
}

/* Register with priority: the loader loads mods in enabled-list order
 * and each new link goes to the FRONT of its chain, so the last-enabled
 * (highest-priority) mod runs first and base() walks toward
 * earlier-enabled mods, then the original (S3AIR semantics: order IS
 * priority). */
int openpete_mods_chain_add(uint32_t addr, openpete_mod_override_fn fn,
                            const char* owner_id) {
    if (addr == 0 || !fn) return -1;
    mod_chain_t* chain = chain_find(addr);
    if (!chain) {
        if (s_chain_count >= MOD_CHAIN_MAX) {
            OPL_ERROR_F("mods", "override chain table full (%d addrs) — "
                        "cannot hook 0x%08X for %s",
                        MOD_CHAIN_MAX, addr, owner_id ? owner_id : "?");
            return -1;
        }
        chain = &s_chains[s_chain_count++];
        memset(chain, 0, sizeof(*chain));
        chain->addr = addr;
    }
    if (chain->count >= MOD_CHAIN_LINKS_MAX) {
        OPL_ERROR_F("mods", "override chain at 0x%08X full (%d links) — "
                    "cannot add %s", addr, chain->count,
                    owner_id ? owner_id : "?");
        return -1;
    }
    memmove(&chain->links[1], &chain->links[0],
            (size_t)chain->count * sizeof(mod_link_t));
    chain->links[0].fn       = fn;
    chain->links[0].owner_id = owner_id;
    chain->count++;
    openpete_mods_active = 1;
    return 0;
}
