#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <sstream>
#include <set>
#include "ps1_exe_parser.h"
#include "function_analysis.h"
#include "control_flow.h"
#include "../src/annotations.hpp"

namespace PSXRecomp {

// CPU state structure (registers + memory access)
struct CPUState {
    // General purpose registers (32 total)
    uint32_t gpr[32];  // $0-$31
    uint32_t pc;       // Program counter
    uint32_t hi, lo;   // Multiply/divide results

    // Memory access functions (to be implemented in runtime)
    uint32_t (*read_word)(uint32_t addr);
    void (*write_word)(uint32_t addr, uint32_t value);
    uint16_t (*read_half)(uint32_t addr);
    void (*write_half)(uint32_t addr, uint16_t value);
    uint8_t (*read_byte)(uint32_t addr);
    void (*write_byte)(uint32_t addr, uint8_t value);
};

// Code generation configuration
struct CodeGenConfig {
    bool emit_comments;           // Include MIPS instruction comments
    bool emit_line_numbers;       // Include original address as comments
    bool optimize_zero_reg;       // Optimize away $zero assignments
    bool use_switch_for_blocks;   // Use switch instead of goto labels
    bool emit_pc_tracking;        // Emit `cpu->pc = X;` before each insn
                                  // (debug builds only — adds one store
                                  // per emitted instruction). When set,
                                  // write-watchers and the state-diff
                                  // harness can pinpoint the writer.
    std::string indent;           // Indentation string (default: "    ")
    std::string func_prefix;      // Prepended to every emitted func_<addr> name
                                  // (default "func_"). Used to namespace per-overlay
                                  // outputs whose address ranges overlap.

    CodeGenConfig()
        : emit_comments(true)
        , emit_line_numbers(true)
        , optimize_zero_reg(true)
        , use_switch_for_blocks(false)
        , emit_pc_tracking(false)
        , indent("    ")
        , func_prefix("func_") {}
};

// Result of code generation for a function
struct GeneratedFunction {
    std::string function_name;    // e.g., "func_80016940"
    std::string signature;        // e.g., "void func_80016940(CPUState* cpu)"
    std::string body;             // C code body
    std::string full_code;        // signature + body
    int line_count;               // Number of lines generated
};

// A render→gameplay channel substitution site, GENERATED from
// games/spyro-1/manifest/render_channels.toml by tools/gen_channel_sites.py and loaded at
// runtime via --channel-sites. This keeps Spyro-specific addresses out of the
// recompiler source: the addresses + knob symbols live in the manifest, and
// the recompiler only knows how to emit each generic `kind`. See
// .claude/plans/openpete-generic-hook-anchor-refactor.md (step 1).
struct ChannelSite {
    enum Kind {
        OffsetAdd,   // addi: emit `rt = rs + <symbol>` at this PC+imm
        ConstLoad,   // lui/addiu: emit `rt = <symbol>` at this PC+imm (replaces
                     //       a hardcoded constant load with a runtime knob)
        AppendCall,  // any op: emit the verbatim translation, then append
                     //       `<symbol>(<operands...>);` — a call to a named C
                     //       hook. Moves bespoke per-site SIDE-EFFECT logic out
                     //       of the recompiler into a feature-module C fn.
        ReplaceCall, // any op: REPLACE the translation with
                     //       `<dest> = <symbol>(<operands...>);` — a hook that
                     //       returns the substituted value. Moves bespoke
                     //       value-computing logic (cone slope, canonical-SX,
                     //       polybuf-canon, …) out of the recompiler.
    };
    Kind        kind;
    int32_t     imm;      // OffsetAdd/ConstLoad: opcode immediate to match
                          // (disambiguates a reused PC; -0x10000 = any)
    std::string symbol;   // OffsetAdd/ConstLoad: knob symbol. Append/Replace:
                          // the hook function name. Emitted verbatim.
    // Append/ReplaceCall: operand register-roles passed to the hook, in order.
    // Roles: rd/rs/rt and named cpu fields (sp/a0..a3/v0/v1/t0..t9/s0..s7/...),
    // "gte<N>" (cpu->gte_data[N]), "imm" (sign-extended opcode immediate),
    // "off" (load/store offset), "cpu" (the CPUState*).
    std::vector<std::string> operands;
    std::string dest;     // ReplaceCall: dest register-role ("rd"/"rt"). Empty
                          // for the other kinds.
};

class CodeGenerator {
public:
    explicit CodeGenerator(const PS1Executable& exe, const CodeGenConfig& config = CodeGenConfig());

