#ifndef OPENPETE_KNOBS_H
#define OPENPETE_KNOBS_H

/*
 * Knob registry — the table behind the M-key dev config overlay.
 *
 * One row per config knob (CLI flag and/or OPENPETE_* env var): what it is,
 * where its boot value came from (CLI beats env beats default — the same
 * per-knob sentinel-unset precedence the consumers implement), and, for the
 * knobs proven runtime-safe, how to read/write the live value.
 *
 * Deliberately NOT a reflection system: rows are added by editing
 * knob_registry.c (a compile-time diff), values flow through two tiny
 * function pointers per row, and everything imgui-specific stays in
 * devui.cpp. The registry itself is plain C and UI-free.
 *
 * Mutability contract (the runtime-safety audit's output, enforced here):
 *  - KNOB_LIVE rows route every write through a public setter that touches
 *    renderer/host state only — never gameplay-readable RAM (north-star).
 *  - KNOB_RESTART_ONLY rows have set_num/set_str == NULL and render disabled;
 *    they still display the resolved boot value + source. Promoting one to
 *    LIVE requires auditing every consumer for boot-time latching first
 *    (see the plan: a single latched consumer means LIVE_FIXUP, not LIVE).
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct openpete_options;   /* openpete.h */

typedef enum {
    KNOB_BOOL,     /* value_num 0/1 */
    KNOB_INT,      /* value_num integral */
    KNOB_FLOAT,    /* value_num */
    KNOB_ENUM,     /* value_num = index into enum_values */
    KNOB_STRING,   /* get_str/set_str */
    KNOB_ACTION    /* one-shot: a button, not a value (see below) */
} knob_type_t;

/* KNOB_ACTION rows carry no value — they fire something once (apply a cheat,
 * reset a counter). The row renders as a button captioned `action_label`
 * ("Apply" when NULL), and:
 *   set_num(k, 1)  fires it; returns 0 when the request was accepted.
 *   get_num        optional gate: 0 => the action can't run right now and the
 *                  button renders disabled (e.g. "not in a level yet").
 *   get_str        optional short status shown beside the button.
 * An action that touches gameplay state must not write it from here — the UI
 * runs outside the sim. Queue a request and let the game's own per-tick hook
 * perform the write at a tick boundary (north-star: gameplay state changes
 * only on the sim's schedule). */

typedef enum {
    KNOB_LIVE,          /* editable; writes go through the row's setter */
    KNOB_RESTART_ONLY   /* display-only: value applies at next launch */
} knob_mut_t;

typedef enum {
    KNOB_SRC_DEFAULT,   /* built-in default (neither CLI nor env supplied) */
    KNOB_SRC_ENV,       /* env var (incl. openpete.env, merged at boot) */
    KNOB_SRC_CLI        /* explicit flag (incl. openpete.opts / bk2 splices) */
} knob_src_t;

#define KNOB_STR_MAX 256

