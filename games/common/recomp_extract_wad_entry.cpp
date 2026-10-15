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
//
// A binary whose disc copy went through post-link mastering transforms
// carries their inverse in the manifest, applied after the carve and before
// the file is written: `decrypt = true` XORs the words [0x1000, key -
// load_addr) with key, the le32 at byte 4 (the span the game's loader
// decrypts in place), and `restore = [[offset, word], ...]` sets each word at
// a file offset back to the linked binary's value. The executable's block
// takes `restore` alone. A manifest with neither carves the raw bytes.
#include "recomp_attach.h"
#include "openpete_atomic_file.h"
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

constexpr uint32_t kSector = DISC_IMAGE_USER_SECTOR;

/** One word the carve sets back to the linked binary's value. */
struct Restore {
    size_t offset;  /* byte offset in the carved file */
    uint32_t word;
};

/** A carve target: one [[overlay]] block of the manifest. */
struct Entry {
    std::string name;
    int wad_entry = -1;
    uint32_t load_addr = 0;
    bool decrypt = false;
    std::vector<Restore> restore;
};

/** @brief Parses a `restore = [[offset, word], ...]` value.
 *
 * @param v The text after `restore = `.
 * @param out Receives the pairs, in order.
 * @return false when the value is not a list of number pairs. */
bool parse_restore(const std::string& v, std::vector<Restore>& out) {
    const char* p = v.c_str();
    std::vector<unsigned long long> nums;
    while (*p) {
        if (*p >= '0' && *p <= '9') {
            char* end = nullptr;
            nums.push_back(std::strtoull(p, &end, 0));
            p = end;
        } else if (*p == '[' || *p == ']' || *p == ',' || *p == ' ') {
            p++;
        } else {
            return false;
        }
    }
    if (nums.empty() || nums.size() % 2) return false;
    for (size_t i = 0; i < nums.size(); i += 2) {
        if (nums[i + 1] > 0xFFFFFFFFull) return false;
        out.push_back({(size_t)nums[i], (uint32_t)nums[i + 1]});
    }
    return true;
}

/** @brief Reads the carve targets of a converged manifest and the executable's restore words.
 *
 * @param path The manifest.toml.
 * @param out Receives one Entry per [[overlay]] block, in file order.
 * @param exe_restore Receives the [main_exe] block's restore words.
 * @return false when the file cannot be read, holds no overlay, an overlay
 *         names no wad_entry, or a restore value does not parse. */
bool read_entries(const char* path, std::vector<Entry>& out, std::vector<Restore>& exe_restore) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    Entry cur;
    bool in_overlay = false;
    bool ok = true;
    auto flush = [&]() {
        if (in_overlay) out.push_back(cur);
        cur = Entry{};
    };
    while (std::getline(f, line)) {
        if (line == "[[overlay]]") { flush(); in_overlay = true; continue; }
        if (line.rfind("restore = ", 0) == 0) {
            ok = ok && parse_restore(line.substr(10), in_overlay ? cur.restore : exe_restore);
            continue;
        }
        if (!in_overlay) continue;
        if (line.rfind("name = \"", 0) == 0) cur.name = line.substr(8, line.size() - 9);
        else if (line.rfind("wad_entry = ", 0) == 0) cur.wad_entry = std::atoi(line.c_str() + 12);
        else if (line.rfind("load_addr = ", 0) == 0)
            cur.load_addr = (uint32_t)std::strtoul(line.c_str() + 12, nullptr, 0);
        else if (line == "decrypt = true") cur.decrypt = true;
    }
    flush();
    for (const auto& e : out)
        if (e.name.empty() || e.wad_entry < 0) return false;
    return ok && !out.empty();
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

/** @brief Writes a little-endian word into a byte buffer.
 *
 * @param p The buffer.
 * @param o The byte offset.
 * @param w The word. */
void put_le32(uint8_t* p, size_t o, uint32_t w) {
    p[o] = (uint8_t)w;
    p[o + 1] = (uint8_t)(w >> 8);
    p[o + 2] = (uint8_t)(w >> 16);
    p[o + 3] = (uint8_t)(w >> 24);
}

