/* core/ui/openpete_imgui_guard.h — containment for the mod ImGui table.
 *
 * Every entry of the engine's ImGui table is a generated wrapper
 * (openpete_imgui_guard.cpp, from tools/gen_imgui_table.py) that asks
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

/* One call that reached ImGui, for the engagement counter. */
void openpete_imgui_count_call(void);

/** @brief Enter a mod UI section body on this thread.
 *
 * The loader's section dispatch brackets each body with this and
 * openpete_imgui_body_leave(). Raises the thread-local depth. The outermost
 * entry records @p mod_index and zeroes the call counter; nested entries
 * only raise the depth, so a body that re-enters keeps the depth balanced.
 * Equivalent to openpete_imgui_body_enter_always() with no_input 0.
 * @param mod_index Index of the mod whose body runs; refusal messages name
 *        it through openpete_imgui_guard_mod_id(). */
void openpete_imgui_body_enter(int mod_index);
void openpete_imgui_body_leave(void);

/* Same, for a body the always pass is running with the dev overlay closed.
 * @p no_input raises a second thread-local bit for the depth of the body,
 * read by openpete_imgui_always_closed(). */
void openpete_imgui_body_enter_always(int mod_index, int no_input);

/* Non-zero while this thread is inside an always-section body drawn with
 * the overlay closed. The ImGui_Begin / ImGui_BeginChild wrappers OR
 * ImGuiWindowFlags_NoInputs into their flags argument on it, so a window a
 * mod draws during play never eats the mouse the game is reading. */
int openpete_imgui_always_closed(void);

/** @brief Return and zero this thread's count of ImGui calls that reached ImGui.
 *
 * The mod loader calls it after a section body and stores the result as
 * that mod's imgui_calls engagement count; the outermost body entry also
 * zeroes the counter.
 * @return Calls counted since the counter was last zeroed. */
uint64_t openpete_imgui_calls_take(void);

/* Resolve a mod's ImGui descriptor (the openpete_imgui_desc symbol, see
 * core/include/openpete_imgui.h) against the engine's table and, when the
 * two agree, write the table into the mod's slot. @p desc is that symbol,
 * @p path the object it came from; both strings are used for the log line
 * only. Returns 1 when the mod may run — bound, or no descriptor at all,
 * which means the mod did not opt in — and 0 when the descriptor disagrees
 * with this engine, in which case the mod must be disabled: its table
 * pointer stays null and its first ImGui_* call from any hook would
 * dereference it. */
int openpete_imgui_bind(const void* desc, const char* mod_id,
                              const char* path);

/* The mod whose hook this thread is currently inside, or NULL on the way
 * out. A refusal raised outside a section body has no mod index to name, so
 * the dispatch sites that run mod code — the present-hook loop and the
 * override chain — leave the owner id here for the message. One pointer
 * store per dispatch; the string is the mod's own id and outlives the call. */
void openpete_imgui_guard_set_hook_owner(const char* id);
const char* openpete_imgui_guard_hook_owner(void);

/* Mod id for a refusal message. The loader installs the resolver, which owns
 * the mapping from index to id; until then the index is printed. The returned
 * pointer must stay valid for the process lifetime. */
const char* openpete_imgui_guard_mod_id(int mod_index);
void openpete_imgui_guard_set_mod_id_fn(const char* (*fn)(int mod_index));

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_IMGUI_GUARD_H */
