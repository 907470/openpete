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
    KNOB_STRING    /* get_str/set_str */
} knob_type_t;

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
} knob_desc_t;

/* The table (mutable: boot stamps + stored values live in the rows). */
knob_desc_t *knob_registry(size_t *count);

/* Resolve boot values + sources from the parsed options + environment.
 * Call once, after openpete_options_parse, before the overlay first draws.
 * Keeps the opts pointer (main's options outlive the process loop). */
void knob_registry_init(const struct openpete_options *opts);

/* Convenience for the UI: current value of any row as a display string. */
void knob_value_str(const knob_desc_t *k, char *buf, size_t cap);

/* Re-stamp boot values that only settle after init (the render-fps pacer
 * resolves its launch target at host-loop start, after the registry seeds).
 * The overlay calls this once, on its first open. */
void knob_registry_late_stamp(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_KNOBS_H */
