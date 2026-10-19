// games/spyro-2/mods/mod_moby_ops.c - Spyro 2 game ops for the mod content
// registry (core/mods/mod_registry.c).
//
// Ground truth (asm/mobydraw.s, asm/data/game.bss.s, asm/nonmatchings/
// loaders/func_8001C734.s, manifest/spyro2_symbols.toml):
//   - Moby array G_MOBYARRAY @ 0x80066F14: a pointer to 0x58-byte records,
//     the class halfword at +0x36 and the state byte at +0x48; the record
//     whose state byte is -1 ends the array (the cull func_80043858,
//     mobydraw.s:63-70, `lhu 0x36` at :113). States below -1 are slots the
//     walk skips but does not end on.
//   - Model table G_MOBYMODELS @ 0x80068C94: 768 x 4 B rows (bss .space
//     0xC00) indexed directly by the class: the cull loads row class * 4
//     (mobydraw.s:113-116) and the level loader func_8001C734 bounds the
//     class with `slti class, 0x300` and tests the row for zero before
//     using it (0x8001D060-0x8001D088). No load clears the table: rows of
//     earlier levels stay set, so a nonzero row alone does not name a class
//     of the current level.
//   - Level regime G_GAMESTATE @ 0x80066EEC: -1 from the load's end through
//     the level's opening, 0 in play and on the title, 1..7 across a level
//     transition (2 loads G_LEVELID, 4..7 the destination's data).
//   - Level id G_LEVELID @ 0x80066F90 (10..66; 0 before the first load);
//     level tick 0x80067008, 0 through the load and the overlay swap.
//   - Master update: func_8001B140, the first call of each main-loop
//     iteration (func_80011ADC), one hit per tick (oracle.conf ANCHOR_PC).

#include "openpete_mods.h"

#include "psx_runtime_internal.h" /* CPUState */

#define S2_MOBY_ARRAY    0x80066F14u
#define S2_MOBY_STRIDE   0x58u
#define S2_MOBY_CLASS    0x36u
#define S2_MOBY_STATE    0x48u
#define S2_MOBY_SCAN_MAX 4096u
#define S2_MODEL_TABLE   0x80068C94u
#define S2_MODEL_ROWS    768u
#define S2_GAME_STATE    0x80066EECu
#define S2_STATE_OPENING 0xFFFFFFFFu
#define S2_STATE_PLAY    0u
#define S2_LEVEL_ID      0x80066F90u
#define S2_LEVEL_FIRST   10u
#define S2_LEVEL_TICK    0x80067008u

/** @brief Return the current level id, or 0 outside a level that has started ticking.
 *
 * A level is live from its opening (G_GAMESTATE -1) through play (0) once
 * its own tick counter has started; the title also runs at regime 0 but
 * before any load, with G_LEVELID below the first level number.
 * @param cpu The guest CPU state.
 * @return G_LEVELID while a level is live, else 0. */
static int ops_level_id(CPUState* cpu) {
    uint32_t regime = cpu->read_word(S2_GAME_STATE);
    uint32_t level  = cpu->read_word(S2_LEVEL_ID);
    if ((regime != S2_STATE_OPENING && regime != S2_STATE_PLAY) ||
        level < S2_LEVEL_FIRST || cpu->read_word(S2_LEVEL_TICK) == 0u)
        return 0;
    return (int)level;
}

/** @brief List the moby classes of the current level's moby array that have a model.
 *
 * The model table keeps rows from every level loaded this session, so the
 * classes are taken from the moby array: every record up to the -1
 * terminator, whatever its state, whose class row in the model table is
 * nonzero. A record with no model row is a logic-only moby that draws
 * nothing and is left out. The set holds the classes the level placed
 * plus any spawned since, ascending. Outside a live level (ops_level_id)
 * the array may be the title's or mid-load and the call returns -1.
 * @param cpu The guest CPU state.
 * @param out Receives up to `cap` class ids in ascending order.
 * @param cap The capacity of `out`.
 * @return The number of ids written, or -1 outside a live level. */
static int ops_classes_loaded(CPUState* cpu, uint16_t* out, int cap) {
    if (ops_level_id(cpu) == 0) return -1;
    uint8_t listed[S2_MODEL_ROWS] = {0};
    uint32_t rec = cpu->read_word(S2_MOBY_ARRAY);
    for (uint32_t i = 0; i < S2_MOBY_SCAN_MAX; i++, rec += S2_MOBY_STRIDE) {
        if ((int8_t)cpu->read_byte(rec + S2_MOBY_STATE) == -1) break;
        uint32_t c = cpu->read_half(rec + S2_MOBY_CLASS);
        if (c < S2_MODEL_ROWS) listed[c] = 1;
    }
    int n = 0;
    for (uint32_t id = 0; id < S2_MODEL_ROWS && n < cap; id++)
        if (listed[id] && cpu->read_word(S2_MODEL_TABLE + 4u * id))
            out[n++] = (uint16_t)id;
    return n;
}

static const openpete_moby_ops_t s_spyro2_moby_ops = {
    .update_hook_addr = 0x8001B140u, /* main-loop tick entry */
    .level_id         = ops_level_id,
    .classes_loaded   = ops_classes_loaded,
};

/** @brief Return Spyro 2's moby ops, the game descriptor's moby_ops member.
 *
 * @return The static ops table. */
const openpete_moby_ops_t* spyro2_moby_ops(void) {
    return &s_spyro2_moby_ops;
}
