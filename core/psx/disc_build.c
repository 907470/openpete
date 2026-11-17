/* core/psx/disc_build.c - which build of a game a disc image holds
 * (contract in openpete_disc_build.h). */

#include "openpete_disc_build.h"

#include "openpete_disc_image.h"
#include "openpete_disc_trace.h"
#include "openpete_psx.h"
#include "sha256.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One known build: a release serial, and the SHA-256 of the boot executable
 * its SYSTEM.CNF names (NULL: not on file, so the row matches by serial
 * alone, which only a serial no supported row shares may do). */
typedef struct {
    const char*            serial;
    const char*            exe_sha256;
    int                    supported;
    openpete_disc_region_t region;
    const char*            game;      /* the registered game this is a release of */
    const char*            title;
    const char*            revision;
    const char*            bin_sha1;  /* a supported row's Redump .bin SHA-1, which a player can check, or NULL */
} disc_build_row_t;

/* Sources, per row:
 *  SCUS-94228 v1.0: the decomp's build/PSX.EXE pin (games/spyro-1/spyro-1/
 *    sha256sum.txt, readme "PSX.EXE SHA-256"), equal to SCUS_942.28 read off
 *    the NTSC-U image. Original, Greatest Hits and Collectors' Edition
 *    pressings share the image.
 *  SCUS-94425 v1.0: games/spyro-2/spyro-2/sha256sum.txt, equal to
 *    SCUS_944.25 read off the image whose .bin SHA-256 is b3fd9fdb...
 *  SCUS-94467 v1.0: SCUS_944.67 read off the image whose .bin SHA-256 is
 *    a313c9b1... (Redump "Spyro - Year of the Dragon (USA) (v1.0)"). The
 *    decomp's build/PSX.EXE (e5406997..., its sha256sum.txt pin and the
 *    recompiler manifest's input) differs from it in the word at 0x8004F6B0
 *    (exe_check, asm/stdutil.s); both are supported, the decomp build's row
 *    first so the disc's row, which carries the .bin SHA-1, names the
 *    release in the messages.
 *  SCUS-94467 v1.1: SCUS_944.67 (380928 bytes) read off an image whose
 *    .bin SHA-1 is 81eae747fbd9b2ff75ae5b3e24287bb25fcb516a, Redump's
 *    "Spyro - Year of the Dragon (USA) (v1.1)". Same serial and boot file
 *    name as v1.0, so only the executable's hash tells the two apart.
 *  bin_sha1 of the supported rows: the .bin SHA-1 the User docs give for
 *    each game's disc (docs/src/user/getting-started/dumping-your-disc.md,
 *    games/spyro-2.md, games/spyro-3.md), Redump's listed value.
 *  PAL and NTSC-J serials: PSX Datacenter's entries for each release
 *    (SCES-01438, SCES-02104, SCES-02835; SCPS-10085, SCPS-10083 disc 1 of
 *    the limited edition, SCPS-45395; SCPS-10128). Spyro: Year of the Dragon
 *    had no NTSC-J release. */
