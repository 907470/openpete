// games/common/recomp_extract_wad_entry.cpp - carve the first-boot pipeline's
// inputs out of the user's disc by WAD entry index.
//
// For a game whose converged manifest names, per overlay, the WAD.WAD entry
// that holds it (`wad_entry = N`, written by scripts/emit-recomp-manifest.sh
// under FIRST_BOOT_CARVE=wad_entry). WAD.WAD's entry table is its first two
// sectors of 8-byte {byte offset, byte length} pairs; offsets are
// sector-aligned. The carve reads the entry the manifest names whatever its
// content, so a modified binary is carved and recompiled like the retail one
// (recomp_build.cpp warns when its hash differs).
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
#include <utility>
#include <vector>

extern "C" const char* game_get_exe_filename(void);

namespace {

constexpr uint32_t kSector = DISC_IMAGE_USER_SECTOR;

/** @brief Reads the (name, wad_entry) pair of every [[overlay]] block of a converged manifest.
 *
 * @param path The manifest.toml.
 * @param out Receives the pairs, in file order.
 * @return false when the file cannot be read, holds no overlay, or an
 *         overlay names no wad_entry. */
bool read_entries(const char* path, std::vector<std::pair<std::string, int>>& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line, name;
    int entry = -1;
    bool in_overlay = false;
    auto flush = [&]() {
        if (in_overlay) out.emplace_back(name, entry);
        name.clear();
        entry = -1;
    };
    while (std::getline(f, line)) {
        if (line == "[[overlay]]") { flush(); in_overlay = true; continue; }
        if (!in_overlay) continue;
        if (line.rfind("name = \"", 0) == 0) name = line.substr(8, line.size() - 9);
        else if (line.rfind("wad_entry = ", 0) == 0) entry = std::atoi(line.c_str() + 12);
    }
    flush();
    for (const auto& e : out)
        if (e.first.empty() || e.second < 0) return false;
    return !out.empty();
}

/** @brief Reads a little-endian word from a byte buffer.
 *
 * @param p The buffer.
 * @param o The byte offset.
 * @return The word. */
uint32_t le32(const uint8_t* p, size_t o) {
    return (uint32_t)p[o] | ((uint32_t)p[o + 1] << 8) | ((uint32_t)p[o + 2] << 16) |
           ((uint32_t)p[o + 3] << 24);
}

}  // namespace

/* Writes PSX.EXE (the game's executable, by its ISO 9660 name) and
 * <name>.ovl for every manifest overlay, each the exact bytes of its WAD
 * entry. A failed carve is logged and the remaining entries still run. */
extern "C" int openpete_extract_wad_entry_inputs(const char* cue_path,
                                                 const char* out_dir,
                                                 const char* manifest_path) {
    if (!cue_path || !out_dir || !manifest_path) {
        std::fprintf(stderr, "extract-recomp-inputs: needs a disc, a directory and a manifest\n");
        return -1;
    }
    std::vector<std::pair<std::string, int>> entries;
    if (!read_entries(manifest_path, entries)) {
        OPL_ERROR_F("extract", "%s names no wad_entry for some overlay", manifest_path);
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

    disc_image_entry_t wad;
    uint8_t table[2 * kSector];
    if (disc_image_find(reader, "WAD.WAD", &wad) != 0 ||
        disc_image_read_user(reader, wad.lba, table) != 0 ||
        disc_image_read_user(reader, wad.lba + 1, table + kSector) != 0) {
        OPL_ERROR_F("extract", "cannot read the WAD.WAD entry table");
        return -1;
    }

    int carved = 0;
    std::vector<uint8_t> buf;
    for (const auto& [name, e] : entries) {
        const size_t at = 8u * (size_t)e;
        const uint32_t off = at + 8 <= sizeof table ? le32(table, at) : 0;
        const uint32_t len = at + 8 <= sizeof table ? le32(table, at + 4) : 0;
        if (len == 0 || off % kSector || off > wad.size || len > wad.size - off) {
            OPL_ERROR_F("extract", "%s: WAD entry %d is not a valid extent", name.c_str(), e);
            continue;
        }
        const uint32_t n = (len + kSector - 1) / kSector;
        buf.assign((size_t)n * kSector, 0);
        bool ok = true;
        for (uint32_t i = 0; ok && i < n; i++)
            ok = disc_image_read_user(reader, wad.lba + off / kSector + i,
                                      buf.data() + (size_t)i * kSector) == 0;
        std::ofstream f(std::filesystem::path(out_dir) / (name + ".ovl"), std::ios::binary);
        if (ok) f.write(reinterpret_cast<const char*>(buf.data()), len);
        if (ok && f.good()) carved++;
        else OPL_ERROR_F("extract", "carve failed: %s (WAD entry %d)", name.c_str(), e);
    }
    OPL_INFO_F("extract", "recomp inputs: PSX.EXE + %d/%zu overlays -> %s",
               carved, entries.size(), out_dir);
    return carved == (int)entries.size() ? 0 : -1;
}
