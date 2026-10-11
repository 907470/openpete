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
// sites) or from the running game's build record (game_build.h): the module
// name, and the overlay window of the game's recomp.conf OVERLAY_RANGE.
//
// Inputs staged next to the exe by the shared-linkage build
// (CMake): recomp/manifest/<game id>/ (converged funcs lists +
// manifest.toml + psyq.toml + channel_sites.txt; one directory per game,
// since one executable stages every game's), recomp/src/ (the 5-file module source
// kit), recomp/tools/PSXRecomp when packaged. The host compiler is
// the bundled clang when packaged, else $OPENPETE_CC or `cc`.
//
// Cross-platform: POSIX uses the bundled clang / $OPENPETE_CC / cc;
// Windows requires the bundled llvm-mingw next to the exe
// (recomp/tools/llvm-mingw) and links against the staged exe-export
// implib (recomp/tools/libopenpete-exe-exports.a).
#include "openpete_launcher.h"
#include "openpete_atomic_file.h"
#include "game_build.h"
#include "recomp_attach.h"
#include "openpete_log.h"
#include "sha256.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
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

#ifdef _WIN32
#define QUIET_REDIR " > NUL"
#else
#define QUIET_REDIR " > /dev/null"
#endif

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

#ifdef _WIN32
/* Spawn `cmd.exe /C <cmd>` and wait, with NO console window. The shared
 * exe is a GUI-subsystem app (-mwindows) with no console of its own, so a
 * plain std::system()/_popen() child pops its own fresh console window -
 * and first boot spawns hundreds (38× PSXRecomp + ~80 compiler/link
 * invocations), each flashing a window. CREATE_NO_WINDOW allocates the
 * child's console hidden; grandchildren it spawns inherit that hidden
 * console, so the whole build runs silently. When `capture` is non-null,
 * the child's stdout+stderr are collected through a pipe (the _popen
 * replacement). Returns the child exit code, or -1 on spawn failure. */
int win_run_cmd(const std::string& cmd, std::string* capture) {
    HANDLE rd = NULL, wr = NULL;
    if (capture) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof sa;
        sa.bInheritHandle = TRUE;
        if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
        /* The read end stays with us - keep it out of the child. */
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    }
    /* cmd.exe /C strips the outermost quotes when the command contains
     * more than one quoted token - the classic system() trap. Wrapping
     * the whole command in one extra pair of quotes defeats it. */
    std::string full = "cmd.exe /C \"" + cmd + "\"";
    std::vector<char> buf(full.begin(), full.end());
    buf.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof si;
    if (capture) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = wr;
        si.hStdError  = wr;
    }
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(NULL, buf.data(), NULL, NULL,
                             capture ? TRUE : FALSE, CREATE_NO_WINDOW,
                             NULL, NULL, &si, &pi);
    if (wr) CloseHandle(wr);   /* the child owns the write end now */
    if (!ok) { if (rd) CloseHandle(rd); return -1; }
    if (capture) {
        char b[4096];
        DWORD n = 0;
        while (ReadFile(rd, b, sizeof b, &n, NULL) && n > 0)
            capture->append(b, n);
        CloseHandle(rd);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}
#endif

