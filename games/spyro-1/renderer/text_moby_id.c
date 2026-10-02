// text_moby_id.c - provenance identity for HUD "text moby" glyphs. See
// text_moby_id.h for what problem this solves and why pool position can't.
//
// Both emitters walk their string and, for every character that is not a
// space, bump-allocate one Moby:
//
//     g_HudMobys -= 1;  Memset(g_HudMobys, 0, sizeof(Moby));  ...
//
// so the addresses a call is about to consume are fully determined at its
// ENTRY by (g_HudMobys, the string): the j-th emitted glyph lands at
// g_HudMobys - (j+1)*sizeof(Moby). That lets one read-only entry hook record
// the whole call - no exit hook, no writes, nothing the emitter can see.
//
// Identity per glyph = (callsite, call ordinal at that callsite this tick,
// glyph index in the string, character). The callsite is the caller's return
// address, which the recompiler materialises as `cpu->ra = <ret>` before every
// jal, so it is exact rather than inferred. A layout that runs the same code
// path produces the same tuple every tick regardless of what else the HUD pool
// allocated, which is the whole point.
//
// Tick boundary: the pool drains at the end of each draw (func_80018880 walks
// it back to D_800756FC), so the first emit of the next tick sees the pool
// pointer jump back UP. That reset - or a change of pool base, which happens
// when the polybuf double-buffer flips - starts a new generation and clears
// the table. A draw that emits no text at all (the credits roll allocates its
// letters directly) never reaches that check, so the drain is hooked too: the
// first drain after an emit closes the generation, and a second drain with no
// emit in between clears it, since the records then describe mobys that
// another allocator has overwritten with matching classes.

#include <stdint.h>
#include <string.h>

#include "feature_registry.h"
#include "openpete_psx.h"
#include "openpete_subtick_view.h"      /* openpete_subtick_host_span */
#include "psx_runtime_internal.h"

#include "renderer/text_moby_id.h"

/* g_HudMobys (bump-down pool cursor) and D_800756FC (the pool's base for this
 * tick - the sentinel func_80018880 drains back to). */
#define HUD_MOBYS_PTR   0x80075710u
#define HUD_POOL_BASE   0x800756FCu
#define MOBY_STRIDE     0x58u
#define MOBY_CLASS_OFF  0x36u

/* Sized for the worst observed layout (the inventory page: ~110 glyphs) with
 * room to spare; a full table drops the overflow to the pool-ordinal key
 * rather than mis-keying it. Power of two - the probe masks with SLOTS-1. */
#define TEXT_ID_SLOTS   512u

typedef struct {
    uint32_t moby;      /* PSX vaddr of the glyph, 0 = empty slot */
    uint32_t key;       /* provenance key handed to the interp store */
    uint16_t cls;       /* m_Class the emitter will write - staleness guard */
} text_id_ent_t;

static text_id_ent_t s_tbl[TEXT_ID_SLOTS];
static uint32_t      s_used;

/* Per-generation callsite ordinals: how many calls each return address has
 * made since the table was cleared. Linear scan - a layout uses a handful of
 * distinct callsites, and this runs once per emitter call, not per glyph. */
#define TEXT_ID_CALLSITES 64u
static struct { uint32_t ra; uint32_t n; } s_calls[TEXT_ID_CALLSITES];
static uint32_t s_ncalls;

static uint32_t s_last_ptr;    /* pool cursor after the previous call */
static uint32_t s_last_base;   /* pool base seen with it */
static int      s_closed;      /* a drain has run since the last emit */

/* MOBYCLASS_* values the emitters map characters to (moby.h). */
#define CLS_EXCLAMATION 75u
#define CLS_APOSTROPHE  76u
#define CLS_NUMBER_0    260u
#define CLS_PERCENT     272u
#define CLS_SLASH       277u
#define CLS_QUESTION    278u
#define CLS_PLUS        317u
#define CLS_CARET       321u
#define CLS_PERIOD      327u
#define CLS_LETTER_A    426u

/** @brief Maps a character to the m_Class func_80017FE4 gives its glyph moby.
 *
 * Digits and capitals map to their NUMBER and LETTER classes; '/', '?',
 * '%', '^' and '+' to their own classes; every other character to
 * MOBYCLASS_PERIOD.
 * @param c The character.
 * @return The glyph's m_Class. */
static uint16_t glyph_class_plain(uint8_t c) {
    if (c >= '0' && c <= '9') return (uint16_t)(CLS_NUMBER_0 + (c - '0'));
    if (c >= 'A' && c <= 'Z') return (uint16_t)(CLS_LETTER_A + (c - 'A'));
    switch (c) {
        case '/': return CLS_SLASH;
        case '?': return CLS_QUESTION;
        case '%': return CLS_PERCENT;
        case '^': return CLS_CARET;
        case '+': return CLS_PLUS;
        default:  return CLS_PERIOD;
    }
}

