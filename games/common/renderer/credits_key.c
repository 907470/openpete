/* games/common/renderer/credits_key.c - the interp ring key of a moby in a
 * rebuilt-slot set: a credits letter or a loading-screen tally gem. See
 * renderer/credits_key.h. */

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

/** @brief Updates the credits letter generations and slot-to-record table from the records as the tick left them. */
static void credits_track(void) {
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

/** @brief The credits letter key of a moby in a letter slot.
 *
 * @param m The moby's PSX address.
 * @return The letter key, or m when the roll is inactive or m is not a letter slot. */
static uint32_t credits_key(uint32_t m) {
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

/* The loading screen's treasure tally (game_layout.tally). The slots the
 * builder rewrote this tick, each its key or 0 when the slot keeps its
 * address: a tally record's key or a HUD role's. */
#define TALLY_CAP 32u                    /* records and slots the tables hold */
#define TALLY_KEY_GEM 0x70000000u        /* | generation << 8 | record index */
#define TALLY_KEY_HUD 0x71000000u        /* | role */

static uint8_t  s_tally_act[TALLY_CAP];   /* each record's active byte at the last capture */
static uint8_t  s_tally_gen[TALLY_CAP];   /* each record's activation generation */
static uint32_t s_tally_key[TALLY_CAP];   /* slot j's key, 0 for an address-keyed slot */
static uint32_t s_tally_n;                /* slots s_tally_key describes */

/** @brief Reports whether the tally builder rewrites the moby array this tick.
 *
 * @return Nonzero while the builder state word holds the tally state. */
static int tally_active(void) {
    const uint32_t st = game_layout.tally.state;
    return st && rd32(st) == game_layout.tally.state_value;
}

/** @brief Reports whether the builder writes the tally records' gems this tick: its own timer gate.
 *
 * @return Nonzero when (unsigned)(timer - timer_lo) < timer_span. */
static int tally_gems_drawn(void) {
    return rd32(game_layout.tally.timer) - game_layout.tally.timer_lo < game_layout.tally.timer_span;
}

/** @brief The HUD role of a tally-class moby by its +0x0C and +0x14 words, or -1 when it is none.
 *
 * @param mh The moby's host bytes.
 * @return The role index into game_layout.tally.hud_x. */
static int tally_hud_role(const uint8_t* mh) {
    int32_t x, z;
    memcpy(&x, mh + 0x0C, 4);
    memcpy(&z, mh + 0x14, 4);
    if (z != game_layout.tally.hud_z) return -1;
    for (int r = 0; r < 2; r++)
        if (x == game_layout.tally.hud_x[r]) return r;
    return -1;
}

/** @brief Updates the record generations and lists the active records in record order.
 *
 * A record's generation is bumped when its active byte turns nonzero.
 * @param recs The record array's PSX address.
 * @param act_rec Receives the active record indices, TALLY_CAP entries.
 * @return The number of active records. */
static uint32_t tally_records(uint32_t recs, uint8_t* act_rec) {
    const uint32_t nrec = game_layout.tally.rec_count < TALLY_CAP ? game_layout.tally.rec_count : TALLY_CAP;
    const uint32_t stride = game_layout.tally.rec_stride;
    uint32_t n_act = 0;
    for (uint32_t i = 0; i < nrec; i++) {
        const uint8_t* g = (const uint8_t*)openpete_psx_to_host_span(recs + i * stride, stride);
        const uint8_t a = g ? g[game_layout.tally.active_off] : 0u;
        if (a && !s_tally_act[i]) s_tally_gen[i]++;
        s_tally_act[i] = a;
        if (a) act_rec[n_act++] = (uint8_t)i;
    }
    return n_act;
}

/** @brief The key of the ord-th tally-class slot the builder wrote: an active record's while ord < n_act, else a HUD role's.
 *
 * @param mh The slot's host bytes.
 * @param ord The slot's ordinal among the tally-class slots.
 * @param act_rec The active record indices.
 * @param n_act The number of record gems the builder wrote.
 * @return The key, or 0 when the slot keeps its address. */
static uint32_t tally_slot_key(const uint8_t* mh, uint32_t ord, const uint8_t* act_rec, uint32_t n_act) {
    if (ord < n_act) {
        const uint32_t i = act_rec[ord];
        return TALLY_KEY_GEM | ((uint32_t)s_tally_gen[i] << 8) | i;
    }
    const int r = tally_hud_role(mh);
    return r >= 0 ? TALLY_KEY_HUD | (uint32_t)r : 0u;
}

/** @brief Updates the record generations and the slot-to-key table from the tally records and the rebuilt array.
 *
 * Repeats the builder's emission order: up to prefix_max slots of another
 * class, then, when its timer gate is open, one gem_class slot per active
 * record in record order, then the HUD gems by role. The walk ends at the
 * array's sentinel or at a slot of another class after the prefix. */
static void tally_track(void) {
    s_tally_n = 0;
    if (!tally_active()) {
        memset(s_tally_act, 0, sizeof(s_tally_act));
        return;
    }
    const uint32_t recs = rd32(game_layout.tally.recs);
    const uint32_t base = rd32(game_layout.tally.slot_base);
    if (recs < 0x80000000u || base < 0x80000000u) return;
    uint8_t act_rec[TALLY_CAP];
    const uint32_t n_rec = tally_records(recs, act_rec);
    const uint32_t n_act = tally_gems_drawn() ? n_rec : 0u;   /* record gems the builder wrote */
    const uint32_t stride = game_layout.tally.slot_stride;
    uint32_t ord = 0;                     /* tally-class slots seen */
    for (uint32_t j = 0; j < TALLY_CAP; j++) {
        const uint8_t* mh = (const uint8_t*)openpete_psx_to_host_span(base + j * stride, stride);
        if (!mh || mh[game_layout.moby.state_off] == (uint8_t)game_layout.moby.state_term) break;
        uint16_t cls;
        memcpy(&cls, mh + game_layout.moby.class_off, 2);
        const int tally_cls = cls == game_layout.tally.gem_class;
        if (!tally_cls && (ord || j >= game_layout.tally.prefix_max)) break;  /* not the builder's array */
        s_tally_key[j] = tally_cls ? tally_slot_key(mh, ord++, act_rec, n_act) : 0u;
        s_tally_n = j + 1u;
    }
}

/** @brief The tally key of a moby in a rebuilt tally slot.
 *
 * @param m The moby's PSX address.
 * @return The record or HUD role key, or m when the tally is inactive or m is not a keyed tally slot. */
static uint32_t tally_key(uint32_t m) {
    if (!s_tally_n || !tally_active()) return m;
    const uint32_t base = rd32(game_layout.tally.slot_base);
    const uint32_t stride = game_layout.tally.slot_stride;
    if (m < base || (m - base) % stride != 0u) return m;
    const uint32_t j = (m - base) / stride;
    if (j >= s_tally_n || !s_tally_key[j]) return m;
    const uint8_t* mh = (const uint8_t*)openpete_psx_to_host_span(m, stride);
    uint16_t cls = 0;
    if (mh) memcpy(&cls, mh + game_layout.moby.class_off, 2);
    return cls == game_layout.tally.gem_class ? s_tally_key[j] : m;
}

/* The rebuilt-slot sets: each tracks its records once per capture and keys
 * the slots its draw rewrote. A moby takes the first set's key that differs
 * from its address. */
typedef struct {
    void     (*track)(void);
    uint32_t (*key)(uint32_t m);
} rebuilt_set_t;

static const rebuilt_set_t k_sets[] = {
    { credits_track, credits_key },
    { tally_track,   tally_key },
};

void game_credits_track(void) {
    for (size_t i = 0; i < sizeof k_sets / sizeof k_sets[0]; i++) k_sets[i].track();
}

uint32_t game_credits_letter_key(uint32_t m) {
    for (size_t i = 0; i < sizeof k_sets / sizeof k_sets[0]; i++) {
        const uint32_t k = k_sets[i].key(m);
        if (k != m) return k;
    }
    return m;
}