typedef struct knob_desc {
    const char *id;        /* stable slug; imgui ID scope */
    const char *label;     /* display name */
    const char *section;   /* collapsing-header group */
    const char *cli;       /* "--flag" or NULL (env-only knob) */
    const char *env;       /* "OPENPETE_*" or NULL (CLI-only knob) */
    const char *help;      /* tooltip: what it does + apply-cost caveats */

    knob_type_t type;
    knob_mut_t  mut;

    const char *const *enum_values;  /* KNOB_ENUM: display strings */
    int    enum_count;
    float  num_min, num_max;         /* numeric widget range (both 0 = free) */

    /* Live value plumbing. Numeric types use get_num/set_num (ENUM = index);
     * strings use get_str/set_str. set_* NULL == not editable (RESTART_ONLY).
     * get_num should read LIVE state where a getter exists (a knob can also
     * change via its host hotkey — L, F, Alt+Enter…), else the registry's
     * stored copy. set_* returns 0 on success. */
    double (*get_num)(const struct knob_desc *k);
    int    (*set_num)(struct knob_desc *k, double v);
    void   (*get_str)(const struct knob_desc *k, char *buf, size_t cap);
    int    (*set_str)(struct knob_desc *k, const char *v);

    /* Stamped once by knob_registry_init (resolution-order replay). */
    knob_src_t boot_src;
    double     boot_num;
    char       boot_str[KNOB_STR_MAX];

    /* Registry-owned current value for rows without a live getter. */
    double stored_num;

    /* Built-in default as a display string (env rows: what an unset var
     * resolves to — shown as "(unset - <def>)"). NULL/empty = unknown. */
    const char *def_str;

    /* KNOB_ACTION only: button caption ("Apply" when NULL). */
    const char *action_label;

    /* Optional: 0 = leave this row out of the overlay for this run. For knobs
     * that act on something the run does not have — a capture that was never
     * started, a device that is absent — where a disabled row would just be a
     * question the user has to answer twice. The row still exists everywhere
     * else (--config-report, the config schema, saving), so this is display
     * only. NULL = always shown. A section whose rows are all hidden emits no
     * header. */
    int (*visible)(const struct knob_desc *k);
} knob_desc_t;

/* The table (mutable: boot stamps + stored values live in the rows). */
knob_desc_t *knob_registry(size_t *count);

/* Resolve boot values + sources from the parsed options + environment.
 * Call once, after openpete_options_parse, before the overlay first draws.
 * Keeps the opts pointer (main's options outlive the process loop). */
void knob_registry_init(const struct openpete_options *opts);

/* Convenience for the UI: current value of any row as a display string. */
void knob_value_str(const knob_desc_t *k, char *buf, size_t cap);

/* The row's current value in the vocabulary openpete.toml stores — which is
 * the CLI's vocabulary, not the overlay's. Mostly the same string, but not
 * always: the SSAA row displays "4x" where the flag wants "4". Returns 0 when
 * the row can be persisted at all (it has a config schema entry and a real
 * value; action rows and placeholder strings like "(default)" cannot). */
int knob_config_value_str(const knob_desc_t *k, char *buf, size_t cap);

/* Persist every row the user changed this session into openpete.toml's
 * managed [saved] zone, and write the file. Returns the number of settings
 * saved, or -1 if the write failed. Rows currently overridden by a launch
 * tier are still saved — with the UI telling the user so, since the override
 * will keep winning while it is present. */
int knob_registry_save_edited(void);

/* Re-stamp boot values that only settle after init (the render-fps pacer
 * resolves its launch target at host-loop start, after the registry seeds).
 * The overlay calls this once, on its first open. */
void knob_registry_late_stamp(void);

/* Late-bind a live string channel onto an existing row. Some knobs' setters
 * live in game feature modules the core library cannot link against; the
 * module attaches them here (apply-options time) and the row flips to
 * KNOB_LIVE. Either pointer may be NULL to keep the row's own. Returns 0 if
 * the id was found (it won't be when the row is compiled out, e.g. Debug
 * rows under OPENPETE_HIDE_DEBUG_FLAGS — callers just ignore that). */
/* Contribute a row the core table cannot name — a game module's own knobs
 * (per-game cheats, per-game toggles). The row is COPIED, so the caller's
 * knob_desc_t may be a stack temporary, but every string it points at must
 * outlive the process (literals or module statics). Rows land after the last
 * existing row of their `section` (so they join that collapsing header), or
 * start a new section at the end.
 *
 * Call at feature apply-options time — before the overlay first draws and
 * before knob_registry_init stamps boot values. Returns 0 on success, -1 when
 * the extra-row capacity is exhausted. */
int openpete_knob_add(const knob_desc_t *row);

int openpete_knob_bind_str(const char *id,
                           void (*get_str)(const struct knob_desc *k,
                                           char *buf, size_t cap),
                           int (*set_str)(struct knob_desc *k, const char *v));

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_KNOBS_H */
