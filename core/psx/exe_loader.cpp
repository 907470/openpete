// core/psx/exe_loader.cpp - fetch PSX EXE bytes from disc or path.
//
// Two routes:
//   1. --exe-path PATH       : read PSX EXE directly (boot bring-up;
//                              matches the decomp's build/PSX.EXE).
//   2. --disc CUE-PATH       : read it out of a BIN/CUE via iso9660 lookup of
//                              the per-game EXE filename (disc_image).

#include "openpete_psx.h"
#include "openpete_log.h"

#include "openpete_disc_image.h"
#include "openpete_disc_trace.h"

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

/** @brief Read one file out of an open disc image, refusing a directory size above max_bytes before allocating.
 *
 * @param img The open image.
 * @param filename iso9660 path of the file.
 * @param max_bytes The largest size the caller accepts.
 * @param out_size Receives the byte count on success.
 * @return The bytes, freed by the caller with free(); NULL on failure. */
static uint8_t* read_disc_file(disc_image_t* img, const char* filename,
                               uint32_t max_bytes, size_t* out_size) {
    disc_image_entry_t entry{};
    if (disc_image_find(img, filename, &entry) != 0 || entry.is_dir) {
        OPL_ERROR_F("disc", "file not found on disc: %s", filename);
        return nullptr;
    }
    OPL_INFO_F("disc", "disc file %s @ LBA=%u size=%u", filename, entry.lba, entry.size);
    if (entry.size > max_bytes) {
        OPL_ERROR_F("disc", "disc file %s: directory size %u exceeds %u; refused",
                    filename, entry.size, max_bytes);
        return nullptr;
    }

    auto* buf = static_cast<uint8_t*>(std::malloc(entry.size ? entry.size : 1u));
    if (!buf) return nullptr;

    size_t got = disc_image_read_file(img, &entry, buf, entry.size);
    if (got != entry.size) {
        OPL_ERROR_F("disc", "disc file %s: read %zu/%u bytes", filename, got, entry.size);
        std::free(buf);
        return nullptr;
    }

    *out_size = got;
    return buf;
}

/** @brief Open a disc image and read one file out of it; see read_disc_file.
 *
 * @param cue_path Disc path, resolved by openpete_psx_resolve_disc_bin.
 * @param filename iso9660 path of the file.
 * @param max_bytes The largest size the caller accepts.
 * @param tag The OPENPETE_DISC_TAG_* path the reads are traced and steered
 *        under; OPENPETE_DISC_TAG_NONE takes the thread's trace scope and
 *        never steers the disc cache loader.
 * @param out_size Receives the byte count on success.
 * @return The bytes, freed by the caller with free(); NULL on failure. */
static uint8_t* load_disc_file(const char* cue_path, const char* filename,
                               uint32_t max_bytes, int tag, size_t* out_size) {
    if (!cue_path || !filename) return nullptr;
    disc_image_t* img = disc_image_open(cue_path, DISC_READ_CACHED, tag);
    if (!img) return nullptr;
    uint8_t* bytes = read_disc_file(img, filename, max_bytes, out_size);
    disc_image_close(img);
    return bytes;
}

extern "C" uint8_t* openpete_psx_load_file_from_disc(const char* cue_path,
                                                    const char* filename,
                                                    size_t* out_size) {
    return load_disc_file(cue_path, filename, DISC_FILE_MAX_BYTES, OPENPETE_DISC_TAG_NONE,
                          out_size);
}

extern "C" uint8_t* openpete_psx_read_exe_from_image(struct disc_image* img,
                                                     const char* exe_filename,
                                                     size_t* out_size) {
    if (!img || !exe_filename) return nullptr;
    return read_disc_file(img, exe_filename, PSX_EXE_MAX_BYTES, out_size);
}

extern "C" uint8_t* openpete_psx_load_exe_from_disc(const char* cue_path,
                                                   const char* exe_filename,
                                                   size_t* out_size) {
    return load_disc_file(cue_path, exe_filename, PSX_EXE_MAX_BYTES, OPENPETE_DISC_TAG_EXE,
                          out_size);
}

