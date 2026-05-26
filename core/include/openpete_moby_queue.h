// core/include/openpete_moby_queue.h — host-side FIFO carrying per-instance
// moby vaddrs from Spyro's cull pass to the regular/specular render passes.
//
// PROBLEM
//   The regular/specular render passes (r_moby.s func_8001F798 /
//   func_80020F34) iterate a render-list whose slots carry a CLASS-SHARED
//   pointer (likely m_Props / model template) rather than the moby's PSX
//   instance vaddr. Tagging cache entries with that class-shared ptr made
//   the matcher's find_unconsumed_by_obj_tag collapse all same-class
//   instances onto one obj_tag, producing the "two gems blending across
//   the screen" mispair whenever cull order shifted.
//
// SOLUTION
//   The cull pass iterates Mobys *by* their PSX vaddr and writes the
//   survivors into the render-list. A recompiler hook at the first
//   instruction past every cull-skip back-branch in the per-moby cull
//   cascade pushes the moby vaddr onto this queue. The regular/specular
//   render passes pop one vaddr per loop iter and feed it into
//   openpete_ui_moby_set_current — restoring per-instance obj_tag
//   identity in the cache. The exact game-specific PCs are listed in
//   the recompiler substitution table (see code_generator.cpp) and in
//   manifest/render_channels.toml under transform_interp_moby_queue.
//
// FIFO INVARIANT
//   The cull pass fully populates the queue before its render pass
//   begins (draw.c sequences them as separate function calls), and there
//   is one cull→render cycle per moby class-set: regular (func_8001F158
//   → func_8001F798) and specular (func_800208FC → func_80020F34). A
//   shared FIFO works because the two cycles are serialised in the
//   frame: cycle 1 pushes, cycle 1 pops to empty, cycle 2 pushes, cycle
//   2 pops to empty. Implementation rejects pop-from-empty silently (the
//   downstream gte just sees obj_tag=0 and falls back to world-ordinal
//   pairing — the pre-queue behaviour).
//
// READ-ONLY w.r.t. gameplay: push stashes a PSX vaddr in a host-side
// ring; pop returns it. No PSX RAM, cpu state, or gameplay-readable
// byte is written.

#ifndef OPENPETE_MOBY_QUEUE_H
#define OPENPETE_MOBY_QUEUE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void     openpete_moby_queue_push(uint32_t moby_vaddr);
uint32_t openpete_moby_queue_pop(void);   /* 0 when empty */
void     openpete_moby_queue_clear(void); /* defensive reset */

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_MOBY_QUEUE_H */
