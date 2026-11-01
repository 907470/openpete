// games/common/recomp_build.cpp - the first-boot module build.
//
// Turns a disc into <tag>-recomp.{so,dll} with no python and no discovery:
//
//   extract (in-process, the running game's extract_recomp_inputs)
//   ──► PSXRecomp once per binary (single pass, staged converged manifest)
//   ──► cc -O2 -fPIC fan-out ──► link
//
// Every per-game fact comes from the staged manifest (each overlay's name,
// id, load address and reference hash; whether the main EXE takes channel
// sites; the base-redirect window every binary takes) or from the running
// game's build record (game_build.h): the module name, and the overlay
// window of the game's recomp.conf OVERLAY_RANGE.
//
// Every compiler and PSXRecomp command runs from an argument vector, never
// through a shell (openpete_host_spawn_argv_stoppable): a stop of the
// launcher job running the build kills the command and every process it
// started.
//
// Inputs staged next to the exe by the shared-linkage build
// (CMake): recomp/manifest/<game id>/ (converged funcs lists +
// manifest.toml + psyq.toml + channel_sites.txt; one directory per game,
// since one executable stages every game's), recomp/src/ (the 5-file module source
// kit), recomp/tools/PSXRecomp (staged by the build, shipped by the
// packages). The host compiler is
// the bundled clang when packaged, else $OPENPETE_CC or `cc`.
//
// Cross-platform: POSIX uses the bundled clang / $OPENPETE_CC / cc;
// Windows requires the bundled llvm-mingw next to the exe
// (recomp/tools/llvm-mingw) and links against the staged exe-export
// implib (recomp/tools/libopenpete-exe-exports.a).
#include "openpete_launcher.h"
#include "openpete_atomic_file.h"
#include "host_spawn.h"
#include "openpete_error_ids.h"
#include "game_build.h"
#include "recomp_attach.h"
#include "openpete_log.h"
#include "openpete_config.h"
#include "sha256.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <limits.h>
#endif


/** @brief Report a build stage to the launcher's progress screen (a no-op without one).
 *
 * @param stage The stage text.
 * @param done Completed units.
 * @param total Total units, or 0 for no bar. */
static void progress(const char* stage, int done, int total) {
    openpete_launcher_progress(stage, done, total);
}

namespace fs = std::filesystem;

namespace {

struct Overlay {
    std::string name;
    int magic = 0;
    std::string load_addr;   /* the manifest's load_addr, as written */
    std::string sha256;      /* of the reference binary (manifest) */
};

std::string exe_dir() {
#ifdef _WIN32
    char exe[4096];
    DWORD len = GetModuleFileNameA(NULL, exe, sizeof exe);
    if (len > 0 && len < sizeof exe) {
        for (char* p = exe; *p; ++p) if (*p == '\\') *p = '/';
        if (char* slash = strrchr(exe, '/')) { *slash = 0; return exe; }
    }
#else
    char exe[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (len > 0) {
        exe[len] = 0;
        if (char* slash = strrchr(exe, '/')) { *slash = 0; return exe; }
    }
#endif
    return ".";
}

/** @brief Join an argument vector with spaces, for a log line.
 *
 * @param args The arguments.
 * @return The joined text. */
std::string joined(const std::vector<std::string>& args) {
    std::string out;
    for (const std::string& a : args) out += (out.empty() ? "" : " ") + a;
    return out;
}

/** @brief Run one compiler or PSXRecomp command from an argument vector, unless the launcher job running the build was asked to stop.
 *
 * The command runs stoppable and below normal priority
 * (openpete_host_spawn_argv_stoppable_flags): a stop read before the start
 * leaves it unstarted, a stop while it runs kills it and every process it
 * started, and the launcher and the desktop keep their CPU share.
 * @param args The program and its arguments.
 * @param log_path The file its stdout and stderr go to, or NULL to inherit
 *        the launcher's.
 * @return true when the command ran and exited 0. */
bool run(const std::vector<std::string>& args, const char* log_path = nullptr) {
    if (openpete_launcher_job_cancelled()) return false;
    std::vector<const char*> argv;
    for (const std::string& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    const int rc = openpete_host_spawn_argv_stoppable_flags(argv.data(), log_path, nullptr, 0,
                                                            OPENPETE_HOST_SPAWN_BELOW_NORMAL);
    if (rc == OPENPETE_HOST_SPAWN_STOPPED) return false;
    if (rc != 0)
        OPL_ERROR_F("recomp-build", "command failed (%d): %s%s%s %s", rc, joined(args).c_str(),
                    log_path ? "; its output is in " : "", log_path ? log_path : "",
                    openpete_error_ref(OP_E_MODULE));
    return rc == 0;
}

/* FNV-1a 64 over a file - the v1 identity/codegen stamp (labeled as
 * such; D4's release stamps may upgrade the hash, which just changes
 * the stamp string and invalidates caches once). */
uint64_t fnv1a_file(const fs::path& p, uint64_t h = 0xcbf29ce484222325ull) {
    std::ifstream f(p, std::ios::binary);
    char buf[65536];
    while (f.read(buf, sizeof buf) || f.gcount() > 0) {
        for (std::streamsize i = 0; i < f.gcount(); i++) {
            h ^= (uint8_t)buf[i];
            h *= 0x100000001b3ull;
        }
        if (!f) break;
    }
    return h;
}

/* Fold a whole directory tree into `h`. Files are visited in sorted
 * relative-path order, never the filesystem's, so the hash is a property
 * of the content rather than of how the tree happened to be laid out;
 * the relative path is mixed in alongside the bytes, so a rename counts
 * as a change too. */
uint64_t fnv1a_tree(const fs::path& root, uint64_t h) {
    std::error_code ec;
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(root, ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec)) files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files) {
        const std::string rel = f.lexically_relative(root).generic_string();
        for (char c : rel) { h ^= (uint8_t)c; h *= 0x100000001b3ull; }
        h = fnv1a_file(f, h);
    }
    return h;
}

std::string hex64(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
    return b;
}

/* Replace every whole-word occurrence of `from` with `to` in file `p`
 * (the dispatch-rename regen does with sed); the rewrite replaces the file
 * atomically (synced), so a failure leaves the previous source whole. */
bool rename_token(const fs::path& p, const std::string& from, const std::string& to) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::string s((std::istreambuf_iterator<char>(in)), {});
    in.close();
    std::string out_s;
    out_s.reserve(s.size());
    size_t i = 0;
    auto is_word = [](char c) { return isalnum((unsigned char)c) || c == '_'; };
    while (i < s.size()) {
        if (!s.compare(i, from.size(), from) &&
            (i == 0 || !is_word(s[i - 1])) &&
            (i + from.size() >= s.size() || !is_word(s[i + from.size()]))) {
            out_s += to;
            i += from.size();
        } else {
            out_s += s[i++];
        }
    }
    return op_atomic_write_file(p.string().c_str(), out_s.data(), out_s.size()) == 0;
}

