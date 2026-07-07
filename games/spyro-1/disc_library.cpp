// games/spyro-1/disc_library.cpp — the content-addressed disc library
// (distribution D2.5; plan openpete-distribution-local-recomp).
//
//   library/<disc_sha256>/
//     disc.bin, disc.cue     ingested copy (cue synthesized)
//     meta.toml              label / source / ingested / last_played
//     modules/<codegen-stamp>/spyro1-recomp.so   (+ obj/ kept)
//
// Lives next to the exe (portable-install layout). Resolution order at
// boot: explicit --disc = dev path, library untouched; --ingest PATH
// stream-hashes + copies the image in (dedup by content), then boots
// the entry; no disc at all = scan the library and boot the single /
// most-recently-played entry (the version-selector UI arrives with the
// splash). After ingest the user's original file is never needed again
// — engine updates rebuild the module from the library's own copy.
#include "openpete.h"
#include "openpete_log.h"
#include "sha256.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#include <limits.h>
#endif

namespace fs = std::filesystem;

extern "C" int spyro1_recomp_codegen_stamp(char* out, size_t cap);

namespace {

/* README-pinned retail NTSC-U disc hash. */
constexpr const char* kRetailSha =
    "95f03abf97c9ff0b2a64888ed7dbbb4b59a7b4363cf188cd0a562b95cfd4809f";

std::string exe_dir() {
#ifndef _WIN32
    char exe[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (len > 0) {
        exe[len] = 0;
        if (char* slash = strrchr(exe, '/')) { *slash = 0; return exe; }
    }
    return ".";
#else
    return ".";  /* Windows library path lands with the D3 packaging pass;
                  * "." == the app dir under normal launch there. */
#endif
}

fs::path library_root() { return fs::path(exe_dir()) / "library"; }

/* Locate the .bin for an image path the user handed us (.bin itself,
 * or a .cue whose sibling/only .bin we find — same tolerance as
 * run.c's derive_bin_path + exe_loader's fallback). */
fs::path locate_bin(const fs::path& given) {
    if (given.extension() == ".bin" || given.extension() == ".BIN")
        return given;
    fs::path sib = given;
    sib.replace_extension(".bin");
    if (fs::exists(sib)) return sib;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(given.parent_path(), ec))
        if (e.path().extension() == ".bin" || e.path().extension() == ".BIN")
            return e.path();
    return {};
}

bool stream_hash_copy(const fs::path& src, const fs::path& dst, char sha_hex[65]) {
    std::ifstream in(src, std::ios::binary);
    if (!in) return false;
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    openpete_sha256_ctx c;
    openpete_sha256_init(&c);
    std::vector<char> buf(1 << 20);
    while (in.read(buf.data(), (std::streamsize)buf.size()) || in.gcount() > 0) {
        openpete_sha256_update(&c, buf.data(), (size_t)in.gcount());
        out.write(buf.data(), in.gcount());
        if (!in) break;
    }
    uint8_t d[32];
    openpete_sha256_final(&c, d);
    openpete_sha256_hex(d, sha_hex);
    return out.good();
}

void write_meta(const fs::path& entry, const std::string& label,
                const std::string& source, long long last_played) {
    std::ofstream f(entry / "meta.toml", std::ios::trunc);
    f << "label = \"" << label << "\"\n"
      << "source = \"" << source << "\"\n"
      << "last_played = " << last_played << "\n";
}

struct Meta {
    fs::path dir;
    std::string label;
    std::string source;
    long long last_played = 0;
};

bool read_meta(const fs::path& entry, Meta& m) {
    std::ifstream f(entry / "meta.toml");
    if (!f) return false;
    m.dir = entry;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("label = \"", 0) == 0)
            m.label = line.substr(9, line.size() - 10);
        else if (line.rfind("source = \"", 0) == 0)
            m.source = line.substr(10, line.size() - 11);
        else if (line.rfind("last_played = ", 0) == 0)
            m.last_played = atoll(line.c_str() + 14);
    }
    return true;
}

void touch_last_played(const Meta& m) {
    write_meta(m.dir, m.label, m.source, (long long)time(nullptr));
}

/* The entry's cue is what the runtime consumes; single data track,
 * MODE2/2352, referencing the library copy by its fixed name. */
void write_cue(const fs::path& entry) {
    std::ofstream f(entry / "disc.cue", std::ios::trunc);
    f << "FILE \"disc.bin\" BINARY\n"
         "  TRACK 01 MODE2/2352\n"
         "    INDEX 01 00:00:00\n";
}

