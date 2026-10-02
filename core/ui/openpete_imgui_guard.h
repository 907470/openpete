/* core/ui/openpete_imgui_guard.h - containment for the mod ImGui table.
 *
 * Every entry of the engine's ImGui table is a generated wrapper
 * (openpete_imgui_guard.cpp, from tools/gen_imgui_table.py) that asks
 * openpete_imgui_body_active() where it was called from. Inside a mod UI
 * section body the call goes through and is counted; anywhere else - a
 * present hook on the extracting thread, a tick hook on the sim thread - it
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

/** @brief Report whether this thread is inside a mod UI section body.
 *
 * Queried by every generated wrapper before it forwards a call to ImGui.
 * @return Non-zero while the thread-local body depth is above zero. */
int openpete_imgui_body_active(void);

/** @brief Log a wrapper's refusal of an ImGui call made outside a section body.
 *
 * Logs OPL_ERROR once per (mod index, hook owner, function name) so a mod
 * calling from the wrong place reports itself without flooding the log.
 * Outside a body the message names the hook owner set by
 * openpete_imgui_guard_set_hook_owner, else the mod id from
 * openpete_imgui_guard_mod_id. The per-thread filter holds 64 pairs; past
 * that, new pairs log every time.
 * @param fn The refused ImGui function's name, a string literal. */
void openpete_imgui_guard_refused(const char* fn);

/** @brief Count one call that reached ImGui, for the engagement counter.
 *
 * Adds one to this thread's counter, read by openpete_imgui_calls_take. */
void openpete_imgui_count_call(void);

/** @brief Enter a mod UI section body on this thread.
 *
 * Equivalent to openpete_imgui_body_enter_always() with no_input 0. Raises
 * the thread-local depth. The outermost entry records @p mod_index and
 * zeroes the call counter; nested entries only raise the depth, so a body
 * that re-enters keeps the depth balanced. Paired with
 * openpete_imgui_body_leave().
 * @param mod_index Index of the mod whose body runs; refusal messages name
 *        it through openpete_imgui_guard_mod_id(). */
void openpete_imgui_body_enter(int mod_index);
/** @brief Leave a mod UI section body on this thread.
 *
 * Lowers the depth; the outermost leave clears the mod index and the
 * no-input bit. A leave at depth 0 does nothing. */
void openpete_imgui_body_leave(void);

/** @brief Enter a mod UI section body, marking whether it draws with the dev overlay closed.
 *
 * The loader's section dispatch brackets every body with this and
 * openpete_imgui_body_leave(). The outermost entry records @p mod_index,
 * zeroes the call counter, and latches @p no_input for the depth of the
 * body; nested entries only raise the depth.
 * @param mod_index Index of the mod whose body runs.
 * @param no_input Non-zero for a body the always pass runs with the dev
 *        overlay closed, read by openpete_imgui_always_closed(). */
void openpete_imgui_body_enter_always(int mod_index, int no_input);

/** @brief Report whether this thread is inside an always-section body drawn with the overlay closed.
 *
 * The ImGui_Begin and ImGui_BeginChild wrappers OR ImGuiWindowFlags_NoInputs
 * into their flags argument on it, so a window a mod draws during play never
 * takes the mouse the game is reading.
 * @return Non-zero inside a body entered with no_input set. */
int openpete_imgui_always_closed(void);

/** @brief Return and zero this thread's count of ImGui calls that reached ImGui.
 *
 * The mod loader calls it after a section body and stores the result as
 * that mod's imgui_calls engagement count; the outermost body entry also
 * zeroes the counter.
 * @return Calls counted since the counter was last zeroed. */
uint64_t openpete_imgui_calls_take(void);

/** @brief Bind a mod's ImGui descriptor to the engine's ImGui table.
 *
 * Checks the descriptor (the openpete_imgui_desc symbol, see
 * core/include/openpete_imgui.h) for its magic and slot, then for the same
 * IMGUI_VERSION_NUM, table size, and table SHA-256 as the engine; when all
 * agree it writes the engine table into the mod's slot. On a mismatch the
 * mod must be disabled: its table pointer stays null and its first ImGui_*
 * call from any hook would dereference it.
 * @param desc The mod's descriptor symbol; NULL means the mod did not opt in.
 * @param mod_id The mod's id, for the log line only.
 * @param path The object the descriptor came from, for the log line only.
 * @return 1 when the mod may run (bound, or no descriptor), 0 when the
 *         descriptor is malformed or disagrees with this engine. */
int openpete_imgui_bind(const void* desc, const char* mod_id,
                              const char* path);

/** @brief Record the mod whose hook this thread is about to run, for refusal messages.
 *
 * A refusal raised outside a section body has no mod index to name, so the
 * dispatch sites that run mod code set the owner id here around each call:
 * the present-hook loop sets it and clears it to NULL after, and the
 * override chain restores the previous owner after, since its descent
 * nests. The string is the mod's own id and outlives the call.
 * @param id The owning mod's id, or NULL. */
void openpete_imgui_guard_set_hook_owner(const char* id);
/** @brief Read the owner id last set on this thread.
 *
 * @return The id, or NULL outside any hook dispatch. */
const char* openpete_imgui_guard_hook_owner(void);

/** @brief Name a mod for a refusal message.
 *
 * Asks the resolver the loader installs, which owns the mapping from index
 * to id; with no resolver, or a NULL or empty id, it formats "mod #<index>"
 * into one of four thread-local buffers so two names in one log line both
 * stay valid.
 * @param mod_index The mod's index.
 * @return The mod's id, or the formatted fallback. */
const char* openpete_imgui_guard_mod_id(int mod_index);
/** @brief Install the resolver from mod index to mod id.
 *
 * @param fn The resolver; each id it returns must stay valid for the process
 *        lifetime. */
void openpete_imgui_guard_set_mod_id_fn(const char* (*fn)(int mod_index));

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_IMGUI_GUARD_H */