/* The facts a converged manifest.toml states ahead of its [[overlay]] tables. */
struct ManifestHeader {
    std::string main_exe_sha;  /* the main EXE's reference sha256 */
    bool channel_sites = false;  /* the main EXE takes --channel-sites */
    std::string window_opt;  /* `--sony-image-window=<lo>:<hi>`, passed to every binary; empty: none */
};

/** @brief Record one line of the manifest's header (the lines before its first [[overlay]]).
 *
 * @param line the manifest line
 * @param h receives the fact the line states; a line stating none leaves it unchanged */
void parse_header_line(const std::string& line, ManifestHeader& h) {
    if (line.rfind("sha256 = \"", 0) == 0)
        h.main_exe_sha = line.substr(10, line.size() - 11);
    else if (line == "channel_sites = true")
        h.channel_sites = true;
    else if (line.rfind("sony_image_window = \"", 0) == 0)
        h.window_opt = "--sony-image-window=" + line.substr(21, line.size() - 22);
}

/** @brief Read a converged manifest.toml: its header facts and its overlays.
 *
 * @param toml the manifest path
 * @param out receives one Overlay per [[overlay]] table, in file order
 * @param header receives the facts the header lines state
 * @return false when the file is unreadable, has no overlay, or an overlay
 *         lacks a load_addr */
bool parse_manifest(const fs::path& toml, std::vector<Overlay>& out,
                    ManifestHeader& header) {
    std::ifstream f(toml);
    if (!f) return false;
    std::string line;
    Overlay cur;
    bool in_overlay = false;
    auto flush = [&]() {
        if (in_overlay && !cur.name.empty()) out.push_back(cur);
        cur = Overlay{};
    };
    while (std::getline(f, line)) {
        if (line == "[[overlay]]") { flush(); in_overlay = true; continue; }
        if (!in_overlay) {
            parse_header_line(line, header);
            continue;
        }
        if (line.rfind("name = \"", 0) == 0)
            cur.name = line.substr(8, line.size() - 9);
        else if (line.rfind("magic = ", 0) == 0)
            cur.magic = std::atoi(line.c_str() + 8);
        else if (line.rfind("load_addr = ", 0) == 0)
            cur.load_addr = line.substr(12);
        else if (line.rfind("sha256 = \"", 0) == 0)
            cur.sha256 = line.substr(10, line.size() - 11);
    }
    flush();
    for (const Overlay& o : out)
        if (o.load_addr.empty()) return false;
    return !out.empty();
}

std::string sha256_file_hex(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    openpete_sha256_ctx c;
    openpete_sha256_init(&c);
    char buf[65536];
    while (f.read(buf, sizeof buf) || f.gcount() > 0) {
        openpete_sha256_update(&c, buf, (size_t)f.gcount());
        if (!f) break;
    }
    uint8_t d[32];
    char hex[65];
    openpete_sha256_final(&c, d);
    openpete_sha256_hex(d, hex);
    return hex;
}

/** @brief Report whether an overlay's dispatch file defines its deferred-spin answer, psx_quiet_<name>.
 *
 * @param dispatch The overlay's renamed dispatch file.
 * @param name The overlay's name.
 * @return true when a line opens with the function's definition. */
bool defines_quiet(const fs::path& dispatch, const std::string& name) {
    std::ifstream in(dispatch, std::ios::binary);
    const std::string head = "int psx_quiet_" + name + "(";
    for (std::string line; std::getline(in, line);)
        if (!line.compare(0, head.size(), head)) return true;
    return false;
}

/** @brief Write spyro_overlay_table.c from the registry and the overlays' dispatch files.
 *
 * The table is derivable from the registry, so the boot build emits it
 * rather than shipping it, proving manifest completeness. An overlay whose
 * dispatch file answers a deferred spin gets the quiet column; any other
 * entry leaves it NULL. The file is replaced atomically (synced).
 * @param out The table file.
 * @param gen The directory holding the renamed dispatch files.
 * @param ovls The registry's overlays, in table order.
 * @return false when the file cannot be written. */
