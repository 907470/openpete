// games/spyro-2/features/ctr_demo_egg.c - the Crash Team Racing demo
// launcher on Spyro 2's title screen, played as Aku Aku's sound.
//
// The title update func_titlescreen_and_loading_80079DF0, in the press-start
// state (D_80067E04 == 0, D_80067E0C == 2) with the title timer D_80068C44
// at 0x348 or more, compares the pad word D_800683A0 with 0x86 (L1 + R2 +
// Square) and on a match calls func_80050AAC (stop every voice),
// func_80050D38, func_8004D810 and the launcher
// func_titlescreen_and_loading_8007CBD0 (ResetGraph and callback teardown,
// then LoadExec("cdrom:\KART\PSX.EXE;1")), then leaves the update. The four
// calls are skipped (demo_egg.h) and the demo's sample plays.
//
// The sample is bank 0 sample 53 of KART/KART.HWL, the CTR demo's sound
// archive, at the rate the demo's sound-effect table gives it.
// KART.HWL layout, little-endian:
//   +16  u32 sample count nU, sound-effect count nS, engine-sound count nE,
//        bank count nB, sequence count nQ
//   +40  u32 sample[nU]: bits 16-31 the ADPCM length in 8-byte units
//        then 8-byte sound effects {u8 flags, u8 vol, u16 pitch, u16 sample,
//        u16}, nS of them, then nE engine sounds of the same shape,
//        then u16 bank[nB]: each bank's first 2048-byte sector
//   bank: u16 n, u16 sample id[n]; the n ADPCM bodies follow from the
//        bank's next sector, in id order.
// A pitch p plays at p * 44100 / 4096 Hz.

#include "features/demo_egg.h"

#include "feature_registry.h"
#include "openpete.h"              /* openpete_options_t */
#include "openpete_disc_image.h"
#include "openpete_log.h"

#include <stdlib.h>
#include <string.h>

#define EGG_FILE        "KART/KART.HWL"
#define EGG_BANK        0u
#define EGG_INDEX       53u
#define HWL_SECTOR      2048u
#define HWL_COUNTS      16u      /* five u32 counts */
#define HWL_TABLES      40u      /* the sample table */
#define HWL_MAX_COUNT   4096u    /* sanity bound on each count */
/* The sample this locator is expected to find, as an assertion. */
#define EGG_EXPECT_OFF  0x384C0u
#define EGG_EXPECT_LEN  7936u

#define OVERLAY_TITLE   34u      /* titlescreen_and_loading */

static const demo_egg_site_t k_sites[] = {
    { 0x80050AACu, 0x8007A1BCu },
    { 0x80050D38u, 0x8007A1C4u },
    { 0x8004D810u, 0x8007A1CCu },
    { 0x8007CBD0u, 0x8007A1D4u },
};

/** @brief Reads exactly len bytes of the HWL at offset into a new buffer.
 *
 * @param img The disc.
 * @param e The HWL's directory entry.
 * @param off Byte offset in the file.
 * @param len Byte count.
 * @return The bytes, freed by the caller, or NULL on a short read. */
static uint8_t* read_range(disc_image_t* img, const disc_image_entry_t* e,
                           uint32_t off, uint32_t len) {
    uint8_t* b = (uint8_t*)malloc(len ? len : 1u);
    if (b && disc_image_read_file_range(img, e, off, b, len) != len) {
        free(b);
        b = NULL;
    }
    return b;
}

/** @brief Finds the rate of a sample id in the sound-effect, then the engine-sound table.
 *
 * @param fx The sound-effect table followed by the engine-sound table.
 * @param n Entries in both tables together.
 * @param sid The sample id.
 * @return The rate in Hz, rounded, or 0 when no entry plays the sample. */
static uint32_t rate_of(const uint8_t* fx, uint32_t n, uint16_t sid) {
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* r = fx + i * 8u;
        if (demo_egg_le16(r + 4) == sid)
            return (demo_egg_le16(r + 2) * 44100u + 2048u) / 4096u;
    }
    return 0;
}

/** @brief Walks the HWL's sample, sound-effect and bank tables to sample EGG_INDEX of bank EGG_BANK.
 *
 * @param img The disc.
 * @param e The HWL's directory entry.
 * @param tab The tables from file offset HWL_TABLES on: samples, sound
 *        effects, engine sounds, banks.
 * @param c The five counts.
 * @param at Receives the sample's byte offset in the file.
 * @param len Receives its byte length.
 * @param rate Receives its rate in Hz, 0 when no table entry plays it.
 * @return 0, or -1 when the bank is unreadable or an id lies out of range. */
