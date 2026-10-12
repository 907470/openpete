/* core/input/rebind.c - the binding page's request queue and an edit's side
 * effects; see openpete_rebind.h. */

#include "openpete_rebind.h"

#include "openpete_config.h"        /* openpete_config_save_binding */
#include "openpete_fiber.h"         /* openpete_frame, for the log lines */
#include "openpete_keymap.h"
#include "openpete_log.h"
#include "openpete_mutex_once.h"
#include "openpete_osd.h"
#include "openpete_settings_save.h"

#include <SDL3/SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef enum { REQ_CAPTURE, REQ_CANCEL, REQ_REMOVE, REQ_RESTORE } req_kind_t;

typedef struct {
    req_kind_t kind;
    int        action;
    int        slot;
    int        hold;
    char       cause[32];
} req_t;

#define REBIND_QUEUE_CAP 16

static void*         s_mx;                 /* openpete_mutex_once slot */
static req_t         s_q[REBIND_QUEUE_CAP];
static int           s_q_n;
static char          s_msg[224];
static unsigned      s_msg_seq;
static SDL_AtomicInt s_restamp_gen;
static SDL_AtomicInt s_restamps_main;
static SDL_AtomicInt s_restamps_host;
static unsigned      s_commits;
static unsigned      s_refusals;
static unsigned      s_cancels;

/** @brief Append a request to the queue under the mutex; a full queue drops it with a warning. */
static void post(const req_t* r) {
    SDL_Mutex* mx = openpete_mutex_once(&s_mx);
    SDL_LockMutex(mx);
    if (s_q_n < REBIND_QUEUE_CAP) s_q[s_q_n++] = *r;
    else OPL_WARN_F("keys", "rebind: request queue full; request dropped");
    SDL_UnlockMutex(mx);
}

void openpete_rebind_request_capture(int action, int slot, int hold) {
    req_t r = { REQ_CAPTURE, action, slot, hold ? 1 : 0, "" };
    post(&r);
}

void openpete_rebind_request_cancel(const char* cause) {
    req_t r = { REQ_CANCEL, -1, -1, 0, "" };
    snprintf(r.cause, sizeof r.cause, "%s", cause ? cause : "cancel");
    post(&r);
}

void openpete_rebind_request_remove(int action, int slot) {
    req_t r = { REQ_REMOVE, action, slot, 0, "" };
    post(&r);
}

void openpete_rebind_request_restore(int action) {
    req_t r = { REQ_RESTORE, action, -1, 0, "" };
    post(&r);
}

/** @brief Write an action's config name, "pad.cross" or "host.pause". */
static void action_name(int a, char* out, size_t cap) {
    openpete_keymap_action_info_t info;
    if (!openpete_keymap_action_info((openpete_action_t)a, &info)) {
        snprintf(out, cap, "?");
        return;
    }
    snprintf(out, cap, "%s.%s", openpete_keymap_domain_name((openpete_action_t)a), info.id);
}

/** @brief Store the page's message under the mutex and advance its counter. */
static void set_message(const char* text) {
    SDL_Mutex* mx = openpete_mutex_once(&s_mx);
    SDL_LockMutex(mx);
    snprintf(s_msg, sizeof s_msg, "%s", text);
    s_msg_seq++;
    SDL_UnlockMutex(mx);
}

unsigned openpete_rebind_last_message(char* out, size_t cap) {
    SDL_Mutex* mx = openpete_mutex_once(&s_mx);
    SDL_LockMutex(mx);
    if (out && cap) snprintf(out, cap, "%s", s_msg);
    const unsigned seq = s_msg_seq;
    SDL_UnlockMutex(mx);
    return seq;
}

/** @brief Mark an action's binding for the settings writer as `keys.<domain>.<id>`. */
static void mark_dirty(int a) {
    openpete_keymap_action_info_t info;
    if (!openpete_keymap_action_info((openpete_action_t)a, &info)) return;
    char id[96];
    snprintf(id, sizeof id, "keys.%s.%s", openpete_keymap_domain_name((openpete_action_t)a),
             info.id);
    openpete_settings_save_mark(id);
}

