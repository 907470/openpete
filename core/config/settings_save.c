/* core/config/settings_save.c - the one writer of openpete.toml's [saved]
 * zone; see openpete_settings_save.h. */

#include "openpete_settings_save.h"
#include "openpete_config.h"     /* openpete_config_has_save_target */
#include "openpete_knobs.h"      /* knob_registry_save_ids */
#include "openpete_log.h"
#include "openpete_mutex_once.h"
#include "openpete_osd.h"
#include "openpete_rebind.h"     /* openpete_rebind_settle_ids */

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#define SAVE_DIRTY_MAX 64
#define SAVE_ID_MAX    64

/* One dirty knob. `gen` is the mark count when the id was last marked: a
 * flush clears only the entries whose gen it copied, so a mark landing
 * while the file is being written stays dirty for the next write. */
typedef struct { char id[SAVE_ID_MAX]; unsigned gen; } dirty_t;

static void    *s_mx;                 /* openpete_mutex_once slot */
static dirty_t  s_dirty[SAVE_DIRTY_MAX];
static int      s_dirty_n;
static unsigned s_gen;
static Uint64   s_deadline;           /* SDL_GetTicks ms; 0 = no write due */
static unsigned s_writes;
static int      s_last_rows;
static int      s_failed;

/** @brief Add one id to the dirty set, or refresh its generation when present; the caller holds the mutex.
 *
 * An id that does not fit SAVE_ID_MAX or a full set is logged and dropped.
 * @param id The knob id. */
static void dirty_add_locked(const char *id) {
    if (!id || !*id) return;
    if (strlen(id) >= SAVE_ID_MAX) {
        OPL_WARN_F("settings", "knob id too long to save: %s", id);
        return;
    }
    s_gen++;
    for (int i = 0; i < s_dirty_n; i++) {
        if (strcmp(s_dirty[i].id, id) != 0) continue;
        s_dirty[i].gen = s_gen;
        return;
    }
    if (s_dirty_n >= SAVE_DIRTY_MAX) {
        OPL_WARN_F("settings", "too many unsaved settings; %s not saved", id);
        return;
    }
    snprintf(s_dirty[s_dirty_n].id, SAVE_ID_MAX, "%s", id);
    s_dirty[s_dirty_n].gen = s_gen;
    s_dirty_n++;
}

void openpete_settings_save_mark(const char *knob_id) {
    SDL_Mutex *mx = openpete_mutex_once(&s_mx);
    SDL_LockMutex(mx);
    dirty_add_locked(knob_id);
    Uint64 due = SDL_GetTicks() + OPENPETE_SETTINGS_SAVE_DEBOUNCE_MS;
    s_deadline = due ? due : 1;
    SDL_UnlockMutex(mx);
}

void openpete_settings_save_request(const char *const *ids, int n) {
    SDL_Mutex *mx = openpete_mutex_once(&s_mx);
    SDL_LockMutex(mx);
    for (int i = 0; i < n; i++) dirty_add_locked(ids[i]);
    s_deadline = 1;   /* due at the next poll */
    SDL_UnlockMutex(mx);
}

/** @brief Settle the dirty knobs and bindings, write the file once, and settle the set by the result.
 *
 * The set is copied under the mutex and written without it, so a mark from
 * the imgui frame thread never waits on the file. On success the copied
 * entries whose generation is unchanged leave the set; on failure the set
 * stays whole, the deadline is cleared so the failure is not retried every
 * poll, and a SAVE FAILED notification is raised. Without a save target the
 * set is dropped.
 * @param cause The trigger, for the log line.
 * @return 1 on a successful write, -1 on a failed one, 0 when the set was empty. */
static int flush_now(const char *cause) {
    SDL_Mutex *mx = openpete_mutex_once(&s_mx);
    dirty_t copy[SAVE_DIRTY_MAX];
    SDL_LockMutex(mx);
    const int n = s_dirty_n;
    memcpy(copy, s_dirty, (size_t)n * sizeof copy[0]);
    s_deadline = 0;
    if (n > 0 && !openpete_config_has_save_target()) s_dirty_n = 0;
    SDL_UnlockMutex(mx);
    if (n == 0) return 0;
    if (!openpete_config_has_save_target()) {
        OPL_INFO_F("settings", "no config file to save to; %d change(s) last for this session", n);
        return 0;
    }

    /* Knob ids and binding ids (`keys.<domain>.<action>`) settle into the
     * in-memory zone separately; the file is written once. */
    const char *knob_ids[SAVE_DIRTY_MAX], *key_ids[SAVE_DIRTY_MAX];
    int nk = 0, nb = 0;
    for (int i = 0; i < n; i++) {
        if (!strncmp(copy[i].id, "keys.", 5)) key_ids[nb++] = copy[i].id;
        else knob_ids[nk++] = copy[i].id;
    }
    int rows = knob_registry_settle_ids(knob_ids, nk) +
               openpete_rebind_settle_ids(key_ids, nb);
    /* The file is written whenever a row was settled, changed in memory or
     * not: a removal that a failed write left only in memory reaches disk
     * on the retry. */
    if (rows > 0 && openpete_config_save_flush() != 0) rows = -1;

    SDL_LockMutex(mx);
    if (rows >= 0) {
        int keep = 0;
        for (int i = 0; i < s_dirty_n; i++) {
            int written = 0;
            for (int j = 0; j < n && !written; j++)
                written = !strcmp(s_dirty[i].id, copy[j].id) &&
                          s_dirty[i].gen == copy[j].gen;
            if (!written) s_dirty[keep++] = s_dirty[i];
        }
        s_dirty_n = keep;
        if (rows > 0) s_writes++;   /* 0: no id could be persisted, nothing written */
        s_last_rows = rows;
        s_failed = 0;
    } else {
        s_failed = 1;
    }
    const unsigned writes = s_writes;
    SDL_UnlockMutex(mx);

    if (rows >= 0) {
        OPL_INFO_F("settings", "saved cause=%s ids=%d rows=%d writes=%u path=%s",
                   cause, n, rows, writes, openpete_config_save_path());
        return 1;
    }
    OPL_ERROR_F("settings", "save FAILED cause=%s ids=%d path=%s; changes kept for the next save",
                cause, n, openpete_config_save_path());
    openpete_osd_notify("host", "menu.save", 0, "SAVE FAILED");
    return -1;
}

int openpete_settings_save_poll(void) {
    SDL_Mutex *mx = openpete_mutex_once(&s_mx);
    SDL_LockMutex(mx);
    const Uint64 due = s_deadline;
    SDL_UnlockMutex(mx);
    if (due == 0 || SDL_GetTicks() < due) return 0;
    return flush_now("debounce");
}

int openpete_settings_save_flush(const char *cause) {
    return flush_now(cause ? cause : "flush");
}

void openpete_settings_save_status(unsigned *writes, int *last_rows, int *failed,
                                   int *pending) {
    SDL_Mutex *mx = openpete_mutex_once(&s_mx);
    SDL_LockMutex(mx);
    if (writes)    *writes = s_writes;
    if (last_rows) *last_rows = s_last_rows;
    if (failed)    *failed = s_failed;
    if (pending)   *pending = s_dirty_n;
    SDL_UnlockMutex(mx);
}
