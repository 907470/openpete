// games/spyro-3/features/bash_demo_egg.c - the Crash Bash demo launcher on
// Spyro 3's title screen, played as Crash's "woah".
//
// The title update func_title_8007566C, in title state 1 sub-state 1
// (D_title_8007F12E == 1), compares the pad word D_8006E538 with 0x86 (L1 +
// R2 + Square) and on a match calls func_8003BEDC, func_8003C184,
// func_80020168 and the launcher func_title_80078B88 (callback teardown,
// then LoadExec("cdrom:\CRASHBSH\SHELL.EXE;1")), then leaves the update
// (func_title_8007566C.s, 0x80075C98-0x80075CD0). The four calls are
// skipped (demo_egg.h) and the demo's sample plays.
//
// The sample is VAG 3 (the third sample) of the VAB in CRASHBSH/CRASHBSH.DAT
// that Crash Bash's shell lists as entry 107 of its archive directory (the
// (sector, byte size) table at SHELL.EXE file offset 0x3483C, 577 entries):
// sector 0x15FC, 0x5846 bytes. CRASHBSH.DAT holds no directory of its own.
// That entry is a chunk opening with a u32 offset table whose first entry
// is the table's own size: item 0 is the VAB body (VB), item 1 the VAB
// header (VH), the last entry the chunk's length. VH layout (PsyQ VAB):
//   +0   "pBAV"; +18 u16 program count; +22 u16 VAG count
//   +32  128 program records of 16 bytes
//   +2080 program count * 16 tone records of 32 bytes: +4 u8 centre note,
//        +5 u8 fine pitch (1/128 semitone), +22 s16 VAG number
//   then u16 VAG size[256] in 8-byte units, entry 0 unused; the chunk
//   stores only entries 0..VAG count, so the header ends there.
// VAG k starts after VAGs 1..k-1 in VB. A tone plays its VAG at
// 44100 * 2^((note - centre - fine / 128) / 12) Hz; at note 60 (middle C).

#include "features/demo_egg.h"

#include "feature_registry.h"
#include "openpete.h"              /* openpete_options_t */
#include "openpete_disc_image.h"
#include "openpete_log.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "openpete_thread_roles.h"

#define EGG_FILE        "CRASHBSH/CRASHBSH.DAT"
#define DAT_SECTOR      2048u
#define EGG_CHUNK_SECTOR 0x15FCu  /* SHELL.EXE directory entry 107 */
#define EGG_CHUNK_BYTES 0x5846u
#define EGG_VAG         3u        /* 1-based VAG number */
#define EGG_NOTE        60
#define VAB_PROGS       128u
#define VAB_PROG_BYTES  16u
#define VAB_TONE_BYTES  32u
#define VAB_TONES_PER_PROG 16u
#define VAB_VAG_SLOTS   256u
#define VAB_FIXED       (32u + VAB_PROGS * VAB_PROG_BYTES)
/* The sample this locator is expected to find, as an assertion. */
#define EGG_EXPECT_OFF  0xB0130Cu
#define EGG_EXPECT_LEN  2704u

#define OVERLAY_TITLE   9u        /* title */

static const demo_egg_site_t k_sites[] = {
    { 0x8003BEDCu, 0x80075CB4u },
    { 0x8003C184u, 0x80075CBCu },
    { 0x80020168u, 0x80075CC4u },
    { 0x80078B88u, 0x80075CCCu },
};

/** @brief Reads exactly len bytes of the DAT at offset into a new buffer.
 *
 * @param img The disc.
 * @param e The DAT's directory entry.
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

/** @brief Finds the playback rate of a VAG from the first tone that plays it.
 *
 * @param tones The tone records.
 * @param n Tone records in tones.
 * @param vag The 1-based VAG number.
 * @return The rate in Hz at note EGG_NOTE, rounded, or 0 when no tone with
 *         a centre note plays the VAG. */
static uint32_t vag_rate(const uint8_t* tones, uint32_t n, uint32_t vag) {
    for (uint32_t t = 0; t < n; t++) {
        const uint8_t* r = tones + t * VAB_TONE_BYTES;
        if (demo_egg_le16(r + 22) != vag || r[4] == 0) continue;
        double semis = (double)EGG_NOTE - (double)r[4] - (double)r[5] / 128.0;
        return (uint32_t)lround(44100.0 * pow(2.0, semis / 12.0));
    }
    return 0;
}

/** @brief Locates VAG EGG_VAG in the chunk's VAB through its offset table and the VAB header.
 *
 * @param chunk The chunk's bytes.
 * @param at Receives the VAG's byte offset in the chunk.
 * @param len Receives its byte length.
 * @param rate Receives its rate in Hz, 0 when no tone plays it.
 * @return 0, or -1 when the chunk is not the expected VAB chunk. */
