// games/spyro-1/dist/disc_library.cpp - the content-addressed disc library.
//
//   library/<disc_sha256>/
//     disc.bin, disc.cue     ingested copy (cue synthesized)
//     meta.toml              label / source / sha256 / last_played / game
//     modules/<codegen-stamp>/spyro1-recomp.so   (+ obj/ kept)
//
// Lives wherever openpete_paths puts writable data: next to the exe in a
// portable install, $XDG_DATA_HOME/openpete/library under the user layout
// (which is what a read-only AppImage mount forces). Resolution order at
// boot: --ingest PATH stream-hashes + copies the image in (dedup by
// content), then boots the entry; else an explicit --disc or exe path = dev
// path, library untouched; else the library is scanned and the
// most-recently-played entry boots, or the splash version selector picks
// among several. After ingest the user's original file is never needed again
// - engine updates rebuild the module from the library's own copy.
#include "openpete.h"
#include "openpete_disc_image.h"
#include "openpete_disc_trace.h"
#include "openpete_identify.h" /* openpete_game_for_serial */
#include "openpete_log.h"
#include "openpete_paths.h" /* library root: portable vs per-user data dir */
#include "openpete_psx.h"   /* openpete_psx_read_disc_serial */
#include "sha256.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <limits.h>
#endif

namespace fs = std::filesystem;

extern "C" int spyro1_recomp_codegen_stamp(char* out, size_t cap);

/* UI progress hook: NULL = console-only. The splash (splash.c) installs
 * itself here from game main when a window is allowed. */
extern "C" {
void (*spyro1_progress_cb)(const char* stage, int done, int total) = nullptr;
}

/* Splash selector (splash.c). Weak coupling by declaration only - the
 * splash TU is always linked into the game exe. */
extern "C" int spyro1_splash_active(void);
extern "C" int spyro1_splash_open(void);
extern "C" int spyro1_splash_select(const char* const* labels, int count);
extern "C" const char* spyro1_splash_dropped_path(void);
extern "C" int spyro1_splash_ui_enabled(void);
extern "C" int spyro1_splash_run_task(int (*fn)(void*), void* userdata);

