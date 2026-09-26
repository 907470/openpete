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

/** @brief Parse an anchor spelling to its openpete_anchor_t value.
 *
 * Matches openpete_anchor_names case-insensitively, with '_' read as '-', so
 * `top_right` and `TOP-RIGHT` are accepted alongside `top-right`.
 * @param s The spelling; NULL or empty is unknown.
 * @return The anchor index, or -1 when unknown. */
int openpete_anchor_parse(const char *s);

/* Edge predicates of the placement rules, exposed for callers that mirror
 * them (per-line text alignment inside a right-anchored block, a slide-in
 * direction). */
/** @brief Return nonzero when anchor a sits on the bottom edge.
 *
 * True for the three BOTTOM_* anchors, which follow the TOP_* ones in the enum.
 * @param a an openpete_anchor_t value.
 * @return nonzero for a bottom anchor. */
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

/** @brief Compute the usable x range [x_min, x_max] for this present.
 *
 * x_max is the overlay aspect; x_min is the settings menu's panel right edge
 * while the menu is open, else 0. The menu sidebar is full height, so every
 * anchor, CENTER included, resolves inside this range. An aspect below x_min
 * is raised to it, giving an empty range rather than an inverted one.
 * @param aspect The overlay width in window-height units.
 * @param x_min Receives the left bound; may be NULL.
 * @param x_max Receives the right bound; may be NULL. */
void openpete_anchor_usable_x(float aspect, float *x_min, float *x_max);

/** @brief Begin a present's placement pass: clear the six cursors and latch the usable x range.
 *
 * @param L The ledger; NULL is ignored.
 * @param x_min Left bound of the usable range.
 * @param x_max Right bound of the usable range. */
void openpete_anchor_reset(openpete_anchor_ledger_t *L,
                           float x_min, float x_max);

/** @brief Claim a w×h block at anchor @p a and write its top-left corner in overlay space.
 *
 * TOP_* anchors stack downward from the top edge, BOTTOM_* upward from the
 * bottom edge; LEFT starts at x_min + margin, RIGHT ends at x_max - margin,
 * CENTER centres between the two. A block wider than that range is placed
 * at its left end rather than clipped. The anchor's cursor then advances by
 * h plus OPENPETE_ANCHOR_GAP, so the next claimant at the same anchor lands
 * clear of this one. The cursor tracks content extent only, so each
 * claimant applies its own margin and none inherits its predecessor's.
 *
 * The margin is per claim rather than a property of the ledger because the
 * two kinds of block differ: a toast is a bordered panel that should not
 * touch the edge, while the stats block is bare text whose region already
 * carries its own inset, and a shared margin would pad it twice. Pass
 * OPENPETE_ANCHOR_MARGIN for a panel, 0 for a block that pads itself.
 * @param L Ledger reset for this present.
 * @param a Anchor index (OPENPETE_ANCHOR_*); an invalid index or a NULL
 *          ledger writes (0, 0) and claims nothing.
 * @param w Block width in overlay units.
 * @param h Block height in overlay units.
 * @param margin Gap from the screen edge.
 * @param out_x Receives the left edge; may be NULL.
 * @param out_y Receives the top edge; may be NULL. */
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