static int walk_vab(const uint8_t* chunk, uint32_t* at, uint32_t* len, uint32_t* rate) {
    uint32_t tab = demo_egg_le32(chunk);
    if (tab != 12u || demo_egg_le32(chunk + 8) != EGG_CHUNK_BYTES) return -1;
    uint32_t vb = demo_egg_le32(chunk);
    uint32_t vh = demo_egg_le32(chunk + 4);
    if (vh + VAB_FIXED > EGG_CHUNK_BYTES || memcmp(chunk + vh, "pBAV", 4) != 0) return -1;
    uint32_t nprog = demo_egg_le16(chunk + vh + 18);
    uint32_t nvag = demo_egg_le16(chunk + vh + 22);
    uint32_t tones = vh + VAB_FIXED;
    uint32_t sizes = tones + nprog * VAB_TONES_PER_PROG * VAB_TONE_BYTES;
    if (nvag < EGG_VAG || nvag >= VAB_VAG_SLOTS ||
        sizes + 2u * (nvag + 1u) > EGG_CHUNK_BYTES) return -1;
    uint32_t off = vb;
    for (uint32_t k = 1; k < EGG_VAG; k++) off += demo_egg_le16(chunk + sizes + 2u * k) * 8u;
    *at = off;
    *len = demo_egg_le16(chunk + sizes + 2u * EGG_VAG) * 8u;
    *rate = vag_rate(chunk + tones, nprog * VAB_TONES_PER_PROG, EGG_VAG);
    return off + *len <= vh ? 0 : -1;
}

/** @brief Locates the woah in CRASHBSH.DAT: the VAB chunk, then its VAG through the VAB header.
 *
 * @param img The disc.
 * @param out Receives the ADPCM body and its rate.
 * @return 0 when found and equal to the expected offset and length, else -1
 *         with the reason logged. */
static int bash_locate(disc_image_t* img, demo_egg_sample_t* out) {
    disc_image_entry_t e;
    if (disc_image_find(img, EGG_FILE, &e) != 0 || e.is_dir) {
        OPL_INFO_F("game", "bash-demo-egg: %s not on this disc", EGG_FILE);
        return -1;
    }
    uint32_t base = EGG_CHUNK_SECTOR * DAT_SECTOR;
    uint8_t* chunk = read_range(img, &e, base, EGG_CHUNK_BYTES);
    uint32_t at = 0, len = 0, rate = 0;
    int found = chunk && walk_vab(chunk, &at, &len, &rate) == 0;
    if (!found) {
        free(chunk);
        OPL_WARN_F("game", "bash-demo-egg: %s: no VAB chunk at sector 0x%X", EGG_FILE,
                   EGG_CHUNK_SECTOR);
        return -1;
    }
    if (base + at != EGG_EXPECT_OFF || len != EGG_EXPECT_LEN || !rate) {
        free(chunk);
        OPL_WARN_F("game", "bash-demo-egg: %s VAB gives +0x%X, %u bytes, %u Hz; "
                   "expected +0x%X, %u bytes", EGG_FILE, base + at, len, rate,
                   EGG_EXPECT_OFF, EGG_EXPECT_LEN);
        return -1;
    }
    out->adpcm = (uint8_t*)malloc(len);
    if (out->adpcm) memcpy(out->adpcm, chunk + at, len);
    free(chunk);
    if (!out->adpcm) return -1;
    out->len = len;
    out->rate_hz = rate;
    return 0;
}

static demo_egg_t s_egg = {
    .name = "bash-demo-egg",
    .overlay = OVERLAY_TITLE,
    .sites = k_sites,
    .n_sites = sizeof k_sites / sizeof k_sites[0],
    .locate = bash_locate,
};

/** @brief Skips a launch-branch call and plays the sample on a new press; see demo_egg_on_call.
 *
 * @param cpu The guest CPU state.
 * @param addr The entered function's vaddr.
 * @return 1 to skip the guest function, else 0. */
static int on_call(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim);
    return demo_egg_on_call(&s_egg, cpu, addr);
}

/** @brief Decodes the sample from the --disc image at boot.
 *
 * @param opts The options, or NULL. */
static void bash_demo_egg_apply_options(const openpete_options_t* opts) {
    OP_ROLE_ASSERT(sim);
    if (opts) demo_egg_load(&s_egg, opts->disc_path);
}

static const feature_dispatch_entry_t k_dispatch[] = {
    { 0x8003BEDCu, on_call, 0u },
    { 0x8003C184u, on_call, 0u },
    { 0x80020168u, on_call, 0u },
    { 0x80078B88u, on_call, OVERLAY_TITLE },
    { 0, NULL, 0u },
};

const feature_module_t bash_demo_egg_module = {
    .name           = "bash-demo-egg",
    .apply_options  = bash_demo_egg_apply_options,
    .dispatch_table = k_dispatch,
};