bool emit_overlay_table(const fs::path& out, const fs::path& gen, const std::vector<Overlay>& ovls) {
    std::ostringstream f;
    f << "/* AUTO-GENERATED by the first-boot module build - do not edit. */\n"
         "#include \"spyro_overlay_table.h\"\n\n";
    std::vector<bool> quiet;
    for (auto& o : ovls) quiet.push_back(defines_quiet(gen / (o.name + ".bin_dispatch.c"), o.name));
    for (size_t i = 0; i < ovls.size(); i++) {
        f << "extern int psx_dispatch_" << ovls[i].name << "(CPUState* cpu, uint32_t addr);\n";
        if (quiet[i])
            f << "extern int psx_quiet_" << ovls[i].name << "(uint32_t addr, uint32_t* cost);\n";
    }
    f << "\nconst spyro_overlay_entry_t spyro_overlay_table[] = {\n";
    for (size_t i = 0; i < ovls.size(); i++) {
        f << "    { " << ovls[i].magic << ", \"" << ovls[i].name << "\", psx_dispatch_"
          << ovls[i].name;
        if (quiet[i]) f << ", psx_quiet_" << ovls[i].name;
        f << " },\n";
    }
    f << "};\nconst size_t spyro_overlay_table_count = "
         "sizeof(spyro_overlay_table) / sizeof(spyro_overlay_table[0]);\n"
         /* The overlay window of the game's recomp.conf (OVERLAY_RANGE). */
         "const uint32_t spyro_overlay_window_lo = "
      << openpete_game_build()->overlay_window_lo << "u;\n"
         "const uint32_t spyro_overlay_window_hi = "
      << openpete_game_build()->overlay_window_hi << "u;\n";
    const std::string text = f.str();
    return op_atomic_write_file(out.string().c_str(), text.data(), text.size()) == 0;
}

#ifdef _WIN32
#define PSXRECOMP_NAME "PSXRecomp.exe"
#else
#define PSXRECOMP_NAME "PSXRecomp"
#endif

/** @brief Return the PSXRecomp path: $OPENPETE_PSXRECOMP, else recomp/tools/ under base.
 *
 * The build stages recomp/tools/ beside the executable (the Windows tree
 * builds its own PSXRecomp.exe there) and the packages ship it, so no
 * repository path is searched.
 * @param base The install directory.
 * @return The path, or "" when neither names a file. */
std::string find_psxrecomp(const fs::path& base) {
    if (const char* env = openpete_config_env("OPENPETE_PSXRECOMP")) return env;
    fs::path staged = base / "recomp" / "tools" / PSXRECOMP_NAME;
    if (fs::exists(staged)) return staged.string();
    return "";
}

/* The compiler for the module build. Windows: the bundled llvm-mingw
 * next to the exe (no system compiler exists there); POSIX: bundled
 * first, then $OPENPETE_CC, then cc. */
std::string find_cc(const fs::path& base) {
#ifdef _WIN32
    fs::path bundled = base / "recomp" / "tools" / "llvm-mingw" / "bin" /
                       "x86_64-w64-mingw32-clang.exe";
    if (fs::exists(bundled)) return bundled.string();
    if (const char* env = openpete_config_env("OPENPETE_CC")) return env;
    return "";
#else
    fs::path bundled = base / "recomp" / "tools" / "clang" / "bin" / "clang";
    if (fs::exists(bundled)) return bundled.string();
    if (const char* env = openpete_config_env("OPENPETE_CC")) return env;
    return "cc";
#endif
}

/** @brief Run a probe command from an argument vector and return its stdout and stderr.
 *
 * The probe is not stoppable: its output keys the codegen stamp, which a
 * killed probe would change for the rest of the process.
 * @param args The program and its arguments.
 * @return The output (at most 1 MiB), or "" when the probe could not run. */
