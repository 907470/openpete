/* named_target.h - resolve a command-line name or number against a table of
 * named targets, and print the valid names when it does not resolve.
 *
 * `--skip-to-level` and `--skip-to-cutscene` take either an all-digit id or a
 * name, normalise the name, look it up, and on failure list every name the
 * table holds. The tables themselves are per game, and their element types
 * carry different extra fields (a display string on one), so the table is
 * described here by base, stride and the byte offsets of the two fields the
 * lookup reads.
 *
 * Parse-only: nothing here reads or writes PSX RAM.
 */
#ifndef OPENPETE_NAMED_TARGET_H
#define OPENPETE_NAMED_TARGET_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct openpete_surface_sink;

typedef struct {
    const void* base;      /* first element of the table */
    size_t      stride;    /* bytes between elements */
    size_t      count;     /* number of elements */
    size_t      name_off;  /* byte offset of the `const char*` name field */
    size_t      id_off;    /* byte offset of the `int` id field */
} named_target_table_t;

/* Build a table descriptor for an array whose element type has a `const char*`
 * name field and an `int` id field, named by the caller. */
#define NAMED_TARGET_TABLE(arr, type, name_field, id_field)                  \
    ((named_target_table_t){ (arr), sizeof(type),                            \
                             sizeof(arr) / sizeof((arr)[0]),                 \
                             offsetof(type, name_field),                     \
                             offsetof(type, id_field) })

/* An all-digit argument is returned verbatim instead of being looked up.
 * Without it an id the table does not hold is rejected, which is what keeps a
 * number from indexing a game's level table past its end. */
#define NAMED_TARGET_NUMERIC_ANY 0x1u

/** @brief Print the heading "valid KIND names:" and each entry's name and id, one line each, to stderr.
 *
 * @param kind Noun printed in the heading ("level", "cutscene").
 * @param tbl Table whose entries are listed. */
void named_target_print(const char* kind, const named_target_table_t* tbl);

/** @brief Resolve a command-line name or all-digit id against a named-target table.
 *
 * An all-digit `arg` is parsed as an id and returned when `tbl` holds it or
 * NAMED_TARGET_NUMERIC_ANY is set. Any other `arg` is normalised
 * (lowercased, '-' mapped to '_', apostrophes dropped so a possessive
 * matches a key spelled without one, cut at 31 bytes) and looked up in `tbl`,
 * then in `aliases`. An empty or NULL `arg` prints the names and returns
 * -1, which is what `--skip-to-level` with no value does; every other
 * failure also prints the valid names of `tbl`.
 * @param arg The command-line value, or NULL.
 * @param kind Noun used in the printed list.
 * @param tbl Table searched first.
 * @param aliases Table searched after `tbl`, or NULL.
 * @param flags NAMED_TARGET_* bits.
 * @return The matched id, or -1. */
int named_target_resolve(const char* arg, const char* kind,
                         const named_target_table_t* tbl,
                         const named_target_table_t* aliases,
                         unsigned flags);

/** @brief Report every name of a table to the surface dump as a vocabulary entry of `flag`.
 *
 * @param sink The dump's collector.
 * @param table Vocabulary table name ("level", "cutscene").
 * @param flag The flag that takes the names.
 * @param tbl The table. */
void named_target_surface(struct openpete_surface_sink* sink, const char* table,
                          const char* flag, const named_target_table_t* tbl);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_NAMED_TARGET_H */