namespace {

/* README-pinned retail NTSC-U disc hash. */
constexpr const char* kRetailSha =
    "fc866b2a02e010a6658f8af2de28bb3001eb33513e5924af014e35643c6dee37";

/** @brief Returns the directory holding the running executable.
 *
 * Reads /proc/self/exe on Linux and GetModuleFileNameA on Windows (with
 * backslashes turned to '/') and strips the file name.
 * @return The directory, or "." when the path cannot be read or does not fit
 *         the buffer. */
std::string exe_dir() {
#ifdef _WIN32
    char exe[4096];
    DWORD len = GetModuleFileNameA(NULL, exe, sizeof exe);
    if (len > 0 && len < sizeof exe) {
        for (char* p = exe; *p; ++p) if (*p == '\\') *p = '/';
        if (char* slash = strrchr(exe, '/')) { *slash = 0; return exe; }
    }
    return ".";
#else
    char exe[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (len > 0) {
        exe[len] = 0;
        if (char* slash = strrchr(exe, '/')) { *slash = 0; return exe; }
    }
    return ".";
#endif
}

/** @brief Returns the disc library's root directory without creating it.
 *
 * The root is the "library" data directory openpete_paths_data_dir names,
 * or "library" beside the executable when that lookup fails. Readers
 * tolerate an absent library (nothing ingested yet), and every writer goes
 * through fs::create_directories, which makes the parents, so a query
 * (--config-report, a scope lookup) leaves no empty directory behind.
 * @return The library root path. */
fs::path library_root() {
    char dir[2048];
    if (openpete_paths_data_dir("library", dir, sizeof dir, 0) != 0)
        return fs::path(exe_dir()) / "library";
    return fs::path(dir);
}

/** @brief Locates the .bin for a disc image path given on the command line.
 *
 * A path with a .bin or .BIN extension is returned as is. Otherwise the
 * path with its extension replaced by .bin is returned when it exists, else
 * the first .bin or .BIN the directory iterator yields in the path's
 * directory.
 * @param given The image path, a .bin or a .cue.
 * @return The .bin path, or an empty path when none is found. */
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

/** @brief Copies a disc image while computing its SHA-256.
 *
 * Reads src through disc_image in direct mode (never the disc cache) in
 * 1 MiB chunks, writing each to dst (created or truncated) and feeding it
 * to the hash. When spyro1_progress_cb is set, it is called with the
 * "COPYING DISC TO YOUR LIBRARY" stage and MiB counts once per 64 MiB
 * copied. The hash is finalized after a read failure too, so it covers
 * only the bytes read.
 * @param src The image to read; disc_image_open refuses one that is not
 *        a whole number of raw 2352-byte sectors.
 * @param dst The file to create or truncate.
 * @param sha_hex Receives 64 hex digits and a NUL; untouched when either file
 *        fails to open.
 * @return true when both files opened, every byte read, and dst's stream
 *         state is good after the last write, else false. */
bool stream_hash_copy(const fs::path& src, const fs::path& dst, char sha_hex[65]) {
    disc_image_t* in = disc_image_open(src.string().c_str(), DISC_READ_DIRECT,
                                       OPENPETE_DISC_TAG_INGEST);
    if (!in) return false;
    std::unique_ptr<disc_image_t, decltype(&disc_image_close)> closer(in, &disc_image_close);
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    const uint64_t total_bytes = disc_image_size(in);
    uint64_t seen = 0;
    bool read_ok = true;
    openpete_sha256_ctx c;
    openpete_sha256_init(&c);
    std::vector<char> buf(1 << 20);
    while (seen < total_bytes) {
        size_t n = total_bytes - seen < buf.size() ? (size_t)(total_bytes - seen) : buf.size();
        int r = disc_image_read_bytes(in, seen, buf.data(), n, OPENPETE_DISC_TAG_INGEST);
        openpete_disc_trace_bytes(OPENPETE_DISC_TAG_INGEST, seen, n,
                                  r < 0 ? OPENPETE_DISC_DIRECT : r);
        if (r < 0) { read_ok = false; break; }
        openpete_sha256_update(&c, buf.data(), n);
        out.write(buf.data(), (std::streamsize)n);
        seen += n;
        if (spyro1_progress_cb && (seen % (64u << 20) < (1u << 20)))
            spyro1_progress_cb("COPYING DISC TO YOUR LIBRARY",
                               (int)(seen >> 20), (int)(total_bytes >> 20));
    }
    uint8_t d[32];
    openpete_sha256_final(&c, d);
    openpete_sha256_hex(d, sha_hex);
    return read_ok && out.good();
}

/** @brief Writes an entry's meta.toml with its label, source, hash, last-played time and game.
 *
 * Truncates the file. String values are written between double quotes
 * without escaping; write errors are not reported. An empty game writes no
 * game line.
 * @param entry The library entry's directory.
 * @param label The display name.
 * @param source The file name the image was ingested from.
 * @param sha The disc's SHA-256 in hex.
 * @param last_played The last boot time in seconds since the Unix epoch.
 * @param game The registered game id the image's serial names, or "". */
void write_meta(const fs::path& entry, const std::string& label,
                const std::string& source, const std::string& sha,
                long long last_played, const std::string& game) {
    std::ofstream f(entry / "meta.toml", std::ios::trunc);
    f << "label = \"" << label << "\"\n"
      << "source = \"" << source << "\"\n"
      << "sha256 = \"" << sha << "\"\n"
      << "last_played = " << last_played << "\n";
    if (!game.empty()) f << "game = \"" << game << "\"\n";
}

struct Meta {
    fs::path dir;
    std::string label;
    std::string source;
    std::string sha;
    std::string game;   /* registered game id; "" = unidentified */
    long long last_played = 0;
};

/** @brief The registered game an image's SYSTEM.CNF serial names.
 *
 * @param image The image's cue or bin.
 * @return The game id, or "" when the image cannot be read or its serial is unregistered. */
std::string game_of_image(const fs::path& image) {
    char serial[32];
    if (openpete_psx_read_disc_serial(image.string().c_str(), serial, sizeof serial) != 0)
        return {};
    const char* g = openpete_game_for_serial(serial);
    return g ? g : "";
}

/** @brief Loads a library entry's meta.toml into a Meta.
 *
 * Recognizes the lines label, source, sha256 (`key = "value"`, the value
 * taken as the text after the opening quote minus the last character) and
 * last_played (parsed with atoll). Other lines are ignored, and a field with
 * no line keeps its default.
 * @param entry The library entry's directory.
 * @param m Receives the fields; dir is set once meta.toml has opened.
 * @return true when meta.toml opened, else false. */
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
        else if (line.rfind("sha256 = \"", 0) == 0)
            m.sha = line.substr(10, line.size() - 11);
        else if (line.rfind("last_played = ", 0) == 0)
            m.last_played = atoll(line.c_str() + 14);
        else if (line.rfind("game = \"", 0) == 0)
            m.game = line.substr(8, line.size() - 9);
    }
    return true;
}

