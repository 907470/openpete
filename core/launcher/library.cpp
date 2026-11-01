// core/launcher/library.cpp - the content-addressed disc library shared by
// every game (layout in openpete_launcher.h).
//
// Resolution order at boot: --ingest PATH stream-hashes and copies the image
// in (dedup by content), then boots the entry; else an explicit --disc or
// exe path is the dev path and leaves the library untouched; else the
// library is scanned and the selector's pick boots: the selector shows for
// several entries, and for any number while no game is named (an executable
// linking several games, so a second game's disc can be dropped); one entry
// of a named game boots without it, and without a screen the most recently
// played boots. After ingest the player's original file is
// never needed again: engine updates rebuild a module from the library's
// own copy.
#include "launcher_internal.h"
#include "launcher_sky.h"

#include "openpete_atomic_file.h"
#include "openpete_build_id.h"
#include "openpete.h"          /* OPENPETE_EXIT_* */
#include "openpete_disc_image.h"
#include "openpete_disc_trace.h"
#include "openpete_identify.h" /* openpete_game_for_serial */
#include "openpete_log.h"
#include "openpete_mods.h"   /* openpete_mods_missing_games */
#include "openpete_paths.h"    /* library root: portable vs per-user data dir */
#include "openpete_psx.h"      /* openpete_psx_read_disc_serial */
#include "sha256.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>



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
 * Replaces the file atomically (synced); a failed write is logged by the
 * helper and leaves the previous file whole. String values are written
 * between double quotes without escaping. An empty game writes no game
 * line.
 * @param entry The library entry's directory.
 * @param label The display name.
 * @param source The file name the image was ingested from.
 * @param sha The disc's SHA-256 in hex.
 * @param last_played The last boot time in seconds since the Unix epoch.
 * @param game The registered game id the image's serial names, or "". */
