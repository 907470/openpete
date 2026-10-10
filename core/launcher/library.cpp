// core/launcher/library.cpp - the content-addressed disc library shared by
// every game (layout in openpete_launcher.h).
//
// Resolution order at boot: --ingest PATH stream-hashes and copies the image
// in (dedup by content), then boots the entry; else an explicit --disc or
// exe path is the dev path and leaves the library untouched; else the
// library is scanned and the one entry, the version selector's pick, or the
// most recently played boots. After ingest the player's original file is
// never needed again: engine updates rebuild a module from the library's
// own copy.
#include "launcher_internal.h"

#include "openpete_disc_image.h"
#include "openpete_disc_trace.h"
#include "openpete_identify.h" /* openpete_game_for_serial */
#include "openpete_log.h"
#include "openpete_paths.h"    /* library root: portable vs per-user data dir */
#include "openpete_psx.h"      /* openpete_psx_read_disc_serial */
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

#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

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
    if (openpete_paths_data_dir("library", dir, sizeof dir, 0) == 0) return fs::path(dir);
    char exe[2048];
    if (openpete_exe_dir(exe, sizeof exe) == 0) return fs::path(exe) / "library";
    return fs::path("library");
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
 * to the hash. Reports the "COPYING DISC TO YOUR LIBRARY" stage with MiB
 * counts once per 64 MiB copied (openpete_launcher_progress). The hash is
 * finalized after a read failure too, so it covers only the bytes read.
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
        if (seen % (64u << 20) < (1u << 20))
            openpete_launcher_progress("COPYING DISC TO YOUR LIBRARY",
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
 * Recognizes the lines label, source, sha256, game (`key = "value"`, the
 * value taken as the text after the opening quote minus the last character)
 * and last_played (parsed with atoll). Other lines are ignored, and a field
 * with no line keeps its default.
 * @param entry The library entry's directory.
 * @param m Receives the fields; dir is set once meta.toml has opened.
 * @return true when meta.toml opened, else false. */
bool read_meta(const fs::path& entry, Meta& m) {
    std::ifstream f(entry / "meta.toml");
    if (!f) return false;
    m.dir = entry;
    static const struct { const char* key; std::string Meta::*field; } k_str[] = {
        {"label = \"", &Meta::label}, {"source = \"", &Meta::source},
        {"sha256 = \"", &Meta::sha}, {"game = \"", &Meta::game},
    };
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("last_played = ", 0) == 0) m.last_played = atoll(line.c_str() + 14);
        for (const auto& k : k_str) {
            const size_t n = strlen(k.key);
            if (line.size() > n && line.rfind(k.key, 0) == 0)
                m.*k.field = line.substr(n, line.size() - n - 1);
        }
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

/** @brief Report whether an entry is this game's: recorded for it, or unidentified.
 *
 * @param m The entry.
 * @return true when this executable may list and stamp it. */
bool entry_is_ours(const Meta& m) {
    const openpete_launcher_game* g = launcher_game();
    return m.game.empty() || !g || !g->game_id || m.game == g->game_id;
}

/* Static storage for the strings the options point into. */
char s_disc_path[4096];
/* The game recorded on the entry boot_entry last booted; s_booted is 0 until
 * an entry boots. */
char s_selected_game[32];
int  s_booted;

/** @brief Points the options at a library entry's cue and module path and stamps the entry played.
 *
 * Copies the entry's disc.cue path into static storage and sets
 * opts->disc_path to it, records the entry's game for
 * openpete_launcher_selected_game, and writes the module path the game's
 * module_path hook names ("" without one). The entry's last_played is
 * rewritten only for this game's entries: another game's disc is refused by
 * identification and stays unplayed.
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
    const openpete_launcher_game* g = launcher_game();
    if (module_path_out && module_path_cap) {
        module_path_out[0] = 0;
        if (g && g->module_path &&
            g->module_path(m.dir.string().c_str(), module_path_out, module_path_cap) != 0)
            module_path_out[0] = 0;   /* static build: no module in the library */
    }
    if (entry_is_ours(m))
        write_meta(m.dir, m.label, m.source, m.sha, (long long)time(nullptr), m.game);
    std::snprintf(s_selected_game, sizeof s_selected_game, "%s", m.game.c_str());
    s_booted = 1;
    OPL_INFO_F("library", "booting library entry %s (%s, %s)",
               m.dir.filename().string().c_str(), m.label.c_str(),
               m.game.empty() ? "unidentified" : m.game.c_str());
    return 0;
}