/** @brief Maps a character to the m_Class the spacing-aware emitter func_800181AC gives its glyph moby.
 *
 * Digits and capitals map to their NUMBER and LETTER classes; '!', '?' and
 * '.' to their own classes; every other character, ',' included, to
 * MOBYCLASS_LETTER_APOSTROPHE.
 * @param c The character.
 * @return The glyph's m_Class. */
static uint16_t glyph_class_spaced(uint8_t c) {
    if (c >= '0' && c <= '9') return (uint16_t)(CLS_NUMBER_0 + (c - '0'));
    if (c >= 'A' && c <= 'Z') return (uint16_t)(CLS_LETTER_A + (c - 'A'));
    switch (c) {
        case '!': return CLS_EXCLAMATION;
        case '?': return CLS_QUESTION;
        case '.': return CLS_PERIOD;
        default:  return CLS_APOSTROPHE;   /* incl. ',' */
    }
}

/** @brief Empties the glyph table and the per-callsite ordinals.
 *
 * The table is zeroed only when it holds an entry; the ordinal list is
 * always reset. */
static void text_id_clear(void) {
    if (s_used) { memset(s_tbl, 0, sizeof(s_tbl)); s_used = 0; }
    s_ncalls = 0;
}

/** @brief Hashes a glyph moby address to its first probe index in the table.
 *
 * Multiplicative hash (2654435761) of the address, taking bits 13 and up so
 * the 0x58 stride does not leave the index bits repeating.
 * @param moby PSX virtual address of the glyph moby.
 * @return Slot index in [0, TEXT_ID_SLOTS). */
static uint32_t text_id_slot(uint32_t moby) {
    /* Pool addresses are stride-0x58 apart; fold the whole word so the low
     * bits of the index aren't just the stride's low bits repeating. */
    uint32_t h = moby * 2654435761u;
    return (h >> 13) & (TEXT_ID_SLOTS - 1u);
}

/** @brief Stores a glyph's key and expected class in the open-addressed table.
 *
 * Linear probing from the hashed slot; an existing entry for the same moby is
 * overwritten. Once half the slots are used the call is a no-op, so an
 * overflowing layout leaves the extra glyphs without an entry.
 * @param moby PSX virtual address of the glyph moby, nonzero.
 * @param key Provenance key from text_id_key.
 * @param cls m_Class the emitter will write to the moby. */
static void text_id_insert(uint32_t moby, uint32_t key, uint16_t cls) {
    if (s_used >= TEXT_ID_SLOTS / 2u) return;     /* keep the probe short */
    uint32_t i = text_id_slot(moby);
    for (uint32_t probe = 0; probe < TEXT_ID_SLOTS; probe++) {
        text_id_ent_t* e = &s_tbl[i];
        if (e->moby == 0u || e->moby == moby) {
            if (e->moby == 0u) s_used++;
            e->moby = moby; e->key = key; e->cls = cls;
            return;
        }
        i = (i + 1u) & (TEXT_ID_SLOTS - 1u);
    }
}

int text_moby_id_lookup(uint32_t moby, uint32_t* out_key) {
    if (!s_used || moby < 0x80000000u) return 0;
    uint32_t i = text_id_slot(moby);
    for (uint32_t probe = 0; probe < TEXT_ID_SLOTS; probe++) {
        const text_id_ent_t* e = &s_tbl[i];
        if (e->moby == 0u) return 0;
        if (e->moby == moby) {
            /* Staleness guard: a generation with no text at all leaves the
             * previous one's records standing, and the pool recycles those
             * addresses for whatever the next layout allocates. Requiring the
             * live m_Class to be the one the emitter wrote rejects those. */
            const void* h = openpete_subtick_host_span(moby + MOBY_CLASS_OFF, 2u);
            uint16_t live = 0;
            if (!h) return 0;
            memcpy(&live, h, 2);
            if (live != e->cls) return 0;
            *out_key = e->key;
            return 1;
        }
        i = (i + 1u) & (TEXT_ID_SLOTS - 1u);
    }
    return 0;
}

/** @brief Hashes a glyph's provenance tuple into an interpolation key in the 0x50000000 namespace.
 *
 * FNV-1a over the four fields, each folded in twice (the word, then its high
 * half), masked to 28 bits and tagged 0x50000000, which keeps it distinct
 * from the raw-vaddr (0x8xxxxxxx), HUD-pool slot (0x40000000) and
 * gem-flyout (0xA0000000) key spaces.
 * @param ra The emitter call's return address.
 * @param call_ord The call's ordinal at that return address in the generation.
 * @param glyph The glyph's index among the string's non-space characters.
 * @param ch The character.
 * @return The key. */