static const disc_build_row_t k_rows[] = {
    { "SCUS-94467", "e5406997dccc7300c8198498c20b9d6c4c0a547813be1010446b6c4e5d50e39f", 1,
      OPENPETE_DISC_REGION_NTSC_U, "spyro-3", "Spyro: Year of the Dragon", "1.0", NULL },
    { "SCUS-94228", "a533d75cab8afaae6107ec35a02a9a5fe979a92c7c955f9cf1ee50f693a1b998", 1,
      OPENPETE_DISC_REGION_NTSC_U, "spyro-1", "Spyro the Dragon", "1.0",
      "cf3ce6bedeb89dfbc40990336180f3b9b0f40d9f" },
    { "SCUS-94425", "7b54002789ab379e0d3a6b49d7b50078b9c9c2fed589f41ff9878ebb68fe0bc1", 1,
      OPENPETE_DISC_REGION_NTSC_U, "spyro-2", "Spyro 2: Ripto's Rage!", "1.0",
      "b3a28f8c5ec02dfbd1f9bd47adbe1d75186a5d3f" },
    { "SCUS-94467", "cb819ee78c556d403779309859cb08a7111331f624759bc1bc380946261bb26e", 1,
      OPENPETE_DISC_REGION_NTSC_U, "spyro-3", "Spyro: Year of the Dragon", "1.0",
      "9df489e2420988494691c9193fe74dec4b1dbfcd" },
    { "SCUS-94467", "006363e485559dcd9a203fe71fe4267cb254d476ca59f04ee841048831ad969e", 0,
      OPENPETE_DISC_REGION_NTSC_U, "spyro-3", "Spyro: Year of the Dragon", "1.1", NULL },
    { "SCES-01438", NULL, 0, OPENPETE_DISC_REGION_PAL, "spyro-1", "Spyro the Dragon", NULL, NULL },
    { "SCPS-10085", NULL, 0, OPENPETE_DISC_REGION_NTSC_J, "spyro-1", "Spyro the Dragon", NULL, NULL },
    { "SCPS-10083", NULL, 0, OPENPETE_DISC_REGION_NTSC_J, "spyro-1", "Spyro the Dragon", "Limited Edition", NULL },
    { "SCPS-45395", NULL, 0, OPENPETE_DISC_REGION_NTSC_J, "spyro-1", "Spyro the Dragon", NULL, NULL },
    { "SCES-02104", NULL, 0, OPENPETE_DISC_REGION_PAL, "spyro-2", "Spyro 2: Gateway to Glimmer", NULL, NULL },
    { "SCPS-10128", NULL, 0, OPENPETE_DISC_REGION_NTSC_J, "spyro-2", "Spyro x Sparx: Tondemo Tours", NULL, NULL },
    { "SCES-02835", NULL, 0, OPENPETE_DISC_REGION_PAL, "spyro-3", "Spyro: Year of the Dragon", NULL, NULL },
};
#define ROW_COUNT (sizeof k_rows / sizeof k_rows[0])

const char* openpete_disc_build_class_name(openpete_disc_build_class_t c) {
    switch (c) {
    case OPENPETE_DISC_BUILD_SUPPORTED:  return "supported";
    case OPENPETE_DISC_BUILD_RECOGNISED: return "recognised";
    case OPENPETE_DISC_BUILD_UNKNOWN:    return "unknown";
    default:                             return "unreadable";
    }
}

const char* openpete_disc_region_name(openpete_disc_region_t r) {
    switch (r) {
    case OPENPETE_DISC_REGION_NTSC_U: return "NTSC-U";
    case OPENPETE_DISC_REGION_PAL:    return "PAL";
    case OPENPETE_DISC_REGION_NTSC_J: return "NTSC-J";
    default:                          return "unknown";
    }
}

/** @brief The region a serial's territory letter names: its third character.
 *
 * Only a serial of the Sony product-code form (four letters, '-', five
 * digits) names a region; any other boot name ("PSX-EXE") names none.
 * @param serial A serial such as "SCES-02835".
 * @return The region; UNKNOWN for another form or another letter. */
static openpete_disc_region_t region_of_serial(const char* serial) {
    if (strlen(serial) != 10 || serial[4] != '-') return OPENPETE_DISC_REGION_UNKNOWN;
    for (int i = 0; i < 10; i++)
        if (i < 4 ? !isalpha((unsigned char)serial[i]) : i > 4 && !isdigit((unsigned char)serial[i]))
            return OPENPETE_DISC_REGION_UNKNOWN;
    switch (toupper((unsigned char)serial[2])) {
    case 'U': return OPENPETE_DISC_REGION_NTSC_U;
    case 'E': return OPENPETE_DISC_REGION_PAL;
    case 'P': case 'A': case 'K': return OPENPETE_DISC_REGION_NTSC_J;
    default:  return OPENPETE_DISC_REGION_UNKNOWN;
    }
}

/** @brief The supported row of a serial.
 *
 * @param serial The serial.
 * @return The row, or NULL when no supported row has the serial. */
static const disc_build_row_t* supported_row(const char* serial) {
    for (size_t i = 0; i < ROW_COUNT; i++)
        if (k_rows[i].supported && !strcmp(k_rows[i].serial, serial)) return &k_rows[i];
    return NULL;
}

/** @brief The row a serial and hash match: the hash, under any boot name, else the serial on a hashless row of a serial no supported row has.
 *
 * The hash decides first, so a supported executable booted under another
 * name (a rebuilt image whose BOOT line names PSX.EXE) is that row's; a
 * hash whose row is another game than the serial's supported game matches
 * nothing, since the serial selects the game that runs.
 * @param serial The serial.
 * @param sha The executable's hash.
 * @return The row, or NULL. */