/** @brief The label a new entry gets: the game's name for a known disc hash, else the image's file name.
 *
 * @param sha The disc's SHA-256 in hex.
 * @param bin The ingested image.
 * @return The label. */
std::string label_for(const char* sha, const fs::path& bin) {
    const openpete_launcher_game* g = launcher_game();
    for (int i = 0; g && i < g->known_disc_count; i++)
        if (std::strcmp(sha, g->known_discs[i].sha256) == 0) return g->known_discs[i].label;
    return bin.filename().string();
}

/** @brief Copy an image into the library and return its entry directory.
 *
 * The copy runs on a launcher task (a window drag must not freeze it) into
 * library/ingest-tmp-<pid>/ (pid 0 on Windows), which is renamed to
 * library/<first 16 hex digits of the SHA-256>/. An image already in the
 * library keeps its entry and the copy is removed. A new entry gets
 * disc.cue and meta.toml.
 * @param given The image path from --ingest or a drop.
 * @param entry Receives the entry directory.
 * @return true on success; false after logging the failure. */
bool ingest(const char* given, fs::path& entry) {
    fs::path bin = locate_bin(given);
    if (bin.empty()) {
        std::fprintf(stderr, "openpete: --ingest: no .bin found for %s\n", given);
        return false;
    }
    std::error_code ec;
    fs::path root = library_root();
#ifndef _WIN32
    const long long pid = (long long)getpid();
#else
    const long long pid = 0;
#endif
    fs::path tmp = root / ("ingest-tmp-" + std::to_string(pid));
    fs::create_directories(tmp, ec);
    if (ec) {
        std::fprintf(stderr, "openpete: library dir not writable: %s\n", root.string().c_str());
        return false;
    }
    char sha[65];
    std::fprintf(stderr, "[library] copying disc into your library...\n");
    struct CopyTask {
        const fs::path* src; fs::path dst; char* sha; bool ok;
        static int run(void* p) {
            CopyTask* t = static_cast<CopyTask*>(p);
            t->ok = stream_hash_copy(*t->src, t->dst, t->sha);
            return t->ok ? 0 : -1;
        }
    } ct{&bin, tmp / "disc.bin", sha, false};
    openpete_launcher_run_task(&CopyTask::run, &ct);
    if (!ct.ok) {
        std::fprintf(stderr, "openpete: --ingest copy failed\n");
        fs::remove_all(tmp, ec);
        return false;
    }
    /* Entry dirname = first 16 hex of the sha256 (2^64 space; the full hash
     * lives in meta.toml). Keeps the deepest module-build paths inside
     * Windows' 260-char budget even under long install prefixes. */
    entry = root / std::string(sha, 16);
    if (fs::exists(entry / "disc.bin")) {
        std::fprintf(stderr, "[library] already in your library (%s)\n", sha);
        fs::remove_all(tmp, ec);
        return true;
    }
    fs::rename(tmp, entry, ec);
    if (ec) {
        std::fprintf(stderr, "openpete: library install failed: %s\n", ec.message().c_str());
        fs::remove_all(tmp, ec);
        return false;
    }
    write_cue(entry);
    const std::string label = label_for(sha, bin);
    write_meta(entry, label, bin.filename().string(), sha,
               (long long)time(nullptr), game_of_image(entry / "disc.cue"));
    std::fprintf(stderr, "[library] added %s (%s)\n", label.c_str(), sha);
    /* The library owns this copy from here on: the module is rebuilt from
     * it, never from wherever the image came from. Said once, since the
     * player otherwise holds two copies of a ~650 MB image with nothing
     * telling them the first one is now redundant. */
    std::fprintf(stderr,
        "[library] your original %s is no longer needed - the copy "
        "in your library is what OpenPete uses from now on\n",
        bin.filename().string().c_str());
    return true;
}

