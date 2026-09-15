// core/renderer/fillpick.c — host-side state for the LP fill-tag inspector (P).
// See openpete_fillpick.h. Generic, address-free; the projection-space pick,
// the tag-table override and the debug tint live in the Spyro terrain feeder
// (games/spyro-1/features/terrain_native.c).

#include "openpete_fillpick.h"
#include "openpete_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FP_MAX 1024

typedef struct {
    int  level, sec, face, verdict;
    int  prev;              /* verdict this record replaced (undo target) */
    char level_name[24];
} fp_entry;

static int      s_active;
static int      s_preview;
static unsigned s_gen = 1;

static openpete_fillpick_hit s_hit;

static fp_entry s_ent[FP_MAX];
static int      s_ent_n;
static int      s_order[FP_MAX];   /* record order — undo pops the tail */
static int      s_order_n;

static char     s_path[1024];
static int      s_path_ready;

int  openpete_fillpick_active(void) { return s_active; }

void openpete_fillpick_set_active(int on) {
    on = on ? 1 : 0;
    if (on == s_active) return;
    s_active = on;
    memset(&s_hit, 0, sizeof s_hit);
    if (s_active) {
        OPL_INFO_F("fillpick",
                   "ON — crosshair picks the LP face under it. "
                   "H preview all LP faces, Y=FILL N=NOFILL B=clear U=undo. "
                   "session -> %s", openpete_fillpick_session_path());
    } else {
        OPL_INFO_F("fillpick", "OFF (%d session verdict(s) held)", s_ent_n);
    }
    /* Preview only draws while engaged, so the tag set changes on both edges. */
    s_gen++;
}

void openpete_fillpick_toggle(void) { openpete_fillpick_set_active(!s_active); }

int  openpete_fillpick_preview(void) { return s_active && s_preview; }

void openpete_fillpick_set_preview(int on) {
    on = on ? 1 : 0;
    if (on == s_preview) return;
    s_preview = on;
    s_gen++;
    OPL_INFO_F("fillpick", "preview (draw untagged LP candidates) %s",
               s_preview ? "ON" : "OFF");
}

unsigned openpete_fillpick_generation(void) { return s_gen; }

void openpete_fillpick_publish(const openpete_fillpick_hit* h) {
    if (h) s_hit = *h; else memset(&s_hit, 0, sizeof s_hit);
}

int openpete_fillpick_get(openpete_fillpick_hit* out) {
    if (out) *out = s_hit;
    return s_hit.valid;
}

static fp_entry* fp_find(int level, int sec, int face) {
    for (int i = 0; i < s_ent_n; i++)
        if (s_ent[i].level == level && s_ent[i].sec == sec && s_ent[i].face == face)
            return &s_ent[i];
    return NULL;
}

int openpete_fillpick_lookup(int level, int sec, int face) {
    if (!s_ent_n) return OPENPETE_FILLPICK_NONE;
    const fp_entry* e = fp_find(level, sec, face);
    return e ? e->verdict : OPENPETE_FILLPICK_NONE;
}

const char* openpete_fillpick_session_path(void) {
    if (!s_path_ready) {
        const char* e = getenv("OPENPETE_FILLPICK_OUT");
        snprintf(s_path, sizeof s_path, "%s", (e && *e) ? e : "fill-tags.session");
        s_path_ready = 1;
    }
    return s_path;
}

static const char* fp_word(int verdict) {
    return verdict == OPENPETE_FILLPICK_FILL   ? "FILL"
         : verdict == OPENPETE_FILLPICK_NOFILL ? "NOFILL"
                                               : "CLEAR";
}

