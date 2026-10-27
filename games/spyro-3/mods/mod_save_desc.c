// games/spyro-3/mods/mod_save_desc.c - Spyro 3's game save descriptor
// (openpete_modsave.h).
//
// The card holds one file, BASCUS-94467SPY3_1: a 0x200-byte card header, a
// 0x80-byte summary at 0x200, then three 0x900-byte SaveFile records at
// 0x280 + index * 0x900 (spyro-3/include/memcard.h). The title (overlay
// 9), loading and options overlays each carry libmcrd, which calls the
// BIOS bu file layer with O_NOWAIT, so every write completes
// asynchronously. Writes: the whole file at creation (three empty
// records), and before each save the summary alone (no record), then the
// record.

#include "openpete_modsave.h"
#include "dispatch_preamble.h"
#include "openpete_psx.h"
#include "psx_runtime.h"

#include <string.h>

#define S3_REC_BYTES     0x900u
#define S3_CHECKSUM_OFS  0x8FCu      /* SaveFile.checksum */
#define S3_VISITED0_OFS  0x8u        /* SaveFile.visitedLevels[0] */
#define S3_OVL_TITLE     9u
#define S3_CHOSEN        0x800725B8u /* a save is chosen */
#define S3_CHOSEN_PORT   0x800725BCu
#define S3_CHOSEN_INDEX  0x800725C0u

/** @brief Empty by func_title_8007928C: visitedLevels[0] == 0. */
static int s3_is_empty(const uint8_t* rec) { return rec[S3_VISITED0_OFS] == 0; }

/** @brief The game's verdict: the byte sum of record[0, 0x8FC) equals SaveFile.checksum. */
static int s3_checksum_ok(const uint8_t* rec) {
    uint32_t sum = 0, want;
    for (uint32_t i = 0; i < S3_CHECKSUM_OFS; i++) sum += rec[i];
    memcpy(&want, rec + S3_CHECKSUM_OFS, 4);
    return sum == want;
}

/** @brief func_title_80078EAC's argument: the chosen record, freshly re-read from the card. */
static uint32_t s3_load_record(CPUState* cpu) { return cpu->a0; }

/** @brief Read a guest word, 0 when unmapped. */
static int32_t s3_word(uint32_t va) {
    const uint8_t* p = (const uint8_t*)openpete_psx_to_host_ptr(va);
    int32_t v = 0;
    if (p) memcpy(&v, p, 4);
    return v;
}

/** @brief The chosen save: D_800725B8 set, port D_800725BC and index D_800725C0, written before the load binding runs. */
static int s3_chosen(CPUState* cpu, int* slot, int* index) {
    (void)cpu;
    if (!s3_word(S3_CHOSEN)) return 0;
    *slot = s3_word(S3_CHOSEN_PORT) & 1;
    *index = s3_word(S3_CHOSEN_INDEX);
    return *index >= 0 && *index < 3;
}

static const char* const k_files[] = { "BASCUS-94467SPY3_1", NULL };
/* func_title_80078918 clears progress at every title menu entry (boot,
 * after each attract demo, after Quit Game). */
static const uint32_t k_resets[] = { 0x80078918u };
static const uint32_t k_reset_ovls[] = { S3_OVL_TITLE };

const openpete_save_desc_t spyro3_save_desc = {
    .file_names       = k_files,
    .record_base      = 0x280u,
    .record_size      = S3_REC_BYTES,
    .record_count     = 3u,
    .hash_ofs         = 0u,
    .hash_len         = S3_REC_BYTES,
    .legacy_ofs       = 0u,
    .legacy_len       = 0u,
    .is_empty         = s3_is_empty,
    .checksum_ok      = s3_checksum_ok,
    .async_write      = 1,
    .save_create_fn   = 0u,
    .load_fn          = 0x80078EACu, /* func_title_80078EAC */
    .load_record      = s3_load_record,
    .chosen           = s3_chosen,
    .reset_fns        = k_resets,
    .reset_count      = 1u,
    .delivery_fn      = 0x80055400u, /* Update */
    .resident_overlay = game_resident_overlay_probe,
    .load_ovl         = S3_OVL_TITLE,
    .delivery_ovl     = 0u,
    .reset_ovls       = k_reset_ovls,
};