/** @brief Scan the library for this game's entries and the unidentified ones, recording each entry's game first.
 *
 * Entries recorded for another game are skipped and counted in the log.
 * @return The entries. */
std::vector<Meta> scan_entries() {
    std::vector<Meta> entries;
    std::error_code ec;
    int others = 0;
    for (auto& e : fs::directory_iterator(library_root(), ec)) {
        Meta m;
        if (!e.is_directory() || !read_meta(e.path(), m)) continue;
        record_game(m);
        if (entry_is_ours(m)) entries.push_back(m);
        else others++;
    }
    if (others > 0)
        OPL_INFO_F("library", "%d entr%s of other games not listed", others,
                   others == 1 ? "y" : "ies");
    return entries;
}

/** @brief Pick the entry to boot: the version selector among several when a screen can show, else the most recently played.
 *
 * @param entries The scanned entries; not empty.
 * @param pick Receives the entry.
 * @return 0, LAUNCHER_SEL_QUIT when the player quit, or
 *         LAUNCHER_SEL_DROPPED when a disc was dropped on the selector. */
int pick_entry(std::vector<Meta>& entries, Meta*& pick) {
    pick = &entries[0];
    for (auto& m : entries)
        if (m.last_played > pick->last_played) pick = &m;
    if (entries.size() == 1) return 0;
    /* Version selector: most recent first, then the rest. */
    std::vector<Meta*> order{pick};
    for (auto& m : entries)
        if (&m != pick) order.push_back(&m);
    std::vector<std::string> label_store;
    for (auto* m : order)
        label_store.push_back(m->game.empty() ? m->label + " (unidentified)" : m->label);
    std::vector<const char*> labels;
    for (auto& l : label_store) labels.push_back(l.c_str());
    const int sel = launcher_select(labels.data(), (int)labels.size());
    if (sel == LAUNCHER_SEL_QUIT || sel == LAUNCHER_SEL_DROPPED) return sel;
    if (sel == LAUNCHER_SEL_NOSCREEN)
        std::fprintf(stderr, "[library] %zu versions installed - booting most recent '%s'\n",
                     entries.size(), pick->label.c_str());
    else
        pick = order[(size_t)sel];
    return 0;
}

}  // namespace

int library_resolve(openpete_options_t* opts, char* module_path_out, size_t module_path_cap) {
    if (module_path_out && module_path_cap) module_path_out[0] = 0;

    /* --ingest PATH (or a drop): copy into the library, then boot that entry. */
    if (opts->ingest_path) {
        fs::path entry;
        Meta m;
        if (!ingest(opts->ingest_path, entry) || !read_meta(entry, m)) return LAUNCHER_LIB_FAILED;
        record_game(m);
        return boot_entry(opts, m, module_path_out, module_path_cap);
    }

    /* Explicit --disc: dev/CI path, the library stays out of the way. */
    if (opts->disc_path || opts->exe_path) return 0;

    /* No disc given: boot from the library. An empty one falls through to
     * the caller's drop screen or its missing-disc diagnostic. */
    std::vector<Meta> entries = scan_entries();
    if (entries.empty()) return 0;
    Meta* pick = nullptr;
    const int rc = pick_entry(entries, pick);
    if (rc == LAUNCHER_SEL_QUIT) return LAUNCHER_LIB_QUIT;
    if (rc == LAUNCHER_SEL_DROPPED) {
        opts->ingest_path = launcher_dropped_path();
        return library_resolve(opts, module_path_out, module_path_cap);
    }
    return boot_entry(opts, *pick, module_path_out, module_path_cap);
}

const char* openpete_launcher_selected_game(void) {
    return s_booted ? s_selected_game : nullptr;
}

int openpete_library_disc_identity(const char* disc_path, char* serial, size_t serial_cap,
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