bool run(const std::string& cmd) {
#ifdef _WIN32
    int rc = win_run_cmd(cmd, NULL);
#else
    int rc = std::system(cmd.c_str());
#endif
    if (rc != 0) OPL_ERROR_F("recomp-build", "command failed (%d): %s", rc, cmd.c_str());
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

bool parse_manifest(const fs::path& toml, std::vector<Overlay>& out,
                    std::string* main_exe_sha, bool* channel_sites) {
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
            if (main_exe_sha && line.rfind("sha256 = \"", 0) == 0)
                *main_exe_sha = line.substr(10, line.size() - 11);
            if (line == "channel_sites = true") *channel_sites = true;
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

/* spyro_overlay_table.c is derivable from the registry - the boot build
 * emits it rather than shipping it, proving manifest completeness. The file
 * is replaced atomically (synced). */
bool emit_overlay_table(const fs::path& out, const std::vector<Overlay>& ovls) {
    std::ostringstream f;
    f << "/* AUTO-GENERATED by the first-boot module build - do not edit. */\n"
         "#include \"spyro_overlay_table.h\"\n\n";
    for (auto& o : ovls)
        f << "extern int psx_dispatch_" << o.name << "(CPUState* cpu, uint32_t addr);\n";
    f << "\nconst spyro_overlay_entry_t spyro_overlay_table[] = {\n";
    for (auto& o : ovls)
        f << "    { " << o.magic << ", \"" << o.name << "\", psx_dispatch_"
          << o.name << " },\n";
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
#define PSXRECOMP_DEVDIR "build-mingw"
#else
#define PSXRECOMP_NAME "PSXRecomp"
#define PSXRECOMP_DEVDIR "build"
#endif

std::string find_psxrecomp(const fs::path& base) {
    if (const char* env = getenv("OPENPETE_PSXRECOMP")) return env;
    fs::path staged = base / "recomp" / "tools" / PSXRECOMP_NAME;
    if (fs::exists(staged)) return staged.string();
    /* dev fallback: the repo build */
    fs::path repo = base / ".." / ".." / ".." / ".." /
                    "third_party" / "psxrecomp" / PSXRECOMP_DEVDIR / PSXRECOMP_NAME;
    std::error_code ec;
    fs::path canon = fs::canonical(repo, ec);
    if (!ec && fs::exists(canon)) return canon.string();
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
    if (const char* env = getenv("OPENPETE_CC")) return env;
    return "";
#else
    fs::path bundled = base / "recomp" / "tools" / "clang" / "bin" / "clang";
    if (fs::exists(bundled)) return bundled.string();
    if (const char* env = getenv("OPENPETE_CC")) return env;
    return "cc";
#endif
}

/* First line of `<cc> --version` - the compiler's self-identity, folded
 * into the codegen stamp so swapping the host compiler (or bumping the
 * bundled one) invalidates cached modules instead of silently reusing a
 * dir built by a different optimizer. Empty string if the probe fails
 * (degrades to psxrecomp-only keying, the prior behavior). */
std::string run_capture(const std::string& cmd) {
#ifdef _WIN32
    /* CreateProcess with CREATE_NO_WINDOW instead of _popen - otherwise
     * the compiler-identity probe flashes a console window too. */
    std::string out;
    win_run_cmd(cmd, &out);
    return out;
#else
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return "";
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
#endif
}

std::string cc_identity(const std::string& cc) {
    if (cc.empty()) return "";
    std::string v = run_capture("\"" + cc + "\" --version 2>&1");
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
    const char* e = getenv("OPENPETE_MODULE_MARCH");
    std::string m = e ? e : "";
    if (m.empty() || m == "none" || m == "off") return "";
    return m;
}

std::string module_arch_flags() {
    std::string m = module_march();
    if (m.empty()) return "";
    return " -march=" + m + " -mtune=native -ffp-contract=off";
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
    return run_capture("\"" + cc + "\" -march=" + m +
                       " -dM -E -x c \"" + nul + "\" 2>&1");
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
 * @return The stamp, or "" when PSXRecomp is not found. */
std::string compute_codegen_stamp(const fs::path& base) {
    const std::string psxrecomp = find_psxrecomp(base);
    if (psxrecomp.empty()) return "";
    uint64_t h = fnv1a_file(psxrecomp);
    h = fnv1a_tree(base / "recomp" / "manifest" / openpete_game_build()->id, h);
    h = fnv1a_tree(base / "recomp" / "src", h);
    const std::string cc = find_cc(base);
    for (char c : cc_identity(cc))          { h ^= (uint8_t)c; h *= 0x100000001b3ull; }
    for (char c : cc_target_fingerprint(cc)){ h ^= (uint8_t)c; h *= 0x100000001b3ull; }
    return "fnv1a-" + hex64(h) + "-O2-fno-strict-aliasing";
}

}  // namespace

extern "C" int openpete_recomp_first_boot_module_path(const char* entry_dir, char* out,
                                                      size_t cap) {
    const std::string stamp = compute_codegen_stamp(exe_dir());
    if (stamp.empty()) return -1;
#ifdef _WIN32
    const std::string name = std::string(openpete_game_build()->module_name) + ".dll";
#else
    const std::string name = std::string(openpete_game_build()->module_name) + ".so";
#endif
    const fs::path mod = fs::path(entry_dir) / "modules" / stamp / name;
    std::error_code ec;
    fs::create_directories(mod.parent_path(), ec);
    std::snprintf(out, cap, "%s", mod.string().c_str());
    return 0;
}

/* The pipeline: extracts the executables from the disc, runs
 * PSXRecomp over them with the shipped manifests, compiles the emitted C and
 * the source kit with the compiler find_cc locates at -O2
 * -fno-strict-aliasing, and links the module. Its build directory is created
 * beside out_module_path. */
extern "C" int openpete_recomp_first_boot_build(const char* disc_path,
                                                const char* out_module_path) {
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
    /* Shell quote: cmd.exe double-quotes on Windows, single elsewhere. */
#ifdef _WIN32
    const std::string q = "\"";
#else
    const std::string q = "'";
#endif
    const std::string cc = find_cc(base);
    if (cc.empty()) {
        std::fprintf(stderr,
            "openpete: no compiler for the first-boot module build - "
            "expected the bundled toolchain at recomp/tools/ next to the "
            "exe (or $OPENPETE_CC)\n");
        return -1;
    }

    if (!fs::exists(man / "manifest.toml") || !fs::exists(src / "psx_runtime.h") ||
        !fs::exists(src / "psx_runtime_internal.h")) {
        std::fprintf(stderr,
            "openpete: recomp inputs not staged next to the exe (%s) - "
            "expected recomp/manifest/%s + recomp/src\n", R.string().c_str(),
            openpete_game_build()->id);
        return -1;
    }
    const std::string psxrecomp = find_psxrecomp(base);
    if (psxrecomp.empty()) {
        std::fprintf(stderr, "openpete: PSXRecomp not found (recomp/tools/, "
                             "$OPENPETE_PSXRECOMP, or the repo build)\n");
        return -1;
    }

    std::vector<Overlay> ovls;
    std::string main_exe_sha;
    bool channel_sites = false;
    if (!parse_manifest(man / "manifest.toml", ovls, &main_exe_sha, &channel_sites)) {
        std::fprintf(stderr, "openpete: cannot parse %s\n",
                     (man / "manifest.toml").string().c_str());
        return -1;
    }

    OPL_INFO_F("recomp-build", "first-boot module build: %zu overlays, cc=%s",
               ovls.size(), cc.c_str());
    std::error_code ec;
    fs::create_directories(fs::path(out_module_path).parent_path(), ec);
    fs::remove_all(work, ec);
    const fs::path inputs = work / "inputs", gen = work / "gen", obj = work / "obj";
    fs::create_directories(inputs, ec);
    fs::create_directories(gen, ec);
    fs::create_directories(obj, ec);
    if (ec || !fs::exists(obj)) {
        std::fprintf(stderr,
            "openpete: cannot create the module build directory (%s): %s\n"
            "  (deep install paths can exceed Windows' path limit - move "
            "the install to a shorter path)\n",
            work.string().c_str(), ec.message().c_str());
        return -1;
    }

    /* 1. extract ---------------------------------------------------------- */
    std::fprintf(stderr, "[first-boot] extracting game code from disc...\n");
    progress("READING GAME CODE FROM YOUR DISC", 0, 0);
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
        if (!main_exe_sha.empty() &&
            sha256_file_hex(inputs / "PSX.EXE") != main_exe_sha) {
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
    const std::string codegen_stamp = compute_codegen_stamp(base);

    /* 2. recompile -------------------------------------------------------- */
    std::fprintf(stderr, "[first-boot] recompiling MIPS -> C (%zu binaries)...\n",
                 ovls.size() + 1);
    progress("TRANSLATING GAME CODE", 0, (int)ovls.size() + 1);
    {
        std::ostringstream c;
        c << q << psxrecomp << q << " " << q << (inputs / "PSX.EXE").string() << q
          << " --output-dir=" << q << gen.string() << q
          << " --external-symbols=" << q << (man / "psyq.toml").string() << q
          << " --external-prefix=psyq_"
          << " --extra-funcs=" << q << (man / "main_exe.funcs.txt").string() << q;
        if (channel_sites)
            c << " --channel-sites=" << q << (man / "channel_sites.txt").string() << q;
        c << " --skip-on-error" << QUIET_REDIR;
        if (!run(c.str())) return -1;
        if (!rename_token(gen / "PSX.EXE_dispatch.c",
                          "psx_dispatch_compiled", "psx_dispatch_main_exe"))
            return -1;
    }
    int ovl_done = 1;
    progress("TRANSLATING GAME CODE", ovl_done, (int)ovls.size() + 1);
    for (auto& o : ovls) {
        std::ostringstream c;
        c << q << psxrecomp << q << " " << q << (inputs / (o.name + ".ovl")).string() << q
          << " --raw-bin --load-addr=" << o.load_addr
          << " --output-stem=" << o.name << ".bin"
          << " --output-dir=" << q << gen.string() << q
          << " --external-symbols=" << q << (man / "psyq.toml").string() << q
          << " --external-prefix=psyq_"
          << " --func-prefix=func_" << o.name << "_"
          << " --extra-funcs=" << q << (man / (o.name + ".funcs.txt")).string() << q
          << " --skip-on-error" << QUIET_REDIR;
        if (!run(c.str())) return -1;
        if (!rename_token(gen / (o.name + ".bin_dispatch.c"),
                          "psx_dispatch_compiled", "psx_dispatch_" + o.name))
            return -1;
        progress("TRANSLATING GAME CODE", ++ovl_done, (int)ovls.size() + 1);
    }
    if (!emit_overlay_table(gen / "spyro_overlay_table.c", ovls)) return -1;

    /* 3. compile ---------------------------------------------------------- */
    std::vector<fs::path> tus;
    for (auto& e : fs::directory_iterator(gen))
        if (e.path().extension() == ".c") tus.push_back(e.path());
    tus.push_back(src / "overlay_dispatch.c");
    tus.push_back(src / "module_main.c");

    const std::string arch = module_arch_flags();  /* -march=native etc. */
    /* Route exe-exported DATA symbols through the import table on PE
     * (psx_runtime.h) - no fragile pseudo-relocation. */
    const std::string moddef = " -DOPENPETE_RECOMP_MODULE_BUILD";
    /* -fno-strict-aliasing: the recompiled C reads guest RAM through wider
     * pointer casts of byte arrays, as the CMake-built module does. */
#ifdef _WIN32
    const std::string common =
        " -c -O2 -fno-strict-aliasing" + arch + moddef + " -w -std=gnu99 -I" + q +
        src.string() + q;
#else
    /* The bundled clang is the shipped configuration, and the machines it
     * ships to (SteamOS, any host without development packages) have no
     * libc headers. -nostdlibinc leaves only the compiler's freestanding
     * headers on every host, so a libc include in the kit fails the
     * build here as it would there. */
    const bool bundled_cc =
        cc == (base / "recomp" / "tools" / "clang" / "bin" / "clang").string();
    const std::string common =
        " -c -O2 -fPIC -fno-strict-aliasing" + arch + moddef + " -w -std=gnu99" +
        (bundled_cc ? " -nostdlibinc" : "") + " -I" + q + src.string() + q;
#endif
    if (!arch.empty())
        std::fprintf(stderr, "[first-boot] tuning for this CPU (%s)\n",
                     module_march().c_str());
#ifdef _WIN32
    /* MSVCRT argv parsing unescapes \" - no single-quote idiom on cmd. */
    const std::string stamps =
        " -DOPENPETE_CODEGEN_STAMP=\\\"" + codegen_stamp + "\\\""
        " -DOPENPETE_ROM_SHA256=\\\"" + rom_stamp + "\\\"";
#else
    const std::string stamps =
        " -DOPENPETE_CODEGEN_STAMP='\"" + codegen_stamp + "\"'"
        " -DOPENPETE_ROM_SHA256='\"" + rom_stamp + "\"'";
#endif

    unsigned jobs = std::thread::hardware_concurrency();
    if (jobs == 0) jobs = 4;
    /* ~490 MB peak per compile process (local-compile-speed benchmark) -
     * cap the fan-out so low-RAM machines don't thrash. */
    if (jobs > 16) jobs = 16;
    std::fprintf(stderr, "[first-boot] compiling %zu C files (-j%u)...\n",
                 tus.size(), jobs);

    std::atomic<size_t> next{0};
    std::atomic<int> failed{0};
    std::atomic<size_t> done{0};
    auto worker = [&]() {
        for (;;) {
            size_t i = next.fetch_add(1);
            if (i >= tus.size() || failed.load()) return;
            const fs::path& tu = tus[i];
            std::string extra = (tu.filename() == "module_main.c") ? stamps : "";
            std::string cmd = q + cc + q + common + extra +
                " -o " + q + (obj / (tu.filename().string() + ".o")).string() +
                q + " " + q + tu.string() + q;
            if (!run(cmd)) { failed.store(1); return; }
            size_t d = done.fetch_add(1) + 1;
            if (d % 20 == 0 || d == tus.size())
                std::fprintf(stderr, "[first-boot]   %zu/%zu\n", d, tus.size());
        }
    };
    {
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < jobs; t++) pool.emplace_back(worker);
        /* Progress from THIS thread (SDL rendering is not thread-safe);
         * workers only bump the counter. */
        size_t last = (size_t)-1;
        while (done.load() < tus.size() && !failed.load()) {
            size_t d = done.load();
            if (d != last) {
                progress("BUILDING FOR YOUR PC", (int)d, (int)tus.size());
                last = d;
            }
#ifdef _WIN32
            Sleep(50);
#else
            usleep(50 * 1000);
#endif
        }
        for (auto& t : pool) t.join();
        progress("BUILDING FOR YOUR PC", (int)tus.size(), (int)tus.size());
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
        std::ostringstream c;
        c << q << cc << q << " -shared -o " << q << out_module_path << q
          << " @" << q << rsp.string() << q;
#ifndef _WIN32
        c << " -Wl,-Bsymbolic-functions -Wl,--unresolved-symbols=ignore-all";
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
        c << " -nostdlib -nostartfiles";
#endif
        if (!run(c.str())) return -1;
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