std::string run_capture(const std::vector<std::string>& args) {
    std::vector<const char*> argv;
    for (const std::string& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    std::vector<char> buf(1u << 20);
    if (openpete_host_spawn_argv(argv.data(), nullptr, buf.data(), buf.size()) < 0) return "";
    return buf.data();
}

/* First line of `<cc> --version` - the compiler's self-identity, folded
 * into the codegen stamp so swapping the host compiler (or bumping the
 * bundled one) invalidates cached modules instead of silently reusing a
 * dir built by a different optimizer. Empty string if the probe fails
 * (degrades to psxrecomp-only keying, the prior behavior). */
std::string cc_identity(const std::string& cc) {
    if (cc.empty()) return "";
    std::string v = run_capture({cc, "--version"});
    /* first line only */
    size_t nl = v.find_first_of("\r\n");
    return nl == std::string::npos ? v : v.substr(0, nl);
}

/* Per-machine target-tuning flag for the module compile. Because the
 * module is compiled ON the user's machine, we CAN target their exact
 * CPU - a prebuilt binary can't. Determinism-safe: the recompiled code
 * is pure INTEGER MIPS translation (verified: no float/double), so
 * -march changes instruction selection but not results (full 20k oracle
 * PASS with -march=native), and the fixed-point GTE math lives in the
 * prebuilt runtime, not here.
 *
 * DEFAULT OFF, though - measured: `-march=native` gave 0%
 * speedup on the attract-demo workload. The hot code (GTE, renderer,
 * PsyCross) is in the prebuilt runtime we can't tune per-user; the
 * module is branchy integer dispatch that neither vectorizes nor
 * dominates frame time. So the default is the portable baseline (no
 * -march) - CI-identical codegen for every user, no per-CPU cache
 * fragmentation. Opt in with OPENPETE_MODULE_MARCH=native (or a specific
 * value like x86-64-v3) if a heavier workload or CPU shows a benefit. */
std::string module_march() {
    const char* e = openpete_config_env("OPENPETE_MODULE_MARCH");
    std::string m = e ? e : "";
    if (m.empty() || m == "none" || m == "off") return "";
    return m;
}

/** @brief Return the module compile's target-tuning arguments for module_march().
 *
 * @return -march, -mtune=native and -ffp-contract=off, or none when no
 *         tuning is set. */
std::vector<std::string> module_arch_args() {
    std::string m = module_march();
    if (m.empty()) return {};
    return {"-march=" + m, "-mtune=native", "-ffp-contract=off"};
}

/* Fingerprint the exact target the compiler will emit for module_march():
 * its predefined macros (`__AVX2__`, feature set, target CPU). Folding
 * this into the stamp makes a native-built module dir CPU-specific, so a
 * library copied to a different machine rebuilds instead of executing an
 * illegal instruction. */
std::string cc_target_fingerprint(const std::string& cc) {
    std::string m = module_march();
    if (cc.empty() || m.empty()) return "";
#ifdef _WIN32
    const char* nul = "NUL";
#else
    const char* nul = "/dev/null";
#endif
    return run_capture({cc, "-march=" + m, "-dM", "-E", "-x", "c", nul});
}

/** @brief Return the first-boot module's codegen identity, the key its cache entries are stored under.
 *
 * A hash of (PSXRecomp binary + emission inputs + compiler identity +
 * native-target feature set), suffixed with the fixed compile flags that
 * are not hashed (-O2, -fno-strict-aliasing). Library entries key module dirs by this, so a
 * toolchain bump, a compiler swap, a manifest change OR a different-CPU
 * copy rebuilds under a fresh dir. Shared by the dir-keying entry point
 * and the in-build descriptor stamp so they never disagree.
 *
 * The emission inputs matter as much as the recompiler binary and change
 * far more often: PSXRecomp reads the manifests (per-binary funcs lists,
 * psyq.toml, channel_sites.txt) to decide what to emit, and the result
 * is compiled against the source kit. Two releases sharing a PSXRecomp
 * but shipping a new funcs list emit DIFFERENT code, so keying on the
 * binary alone would hand an upgrading user the previous version's
 * module - silently, since nothing about it looks stale.
 * @param base The install directory.
 * @param b The game whose manifest tree the stamp hashes.
 * @return The stamp, or "" when PSXRecomp is not found. */
std::string compute_codegen_stamp(const fs::path& base, const openpete_game_build_t* b) {
    /* Computed once per game per process: the compiler probes spawn a
     * process each, and every module path lookup and the build itself read
     * the stamp. The probes and the PSXRecomp hash are the same for every
     * game and are taken once; the trees are folded per game, in the order
     * the stamp has always used (the manifest before the source kit). */
    static std::mutex m;
    static std::map<std::string, std::string> stamps;
    static bool probed = false;
    static std::string psxrecomp, cc, cc_id, cc_fp;
    static uint64_t psx_h = 0;
    std::lock_guard<std::mutex> lock(m);
    const auto it = stamps.find(b->id);
    if (it != stamps.end()) return it->second;
    if (!probed) {
        probed = true;
        psxrecomp = find_psxrecomp(base);
        if (!psxrecomp.empty()) psx_h = fnv1a_file(psxrecomp);
        cc = find_cc(base);
        cc_id = cc_identity(cc);
        cc_fp = cc_target_fingerprint(cc);
    }
    if (psxrecomp.empty()) return "";
    uint64_t h = fnv1a_tree(base / "recomp" / "manifest" / b->id, psx_h);
    h = fnv1a_tree(base / "recomp" / "src", h);
    for (char c : cc_id) { h ^= (uint8_t)c; h *= 0x100000001b3ull; }
    for (char c : cc_fp) { h ^= (uint8_t)c; h *= 0x100000001b3ull; }
    const std::string stamp = "fnv1a-" + hex64(h) + "-O2-fno-strict-aliasing";
    stamps[b->id] = stamp;
    OPL_INFO_F("recomp-build", "codegen stamp of %s: %s", b->id, stamp.c_str());
    return stamp;
}

/* The compile step's progress counts in units of this many per build. */
constexpr int kCostScale = 100000;

/* The compile step's schedule: the translation units in the order the
 * workers take them, largest cost first, and each unit's share of the
 * step's compile time (summing to 1), the measure the progress report
 * counts in. */
struct CompileOrder {
    std::vector<size_t> order;
    std::vector<double> cost;
    int covered = 0;   /* units whose share came from the cost table */
};

/** @brief Read a compile cost table: one line per translation unit, its file name, a tab and its share of a reference build's compile time; lines opening with # are comments.
 *
 * @param path The table.
 * @return The shares by file name; empty when the file is missing. */
std::map<std::string, double> read_compile_costs(const fs::path& path) {
    std::map<std::string, double> t;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        const size_t tab = line.find('\t');
        if (line.empty() || line[0] == '#' || tab == std::string::npos) continue;
        const double c = std::strtod(line.c_str() + tab + 1, nullptr);
        if (c > 0.0) t[line.substr(0, tab)] = c;
    }
    return t;
}

/** @brief Weight the translation units by their compile cost and order them largest first, so the longest compiles start first and the progress report advances by compile time.
 *
 * A unit the cost table names takes its share from the table; any other
 * takes its share of the source size (KiB, at least 1) times the share of
 * the step the table leaves to the units it does not name; the shares
 * are then normalised to sum to 1.
 * @param tus The translation units, in link order (unchanged).
 * @param table The cost table (read_compile_costs); may be empty.
 * @return The schedule. */