/** @brief Records the game of an entry that has none, from its image's serial.
 *
 * An entry ingested before entries recorded their game is identified at its
 * first read in the library scan: the serial is read from the entry's
 * disc.cue (a SYSTEM.CNF read) and meta.toml rewritten with the game. An
 * entry whose image cannot be read, or whose serial names no registered
 * game, is left as it is and stays unidentified.
 * @param m The entry; its game is set when recorded. */
void record_game(Meta& m) {
    if (!m.game.empty()) return;
    std::string g = game_of_image(m.dir / "disc.cue");
    if (g.empty()) return;
    m.game = g;
    write_meta(m.dir, m.label, m.source, m.sha, m.last_played, m.game);
    OPL_INFO_F("library", "entry %s recorded as %s",
               m.dir.filename().string().c_str(), m.game.c_str());
}

/** @brief Rewrites an entry's meta.toml with last_played set to the current time. */
void touch_last_played(const Meta& m) {
    write_meta(m.dir, m.label, m.source, m.sha, (long long)time(nullptr), m.game);
}

/** @brief Writes a library entry's disc.cue, the file the runtime boots.
 *
 * The cue declares one MODE2/2352 data track referencing the entry's copy
 * by its fixed name disc.bin, and replaces any existing cue.
 * @param entry The library entry's directory. */
void write_cue(const fs::path& entry) {
    std::ofstream f(entry / "disc.cue", std::ios::trunc);
    f << "FILE \"disc.bin\" BINARY\n"
         "  TRACK 01 MODE2/2352\n"
         "    INDEX 01 00:00:00\n";
}

/* Static storage for the strings the options point into. */
char s_disc_path[4096];
char s_module_path[4096];
/* The game recorded on the entry boot_entry last booted; "" for none. */
char s_selected_game[32];

/** @brief Points the options at a library entry's cue and module path and stamps the entry played.
 *
 * Copies the entry's disc.cue path into static storage and sets
 * opts->disc_path to it, and records the entry's game for
 * spyro1_library_selected_game. When module_path_out is given, writes the entry's
 * modules/<codegen-stamp>/spyro1-recomp.dll (Windows) or .so path and
 * creates that directory; when spyro1_recomp_codegen_stamp reports no stamp
 * (a static build) it writes an empty string instead. Then rewrites the
 * entry's meta.toml with the current time and logs the entry.
 * @param opts Receives disc_path.
 * @param m The entry to boot.
 * @param module_path_out Receives the module path, or NULL for none.
 * @param module_path_cap The capacity of module_path_out.
 * @return 0. */
int boot_entry(openpete_options_t* opts, Meta& m,
               char* module_path_out, size_t module_path_cap) {
    std::snprintf(s_disc_path, sizeof s_disc_path, "%s",
                  (m.dir / "disc.cue").string().c_str());
    opts->disc_path = s_disc_path;
    if (module_path_out && module_path_cap) {
        char stamp[128];
        if (spyro1_recomp_codegen_stamp(stamp, sizeof stamp) == 0) {
#ifdef _WIN32
            fs::path mod = m.dir / "modules" / stamp / "spyro1-recomp.dll";
#else
            fs::path mod = m.dir / "modules" / stamp / "spyro1-recomp.so";
#endif
            std::error_code ec;
            fs::create_directories(mod.parent_path(), ec);
            std::snprintf(module_path_out, module_path_cap, "%s",
                          mod.string().c_str());
        } else {
            module_path_out[0] = 0;   /* static build / no PSXRecomp yet */
        }
    }
    touch_last_played(m);
    std::snprintf(s_selected_game, sizeof s_selected_game, "%s", m.game.c_str());
    OPL_INFO_F("library", "booting library entry %s (%s, %s)",
               m.dir.filename().string().c_str(), m.label.c_str(),
               m.game.empty() ? "unidentified" : m.game.c_str());
    return 0;
}

}  // namespace