static const disc_build_row_t* match_row(const char* serial, const char* sha) {
    const disc_build_row_t* own = supported_row(serial);
    for (size_t i = 0; i < ROW_COUNT; i++)
        if (k_rows[i].exe_sha256 && !strcmp(k_rows[i].exe_sha256, sha))
            return own && strcmp(own->game, k_rows[i].game) ? NULL : &k_rows[i];
    if (supported_row(serial)) return NULL;
    for (size_t i = 0; i < ROW_COUNT; i++)
        if (!k_rows[i].exe_sha256 && !strcmp(k_rows[i].serial, serial)) return &k_rows[i];
    return NULL;
}

/** @brief The warning of a region: PAL, NTSC-J, else OTHER.
 *
 * @param r The region.
 * @return The warning. */
static openpete_disc_warning_t region_warning(openpete_disc_region_t r) {
    if (r == OPENPETE_DISC_REGION_PAL) return OPENPETE_DISC_WARN_PAL;
    if (r == OPENPETE_DISC_REGION_NTSC_J) return OPENPETE_DISC_WARN_NTSC_J;
    return OPENPETE_DISC_WARN_OTHER;
}

void openpete_disc_build_classify(const char* cnf_text, const uint8_t* exe, size_t exe_len,
                                  openpete_disc_build_t* out) {
    memset(out, 0, sizeof *out);
    out->cls = OPENPETE_DISC_BUILD_UNREADABLE;
    out->warning = OPENPETE_DISC_WARN_UNREADABLE;
    if (!cnf_text ||
        openpete_psx_parse_boot_line(cnf_text, out->boot_path, sizeof out->boot_path,
                                     out->serial, sizeof out->serial) != 0)
        return;
    out->region = region_of_serial(out->serial);
    if (!exe) return;

    openpete_sha256_ctx c;
    uint8_t d[32];
    openpete_sha256_init(&c);
    openpete_sha256_update(&c, exe, exe_len);
    openpete_sha256_final(&c, d);
    openpete_sha256_hex(d, out->exe_sha256);

    const disc_build_row_t* row = match_row(out->serial, out->exe_sha256);
    if (row) {
        out->cls = row->supported ? OPENPETE_DISC_BUILD_SUPPORTED : OPENPETE_DISC_BUILD_RECOGNISED;
        out->region = row->region;
        out->game = row->game;
        out->title = row->title;
        out->revision = row->revision;
        if (row->supported) out->warning = OPENPETE_DISC_WARN_NONE;
        else if (supported_row(row->serial)) out->warning = OPENPETE_DISC_WARN_REVISION;
        else out->warning = region_warning(row->region);
        return;
    }
    out->cls = OPENPETE_DISC_BUILD_UNKNOWN;
    const disc_build_row_t* sup = supported_row(out->serial);
    if (sup) {
        out->game = sup->game;
        out->title = sup->title;
        out->warning = OPENPETE_DISC_WARN_MODIFIED;
        return;
    }
    out->warning = region_warning(out->region);
}

/** @brief Read SYSTEM.CNF and the boot executable off an open image and classify them.
 *
 * @param img The image.
 * @param out Receives the classification. */
static void classify_image(disc_image_t* img, openpete_disc_build_t* out) {
    char cnf[4097] = {0};
    const char* cnf_text = NULL;
    disc_image_entry_t e;
    if (disc_image_find(img, "SYSTEM.CNF", &e) == 0 && !e.is_dir && e.size > 0 && e.size < sizeof cnf &&
        disc_image_read_file(img, &e, cnf, e.size) == e.size)
        cnf_text = cnf;
    char path[64];
    uint8_t* exe = NULL;
    size_t exe_len = 0;
    if (cnf_text && openpete_psx_parse_boot_line(cnf_text, path, sizeof path, NULL, 0) == 0 &&
        disc_image_find(img, path, &e) == 0 && !e.is_dir)
        exe = openpete_psx_read_exe_from_image(img, path, &exe_len);
    openpete_disc_build_classify(cnf_text, exe, exe_len, out);
    free(exe);
}

int openpete_disc_build_identify(const char* disc_path, openpete_disc_build_t* out) {
    openpete_disc_build_classify(NULL, NULL, 0, out);
    if (!disc_path) return -1;
    disc_image_t* img = disc_image_open(disc_path, DISC_READ_CACHED, OPENPETE_DISC_TAG_SERIAL);
    if (!img) return -1;
    classify_image(img, out);
    disc_image_close(img);
    return 0;
}

/* The supported builds, as the messages name them. */
static const char k_supported_list[] =
    "the NTSC-U releases Spyro the Dragon (SCUS-94228), Spyro 2: Ripto's Rage! (SCUS-94425) "
    "and Spyro: Year of the Dragon version 1.0 (SCUS-94467)";

