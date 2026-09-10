/* core/runtime/imgui_guard.c — body-active state behind the mod ImGui table
 * guard wrappers. See openpete_imgui_guard.h. */

#include "openpete_imgui_guard.h"

#include <stdio.h>
#include <string.h>

#include "openpete_imgui.h"   /* the descriptor the loader resolves */
#include "openpete_log.h"

/* Emitted next to the wrappers by tools/gen_imgui_table.py. */
extern const openpete_imgui_table_t openpete_imgui_engine_table;
extern const uint32_t openpete_imgui_table_entries;
extern const uint8_t  openpete_imgui_table_sha256[32];

static _Thread_local int      t_depth = 0;
static _Thread_local int      t_no_input = 0;
static _Thread_local int      t_mod_index = -1;
static _Thread_local uint64_t t_calls = 0;
static _Thread_local const char* t_hook_owner = NULL;

static const char* (*s_mod_id_fn)(int mod_index) = NULL;

/* One-shot filter for the refusal message. A refusing mod calls from a hook
 * that runs every tick or every present, so the pair is remembered rather
 * than the message repeated. Thread-local, so no lock is needed on a path
 * any thread can reach; fixed-size, and stops recording once full, past
 * which the log already names the problem. */
#define SEEN_MAX 64
typedef struct {
    int         mod_index;
    const char* owner;      /* hook owner id when the index is -1 */
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
    openpete_imgui_body_enter_always(mod_index, 0);
}

void openpete_imgui_body_enter_always(int mod_index, int no_input)
{
    if (t_depth == 0) {
        t_mod_index = mod_index;
        t_calls = 0;
        t_no_input = no_input ? 1 : 0;
    }
    t_depth++;
}

void openpete_imgui_body_leave(void)
{
    if (t_depth > 0 && --t_depth == 0) {
        t_mod_index = -1;
        t_no_input = 0;
    }
}

int openpete_imgui_always_closed(void)
{
    return t_depth > 0 && t_no_input;
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

void openpete_imgui_guard_set_hook_owner(const char* id)
{
    t_hook_owner = id;
}

const char* openpete_imgui_guard_hook_owner(void)
{
    return t_hook_owner;
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
    /* Outside a body there is no index; the hook dispatch that is running
     * mod code names the owner instead. */
    const char* owner = (idx < 0) ? t_hook_owner : NULL;
    for (int i = 0; i < t_seen_count; i++) {
        if (t_seen[i].mod_index == idx && t_seen[i].owner == owner &&
            strcmp(t_seen[i].fn, fn) == 0)
            return;
    }
    if (t_seen_count < SEEN_MAX) {
        t_seen[t_seen_count].mod_index = idx;
        t_seen[t_seen_count].owner = owner;
        t_seen[t_seen_count].fn = fn;
        t_seen_count++;
    }
    OPL_ERROR_F("mods", "%s: %s called outside the mod's UI section "
                        "(from a present or tick hook) — dropped",
                (owner && owner[0]) ? owner
                                    : openpete_imgui_guard_mod_id(idx), fn);
}

int openpete_imgui_bind(const void* desc, const char* mod_id,
                              const char* path)
{
    const openpete_imgui_desc_t* d = (const openpete_imgui_desc_t*)desc;
    if (!d) return 1; /* no descriptor: the mod did not opt in */

    if (d->magic != 0x4F504947u || !d->slot) {
        OPL_ERROR_F("mods", "%s: %s exports a malformed ImGui "
                    "descriptor (magic 0x%08x); rebuild the mod against "
                    "this SDK — disabled",
                    mod_id, path, (unsigned)d->magic);
        return 0;
    }

    /* IMGUI_VERSION_NUM is XYYZZ, which does not map back to the dotted
     * string, so the engine names its own version from IMGUI_VERSION and
     * the mod's from the number it was built with. */
    const int same_version =
        d->imgui_version_num == (uint32_t)IMGUI_VERSION_NUM;
    if (!same_version ||
        d->table_size != (uint32_t)sizeof(openpete_imgui_table_t) ||
        memcmp(d->table_sha256, openpete_imgui_table_sha256, 32) != 0) {
        OPL_ERROR_F("mods", "%s: %s was built against ImGui version "
                    "%u (table %u bytes) and this engine has " IMGUI_VERSION
                    " (version %u, %u bytes)%s; a mod bound to a table "
                    "whose slots have moved calls the wrong function on "
                    "its first widget — rebuild the mod against this SDK "
                    "— disabled",
                    mod_id, path, (unsigned)d->imgui_version_num,
                    (unsigned)d->table_size, (unsigned)IMGUI_VERSION_NUM,
                    (unsigned)sizeof(openpete_imgui_table_t),
                    same_version ? ", so the SDK table itself changed" : "");
        return 0;
    }

    *d->slot = &openpete_imgui_engine_table;
    OPL_INFO_F("mods", "%s: ImGui bound (" IMGUI_VERSION
               ", %u entries)",
               mod_id, (unsigned)openpete_imgui_table_entries);
    return 1;
}
