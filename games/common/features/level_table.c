// games/common/features/level_table.c - lookups over a game's level table;
// see level_table.h.

#include "features/level_table.h"

#include "named_target.h"

#include <stdio.h>

/** @brief Describe a span of level rows for the named-target lookup.
 *
 * @param rows The first row.
 * @param n The row count.
 * @return The descriptor over `name` and `number`. */
static named_target_table_t rows_of(const game_level_t* rows, size_t n) {
    named_target_table_t r = { rows, sizeof(game_level_t), n,
                               offsetof(game_level_t, name),
                               offsetof(game_level_t, number) };
    return r;
}

int game_level_resolve(const game_level_table_t* t, const char* arg) {
    const named_target_table_t rows = rows_of(t->levels, t->count);
    const named_target_table_t aliases = rows_of(t->aliases, t->n_aliases);
    return named_target_resolve(arg, t->kind, &rows,
                                t->aliases ? &aliases : NULL, t->resolve_flags);
}

void game_level_surface(const game_level_table_t* t,
                        struct openpete_surface_sink* sink) {
    const named_target_table_t rows = rows_of(t->levels, t->count);
    named_target_surface(sink, t->kind, t->flag, &rows);
    if (t->aliases) {
        const named_target_table_t aliases = rows_of(t->aliases, t->n_aliases);
        named_target_surface(sink, t->kind, t->flag, &aliases);
    }
}

int game_level_index(const game_level_table_t* t, int number) {
    for (size_t i = 0; i < t->count; i++)
        if (t->levels[i].number == number) return (int)i;
    return -1;
}

void game_level_name(const game_level_table_t* t, int number, char* out,
                     size_t cap) {
    const int i = game_level_index(t, number);
    if (i >= 0) snprintf(out, cap, "%s", t->levels[i].name);
    else        snprintf(out, cap, "level%d", number);
}
