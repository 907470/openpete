// core/psx/exe_loader.cpp - fetch PSX EXE bytes from disc or path.
//
// Two routes:
//   1. --exe-path PATH       : read PSX EXE directly (boot bring-up;
//                              matches the decomp's build/PSX.EXE).
//   2. --disc CUE-PATH       : read it out of a BIN/CUE via iso9660 lookup of
//                              the per-game EXE filename.
//
// The disc path leans on psxrecomp's iso_reader (vendored, untouched) so we
// stay in step with the recompiler's view of disc layout.

#include "openpete_psx.h"
#include "openpete_log.h"

#include "../../third_party/psxrecomp/runner/include/iso_reader.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

/** @brief Test a path's extension, case-insensitively, against a lowercase literal.
 *
 * @param p Path.
 * @param lower Extension with its dot, lowercase (".cue").
 * @return true when the extension matches. */
bool ext_is(const std::filesystem::path& p, const char* lower) {
    std::string e = p.extension().string();
    for (char& c : e) c = (char)std::tolower((unsigned char)c);
    return e == lower;
}

/** @brief Test whether a path names a regular file.
 *
 * A path that cannot be examined counts as absent.
 * @param p Path.
 * @return true when the path is an existing regular file. */
bool file_exists(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}

/** @brief Return the path the cue's first FILE line names, resolved against the cue's directory when relative.
 *
 * The first line containing FILE and a double-quoted string supplies the
 * name between the quotes.
 * @param cue Cue sheet path.
 * @return The path; empty when the cue cannot be opened or has no such
 *         line. */
std::filesystem::path cue_named_bin(const std::filesystem::path& cue) {
    std::ifstream f(cue);
    if (!f.is_open()) return {};
    std::string line;
    while (std::getline(f, line)) {
        if (line.find("FILE") == std::string::npos) continue;
        size_t q0 = line.find('"');
        if (q0 == std::string::npos) continue;
        size_t q1 = line.find('"', q0 + 1);
        if (q1 == std::string::npos) continue;
        std::filesystem::path named(line.substr(q0 + 1, q1 - q0 - 1));
        if (named.is_relative()) return cue.parent_path() / named;
        return named;
    }
    return {};
}

/** @brief Return the single .bin file in the cue's directory.
 *
 * More than one .bin resolves to nothing, since guessing between images
 * would boot the wrong disc. The extension test is case-insensitive.
 * @param cue Cue sheet path.
 * @return The path; empty when the directory holds no .bin or more than
 *         one, or cannot be listed. */
std::filesystem::path lone_sibling_bin(const std::filesystem::path& cue) {
    std::error_code ec;
    std::filesystem::path found;
    for (std::filesystem::directory_iterator it(cue.parent_path(), ec), end;
         !ec && it != end; it.increment(ec)) {
        const std::filesystem::path& p = it->path();
        if (!ext_is(p, ".bin")) continue;
        if (!found.empty()) return {};
        found = p;
    }
    return found;
}

/** @brief Append a path to a comma-separated candidate list.
 *
 * An empty path is skipped.
 * @param tried List to extend.
 * @param p Path to append. */
void append_tried(std::string& tried, const std::filesystem::path& p) {
    if (p.empty()) return;
    if (!tried.empty()) tried += ", ";
    tried += p.string();
}

} // namespace

extern "C" int openpete_psx_resolve_disc_bin(const char* disc_path,
                                             char* out, size_t out_cap,
                                             char* tried, size_t tried_cap) {
    if (tried && tried_cap) tried[0] = '\0';
    if (!disc_path || !out || out_cap == 0) return -1;
    out[0] = '\0';

    std::string cands;
    std::filesystem::path resolved;
    try {
        std::filesystem::path given(disc_path);
        if (ext_is(given, ".cue")) {
            if (!file_exists(given)) {
                append_tried(cands, given);
            } else {
                const std::filesystem::path order[] = {
                    cue_named_bin(given),
                    std::filesystem::path(given).replace_extension(".bin"),
                    lone_sibling_bin(given),
                };
                for (const auto& c : order) {
                    if (c.empty()) continue;
                    append_tried(cands, c);
                    if (file_exists(c)) { resolved = c; break; }
                }
            }
        } else if (ext_is(given, ".bin") || ext_is(given, ".img") ||
                   ext_is(given, ".iso")) {
            append_tried(cands, given);
            if (file_exists(given)) resolved = given;
        } else {
            append_tried(cands, given);
        }
    } catch (const std::exception&) {
        resolved.clear();
    }

    if (tried && tried_cap)
        std::snprintf(tried, tried_cap, "%s", cands.c_str());
    if (resolved.empty()) return -1;

    std::string r = resolved.string();
    if (r.size() + 1 > out_cap) return -1;
    std::snprintf(out, out_cap, "%s", r.c_str());
    return 0;
}

/* The largest file the disc loader allocates: PsyCross addresses the image
 * with an int, so no file on it is longer. */
#define DISC_FILE_MAX_BYTES 0x7FFFFFFFu
/* A PSX EXE's body loads into the 2 MiB of main RAM behind its 2048-byte header. */
#define PSX_EXE_MAX_BYTES (OPENPETE_PSX_RAM_SIZE + 2048u)

/** @brief Read one file out of a disc image, refusing a directory size above max_bytes before allocating.
 *
 * @param cue_path Disc path, resolved by openpete_psx_resolve_disc_bin.
 * @param filename iso9660 path of the file.
 * @param max_bytes The largest size the caller accepts.
 * @param out_size Receives the byte count on success.
 * @return The bytes, freed by the caller with free(); NULL on failure. */
