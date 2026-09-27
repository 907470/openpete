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
    KNOB_SRC_ENV,       /* env var (incl. an openpete.toml [env] table) */
    KNOB_SRC_CLI        /* explicit flag (incl. openpete.toml / bk2 splices) */
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

    /* Numeric rows whose 0 is a distinct mode rather than a magnitude, so the
     * setter accepts 0 and [num_min, num_max] but refuses the span between:
     * the caption of the checkbox that selects 0, beside a slider over the
     * range. NULL = the range runs to num_min with nothing below it. */
    const char *zero_label;
} knob_desc_t;

/** @brief The knob table; mutable, since boot stamps and stored values live in the rows.
 *
 * The first call copies the built-in rows in; openpete_knob_add inserts
 * rows after the last row of their section.
 * @param count Receives the row count; may be NULL.
 * @return The first row. */
knob_desc_t *knob_registry(size_t *count);

/** @brief Stamp every row's boot source and boot value.
 *
 * The source comes from the config resolver (openpete_config_knob_layer).
 * The stored value is seeded from the row's openpete_options field; vsync,
 * devui_open and record_fps_overlay read their env var when the option is
 * unset, and freecam and camlock start at 0. Each non-action row's boot
 * value is then stamped from its getter, or from the stored value when it
 * has none. The dev UI calls it at init, after openpete_options_parse.
 * @param opts Parsed options; the pointer is kept and must outlive the registry. */
void knob_registry_init(const struct openpete_options *opts);
/** @brief Counter bumped on every env var write by a live registry row.
 *
 * A consumer that caches an env knob re-reads it when the epoch changes,
 * paying one getenv per edit instead of one per poll.
 * @return The current epoch; it starts at 1. */
unsigned knob_registry_env_epoch(void);

/** @brief Whether a cache keyed on the registry env epoch must re-read its knob.
 *
 * The pattern for a live knob read on a hot path: keep the parsed value and
 * the epoch it was read at in statics, and call getenv only when this
 * returns 1. A cache seeded with 0 reads on its first call (the epoch starts
 * at 1).
 * @param seen The epoch the cache last read at; updated when stale.
 * @return 1 when the knob must be re-read. */
static inline int knob_env_epoch_stale(unsigned* seen) {
    const unsigned ep = knob_registry_env_epoch();
    if (*seen == ep) return 0;
    *seen = ep;
    return 1;
}

/** @brief Write a row's current value as a display string.
 *
 * Action rows show their get_str output, else action_label ("Apply" when
 * NULL); string rows their get_str output, else "?". Numeric rows read
 * get_num, else the stored value: bool as "on"/"off", int as %d, float as
 * %.3g, enum as its value name, or the index when out of range.
 * @param k Row.
 * @param buf Output buffer.
 * @param cap Size of `buf`. */
void knob_value_str(const knob_desc_t *k, char *buf, size_t cap);

/** @brief Write a row's current value in the vocabulary openpete.toml stores, the CLI's.
 *
 * The string is knob_value_str's except for the SSAA row, which displays
 * "4x" where the flag takes "4". A value starting with '(' is a get_str
 * placeholder ("(default)", "(none)") and is refused, so it is never saved
 * as a literal.
 * @param k Row.
 * @param buf Output buffer.
 * @param cap Size of `buf`.
 * @return 0 when the value can be persisted; -1 for a NULL or action row,
 *         a row with no config schema entry, or a placeholder. */
int knob_config_value_str(const knob_desc_t *k, char *buf, size_t cap);

/** @brief Write every row whose live value differs from its boot value into openpete.toml's [saved] zone.
 *
 * Action rows, string rows without a getter, and rows knob_config_value_str
 * cannot persist are skipped. Each changed row goes through
 * openpete_config_save_knob, then openpete_config_save_flush writes the file
 * once. A row pinned by a launch tier (a typed flag or an exported variable)
 * is saved like any other; that tier keeps winning while it is present, and
 * the M overlay marks such rows "pinned by ...". Called by the O menu's save
 * row and the M overlay's Save settings button.
 * @return The number of rows saved, or -1 when the flush failed. */
int knob_registry_save_edited(void);

/** @brief Re-stamp the boot values of the rows that settle after knob_registry_init.
 *
 * The pacer resolves the render_fps target, tick_scale and the gated
 * runahead depth at host-loop start. Each of those rows adopts its getter's
 * value as both boot and stored value, so it does not read as edited. The
 * dev UI calls it once, on the overlay's first open. */
void knob_registry_late_stamp(void);

/* Late-bind a live string channel onto an existing row. Some knobs' setters
 * live in game feature modules the core library cannot link against; the
 * module attaches them here (apply-options time) and the row flips to
 * KNOB_LIVE. Either pointer may be NULL to keep the row's own. Returns 0 if
 * the id was found (it won't be when the row is compiled out, e.g. Debug
 * rows under OPENPETE_HIDE_DEBUG_FLAGS — callers just ignore that). */
/** @brief Append a row the core table cannot name, such as a game module's cheats or toggles.
 *
 * The row is copied, so the caller's knob_desc_t may be a stack temporary,
 * but every string it points at must outlive the process (literals or module
 * statics). The row lands after the last existing row of its section, so it
 * joins that collapsing header, or starts a new section at the end. Call at
 * feature apply-options time, before the overlay first draws and before
 * knob_registry_init stamps boot values.
 * @param row The row to copy in.
 * @return 0 on success, -1 when row is NULL or the extra-row capacity
 *         (KNOB_EXTRA_MAX rows) is exhausted. */
int openpete_knob_add(const knob_desc_t *row);

/** @brief Record the game's default window size for the --window row's "(default)" text.
 *
 * The host loop calls it once the game layout is known; the registry links
 * no game layout, so console tests that compile it need no game. Until it is
 * called, and while either size is 0, the row shows "(default)" with no size.
 * @param w Default window width in pixels.
 * @param h Default window height in pixels. */
void openpete_knob_default_window_set(int w, int h);

int openpete_knob_bind_str(const char *id,
                           void (*get_str)(const struct knob_desc *k,
                                           char *buf, size_t cap),
                           int (*set_str)(struct knob_desc *k, const char *v));

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_KNOBS_H */