namespace {

/** @brief Locate the BOOT value of a SYSTEM.CNF text.
 *
 * The value starts after the '=', optional spaces, an optional "cdrom:" and
 * leading slashes or backslashes, and ends at ';', a space or the line end,
 * which tolerates the spacing and slash variations real images use: the
 * line `BOOT = cdrom:/SCUS_942.28;1` gives `SCUS_942.28`.
 * @param text The NUL-terminated text.
 * @param len Receives the value's length; 0 when there is none.
 * @return The value's first character, or nullptr when the text has no BOOT key. */
const char* boot_value(const char* text, size_t* len) {
    *len = 0;
    const char* boot = std::strstr(text, "BOOT");
    const char* eq = boot ? std::strchr(boot, '=') : nullptr;
    if (!eq) return nullptr;
    const char* p = eq + 1 + std::strspn(eq + 1, " \t");
    if (!std::strncmp(p, "cdrom:", 6)) p += 6;
    p += std::strspn(p, "\\/");
    *len = std::strcspn(p, "; \t\r\n");
    return p;
}

/** @brief Write the release serial a boot path names.
 *
 * The serial is the path's last component with '.' and '_' made '-' and the
 * second '-' dropped, since the dot inside the numeric half is 8.3 padding:
 * `SCUS_942.28` gives `SCUS-94228`.
 * @param path The boot path.
 * @param n Its length.
 * @param serial Receives the serial, cut at @p cap - 1.
 * @param cap Capacity of @p serial; at least 1. */
void serial_of_boot_path(const char* path, size_t n, char* serial, size_t cap) {
    size_t start = 0;
    for (size_t i = 0; i < n; i++)
        if (path[i] == '\\' || path[i] == '/') start = i + 1;
    size_t o = 0;
    for (size_t i = start; i < n && o + 1 < cap; i++)
        serial[o++] = (path[i] == '.' || path[i] == '_') ? '-' : path[i];
    serial[o] = '\0';
    char* second = std::strchr(serial, '-');
    char* third = second ? std::strchr(second + 1, '-') : nullptr;
    if (third) std::memmove(third, third + 1, std::strlen(third));
}

}  // namespace

/* BOOT line of a SYSTEM.CNF text; see openpete_psx.h. */
extern "C" int openpete_psx_parse_boot_line(const char* text, char* path, size_t path_cap,
                                            char* serial, size_t serial_cap) {
    if (path && path_cap) path[0] = '\0';
    if (serial && serial_cap) serial[0] = '\0';
    size_t n = 0;
    const char* p = text ? boot_value(text, &n) : nullptr;
    if (!p || n == 0) return -1;
    if (path && path_cap) {
        if (n + 1 > path_cap) return -1;
        std::memcpy(path, p, n);
        path[n] = '\0';
    }
    if (serial && serial_cap) {
        serial_of_boot_path(p, n, serial, serial_cap);
        if (!serial[0]) return -1;
    }
    return 0;
}

/* Serial from SYSTEM.CNF's BOOT line; see openpete_psx.h. */
extern "C" int openpete_psx_read_disc_serial(const char* image_path,
                                             char* out, size_t out_cap) {
    if (!image_path || !out || out_cap < 12) return -1;
    out[0] = '\0';

    disc_image_t* img = disc_image_open(image_path, DISC_READ_CACHED, OPENPETE_DISC_TAG_SERIAL);
    if (!img) return -1;

    disc_image_entry_t entry{};
    if (disc_image_find(img, "SYSTEM.CNF", &entry) != 0 || entry.is_dir ||
        entry.size == 0 || entry.size > 4096) {
        disc_image_close(img);
        return -1;
    }
    std::vector<uint8_t> buf(entry.size + 1, 0);
    size_t got = disc_image_read_file(img, &entry, buf.data(), entry.size);
    disc_image_close(img);
    if (got != entry.size)
        return -1;
    return openpete_psx_parse_boot_line(reinterpret_cast<const char*>(buf.data()), nullptr, 0,
                                        out, out_cap);
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
