/* openpete_rebind.h - the binding page's requests and an edit's side effects.
 *
 * Every write to the binding store runs on the host loop. The overlay's
 * Controls page (imgui frame thread), the injection script and the pump's
 * Escape cancel post requests here; openpete_rebind_drain, called on the
 * host loop before openpete_keymap_poll, applies them through the keymap's
 * editor (openpete_keymap.h) and turns each result into a log line, a
 * toast per swapped action, settings-writer marks (`keys.<domain>.<id>`)
 * and, when a player-1 pad action changed, a bump of the restamp
 * generation the pad samplers compare against. */
#ifndef OPENPETE_REBIND_H
#define OPENPETE_REBIND_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Post a capture request for one slot of an action; safe from any thread.
 *
 * @param action the action (openpete_action_t).
 * @param slot the source index to replace, or -1 to add.
 * @param hold nonzero to add a hold: source. */
void openpete_rebind_request_capture(int action, int slot, int hold);

/** @brief Post a cancel of the running capture; safe from any thread.
 *
 * @param cause the cancel cause for the log ("escape"). */
void openpete_rebind_request_cancel(const char* cause);

/** @brief Post the removal of one source of an action; safe from any thread.
 *
 * @param action the action.
 * @param slot the source index. */
void openpete_rebind_request_remove(int action, int slot);

/** @brief Post a restore of an action to the binding below the saved layer; safe from any thread.
 *
 * @param action the action. */
void openpete_rebind_request_restore(int action);

/** @brief Apply the queued requests and the result of the capture that last ended; host loop only, before openpete_keymap_poll. */
void openpete_rebind_drain(void);

/** @brief Return the restamp generation, bumped by every edit that changed a player-1 pad action.
 *
 * Safe from any thread.
 * @return the generation. */
unsigned openpete_rebind_restamp_generation(void);

/** @brief Count one pad restamp performed on a sampling thread, for the exit summary.
 *
 * @param on_main nonzero for main's pump loop, zero for the host loop. */
void openpete_rebind_count_restamp(int on_main);

/** @brief Log the session's edit and restamp counts (`rebind: commits=... restamps main=... host=...`). */
void openpete_rebind_log_summary(void);

/** @brief Copy the message of the last edit (committed, refused, cancelled) for the page; safe from any thread.
 *
 * @param out receives the text, "" when no edit ended yet.
 * @param cap capacity of out.
 * @return a counter that changes with every new message. */
unsigned openpete_rebind_last_message(char* out, size_t cap);

/** @brief Settle `keys.<domain>.<id>` ids into the in-memory [saved.keys] zone, without writing the file.
 *
 * A binding the page restored, or whose live sources equal the global
 * layer's in every field and order while no game or disc scope binds the
 * action, loses its saved row; any other edited binding is written.
 * Called by the settings writer on the host loop.
 * @param ids the ids.
 * @param n their count.
 * @return the number of rows settled. */
int openpete_rebind_settle_ids(const char* const* ids, int n);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_REBIND_H */