int openpete_fillpick_save(void) {
    const char* path = openpete_fillpick_session_path();
    FILE* f = fopen(path, "w");
    if (!f) {
        OPL_WARN_F("fillpick", "cannot write %s", path);
        return 0;
    }
    fprintf(f, "# openpete fill-tag pick session — verdicts recorded in-game "
               "with the P-key inspector.\n"
               "# Fold into tests/freecam/fill-tags.verdicts (and regenerate\n"
               "# terrain_fill_tags.h) with:  scripts/fill-tags-apply.py %s\n"
               "# verdict level sector face\n", path);
    /* Record order, so the file reads as the session transcript. Cleared rows
     * are held in memory (they defer to the frozen table, and undo steps back
     * through them) but carry no instruction for the merge, so they stay out
     * of the file. */
    for (int k = 0; k < s_order_n; k++) {
        const fp_entry* e = &s_ent[s_order[k]];
        if (e->verdict == OPENPETE_FILLPICK_NONE) continue;
        fprintf(f, "%-6s %s %d %d\n", fp_word(e->verdict), e->level_name,
                e->sec, e->face);
    }
    fclose(f);
    return 1;
}

void openpete_fillpick_mark(int verdict) {
    if (!s_hit.valid) {
        OPL_WARN_F("fillpick", "%s", "nothing under the crosshair");
        return;
    }
    fp_entry* e = fp_find(s_hit.level, s_hit.sec, s_hit.face);
    if (!e) {
        /* Clearing a face that carries no session verdict is already the
         * state being asked for. */
        if (verdict == OPENPETE_FILLPICK_NONE) return;
        if (s_ent_n >= FP_MAX) {
            OPL_WARN_F("fillpick", "session full (%d entries)", FP_MAX);
            return;
        }
        e = &s_ent[s_ent_n];
        memset(e, 0, sizeof *e);
        e->level = s_hit.level; e->sec = s_hit.sec; e->face = s_hit.face;
        e->prev  = OPENPETE_FILLPICK_NONE;
        snprintf(e->level_name, sizeof e->level_name, "%s", s_hit.level_name);
        s_ent_n++;
    } else {
        e->prev = e->verdict;
    }
    e->verdict = verdict;

    /* An entry re-marked in the same session moves to the tail of the record
     * order, so undo always steps back through the visible transcript. */
    int idx = (int)(e - s_ent);
    int w = 0;
    for (int k = 0; k < s_order_n; k++)
        if (s_order[k] != idx) s_order[w++] = s_order[k];
    s_order_n = w;
    s_order[s_order_n++] = idx;

    s_gen++;
    openpete_fillpick_save();
    OPL_INFO_F("fillpick", "%s %s sec=%d face=%d  (%d session verdict(s))",
               fp_word(verdict), s_hit.level_name, s_hit.sec, s_hit.face, s_ent_n);
}

void openpete_fillpick_undo(void) {
    if (!s_order_n) {
        OPL_WARN_F("fillpick", "%s", "nothing to undo");
        return;
    }
    int idx = s_order[--s_order_n];
    fp_entry* e = &s_ent[idx];
    char name[24]; snprintf(name, sizeof name, "%s", e->level_name);
    int sec = e->sec, face = e->face, prev = e->prev;
    if (prev == OPENPETE_FILLPICK_NONE) {
        int last = s_ent_n - 1;
        if (idx != last) s_ent[idx] = s_ent[last];
        s_ent_n--;
        for (int k = 0; k < s_order_n; k++)
            if (s_order[k] == last) s_order[k] = idx;
    } else {
        e->verdict = prev;
        e->prev = OPENPETE_FILLPICK_NONE;
        s_order[s_order_n++] = idx;   /* still a live record, just older */
    }
    s_gen++;
    openpete_fillpick_save();
    OPL_INFO_F("fillpick", "undo -> %s %s sec=%d face=%d",
               fp_word(prev), name, sec, face);
}

int openpete_fillpick_count(void) { return s_ent_n; }

int openpete_fillpick_entry(int idx, int* level, const char** level_name,
                            int* sec, int* face, int* verdict) {
    if (idx < 0 || idx >= s_order_n) return 0;
    const fp_entry* e = &s_ent[s_order[idx]];
    if (level)      *level      = e->level;
    if (level_name) *level_name = e->level_name;
    if (sec)        *sec        = e->sec;
    if (face)       *face       = e->face;
    if (verdict)    *verdict    = e->verdict;
    return 1;
}