/** @brief Raise the toast naming what a swap did to a conflicting action, in the toast font's capitals. */
static void toast_swap(int b) {
    openpete_keymap_action_info_t info;
    if (!openpete_keymap_action_info((openpete_action_t)b, &info)) return;
    char label[128], text[224];
    openpete_keymap_action_label((openpete_action_t)b, label, sizeof label);
    snprintf(text, sizeof text, "%s: now %s", info.purpose, label);
    for (char* p = text; *p; p++) *p = (char)toupper((unsigned char)*p);
    openpete_osd_notify("host", "keys.swap", 0, "%s", text);
}

/** @brief Turn one edit result into its log lines, toasts, save marks, page message and restamp bump. */
static void handle(const openpete_keymap_edit_t* e) {
    char name[96], msg[224];
    action_name(e->action, name, sizeof name);
    switch (e->kind) {
    case KM_EDIT_COMMITTED:
        s_commits++;
        OPL_INFO_F("keys", "rebind commit %s %s frame=%u", name, e->token, openpete_frame);
        snprintf(msg, sizeof msg, "%s: %s bound", name, e->token);
        break;
    case KM_EDIT_REMOVED:
        OPL_INFO_F("keys", "rebind remove %s %s frame=%u", name, e->token, openpete_frame);
        snprintf(msg, sizeof msg, "%s: %s removed", name, e->token);
        break;
    case KM_EDIT_RESTORED:
        OPL_INFO_F("keys", "rebind restore %s to %s frame=%u", name, e->token, openpete_frame);
        snprintf(msg, sizeof msg, "%s: restored to %s", name, e->token);
        break;
    case KM_EDIT_REFUSED:
        s_refusals++;
        OPL_INFO_F("keys", "rebind refused %s frame=%u: %s", name, openpete_frame, e->reason);
        snprintf(msg, sizeof msg, "Not changed: %s", e->reason);
        set_message(msg);
        return;
    case KM_EDIT_CANCELLED:
        s_cancels++;
        OPL_INFO_F("keys", "rebind cancel %s frame=%u cause=%s", name, openpete_frame, e->reason);
        snprintf(msg, sizeof msg, "Cancelled (%s)", e->reason);
        set_message(msg);
        return;
    default:
        return;
    }
    for (int i = 0; i < e->n_changed; i++) {
        mark_dirty(e->changed[i].action);
        if (i == 0) continue;
        char b[96];
        action_name(e->changed[i].action, b, sizeof b);
        OPL_INFO_F("keys", "rebind swap %s %s -> %s", b, e->changed[i].old_tok,
                   e->changed[i].new_tok);
        toast_swap(e->changed[i].action);
    }
    if (e->pad_changed) SDL_AddAtomicInt(&s_restamp_gen, 1);
    set_message(msg);
}

/** @brief Apply one queued request through the keymap's editor. */
static void apply(const req_t* r) {
    openpete_keymap_edit_t e;
    memset(&e, 0, sizeof e);
    char name[96];
    switch (r->kind) {
    case REQ_CAPTURE:
        action_name(r->action, name, sizeof name);
        if (openpete_keymap_capture_begin((openpete_action_t)r->action, r->slot, r->hold,
                                          &e) == 0) {
            OPL_INFO_F("keys", "rebind capture %s slot=%d hold=%d frame=%u", name, r->slot, r->hold, openpete_frame);
            set_message("");
        } else {
            handle(&e);
        }
        break;
    case REQ_CANCEL:
        openpete_keymap_capture_cancel(r->cause);
        if (openpete_keymap_capture_take_result(&e)) handle(&e);
        break;
    case REQ_REMOVE:
        (void)openpete_keymap_remove_source((openpete_action_t)r->action, r->slot, &e);
        handle(&e);
        break;
    case REQ_RESTORE:
        (void)openpete_keymap_restore((openpete_action_t)r->action, &e);
        handle(&e);
        break;
    }
}

