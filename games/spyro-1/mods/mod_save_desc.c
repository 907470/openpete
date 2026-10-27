// games/spyro-1/mods/mod_save_desc.c - Spyro 1's game save descriptor
// (openpete_modsave.h).
//
// The card holds one file, BASCUS-94228SPYRO: a 0x200-byte header, then
// three 0x600-byte SaveFile records (spyro-1/include/save_file.h). The
// title writes the whole file at creation; the fairy save
// (func_800314B4) writes one record at slot * 0x600 + 0x200 through the
// MemCardWriteFile thunk, synchronously. SaveLoad receives the record,
// a pointer into the title's whole-file read buffer.

#include "openpete_modsave.h"
#include "psx_runtime.h"
#include "openpete_psx.h"

#include <string.h>

#define S1_REC_BYTES     0x600u
#define S1_CHECKSUM_OFS  0x58Cu  /* SaveFile.m_Checksum */
#define S1_PAD_OFS       0x590u  /* SaveFile.pad[0x70], outside the checksum */
#define S1_VISITED0_OFS  0x40u   /* SaveFile.m_LevelsVisited[0] */
#define S1_FAIRY         0x80078D00u /* g_FairyCutscene */

/** @brief Empty by the title's predicate: m_LevelsVisited[0] == 0. */
static int s1_is_empty(const uint8_t* rec) { return rec[S1_VISITED0_OFS] == 0; }

/** @brief SaveChecksum's verdict: the byte sum of the record up to m_Checksum equals it. */
static int s1_checksum_ok(const uint8_t* rec) {
    uint32_t sum = 0, want;
    for (uint32_t i = 0; i < S1_CHECKSUM_OFS; i++) sum += rec[i];
    memcpy(&want, rec + S1_CHECKSUM_OFS, 4);
    return sum == want;
}

/** @brief SaveLoad's argument: the record's guest address. */
static uint32_t s1_load_record(CPUState* cpu) { return cpu->a0; }

/** @brief The card and save the title selected: g_FairyCutscene.m_MemoryCardSlot (port << 4) and unk_0x1c, which START_DEMO sets. */
static int s1_chosen(CPUState* cpu, int* slot, int* index) {
    (void)cpu;
    const uint8_t* f = (const uint8_t*)openpete_psx_to_host_ptr(S1_FAIRY);
    if (!f) return 0;
    int32_t port, idx;
    memcpy(&port, f + 0x18, 4);
    memcpy(&idx, f + 0x1C, 4);
    *slot = (port >> 4) & 1;
    *index = idx;
    return idx >= 0 && idx < 3;
}

static const char* const k_files[] = { "BASCUS-94228SPYRO", NULL };
/* func_8001277C clears progress: title New Game and card paths, demo-level
 * setup, and the end of the intro cutscene. */
static const uint32_t k_resets[] = { 0x8001277Cu };

const openpete_save_desc_t spyro1_save_desc = {
    .file_names     = k_files,
    .record_base    = 0x200u,
    .record_size    = S1_REC_BYTES,
    .record_count   = 3u,
    .hash_ofs       = 0u,
    .hash_len       = S1_REC_BYTES,
    .legacy_ofs     = S1_PAD_OFS,
    .legacy_len     = S1_REC_BYTES - S1_PAD_OFS,
    .is_empty       = s1_is_empty,
    .checksum_ok    = s1_checksum_ok,
    .async_write    = 0,
    .save_create_fn = 0x80059864u, /* SaveCreate */
    .load_fn        = 0x80059594u, /* SaveLoad */
    .load_record    = s1_load_record,
    .chosen         = s1_chosen,
    .reset_fns      = k_resets,
    .reset_count    = 1u,
    .delivery_fn    = 0x8003385Cu, /* GamestateUpdate */
};