/* Boot a library entry: point the runtime at the entry's cue and (for
 * shared linkage) at its stamp-keyed module path. Static storage for
 * the strings opts point into. */
char s_disc_path[4096];
char s_module_path[4096];

int boot_entry(openpete_options_t* opts, Meta& m,
               char* module_path_out, size_t module_path_cap) {
    std::snprintf(s_disc_path, sizeof s_disc_path, "%s",
                  (m.dir / "disc.cue").string().c_str());
    opts->disc_path = s_disc_path;
    if (module_path_out && module_path_cap) {
        char stamp[128];
        if (spyro1_recomp_codegen_stamp(stamp, sizeof stamp) == 0) {
            fs::path mod = m.dir / "modules" / stamp / "spyro1-recomp.so";
            std::error_code ec;
            fs::create_directories(mod.parent_path(), ec);
            std::snprintf(module_path_out, module_path_cap, "%s",
                          mod.string().c_str());
        } else {
            module_path_out[0] = 0;   /* static build / no PSXRecomp yet */
        }
    }
    touch_last_played(m);
    OPL_INFO_F("library", "booting library entry %s (%s)",
               m.dir.filename().string().c_str(), m.label.c_str());
    return 0;
}

}  // namespace

extern "C" int spyro1_library_resolve(openpete_options_t* opts,
                                      char* module_path_out,
                                      size_t module_path_cap) {
    if (module_path_out && module_path_cap) module_path_out[0] = 0;

    /* --ingest PATH: copy into the library, then boot that entry. */
    if (opts->ingest_path) {
        fs::path bin = locate_bin(opts->ingest_path);
        if (bin.empty()) {
            std::fprintf(stderr, "openpete: --ingest: no .bin found for %s\n",
                         opts->ingest_path);
            return -1;
        }
        std::error_code ec;
        fs::path root = library_root();
        fs::path tmp = root / ("ingest-tmp-" + std::to_string(
#ifndef _WIN32
                                   (long long)getpid()
#else
                                   0LL
#endif
                                   ));
        fs::create_directories(tmp, ec);
        if (ec) {
            std::fprintf(stderr, "openpete: library dir not writable: %s\n",
                         root.string().c_str());
            return -1;
        }
        char sha[65];
        std::fprintf(stderr, "[library] copying disc into your library...\n");
        if (!stream_hash_copy(bin, tmp / "disc.bin", sha)) {
            std::fprintf(stderr, "openpete: --ingest copy failed\n");
            fs::remove_all(tmp, ec);
            return -1;
        }
        fs::path entry = root / sha;
        if (fs::exists(entry / "disc.bin")) {
            std::fprintf(stderr, "[library] already in your library (%s)\n", sha);
            fs::remove_all(tmp, ec);
        } else {
            fs::rename(tmp, entry, ec);
            if (ec) {
                std::fprintf(stderr, "openpete: library install failed: %s\n",
                             ec.message().c_str());
                fs::remove_all(tmp, ec);
                return -1;
            }
            write_cue(entry);
            std::string label = (std::strcmp(sha, kRetailSha) == 0)
                                    ? "Spyro the Dragon (NTSC-U)"
                                    : bin.filename().string();
            write_meta(entry, label, bin.filename().string(),
                       (long long)time(nullptr));
            std::fprintf(stderr, "[library] added %s (%s)\n", label.c_str(), sha);
        }
        Meta m;
        if (!read_meta(entry, m)) return -1;
        return boot_entry(opts, m, module_path_out, module_path_cap);
    }

    /* Explicit --disc: dev/CI path, the library stays out of the way. */
    if (opts->disc_path || opts->exe_path) return 0;

    /* No disc given: boot from the library. */
    std::vector<Meta> entries;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(library_root(), ec)) {
        Meta m;
        if (e.is_directory() && read_meta(e.path(), m)) entries.push_back(m);
    }
    if (entries.empty()) return 0;   /* fall through to the normal
                                      * "--disc is required" diagnostic
                                      * (the splash replaces this) */
    Meta* pick = &entries[0];
    for (auto& m : entries)
        if (m.last_played > pick->last_played) pick = &m;
    if (entries.size() > 1)
        std::fprintf(stderr,
                     "[library] %zu versions installed — booting most recent "
                     "'%s' (version selector UI comes with the splash)\n",
                     entries.size(), pick->label.c_str());
    return boot_entry(opts, *pick, module_path_out, module_path_cap);
}
