// games/common/psyq/psyq_sfx_slots.c - the sfx pack's view of a game's sound
// tables.
//
// The sfx pack replaces a keyed voice's sample by content hash or by the
// name of the SoundTable slot that plays it. Both need the game's tables:
// the slot-name resolver maps an SPU sample address back to every slot
// whose definition owns it, and the pitch reader reports the base pitch the
// game keeps writing to a voice, which a symbolic file folds into its rate.
// The layout comes from the game's psyq_game_sfx_slots() descriptor; this
// file reads guest RAM only and writes none.

#include "psyq/psyq_game_hooks.h"
#include "openpete_psx.h"
#include "sfx_pack.h"

#include <string.h>

#define SFX_SLOTS          256u
#define SFX_DEF_STRIDE     0x14u  /* SoundDefinition: SPU address first */
#define SFX_TABLE_BYTES    (SFX_SLOTS + 4u) /* slots, then the definition count */
#define SFX_VOICES         24

/** @brief Read a little-endian guest word, or 0 when its four bytes are not mapped. */
static uint32_t slots_word(uint32_t va) {
    const void* p = openpete_psx_to_host_span(va, 4u);
    uint32_t v = 0;
    if (p) memcpy(&v, p, 4);
    return v;
}

/** @brief Resolve an SPU sample address to the names of the slots whose definition owns it.
 *
 * Walks the 256 slot bytes at the SoundTable pointer, maps each slot through
 * its definition index to the SoundDefinition array, and names every slot
 * whose definition's address equals `spu_addr`. Returns no names unless the
 * definition pointer equals the table pointer plus 0x104, the arrangement
 * the level loader writes: a cutscene, title or loading screen repoints the
 * definitions at a short array of its own that the level slots do not
 * index. Also none while either pointer is NULL or the definition count
 * (the word after the slots) is outside 1..256.
 * @param g The game's descriptor; not NULL.
 * @param spu_addr SPU RAM address of the sample.
 * @param out Receives up to `max` slot name pointers.
 * @param max The capacity of `out`.
 * @return The number of names written. */
static int slots_names(const psyq_sfx_slots_t* g, uint32_t spu_addr,
                       const char** out, int max) {
    const uint32_t table_va = slots_word(g->table_ptr_va);
    const uint32_t defs_va  = slots_word(g->defs_ptr_va);
    if (!table_va || defs_va != table_va + SFX_TABLE_BYTES) return 0;
    const uint8_t* table =
        (const uint8_t*)openpete_psx_to_host_span(table_va, SFX_TABLE_BYTES);
    if (!table) return 0;
    int32_t count;
    memcpy(&count, table + SFX_SLOTS, 4);
    if (count <= 0 || count > (int32_t)SFX_SLOTS) return 0;
    const uint8_t* defs = (const uint8_t*)openpete_psx_to_host_span(
        defs_va, (uint32_t)count * SFX_DEF_STRIDE);
    if (!defs) return 0;
    int n = 0;
    for (uint32_t slot = 0; slot < SFX_SLOTS && n < max; slot++) {
        const uint8_t id = table[slot];
        if (id >= count) continue;
        uint32_t addr;
        memcpy(&addr, defs + id * SFX_DEF_STRIDE, 4);
        if (addr == spu_addr) out[n++] = g->names[slot];
    }
    return n;
}

/** @brief The sfx pack's name resolver over the running game's descriptor.
 * @param addr SPU RAM address of the keyed sample.
 * @param names Receives up to `max` slot names.
 * @param max The capacity of `names`.
 * @return The number of names written. */
static int slots_resolver(uint32_t addr, const char** names, int max) {
    const psyq_sfx_slots_t* g = psyq_game_sfx_slots();
    return g ? slots_names(g, addr, names, max) : 0;
}

/** @brief The sfx pack's pitch reader: the pitch word of voice's ActiveSound, or 0 when out of range (0 or 0x4000 and above).
 * @param voice SPU voice index.
 * @return The base pitch, or 0 when unknown. */
static int slots_base_pitch(int voice) {
    const psyq_sfx_slots_t* g = psyq_game_sfx_slots();
    if (!g || voice < 0 || voice >= SFX_VOICES) return 0;
    const uint32_t p = slots_word(g->active_va + (uint32_t)voice * g->active_stride +
                                  g->active_pitch_off);
    return (p > 0 && p < 0x4000u) ? (int)p : 0;
}

void psyq_game_sfx_arm(void) {
    static int s_armed = 0;
    if (s_armed || !psyq_game_sfx_slots()) return;
    s_armed = 1;
    sfx_pack_set_pitch_reader(slots_base_pitch);
    sfx_pack_set_name_resolver(slots_resolver);
}

unsigned psyq_game_spu_bank_blast_len(void) {
    const psyq_sfx_slots_t* g = psyq_game_sfx_slots();
    return g ? g->bank_blast_len : 0u;
}

int psyq_game_sfx_slot_names(unsigned spu_addr, const char** out, int max) {
    return slots_resolver(spu_addr, out, max);
}
