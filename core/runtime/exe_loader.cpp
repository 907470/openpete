// core/runtime/exe_loader.cpp — fetch PSX EXE bytes from disc or path.
//
// Phase 1b uses two routes:
//   1. --exe-path PATH       : read PSX EXE directly (preferred for Phase 1b
//                              boot bring-up; matches the decomp's build/PSX.EXE).
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

extern "C" uint8_t* openpete_psx_load_exe_from_disc(const char* cue_path,
                                                   const char* exe_filename,
                                                   size_t* out_size) {
    if (!cue_path || !exe_filename) return nullptr;

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
    if (!reader.FindFile(exe_filename, entry)) {
        OPL_ERROR_F("disc", "EXE not found on disc: %s", exe_filename);
        return nullptr;
    }
    OPL_INFO_F("disc", "EXE %s @ LBA=%u size=%u", exe_filename, entry.lba, entry.size);

    auto* buf = static_cast<uint8_t*>(std::malloc(entry.size));
    if (!buf) return nullptr;

    size_t got = reader.ReadFile(exe_filename, buf, entry.size);
    if (got != entry.size) {
        OPL_ERROR_F("disc", "ReadFile got %zu/%u bytes", got, entry.size);
        std::free(buf);
        return nullptr;
    }

    *out_size = got;
    return buf;
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
