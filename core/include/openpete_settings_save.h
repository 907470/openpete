/* openpete_settings_save.h - the one writer of openpete.toml's [saved] zone.
 *
 * The settings menu and the dev overlay never write the file themselves:
 * they add knob ids to a dirty set, and the host loop writes the set through
 * knob_registry_settle_ids, openpete_rebind_settle_ids for the binding ids
 * (`keys.<domain>.<action>`, marked by the binding page at every edit) and
 * one openpete_config_save_flush. The menu marks a row's knob on every change and
 * arms a 500 ms debounce deadline, so stepping through a ladder writes the
 * file once; the overlay's Save settings button enqueues its edited rows
 * with an immediate deadline. The host loop flushes when the deadline
 * passes (openpete_settings_save_poll), when the menu closes and at
 * shutdown (openpete_settings_save_flush).
 *
 * The dirty set is guarded by one mutex, shared by the imgui frame thread
 * (the overlay's enqueue) and the host loop (the menu's marks and every
 * flush). Only the host loop flushes, so the in-memory [saved] zone and the
 * atomic writer's "<path>.tmp-<pid>" temporary have one user.
 *
 * A failed write keeps the dirty set, raises a SAVE FAILED notification,
 * and is retried by the next mark, enqueue, menu close or shutdown. A run
 * without a save target (--no-config) drops the set: its changes last for
 * the session.
 */
#ifndef OPENPETE_SETTINGS_SAVE_H
#define OPENPETE_SETTINGS_SAVE_H

#ifdef __cplusplus
extern "C" {
#endif

/** Milliseconds between a menu change and the write it schedules. */
#define OPENPETE_SETTINGS_SAVE_DEBOUNCE_MS 500u

/** @brief Add a knob to the dirty set and move the write deadline to the debounce interval from now.
 *
 * Called by the settings menu on every change of a persisted row and by the
 * binding page on every edit. An id already in the set keeps one entry.
 * Safe from any thread.
 * @param knob_id Knob registry id or `keys.<domain>.<action>`; NULL or
 *        empty is ignored. */
void openpete_settings_save_mark(const char *knob_id);

/** @brief Add knobs to the dirty set and make the write due at the host loop's next poll.
 *
 * The dev overlay's Save settings button calls it with every row edited
 * this session. Safe from any thread.
 * @param ids Knob registry ids.
 * @param n Number of ids. */
void openpete_settings_save_request(const char *const *ids, int n);

/** @brief Write the dirty set when its deadline has passed.
 *
 * Called by the host loop once per iteration; the only time source is
 * SDL_GetTicks.
 * @return 1 when a write ran and succeeded, -1 when it failed, 0 when none was due. */
int openpete_settings_save_poll(void);

/** @brief Write the dirty set now, whatever its deadline.
 *
 * Called by the host loop when the settings menu closes and at shutdown.
 * @param cause The trigger, printed in the log line ("menu-close", "shutdown").
 * @return 1 when a write ran and succeeded, -1 when it failed, 0 when the set was empty. */
int openpete_settings_save_flush(const char *cause);

/** @brief The writer's counters and last result, for the overlay's tooltip and the log.
 *
 * Any pointer may be NULL.
 * @param writes Receives the number of writes that replaced the file.
 * @param last_rows Receives the rows the last successful write saved or removed.
 * @param failed Receives 1 while the last write failed and its dirty set is retained.
 * @param pending Receives the size of the dirty set. */
void openpete_settings_save_status(unsigned *writes, int *last_rows, int *failed,
                                   int *pending);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_SETTINGS_SAVE_H */