CompileOrder compile_order(const std::vector<fs::path>& tus, const std::map<std::string, double>& table) {
    CompileOrder o;
    o.cost.resize(tus.size());
    std::vector<double> kib(tus.size());
    double kib_rest = 0.0, known = 0.0;
    for (size_t i = 0; i < tus.size(); i++) {
        std::error_code ec;
        const auto bytes = fs::file_size(tus[i], ec);
        kib[i] = ec ? 1.0 : std::max(1.0, (double)(bytes / 1024));
        const auto it = table.find(tus[i].filename().string());
        if (it != table.end()) {
            o.cost[i] = it->second;
            known += it->second;
            o.covered++;
        } else {
            kib_rest += kib[i];
        }
        o.order.push_back(i);
    }
    const double rest = o.covered ? std::max(0.0, 1.0 - known) : 1.0;
    double sum = 0.0;
    for (size_t i = 0; i < tus.size(); i++) {
        if (!table.count(tus[i].filename().string())) o.cost[i] = kib_rest > 0.0 ? rest * kib[i] / kib_rest : 0.0;
        sum += o.cost[i];
    }
    for (double& c : o.cost) c = sum > 0.0 ? c / sum : 1.0 / (double)tus.size();
    std::stable_sort(o.order.begin(), o.order.end(), [&](size_t a, size_t b) { return o.cost[a] > o.cost[b]; });
    return o;
}

/** @brief Write a compile cost table from the compile seconds of each unit of a build: each unit's seconds over the total.
 *
 * @param path The table to write.
 * @param tus The translation units.
 * @param secs Each unit's compile seconds.
 * @param jobs The build's worker count, recorded in the header.
 * @return true when the file was written. */
bool write_compile_costs(const fs::path& path, const std::vector<fs::path>& tus, const std::vector<double>& secs,
                         unsigned jobs) {
    double total = 0.0;
    for (double v : secs) total += v;
    if (total <= 0.0) return false;
    std::string text = "# First-boot compile cost per translation unit: its compile seconds over the build's\n"
                       "# total, from a reference build at -j" + std::to_string(jobs) +
                       " (scripts/emit-first-boot-costs.sh).\n";
    char row[64];
    for (size_t i = 0; i < tus.size(); i++) {
        snprintf(row, sizeof row, "\t%.6f\n", secs[i] / total);
        text += tus[i].filename().string() + row;
    }
    return op_atomic_write_file(path.string().c_str(), text.data(), text.size()) == 0;
}

}  // namespace

extern "C" int openpete_recomp_first_boot_module_path(const openpete_game_build_t* b,
                                                      const char* entry_dir, char* out,
                                                      size_t cap) {
    const std::string stamp = compute_codegen_stamp(exe_dir(), b);
    if (stamp.empty()) return -1;
#ifdef _WIN32
    const std::string name = std::string(b->module_name) + ".dll";
#else
    const std::string name = std::string(b->module_name) + ".so";
#endif
    const fs::path mod = fs::path(entry_dir) / "modules" / stamp / name;
    std::error_code ec;
    fs::create_directories(mod.parent_path(), ec);
    std::snprintf(out, cap, "%s", mod.string().c_str());
    return 0;
}

