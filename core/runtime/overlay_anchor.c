/* core/runtime/overlay_anchor.c — see openpete_overlay_anchor.h.
 *
 * Placement arithmetic only. The one piece of state it consults is whether the
 * settings menu is open, which decides where the usable x range starts. */

#include "openpete_overlay_anchor.h"
#include "openpete_menu.h"

#include <stddef.h>
#include <string.h>

const char *const openpete_anchor_names[OPENPETE_ANCHOR_COUNT] = {
    "top-left", "top-center", "top-right",
    "bottom-left", "bottom-center", "bottom-right",
};

static char lower_dash(char c) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return (c == '_') ? '-' : c;
}

int openpete_anchor_parse(const char *s) {
    if (!s || !*s) return -1;
    for (int i = 0; i < OPENPETE_ANCHOR_COUNT; i++) {
        const char *n = openpete_anchor_names[i];
        const char *p = s;
        while (*n && *p && *n == lower_dash(*p)) { n++; p++; }
        if (!*n && !*p) return i;
    }
    return -1;
}

void openpete_anchor_usable_x(float aspect, float *x_min, float *x_max) {
    float lo = 0.0f;
    if (openpete_menu_is_open()) {
        openpete_menu_layout_t L;
        openpete_menu_get_layout(&L);
        /* Exactly the panel's right edge, no extra gap: a block's own left
         * margin then reproduces the same visual inset against the panel that
         * it has against the window edge when the menu is closed. */
        lo = L.panel_x1;
    }
    if (aspect < lo) aspect = lo;   /* degenerate window: empty range, not inverted */
    if (x_min) *x_min = lo;
    if (x_max) *x_max = aspect;
}

void openpete_anchor_reset(openpete_anchor_ledger_t *L,
                           float x_min, float x_max) {
    if (!L) return;
    L->x_min = x_min;
    L->x_max = x_max;
    memset(L->used, 0, sizeof L->used);
}

void openpete_anchor_place(openpete_anchor_ledger_t *L, int a,
                           float w, float h, float margin,
                           float *out_x, float *out_y) {
    float x = 0.0f, y = 0.0f;
    if (!L || a < 0 || a >= OPENPETE_ANCHOR_COUNT) {
        if (out_x) *out_x = 0.0f;
        if (out_y) *out_y = 0.0f;
        return;
    }
    const float M = margin;
    const float lo = L->x_min + M, hi = L->x_max - M;

    if (openpete_anchor_is_right(a))        x = hi - w;
    else if (openpete_anchor_is_center(a))  x = 0.5f * (lo + hi - w);
    else                                    x = lo;
    /* Wider than the range it was asked to sit in: keep the near edge visible
     * rather than centring the overflow across both edges. */
    if (x < lo) x = lo;

    if (openpete_anchor_is_bottom(a)) y = 1.0f - M - L->used[a] - h;
    else                              y =        M + L->used[a];

    L->used[a] += h + OPENPETE_ANCHOR_GAP;

    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
}
