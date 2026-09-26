#ifndef OPENPETE_CONFIG_SCHEMA_H
#define OPENPETE_CONFIG_SCHEMA_H

/*
 * The config schema — every knob that openpete.toml can name with a
 * friendly bare key, and the CLI / env spellings that mean the same knob.
 *
 * This table exists for ONE job: telling the resolver that two spellings
 * are the same setting, so exactly one of them survives flattening. A
 * knob only needs a row when it could be spelled two ways (a flag and an
 * env var) or when we want a bare key for it. The ~150 debug-only
 * OPENPETE_* vars have neither a flag twin nor a bare key, so they need
 * no rows and flow through [env] untouched — matching the standing rule
 * that temporary debug vars stay out of the knob registry.
 *
 * Kept a separate translation unit from knob_registry.c deliberately:
 * the registry's rows carry function pointers into the renderer and the
 * game modules, so it cannot be linked into a standalone resolver test.
 * knob_registry_init cross-checks the two tables at boot, so a knob that
 * gains a spelling in one and not the other is reported rather than
 * silently mis-resolved.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CFG_BOOL,
    CFG_INT,
    CFG_FLOAT,
    CFG_ENUM,
    CFG_STRING
} cfg_type_t;

typedef struct {
    const char *id;      /* bare key in openpete.toml == knob registry id */
    const char *cli;     /* "--lod", or NULL for an env-only knob */
    const char *env;     /* "OPENPETE_...", or NULL for a CLI-only knob */
    cfg_type_t  type;

    /* CFG_ENUM: NULL-terminated list of accepted spellings, which are the
     * CLI's own vocabulary — `lod = "extended"`, never `lod = 1`. */
    const char *const *vocab;

    /* A flag that means this knob at a fixed value ("--no-lod" ==
     * "--lod extended"). Without this the resolver would treat the alias
     * as an unrelated flag and both spellings could be emitted. */
    const char *alias;
    const char *alias_value;

    /* 1 when the flag takes no value ("--render-stats"): a true bare key
     * emits the flag alone, a false one emits nothing. */
    int flag_valueless;
} cfg_knob_t;

const cfg_knob_t *cfg_schema(size_t *count);

/** @brief Index of the schema row whose id is `id`.
 *
 * @param id Knob id; NULL returns -1.
 * @return The row index, or -1 when no row has that id. */
int cfg_schema_find_id(const char *id);
/** @brief Index of the schema row whose flag or alias flag is `flag`.
 *
 * @param flag Flag spelling ("--lod"); NULL returns -1.
 * @return The row index, or -1 when no row has that flag. */
int cfg_schema_find_cli(const char *flag);      /* matches alias too */
/** @brief Index of the schema row whose env var is `var`.
 *
 * @param var Env var name; NULL returns -1.
 * @return The row index, or -1 when no row has that variable. */
int cfg_schema_find_env(const char *var);

/** @brief The group of mutually exclusive flags that contains `flag`.
 *
 * Flags in one group cannot coexist: when a higher tier supplies one, the
 * config drops its own copies of the others rather than leave them to
 * collide in the parser.
 * @param flag Flag spelling; NULL returns NULL.
 * @return The NULL-terminated group, or NULL when `flag` is in none. */
const char *const *cfg_schema_mutex_group(const char *flag);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_CONFIG_SCHEMA_H */