void openpete_rebind_drain(void) {
    openpete_keymap_edit_t e;
    if (openpete_keymap_capture_take_result(&e)) handle(&e);

    req_t q[REBIND_QUEUE_CAP];
    SDL_Mutex* mx = openpete_mutex_once(&s_mx);
    SDL_LockMutex(mx);
    const int n = s_q_n;
    memcpy(q, s_q, sizeof(req_t) * (size_t)n);
    s_q_n = 0;
    SDL_UnlockMutex(mx);
    for (int i = 0; i < n; i++) apply(&q[i]);
}

unsigned openpete_rebind_restamp_generation(void) {
    return (unsigned)SDL_GetAtomicInt(&s_restamp_gen);
}

void openpete_rebind_count_restamp(int on_main) {
    SDL_AddAtomicInt(on_main ? &s_restamps_main : &s_restamps_host, 1);
}

void openpete_rebind_log_summary(void) {
    OPL_INFO_F("keys", "rebind: commits=%u refusals=%u cancels=%u restamps main=%d host=%d",
               s_commits, s_refusals, s_cancels, SDL_GetAtomicInt(&s_restamps_main),
               SDL_GetAtomicInt(&s_restamps_host));
}

/** @brief Return whether two source lists are equal in every field of every source, in order. */
static int sources_equal(const km_source_t* x, int nx, const km_source_t* y, int ny) {
    if (nx != ny) return 0;
    for (int i = 0; i < nx; i++)
        if (x[i].kind != y[i].kind || x[i].code != y[i].code || x[i].invert != y[i].invert ||
            x[i].device != y[i].device || x[i].hold != y[i].hold)
            return 0;
    return 1;
}

/** @brief Settle one binding id into the in-memory [saved.keys] zone.
 *
 * @param id "keys.<domain>.<action>".
 * @return 1 when a row was settled, 0 for an id that names no action. */
static int settle_one(const char* id) {
    char domain[32];
    const char* dot = strchr(id + 5, '.');
    if (!dot || (size_t)(dot - (id + 5)) >= sizeof domain) return 0;
    memcpy(domain, id + 5, (size_t)(dot - (id + 5)));
    domain[dot - (id + 5)] = '\0';
    const char* act = dot + 1;
    const int a = openpete_keymap_find_action(domain, act);
    if (a < 0) return 0;

    km_source_t live[KM_MAX_SOURCES], glob[KM_MAX_SOURCES];
    const int n = openpete_keymap_sources((openpete_action_t)a, live, KM_MAX_SOURCES);
    int scoped = 0;
    const int ng = openpete_keymap_global_layer((openpete_action_t)a, glob, KM_MAX_SOURCES,
                                                &scoped);
    if (!openpete_keymap_has_saved((openpete_action_t)a) ||
        (!scoped && sources_equal(live, n, glob, ng))) {
        (void)openpete_config_unsave_binding(domain, act);
        openpete_keymap_clear_saved((openpete_action_t)a);
        return 1;
    }
    char bufs[KM_MAX_SOURCES][64];
    const char* toks[KM_MAX_SOURCES];
    for (int i = 0; i < n; i++) {
        openpete_keymap_source_token(&live[i], bufs[i], sizeof bufs[i]);
        toks[i] = bufs[i];
    }
    return openpete_config_save_binding(domain, act, toks, n) == 0 ? 1 : 0;
}

int openpete_rebind_settle_ids(const char* const* ids, int n) {
    int rows = 0;
    for (int i = 0; i < n; i++)
        if (ids[i] && !strncmp(ids[i], "keys.", 5)) rows += settle_one(ids[i]);
    return rows;
}