static uint8_t* load_disc_file(const char* cue_path, const char* filename,
                               uint32_t max_bytes, size_t* out_size) {
    if (!cue_path || !filename) return nullptr;

    char bin[4096], tried[4096];
    if (openpete_psx_resolve_disc_bin(cue_path, bin, sizeof bin,
                                      tried, sizeof tried) != 0) {
        OPL_ERROR_F("disc", "no disc image at --disc %s (tried: %s)",
                    cue_path, tried[0] ? tried : "(nothing)");
        return nullptr;
    }

    PS1::ISOReader reader;
    if (!reader.Open(bin)) {
        OPL_ERROR_F("disc", "ISOReader::Open failed for %s (--disc %s)",
                    bin, cue_path);
        return nullptr;
    }

    PS1::ISOFileEntry entry{};
    if (!reader.FindFile(filename, entry)) {
        OPL_ERROR_F("disc", "file not found on disc: %s", filename);
        return nullptr;
    }
    OPL_INFO_F("disc", "disc file %s @ LBA=%u size=%u", filename, entry.lba, entry.size);
    if (entry.size > max_bytes) {
        OPL_ERROR_F("disc", "disc file %s: directory size %u exceeds %u; refused",
                    filename, entry.size, max_bytes);
        return nullptr;
    }

    auto* buf = static_cast<uint8_t*>(std::malloc(entry.size));
    if (!buf) return nullptr;

    size_t got = reader.ReadFile(filename, buf, entry.size);
    if (got != entry.size) {
        OPL_ERROR_F("disc", "ReadFile got %zu/%u bytes", got, entry.size);
        std::free(buf);
        return nullptr;
    }

    *out_size = got;
    return buf;
}

extern "C" uint8_t* openpete_psx_load_file_from_disc(const char* cue_path,
                                                    const char* filename,
                                                    size_t* out_size) {
    return load_disc_file(cue_path, filename, DISC_FILE_MAX_BYTES, out_size);
}

extern "C" uint8_t* openpete_psx_load_exe_from_disc(const char* cue_path,
                                                   const char* exe_filename,
                                                   size_t* out_size) {
    return load_disc_file(cue_path, exe_filename, PSX_EXE_MAX_BYTES, out_size);
}

/* Serial from SYSTEM.CNF's BOOT line; see openpete_psx.h. */
extern "C" int openpete_psx_read_disc_serial(const char* image_path,
                                             char* out, size_t out_cap) {
    if (!image_path || !out || out_cap < 12) return -1;
    out[0] = '\0';

    char bin[4096];
    if (openpete_psx_resolve_disc_bin(image_path, bin, sizeof bin,
                                      nullptr, 0) != 0)
        return -1;

    PS1::ISOReader reader;
    if (!reader.Open(bin)) return -1;

    PS1::ISOFileEntry entry{};
    if (!reader.FindFile("SYSTEM.CNF", entry) || entry.size == 0 ||
        entry.size > 4096)
        return -1;
    std::vector<uint8_t> buf(entry.size + 1, 0);
    if (reader.ReadFile("SYSTEM.CNF", buf.data(), entry.size) != entry.size)
        return -1;
    const char* text = reinterpret_cast<const char*>(buf.data());

    /* BOOT = cdrom:\SCUS_942.28;1  ->  SCUS-94228. Tolerant of the spacing
     * and slash direction variations real images use. */
    const char* boot = strstr(text, "BOOT");
    if (!boot) return -1;
    const char* eq = strchr(boot, '=');
    if (!eq) return -1;
    const char* p = eq + 1;
    while (*p == ' ' || *p == '\t') p++;
    if (!strncmp(p, "cdrom:", 6)) p += 6;
    while (*p == '\\' || *p == '/') p++;

    size_t o = 0;
    for (; *p && *p != ';' && *p != '\r' && *p != '\n' && o + 1 < out_cap; p++) {
        if (*p == '.' || *p == '_') { out[o++] = '-'; continue; }
        out[o++] = *p;
    }
    out[o] = '\0';
    /* "SCUS-942-28" -> "SCUS-94228": the dot inside the numeric half is
     * 8.3 padding, not part of the serial. */
    char* second = strchr(out, '-');
    if (second) {
        char* third = strchr(second + 1, '-');
        if (third) memmove(third, third + 1, strlen(third));
    }
    return out[0] ? 0 : -1;
}

/** @brief Read a whole host file into a malloc'd buffer.
 *
 * Used for the --exe-path route, which loads a PSX EXE straight from the
 * host file system. An open failure, an empty or unreadable file, an
 * allocation failure and a short read each return NULL, the first, second
 * and fourth with an error logged.
 * @param path Host file path.
 * @param out_size Receives the byte count on success.
 * @return The bytes, freed by the caller with free(); NULL on failure. */
extern "C" uint8_t* openpete_psx_read_file(const char* path, size_t* out_size) {
    if (!path) return nullptr;
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        OPL_ERROR_F("boot", "open failed: %s", path);
        return nullptr;
    }
    std::streamsize len = f.tellg();
    if (len <= 0) {
        OPL_ERROR_F("boot", "empty/unreadable: %s", path);
        return nullptr;
    }
    f.seekg(0, std::ios::beg);
    auto* buf = static_cast<uint8_t*>(std::malloc((size_t)len));
    if (!buf) return nullptr;
    if (!f.read(reinterpret_cast<char*>(buf), len)) {
        std::free(buf);
        OPL_ERROR_F("boot", "read failed: %s", path);
        return nullptr;
    }
    *out_size = (size_t)len;
    return buf;
}