/** @brief Write the sentence naming the build OpenPete runs in place of this disc.
 *
 * @param b The classification.
 * @param out Receives the sentence.
 * @param cap Its capacity. */
static void runs_instead(const openpete_disc_build_t* b, char* out, size_t cap) {
    const disc_build_row_t* sup = NULL;
    for (size_t i = 0; b->game && i < ROW_COUNT; i++)
        if (k_rows[i].supported && !strcmp(k_rows[i].game, b->game)) sup = &k_rows[i];
    if (sup && sup->bin_sha1)
        snprintf(out, cap, "OpenPete supports only version %s of the NTSC-U release of %s (%s), "
                 "whose .bin file has the SHA-1 %s.", sup->revision, sup->title, sup->serial,
                 sup->bin_sha1);
    else if (sup)
        snprintf(out, cap, "OpenPete supports only version %s of the NTSC-U release of %s (%s).",
                 sup->revision, sup->title, sup->serial);
    else
        snprintf(out, cap, "OpenPete supports only %s.", k_supported_list);
}

void openpete_disc_build_message(const openpete_disc_build_t* b, int refused, char* out, size_t cap) {
    if (!cap) return;
    out[0] = '\0';
    char what[512], instead[256];
    const char* serial = b->serial[0] ? b->serial : "no serial";
    switch (b->warning) {
    case OPENPETE_DISC_WARN_NONE:
        return;
    case OPENPETE_DISC_WARN_PAL:
    case OPENPETE_DISC_WARN_NTSC_J: {
        const char* region = b->warning == OPENPETE_DISC_WARN_PAL ? "PAL (European)" : "NTSC-J (Japanese)";
        if (b->title)
            snprintf(what, sizeof what, "This disc is the %s release of %s (%s).", region, b->title, serial);
        else
            snprintf(what, sizeof what, "This disc is a %s release (%s), not a build OpenPete runs.",
                     region, serial);
        break;
    }
    case OPENPETE_DISC_WARN_REVISION:
        snprintf(what, sizeof what, "This disc is %s revision %s (%s). Revision %s has a different game "
                 "program from the original 1.0 pressing.", b->title, b->revision, serial, b->revision);
        break;
    case OPENPETE_DISC_WARN_MODIFIED:
        snprintf(what, sizeof what, "This disc has the serial of %s (%s), but its game program %s is not "
                 "the one OpenPete runs: it is another version of the release, or a modified image.",
                 b->title, serial, b->boot_path);
        break;
    case OPENPETE_DISC_WARN_UNREADABLE:
        snprintf(what, sizeof what, "This disc image has no usable SYSTEM.CNF boot line or boot program, "
                 "so its build cannot be identified.");
        break;
    default:
        snprintf(what, sizeof what, "This disc (%s) is not a build OpenPete runs.", serial);
        break;
    }
    runs_instead(b, instead, sizeof instead);
    snprintf(out, cap, "%s %s %s", what, instead,
             refused ? "OpenPete cannot start it."
                     : "The game is expected to crash or misbehave with this disc.");
}

void openpete_disc_build_toast(const openpete_disc_build_t* b, char* out, size_t cap) {
    if (!cap) return;
    out[0] = '\0';
    if (b->warning == OPENPETE_DISC_WARN_NONE) return;
    const char* serial = b->serial[0] ? b->serial : "UNKNOWN";
    switch (b->warning) {
    case OPENPETE_DISC_WARN_REVISION:
        snprintf(out, cap, "UNSUPPORTED DISC: %s REVISION %s. ONLY THE ORIGINAL 1.0 PRESSING IS "
                 "SUPPORTED. EXPECT CRASHES.", serial, b->revision);
        break;
    case OPENPETE_DISC_WARN_MODIFIED:
        snprintf(out, cap, "UNSUPPORTED DISC: OTHER VERSION OR MODIFIED IMAGE OF %s. "
                 "EXPECT CRASHES.", serial);
        break;
    case OPENPETE_DISC_WARN_UNREADABLE:
        snprintf(out, cap, "UNSUPPORTED DISC: BUILD NOT IDENTIFIED. EXPECT CRASHES.");
        break;
    default:
        snprintf(out, cap, "UNSUPPORTED DISC: %s. EXPECT CRASHES.", serial);
        break;
    }
    for (char* p = out; *p; p++) *p = (char)toupper((unsigned char)*p);
}
