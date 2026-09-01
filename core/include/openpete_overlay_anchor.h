// core/include/openpete_overlay_anchor.h — where chrome goes, and who got
// there first.
//
// openpete draws two kinds of persistent chrome over the game: the
// performance-stats readout and the OSD's toast notifications. Both are
// user-positionable to any of six corners/edges, so both can be pointed at the
// same one. This is the ledger that resolves that: once per present the six
// cursors are reset, the persistent block claims its space first, and the
// transient toasts stack away from it. Nothing overprints anything else.
//
// OVERLAY SPACE (shared with the FPS overlay, the settings menu and the mod
// HUD): x is in units of window HEIGHT and spans [0, aspect]; y is [0,1] over
// the FULL window height; the origin is top-left and +y runs down. Keeping x
// in height units is what makes a glyph square at any aspect.
//
// Pure placement arithmetic — no renderer or game state beyond the settings
// menu's panel rect, which is what the usable-x range exists to dodge.

#ifndef OPENPETE_OVERLAY_ANCHOR_H
#define OPENPETE_OVERLAY_ANCHOR_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OPENPETE_ANCHOR_TOP_LEFT = 0,
    OPENPETE_ANCHOR_TOP_CENTER,
    OPENPETE_ANCHOR_TOP_RIGHT,
    OPENPETE_ANCHOR_BOTTOM_LEFT,
    OPENPETE_ANCHOR_BOTTOM_CENTER,
    OPENPETE_ANCHOR_BOTTOM_RIGHT,
    OPENPETE_ANCHOR_COUNT
} openpete_anchor_t;

/* Canonical spellings, indexed by the enum: "top-left" … "bottom-right".
 * These are the CLI / openpete.toml / knob-registry vocabulary. */
extern const char *const openpete_anchor_names[OPENPETE_ANCHOR_COUNT];

/* Parse a spelling; -1 if unknown. Case-insensitive, and '_' reads as '-' so
 * a toml-flavoured `top_right` is accepted alongside `top-right`. */
int openpete_anchor_parse(const char *s);

/* Edge predicates — the placement rules below, exposed for callers that need
 * to mirror them (per-line text alignment inside a right-anchored block, a
 * slide-in direction). */
static inline int openpete_anchor_is_bottom(int a) {
    return a >= OPENPETE_ANCHOR_BOTTOM_LEFT;
}
static inline int openpete_anchor_is_right(int a) {
    return a == OPENPETE_ANCHOR_TOP_RIGHT || a == OPENPETE_ANCHOR_BOTTOM_RIGHT;
}
static inline int openpete_anchor_is_center(int a) {
    return a == OPENPETE_ANCHOR_TOP_CENTER || a == OPENPETE_ANCHOR_BOTTOM_CENTER;
}

/* The per-present occupancy ledger. Six cursors, one per anchor, each holding
 * how far the blocks placed so far reach IN from that anchor's edge. */
typedef struct {
    float x_min, x_max;                    /* usable x range this present  */
    float used[OPENPETE_ANCHOR_COUNT];     /* consumed depth from the edge */
} openpete_anchor_ledger_t;

/* The usable x range for this present: [x_min, x_max]. x_max is the overlay
 * aspect; x_min is the settings menu's right panel edge while it is open, else
 * 0. Every anchor resolves inside that range — the menu sidebar is FULL height,
 * so CENTER anchors need this as much as LEFT ones do. */
void openpete_anchor_usable_x(float aspect, float *x_min, float *x_max);

/* Begin a present's placement pass: clear the cursors and latch the range. */
void openpete_anchor_reset(openpete_anchor_ledger_t *L,
                           float x_min, float x_max);

/* Claim a w×h block at `a` and report its TOP-LEFT corner in overlay space.
 * TOP_* anchors stack downward from the edge, BOTTOM_* upward; LEFT hugs
 * x_min, RIGHT hugs x_max, CENTER centres in the range. The anchor's cursor
 * then advances by h plus the inter-block gap, so the next claimant at the
 * same anchor lands clear of this one. A block wider than the usable range is
 * placed flush at the range's near edge rather than clipped.
 *
 * `margin` is the gap from the screen edge, and it is PER CLAIM rather than a
 * property of the ledger, because the two kinds of block differ: a toast is a
 * bordered panel that should not touch the edge, while the stats block is bare
 * text whose region ALREADY carries its own inset. Charging a shared margin to
 * both double-pads the second one — which is what pushed the stats readout off
 * the window edge it used to sit flush against. Pass OPENPETE_ANCHOR_MARGIN
 * for a panel, 0 for a block that pads itself.
 *
 * The cursor tracks CONTENT extent only, so each claimant applies its own
 * margin and none inherits its predecessor's. */
void openpete_anchor_place(openpete_anchor_ledger_t *L, int a,
                           float w, float h, float margin,
                           float *out_x, float *out_y);

/* The standard margin for a block that does NOT pad itself, and the gap
 * between stacked blocks (overlay units). Exposed so a caller can size a
 * region against the space that is actually left. */
#define OPENPETE_ANCHOR_MARGIN 0.012f
#define OPENPETE_ANCHOR_GAP    0.010f

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_OVERLAY_ANCHOR_H */
