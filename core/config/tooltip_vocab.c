/* core/config/tooltip_vocab.c - OPENPETE_TOOLTIP_VOCAB: the vocabulary
 * check over every string the dev overlay's tooltips, --help, the settings
 * menu's hints and the mod rows' notes show. See openpete_surface.h. */

#include "openpete_surface.h"
#include "openpete.h"
#include "openpete_devui.h"
#include "openpete_knobs.h"
#include "openpete_menu.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* Barred from every string: a mutability class (how a row applies is shown
 * by the overlay's generated "restart to apply" line) or a claim about
 * whether a setting reaches gameplay state. */
static const char *const k_barred_all[] = {
    "display-only", "latched", "read once", "parsed once", "opened once",
    "scanned once", "once at startup", "one-shot at boot", "read-only here",
    "live once", "boot value", "presentation-only", "render-only",
    "intentionally divergent", "non-presentation", "gameplay state",
    "changes gameplay", "guest ram", "mutates",
};

/* Barred from strings a player reads (every row outside the debug block,
 * the game's rows, the menu hints and the mod notes): instrument terms.
 * Debug rows keep them, since their reader is a developer. */
static const char *const k_barred_player[] = {
    "oracle", "golden", "harness", "census", "a/b", "deterministic",
};

typedef struct {
    FILE *out;
    int   rows;      /* registry rows, game rows, help-only entries, hints, notes */
    int   strings;   /* nonempty strings scanned */
    int   hits;
} vocab_scan_t;

/** @brief Return whether `term` occurs in `s`, ignoring ASCII case. */
static int contains_ci(const char *s, const char *term)
{
    size_t n = strlen(term);
    for (; *s; s++) {
        size_t i = 0;
        while (i < n && s[i] &&
               tolower((unsigned char)s[i]) == (unsigned char)term[i])
            i++;
        if (i == n) return 1;
    }
    return 0;
}

/** @brief Scan one string against one barred list, printing a HIT line per term found. */
static void scan_list(vocab_scan_t *sc, const char *id, const char *field,
                      const char *s, const char *const *terms, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (!contains_ci(s, terms[i])) continue;
        fprintf(sc->out, "tooltip-vocab: HIT %s %s: \"%s\"\n", id, field, terms[i]);
        sc->hits++;
    }
}

/** @brief Scan one string of a row; `player` adds the instrument terms. */
static void scan_str(vocab_scan_t *sc, const char *id, const char *field,
                     const char *s, int player)
{
    if (!s || !*s) return;
    sc->strings++;
    scan_list(sc, id, field, s, k_barred_all,
              sizeof k_barred_all / sizeof k_barred_all[0]);
    if (player)
        scan_list(sc, id, field, s, k_barred_player,
                  sizeof k_barred_player / sizeof k_barred_player[0]);
}

/** @brief Scan a registry or game row's tooltip and --help description. */
static void scan_row(vocab_scan_t *sc, const knob_desc_t *k)
{
    const int player = !k->debug_block;
    sc->rows++;
    scan_str(sc, k->id, "help", k->help, player);
    scan_str(sc, k->id, "help_text", k->help_text, player);
}

/** @brief Scan the --help descriptions of the parser flags that have no registry row. */
static void scan_help_only(vocab_scan_t *sc)
{
    size_t n = 0;
    const openpete_help_only_t *h = openpete_options_help_only(&n);
    for (size_t i = 0; i < n; i++) {
        sc->rows++;
        scan_str(sc, h[i].flag, "help_text", h[i].help_text,
                 strcmp(h[i].block, "debug") != 0);
    }
}

/** @brief Scan the settings menu's row hints and the mod rows' tooltip notes. */
static void scan_menu_and_mods(vocab_scan_t *sc)
{
    openpete_menu_surface_t m;
    for (int i = 0; openpete_menu_surface_row(i, &m) == 0; i++) {
        sc->rows++;
        scan_str(sc, m.id, "hint", m.hint, 1);
    }
    size_t n = 0;
    const char *const *notes = openpete_devui_mod_notes(&n);
    for (size_t i = 0; i < n; i++) {
        sc->rows++;
        scan_str(sc, "mod_row", "note", notes[i], 1);
    }
}

/** @brief Append a barred term to the named registry row's tooltip, the negative control. */
static void break_row(const char *id)
{
    static char buf[1024];
    size_t n = 0;
    knob_desc_t *ks = knob_registry(&n);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(ks[i].id, id) != 0) continue;
        snprintf(buf, sizeof buf, "%s Display-only.", ks[i].help ? ks[i].help : "");
        ks[i].help = buf;
        return;
    }
}

int openpete_tooltip_vocab_check(const knob_desc_t *game_rows, int game_row_n,
                                 const char *broken, FILE *out)
{
    vocab_scan_t sc = { out, 0, 0, 0 };
    if (broken && *broken) break_row(broken);
    size_t n = 0;
    const knob_desc_t *ks = knob_registry(&n);
    for (size_t i = 0; i < n; i++) scan_row(&sc, &ks[i]);
    for (int i = 0; i < game_row_n; i++) scan_row(&sc, &game_rows[i]);
    scan_help_only(&sc);
    scan_menu_and_mods(&sc);
    fprintf(out, "tooltip-vocab: rows %d strings %d hits %d\n",
            sc.rows, sc.strings, sc.hits);
    return (sc.hits || sc.rows == 0 || sc.strings == 0) ? 1 : 0;
}