static int walk_bank(disc_image_t* img, const disc_image_entry_t* e, const uint8_t* tab,
                     const uint32_t c[5], uint32_t* at, uint32_t* len, uint32_t* rate) {
    uint32_t fx_off = 4u * c[0];
    uint32_t bank_off = fx_off + 8u * (c[1] + c[2]);
    uint32_t bank_at = demo_egg_le16(tab + bank_off + 2u * EGG_BANK) * HWL_SECTOR;
    uint8_t* bank = read_range(img, e, bank_at, HWL_SECTOR);
    if (!bank) return -1;
    int rc = -1;
    uint32_t n = demo_egg_le16(bank);
    if (n > EGG_INDEX && 2u + 2u * n <= HWL_SECTOR) {
        uint32_t off = bank_at + HWL_SECTOR;
        uint32_t k = 0;
        for (; k < EGG_INDEX; k++) {
            uint16_t id = demo_egg_le16(bank + 2u + 2u * k);
            if (id >= c[0]) break;
            off += (demo_egg_le32(tab + 4u * id) >> 16) * 8u;
        }
        uint16_t sid = demo_egg_le16(bank + 2u + 2u * EGG_INDEX);
        if (k == EGG_INDEX && sid < c[0]) {
            *at = off;
            *len = (demo_egg_le32(tab + 4u * sid) >> 16) * 8u;
            *rate = rate_of(tab + fx_off, c[1] + c[2], sid);
            rc = 0;
        }
    }
    free(bank);
    return rc;
}

/** @brief Locates bank 0 sample 53 of KART.HWL through the archive's sample, sound-effect and bank tables.
 *
 * @param img The disc.
 * @param out Receives the ADPCM body and its rate.
 * @return 0 when found and equal to the expected offset and length, else -1
 *         with the reason logged. */
static int ctr_locate(disc_image_t* img, demo_egg_sample_t* out) {
    disc_image_entry_t e;
    if (disc_image_find(img, EGG_FILE, &e) != 0 || e.is_dir) {
        OPL_INFO_F("game", "ctr-demo-egg: %s not on this disc", EGG_FILE);
        return -1;
    }
    uint8_t* head = read_range(img, &e, 0, HWL_TABLES);
    uint32_t c[5] = {0};
    for (int i = 0; head && i < 5; i++)
        c[i] = demo_egg_le32(head + HWL_COUNTS + 4u * (uint32_t)i);
    free(head);
    int sane = c[0] <= HWL_MAX_COUNT && c[1] <= HWL_MAX_COUNT && c[2] <= HWL_MAX_COUNT &&
               c[3] > EGG_BANK && c[3] <= HWL_MAX_COUNT;
    uint8_t* tab = sane ? read_range(img, &e, HWL_TABLES,
                                     4u * c[0] + 8u * (c[1] + c[2]) + 2u * c[3]) : NULL;
    uint32_t at = 0, len = 0, rate = 0;
    int found = tab && walk_bank(img, &e, tab, c, &at, &len, &rate) == 0;
    free(tab);
    if (!found) {
        OPL_WARN_F("game", "ctr-demo-egg: %s: tables unreadable or out of range", EGG_FILE);
        return -1;
    }
    if (at != EGG_EXPECT_OFF || len != EGG_EXPECT_LEN || !rate) {
        OPL_WARN_F("game", "ctr-demo-egg: %s tables give +0x%X, %u bytes, %u Hz; "
                   "expected +0x%X, %u bytes", EGG_FILE, at, len, rate,
                   EGG_EXPECT_OFF, EGG_EXPECT_LEN);
        return -1;
    }
    out->adpcm = read_range(img, &e, at, len);
    if (!out->adpcm) return -1;
    out->len = len;
    out->rate_hz = rate;
    return 0;
}

static demo_egg_t s_egg = {
    .name = "ctr-demo-egg",
    .overlay = OVERLAY_TITLE,
    .sites = k_sites,
    .n_sites = sizeof k_sites / sizeof k_sites[0],
    .locate = ctr_locate,
};

/** @brief Skips a launch-branch call and plays the sample on a new press; see demo_egg_on_call.
 *
 * @param cpu The guest CPU state.
 * @param addr The entered function's vaddr.
 * @return 1 to skip the guest function, else 0. */
static int on_call(CPUState* cpu, uint32_t addr) {
    return demo_egg_on_call(&s_egg, cpu, addr);
}

/** @brief Decodes the sample from the --disc image at boot.
 *
 * @param opts The options, or NULL. */
static void ctr_demo_egg_apply_options(const openpete_options_t* opts) {
    if (opts) demo_egg_load(&s_egg, opts->disc_path);
}

static const feature_dispatch_entry_t k_dispatch[] = {
    { 0x80050AACu, on_call, 0u },
    { 0x80050D38u, on_call, 0u },
    { 0x8004D810u, on_call, 0u },
    { 0x8007CBD0u, on_call, OVERLAY_TITLE },
    { 0, NULL, 0u },
};

const feature_module_t ctr_demo_egg_module = {
    .name           = "ctr-demo-egg",
    .apply_options  = ctr_demo_egg_apply_options,
    .dispatch_table = k_dispatch,
};
