/* core/runtime/imgui_guard.c — body-active state behind the ImGui annex
 * guard wrappers. See openpete_imgui_guard.h. */

#include "openpete_imgui_guard.h"

#include <stdio.h>
#include <string.h>

#include "openpete_log.h"

static _Thread_local int      t_depth = 0;
static _Thread_local int      t_mod_index = -1;
static _Thread_local uint64_t t_calls = 0;

static const char* (*s_mod_id_fn)(int mod_index) = NULL;

/* One-shot filter for the refusal message. A refusing mod calls from a hook
 * that runs every tick or every present, so the pair is remembered rather
 * than the message repeated. Thread-local, so no lock is needed on a path
 * any thread can reach; fixed-size, and stops recording once full, past
 * which the log already names the problem. */
#define SEEN_MAX 64
typedef struct {
    int         mod_index;
    const char* fn;         /* generated string literal, never freed */
} seen_entry_t;
static _Thread_local seen_entry_t t_seen[SEEN_MAX];
static _Thread_local int         t_seen_count = 0;

int openpete_imgui_body_active(void)
{
    return t_depth > 0;
}

void openpete_imgui_body_enter(int mod_index)
{
    if (t_depth == 0) {
        t_mod_index = mod_index;
        t_calls = 0;
    }
    t_depth++;
}

void openpete_imgui_body_leave(void)
{
    if (t_depth > 0 && --t_depth == 0)
        t_mod_index = -1;
}

uint64_t openpete_imgui_calls_take(void)
{
    uint64_t n = t_calls;
    t_calls = 0;
    return n;
}

void openpete_imgui_count_call(void)
{
    t_calls++;
}

void openpete_imgui_guard_set_mod_id_fn(const char* (*fn)(int mod_index))
{
    s_mod_id_fn = fn;
}

const char* openpete_imgui_guard_mod_id(int mod_index)
{
    if (s_mod_id_fn) {
        const char* id = s_mod_id_fn(mod_index);
        if (id && id[0])
            return id;
    }
    /* No resolver installed, or the index is not a loaded mod. Ring of
     * buffers so two refusals in one log line still read correctly. */
    static _Thread_local char bufs[4][32];
    static _Thread_local unsigned next = 0;
    char* buf = bufs[next++ & 3u];
    snprintf(buf, sizeof bufs[0], "mod #%d", mod_index);
    return buf;
}

void openpete_imgui_guard_refused(const char* fn)
{
    int idx = t_mod_index;
    for (int i = 0; i < t_seen_count; i++) {
        if (t_seen[i].mod_index == idx && strcmp(t_seen[i].fn, fn) == 0)
            return;
    }
    if (t_seen_count < SEEN_MAX) {
        t_seen[t_seen_count].mod_index = idx;
        t_seen[t_seen_count].fn = fn;
        t_seen_count++;
    }
    OPL_ERROR_F("mods", "%s: %s called outside the mod's UI section "
                        "(from a present or tick hook) — dropped",
                openpete_imgui_guard_mod_id(idx), fn);
}