/** @brief Resolves the serial and library hash of the image openpete.toml's [disc.*] scopes describe.
 *
 * Runs before the flag parser, before the library has booted anything, and
 * reads few bytes:
 *
 *   - the serial comes from the image's own SYSTEM.CNF (a few sectors), so
 *     it works for a dev --disc the library has never seen;
 *   - the hash is only read back from a library entry's meta.toml, recorded
 *     when the image was ingested; hashing here would digest the whole image
 *     on every launch.
 *
 * Called with the identified disc: the --disc path, or the library entry
 * the launch selected. A disc takes the hash of the library entry whose
 * disc.cue or disc.bin is the same file, so a raw --disc outside the library
 * resolves a serial and no hash, and a [disc.SHA] scope does not apply to
 * it. With no disc, nothing is resolved.
 * @param disc_path The identified disc, or NULL or empty for none.
 * @param serial Receives the disc serial, or NULL.
 * @param serial_cap The capacity of serial.
 * @param sha Receives the library entry's hash, or NULL.
 * @param sha_cap The capacity of sha.
 * @return 0 when a serial or a hash was resolved, else -1. */
extern "C" int spyro1_config_disc_identity(const char* disc_path,
                                           char* serial, size_t serial_cap,
                                           char* sha, size_t sha_cap) {
    if (serial && serial_cap) serial[0] = '\0';
    if (sha && sha_cap) sha[0] = '\0';

    if (!disc_path || !*disc_path) return -1;
    const fs::path image(disc_path);
    /* A library entry, an ingest source or a dev path: if the same content
     * is installed, its recorded hash names the [disc.SHA] scope. */
    std::error_code ec;
    for (auto& e : fs::directory_iterator(library_root(), ec)) {
        Meta m;
        if (!e.is_directory() || !read_meta(e.path(), m)) continue;
        if (fs::equivalent(m.dir / "disc.cue", image, ec) ||
            fs::equivalent(m.dir / "disc.bin", image, ec)) {
            if (sha && sha_cap) std::snprintf(sha, sha_cap, "%s", m.sha.c_str());
            break;
        }
    }

    if (serial && serial_cap)
        (void)openpete_psx_read_disc_serial(disc_path, serial, serial_cap);

    return (serial && serial[0]) || (sha && sha[0]) ? 0 : -1;
}

