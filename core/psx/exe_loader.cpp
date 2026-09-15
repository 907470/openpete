// core/psx/exe_loader.cpp — fetch PSX EXE bytes from disc or path.
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

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

extern "C" uint8_t* openpete_psx_load_file_from_disc(const char* cue_path,
                                                    const char* filename,
                                                    size_t* out_size) {
    if (!cue_path || !filename) return nullptr;

    PS1::ISOReader reader;
    if (!reader.Open(cue_path)) {
        /* iso_reader's CUE parser uses the FILE "..." line literally, which
         * sometimes mismatches the on-disc bin name. Fall back: try a sibling
         * .bin in the same directory. */
        std::filesystem::path cue(cue_path);
        std::filesystem::path dir = cue.parent_path();
        bool retried = false;
        for (auto& e : std::filesystem::directory_iterator(dir)) {
            auto p = e.path();
            if (p.extension() == ".bin" || p.extension() == ".BIN") {
                if (reader.Open(p.string())) {
                    OPL_INFO_F("disc",
                               "fell back to direct BIN: %s (cue's FILE line did not match)",
                               p.string().c_str());
                    retried = true;
                    break;
                }
            }
        }
        if (!retried) {
            OPL_ERROR_F("disc", "ISOReader::Open failed for %s", cue_path);
            return nullptr;
        }
    }

    PS1::ISOFileEntry entry{};
    if (!reader.FindFile(filename, entry)) {
        OPL_ERROR_F("disc", "file not found on disc: %s", filename);
        return nullptr;
    }
    OPL_INFO_F("disc", "disc file %s @ LBA=%u size=%u", filename, entry.lba, entry.size);

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

extern "C" uint8_t* openpete_psx_load_exe_from_disc(const char* cue_path,
                                                   const char* exe_filename,
                                                   size_t* out_size) {
    return openpete_psx_load_file_from_disc(cue_path, exe_filename, out_size);
}

/* The disc's own serial ("SCUS-94228"), read out of SYSTEM.CNF's
 * BOOT=cdrom:\SCUS_942.28;1 line. A few sectors, so it is cheap enough to
 * run at every boot — which is what lets openpete.toml carry per-release
 * settings ([disc.SCUS-94228]) for an image the library has never seen and
 * whose content hash we therefore don't know. Returns 0 on success. */
extern "C" int openpete_psx_read_disc_serial(const char* image_path,
                                             char* out, size_t out_cap) {
    if (!image_path || !out || out_cap < 12) return -1;
    out[0] = '\0';

    PS1::ISOReader reader;
    if (!reader.Open(image_path)) {
        std::filesystem::path given(image_path);
        std::error_code ec;
        bool opened = false;
        for (auto& e : std::filesystem::directory_iterator(given.parent_path(), ec)) {
            auto p = e.path();
            if ((p.extension() == ".bin" || p.extension() == ".BIN") &&
                reader.Open(p.string())) { opened = true; break; }
        }
        if (!opened) return -1;
    }

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
