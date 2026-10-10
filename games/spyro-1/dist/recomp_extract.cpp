// games/spyro-1/dist/recomp_extract.cpp - carve the recomp pipeline's inputs
// out of the user's disc.
//
// Extracts PSX.EXE (iso9660 file lookup) and every code overlay
// (WAD.WAD carve) to a directory - the first stage of the first-boot
// pipeline, exposed standalone via --extract-recomp-inputs DIR for the
// carve-equivalence gate (scripts/check_carve_equivalence.sh proves the
// carved bytes == the decomp's build outputs that regen consumes).
//
// WAD layout (docs/src/spyro/level-format.md §1, spyro-1/include/wad.h,
// WadInitialize + CDLoadSync in the decomp): the header is the first
// 2048 bytes of WAD.WAD at disc LBA 37 (g_CdState.m_WadSector); every
// entry is {int byte_offset, int byte_length} into the WAD; reads
// resolve to LBA 37 + offset/2048 (offsets are sector-aligned).
#include "recomp_attach.h"
#include "openpete_log.h"
#include "openpete_disc_image.h"
#include "openpete_disc_trace.h"
#include "openpete_psx.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

extern "C" const char* game_get_exe_filename(void);

namespace {

constexpr uint32_t kWadSector = 37;   // WadInitialize: g_CdState.m_WadSector
constexpr uint32_t kSectorSize = 2048;

struct OffsetLength {
    int32_t offset;
    int32_t length;
};

/* g_WadHeader byte offsets (wad.h field order):
 *   0  m_UniversalLogo      16 m_TitleScreenOverlay
 *   24 m_CutsceneData[4]    56 m_GameOverSkybox   64 m_PETE
 *   72 m_LevelEntry[36]{m_Overlay,m_Data}         648 m_CreditsOverlay */
constexpr size_t kTitlescreenOff = 16;
constexpr size_t kLevelEntryOff  = 72;
constexpr size_t kCreditsOff     = 648;
constexpr int    kLevelSlots     = 36;   // last is empty

/** @brief Reads the {offset, length} pair at a byte offset of the WAD header.
 *
 * The pair is copied in host byte order; a little-endian host is assumed.
 * @param hdr The WAD header sector.
 * @param off The byte offset of the pair; the caller keeps off + 8 inside
 *        hdr.
 * @return The pair. */
OffsetLength read_entry(const uint8_t* hdr, size_t off) {
    OffsetLength e;
    std::memcpy(&e, hdr + off, sizeof e);   // little-endian host assumed
    return e;
}

/** @brief Writes one WAD entry's bytes to a file.
 *
 * Reads ceil(length / 2048) sectors from disc LBA 37 + offset / 2048 and
 * writes exactly length bytes. A length of 0 or less fails silently; a
 * negative or non-sector-aligned offset and a failed sector read are logged.
 * @param reader The opened disc.
 * @param e The entry: byte offset into WAD.WAD and byte length.
 * @param out The output file, created or truncated.
 * @return true when every sector read and the file write succeed, else
 *         false. */
bool carve(disc_image_t* reader, const OffsetLength& e,
           const std::filesystem::path& out) {
    if (e.length <= 0) return false;
    if (e.offset < 0 || (e.offset % kSectorSize) != 0) {
        OPL_ERROR_F("extract", "%s: unaligned WAD offset 0x%X",
                    out.filename().string().c_str(), (unsigned)e.offset);
        return false;
    }
    const uint32_t first_lba = kWadSector + (uint32_t)e.offset / kSectorSize;
    const uint32_t nsectors  = ((uint32_t)e.length + kSectorSize - 1) / kSectorSize;
    std::vector<uint8_t> buf(nsectors * kSectorSize);
    for (uint32_t i = 0; i < nsectors; i++) {
        if (disc_image_read_user(reader, first_lba + i, buf.data() + i * kSectorSize) != 0) {
            OPL_ERROR_F("extract", "%s: sector read failed at LBA %u",
                        out.filename().string().c_str(), first_lba + i);
            return false;
        }
    }
    std::ofstream f(out, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(buf.data()), e.length);
    return f.good();
}

}  // namespace

