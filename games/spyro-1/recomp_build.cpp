// games/spyro-1/recomp_build.cpp — the first-boot module build
// (distribution D2.4; plan openpete-distribution-local-recomp).
//
// Turns a disc into spyro1-recomp.so with no python and no discovery:
//
//   extract (in-process, D2.3) ──► PSXRecomp ×38 (single pass, staged
//   converged manifests) ──► cc -O2 -fPIC fan-out ──► link
//
// Inputs staged next to the exe by the shared-linkage build (D2.4
// CMake): recomp/manifest/ (converged funcs lists + manifest.toml +
// psyq.toml + channel_sites.txt), recomp/src/ (the 5-file module source
// kit), recomp/tools/PSXRecomp when packaged. The host compiler is
// $OPENPETE_CC or `cc` until D3 bundles a pinned clang.
//
// Cross-platform: POSIX uses the bundled clang / $OPENPETE_CC / cc;
// Windows requires the bundled llvm-mingw next to the exe
// (recomp/tools/llvm-mingw) and links against the staged exe-export
// implib (recomp/tools/libopenpete-exe-exports.a).
#include "openpete_log.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

extern "C" int spyro1_extract_recomp_inputs(const char* cue_path,
                                            const char* out_dir);

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

bool run(const std::string& cmd) {
#ifdef _WIN32
    /* cmd.exe /C strips the outermost quotes when the command contains
     * more than one quoted token — the classic system() trap. Wrapping
     * the whole command in one extra pair of quotes defeats it. */
    const std::string wrapped = "\"" + cmd + "\"";
    int rc = std::system(wrapped.c_str());
#else
    int rc = std::system(cmd.c_str());
#endif
    if (rc != 0) OPL_ERROR_F("recomp-build", "command failed (%d): %s", rc, cmd.c_str());
    return rc == 0;
}

/* FNV-1a 64 over a file — the v1 identity/codegen stamp (labeled as
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

std::string hex64(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
    return b;
}

/* Replace every whole-word occurrence of `from` with `to` in file `p`
 * (the dispatch-rename regen does with sed). */
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
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << out_s;
    return out.good();
}

bool parse_manifest(const fs::path& toml, std::vector<Overlay>& out) {
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
        if (!in_overlay) continue;
        if (line.rfind("name = \"", 0) == 0)
            cur.name = line.substr(8, line.size() - 9);
        else if (line.rfind("magic = ", 0) == 0)
            cur.magic = std::atoi(line.c_str() + 8);
    }
    flush();
    return !out.empty();
}

/* spyro_overlay_table.c is derivable from the registry — the boot build
 * emits it rather than shipping it, proving manifest completeness. */
bool emit_overlay_table(const fs::path& out, const std::vector<Overlay>& ovls) {
    std::ofstream f(out);
    f << "/* AUTO-GENERATED by the first-boot module build — do not edit. */\n"
         "#include \"spyro_overlay_table.h\"\n\n";
    for (auto& o : ovls)
        f << "extern int psx_dispatch_" << o.name << "(CPUState* cpu, uint32_t addr);\n";
    f << "\nconst spyro_overlay_entry_t spyro_overlay_table[] = {\n";
    for (auto& o : ovls)
        f << "    { " << o.magic << ", \"" << o.name << "\", psx_dispatch_"
          << o.name << " },\n";
    f << "};\nconst size_t spyro_overlay_table_count = "
         "sizeof(spyro_overlay_table) / sizeof(spyro_overlay_table[0]);\n";
    return f.good();
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

}  // namespace

/* The codegen identity for cache keying: hash of the PSXRecomp binary +
 * the opt level. Library entries key module dirs by this stamp so an
 * engine/toolchain update rebuilds under a fresh dir. */
extern "C" int spyro1_recomp_codegen_stamp(char* out, size_t cap) {
    const std::string psxrecomp = find_psxrecomp(exe_dir());
    if (psxrecomp.empty()) return -1;
    std::snprintf(out, cap, "fnv1a-%s-O2",
                  hex64(fnv1a_file(psxrecomp)).c_str());
    return 0;
}

