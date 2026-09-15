// games/spyro-1/recomp_extract.cpp — carve the recomp pipeline's inputs
// out of the user's disc (distribution D2.3; plan
// openpete-distribution-local-recomp).
//
// Extracts PSX.EXE (iso9660 file lookup) and every code overlay
// (WAD.WAD carve) to a directory — the first stage of the first-boot
// pipeline, exposed standalone via --extract-recomp-inputs DIR for the
// carve-equivalence gate (scripts/check_carve_equivalence.sh proves the
// carved bytes == the decomp's build outputs that regen consumes).
//
// WAD layout (docs/level-format.md §1, spyro-1/include/wad.h,
// WadInitialize + CDLoadSync in the decomp): the header is the first
// 2048 bytes of WAD.WAD at disc LBA 37 (g_CdState.m_WadSector); every
// entry is {int byte_offset, int byte_length} into the WAD; reads
// resolve to LBA 37 + offset/2048 (offsets are sector-aligned).
#include "openpete_log.h"
#include "iso_reader.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

extern "C" uint8_t* openpete_psx_load_exe_from_disc(const char* cue_path,
                                                    const char* exe_filename,
                                                    size_t* out_size);
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

OffsetLength read_entry(const uint8_t* hdr, size_t off) {
    OffsetLength e;
    std::memcpy(&e, hdr + off, sizeof e);   // little-endian host assumed
    return e;
}

bool open_disc(PS1::ISOReader& reader, const char* cue_path) {
    if (reader.Open(cue_path)) return true;
    /* Same fallback as exe_loader.cpp: the CUE's FILE line sometimes
     * mismatches the on-disc bin name — try sibling .bin files. */
    std::filesystem::path dir = std::filesystem::path(cue_path).parent_path();
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
        auto p = e.path();
        if (p.extension() == ".bin" || p.extension() == ".BIN") {
            if (reader.Open(p.string())) return true;
        }
    }
    return false;
}

bool carve(PS1::ISOReader& reader, const OffsetLength& e,
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
        if (!reader.ReadSector(first_lba + i, buf.data() + i * kSectorSize)) {
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

extern "C" int spyro1_extract_recomp_inputs(const char* cue_path,
                                            const char* out_dir) {
    if (!cue_path || !out_dir) {
        std::fprintf(stderr, "extract-recomp-inputs: needs --disc and a directory\n");
        return -1;
    }
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);

    /* PSX.EXE via the iso9660 path the runtime already uses. */
    size_t exe_size = 0;
    uint8_t* exe = openpete_psx_load_exe_from_disc(
        cue_path, game_get_exe_filename(), &exe_size);
    if (!exe) return -1;
    {
        std::ofstream f(std::filesystem::path(out_dir) / "PSX.EXE",
                        std::ios::binary);
        f.write(reinterpret_cast<const char*>(exe), (std::streamsize)exe_size);
        std::free(exe);
        if (!f.good()) return -1;
    }

    PS1::ISOReader reader;
    if (!open_disc(reader, cue_path)) {
        OPL_ERROR_F("extract", "cannot open disc: %s", cue_path);
        return -1;
    }

    uint8_t hdr[kSectorSize];
    if (!reader.ReadSector(kWadSector, hdr)) {
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
