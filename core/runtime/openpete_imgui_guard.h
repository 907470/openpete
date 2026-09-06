/* core/runtime/openpete_imgui_guard.h — containment for the ImGui annex.
 *
 * Every entry of the engine's ImGui table is a generated wrapper
 * (openpete_imgui_guard.cpp, from tools/gen_imgui_annex.py) that asks
 * openpete_imgui_body_active() where it was called from. Inside a mod UI
 * section body the call goes through and is counted; anywhere else — a
 * present hook on the extracting thread, a tick hook on the sim thread — it
 * is dropped with a one-shot error and the wrapper returns a zeroed value.
 * The drop is required, not defensive: those call sites run outside
 * NewFrame/Render, where ImGui's own checks are compiled out under NDEBUG
 * and the failure would be a null-window dereference on a foreign thread.
 *
 * The depth and the call counter are thread-local: bodies run on the
 * compose thread while other threads may be inside mod code.
 */

#ifndef OPENPETE_IMGUI_GUARD_H
#define OPENPETE_IMGUI_GUARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Queried by every generated wrapper. Non-zero while this thread is inside a
 * mod UI section body. */
int openpete_imgui_body_active(void);

/* Logs OPL_ERROR once per (mod index, function name) so a mod calling from
 * the wrong place reports itself without flooding the log. */
void openpete_imgui_guard_refused(const char* fn);

/* One call that reached Dear ImGui, for the engagement counter. */
void openpete_imgui_count_call(void);

/* Raised around a body by the loader's section dispatch. @p mod_index
 * attributes refusals and is reported by openpete_imgui_guard_mod_id().
 * Nested entries are counted, so a body that re-enters keeps the depth
 * balanced. */
void openpete_imgui_body_enter(int mod_index);
void openpete_imgui_body_leave(void);

/* Reads and zeroes this thread's call counter. Called after a body so the
 * count reported is the count for that body. */
uint64_t openpete_imgui_calls_take(void);

/* Mod id for a refusal message. The loader installs the resolver, which owns
 * the mapping from index to id; until then the index is printed. The returned
 * pointer must stay valid for the process lifetime. */
const char* openpete_imgui_guard_mod_id(int mod_index);
void openpete_imgui_guard_set_mod_id_fn(const char* (*fn)(int mod_index));

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_IMGUI_GUARD_H */
