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

/* The rows every linked game registered (openpete_disc_build_add_rows),
 * in registration order. */
#define ROW_CAP 64
static const openpete_disc_build_row_t* s_rows[ROW_CAP] OP_SEALED(disc_builds); /* thread-roles: written before openpete_run seals it, read-only after */
static int s_row_count OP_SEALED(disc_builds); /* thread-roles: written before openpete_run seals it, read-only after */
OP_SEAL_DEFINE(disc_builds);

void openpete_disc_build_add_rows(const openpete_disc_build_row_t* rows) {
    if (!rows) return;
    op_seal_check(disc_builds);
    for (int i = 0; i < s_row_count; i++)
        if (s_rows[i] == rows) return;
    for (const openpete_disc_build_row_t* r = rows; r->serial && s_row_count < ROW_CAP; r++)
        s_rows[s_row_count++] = r;
}

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
static const openpete_disc_build_row_t* supported_row(const char* serial) {
    for (int i = 0; i < s_row_count; i++)
        if (s_rows[i]->supported && !strcmp(s_rows[i]->serial, serial)) return s_rows[i];
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
static const openpete_disc_build_row_t* match_row(const char* serial, const char* sha) {
    const openpete_disc_build_row_t* own = supported_row(serial);
    for (int i = 0; i < s_row_count; i++)
        if (s_rows[i]->exe_sha256 && !strcmp(s_rows[i]->exe_sha256, sha))
            return own && strcmp(own->game, s_rows[i]->game) ? NULL : s_rows[i];
    if (supported_row(serial)) return NULL;
    for (int i = 0; i < s_row_count; i++)
        if (!s_rows[i]->exe_sha256 && !strcmp(s_rows[i]->serial, serial)) return s_rows[i];
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
    if (exe) {
        openpete_sha256_ctx c;
        uint8_t d[32];
        openpete_sha256_init(&c);
        openpete_sha256_update(&c, exe, exe_len);
        openpete_sha256_final(&c, d);
        openpete_sha256_hex(d, out->exe_sha256);
    }

    /* A serial-only row needs no executable; anything else does. */
    const openpete_disc_build_row_t* row = match_row(out->serial, out->exe_sha256);
    if (!exe && (!row || row->exe_sha256)) return;
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
    const openpete_disc_build_row_t* sup = supported_row(out->serial);
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

/** @brief The supported row of a game that names its release: the one carrying a .bin SHA-1, else the first.
 *
 * @param game A game id, or NULL.
 * @return The row, or NULL when the game has no supported row. */
static const openpete_disc_build_row_t* release_row(const char* game) {
    const openpete_disc_build_row_t* first = NULL;
    for (int i = 0; game && i < s_row_count; i++) {
        if (!s_rows[i]->supported || strcmp(s_rows[i]->game, game)) continue;
        if (s_rows[i]->bin_sha1) return s_rows[i];
        if (!first) first = s_rows[i];
    }
    return first;
}

/** @brief Write the list of supported releases ("Title (SERIAL) version V, ..."), one per game.
 *
 * @param out Receives the list.
 * @param cap Its capacity. */
static void supported_list(char* out, size_t cap) {
    size_t o = 0;
    out[0] = '\0';
    for (int i = 0; i < s_row_count && o < cap; i++) {
        const openpete_disc_build_row_t* r = s_rows[i];
        if (!r->supported || release_row(r->game) != r) continue;
        const int n = snprintf(out + o, cap - o, "%s%s (%s) version %s", o ? ", " : "", r->title,
                               r->serial, r->revision);
        if (n < 0) break;
        o += (size_t)n;
    }
}

/** @brief Write the sentence naming the build OpenPete runs in place of this disc.
 *
 * @param b The classification.
 * @param out Receives the sentence.
 * @param cap Its capacity. */
static void runs_instead(const openpete_disc_build_t* b, char* out, size_t cap) {
    const openpete_disc_build_row_t* sup = release_row(b->game);
    if (sup && sup->bin_sha1)
        snprintf(out, cap, "OpenPete supports only version %s of the %s release of %s (%s), "
                 "whose .bin file has the SHA-1 %s.", sup->revision, openpete_disc_region_name(sup->region),
                 sup->title, sup->serial, sup->bin_sha1);
    else if (sup)
        snprintf(out, cap, "OpenPete supports only version %s of the %s release of %s (%s).",
                 sup->revision, openpete_disc_region_name(sup->region), sup->title, sup->serial);
    else {
        char list[320];
        supported_list(list, sizeof list);
        snprintf(out, cap, "OpenPete supports only %s.", list[0] ? list : "the builds its games name");
    }
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
                 "program from the original %s pressing.", b->title, b->revision, serial, b->revision,
                 release_row(b->game) ? release_row(b->game)->revision : "supported");
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
