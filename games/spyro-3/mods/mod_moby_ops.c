// games/spyro-3/mods/mod_moby_ops.c - Spyro 3 game ops for the mod content
// registry (core/mods/mod_registry.c).
//
// Ground truth (psx.map, asm/data/game.bss.s, include/common.h, src/main.c):
//   - Model table D_8006EE2C: 768 x 4 B rows (bss 0x8006EE2C..0x8006FA2C)
//     indexed directly by Moby.m_Class (+0x36): func_80038BF8 loads
//     D_8006EE2C[lh 0x36(moby) * 4]; func_8002B810 bounds the class with
//     `slti m_Class, 0x300` and tests the row for zero before using it.
//     func_8002AF9C writes a row as a KSEG0 animated-model pointer or a
//     KUSEG simple-model pointer. No load clears the table, so rows of
//     earlier levels stay set; ops_classes_loaded reads the load's lists.
//   - Gameplay gate: game.state @ 0x8006E344 == 0 (GAMESTATE_GAMEPLAY).
//     Reading the table only in gameplay keeps the census off a table the
//     loader is still filling.
//   - Level id: currentLevel @ 0x8006C5BC, the loader's level number
//     (10..50).
//   - Master update: Update @ 0x80055400, the first call of each main-loop
//     iteration in main (func_8001200C: { Update(); Draw(); } forever).

#include "openpete_mods.h"

#include "psx_runtime_internal.h" /* CPUState */

#define S3_MODEL_TABLE   0x8006EE2Cu
#define S3_MODEL_ROWS    768
#define S3_GAME_STATE    0x8006E344u
#define S3_CURRENT_LEVEL 0x8006C5BCu
/* A level's models reach the table through func_8002AF9C from two lists, the
 * two loads func_80052A84 (loader state 4) issues:
 *   - the base load (D_8006C5C8 == 0, 0x80052B84) calls func_8002B31C, whose
 *     loop (0x8002B588-0x8002B5B8) walks the header list: entry i while the
 *     word at 0x80072140 + 4i is positive, class id the halfword at
 *     0x8007223A + 2i;
 *   - the sub-load (D_8006C5C8 != 0, 0x80052C2C) walks a second list in the
 *     loaded data: s1 = lw(0x800722C4), s2 = s1 + lw(s1), entry i while the
 *     word at s2 + 8 + 4i is positive, class id the halfword at
 *     s2 + 0x102 + 2i. With D_8006C5C8 == 0 that region holds no list.
 * Each list's words end at its class halfwords, so at most 62 entries. */
#define S3_HEADER_NEXT   0x80072140u
#define S3_HEADER_CLASS  0x8007223Au
#define S3_SUBLOAD_INDEX 0x8006C5C8u
#define S3_SUBLOAD_BASE  0x800722C4u
#define S3_LIST_MAX      62u

/** @brief Return the current level id, or 0 outside gameplay.
 *
 * @param cpu The guest CPU state.
 * @return currentLevel while game.state is GAMESTATE_GAMEPLAY, else 0. */
static int ops_level_id(CPUState* cpu) {
    if (cpu->read_word(S3_GAME_STATE) != 0u) return 0;
    return (int)cpu->read_word(S3_CURRENT_LEVEL);
}

/** @brief Mark the classes of one model list func_8002AF9C is called over.
 *
 * @param cpu The guest CPU state.
 * @param next Address of entry 0's continuation word; entry i is listed
 *        while the word at next + 4i is positive.
 * @param cls Address of entry 0's class id halfword.
 * @param listed Per-class flags, set for each listed class below
 *        S3_MODEL_ROWS. */
static void mark_list(CPUState* cpu, uint32_t next, uint32_t cls,
                      uint8_t* listed) {
    for (uint32_t i = 0; i < S3_LIST_MAX; i++) {
        if ((int32_t)cpu->read_word(next + 4u * i) <= 0) break;
        uint32_t c = cpu->read_half(cls + 2u * i);
        if (c < S3_MODEL_ROWS) listed[c] = 1;
    }
}

/** @brief List the moby classes the current level's load placed in the model table.
 *
 * The table keeps rows from every level loaded this session, and a stale
 * row points into memory the current level's load has overwritten, so a
 * nonzero row alone does not name a usable class. The classes are those of
 * the lists the load walked: the base load's header list always, the
 * sub-load's list while D_8006C5C8 is nonzero. Each listed class whose row
 * is nonzero is returned, ascending. Outside gameplay the table may be
 * mid-load and the call returns -1.
 * @param cpu The guest CPU state.
 * @param out Receives up to `cap` class ids in ascending order.
 * @param cap The capacity of `out`.
 * @return The number of ids written, or -1 outside gameplay. */
static int ops_classes_loaded(CPUState* cpu, uint16_t* out, int cap) {
    if (cpu->read_word(S3_GAME_STATE) != 0u) return -1;
    uint8_t listed[S3_MODEL_ROWS] = {0};
    mark_list(cpu, S3_HEADER_NEXT, S3_HEADER_CLASS, listed);
    if (cpu->read_word(S3_SUBLOAD_INDEX) != 0u) {
        uint32_t s1 = cpu->read_word(S3_SUBLOAD_BASE);
        uint32_t s2 = s1 + cpu->read_word(s1);
        if ((s1 >> 24) == 0x80u && (s2 >> 24) == 0x80u)   /* KSEG0 RAM */
            mark_list(cpu, s2 + 8u, s2 + 0x102u, listed);
    }
    int n = 0;
    for (int id = 0; id < S3_MODEL_ROWS && n < cap; id++)
        if (listed[id] && cpu->read_word(S3_MODEL_TABLE + 4u * (uint32_t)id))
            out[n++] = (uint16_t)id;
    return n;
}

static const openpete_moby_ops_t s_spyro3_moby_ops = {
    .update_hook_addr = 0x80055400u, /* Update */
    .level_id         = ops_level_id,
    .classes_loaded   = ops_classes_loaded,
};

/** @brief Return Spyro 3's moby ops, the game descriptor's moby_ops member.
 *
 * @return The static ops table. */
const openpete_moby_ops_t* spyro3_moby_ops(void) {
    return &s_spyro3_moby_ops;
}