namespace {

/** @brief Run the first-boot pipeline: extract the executables from the disc, run PSXRecomp over them with the shipped manifests, compile the emitted C and the source kit with the compiler find_cc locates at -O2 -fno-strict-aliasing, and link the module.
 *
 * Its build directory is created beside out_module_path.
 * @param disc_path The disc image.
 * @param out_module_path Where the linked module is written.
 * @return 0 on success, -1 on any failure (reported on stderr). */
int first_boot_build(const char* disc_path, const char* out_module_path) {
    if (!disc_path) {
        std::fprintf(stderr, "openpete: first-boot module build needs --disc\n");
        return -1;
    }
    const fs::path base = exe_dir();
    const fs::path R    = base / "recomp";
    const fs::path man  = R / "manifest" / openpete_game_build()->id;
    const fs::path src  = R / "src";
    /* Work beside the output module: obj/ is deliberately kept (the
     * modding framework's link-time override chains re-link from these
     * objects); inputs/gen are scratch. */
    const fs::path work = fs::path(out_module_path).parent_path() / "build";
    const std::string cc = find_cc(base);
    if (cc.empty()) {
        std::fprintf(stderr,
            "openpete: no compiler for the first-boot module build - "
            "expected the bundled toolchain at recomp/tools/ next to the "
            "exe (or $OPENPETE_CC) %s\n", openpete_error_ref(OP_E_NO_COMPILER));
        return -1;
    }

    if (!fs::exists(man / "manifest.toml") || !fs::exists(src / "psx_runtime.h") ||
        !fs::exists(src / "psx_runtime_internal.h")) {
        std::fprintf(stderr,
            "openpete: recomp inputs not staged next to the exe (%s) - "
            "expected recomp/manifest/%s + recomp/src %s\n", R.string().c_str(),
            openpete_game_build()->id, openpete_error_ref(OP_E_PACKAGE));
        return -1;
    }
    const std::string psxrecomp = find_psxrecomp(base);
    if (psxrecomp.empty()) {
        std::fprintf(stderr, "openpete: PSXRecomp not found (recomp/tools/ "
                             "or $OPENPETE_PSXRECOMP) %s\n", openpete_error_ref(OP_E_PACKAGE));
        return -1;
    }

    std::vector<Overlay> ovls;
    ManifestHeader hdr;
    if (!parse_manifest(man / "manifest.toml", ovls, hdr)) {
        std::fprintf(stderr, "openpete: cannot parse %s %s\n",
                     (man / "manifest.toml").string().c_str(), openpete_error_ref(OP_E_PACKAGE));
        return -1;
    }

    OPL_INFO_F("recomp-build", "first-boot module build of %s into %s: %zu overlays, cc=%s",
               openpete_game_build()->id, out_module_path, ovls.size(), cc.c_str());
    std::error_code ec;
    fs::create_directories(fs::path(out_module_path).parent_path(), ec);
    fs::remove_all(work, ec);
    const fs::path inputs = work / "inputs", gen = work / "gen", obj = work / "obj";
    fs::create_directories(inputs, ec);
    fs::create_directories(gen, ec);
    fs::create_directories(obj, ec);
    if (ec || !fs::exists(obj)) {
        std::fprintf(stderr,
            "openpete: cannot create the module build directory (%s): %s %s\n"
            "  (deep install paths can exceed Windows' path limit - move "
            "the install to a shorter path)\n",
            work.string().c_str(), ec.message().c_str(), openpete_error_ref(OP_E_MODULE_DIR));
        return -1;
    }

    /* 1. extract ---------------------------------------------------------- */
    std::fprintf(stderr, "[first-boot] extracting game code from disc...\n");
    progress("READING GAME CODE FROM YOUR DISC", 0, 0);
    /* Harness fault: OPENPETE_RECOMP_BUILD_FAIL set fails the build at the
     * read step, as an unreadable disc would. */
    if (const char* f = getenv("OPENPETE_RECOMP_BUILD_FAIL"); f && *f && *f != '0') {
        OPL_ERROR_F("recomp-build", "%s", "build failed at the read step (OPENPETE_RECOMP_BUILD_FAIL)");
        return -1;
    }
    if (openpete_game_build()->extract_recomp_inputs(
            disc_path, inputs.string().c_str(), (man / "manifest.toml").string().c_str()) != 0)
        return -1;

    /* Per-binary identity: the manifests were converged against the
     * retail binaries whose sha256 they record. A mismatched binary is
     * a code-modified romhack - the forced-entry lists may under-cover
     * it (stub-miss risk); run anyway, loudly (default-and-warn; no
     * discovery pass runs for a modified binary). */
    {
        int modified = 0;
        if (!hdr.main_exe_sha.empty() &&
            sha256_file_hex(inputs / "PSX.EXE") != hdr.main_exe_sha) {
            std::fprintf(stderr, "[first-boot] NOTE: PSX.EXE differs from "
                                 "the retail binary\n");
            modified++;
        }
        for (auto& o : ovls) {
            if (!o.sha256.empty() &&
                sha256_file_hex(inputs / (o.name + ".ovl")) != o.sha256) {
                std::fprintf(stderr, "[first-boot] NOTE: %s differs from "
                                     "the retail binary\n", o.name.c_str());
                modified++;
            }
        }
        if (modified) {
            std::fprintf(stderr,
                "[first-boot] WARNING: %d of %zu game binaries are modified "
                "(romhack?). Recompiling with retail metadata - best effort; "
                "report stub-miss warnings if the game misbehaves.\n",
                modified, ovls.size() + 1);
        }
    }

    /* identity stamps (v1: FNV-1a over the carved inputs / the tool) */
    uint64_t rom_h = 0xcbf29ce484222325ull;
    rom_h = fnv1a_file(inputs / "PSX.EXE", rom_h);
    for (auto& o : ovls) rom_h = fnv1a_file(inputs / (o.name + ".ovl"), rom_h);
    const std::string rom_stamp     = "fnv1a:" + hex64(rom_h);
    /* Same stamp the library used to KEY this module's dir - must match
     * compute_codegen_stamp exactly (psxrecomp + compiler identity). */
    const std::string codegen_stamp = compute_codegen_stamp(base, openpete_game_build());

    /* 2. recompile -------------------------------------------------------- */
    std::fprintf(stderr, "[first-boot] recompiling MIPS -> C (%zu binaries)...\n",
                 ovls.size() + 1);
    progress("TRANSLATING GAME CODE", 0, (int)ovls.size() + 1);
    {
        std::vector<std::string> c = {
            psxrecomp, (inputs / "PSX.EXE").string(),
            "--output-dir=" + gen.string(),
            "--external-symbols=" + (man / "psyq.toml").string(),
            "--external-prefix=psyq_",
            "--extra-funcs=" + (man / "main_exe.funcs.txt").string()};
        if (hdr.channel_sites)
            c.push_back("--channel-sites=" + (man / "channel_sites.txt").string());
        if (!hdr.window_opt.empty()) c.push_back(hdr.window_opt);
        c.push_back("--defer-foreign-calls");
        c.push_back("--skip-on-error");
        if (!run(c, (gen / "PSX.EXE.psxrecomp.log").string().c_str())) return -1;
        if (!rename_token(gen / "PSX.EXE_dispatch.c",
                          "psx_dispatch_compiled", "psx_dispatch_main_exe"))
            return -1;
    }
    int ovl_done = 1;
    progress("TRANSLATING GAME CODE", ovl_done, (int)ovls.size() + 1);
    for (auto& o : ovls) {
        std::vector<std::string> c = {
            psxrecomp, (inputs / (o.name + ".ovl")).string(),
            "--raw-bin", "--load-addr=" + o.load_addr,
            "--output-stem=" + o.name + ".bin",
            "--output-dir=" + gen.string(),
            "--external-symbols=" + (man / "psyq.toml").string(),
            "--external-prefix=psyq_",
            "--func-prefix=func_" + o.name + "_",
            "--extra-funcs=" + (man / (o.name + ".funcs.txt")).string(),
            /* The main EXE's pure leaves, written by its translation above:
             * an overlay loop polling through one is a delivery site, as in
             * scripts/regen-recomp.sh. */
            "--leaf-summary=" + (gen / "PSX.EXE_leaves.tsv").string(),
            /* The main EXE's deferred call-carrying spins: the overlay
             * answers for those whose callee is one of its functions. */
            "--quiet-targets=" + (gen / "PSX.EXE_loop_sites.tsv").string()};
        if (!hdr.window_opt.empty()) c.push_back(hdr.window_opt);
        c.push_back("--skip-on-error");
        if (!run(c, (gen / (o.name + ".psxrecomp.log")).string().c_str())) return -1;
        if (!rename_token(gen / (o.name + ".bin_dispatch.c"),
                          "psx_dispatch_compiled", "psx_dispatch_" + o.name) ||
            !rename_token(gen / (o.name + ".bin_dispatch.c"),
                          "psx_quiet_compiled", "psx_quiet_" + o.name))
            return -1;
        progress("TRANSLATING GAME CODE", ++ovl_done, (int)ovls.size() + 1);
    }
    if (!emit_overlay_table(gen / "spyro_overlay_table.c", gen, ovls)) return -1;

    /* 3. compile ---------------------------------------------------------- */
    std::vector<fs::path> tus;
    for (auto& e : fs::directory_iterator(gen))
        if (e.path().extension() == ".c") tus.push_back(e.path());
    tus.push_back(src / "overlay_dispatch.c");
    tus.push_back(src / "module_main.c");

    const std::vector<std::string> arch = module_arch_args();  /* -march=native etc. */
    /* Route exe-exported DATA symbols through the import table on PE
     * (psx_runtime.h) - no fragile pseudo-relocation. */
    const std::string moddef = "-DOPENPETE_RECOMP_MODULE_BUILD";
    /* -fno-strict-aliasing: the recompiled C reads guest RAM through wider
     * pointer casts of byte arrays, as the CMake-built module does. */
#ifdef _WIN32
    std::vector<std::string> common = {"-c", "-O2", "-fno-strict-aliasing"};
    common.insert(common.end(), arch.begin(), arch.end());
    common.push_back(moddef);
    common.push_back("-w");
    common.push_back("-std=gnu99");
    common.push_back("-I" + src.string());
#else
    /* The bundled clang is the shipped configuration, and the machines it
     * ships to (SteamOS, any host without development packages) have no
     * libc headers. -nostdlibinc leaves only the compiler's freestanding
     * headers on every host, so a libc include in the kit fails the
     * build here as it would there. */
    const bool bundled_cc =
        cc == (base / "recomp" / "tools" / "clang" / "bin" / "clang").string();
    std::vector<std::string> common = {"-c", "-O2", "-fPIC", "-fno-strict-aliasing"};
    common.insert(common.end(), arch.begin(), arch.end());
    common.push_back(moddef);
    common.push_back("-w");
    common.push_back("-std=gnu99");
    if (bundled_cc) common.push_back("-nostdlibinc");
    common.push_back("-I" + src.string());
#endif
    if (!arch.empty())
        std::fprintf(stderr, "[first-boot] tuning for this CPU (%s)\n",
                     module_march().c_str());
    /* String-literal macros: each value carries its double quotes, passed
     * literally since no shell parses the argument vector. */
    const std::vector<std::string> stamps = {
        "-DOPENPETE_CODEGEN_STAMP=\"" + codegen_stamp + "\"",
        "-DOPENPETE_ROM_SHA256=\"" + rom_stamp + "\""};

    unsigned jobs = std::thread::hardware_concurrency();
    if (jobs == 0) jobs = 4;
    /* ~490 MB peak per compile process (local-compile-speed benchmark) -
     * cap the fan-out so low-RAM machines don't thrash. */
    if (jobs > 16) jobs = 16;
    std::fprintf(stderr, "[first-boot] compiling %zu C files (-j%u)...\n",
                 tus.size(), jobs);

    const fs::path cost_path = R / "costs" / (std::string(openpete_game_build()->id) + ".tsv");
    const CompileOrder sched = compile_order(tus, read_compile_costs(cost_path));
    OPL_INFO_F("recomp-build", "compile cost table: %d of %zu units (%s)", sched.covered, tus.size(),
               cost_path.string().c_str());
    std::vector<double> secs(tus.size(), 0.0);
    std::atomic<size_t> next{0};
    std::atomic<int> failed{0};
    std::atomic<size_t> done{0};
    std::atomic<int> done_cost{0};
    /* The workers read the stop of the job running the build. */
    void* const stop = openpete_launcher_job_stop_handle();
    auto worker = [&]() {
        openpete_launcher_job_stop_adopt(stop);
        for (;;) {
            const size_t n = next.fetch_add(1);
            if (n >= tus.size() || failed.load()) break;
            const size_t i = sched.order[n];
            const fs::path& tu = tus[i];
            std::vector<std::string> cmd = {cc};
            cmd.insert(cmd.end(), common.begin(), common.end());
            if (tu.filename() == "module_main.c") cmd.insert(cmd.end(), stamps.begin(), stamps.end());
            cmd.push_back("-o");
            cmd.push_back((obj / (tu.filename().string() + ".o")).string());
            cmd.push_back(tu.string());
            const auto t0 = std::chrono::steady_clock::now();
            if (!run(cmd)) { failed.store(1); break; }
            secs[i] = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            done_cost.fetch_add((int)std::lround(sched.cost[i] * kCostScale));
            size_t d = done.fetch_add(1) + 1;
            if (d % 20 == 0 || d == tus.size())
                std::fprintf(stderr, "[first-boot]   %zu/%zu\n", d, tus.size());
        }
        openpete_launcher_job_stop_adopt(nullptr);
    };
    {
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < jobs; t++) pool.emplace_back(worker);
        /* Progress from THIS thread (SDL rendering is not thread-safe);
         * workers only bump the counters. The report counts the compile
         * cost of the completed units, so a unit advances the bar by its
         * share of the step's compile time. */
        int last = -1;
        while (done.load() < tus.size() && !failed.load()) {
            const int d = std::min(done_cost.load(), kCostScale);
            if (d > last) {
                progress("BUILDING FOR YOUR PC", d, kCostScale);
                last = d;
            }
#ifdef _WIN32
            Sleep(50);
#else
            usleep(50 * 1000);
#endif
        }
        for (auto& t : pool) t.join();
        progress("BUILDING FOR YOUR PC", kCostScale, kCostScale);
    }
    if (!failed.load()) {
        if (const char* out = getenv("OPENPETE_FIRST_BOOT_COSTS_OUT"); out && *out)
            OPL_INFO_F("recomp-build", "compile cost table written to %s: %s", out,
                       write_compile_costs(out, tus, secs, jobs) ? "ok" : "failed");
    }
    if (failed.load()) return -1;