    // Generate C code for a single function.
    // fallthrough_name: if non-empty and the last block has no explicit control
    // flow (ControlFlowType::None), emit a tail call to that function before '}'.
    // This handles MIPS functions that fall through into the next function without
    // an explicit jr/j instruction.
    GeneratedFunction generate_function(
        const Function& func,
        const ControlFlowGraph& cfg,
        const std::string& fallthrough_name = "");

    // Generate C code for all functions
    std::vector<GeneratedFunction> generate_all_functions(
        const std::vector<Function>& functions,
        const std::map<uint32_t, ControlFlowGraph>& cfgs);

    // Generate complete C file with all functions
    std::string generate_file(
        const std::vector<Function>& functions,
        const std::map<uint32_t, ControlFlowGraph>& cfgs);

    // Set known functions for this compilation unit (for linking)
    void set_known_functions(const std::set<uint32_t>& functions) {
        known_functions_ = functions;
    }

    // Set annotation table (optional — no-op if not called)
    void set_annotations(const AnnotationTable* at) { annotations_ = at; }

    // External symbols: addresses in [range_start, range_end) are NOT
    // translated. JAL targets that resolve into the range are emitted as
    // calls to the named extern (PSYQ symbols supplied by PsyCross).
    void set_external_symbols(std::map<uint32_t, std::string> syms,
                              uint32_t range_start, uint32_t range_end) {
        external_symbols_ = std::move(syms);
        external_range_start_ = range_start;
        external_range_end_ = range_end;
    }

    // Manual implementations: addresses in this map are NOT translated. JAL
    // targets are emitted as calls to the named C function (typically a
    // hand-written port of a non-matching asm function).
    void set_manual_impls(std::map<uint32_t, std::string> impls) {
        manual_impls_ = std::move(impls);
    }

    // Skip-on-error: when codegen throws on a function, emit a stub that
    // logs+aborts at runtime and continue translating.
    void set_skip_on_error(bool v) { skip_on_error_ = v; }

    // Channel substitution sites (generated from render_channels.toml). The
    // translate_* handlers consult this map by address so render→gameplay
    // intercepts are data-driven instead of hardcoded. See ChannelSite.
    void set_channel_sites(std::map<uint32_t, ChannelSite> sites) {
        channel_sites_ = std::move(sites);
    }

    // Inspectors used by main_psx.cpp when emitting the dispatch table.
    // An address is "external" only if it has an entry in the symbol map.
    // Addresses inside the PSYQ range but without an exported name are
    // internal helpers — they get translated normally so they can be called
    // from any path (e.g. when PsyCross's PSYQ replacement falls through to
    // a PSYQ-internal helper that we haven't reimplemented).
    bool addr_is_external(uint32_t addr) const {
        return external_symbols_.count(addr) > 0;
    }
    const std::map<uint32_t, std::string>& external_symbols() const {
        return external_symbols_;
    }
    const std::map<uint32_t, std::string>& manual_impls() const {
        return manual_impls_;
    }
    uint32_t external_range_start() const { return external_range_start_; }
    uint32_t external_range_end()   const { return external_range_end_; }

private:
    const PS1Executable& exe_;
    CodeGenConfig config_;
    std::set<uint32_t> known_functions_;  // Addresses of functions in this compilation unit
    std::set<uint32_t> extra_labels_;    // Mid-block addresses that need inline labels (jump table targets)
    const AnnotationTable* annotations_ = nullptr;
    std::map<uint32_t, std::string> external_symbols_;
    uint32_t external_range_start_ = 0;
    uint32_t external_range_end_   = 0;
    std::map<uint32_t, std::string> manual_impls_;
    std::map<uint32_t, ChannelSite> channel_sites_;  // from render_channels.toml
    bool skip_on_error_ = false;

