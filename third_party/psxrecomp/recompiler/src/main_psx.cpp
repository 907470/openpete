#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <algorithm>

#include "ps1_exe_parser.h"
#include "gte.h"
#include "function_analysis.h"
#include "control_flow.h"
#include "code_generator.h"
#include "annotations.hpp"
#include "rabbitizer.hpp"
#include "fmt/format.h"

namespace {

/* Parse "key = value" / "[[symbols]]" / "name = "..."" / "addr = 0x..."
 * style TOML used by gen_psyq_toml.py and the manual-impl manifest. We do
 * not pull in toml11 here — the format we emit is line-oriented and
 * limited, so a hand-rolled parser stays cheap. */
struct ExternalManifest {
    uint32_t range_start = 0;
    uint32_t range_end   = 0;
    std::map<uint32_t, std::string> by_addr;
};

static std::string strip(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, b - a + 1);
}

static std::string strip_quotes(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    return s;
}

static bool parse_toml_manifest(const std::string& path, ExternalManifest& out, std::string& err) {
    std::ifstream f(path);
    if (!f.is_open()) { err = "cannot open " + path; return false; }
    std::string line;
    bool in_block = false;
    std::string cur_name;
    uint32_t cur_addr = 0;
    bool have_name = false, have_addr = false;
    auto flush = [&] {
        if (in_block && have_name && have_addr) {
            out.by_addr.emplace(cur_addr, cur_name);
        }
        cur_name.clear();
        have_name = have_addr = false;
        cur_addr = 0;
    };
    while (std::getline(f, line)) {
        std::string t = strip(line);
        if (t.empty() || t[0] == '#') continue;
        if (t == "[[symbols]]" || t == "[[manual]]") {
            flush();
            in_block = true;
            continue;
        }
        if (!t.empty() && t[0] == '[') {
            flush();
            in_block = false;
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = strip(t.substr(0, eq));
        std::string val = strip(t.substr(eq + 1));
        if (in_block) {
            if (key == "name") {
                cur_name = strip_quotes(val);
                have_name = true;
            } else if (key == "addr") {
                cur_addr = (uint32_t)std::strtoul(val.c_str(), nullptr, 0);
                have_addr = true;
            }
        } else {
            if (key == "range_start") {
                out.range_start = (uint32_t)std::strtoul(val.c_str(), nullptr, 0);
            } else if (key == "range_end") {
                out.range_end = (uint32_t)std::strtoul(val.c_str(), nullptr, 0);
            }
        }
    }
    flush();
    return true;
}

// Parse the flat channel-sites table generated from render_channels.toml by
// tools/gen_channel_sites.py. One site per non-comment line:
//   <kind> <addr-hex> <imm-hex> <symbol>
// e.g. `offset_add 0x800259BC 0x1C00 psx_runtime_env_hp_queue_offset`
// This keeps Spyro addresses out of the recompiler source: the manifest is
// the source of truth, this flat file is its build-time derivative (same
// pattern as --extra-funcs). Unknown kinds are a hard error so a manifest
// typo can't silently drop a substitution.
static bool parse_channel_sites(const std::string& path,
                                std::map<uint32_t, PSXRecomp::ChannelSite>& out,
                                std::string& err) {
    std::ifstream f(path);
    if (!f.is_open()) { err = "cannot open " + path; return false; }
    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        std::string t = strip(line);
        if (t.empty() || t[0] == '#') continue;
        std::istringstream is(t);
        std::string kind, addr_s, a2, a3;
        if (!(is >> kind >> addr_s >> a2 >> a3)) {
            err = fmt::format("{}:{}: expected `<kind> <addr> <arg2> <arg3>`",
                              path, lineno);
            return false;
        }
        uint32_t addr = (uint32_t)std::strtoul(addr_s.c_str(), nullptr, 0);
        PSXRecomp::ChannelSite site;
        // arg2/arg3 are interpreted per kind:
        //   offset_add / const_load : a2 = opcode imm,      a3 = knob symbol
        //   append_call             : a2 = "op1,op2,..."    a3 = hook fn name
        //   replace_call            : a2 = "dest=op1,op2,…" a3 = hook fn name
        auto split_csv = [](const std::string& s) {
            std::vector<std::string> out_v;
            size_t p = 0;
            while (p < s.size()) {
                size_t c = s.find(',', p);
                if (c == std::string::npos) c = s.size();
                if (c > p) out_v.push_back(s.substr(p, c - p));
                p = c + 1;
            }
            return out_v;
        };
        if (kind == "offset_add" || kind == "const_load") {
            site.kind = (kind == "offset_add")
                ? PSXRecomp::ChannelSite::OffsetAdd
                : PSXRecomp::ChannelSite::ConstLoad;
            site.imm    = (int32_t)std::strtol(a2.c_str(), nullptr, 0);
            site.symbol = a3;
        } else if (kind == "append_call") {
            site.kind     = PSXRecomp::ChannelSite::AppendCall;
            // "void" => a no-argument hook call (`hook();`).
            site.operands = (a2 == "void") ? std::vector<std::string>{}
                                           : split_csv(a2);
            site.symbol   = a3;
        } else if (kind == "replace_call") {
            site.kind = PSXRecomp::ChannelSite::ReplaceCall;
            // a2 = "dest=op1,op2,..."
            size_t eq = a2.find('=');
            if (eq == std::string::npos) {
                err = fmt::format("{}:{}: replace_call needs `dest=ops`: {}",
                                  path, lineno, a2);
                return false;
            }
            site.dest     = a2.substr(0, eq);
            {
                std::string ops = a2.substr(eq + 1);
                site.operands = (ops == "void") ? std::vector<std::string>{}
                                                : split_csv(ops);
            }
            site.symbol   = a3;
        } else {
            err = fmt::format("{}:{}: unknown channel-site kind '{}'",
                              path, lineno, kind);
            return false;
        }
        if (!out.emplace(addr, site).second) {
            err = fmt::format("{}:{}: duplicate channel site for 0x{:08X}",
                              path, lineno, addr);
            return false;
        }
    }
    return true;
}