    /* 4. link ------------------------------------------------------------- */
    std::fprintf(stderr, "[first-boot] linking module...\n");
    progress("LINKING", 0, 0);
    {
        /* Object list via a response file: 79 absolute paths overflow
         * cmd.exe's 8191-char command limit. Paths are normalized to
         * forward slashes - clang's GNU rsp tokenizer treats backslash
         * as an escape character. */
        const fs::path rsp = work / "link.rsp";
        {
            std::ofstream rf(rsp, std::ios::binary);
            for (auto& tu : tus) {
                std::string o = (obj / (tu.filename().string() + ".o")).string();
                for (auto& ch : o) if (ch == '\\') ch = '/';
                rf << '"' << o << '"' << "\n";
            }
#ifdef _WIN32
            /* PE: imports resolve at link against the exe-export implib
             * staged with the manifests; no preemption flags needed. */
            std::string implib = (R / "tools" / "libopenpete-exe-exports.a").string();
            for (auto& ch : implib) if (ch == '\\') ch = '/';
            rf << '"' << implib << '"' << "\n";
#endif
        }
        /* The link writes into the work directory under the module's own
         * file name (a PE module records that name in its export table)
         * and the whole file is renamed onto the module path, so a stopped
         * or failed link leaves no partial module there. */
        const fs::path linked = work / "link" / fs::path(out_module_path).filename();
        fs::create_directories(linked.parent_path(), ec);
        std::vector<std::string> c = {cc, "-shared", "-o", linked.string(), "@" + rsp.string()};
#ifndef _WIN32
        c.push_back("-Wl,-Bsymbolic-functions");
        c.push_back("-Wl,--unresolved-symbols=ignore-all");
        /* The translated MIPS is pure integer code: it calls nothing from
         * the C library, defines no constructors and needs no crt frames,
         * so the module links with no libc and no DT_NEEDED at all. That
         * is what lets a bundled clang compile it on a machine with no
         * development packages installed - no crt*.o, no libc.so linker
         * script, no headers. Anything the module does reference (the
         * host's psyq_, psx_ and gte_ surface, and a compiler-emitted
         * memcpy should codegen ever grow one) resolves at dlopen from
         * the process-global scope, which already holds the exe and its
         * libc. */
        c.push_back("-nostdlib");
        c.push_back("-nostartfiles");
#endif
        if (!run(c)) return -1;
        fs::remove(out_module_path, ec);
        fs::rename(linked, out_module_path, ec);
        if (ec) {
            OPL_ERROR_F("recomp-build", "cannot move the linked module onto %s: %s %s", out_module_path,
                        ec.message().c_str(), openpete_error_ref(OP_E_MODULE));
            return -1;
        }
        fs::remove_all(linked.parent_path(), ec);
    }

