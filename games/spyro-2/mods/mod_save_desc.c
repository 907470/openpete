// games/spyro-2/mods/mod_save_desc.c - Spyro 2's game save descriptor
// (openpete_modsave.h).
//
// The card holds one file, BASCUS-94425SP2RR: a 0x200-byte header, then
// three 0x600-byte records at 0x200 + index * 0x600. The card code lives in
// the titlescreen_and_loading overlay (id 34), which the pause save swaps in
// for the level; its libmcrd copy calls the BIOS bu file layer with
// O_NOWAIT, so every write completes asynchronously. Writes: the whole
// file at creation (0x1400 bytes at 0, three settings-only records), a
// record (0x600 at its offset) from the pause save and the story-step
// autosave, and a 0x80-byte header write that covers no record.

#include "openpete_modsave.h"
#include "dispatch_preamble.h"
#include "openpete_psx.h"
#include "psx_runtime.h"

#include <string.h>

#define S2_REC_BYTES     0x600u
#define S2_CHECKSUM_OFS  0x5A8u      /* the word func_8007663C compares */
#define S2_VISITED1_OFS  0x11u       /* visited flag of level 11 (Glimmer) */
#define S2_OVL_TITLE     34u         /* titlescreen_and_loading */
#define S2_SELECTED      0x80067E20u /* 1 when a save is selected */
#define S2_SEL_INDEX     0x80067E24u
#define S2_SEL_PORT      0x80067E28u

/** @brief Empty by the title's load cursor: record byte 0x11 (the Glimmer visited flag) is zero. */
static int s2_is_empty(const uint8_t* rec) { return rec[S2_VISITED1_OFS] == 0; }

/** @brief The verdict of func_80076614 / func_8007663C: the byte sum of record[0, 0x5A8) equals the word at 0x5A8. */
static int s2_checksum_ok(const uint8_t* rec) {
    uint32_t sum = 0, want;
    for (uint32_t i = 0; i < S2_CHECKSUM_OFS; i++) sum += rec[i];
    memcpy(&want, rec + S2_CHECKSUM_OFS, 4);
    return sum == want;
}

/** @brief The load binding's argument: the record's guest address in the whole-file read buffer. */
static uint32_t s2_load_record(CPUState* cpu) { return cpu->a0; }

/** @brief Read a guest word, 0 when unmapped. */
static int32_t s2_word(uint32_t va) {
    const uint8_t* p = (const uint8_t*)openpete_psx_to_host_ptr(va);
    int32_t v = 0;
    if (p) memcpy(&v, p, 4);
    return v;
}

/** @brief The selected save: 0x80067E20 set, port 0x80067E28 and index 0x80067E24 (the title load and every save set them). */
static int s2_chosen(CPUState* cpu, int* slot, int* index) {
    (void)cpu;
    if (!s2_word(S2_SELECTED)) return 0;
    *slot = s2_word(S2_SEL_PORT) & 1;
    *index = s2_word(S2_SEL_INDEX);
    return *index >= 0 && *index < 3;
}

static const char* const k_files[] = { "BASCUS-94425SP2RR", NULL };
/* func_titlescreen_and_loading_8007644C clears progress (next level 11,
 * the visited array): title New Game, title Load before the decode, and
 * every attract-demo start. */
static const uint32_t k_resets[] = { 0x8007644Cu };
static const uint32_t k_reset_ovls[] = { S2_OVL_TITLE };

const openpete_save_desc_t spyro2_save_desc = {
    .file_names       = k_files,
    .record_base      = 0x200u,
    .record_size      = S2_REC_BYTES,
    .record_count     = 3u,
    .hash_ofs         = 0u,
    .hash_len         = S2_REC_BYTES,
    .legacy_ofs       = 0u,
    .legacy_len       = 0u,
    .is_empty         = s2_is_empty,
    .checksum_ok      = s2_checksum_ok,
    .async_write      = 1,
    .save_create_fn   = 0u,
    .load_fn          = 0x800769BCu, /* func_800769BC, LoadGame */
    .load_record      = s2_load_record,
    .chosen           = s2_chosen,
    .reset_fns        = k_resets,
    .reset_count      = 1u,
    .delivery_fn      = 0x8001B140u, /* Update */
    .resident_overlay = game_resident_overlay_probe,
    .load_ovl         = S2_OVL_TITLE,
    .delivery_ovl     = 0u,
    .reset_ovls       = k_reset_ovls,
};