/** @brief Resolves the disc and recompiled-module path of this boot through the disc library.
 *
 * Three cases, in order:
 *
 *   - opts->ingest_path set: locates the .bin, copies it with
 *     stream_hash_copy on the splash worker task into
 *     library/ingest-tmp-<pid>/ (pid 0 on Windows), and renames that to
 *     library/<first 16 hex digits of the SHA-256>/. An image already in the
 *     library keeps its entry and the temporary copy is removed. A new entry
 *     gets disc.cue and meta.toml, labelled "Spyro the Dragon (NTSC-U)" for
 *     the retail hash and by file name otherwise. The entry then boots.
 *   - opts->disc_path or opts->exe_path set: nothing changes and 0 returns,
 *     leaving the library untouched.
 *   - otherwise the library is scanned. No entries returns 0 for the caller
 *     to report the missing disc. The most recently played entry is picked;
 *     with several entries and the splash UI enabled the version selector
 *     picks instead, and a disc dropped on it re-enters this function as an
 *     ingest. The pick boots.
 *
 * Booting sets opts->disc_path and the module path and updates the entry's
 * last_played (boot_entry).
 * @param opts The options; disc_path is set on boot and ingest_path may be
 *        set from a splash drop.
 * @param module_path_out Receives the module path, empty when there is none;
 *        NULL for none.
 * @param module_path_cap The capacity of module_path_out.
 * @return 0 on a boot, a bypass or an empty library; -1 on an ingest failure,
 *         an unreadable entry, or when the user quits the selector. */
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
        /* Splash worker task: the multi-hundred-MB copy must not run on
         * the UI thread (a window drag would freeze it - see
         * spyro1_splash_run_task). */
        struct CopyTask {
            const fs::path* src; fs::path dst; char* sha; bool ok;
            static int run(void* p) {
                CopyTask* t = static_cast<CopyTask*>(p);
                t->ok = stream_hash_copy(*t->src, t->dst, t->sha);
                return t->ok ? 0 : -1;
            }
        } ct{&bin, tmp / "disc.bin", sha, false};
        spyro1_splash_run_task(&CopyTask::run, &ct);
        if (!ct.ok) {
            std::fprintf(stderr, "openpete: --ingest copy failed\n");
            fs::remove_all(tmp, ec);
            return -1;
        }
        /* Entry dirname = first 16 hex of the sha256 (2^64 space - the
         * full hash lives in meta.toml). Keeps the deepest module-build
         * paths inside Windows' 260-char budget even under long install
         * prefixes. */
        char short_id[17];
        std::memcpy(short_id, sha, 16);
        short_id[16] = 0;
        fs::path entry = root / short_id;
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
            write_meta(entry, label, bin.filename().string(), sha,
                       (long long)time(nullptr), game_of_image(entry / "disc.cue"));
            std::fprintf(stderr, "[library] added %s (%s)\n", label.c_str(), sha);
            /* The library owns this copy from here on - the module is
             * rebuilt from it, never from wherever the image came from.
             * Worth saying once: the user is otherwise left holding two
             * copies of a ~650 MB image with nothing telling them the
             * first one is now redundant. */
            std::fprintf(stderr,
                "[library] your original %s is no longer needed - the copy "
                "in your library is what OpenPete uses from now on\n",
                bin.filename().string().c_str());
        }
        Meta m;
        if (!read_meta(entry, m)) return -1;
        record_game(m);
        return boot_entry(opts, m, module_path_out, module_path_cap);
    }

    /* Explicit --disc: dev/CI path, the library stays out of the way. */
    if (opts->disc_path || opts->exe_path) return 0;

    /* No disc given: boot from the library. */
    std::vector<Meta> entries;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(library_root(), ec)) {
        Meta m;
        if (!e.is_directory() || !read_meta(e.path(), m)) continue;
        record_game(m);
        entries.push_back(m);
    }
    if (entries.empty()) return 0;   /* fall through to the normal
                                      * "--disc is required" diagnostic
                                      * (the splash replaces this) */
    Meta* pick = &entries[0];
    for (auto& m : entries)
        if (m.last_played > pick->last_played) pick = &m;
    if (entries.size() > 1 && spyro1_splash_ui_enabled()) {
        /* Version selector: most-recent first, then the rest. */
        std::vector<Meta*> order;
        order.push_back(pick);
        for (auto& m : entries)
            if (&m != pick) order.push_back(&m);
        std::vector<std::string> label_store;
        for (auto* m : order)
            label_store.push_back(m->game.empty() ? m->label + " (unidentified)" : m->label);
        std::vector<const char*> labels;
        for (auto& l : label_store) labels.push_back(l.c_str());
        if (spyro1_splash_open() == 0) {
            int sel = spyro1_splash_select(labels.data(), (int)labels.size());
            if (sel == -1) return -1;              /* user quit */
            if (sel == -2) {                       /* dropped a new disc */
                opts->ingest_path = spyro1_splash_dropped_path();
                return spyro1_library_resolve(opts, module_path_out,
                                              module_path_cap);
            }
            pick = order[(size_t)sel];
        }
    } else if (entries.size() > 1) {
        std::fprintf(stderr,
                     "[library] %zu versions installed - booting most recent "
                     "'%s'\n", entries.size(), pick->label.c_str());
    }
    return boot_entry(opts, *pick, module_path_out, module_path_cap);
}

/** @brief The game recorded on the library entry spyro1_library_resolve last booted.
 *
 * @return The registered game id, or "" when no entry was booted (a --disc
 *         or --exe-path run, an empty library) or the entry is unidentified. */
extern "C" const char* spyro1_library_selected_game(void) {
    return s_selected_game;
}