// Anchor-resolution / fail-safe pass (hook-anchor plan step 1 + the cached-
// address verification tier of the per-binary anchor model). For every
// channel site that falls within THIS binary's range, decode the instruction
// at its cached address and verify it matches the site's kind: OffsetAdd must
// be an `addi` with the recorded immediate; ConstLoad must be a lui/addiu/ori
// with the recorded immediate. Append/ReplaceCall are opcode-agnostic (they
// attach to many opcodes), so in-range is the only check. Sites out of this
// binary's range belong to another binary (overlay) and are skipped.
//
// A mismatch means the cached address no longer resolves to its anchored
// instruction — i.e. the binary differs from the one the manifest was
// calibrated against (a romhack / custom build, or a stale manifest). The
// substitution would then silently NOT fire; we WARN loudly instead of
// corrupting an unrelated instruction (graceful degradation / fail-safe).
// For the retail SCUS-94228 main EXE every in-range anchor must verify clean.
static void verify_channel_anchors(
    const PSXRecomp::PS1Executable& exe,
    const std::map<uint32_t, PSXRecomp::ChannelSite>& sites,
    int& in_range, int& mismatched) {
    in_range = 0;
    mismatched = 0;
    for (const auto& kv : sites) {
        uint32_t addr = kv.first;
        const PSXRecomp::ChannelSite& site = kv.second;
        auto w = exe.read_word(addr);
        if (!w.has_value()) continue;  // belongs to another binary (overlay)
        ++in_range;
        uint32_t instr = *w;
        uint32_t op = (instr >> 26) & 0x3F;
        uint16_t imm = (uint16_t)(instr & 0xFFFFu);
        bool ok = true;
        const char* why = "";
        if (site.kind == PSXRecomp::ChannelSite::OffsetAdd) {
            if (op != 0x08u) { ok = false; why = "expected addi (op 0x08)"; }
            else if (imm != (uint16_t)site.imm) { ok = false; why = "immediate mismatch"; }
        } else if (site.kind == PSXRecomp::ChannelSite::ConstLoad) {
            if (op != 0x0Fu && op != 0x09u && op != 0x0Du) {
                ok = false; why = "expected lui/addiu/ori";
            } else if (imm != (uint16_t)site.imm) {
                ok = false; why = "immediate mismatch";
            }
        }
        // AppendCall / ReplaceCall: opcode-agnostic; in-range is sufficient.
        if (!ok) {
            ++mismatched;
            fmt::print(stderr,
                "  WARN: channel anchor 0x{:08X} ({}) {} — cached address does "
                "not resolve to its anchored instruction (instr=0x{:08X}); "
                "substitution will NOT fire (binary differs from manifest)\n",
                addr, site.symbol, why, instr);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    fmt::print("PSXRecomp - PlayStation 1 Static Recompiler\n");
    fmt::print("============================================\n\n");

    if (argc < 2) {
        fmt::print("Usage: {} <PS1-EXE file> [options]\n\n", argv[0]);
        fmt::print("Options:\n");
        fmt::print("  --output-dir=<dir>            Where to write *_full.c / *_dispatch.c (default: generated/)\n");
        fmt::print("  --extra-funcs=<file>          Discovered-function address list\n");
        fmt::print("  --external-symbols=<file>     PSYQ manifest (skip translation; emit externs)\n");
        fmt::print("  --external-prefix=<str>       Prepend <str> to every PSYQ symbol name (e.g. psyq_)\n");
        fmt::print("  --manual-impl=<file>          Manual-implementation manifest\n");
        fmt::print("  --manual-prefix=<str>         Prepend <str> to every manual-impl symbol name\n");
        fmt::print("  --func-prefix=<str>           Replace 'func_' prefix on emitted func names (e.g. level_10_func_)\n");
        fmt::print("  --emit-pc-tracking            Emit `cpu->pc = X;` before each insn (debug — for write-watchers)\n");
        fmt::print("  --forced-entry=<hex-addr>     Add a forced function entry point (repeatable)\n");
        fmt::print("  --emit-external-jals=<file>   Write KSEG0 JAL targets that fall outside this binary's range\n");
        fmt::print("  --channel-sites=<file>        Render-channel substitution table (from render_channels.toml)\n");
        fmt::print("  --skip-on-error               On codegen failure, emit a stub and continue\n");
        fmt::print("  --raw-bin                     Input is a raw code blob (no PS-X EXE header);\n");
        fmt::print("                                requires --load-addr. Replaces the out-of-tree\n");
        fmt::print("                                synth_overlay_exe.py wrapper for overlay blobs.\n");
        fmt::print("  --load-addr=<hex-addr>        KSEG0 load address for --raw-bin input\n");
        fmt::print("  --output-stem=<name>          Basename for <stem>_full.c/_dispatch.c\n");
        fmt::print("                                (default: the input filename)\n");
        return 0;
    }

    std::filesystem::path exe_path = argv[1];
    std::string output_dir       = "generated";
    std::string extra_funcs_path;
    bool raw_bin = false;
    uint32_t raw_load_addr = 0;
    std::string output_stem;
    std::string external_syms_path;
    std::string manual_impl_path;
    std::string external_prefix;   // prepended to every PSYQ symbol name
    std::string manual_prefix;     // prepended to every manual-impl name
    std::string func_prefix = "func_"; // prefix for emitted func_<addr> names
    bool skip_on_error = false;
    std::vector<uint32_t> forced_entries;
    std::string emit_external_jals_path;
    std::string channel_sites_path;
    bool emit_pc_tracking = false;

    auto take_value = [](const std::string& a, const char* prefix) -> std::string {
        size_t plen = std::strlen(prefix);
        if (a.size() > plen && a.compare(0, plen, prefix) == 0 && a[plen] == '=')
            return a.substr(plen + 1);
        return std::string();
    };

    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        std::string v;
        if (!(v = take_value(a, "--output-dir")).empty())            output_dir = v;
        else if (a == "--raw-bin")                                    raw_bin = true;
        else if (!(v = take_value(a, "--output-stem")).empty())       output_stem = v;
        else if (!(v = take_value(a, "--load-addr")).empty())
            raw_load_addr = (uint32_t)std::strtoul(v.c_str(), nullptr, 0);
        else if (!(v = take_value(a, "--extra-funcs")).empty())      extra_funcs_path = v;
        else if (!(v = take_value(a, "--external-symbols")).empty()) external_syms_path = v;
        else if (!(v = take_value(a, "--manual-impl")).empty())      manual_impl_path = v;
        else if (!(v = take_value(a, "--forced-entry")).empty())
            forced_entries.push_back((uint32_t)std::strtoul(v.c_str(), nullptr, 0));
        else if (!(v = take_value(a, "--external-prefix")).empty())   external_prefix = v;
        else if (!(v = take_value(a, "--manual-prefix")).empty())     manual_prefix = v;
        else if (!(v = take_value(a, "--func-prefix")).empty())       func_prefix = v;
        else if (!(v = take_value(a, "--emit-external-jals")).empty()) emit_external_jals_path = v;
        else if (a == "--emit-external-jals" && i + 1 < argc)         emit_external_jals_path = argv[++i];
        else if (!(v = take_value(a, "--channel-sites")).empty())     channel_sites_path = v;
        else if (a == "--channel-sites" && i + 1 < argc)              channel_sites_path = argv[++i];
        else if (a == "--skip-on-error")                              skip_on_error = true;
        else if (a == "--emit-pc-tracking")                           emit_pc_tracking = true;
        // Legacy form: `--extra-funcs <file>` (space-separated).
        else if (a == "--extra-funcs" && i + 1 < argc)                extra_funcs_path = argv[++i];
        else if (a == "--external-symbols" && i + 1 < argc)           external_syms_path = argv[++i];
        else if (a == "--manual-impl" && i + 1 < argc)                manual_impl_path = argv[++i];
        else if (a == "--forced-entry" && i + 1 < argc)
            forced_entries.push_back((uint32_t)std::strtoul(argv[++i], nullptr, 0));
        else if (a == "--output-dir" && i + 1 < argc)                 output_dir = argv[++i];
        else if (a == "--external-prefix" && i + 1 < argc)            external_prefix = argv[++i];
        else if (a == "--manual-prefix" && i + 1 < argc)              manual_prefix = argv[++i];
        else if (a == "--func-prefix" && i + 1 < argc)                func_prefix = argv[++i];
        else if (a.size() > 2 && a[0] == '-' && a[1] == '-') {
            fmt::print(stderr, "Unknown option: {}\n", a);
            return 1;
        }
    }

    // Parse the PS1-EXE file
    std::string error_msg;
    if (raw_bin && (raw_load_addr < 0x80000000u || raw_load_addr >= 0xA0000000u)) {
        fmt::print(stderr, "--raw-bin requires a KSEG0 --load-addr\n");
        return 1;
    }

    fmt::print("Parsing PS1-EXE: {}\n", exe_path.string());

    std::optional<PSXRecomp::PS1Executable> exe;
    if (raw_bin) {
        /* Raw overlay blob: synthesize the 2 KiB PS-X EXE header in memory
         * — byte-identical to tools/synth_overlay_exe.py's wrapper (magic,
         * pc0 = t_addr = load address, gp = 0, t_size = blob size; entry
         * is a placeholder — real entries come from --extra-funcs). */
        std::ifstream rf(exe_path, std::ios::binary);
        if (!rf) {
            fmt::print(stderr, "Failed to open raw binary: {}\n", exe_path.string());
            return 1;
        }
        std::vector<uint8_t> buf(2048, 0);
        buf.insert(buf.end(), std::istreambuf_iterator<char>(rf),
                   std::istreambuf_iterator<char>());
        const uint32_t body_size = (uint32_t)(buf.size() - 2048);
        std::memcpy(buf.data(), "PS-X EXE", 8);
        auto put32 = [&](size_t off, uint32_t v) {
            buf[off]     = (uint8_t)(v);
            buf[off + 1] = (uint8_t)(v >> 8);
            buf[off + 2] = (uint8_t)(v >> 16);
            buf[off + 3] = (uint8_t)(v >> 24);
        };
        put32(0x10, raw_load_addr);  /* initial_pc  */
        put32(0x14, 0);              /* initial_gp  */
        put32(0x18, raw_load_addr);  /* load_address */
        put32(0x1C, body_size);      /* file_size   */
        exe = PSXRecomp::PS1ExeParser::parse_buffer(buf, error_msg);
    } else {
        exe = PSXRecomp::PS1ExeParser::parse_file(exe_path, error_msg);
    }

    if (!exe.has_value()) {
        fmt::print(stderr, "Failed to parse PS1-EXE: {}\n", error_msg);
        return 1;
    }

    fmt::print("✓ Successfully parsed PS1-EXE!\n\n");

    // Print header information
    fmt::print("PS1-EXE Header Info:\n");
    fmt::print("  Entry Point:    0x{:08X}\n", exe->header.initial_pc);
    fmt::print("  Load Address:   0x{:08X}\n", exe->header.load_address);
    fmt::print("  Code Size:      {} bytes ({} KB)\n",
               exe->header.file_size, exe->header.file_size / 1024);
    fmt::print("  End Address:    0x{:08X}\n", exe->end_address());
    fmt::print("  Global Pointer: 0x{:08X}\n", exe->header.initial_gp);
    fmt::print("  Stack Pointer:  0x{:08X}\n\n", exe->header.initial_sp);

    // Validate entry point
    if (!exe->header.entry_in_range()) {
        fmt::print(stderr, "⚠ Warning: Entry point 0x{:08X} is outside loaded code range!\n\n",
                   exe->header.initial_pc);
    }

    // Disassemble first 20 instructions from entry point
    fmt::print("Disassembly at Entry Point (0x{:08X}):\n", exe->header.initial_pc);
    fmt::print("---------------------------------------\n");

    uint32_t current_addr = exe->header.initial_pc;
    const int num_instructions = 20;

    for (int i = 0; i < num_instructions; i++) {
        auto word_opt = exe->read_word(current_addr);

        if (!word_opt.has_value()) {
            fmt::print("  [End of code or read error]\n");
            break;
        }

        uint32_t instr_word = *word_opt;

        // Use Rabbitizer to disassemble the instruction
        // Note: Rabbitizer expects big-endian, PS1 is little-endian, so the word is already in correct format
        rabbitizer::InstructionCpu instr(instr_word, current_addr);

        // Get disassembly (parameter is flags, 0 = default)
        std::string disasm = instr.disassemble(0);

        // Print address, hex, and disassembly
        fmt::print("  {:08X}:  {:08X}  {}\n", current_addr, instr_word, disasm);

        current_addr += 4;
    }

    fmt::print("\n");
    fmt::print("✓ Disassembly complete!\n");

    // Instruction frequency analysis
    fmt::print("\n");
    fmt::print("Analyzing instruction frequency...\n");

    std::map<std::string, int> instr_freq;
    std::map<uint32_t, int> opcode_freq;
    std::map<uint32_t, int> special_funct_freq;

    uint32_t total_instructions = 0;
    uint32_t addr = exe->header.load_address;
    while (addr < exe->end_address()) {
        auto word_opt = exe->read_word(addr);
        if (!word_opt.has_value()) break;

        uint32_t instr_word = *word_opt;
        rabbitizer::InstructionCpu instr(instr_word, addr);
        std::string mnemonic = instr.getOpcodeName();

        instr_freq[mnemonic]++;
        total_instructions++;

        // Track opcode distribution
        uint32_t opcode = (instr_word >> 26) & 0x3F;
        opcode_freq[opcode]++;

        // Track SPECIAL function codes
        if (opcode == 0x00) {
            uint32_t funct = instr_word & 0x3F;
            special_funct_freq[funct]++;
        }

        addr += 4;
    }

    fmt::print("Total instructions: {}\n\n", total_instructions);

    // Sort by frequency and print top 20
    std::vector<std::pair<std::string, int>> sorted_freq(instr_freq.begin(), instr_freq.end());
    std::sort(sorted_freq.begin(), sorted_freq.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    fmt::print("=== Top 20 Most Common Instructions ===\n\n");
    fmt::print("{:4s}  {:12s}  {:10s}  {:s}\n", "Rank", "Instruction", "Count", "Percentage");
    fmt::print("-----------------------------------------------------\n");

    for (size_t i = 0; i < std::min(size_t(20), sorted_freq.size()); i++) {
        const auto& [instr_name, count] = sorted_freq[i];
        double pct = 100.0 * count / total_instructions;
        fmt::print("{:4d}  {:12s}  {:10d}  {:5.2f}%\n", i + 1, instr_name, count, pct);
    }

    fmt::print("\n");

    // Function boundary detection
    fmt::print("Performing function analysis...\n");

    PSXRecomp::FunctionAnalyzer analyzer(*exe);

    // Forced entry points: addresses the analyzer wouldn't auto-discover (no
    // standard prologue/epilogue, or only reached through a runtime dispatch).
    // Provide them via --forced-entry=0xADDR (repeatable). Game-specific
    // entries are NOT hardcoded here — keep the recompiler game-agnostic.
    for (uint32_t e : forced_entries) {
        fmt::print("Forced entry: 0x{:08X}\n", e);
        analyzer.add_forced_entry(e);
    }

    /* Load extra function addresses from discovered_functions.log */
    if (!extra_funcs_path.empty()) {
        std::ifstream ef(extra_funcs_path);
        if (ef.is_open()) {
            std::string line;
            int extra_count = 0;
            while (std::getline(ef, line)) {
                if (line.empty() || line[0] == '#') continue;
                uint32_t addr = (uint32_t)std::strtoul(line.c_str(), nullptr, 16);
                if (addr >= 0x80010000u && addr < 0x80200000u) {
                    analyzer.add_forced_entry(addr);
                    extra_count++;
                }
            }
            fmt::print("Loaded {} extra function addresses from {}\n", extra_count, extra_funcs_path);
        } else {
            fmt::print("WARNING: Cannot open extra-funcs file: {}\n", extra_funcs_path);
        }
    }

    auto analysis_result = analyzer.analyze();

    /* Optional: emit cross-binary JAL targets.
     *
     * Static JAL discovery (Pass 2.5) only follows JALs whose target lands
     * inside the current binary. When translating an overlay, JALs into
     * main-EXE bytes get rejected — the main EXE is a different binary
     * processed in its own psxrecomp invocation. Without help, those
     * call-sites stub-miss at runtime: the main-EXE pass never saw the
     * JAL, so the callee was never promoted to a function entry.
     *
     * `--emit-external-jals=<file>` writes every KSEG0 (0x80000000..
     * 0x80200000) JAL target that falls outside this binary's load range
     * to <file>, one address per line (in extra-funcs format). The driver
     * (regen-recomp.sh) unions these across overlays and feeds the result
     * back as `--extra-funcs=` on the main-EXE pass, closing the loop. */
    if (!emit_external_jals_path.empty()) {
        std::set<uint32_t> external_targets;
        uint32_t exe_start = exe->header.load_address;
        uint32_t exe_end   = exe->end_address();
        for (uint32_t a = exe_start; a + 4 <= exe_end; a += 4) {
            auto w = exe->read_word(a);
            if (!w.has_value()) break;
            uint32_t instr = *w;
            if (((instr >> 26) & 0x3F) != 3u) continue;  // not JAL
            uint32_t target = ((instr & 0x03FFFFFFu) << 2) | 0x80000000u;
            if ((target & 3u) != 0) continue;
            if (target < 0x80010000u || target >= 0x80200000u) continue;
            if (target >= exe_start && target < exe_end) continue;  // in-range
            external_targets.insert(target);
        }
        std::ofstream ej(emit_external_jals_path);
        if (ej.is_open()) {
            ej << "# external JAL targets (KSEG0, outside this binary's range)\n";
            ej << fmt::format("# binary range: 0x{:08X}..0x{:08X}\n", exe_start, exe_end);
            for (uint32_t t : external_targets) ej << fmt::format("0x{:08X}\n", t);
            fmt::print("✓ Wrote {} external JAL targets to {}\n\n",
                       external_targets.size(), emit_external_jals_path);
        } else {
            fmt::print(stderr, "⚠ Cannot open --emit-external-jals path: {}\n",
                       emit_external_jals_path);
        }
    }

    // Re-stamp function names with the configured --func-prefix. The analyzer
    // hard-codes "func_<addr>"; per-overlay outputs need a unique prefix so
    // their address-overlapping symbols don't collide at link time.
    if (func_prefix != "func_") {
        for (auto& f : analysis_result.functions) {
            // Only rename names that start with the default "func_" — leave
            // any analyzer-supplied symbolic names (jump-tables etc.) alone.
            if (f.name.rfind("func_", 0) == 0) {
                f.name = fmt::format("{}{:08X}", func_prefix, f.start_addr);
            }
        }
        fmt::print("  ↪ applied --func-prefix='{}'\n", func_prefix);
    }

    // Print summary statistics
    fmt::print("\n=== Function Analysis Summary ===\n\n");
    fmt::print("Total Functions: {}\n", analysis_result.functions.size());
    fmt::print("Code Coverage:   {} KB analyzed\n",
               (exe->end_address() - exe->header.load_address) / 1024);

    // Count functions with prologues/epilogues
    int with_prologue = 0;
    int with_epilogue = 0;
    int with_both = 0;

    for (const auto& func : analysis_result.functions) {
        if (func.has_prologue) with_prologue++;
        if (func.has_epilogue) with_epilogue++;
        if (func.has_prologue && func.has_epilogue) with_both++;
    }

    fmt::print("With Prologue:   {} ({:.1f}%)\n", with_prologue,
               100.0 * with_prologue / analysis_result.functions.size());
    fmt::print("With Epilogue:   {} ({:.1f}%)\n", with_epilogue,
               100.0 * with_epilogue / analysis_result.functions.size());
    fmt::print("Complete:        {} ({:.1f}%)\n\n", with_both,
               100.0 * with_both / analysis_result.functions.size());

    // Print first 20 functions
    fmt::print("First 20 Functions:\n");
    fmt::print("---------------------------------------\n");

    for (size_t i = 0; i < std::min(size_t(20), analysis_result.functions.size()); i++) {
        const auto& func = analysis_result.functions[i];
        fmt::print("  {:08X}-{:08X}  {:5} bytes  {}{}\n",
                   func.start_addr,
                   func.end_addr,
                   func.size,
                   func.name,
                   func.has_prologue ? fmt::format(" (frame={})", func.stack_frame_size) : "");
    }

    fmt::print("\n... ({} more functions)\n\n",
               analysis_result.functions.size() - std::min(size_t(20), analysis_result.functions.size()));

    // Control flow analysis
    PSXRecomp::ControlFlowAnalyzer cfg_analyzer(*exe);
    auto all_cfgs = cfg_analyzer.analyze_all_functions(analysis_result.functions);

    // Print CFG summary statistics
    fmt::print("=== Control Flow Summary ===\n\n");

    int total_basic_blocks = 0;
    int total_loops = 0;
    int total_branches = 0;

    for (const auto& [func_addr, cfg] : all_cfgs) {
        total_basic_blocks += static_cast<int>(cfg.blocks.size());
        total_loops += cfg.loop_count;

        // Count branch instructions across all blocks
        for (const auto& [block_addr, block] : cfg.blocks) {
            if (block.exit_instr.type == PSXRecomp::ControlFlowType::Branch) {
                total_branches++;
            }
        }
    }

    fmt::print("Basic Blocks:       {}\n", total_basic_blocks);
    fmt::print("Loops Detected:     {}\n", total_loops);
    fmt::print("Branch Instructions: {}\n", total_branches);
    fmt::print("Avg Blocks/Function: {:.1f}\n\n",
               static_cast<double>(total_basic_blocks) / all_cfgs.size());

    // Show detailed CFG for first function (as an example)
    if (!analysis_result.functions.empty() && !all_cfgs.empty()) {
        const auto& first_func = analysis_result.functions[0];
        const auto& first_cfg = all_cfgs.at(first_func.start_addr);

        fmt::print("Example: CFG for {} (0x{:08X}):\n", first_func.name, first_func.start_addr);
        fmt::print("---------------------------------------\n");
        fmt::print("  Blocks: {}\n", first_cfg.blocks.size());
        fmt::print("  Loops:  {}\n", first_cfg.loop_count);

        if (first_cfg.blocks.size() > 1) {
            fmt::print("\n  Block Details:\n");
            int count = 0;
            for (const auto& block_addr : first_cfg.block_order) {
                const auto& block = first_cfg.blocks.at(block_addr);
                fmt::print("    Block 0x{:08X}: {} instructions", block.start_addr, block.instruction_count);

                if (block.is_entry) fmt::print(" [ENTRY]");
                if (block.is_exit) fmt::print(" [EXIT]");
                if (block.is_loop_header) fmt::print(" [LOOP]");

                fmt::print("\n");
                fmt::print("      Successors: {}", block.successors.size());
                if (!block.successors.empty()) {
                    fmt::print(" (");
                    for (size_t i = 0; i < block.successors.size(); i++) {
                        fmt::print("0x{:08X}", block.successors[i]);
                        if (i + 1 < block.successors.size()) fmt::print(", ");
                    }
                    fmt::print(")");
                }
                fmt::print("\n");

                // Only show first 5 blocks for brevity
                if (++count >= 5 && first_cfg.blocks.size() > 5) {
                    fmt::print("    ... ({} more blocks)\n", first_cfg.blocks.size() - 5);
                    break;
                }
            }
        }
        fmt::print("\n");
    }

    // C code generation
    PSXRecomp::CodeGenConfig codegen_config;
    codegen_config.emit_comments = true;
    codegen_config.emit_line_numbers = true;
    codegen_config.func_prefix = func_prefix;
    codegen_config.emit_pc_tracking = emit_pc_tracking;

    // Load per-game annotations: <output_dir>/../manifest/<stem>_annotations.csv,
    // falling back to ./annotations/<stem>_annotations.csv for compat.
    PSXRecomp::AnnotationTable annotations;
    {
        std::string stem = exe_path.filename().string();
        std::string ann_path = "annotations/" + stem + "_annotations.csv";
        if (annotations.load(ann_path.c_str()))
            fmt::print("✓ Loaded {} annotations from {}\n\n", annotations.count(), ann_path);
        else
            fmt::print("  (No annotations file found at {})\n\n", ann_path);
    }

    PSXRecomp::CodeGenerator codegen(*exe, codegen_config);
    codegen.set_annotations(&annotations);
    codegen.set_skip_on_error(skip_on_error);

    // Load the render-channel substitution table (generated from
    // manifest/render_channels.toml). Optional — without it the data-driven
    // channel shapes simply aren't substituted (identity), so a recompile
    // that forgets the flag is safe-degraded, not silently wrong.
    if (!channel_sites_path.empty()) {
        std::map<uint32_t, PSXRecomp::ChannelSite> channel_sites;
        std::string err;
        if (!parse_channel_sites(channel_sites_path, channel_sites, err)) {
            fmt::print(stderr, "error: --channel-sites: {}\n", err);
            return 1;
        }
        // Anchor-resolution / fail-safe verification against THIS binary.
        int anchors_in_range = 0, anchors_mismatched = 0;
        verify_channel_anchors(*exe, channel_sites,
                               anchors_in_range, anchors_mismatched);
        fmt::print("Channel sites: {} ({} total, {} in-range anchors verified, "
                   "{} mismatched)\n",
                   channel_sites_path, channel_sites.size(),
                   anchors_in_range - anchors_mismatched, anchors_mismatched);
        if (anchors_mismatched > 0) {
            fmt::print(stderr,
                "warning: {} channel anchor(s) did not resolve to their "
                "cached instruction — those substitutions are disabled "
                "(see WARN lines above). For retail SCUS-94228 this must be 0.\n",
                anchors_mismatched);
        }
        codegen.set_channel_sites(std::move(channel_sites));
    }

    // Load PSYQ external-symbol manifest. Targets in this map are NOT
    // translated by the recompiler — they're emitted as extern declarations
    // and PsyCross supplies the C bodies at link time.
    ExternalManifest extman;
    if (!external_syms_path.empty()) {
        std::string err;
        if (!parse_toml_manifest(external_syms_path, extman, err)) {
            fmt::print(stderr, "⚠ External-symbols manifest failed: {}\n", err);
            return 1;
        }
        fmt::print("✓ Loaded {} external symbols from {} (range 0x{:08X}..0x{:08X})\n\n",
                   extman.by_addr.size(), external_syms_path,
                   extman.range_start, extman.range_end);
        if (!external_prefix.empty()) {
            std::map<uint32_t, std::string> prefixed;
            for (auto& [a, n] : extman.by_addr)
                prefixed[a] = external_prefix + n;
            extman.by_addr = std::move(prefixed);
            fmt::print("  ↪ applied --external-prefix='{}'\n\n", external_prefix);
        }
        codegen.set_external_symbols(extman.by_addr, extman.range_start, extman.range_end);
    }

    // Load manual-impl manifest. Each address listed gets an extern decl and
    // the C body is supplied by hand-written code.
    ExternalManifest manman;
    if (!manual_impl_path.empty()) {
        std::string err;
        if (!parse_toml_manifest(manual_impl_path, manman, err)) {
            fmt::print(stderr, "⚠ Manual-impl manifest failed: {}\n", err);
            return 1;
        }
        fmt::print("✓ Loaded {} manual implementations from {}\n\n",
                   manman.by_addr.size(), manual_impl_path);
        if (!manual_prefix.empty()) {
            std::map<uint32_t, std::string> prefixed;
            for (auto& [a, n] : manman.by_addr)
                prefixed[a] = manual_prefix + n;
            manman.by_addr = std::move(prefixed);
            fmt::print("  ↪ applied --manual-prefix='{}'\n\n", manual_prefix);
        }
        codegen.set_manual_impls(manman.by_addr);
    }

    // Skip the printing of a 5-function preview: keep the run quiet so the
    // tail of the log is dominated by stats and exit-criterion-relevant info.

    // Use filename() not stem() because ".28" in SCUS_942.28 is part of the
    // serial, not an extension.
    std::string exe_stem =
        output_stem.empty() ? exe_path.filename().string() : output_stem;

    std::filesystem::create_directories(output_dir);
    std::string output_filename   = output_dir + "/" + exe_stem + "_full.c";
    std::string dispatch_filename = output_dir + "/" + exe_stem + "_dispatch.c";
    std::string discovered_log    = output_dir + "/discovered_functions.log";

    fmt::print("Generating complete C file: {}\n", output_filename);

    std::string full_c_code = codegen.generate_file(analysis_result.functions, all_cfgs);

    std::ofstream out_file(output_filename);
    if (out_file.is_open()) {
        out_file << full_c_code;
        out_file.close();
        fmt::print("✓ Saved {} lines to {}\n\n",
                  std::count(full_c_code.begin(), full_c_code.end(), '\n'),
                  output_filename);
    } else {
        fmt::print(stderr, "⚠ Failed to write output file\n\n");
    }

    // Discovered-functions log — every function the analyzer landed on, in
    // order, so future runs can replay the same entry-point set via
    // --extra-funcs= and exit-criterion checks can grep for `main`.
    {
        std::ofstream df(discovered_log);
        if (df.is_open()) {
            df << "# Auto-generated by PSXRecomp.  One function start per line.\n";
            for (const auto& func : analysis_result.functions) {
                df << fmt::format("0x{:08X}  {}\n", func.start_addr, func.name);
            }
            fmt::print("✓ Wrote {} discovered functions to {}\n\n",
                       analysis_result.functions.size(), discovered_log);
        }
    }

    // Generate dispatch table. Maps PS1 addresses to compiled C functions
    // so call_by_address() can dispatch dynamic jalr/jr calls to the right
    // compiled function. External (PSYQ) and manual-impl entries are also
    // wired in — they dispatch to the named C function rather than to a
    // recompiled func_XXXXXXXX.
    {
        fmt::print("Generating dispatch table: {}\n", dispatch_filename);

        std::ostringstream ds;
        ds << "/* Generated by PSXRecomp - dynamic dispatch table */\n";
        ds << "#include \"psx_runtime.h\"\n\n";

        const auto& ext_syms = codegen.external_symbols();
        const auto& man_imps = codegen.manual_impls();

        // Forward declarations.
        ds << "/* Forward declarations for all recompiled functions */\n";
        for (const auto& func : analysis_result.functions) {
            if (codegen.addr_is_external(func.start_addr)) continue;
            if (man_imps.count(func.start_addr)) continue;
            ds << fmt::format("extern void {}(CPUState* cpu);\n", func.name);
        }
        if (!ext_syms.empty()) {
            ds << "\n/* External (PSYQ) bodies — supplied by PsyCross. */\n";
            for (const auto& [addr, name] : ext_syms) {
                ds << fmt::format("extern void {}(CPUState* cpu);\n", name);
            }
        }
        if (!man_imps.empty()) {
            ds << "\n/* Manual implementations — supplied by hand-written C. */\n";
            for (const auto& [addr, name] : man_imps) {
                ds << fmt::format("extern void {}(CPUState* cpu);\n", name);
            }
        }
        ds << "\n";

        // Dispatch function.
        ds << "/* Maps PS1 address to compiled function. Returns 1 if dispatched, 0 if unknown. */\n";
        ds << "int psx_dispatch_compiled(CPUState* cpu, uint32_t addr) {\n";
        ds << "    switch (addr) {\n";
        size_t total = 0;
        for (const auto& func : analysis_result.functions) {
            uint32_t a = func.start_addr;
            if (auto it = ext_syms.find(a); it != ext_syms.end()) {
                ds << fmt::format("        case 0x{:08X}u: {}(cpu); return 1;  /* PSYQ */\n",
                                  a, it->second);
            } else if (auto it2 = man_imps.find(a); it2 != man_imps.end()) {
                ds << fmt::format("        case 0x{:08X}u: {}(cpu); return 1;  /* manual */\n",
                                  a, it2->second);
            } else {
                ds << fmt::format("        case 0x{:08X}u: {}(cpu); return 1;\n", a, func.name);
            }
            total++;
        }
        ds << "        default: return 0;\n";
        ds << "    }\n";
        ds << "}\n";

        std::ofstream dispatch_file(dispatch_filename);
        if (dispatch_file.is_open()) {
            dispatch_file << ds.str();
            dispatch_file.close();
            fmt::print("✓ Dispatch table written ({} entries)\n\n", total);
        } else {
            fmt::print(stderr, "⚠ Failed to write dispatch file\n\n");
        }
    }

    return 0;
}
