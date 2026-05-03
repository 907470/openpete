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
        fmt::print("  --skip-on-error               On codegen failure, emit a stub and continue\n");
        return 0;
    }

    std::filesystem::path exe_path = argv[1];
    std::string output_dir       = "generated";
    std::string extra_funcs_path;
    std::string external_syms_path;
    std::string manual_impl_path;
    std::string external_prefix;   // prepended to every PSYQ symbol name
    std::string manual_prefix;     // prepended to every manual-impl name
    std::string func_prefix = "func_"; // prefix for emitted func_<addr> names
    bool skip_on_error = false;
    std::vector<uint32_t> forced_entries;
    std::string emit_external_jals_path;
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
    fmt::print("Parsing PS1-EXE: {}\n", exe_path.string());

    auto exe = PSXRecomp::PS1ExeParser::parse_file(exe_path, error_msg);

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
    std::string exe_stem = exe_path.filename().string();

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