/** @brief Extracts PSX.EXE and every WAD code overlay of a disc into a directory.
 *
 * Creates out_dir and writes PSX.EXE (read through
 * openpete_psx_read_exe_from_image under the game's executable name). Then
 * reads the WAD header at disc LBA 37 and carves each entry with a positive
 * length to <name>.ovl: level_10..15, level_20..25 through level_60..65 (the
 * overlay half of the 36 level entries, at header offset 72 + 16 * slot),
 * titlescreen (offset 16) and credits (offset 648). A failed carve is logged
 * and the remaining entries still run. A summary line reports the carved
 * count against the expected count.
 * @param cue_path The disc's .cue path.
 * @param out_dir The output directory.
 * @return 0 when PSX.EXE and every non-empty overlay were written; -1 for a
 *         NULL argument, an unreadable executable, disc or WAD header, or any
 *         failed carve. */
extern "C" int spyro1_extract_recomp_inputs(const char* cue_path,
                                            const char* out_dir) {
    if (!cue_path || !out_dir) {
        std::fprintf(stderr, "extract-recomp-inputs: needs --disc and a directory\n");
        return -1;
    }
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);

    disc_image_t* reader = disc_image_open(cue_path, DISC_READ_CACHED, OPENPETE_DISC_TAG_EXTRACT);
    if (!reader) {
        OPL_ERROR_F("extract", "cannot open disc: %s", cue_path);
        return -1;
    }
    std::unique_ptr<disc_image_t, decltype(&disc_image_close)> closer(reader, &disc_image_close);

    size_t exe_size = 0;
    uint8_t* exe = openpete_psx_read_exe_from_image(reader, game_get_exe_filename(), &exe_size);
    if (!exe) return -1;
    {
        std::ofstream f(std::filesystem::path(out_dir) / "PSX.EXE", std::ios::binary);
        f.write(reinterpret_cast<const char*>(exe), (std::streamsize)exe_size);
        std::free(exe);
        if (!f.good()) return -1;
    }

    uint8_t hdr[kSectorSize];
    if (disc_image_read_user(reader, kWadSector, hdr) != 0) {
        OPL_ERROR_F("extract", "cannot read WAD header at LBA %u", kWadSector);
        return -1;
    }

    int carved = 0, expected = 0;
    auto do_one = [&](const OffsetLength& e, const std::string& name) {
        if (e.length <= 0) return;   // empty slot
        expected++;
        if (carve(reader, e, std::filesystem::path(out_dir) / (name + ".ovl")))
            carved++;
        else
            OPL_ERROR_F("extract", "carve failed: %s", name.c_str());
    };

    for (int f = 0; f < kLevelSlots; f++) {
        OffsetLength ovl = read_entry(hdr, kLevelEntryOff + (size_t)f * 16);
        char name[32];
        std::snprintf(name, sizeof name, "level_%d",
                      (f / 6 + 1) * 10 + (f % 6));
        do_one(ovl, name);
    }
    do_one(read_entry(hdr, kTitlescreenOff), "titlescreen");
    do_one(read_entry(hdr, kCreditsOff), "credits");

    OPL_INFO_F("extract", "recomp inputs: PSX.EXE + %d/%d overlays -> %s",
               carved, expected, out_dir);
    return (carved == expected) ? 0 : -1;
}

/* Spyro 1 carves by the WAD header's own layout, so the manifest is not
 * read. */
extern "C" int openpete_game_extract_recomp_inputs(const char* cue_path,
                                                   const char* out_dir,
                                                   const char* manifest_path) {
    (void)manifest_path;
    return spyro1_extract_recomp_inputs(cue_path, out_dir);
}