static uint32_t text_id_key(uint32_t ra, uint32_t call_ord,
                            uint32_t glyph, uint8_t ch) {
    uint32_t h = 2166136261u;                     /* FNV-1a over the tuple */
    uint32_t f[4] = { ra, call_ord, glyph, (uint32_t)ch };
    for (int i = 0; i < 4; i++) {
        h ^= f[i]; h *= 16777619u;
        h ^= f[i] >> 16; h *= 16777619u;
    }
    return 0x50000000u | (h & 0x0FFFFFFFu);
}

/** @brief Records the provenance key and expected m_Class of every glyph moby a text emitter call is about to allocate.
 *
 * Shared entry hook for func_80017FE4 and func_800181AC. Clears the table
 * when the pool base (D_800756FC) changed or the cursor (g_HudMobys) moved
 * back up since the previous call, counts the call's ordinal at its return
 * address, then walks the string at a0 (at most 128 bytes) and records the
 * j-th non-space character at cursor - (j+1) * 0x58. Reads guest RAM only.
 * @param cpu The guest CPU state at the emitter's entry.
 * @param spaced 1 for func_800181AC's character mapping, 0 for func_80017FE4's.
 * @return 0, so the recompiled emitter runs and does the allocation. */
static int text_emit_entry(CPUState* cpu, int spaced) {
    uint32_t base = cpu->read_word(HUD_POOL_BASE);
    uint32_t cur  = cpu->read_word(HUD_MOBYS_PTR);
    if (cur < 0x80000000u) return 0;

    /* Generation boundary. Either the pool base moved (the polybuf double
     * buffer flipped, so this is a new tick's draw) or the cursor jumped back
     * up (func_80018880 drained the pool - the last thing a draw does, and
     * the point past which the previous generation's records describe mobys
     * that are about to be overwritten). */
    if (base != s_last_base || cur > s_last_ptr) text_id_clear();
    s_closed = 0;

    /* Call ordinal for this callsite within the generation. */
    uint32_t ra = cpu->ra;
    uint32_t call_ord = 0;
    uint32_t ci = 0;
    for (; ci < s_ncalls; ci++) if (s_calls[ci].ra == ra) break;
    if (ci < s_ncalls) {
        call_ord = ++s_calls[ci].n;
    } else if (s_ncalls < TEXT_ID_CALLSITES) {
        s_calls[s_ncalls].ra = ra;
        s_calls[s_ncalls].n  = 0;
        s_ncalls++;
    }

    /* Walk the string exactly as the emitter will: one moby per non-space. */
    uint32_t str = cpu->a0;
    uint32_t glyph = 0;
    for (uint32_t k = 0; k < 128u; k++) {
        uint8_t c = (uint8_t)cpu->read_byte(str + k);
        if (c == 0) break;
        if (c == ' ') continue;
        uint32_t m = cur - (glyph + 1u) * MOBY_STRIDE;
        text_id_insert(m, text_id_key(ra, call_ord, glyph, c),
                       spaced ? glyph_class_spaced(c) : glyph_class_plain(c));
        glyph++;
    }

    s_last_ptr  = cur - glyph * MOBY_STRIDE;
    s_last_base = base;
    return 0;
}

/** @brief Entry hook for func_80017FE4 that records its glyphs with the plain class mapping.
 *
 * @param cpu The guest CPU state at the emitter's entry.
 * @param addr Unused.
 * @return 0, so the recompiled emitter runs. */
static int on_80017FE4_text(CPUState* cpu, uint32_t addr) {
    (void)addr;
    return text_emit_entry(cpu, 0);
}

/** @brief Entry hook for func_800181AC that records its glyphs with the spacing-aware class mapping.
 *
 * @param cpu The guest CPU state at the emitter's entry.
 * @param addr Unused.
 * @return 0, so the recompiled emitter runs. */
static int on_800181AC_text(CPUState* cpu, uint32_t addr) {
    (void)addr;
    return text_emit_entry(cpu, 1);
}

/** @brief Entry hook on the HUD pool drain func_80018880 that clears the glyph table on the second drain without an emit.
 *
 * The first drain after an emit marks the generation closed; its records
 * stay valid for this tick's capture and presents. A second drain with no
 * emit in between means a whole draw allocated no text, and the records
 * then describe recycled pool addresses.
 * @param cpu Unused.
 * @param addr Unused.
 * @return 0, so the recompiled function runs. */
static int on_80018880_drain(CPUState* cpu, uint32_t addr) {
    (void)cpu; (void)addr;
    if (s_closed) text_id_clear();
    s_closed = 1;
    return 0;
}

static const feature_dispatch_entry_t text_moby_id_dispatch[] = {
    { 0x80017FE4u, on_80017FE4_text },   /* func_80017FE4 - plain text */
    { 0x800181ACu, on_800181AC_text },   /* func_800181AC - spacing-aware */
    { 0x80018880u, on_80018880_drain },  /* func_80018880 - pool drain */
    { 0u, NULL },
};

const feature_module_t text_moby_id_module = {
    .name           = "text_moby_id",
    .apply_options  = NULL,
    .dispatch_table = text_moby_id_dispatch,
};