/** First byte of an overlay's encrypted span (func_8002CA50 in Spyro 3
 * starts its loop at base + 0x1000). */
constexpr size_t kDecryptFrom = 0x1000;

/** @brief Undoes the load-time XOR of an overlay carved from the disc.
 *
 * The key is the le32 at byte 4, an address: the words from byte 0x1000 up
 * to key - load_addr are XORed with it, the inverse of the game loader's
 * in-place loop over the same span.
 * @param buf The carved bytes.
 * @param len Their length.
 * @param load_addr The overlay's load address.
 * @return false when the key's end lies before the load address or past
 *         the carved bytes. */
bool decrypt_overlay(uint8_t* buf, size_t len, uint32_t load_addr) {
    if (len < 8) return false;
    const uint32_t key = le32(buf, 4);
    if (key < load_addr || (size_t)(key - load_addr) > len) return false;
    const size_t end = key - load_addr;
    for (size_t o = kDecryptFrom; o < end && o + 4 <= len; o += 4)
        put_le32(buf, o, le32(buf, o) ^ key);
    return true;
}

/** @brief Sets each restore word of a carved binary.
 *
 * @param buf The carved bytes.
 * @param len Their length.
 * @param rows The words and their byte offsets.
 * @return false when a word lies past the carved bytes. */
bool apply_restore(uint8_t* buf, size_t len, const std::vector<Restore>& rows) {
    for (const Restore& r : rows) {
        if (r.offset > len || len - r.offset < 4) return false;
        put_le32(buf, r.offset, r.word);
    }
    return true;
}

}  // namespace

/* Writes PSX.EXE (the game's executable, by its ISO 9660 name) and
 * <name>.ovl for every manifest overlay, each the bytes of its WAD entry
 * with the manifest's decrypt and restore applied. A failed carve is logged
 * and the remaining entries still run. */
extern "C" int openpete_extract_wad_entry_inputs(const char* cue_path,
                                                 const char* out_dir,
                                                 const char* manifest_path) {
    if (!cue_path || !out_dir || !manifest_path) {
        std::fprintf(stderr, "extract-recomp-inputs: needs a disc, a directory and a manifest\n");
        return -1;
    }
    std::vector<Entry> entries;
    std::vector<Restore> exe_restore;
    if (!read_entries(manifest_path, entries, exe_restore)) {
        OPL_ERROR_F("extract", "%s names no wad_entry for some overlay, or a restore list "
                    "does not parse", manifest_path);
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
    if (!apply_restore(exe, exe_size, exe_restore)) {
        OPL_ERROR_F("extract", "%s: a restore word lies past the executable's %zu bytes",
                    game_get_exe_filename(), exe_size);
        std::free(exe);
        return -1;
    }
    {
        const std::string exe_path = (std::filesystem::path(out_dir) / "PSX.EXE").string();
        const int wrc = op_atomic_write_file(exe_path.c_str(), exe, exe_size);
        std::free(exe);
        if (wrc != 0) return -1;
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
    for (const Entry& ent : entries) {
        const std::string& name = ent.name;
        const int e = ent.wad_entry;
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
        if (ok && ent.decrypt && !decrypt_overlay(buf.data(), len, ent.load_addr)) {
            OPL_ERROR_F("extract", "%s: WAD entry %d's key 0x%08X is no end address of its %u bytes",
                        name.c_str(), e, le32(buf.data(), 4), len);
            ok = false;
        }
        if (ok && !apply_restore(buf.data(), len, ent.restore)) {
            OPL_ERROR_F("extract", "%s: a restore word lies past WAD entry %d's %u bytes",
                        name.c_str(), e, len);
            ok = false;
        }
        const std::string ovl_path = (std::filesystem::path(out_dir) / (name + ".ovl")).string();
        if (ok) ok = op_atomic_write_file(ovl_path.c_str(), buf.data(), len) == 0;
        if (ok) carved++;
        else OPL_ERROR_F("extract", "carve failed: %s (WAD entry %d)", name.c_str(), e);
    }
    OPL_INFO_F("extract", "recomp inputs: PSX.EXE + %d/%zu overlays -> %s",
               carved, entries.size(), out_dir);
    return carved == (int)entries.size() ? 0 : -1;
}
