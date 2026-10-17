/* games/common/renderer/credits_key.c - the interp ring key of a credits
 * letter moby. See renderer/credits_key.h. */

#include "renderer/credits_key.h"

#include "game_layout.h"
#include "openpete_psx.h"          /* openpete_psx_to_host_span */

#include <stdint.h>
#include <string.h>

/* Records the tables hold; a layout's rec_max above it is clamped. */
#define CREDITS_CAP 128u

static uint8_t  s_state[CREDITS_CAP];    /* each record's state byte at the last capture */
static uint8_t  s_gen[CREDITS_CAP];      /* each record index's reuse generation */
static uint8_t  s_slot_rec[CREDITS_CAP]; /* drawn-set games: slot j draws record s_slot_rec[j] */
static uint32_t s_slot_n;                /* slots s_slot_rec describes */

/** @brief Read a guest word through a bounds-checked span; 0 when it does not map.
 *
 * @param addr The PSX address.
 * @return The word. */
static uint32_t rd32(uint32_t addr) {
    const void* p = openpete_psx_to_host_span(addr, 4u);
    uint32_t v = 0;
    if (p) memcpy(&v, p, 4);
    return v;
}

/** @brief Read a guest signed halfword through a bounds-checked span; 0 when it does not map.
 *
 * @param addr The PSX address.
 * @return The halfword, sign-extended. */
static int32_t rd16s(uint32_t addr) {
    const void* p = openpete_psx_to_host_span(addr, 2u);
    int16_t v = 0;
    if (p) memcpy(&v, p, 2);
    return v;
}

/** @brief The record bound the tables cover: the layout's rec_max, at most CREDITS_CAP.
 *
 * @return The bound. */
static uint32_t credits_rec_max(void) {
    const uint32_t n = game_layout.credits.rec_max;
    return n < CREDITS_CAP ? n : CREDITS_CAP;
}

/** @brief Reports whether the credits letter roll is drawing letters.
 *
 * The game-state word must hold the credits state; where the layout names
 * them, the stage word must be below its bound (Spyro 1's g_CreditsStage <
 * 99) and the draw gate halfword must not be -1 (Spyro 2 and 3 draw the
 * level instead of the letters while it is).
 * @return Nonzero while the roll runs. */
static int credits_active(void) {
    const uint32_t gs = game_layout.credits.gamestate;
    if (!gs || !game_layout.credits.rec_max || rd32(gs) != game_layout.credits.gamestate_value) return 0;
    if (game_layout.credits.stage &&
        (int32_t)rd32(game_layout.credits.stage) >= game_layout.credits.stage_below) return 0;
    if (game_layout.credits.draw_gate && rd16s(game_layout.credits.draw_gate) == -1) return 0;
    return 1;
}

/** @brief The number of records in use, or -1 when the count is negative or above the record bound.
 *
 * @return The count. */
static int32_t credits_count(void) {
    const uint32_t a = game_layout.credits.count;
    const uint32_t c = game_layout.credits.count_width == 2u ? (uint32_t)rd16s(a) : rd32(a);
    return c > credits_rec_max() ? -1 : (int32_t)c;
}

void game_credits_track(void) {
    if (!credits_active()) {
        memset(s_state, 0, sizeof(s_state));
        s_slot_n = 0;
        return;
    }
    const uint32_t recs = rd32(game_layout.credits.recs);
    const int32_t cnt = credits_count();
    if (recs < 0x80000000u || cnt < 0) {
        s_slot_n = 0;
        return;
    }
    const uint32_t stride = game_layout.credits.rec_stride;
    const uint32_t mask = game_layout.credits.drawn_states;
    const uint8_t flyin = (uint8_t)game_layout.credits.flyin_state;
    uint32_t n = 0;
    for (uint32_t k = 0; k < credits_rec_max(); k++) {
        const uint8_t* r = (k < (uint32_t)cnt)
            ? (const uint8_t*)openpete_psx_to_host_span(recs + k * stride, stride) : NULL;
        const uint8_t st = r ? r[game_layout.credits.state_off] : 0u;
        if (st == flyin && s_state[k] != flyin) s_gen[k]++;
        s_state[k] = st;
        if (r && st < 32u && ((mask >> st) & 1u)) s_slot_rec[n++] = (uint8_t)k;
    }
    s_slot_n = n;
}

/** @brief The slot index of a moby address in the letter slots, or -1 when it is not one.
 *
 * Slot j sits at base + j * stride when slot_dir is +1 and at base - (j + 1)
 * * stride when it is -1, base the word at slot_base.
 * @param m The moby's PSX address.
 * @return The slot index. */
static int32_t credits_slot(uint32_t m) {
    const uint32_t base = rd32(game_layout.credits.slot_base);
    const uint32_t stride = game_layout.credits.slot_stride;
    if (base < 0x80000000u || !stride) return -1;
    uint32_t delta;
    if (game_layout.credits.slot_dir < 0) {
        if (m >= base || base - m < stride) return -1;
        delta = base - m - stride;              /* slot 0 ends at the base */
    } else {
        if (m < base) return -1;
        delta = m - base;
    }
    if (delta % stride != 0u || delta / stride >= credits_rec_max()) return -1;
    return (int32_t)(delta / stride);
}

uint32_t game_credits_letter_key(uint32_t m) {
    if (!game_layout.credits.slot_base || !credits_active()) return m;
    const int32_t j = credits_slot(m);
    if (j < 0) return m;
    uint32_t k = (uint32_t)j;
    if (game_layout.credits.drawn_states) {
        if (k >= s_slot_n) return m;
        k = s_slot_rec[k];
    }
    const uint8_t* mh = (const uint8_t*)openpete_psx_to_host_span(m, game_layout.credits.slot_stride);
    uint16_t cls = 0;
    if (mh) memcpy(&cls, mh + game_layout.moby.class_off, 2);
    if (game_layout.credits.letter_class && cls != game_layout.credits.letter_class) return m;
    return 0x60000000u | ((uint32_t)(cls & 0x3FFu) << 16) |
           ((uint32_t)(s_gen[k] & 0x3Fu) << 10) | (k & 0x3FFu);
}
