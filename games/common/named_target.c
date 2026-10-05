#include "named_target.h"
#include "openpete_surface.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Read the name field of a table entry.
 *
 * @param t The table.
 * @param i The entry index, below t->count.
 * @return The entry's name string. */
static const char* nt_name(const named_target_table_t* t, size_t i) {
    const char* e = (const char*)t->base + i * t->stride;
    return *(const char* const*)(const void*)(e + t->name_off);
}

/** @brief Read the id field of a table entry.
 *
 * @param t The table.
 * @param i The entry index, below t->count.
 * @return The entry's id. */
static int nt_id(const named_target_table_t* t, size_t i) {
    const char* e = (const char*)t->base + i * t->stride;
    return *(const int*)(const void*)(e + t->id_off);
}

void named_target_surface(struct openpete_surface_sink* sink, const char* table,
                          const char* flag, const named_target_table_t* tbl) {
    size_t i;
    for (i = 0; i < tbl->count; i++)
        openpete_surface_vocab(sink, table, flag, nt_name(tbl, i));
}

void named_target_print(const char* kind, const named_target_table_t* tbl) {
    size_t i;
    fprintf(stderr, "valid %s names:\n", kind);
    for (i = 0; i < tbl->count; i++)
        fprintf(stderr, "  %-18s (%d)\n", nt_name(tbl, i), nt_id(tbl, i));
}

/** @brief Find a name's id in a table by exact string match.
 *
 * @param t The table; NULL is an empty table.
 * @param norm The normalised name.
 * @return The id of the first entry with that name; -1 when none matches. */
static int nt_lookup(const named_target_table_t* t, const char* norm) {
    size_t i;
    if (!t) return -1;
    for (i = 0; i < t->count; i++)
        if (strcmp(norm, nt_name(t, i)) == 0) return nt_id(t, i);
    return -1;
}

/** @brief Report whether a table holds an id.
 *
 * @param t The table.
 * @param id The id.
 * @return 1 when an entry carries the id, else 0. */
static int nt_has_id(const named_target_table_t* t, int id) {
    size_t i;
    for (i = 0; i < t->count; i++)
        if (nt_id(t, i) == id) return 1;
    return 0;
}

int named_target_resolve(const char* arg, const char* kind,
                         const named_target_table_t* tbl,
                         const named_target_table_t* aliases,
                         unsigned flags) {
    size_t i, n;
    int    id;
    char   norm[32];

    /* No value: the list is the answer (`--skip-to-level` alone). */
    if (!arg || !arg[0]) { named_target_print(kind, tbl); return -1; }

    for (i = 0; arg[i] && isdigit((unsigned char)arg[i]); i++) {}
    if (!arg[i]) {
        id = atoi(arg);
        if ((flags & NAMED_TARGET_NUMERIC_ANY) || nt_has_id(tbl, id)) return id;
        named_target_print(kind, tbl);
        return -1;
    }

    for (i = 0, n = 0; arg[i] && n < sizeof(norm) - 1; i++) {
        char c = (char)tolower((unsigned char)arg[i]);
        if (c == '\'' && (flags & NAMED_TARGET_DROP_APOSTROPHE)) continue;
        norm[n++] = (c == '-') ? '_' : c;
    }
    norm[n] = '\0';

    id = nt_lookup(tbl, norm);
    if (id < 0) id = nt_lookup(aliases, norm);
    if (id >= 0) return id;

    named_target_print(kind, tbl);
    return -1;
}