extern "C" int spyro1_recomp_build_module(const char* disc_path,
                                          const char* out_module_path) {
    if (!disc_path) {
        std::fprintf(stderr, "openpete: first-boot module build needs --disc\n");
        return -1;
    }
    const fs::path base = exe_dir();
    const fs::path R    = base / "recomp";
    const fs::path man  = R / "manifest";
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
            "openpete: no compiler for the first-boot module build — "
            "expected the bundled toolchain at recomp/tools/ next to the "
            "exe (or $OPENPETE_CC)\n");
        return -1;
    }

    if (!fs::exists(man / "manifest.toml") || !fs::exists(src / "psx_runtime.h")) {
        std::fprintf(stderr,
            "openpete: recomp inputs not staged next to the exe (%s) — "
            "expected recomp/manifest + recomp/src\n", R.string().c_str());
        return -1;
    }
    const std::string psxrecomp = find_psxrecomp(base);
    if (psxrecomp.empty()) {
        std::fprintf(stderr, "openpete: PSXRecomp not found (recomp/tools/, "
                             "$OPENPETE_PSXRECOMP, or the repo build)\n");
        return -1;
    }

    std::vector<Overlay> ovls;
    if (!parse_manifest(man / "manifest.toml", ovls)) {
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
    fs::create_directories(inputs);
    fs::create_directories(gen);
    fs::create_directories(obj);

    /* 1. extract ---------------------------------------------------------- */
    std::fprintf(stderr, "[first-boot] extracting game code from disc...\n");
    if (spyro1_extract_recomp_inputs(disc_path, inputs.string().c_str()) != 0)
        return -1;

    /* identity stamps (v1: FNV-1a over the carved inputs / the tool) */
    uint64_t rom_h = 0xcbf29ce484222325ull;
    rom_h = fnv1a_file(inputs / "PSX.EXE", rom_h);
    for (auto& o : ovls) rom_h = fnv1a_file(inputs / (o.name + ".ovl"), rom_h);
    const std::string rom_stamp     = "fnv1a:" + hex64(rom_h);
    const std::string codegen_stamp =
        "fnv1a-" + hex64(fnv1a_file(psxrecomp)) + "-O2";

    /* 2. recompile -------------------------------------------------------- */
    std::fprintf(stderr, "[first-boot] recompiling MIPS -> C (%zu binaries)...\n",
                 ovls.size() + 1);
    {
        std::ostringstream c;
        c << q << psxrecomp << q << " " << q << (inputs / "PSX.EXE").string() << q
          << " --output-dir=" << q << gen.string() << q
          << " --external-symbols=" << q << (man / "psyq.toml").string() << q
          << " --external-prefix=psyq_"
          << " --extra-funcs=" << q << (man / "main_exe.funcs.txt").string() << q
          << " --channel-sites=" << q << (man / "channel_sites.txt").string() << q
          << " --skip-on-error" << QUIET_REDIR;
        if (!run(c.str())) return -1;
        if (!rename_token(gen / "PSX.EXE_dispatch.c",
                          "psx_dispatch_compiled", "psx_dispatch_main_exe"))
            return -1;
    }
    for (auto& o : ovls) {
        std::ostringstream c;
        c << q << psxrecomp << q << " " << q << (inputs / (o.name + ".ovl")).string() << q
          << " --raw-bin --load-addr=0x8007AA38"
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
    }
    if (!emit_overlay_table(gen / "spyro_overlay_table.c", ovls)) return -1;

    /* 3. compile ---------------------------------------------------------- */
    std::vector<fs::path> tus;
    for (auto& e : fs::directory_iterator(gen))
        if (e.path().extension() == ".c") tus.push_back(e.path());
    tus.push_back(src / "dispatch_chain.c");
    tus.push_back(src / "module_main.c");

#ifdef _WIN32
    const std::string common =
        " -c -O2 -w -std=gnu99 -I" + q + src.string() + q;
#else
    const std::string common =
        " -c -O2 -fPIC -w -std=gnu99 -I" + q + src.string() + q;
#endif
#ifdef _WIN32
    /* MSVCRT argv parsing unescapes \" — no single-quote idiom on cmd. */
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
    /* ~490 MB peak per compile process (local-compile-speed benchmark) —
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
        for (auto& t : pool) t.join();
    }
    if (failed.load()) return -1;

    /* 4. link ------------------------------------------------------------- */
    std::fprintf(stderr, "[first-boot] linking module...\n");
    {
        /* Object list via a response file: 79 absolute paths overflow
         * cmd.exe's 8191-char command limit. Paths are normalized to
         * forward slashes — clang's GNU rsp tokenizer treats backslash
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
#endif
        if (!run(c.str())) return -1;
    }

    OPL_INFO_F("recomp-build", "module built: %s (codegen=%s rom=%s)",
               out_module_path, codegen_stamp.c_str(), rom_stamp.c_str());
    std::fprintf(stderr, "[first-boot] done: %s\n", out_module_path);
    return 0;
}