void write_meta(const fs::path& entry, const std::string& label,
                const std::string& source, const std::string& sha,
                long long last_played, const std::string& game) {
    std::ostringstream f;
    f << "label = \"" << label << "\"\n"
      << "source = \"" << source << "\"\n"
      << "sha256 = \"" << sha << "\"\n"
      << "last_played = " << last_played << "\n";
    if (!game.empty()) f << "game = \"" << game << "\"\n";
    const std::string text = f.str();
    (void)op_atomic_write_file((entry / "meta.toml").string().c_str(), text.data(), text.size());
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
 * by its fixed name disc.bin, and replaces any existing cue atomically
 * (synced).
 * @param entry The library entry's directory. */
void write_cue(const fs::path& entry) {
    static const char kCue[] = "FILE \"disc.bin\" BINARY\n"
                               "  TRACK 01 MODE2/2352\n"
                               "    INDEX 01 00:00:00\n";
    (void)op_atomic_write_file((entry / "disc.cue").string().c_str(), kCue, sizeof kCue - 1);
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
/* The directory of the entry boot_entry last booted. */
char s_entry_dir[4096];
/* The game recorded on the entry boot_entry last booted; s_booted is 0 until
 * an entry boots. */
char s_selected_game[32];
int  s_booted;

/** @brief Write the module path the launcher's game names for a library entry.
 *
 * @param entry_dir The entry's directory.
 * @param out Receives the path, "" when the game names none (a static
 *        build, or no game installed yet); NULL for none.
 * @param cap The capacity of out. */
void entry_module_path(const char* entry_dir, char* out, size_t cap) {
    if (!out || !cap) return;
    out[0] = 0;
    const openpete_launcher_game* g = launcher_game();
    if (g && g->module_path && g->module_path(g, entry_dir, out, cap) != 0)
        out[0] = 0;   /* static build: no module in the library */
}

/** @brief Points the options at a library entry's cue and module path and stamps the entry played.
 *
 * Copies the entry's disc.cue path and directory into static storage and
 * sets opts->disc_path to the cue, records the entry's game for
 * openpete_launcher_selected_game, and writes the module path the game's
 * module_path hook names ("" without one). The entry's last_played is
 * rewritten only for this game's entries, or for every entry while no game
 * is installed: another game's disc is refused by identification and stays
 * unplayed.
 * @param opts Receives disc_path.
 * @param m The entry to boot.
 * @param module_path_out Receives the module path, or NULL for none.
 * @param module_path_cap The capacity of module_path_out.
 * @return 0. */
int boot_entry(openpete_options_t* opts, Meta& m,
               char* module_path_out, size_t module_path_cap) {
    std::snprintf(s_disc_path, sizeof s_disc_path, "%s",
                  (m.dir / "disc.cue").string().c_str());
    std::snprintf(s_entry_dir, sizeof s_entry_dir, "%s", m.dir.string().c_str());
    opts->disc_path = s_disc_path;
    entry_module_path(s_entry_dir, module_path_out, module_path_cap);
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

/** @brief Return the library entry that holds a disc of the same game as an image about to be ingested.
 *
 * The image's game is read from its SYSTEM.CNF before any copy. The
 * library's own copy given again counts as well.
 * @param given The image path from --ingest or a drop.
 * @param bin The image's .bin, read when the given path names no serial.
 * @return The entry's directory, or an empty path when the image's game is
 *         unknown or the library holds no disc of it. */
fs::path held_entry_of(const char* given, const fs::path& bin) {
    std::string game = game_of_image(given);
    if (game.empty()) game = game_of_image(bin);
    if (game.empty()) return {};
    std::error_code ec;
    for (auto& e : fs::directory_iterator(library_root(), ec)) {
        Meta m;
        if (!e.is_directory() || !read_meta(e.path(), m)) continue;
        const std::string g = m.game.empty() ? game_of_image(m.dir / "disc.cue") : m.game;
        if (g == game) return m.dir;
    }
    return {};
}

/** @brief Copy an image into the library and return its entry directory.
 *
 * The copy runs on a launcher task (a window drag must not freeze it) into
 * library/ingest-tmp-<pid>/, which is renamed to
 * library/<first 16 hex digits of the SHA-256>/. An image of a game the
 * library already holds a disc of is refused before the copy, with a
 * message naming the library's entry (held_entry_of). An image whose game
 * cannot be read and whose content is already in the library keeps its
 * entry and the copy is removed. A new entry gets disc.cue and meta.toml.
 * @param given The image path from --ingest or a drop.
 * @param entry Receives the entry directory.
 * @return true on success; false after logging the failure. */
bool ingest(const char* given, fs::path& entry) {
    fs::path bin = locate_bin(given);
    if (bin.empty()) {
        std::fprintf(stderr, "openpete: --ingest: no .bin found for %s\n", given);
        return false;
    }
    if (const fs::path held = held_entry_of(given, bin); !held.empty()) {
        std::fprintf(stderr, "openpete: --ingest: the library already holds a disc of %s (%s); "
                             "%s was not added and the library copy is unchanged\n",
                     game_of_image(held / "disc.cue").c_str(), held.string().c_str(), given);
        OPL_ERROR_F("library", "ingest of %s refused: the library already holds a disc of its game "
                    "in %s", given, held.string().c_str());
        return false;
    }
    std::error_code ec;
    fs::path root = library_root();
    const long long pid = openpete_process_id();
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
 * Runs on the launcher's scan worker when screens show, so it reads no
 * process-wide path state: the caller resolves the root.
 * @param root The library root.
 * @return The entries. */
std::vector<Meta> scan_entries(const fs::path& root) {
    std::vector<Meta> entries;
    std::error_code ec;
    int others = 0;
    for (auto& e : fs::directory_iterator(root, ec)) {
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

/** @brief The selector's label of an entry: its label, marked when unidentified, and prefixed with its game's name when the launcher names games and the label does not already start with it.
 *
 * @param m The entry.
 * @return The label. */
std::string entry_label(const Meta& m) {
    if (m.game.empty()) return m.label + " (unidentified)";
    const openpete_launcher_game* g = launcher_game();
    const char* name = (g && g->game_name) ? g->game_name(m.game.c_str()) : nullptr;
    if (!name || !*name || m.label.rfind(name, 0) == 0) return m.label;
    return std::string(name) + " - " + m.label;
}

/** @brief Pick the entry to boot: the --game entry, else the selector, else the most recently played.
 *
 * A launch whose --game names a game (the launcher's game, or any game
 * while the launcher names none) boots that game's most recently played
 * identified entry without the selector, and with none leaves the pick
 * empty for the caller. Any other launch shows the selector, for one entry
 * too. The scripted drop (launcher_scripted_drop) is taken in every case,
 * ahead of any pick.
 * @param opts The options; game_selector is the --game value.
 * @param entries The scanned entries; not empty.
 * @param pick Receives the entry, or NULL when --game names a game with no
 *        entry.
 * @param allow_direct false to show the selector whatever --game names (the
 *        page after a --game entry could not boot).
 * @return 0, LAUNCHER_SEL_QUIT when the player quit, or
 *         LAUNCHER_SEL_DROPPED when a disc was dropped on the selector. */
int pick_entry(const openpete_options_t* opts, std::vector<Meta>& entries, Meta*& pick,
               bool allow_direct = true) {
    const bool direct = allow_direct && opts->game_selector && opts->game_selector[0];
    const std::string id = !direct ? "" : launcher_neutral() ? opts->game_selector
                                                              : launcher_game()->game_id;
    pick = nullptr;
    for (auto& m : entries)
        if ((!direct || m.game == id) && (!pick || m.last_played > pick->last_played)) pick = &m;
    if (direct) return launcher_scripted_drop() ? LAUNCHER_SEL_DROPPED : 0;
    /* The game page's entries, most recently played first: each game's box
     * goes to its first entry. */
    std::vector<Meta*> order{pick};
    for (auto& m : entries)
        if (&m != pick) order.push_back(&m);
    std::stable_sort(order.begin() + 1, order.end(),
                     [](const Meta* a, const Meta* b) { return a->last_played > b->last_played; });
    std::vector<std::string> label_store;
    for (auto* m : order)
        label_store.push_back(entry_label(*m));
    std::vector<const char*> labels, games;
    for (auto& l : label_store) labels.push_back(l.c_str());
    for (auto* m : order) games.push_back(m->game.c_str());
    const int sel = launcher_select(labels.data(), games.data(), (int)labels.size());
    if (sel == LAUNCHER_SEL_QUIT || sel == LAUNCHER_SEL_DROPPED) return sel;
    if (sel == LAUNCHER_SEL_NOSCREEN)
        std::fprintf(stderr, "[library] %zu entr%s, no screen - booting the most recently played '%s'\n",
                     entries.size(), entries.size() == 1 ? "y" : "ies", pick->label.c_str());
    else
        pick = order[(size_t)sel];
    return 0;
}

/** @brief Log each enabled mod's required game that has no disc in the library; the game page shows no mod-requirement message.
 *
 * @param opts The options naming the mods root and the enabled list. */
void note_missing_games(const openpete_options_t* opts) {
    openpete_mods_missing_games(
        opts, launcher_game() ? launcher_game()->game_id : nullptr,
        [](void* ud, const char* mod_id, const char* game) {
            (void)ud;
            OPL_WARN_F("library", "mod %s requires %s, which has no disc in the library", mod_id, game);
        },
        nullptr);
}

/** @brief Return this executable's build id: the configured version joined with the executable's size and modification time.
 *
 * A relinked executable gets a new id even when the configured version is
 * unchanged (a rebuild without a reconfigure).
 * @return The id. */
std::string build_id() {
    static std::string id;
    if (!id.empty()) return id;
    char exe_buf[4096];
    const fs::path exe = openpete_exe_path(exe_buf, sizeof exe_buf) == 0 ? fs::path(exe_buf) : fs::path();
    std::error_code ec;
    const uintmax_t size = exe.empty() ? 0 : fs::file_size(exe, ec);
    const long long mtime = exe.empty() ? 0 : (long long)fs::last_write_time(exe, ec).time_since_epoch().count();
    id = std::string(openpete_build_version()) + "-" + std::to_string(size) + "-" + std::to_string(mtime);
    return id;
}

/** @brief The failed-build marker's path in an entry, beside its meta.toml.
 *
 * @param entry_dir The entry's directory.
 * @return The path. */
fs::path marker_path(const char* entry_dir) { return fs::path(entry_dir) / "build-failed.toml"; }

/** @brief Return the build id a failed-build marker names.
 *
 * @param entry_dir The entry's directory.
 * @return The id, or "" when the entry holds no marker. */
std::string marker_build(const char* entry_dir) {
    std::ifstream f(marker_path(entry_dir));
    std::string line;
    while (f && std::getline(f, line))
        if (line.rfind("build = \"", 0) == 0 && line.size() > 10) return line.substr(9, line.size() - 10);
    return {};
}

intptr_t s_lock_handle = 0;
bool s_lock_held = false;
/* The launch-time jobs are queued once per process. */
bool s_jobs_queued = false;

/** @brief Return the neutral descriptor's per-game entry of a game id.
 *
 * @param game The game id.
 * @return The entry, or NULL when the launcher names a game or no linked game has the id. */
const openpete_launcher_game* game_entry_of(const std::string& game) {
    const openpete_launcher_game* n = launcher_game();
    if (!n || n->game_id || game.empty()) return nullptr;
    for (int i = 0; i < n->game_count; i++)
        if (n->games[i] && game == n->games[i]->game_id) return n->games[i];
    return nullptr;
}

/* A module build the launcher runs for the pick, on a launcher task. */
struct PickBuild {
    const openpete_launcher_game* g;
    const char* disc;
    const char* out;
    /** @brief Run the build for openpete_launcher_run_task_exclusive.
     *
     * @param p The PickBuild.
     * @return The build's status. */
    static int run(void* p) {
        PickBuild* t = static_cast<PickBuild*>(p);
        return t->g->build_module(t->g, t->disc, t->out);
    }
};

/** @brief Build an entry's module when its game's stamped path holds no file: on a launcher task with the pipeline compile paused, its progress drawn while screens show, inline without them.
 *
 * A failed build writes the entry's failed-build marker; a completed one
 * clears it. The game must be installed.
 * @param g The game's per-game entry, with a build pipeline.
 * @param m The entry.
 * @return 0 when the module is there or built, -1 when the build failed. */
int build_entry_module(const openpete_launcher_game* g, const Meta& m) {
    char path[4096];
    const std::string entry = m.dir.string();
    if (g->module_path(g, entry.c_str(), path, sizeof path) != 0) return 0;
    std::error_code ec;
    if (fs::exists(path, ec)) return 0;
    const std::string disc = (m.dir / "disc.cue").string();
    OPL_INFO_F("launcher", "launcher-side module build of %s for entry %s: %s", g->game_id,
               m.dir.filename().string().c_str(), path);
    PickBuild pb{g, disc.c_str(), path};
    const int rc = openpete_launcher_run_task_exclusive(&PickBuild::run, &pb);
    if (rc == 0) {
        library_marker_clear(entry.c_str());
        return 0;
    }
    library_marker_write(entry.c_str(), launcher_last_stage());
    OPL_ERROR_F("launcher", "the module of %s could not be built (step: %s); see the log file",
                g->game_id, launcher_last_stage()[0] ? launcher_last_stage() : "start");
    return -1;
}

/** @brief Prepare a picked or ingested entry's game: stop other games' jobs, wait for its own, install it, then bring its asset cache up to date alongside its module build.
 *
 * The asset job (launcher_assets_start) starts first, on the ingest thread
 * while screens show (inline before the build without them), and is joined
 * after the build (build_entry_module). An entry whose failed-build marker
 * names this build is admitted by the caller.
 * @param m The entry; its game must be recorded.
 * @param context What prepares it: "ingest" or "pick".
 * @return 0 when the game can start, -1 when its module could not be built. */
int prepare_entry(const Meta& m, const char* context) {
    const openpete_launcher_game* g = game_entry_of(m.game);
    if (!g) return 0;   /* unidentified or not linked: the entry's identification refuses it */
    launcher_jobs_stop_other(g->game_id, true);
    launcher_jobs_finish_game(g->game_id);
    const bool build = g->build_module && g->module_path;
    if (!build && !g->update_assets) return 0;
    g->install(g);
    launcher_jobs_token(1);
    launcher_assets_start(g, (m.dir / "disc.cue").string().c_str(), m.sha.c_str(), context);
    const int rc = build ? build_entry_module(g, m) : 0;
    launcher_assets_wait();
    launcher_jobs_token(-1);
    return rc;
}

/** @brief Offer the most recently played identified entry's game for the 3D title (launcher_title_offer).
 *
 * @param entries The scanned entries.
 * @param n The neutral descriptor listing the per-game entries. */
void offer_recent_title(const std::vector<Meta>& entries, const openpete_launcher_game* n) {
    const Meta* recent = nullptr;
    for (const auto& m : entries)
        if (!m.game.empty() && (!recent || m.last_played > recent->last_played)) recent = &m;
    for (int i = 0; recent && i < n->game_count; i++)
        if (n->games[i] && n->games[i]->game_id && recent->game == n->games[i]->game_id)
            launcher_title_offer(n->games[i], true, "the most recently played entry's game");
}

/** @brief Queue the launch-time job of every library game, once per process: the most recently played entry of each linked game with a first-boot pipeline or launcher assets, its module check skipped when its failed-build marker names this build.
 *
 * First offers the most recently played identified entry's game for the 3D
 * title (launcher_title_offer), which shows it from that game's cache.
 * @param entries The scanned entries. */
void queue_launch_jobs(const std::vector<Meta>& entries) {
    if (s_jobs_queued) return;
    s_jobs_queued = true;
    const openpete_launcher_game* n = launcher_game();
    if (!n || n->game_id) return;
    offer_recent_title(entries, n);
    for (int i = 0; i < n->game_count; i++) {
        const openpete_launcher_game* g = n->games[i];
        if (!g || (!g->build_module && !g->update_assets)) continue;
        const Meta* best = nullptr;
        for (const auto& m : entries)
            if (m.game == g->game_id && (!best || m.last_played > best->last_played)) best = &m;
        if (!best) continue;
        const std::string dir = best->dir.string();
        bool module = g->build_module != nullptr;
        if (module && library_marker_current(dir.c_str())) {
            OPL_INFO_F("launcher", "launch-time job skipped: entry %s of %s holds a failed-build "
                       "marker of this build", best->dir.filename().string().c_str(), g->game_id);
            module = false;
        }
        if (!module && !g->update_assets) continue;
        launcher_job_add(g, dir.c_str(), (best->dir / "disc.cue").string().c_str(), best->sha.c_str(), module);
    }
}

/** @brief Ingest a disc dropped on a screen, then build its game's module, and return to the screens.
 *
 * The game page opens focused on the disc's game (launcher_focus_game_set).
 * A running job of another game is stopped at the copy's end and requeued.
 * @param path The dropped image. */
void ingest_drop(const char* path) {
    fs::path entry;
    Meta m;
    if (!ingest(path, entry) || !read_meta(entry, m)) {
        OPL_ERROR_F("library", "the dropped image %s could not be added to the library", path);
        return;
    }
    record_game(m);
    launcher_focus_game_set(m.game.c_str());
    launcher_jobs_stop_other(m.game.c_str(), true);
    (void)prepare_entry(m, "ingest");
}

/* pick_pass results besides 0 and the LAUNCHER_LIB_* values. */
constexpr int kPassAgain = 1;   /* show the library again */
constexpr int kPassBoot = 2;    /* boot the chosen entry */

/** @brief Admit a picked entry: refuse or retry one whose failed-build marker names this build, then prepare it (prepare_entry).
 *
 * Without screens a current marker or a failed build ends the run. With
 * screens a --game entry with a current marker shows the library, a pick
 * from the selector retries (the marker is removed), and a failed build
 * shows the library again.
 * @param m The entry.
 * @param screens Whether screens show.
 * @param direct Whether --game picked it; cleared when the library shows.
 * @return kPassBoot, kPassAgain or LAUNCHER_LIB_BUILD_FAILED. */
int admit_pick(const Meta& m, bool screens, bool& direct) {
    const std::string dir = m.dir.string();
    if (library_marker_current(dir.c_str())) {
        OPL_ERROR_F("launcher", "entry %s of %s holds a failed module build of this build (%s); "
                    "the log file names the failure", m.dir.filename().string().c_str(),
                    m.game.c_str(), marker_path(dir.c_str()).string().c_str());
        if (!screens) return LAUNCHER_LIB_BUILD_FAILED;
        if (direct) { direct = false; return kPassAgain; }   /* --game: the library */
        library_marker_clear(dir.c_str());                   /* a pick retries */
    }
    if (prepare_entry(m, "pick") == 0) return kPassBoot;
    if (!screens) return LAUNCHER_LIB_BUILD_FAILED;
    direct = false;
    return kPassAgain;
}

/** @brief One pass over the library while no game is installed: scan, queue the launch-time jobs once (running them inline without screens), pick an entry and admit it (admit_pick).
 *
 * A drop on a screen is ingested (ingest_drop) and the library shows again.
 * @param opts The options.
 * @param screens Whether screens show.
 * @param direct Whether --game picks; cleared when the library shows instead.
 * @param chosen Receives the entry to boot.
 * @return kPassBoot, kPassAgain, 0 when no entry boots, or a LAUNCHER_LIB_* value. */
int pick_pass(openpete_options_t* opts, bool screens, bool& direct, Meta& chosen) {
    struct ScanTask {
        fs::path root;
        std::vector<Meta> entries;
        /** @brief Scan the library for the launcher's game's entries, stamping the scan's start and end.
         *
         * @param p The ScanTask.
         * @return 0. */
        static int run(void* p) {
            ScanTask* t = static_cast<ScanTask*>(p);
            launcher_stamp(LAUNCHER_EV_SCAN_START);
            t->entries = scan_entries(t->root);
            launcher_stamp(LAUNCHER_EV_SCAN_END);
            return 0;
        }
    } scan{library_root(), {}};
    const int waited = launcher_await_scan(&ScanTask::run, &scan);
    for (const Meta& m : scan.entries) launcher_sky_seed(m.game.c_str());
    queue_launch_jobs(scan.entries);
    if (!screens) launcher_jobs_run_inline();
    if (waited == LAUNCHER_SEL_QUIT) return LAUNCHER_LIB_QUIT;
    if (waited == LAUNCHER_SEL_DROPPED) { ingest_drop(launcher_dropped_path()); return kPassAgain; }
    if (scan.entries.empty()) return 0;
    note_missing_games(opts);
    Meta* pick = nullptr;
    const int rc = pick_entry(opts, scan.entries, pick, direct);
    if (rc == LAUNCHER_SEL_QUIT) return LAUNCHER_LIB_QUIT;
    if (rc == LAUNCHER_SEL_DROPPED || (!pick && screens)) {
        if (rc == LAUNCHER_SEL_DROPPED) ingest_drop(launcher_dropped_path());
        direct = false;
        return kPassAgain;
    }
    if (!pick) return 0;   /* --game names a game with no entry: the entry's diagnostic */
    chosen = *pick;
    return admit_pick(chosen, screens, direct);
}

/** @brief Resolve the pick made before any game is installed: the --ingest image, the --game entry or the selector's, each prepared (prepare_entry) before it boots.
 *
 * Without screens the launch-time jobs run inline before the pick (pick_pass).
 * @param opts The options.
 * @param module_path_out Receives the module path, "" for none.
 * @param module_path_cap Its capacity.
 * @return 0 on a boot, a bypass or no entry, else a LAUNCHER_LIB_* value. */
int resolve_pick(openpete_options_t* opts, char* module_path_out, size_t module_path_cap) {
    const bool screens = launcher_screens();
    if (opts->ingest_path) {
        fs::path entry;
        Meta m;
        if (!ingest(opts->ingest_path, entry) || !read_meta(entry, m)) return LAUNCHER_LIB_FAILED;
        record_game(m);
        if (prepare_entry(m, "ingest") == 0) return boot_entry(opts, m, module_path_out, module_path_cap);
        if (!screens) return LAUNCHER_LIB_BUILD_FAILED;
        opts->ingest_path = nullptr;   /* the library, with the entry's failed build */
    }
    if (opts->disc_path || opts->exe_path) return 0;
    bool direct = true;
    for (;;) {
        Meta chosen;
        const int rc = pick_pass(opts, screens, direct, chosen);
        if (rc == kPassAgain) continue;
        if (rc == kPassBoot) return boot_entry(opts, chosen, module_path_out, module_path_cap);
        return rc;
    }
}

}  // namespace

bool library_marker_current(const char* entry_dir) {
    return marker_build(entry_dir) == build_id();
}

void library_marker_write(const char* entry_dir, const char* step) {
    const std::string text = "build = \"" + build_id() + "\"\nstep = \"" +
                             std::string(step && *step ? step : "start") + "\"\n";
    (void)op_atomic_write_file(marker_path(entry_dir).string().c_str(), text.data(), text.size());
    OPL_INFO_F("library", "failed-build marker written: %s (build %s)",
               marker_path(entry_dir).string().c_str(), build_id().c_str());
}

void library_marker_clear(const char* entry_dir) {
    std::error_code ec;
    if (fs::remove(marker_path(entry_dir), ec))
        OPL_INFO_F("library", "failed-build marker removed: %s", marker_path(entry_dir).string().c_str());
}

void openpete_launcher_mark_build_failed(void) {
    if (!s_booted) return;
    library_marker_write(s_entry_dir, launcher_last_stage());
}

int openpete_launcher_lock(void) {
    if (s_lock_held) return 0;
    const fs::path root = library_root();
    std::error_code ec;
    fs::create_directories(root, ec);
    const fs::path lock = root / ".lock";
    if (const int rc = openpete_paths_lock(lock.string().c_str(), &s_lock_handle); rc != 0) return rc;
    s_lock_held = true;
    OPL_INFO_F("library", "library lock taken: %s", lock.string().c_str());
    return 0;
}

void library_lock_release(void) {
    if (!s_lock_held) return;
    openpete_paths_unlock(s_lock_handle);
    s_lock_held = false;
    OPL_INFO_F("library", "%s", "library lock released");
}

void library_ingest_drop(const char* path) { ingest_drop(path); }

int library_resolve(openpete_options_t* opts, char* module_path_out, size_t module_path_cap) {
    if (module_path_out && module_path_cap) module_path_out[0] = 0;

    /* An entry booted before the game was installed: the installed game's
     * entry boots it again, with the module path that game names for it. */
    if (s_booted && !opts->disc_path && !opts->exe_path) {
        opts->disc_path = s_disc_path;
        entry_module_path(s_entry_dir, module_path_out, module_path_cap);
        OPL_INFO_F("library", "booting the picked entry %s (%s)",
                   fs::path(s_entry_dir).filename().string().c_str(),
                   s_selected_game[0] ? s_selected_game : "unidentified");
        return 0;
    }

    /* Before any game is installed: the pick, which prepares its entry. */
    if (launcher_neutral()) return resolve_pick(opts, module_path_out, module_path_cap);

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
    struct ScanTask {
        fs::path root;
        std::vector<Meta> entries;
        /** @brief Scan the library for the launcher's game's entries, stamping the scan's start and end.
         *
         * @param p The ScanTask.
         * @return 0. */
        static int run(void* p) {
            ScanTask* t = static_cast<ScanTask*>(p);
            launcher_stamp(LAUNCHER_EV_SCAN_START);
            t->entries = scan_entries(t->root);
            launcher_stamp(LAUNCHER_EV_SCAN_END);
            return 0;
        }
    } scan{library_root(), {}};
    const int waited = launcher_await_scan(&ScanTask::run, &scan);
    if (waited == LAUNCHER_SEL_QUIT) return LAUNCHER_LIB_QUIT;
    if (waited == LAUNCHER_SEL_DROPPED) {
        opts->ingest_path = launcher_dropped_path();
        return library_resolve(opts, module_path_out, module_path_cap);
    }
    std::vector<Meta>& entries = scan.entries;
    if (entries.empty()) return 0;
    note_missing_games(opts);
    Meta* pick = nullptr;
    const int rc = pick_entry(opts, entries, pick);
    if (rc == LAUNCHER_SEL_QUIT) return LAUNCHER_LIB_QUIT;
    if (rc == LAUNCHER_SEL_DROPPED) {
        opts->ingest_path = launcher_dropped_path();
        return library_resolve(opts, module_path_out, module_path_cap);
    }
    if (!pick) return 0;
    return boot_entry(opts, *pick, module_path_out, module_path_cap);
}

const char* openpete_launcher_build_id(void) {
    static const std::string id = build_id();
    return id.c_str();
}

const char* openpete_launcher_selected_game(void) {
    return s_booted ? s_selected_game : nullptr;
}

int openpete_library_game_disc(const char* game, char* cue, size_t cue_cap) {
    if (!game || !*game || !cue || !cue_cap) return -1;
    std::error_code ec;
    fs::path best;
    long long best_played = -1;
    for (auto& e : fs::directory_iterator(library_root(), ec)) {
        Meta m;
        if (!e.is_directory() || !read_meta(e.path(), m)) continue;
        const fs::path c = m.dir / "disc.cue";
        if (!fs::exists(c, ec)) continue;
        const std::string g = m.game.empty() ? game_of_image(c) : m.game;
        if (g != game || m.last_played <= best_played) continue;
        best = c;
        best_played = m.last_played;
    }
    if (best.empty()) return -1;
    std::snprintf(cue, cue_cap, "%s", best.string().c_str());
    return 0;
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