    // Resolve one operand register-role to its C expression for this insn.
    std::string resolve_channel_operand(const std::string& role,
                                        uint32_t instr) const;
    // Build the comma-separated argument list for a channel hook call.
    std::string channel_arg_list(const ChannelSite& cs, uint32_t instr) const;
    // Emit ` <hook>(<args>);` to append after the verbatim translation
    // (ChannelSite::AppendCall).
    std::string emit_channel_append_call(const ChannelSite& cs,
                                         uint32_t instr) const;
    // Emit `<dest> = <hook>(<args>);` replacing the translation
    // (ChannelSite::ReplaceCall).
    std::string emit_channel_replace_call(const ChannelSite& cs,
                                          uint32_t instr) const;

    const std::string* external_name_for(uint32_t addr) const {
        auto it = external_symbols_.find(addr);
        return it == external_symbols_.end() ? nullptr : &it->second;
    }
    const std::string* manual_name_for(uint32_t addr) const {
        auto it = manual_impls_.find(addr);
        return it == manual_impls_.end() ? nullptr : &it->second;
    }

    // Instruction translation
    std::string translate_instruction(uint32_t addr, uint32_t instr);

    // Register name mapping
    static std::string reg_name(int reg_num);

    // Basic block translation
    std::string translate_basic_block(
        const BasicBlock& block,
        const ControlFlowGraph& cfg);

    // Control flow translation
    std::string generate_branch_condition(uint32_t instr);
    std::string translate_branch(const ControlFlowInstr& cf, uint32_t fall_through);
    std::string translate_jump(const ControlFlowInstr& cf);

    // Arithmetic instructions
    std::string translate_add(uint32_t instr);
    std::string translate_addu(uint32_t instr);
    std::string translate_sub(uint32_t instr);
    std::string translate_subu(uint32_t instr);
    std::string translate_mult(uint32_t instr);
    std::string translate_multu(uint32_t instr);
    std::string translate_div(uint32_t instr);
    std::string translate_divu(uint32_t instr);
    std::string translate_mfhi(uint32_t instr);
    std::string translate_mflo(uint32_t instr);
    std::string translate_mthi(uint32_t instr);
    std::string translate_mtlo(uint32_t instr);

    // Logical instructions
    std::string translate_and(uint32_t instr);
    std::string translate_or(uint32_t instr);
    std::string translate_xor(uint32_t instr);
    std::string translate_nor(uint32_t instr);

    // Shift instructions
    std::string translate_sll(uint32_t instr);
    std::string translate_srl(uint32_t instr);
    std::string translate_sra(uint32_t instr);
    std::string translate_sllv(uint32_t instr);
    std::string translate_srlv(uint32_t instr);
    std::string translate_srav(uint32_t instr);

    // Load/store instructions
    std::string translate_lw(uint32_t instr);
    std::string translate_sw(uint32_t instr);
    std::string translate_lb(uint32_t instr);
    std::string translate_lbu(uint32_t instr);
    std::string translate_lh(uint32_t instr);
    std::string translate_lhu(uint32_t instr);
    std::string translate_lwl(uint32_t instr);
    std::string translate_lwr(uint32_t instr);
    std::string translate_swl(uint32_t instr);
    std::string translate_swr(uint32_t instr);
    std::string translate_sb(uint32_t instr, uint32_t addr);
    std::string translate_sh(uint32_t instr);

    // Immediate instructions
    std::string translate_addiu(uint32_t instr);
    std::string translate_andi(uint32_t instr);
    std::string translate_ori(uint32_t instr);
    std::string translate_xori(uint32_t instr);
    std::string translate_lui(uint32_t instr);

    // Set instructions
    std::string translate_slt(uint32_t instr);
    std::string translate_sltu(uint32_t instr);
    std::string translate_slti(uint32_t instr);
    std::string translate_sltiu(uint32_t instr);

    // Helper: Extract register fields
    static uint32_t get_rs(uint32_t instr) { return (instr >> 21) & 0x1F; }
    static uint32_t get_rt(uint32_t instr) { return (instr >> 16) & 0x1F; }
    static uint32_t get_rd(uint32_t instr) { return (instr >> 11) & 0x1F; }
    static uint32_t get_shamt(uint32_t instr) { return (instr >> 6) & 0x1F; }
    static uint32_t get_funct(uint32_t instr) { return instr & 0x3F; }
    static int16_t get_imm16(uint32_t instr) { return (int16_t)(instr & 0xFFFF); }
    static uint16_t get_imm16_u(uint32_t instr) { return (uint16_t)(instr & 0xFFFF); }
};

} // namespace PSXRecomp