    /* Drop the scratch now that the link has consumed it. gen/ is ~64 MB
     * of generated C and inputs/ the carved binaries it came from -
     * together about 70% of the module directory, reproducible from the
     * disc at any time, and read by nothing after this point. They are
     * kept on FAILURE (the early returns above), where they are the only
     * evidence of what went wrong. obj/ stays: the modding framework's
     * link-time override chains re-link from those objects. */
    fs::remove_all(gen, ec);
    fs::remove_all(inputs, ec);

    OPL_INFO_F("recomp-build", "module built: %s (codegen=%s rom=%s)",
               out_module_path, codegen_stamp.c_str(), rom_stamp.c_str());
    std::fprintf(stderr, "[first-boot] done: %s\n", out_module_path);
    return 0;
}

}  // namespace

extern "C" int openpete_recomp_first_boot_build(const char* disc_path,
                                                const char* out_module_path) {
    const int rc = first_boot_build(disc_path, out_module_path);
    if (rc == 0 || !openpete_launcher_job_cancelled()) return rc;
    /* A stopped build leaves nothing behind: its work directory (the
     * killed commands' partial objects and generated C) is removed, and the
     * module path never held a partial file (the link's rename). */
    std::error_code ec;
    fs::remove_all(fs::path(out_module_path).parent_path() / "build", ec);
    OPL_INFO_F("recomp-build", "module build stopped, its compiler commands killed: %s", out_module_path);
    return -2;
}
