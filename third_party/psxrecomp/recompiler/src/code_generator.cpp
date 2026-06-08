#include "code_generator.h"
#include "control_flow.h"
#include <fmt/format.h>
#include <algorithm>
#include <cstdlib>
#include <set>
#include <vector>

namespace PSXRecomp {

CodeGenerator::CodeGenerator(const PS1Executable& exe, const CodeGenConfig& config)
    : exe_(exe), config_(config) {}

std::string CodeGenerator::reg_name(int reg_num) {
    // MIPS register names
    static const char* reg_names[32] = {
        "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
        "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
        "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
        "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"
    };

    if (reg_num >= 0 && reg_num < 32) {
        return std::string("cpu->") + reg_names[reg_num];
    }
    return fmt::format("cpu->gpr[{}]", reg_num);
}

std::string CodeGenerator::resolve_channel_operand(const std::string& role,
                                                   uint32_t instr) const {
    // Instruction-relative register roles.
    if (role == "rd") return reg_name(get_rd(instr));
    if (role == "rs") return reg_name(get_rs(instr));
    if (role == "rt") return reg_name(get_rt(instr));
    // The whole CPU state, for hooks that read several fixed fields.
    if (role == "cpu") return "cpu";
    // Sign-extended opcode immediate / load-store offset as an int literal.
    if (role == "imm" || role == "off")
        return fmt::format("{}", (int)(int16_t)get_imm16_u(instr));
    // cpu->gte_data[N].
    if (role.rfind("gte", 0) == 0 && role.size() > 3) {
        int idx = std::atoi(role.c_str() + 3);
        return fmt::format("cpu->gte_data[{}]", idx);
    }
    // Otherwise a named cpu GPR field (sp/a0..a3/v0/v1/t0..t9/s0..s7/...).
    return fmt::format("cpu->{}", role);
}

std::string CodeGenerator::channel_arg_list(const ChannelSite& cs,
                                            uint32_t instr) const {
    std::string args;
    for (size_t i = 0; i < cs.operands.size(); ++i) {
        if (i) args += ", ";
        args += resolve_channel_operand(cs.operands[i], instr);
    }
    return args;
}

std::string CodeGenerator::emit_channel_append_call(const ChannelSite& cs,
                                                    uint32_t instr) const {
    return fmt::format("  {}({});  /* channel append_call */",
                       cs.symbol, channel_arg_list(cs, instr));
}

std::string CodeGenerator::emit_channel_replace_call(const ChannelSite& cs,
                                                     uint32_t instr) const {
    std::string dest = (cs.dest == "rt") ? reg_name(get_rt(instr))
                     : (cs.dest == "rs") ? reg_name(get_rs(instr))
                     : reg_name(get_rd(instr));  // default rd
    return fmt::format("{} = {}({});  /* channel replace_call */",
                       dest, cs.symbol, channel_arg_list(cs, instr));
}

std::string CodeGenerator::translate_addiu(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t imm = get_imm16(instr);

    // Optimize: loading immediate into register (from $zero)
    if (rs == 0) {
        return fmt::format("{} = {};", reg_name(rt), imm);
    }

    // Optimize: don't assign to $zero
    if (config_.optimize_zero_reg && rt == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = {} + {};", reg_name(rt), reg_name(rs), imm);
}

std::string CodeGenerator::translate_lui(uint32_t instr) {
    uint32_t rt = get_rt(instr);
    uint16_t imm = get_imm16_u(instr);

    if (config_.optimize_zero_reg && rt == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = 0x{:04X} << 16;  /* 0x{:08X} */",
                       reg_name(rt), imm, ((uint32_t)imm) << 16);
}

std::string CodeGenerator::translate_lw(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    if (config_.optimize_zero_reg && rt == 0) {
        return "/* nop: load to $zero */";
    }

    if (offset == 0) {
        return fmt::format("{} = cpu->read_word({});", reg_name(rt), reg_name(rs));
    } else {
        return fmt::format("{} = cpu->read_word({} + {});",
                          reg_name(rt), reg_name(rs), offset);
    }
}

std::string CodeGenerator::translate_sw(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    if (offset == 0) {
        return fmt::format("cpu->write_word({}, {});", reg_name(rs), reg_name(rt));
    } else {
        return fmt::format("cpu->write_word({} + {}, {});",
                          reg_name(rs), offset, reg_name(rt));
    }
}

std::string CodeGenerator::translate_or(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    uint32_t rd = get_rd(instr);

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    // Optimize: move instruction (or $rd, $rs, $zero)
    if (rt == 0) {
        return fmt::format("{} = {};  /* move */", reg_name(rd), reg_name(rs));
    }
    if (rs == 0) {
        return fmt::format("{} = {};  /* move */", reg_name(rd), reg_name(rt));
    }

    return fmt::format("{} = {} | {};", reg_name(rd), reg_name(rs), reg_name(rt));
}

std::string CodeGenerator::translate_and(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    uint32_t rd = get_rd(instr);

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = {} & {};", reg_name(rd), reg_name(rs), reg_name(rt));
}

std::string CodeGenerator::translate_sll(uint32_t instr) {
    uint32_t rt = get_rt(instr);
    uint32_t rd = get_rd(instr);
    uint32_t shamt = get_shamt(instr);

    // nop instruction (sll $zero, $zero, 0)
    if (rd == 0 && rt == 0 && shamt == 0) {
        return "/* nop */";
    }

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    if (shamt == 0) {
        return fmt::format("{} = {};  /* no shift */", reg_name(rd), reg_name(rt));
    }

    return fmt::format("{} = {} << {};", reg_name(rd), reg_name(rt), shamt);
}

std::string CodeGenerator::translate_srl(uint32_t instr) {
    uint32_t rt = get_rt(instr);
    uint32_t rd = get_rd(instr);
    uint32_t shamt = get_shamt(instr);

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = (uint32_t){} >> {};", reg_name(rd), reg_name(rt), shamt);
}

std::string CodeGenerator::translate_slt(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    uint32_t rd = get_rd(instr);

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = ((int32_t){} < (int32_t){}) ? 1 : 0;",
                      reg_name(rd), reg_name(rs), reg_name(rt));
}

std::string CodeGenerator::translate_sltu(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    uint32_t rd = get_rd(instr);

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = ({} < {}) ? 1 : 0;",
                      reg_name(rd), reg_name(rs), reg_name(rt));
}

std::string CodeGenerator::translate_addu(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    uint32_t rd = get_rd(instr);

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    // Optimize: move instruction (addu $rd, $rs, $zero or addu $rd, $zero, $rt)
    if (rt == 0) {
        return fmt::format("{} = {};  /* move */", reg_name(rd), reg_name(rs));
    }
    if (rs == 0) {
        return fmt::format("{} = {};  /* move */", reg_name(rd), reg_name(rt));
    }

    return fmt::format("{} = {} + {};", reg_name(rd), reg_name(rs), reg_name(rt));
}

std::string CodeGenerator::translate_add(uint32_t instr) {
    // add and addu are functionally identical in practice (trap on overflow not used)
    return translate_addu(instr);
}

std::string CodeGenerator::translate_subu(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    uint32_t rd = get_rd(instr);

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = {} - {};", reg_name(rd), reg_name(rs), reg_name(rt));
}

std::string CodeGenerator::translate_sub(uint32_t instr) {
    // sub and subu are functionally identical in practice (trap on overflow not used)
    return translate_subu(instr);
}

std::string CodeGenerator::translate_lb(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    if (config_.optimize_zero_reg && rt == 0) {
        return "/* nop: load to $zero */";
    }

    if (offset == 0) {
        return fmt::format("{} = (int32_t)(int8_t)cpu->read_byte({});", reg_name(rt), reg_name(rs));
    } else {
        return fmt::format("{} = (int32_t)(int8_t)cpu->read_byte({} + {});",
                          reg_name(rt), reg_name(rs), offset);
    }
}

std::string CodeGenerator::translate_lbu(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    if (config_.optimize_zero_reg && rt == 0) {
        return "/* nop: load to $zero */";
    }

    if (offset == 0) {
        return fmt::format("{} = cpu->read_byte({});", reg_name(rt), reg_name(rs));
    } else {
        return fmt::format("{} = cpu->read_byte({} + {});",
                          reg_name(rt), reg_name(rs), offset);
    }
}

std::string CodeGenerator::translate_lh(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    if (config_.optimize_zero_reg && rt == 0) {
        return "/* nop: load to $zero */";
    }

    if (offset == 0) {
        return fmt::format("{} = (int32_t)(int16_t)cpu->read_half({});", reg_name(rt), reg_name(rs));
    } else {
        return fmt::format("{} = (int32_t)(int16_t)cpu->read_half({} + {});",
                          reg_name(rt), reg_name(rs), offset);
    }
}

std::string CodeGenerator::translate_lhu(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    if (config_.optimize_zero_reg && rt == 0) {
        return "/* nop: load to $zero */";
    }

    if (offset == 0) {
        return fmt::format("{} = cpu->read_half({});", reg_name(rt), reg_name(rs));
    } else {
        return fmt::format("{} = cpu->read_half({} + {});",
                          reg_name(rt), reg_name(rs), offset);
    }
}

std::string CodeGenerator::translate_lwl(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    if (config_.optimize_zero_reg && rt == 0) {
        return "/* nop: load to $zero */";
    }

    // LWL: Load Word Left - loads bytes into the most-significant portion of rt
    // addr = rs + offset; byte_pos = addr & 3
    // Result merges high bytes from aligned word with low bytes from current rt value
    // Generate a helper call: cpu->rt = cpu->lwl(addr, cpu->rt)
    if (offset == 0) {
        return fmt::format("{} = cpu->lwl({}, {});", reg_name(rt), reg_name(rs), reg_name(rt));
    } else {
        return fmt::format("{} = cpu->lwl({} + {}, {});",
                          reg_name(rt), reg_name(rs), (int32_t)offset, reg_name(rt));
    }
}

std::string CodeGenerator::translate_lwr(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    if (config_.optimize_zero_reg && rt == 0) {
        return "/* nop: load to $zero */";
    }

    // LWR: Load Word Right - loads bytes into the least-significant portion of rt
    // addr = rs + offset; byte_pos = addr & 3
    // Result merges low bytes from aligned word with high bytes from current rt value
    // Generate a helper call: cpu->rt = cpu->lwr(addr, cpu->rt)
    if (offset == 0) {
        return fmt::format("{} = cpu->lwr({}, {});", reg_name(rt), reg_name(rs), reg_name(rt));
    } else {
        return fmt::format("{} = cpu->lwr({} + {}, {});",
                          reg_name(rt), reg_name(rs), (int32_t)offset, reg_name(rt));
    }
}

std::string CodeGenerator::translate_swl(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    // SWL: Store Word Left - stores rt into memory using addr, MSB side
    // Generate a helper call: cpu->swl(addr, cpu->rt)
    if (offset == 0) {
        return fmt::format("cpu->swl({}, {});", reg_name(rs), reg_name(rt));
    } else {
        return fmt::format("cpu->swl({} + {}, {});",
                          reg_name(rs), (int32_t)offset, reg_name(rt));
    }
}

std::string CodeGenerator::translate_swr(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    // SWR: Store Word Right - stores rt into memory using addr, LSB side
    // Generate a helper call: cpu->swr(addr, cpu->rt)
    if (offset == 0) {
        return fmt::format("cpu->swr({}, {});", reg_name(rs), reg_name(rt));
    } else {
        return fmt::format("cpu->swr({} + {}, {});",
                          reg_name(rs), (int32_t)offset, reg_name(rt));
    }
}

std::string CodeGenerator::translate_sb(uint32_t instr, uint32_t addr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    /* m_WasDrawn iter-start clear sites in the per-bucket render walkers
     * (0x8001F1FC regular, 0x800209A8 specular, 0x80022B28 shaded). Set
     * `psx_runtime_native_cone_pass = 1` here so billboard / extended-
     * radius mobys that bypass the cone-test substitution still get
     * their m_WasDrawn=1 write through; the cone-test substitution
     * (sub at 0x8001F30C / 0x80020AB8, addi at 0x80022C0C) overrides to
     * 0 when it culls at native cone. */
    if (addr == 0x8001F1FCu || addr == 0x800209A8u || addr == 0x80022B28u) {
        if (offset == 0) {
            return fmt::format(
                "psx_runtime_native_cone_pass = 1; psx_runtime_native_dist_pass = 1; cpu->write_byte({}, (uint8_t){});",
                reg_name(rs), reg_name(rt));
        } else {
            return fmt::format(
                "psx_runtime_native_cone_pass = 1; psx_runtime_native_dist_pass = 1; cpu->write_byte({} + {}, (uint8_t){});",
                reg_name(rs), offset, reg_name(rt));
        }
    }

    /* m_WasDrawn=1 cull-pass write sites: gate on the native cone AND
     * native distance verdicts (the two halves of the canonical box-cull).
     * native_dist_pass closes the --no-lod m_Distance-saturation leak. */
    if (addr == 0x8001F4CCu || addr == 0x80020C7Cu || addr == 0x80022D98u) {
        if (offset == 0) {
            return fmt::format(
                "if (psx_runtime_native_cone_pass && psx_runtime_native_dist_pass) cpu->write_byte({}, (uint8_t){});",
                reg_name(rs), reg_name(rt));
        } else {
            return fmt::format(
                "if (psx_runtime_native_cone_pass && psx_runtime_native_dist_pass) cpu->write_byte({} + {}, (uint8_t){});",
                reg_name(rs), offset, reg_name(rt));
        }
    }

    /* Particle "drawn" flag (m_03 of each Particle, written via the
     * handwritten `sb $s2, -0x1D($ra)` idiom in r_particles.s — $ra
     * is the iteration cursor pre-incremented by 0x20 to the next
     * particle, so -0x1D reaches the *current* particle's m_03).
     *
     * Same shape as m_WasDrawn for mobys: render path emits prims
     * for any particle that passes the *widened* screen cull (visual
     * coverage extends beyond 4:3), but the gameplay-readable byte
     * itself must reflect the canonical 4:3 verdict — class-specific
     * particle update handlers in the level overlays read m_03 and
     * gate expensive work (including rand() calls) on it; an extra
     * "drawn" particle in 16:9 would call rand() more times and
     * drift psyq_rand_seed across aspects.
     *
     * Verdict is computed at the preceding cull (substituted in
     * translate_special / translate_sub: `sub $a0, $a0, $s6`); see
     * psx_runtime.h `psx_runtime_native_part_pass`. */
    if (addr == 0x80057720u || addr == 0x80057A10u
     || addr == 0x80057B20u || addr == 0x80057CDCu
     || addr == 0x80057F60u || addr == 0x80058028u) {
        if (offset == 0) {
            return fmt::format(
                "if (psx_runtime_native_part_pass) cpu->write_byte({}, (uint8_t){});",
                reg_name(rs), reg_name(rt));
        } else {
            return fmt::format(
                "if (psx_runtime_native_part_pass) cpu->write_byte({} + {}, (uint8_t){});",
                reg_name(rs), offset, reg_name(rt));
        }
    }

    if (offset == 0) {
        return fmt::format("cpu->write_byte({}, (uint8_t){});", reg_name(rs), reg_name(rt));
    } else {
        return fmt::format("cpu->write_byte({} + {}, (uint8_t){});",
                          reg_name(rs), offset, reg_name(rt));
    }
}

std::string CodeGenerator::translate_sh(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t offset = get_imm16(instr);

    if (offset == 0) {
        return fmt::format("cpu->write_half({}, (uint16_t){});", reg_name(rs), reg_name(rt));
    } else {
        return fmt::format("cpu->write_half({} + {}, (uint16_t){});",
                          reg_name(rs), offset, reg_name(rt));
    }
}

std::string CodeGenerator::translate_sra(uint32_t instr) {
    uint32_t rt = get_rt(instr);
    uint32_t rd = get_rd(instr);
    uint32_t shamt = get_shamt(instr);

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = (int32_t){} >> {};", reg_name(rd), reg_name(rt), shamt);
}

std::string CodeGenerator::translate_andi(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    uint16_t imm = get_imm16_u(instr);

    if (config_.optimize_zero_reg && rt == 0) {
        return "/* nop: write to $zero */";
    }

    /* Spyro's handwritten env renderer (r_environment.s) builds a
     * per-vertex screen-bounds clip mask via `andi $reg, SXY2, 0xFE00`
     * — bits 9..15 of SX, rejecting SX outside [0, 512). For widescreen
     * SX legitimately exceeds 512; substitute the literal with a
     * runtime-tunable mask so openpete can widen it without altering
     * native PSX behavior (default 0xFE00 == identity). See
     * psx_runtime.h `psx_runtime_clip_x_mask`. */
    if (imm == 0xFE00u) {
        return fmt::format("{} = {} & psx_runtime_clip_x_mask;",
                           reg_name(rt), reg_name(rs));
    }

    return fmt::format("{} = {} & 0x{:04X};", reg_name(rt), reg_name(rs), imm);
}

std::string CodeGenerator::translate_ori(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    uint16_t imm = get_imm16_u(instr);

    if (config_.optimize_zero_reg && rt == 0) {
        return "/* nop: write to $zero */";
    }

    // Optimize: loading immediate into register (from $zero)
    if (rs == 0) {
        return fmt::format("{} = 0x{:04X};", reg_name(rt), imm);
    }

    return fmt::format("{} = {} | 0x{:04X};", reg_name(rt), reg_name(rs), imm);
}

std::string CodeGenerator::translate_xori(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    uint16_t imm = get_imm16_u(instr);

    if (config_.optimize_zero_reg && rt == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = {} ^ 0x{:04X};", reg_name(rt), reg_name(rs), imm);
}

std::string CodeGenerator::translate_xor(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    uint32_t rd = get_rd(instr);

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = {} ^ {};", reg_name(rd), reg_name(rs), reg_name(rt));
}

std::string CodeGenerator::translate_nor(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    uint32_t rd = get_rd(instr);

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    // Optimize: not instruction (nor $rd, $rs, $zero)
    if (rt == 0) {
        return fmt::format("{} = ~{};  /* not */", reg_name(rd), reg_name(rs));
    }
    if (rs == 0) {
        return fmt::format("{} = ~{};  /* not */", reg_name(rd), reg_name(rt));
    }

    return fmt::format("{} = ~({} | {});", reg_name(rd), reg_name(rs), reg_name(rt));
}

std::string CodeGenerator::translate_sllv(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    uint32_t rd = get_rd(instr);

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = {} << ({} & 0x1F);", reg_name(rd), reg_name(rt), reg_name(rs));
}

std::string CodeGenerator::translate_srlv(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    uint32_t rd = get_rd(instr);

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = (uint32_t){} >> ({} & 0x1F);", reg_name(rd), reg_name(rt), reg_name(rs));
}

std::string CodeGenerator::translate_srav(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    uint32_t rd = get_rd(instr);

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = (int32_t){} >> ({} & 0x1F);", reg_name(rd), reg_name(rt), reg_name(rs));
}

std::string CodeGenerator::translate_slti(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t imm = get_imm16(instr);

    if (config_.optimize_zero_reg && rt == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = ((int32_t){} < {}) ? 1 : 0;",
                      reg_name(rt), reg_name(rs), imm);
}

std::string CodeGenerator::translate_sltiu(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);
    int16_t imm = get_imm16(instr);  // Sign-extended for comparison

    if (config_.optimize_zero_reg && rt == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = ({} < (uint32_t){}) ? 1 : 0;",
                      reg_name(rt), reg_name(rs), imm);
}

std::string CodeGenerator::translate_mult(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);

    return fmt::format("{{ int64_t result = (int64_t)(int32_t){} * (int64_t)(int32_t){}; cpu->lo = (uint32_t)result; cpu->hi = (uint32_t)(result >> 32); }}",
                      reg_name(rs), reg_name(rt));
}

std::string CodeGenerator::translate_multu(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);

    return fmt::format("{{ uint64_t result = (uint64_t){} * (uint64_t){}; cpu->lo = (uint32_t)result; cpu->hi = (uint32_t)(result >> 32); }}",
                      reg_name(rs), reg_name(rt));
}

std::string CodeGenerator::translate_div(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);

    // MIPS division: quotient in LO, remainder in HI
    // Handle division by zero (MIPS behavior: result is unpredictable but doesn't trap)
    return fmt::format("if ({} != 0) {{ cpu->lo = (uint32_t)((int32_t){} / (int32_t){}); cpu->hi = (uint32_t)((int32_t){} % (int32_t){}); }} else {{ cpu->lo = ((int32_t){} >= 0) ? 0xFFFFFFFF : 1; cpu->hi = (uint32_t){}; }}",
                      reg_name(rt), reg_name(rs), reg_name(rt), reg_name(rs), reg_name(rt), reg_name(rs), reg_name(rs));
}

std::string CodeGenerator::translate_divu(uint32_t instr) {
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);

    // MIPS division: quotient in LO, remainder in HI
    // Handle division by zero (MIPS behavior: result is unpredictable but doesn't trap)
    return fmt::format("if ({} != 0) {{ cpu->lo = {} / {}; cpu->hi = {} % {}; }} else {{ cpu->lo = 0xFFFFFFFF; cpu->hi = {}; }}",
                      reg_name(rt), reg_name(rs), reg_name(rt), reg_name(rs), reg_name(rt), reg_name(rs));
}

std::string CodeGenerator::translate_mfhi(uint32_t instr) {
    uint32_t rd = get_rd(instr);

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = cpu->hi;", reg_name(rd));
}

std::string CodeGenerator::translate_mflo(uint32_t instr) {
    uint32_t rd = get_rd(instr);

    if (config_.optimize_zero_reg && rd == 0) {
        return "/* nop: write to $zero */";
    }

    return fmt::format("{} = cpu->lo;", reg_name(rd));
}

std::string CodeGenerator::translate_mthi(uint32_t instr) {
    uint32_t rs = get_rs(instr);

    return fmt::format("cpu->hi = {};", reg_name(rs));
}

std::string CodeGenerator::translate_mtlo(uint32_t instr) {
    uint32_t rs = get_rs(instr);

    return fmt::format("cpu->lo = {};", reg_name(rs));
}

std::string CodeGenerator::generate_branch_condition(uint32_t instr) {
    uint32_t opcode = (instr >> 26) & 0x3F;
    uint32_t rs = get_rs(instr);
    uint32_t rt = get_rt(instr);

    // REGIMM branches (bltz, bgez, bltzal, bgezal)
    if (opcode == 0x01) {
        uint32_t regimm_op = (instr >> 16) & 0x1F;
        if (regimm_op == 0x00) { // bltz
            return fmt::format("(int32_t){} < 0", reg_name(rs));
        } else if (regimm_op == 0x01) { // bgez
            return fmt::format("(int32_t){} >= 0", reg_name(rs));
        } else if (regimm_op == 0x10) { // bltzal
            return fmt::format("(int32_t){} < 0", reg_name(rs));
        } else if (regimm_op == 0x11) { // bgezal
            return fmt::format("(int32_t){} >= 0", reg_name(rs));
        }
    }

    // Standard branches
    switch (opcode) {
        case 0x04: // beq
        case 0x14: // beql
            return fmt::format("{} == {}", reg_name(rs), reg_name(rt));

        case 0x05: // bne
        case 0x15: // bnel
            return fmt::format("{} != {}", reg_name(rs), reg_name(rt));

        case 0x06: // blez
        case 0x16: // blezl
            return fmt::format("(int32_t){} <= 0", reg_name(rs));

        case 0x07: // bgtz
        case 0x17: // bgtzl
            return fmt::format("(int32_t){} > 0", reg_name(rs));
    }

    return "0 /* unknown branch condition: defaults to not-taken */";
}

std::string CodeGenerator::translate_instruction(uint32_t addr, uint32_t instr) {
    uint32_t opcode = (instr >> 26) & 0x3F;
    uint32_t funct = instr & 0x3F;

    // Instruction comment (if enabled)
    std::string comment;
    if (config_.emit_comments) {
        comment = fmt::format("  /* 0x{:08X}: 0x{:08X} */", addr, instr);
    }

    std::string code;

    // SPECIAL opcode (0x00)
    if (opcode == 0x00) {
        switch (funct) {
            case 0x00:                                          // sll
                if (addr == 0x800576ECu) {
                    /* Entry to r_particles.s 4-vertex screen-cull chain
                     * preceding `sb $s2, -0x1D($ra)` at 0x80057720. At
                     * this PC, the four vertex SXs are in $at, $v0, $v1,
                     * $a0 (low halves). The cull chain short-circuits on
                     * widened pass (`bltz $a1, .L8005771C`) which would
                     * skip later canonical tests; we instead compute the
                     * full canonical OR here, unconditionally, using all
                     * four vertices. The sb at 0x80057720 then gates on
                     * `psx_runtime_native_part_pass`. The actual sll
                     * (`$a1 = $at << 16`) still executes for the widened
                     * cull chain to evaluate normally.
                     *
                     * Threshold note: OFX widening shifts SX uniformly by
                     * (factor-1)*256, so the canonical 4:3 cull at SX<0x200
                     * becomes SX_widened < (0x200 + delta_OFX) in widened
                     * coords. That's psx_runtime_native_clip_x_in_widened_space. */
                    /* Canonical 4:3 strip-pass over 4 vertices — read
                     * the canonical-SX side channel populated by gte.cpp
                     * for each RTPS/RTPT-internal call. The 4-vertex
                     * sequence is: RTPT (v0/v1/v2 → SXY0/1/2 → canonical
                     * sx0/1/2), then a separate RTPS for v3 (overwrites
                     * SXY2, FIFO shifts so the v3 canonical sx is in
                     * sx2 and v0's canonical sx falls off). Asm uses
                     * mfc2 of SXY0/SXY1/SXY2_pre + SXY2_post — for the
                     * canonical verdict we approximate with the FIFO
                     * tail: any-of-3 = vertices 1, 2, 3 (skipping v0).
                     * Per-vertex test: 0 < sx_q16 < 0x02000000. */
                    code = fmt::format(
                        "{{ int32_t s0 = (int32_t)(openpete_canonical_sx0_q16 >> 16); "
                        "int32_t s1 = (int32_t)(openpete_canonical_sx1_q16 >> 16); "
                        "int32_t s2 = (int32_t)(openpete_canonical_sx2_q16 >> 16); "
                        "uint8_t pass = (uint8_t)( "
                        "((s0 > 0) & (s0 < 512)) "
                        "| ((s1 > 0) & (s1 < 512)) "
                        "| ((s2 > 0) & (s2 < 512))); "
                        "psx_runtime_native_part_pass = pass; "
                        "cpu->a1 = (int32_t)cpu->at << 16; }}");
                } else if (addr == 0x80058648u) {
                    /* Sparkle render-cull SX-left-clip entry in
                     * r_particles.s func_80058864. Original asm:
                     *   sll $a1, $a3, 16    ; $a1 = SX_widened << 16
                     *   blez $a1, .kill     ; canonical-style left-clip
                     *   lui $t0, 0x0200
                     *   sub $a1, $a1, $t0   ; right-clip
                     *   bgez $a1, .kill
                     *   ... else continue (sparkle stays alive)
                     * .kill clears in_use byte → frees slot, leaks into
                     * gameplay (allocator state cascades into next-frame
                     * SpawnSparkleAt slot assignments which gate rotation
                     * matrix updates via UPDATE_PROP_ROTMATRIX).
                     *
                     * Substitute $a1 with canonical SX_q16 (truncated to
                     * int via >> 16, then re-shifted to match the asm's
                     * shape). Both blez (left) and the subsequent sub-
                     * then-bgez (right at 0x80058654) now test canonical
                     * SX instead of widened, so the kill verdict matches
                     * canonical 4:3 — visible periphery in widened mode
                     * still renders for one frame (sparkle is on its
                     * normal alive path), then dies the SAME frame
                     * canonical would. */
                    code = "{ int32_t cs_q16 = (int32_t)(openpete_canonical_sx2_q16 >> 16) << 16; cpu->a1 = cs_q16; }";
                } else {
                    code = translate_sll(instr);
                }
                break;
            case 0x02: code = translate_srl(instr); break;     // srl
            case 0x03: code = translate_sra(instr); break;     // sra
            case 0x04: code = translate_sllv(instr); break;    // sllv
            case 0x06: code = translate_srlv(instr); break;    // srlv
            case 0x07: code = translate_srav(instr); break;    // srav
            case 0x08:                                          // jr
                {
                    /* If rs == $ra (31), this is a function return.
                     * Otherwise this is an indirect tail call (e.g., switch
                     * jump table). Dispatch through the runtime so the
                     * target's recompiled body executes, then return.
                     * (jr in a normal control-flow position is handled by
                     *  translate_basic_block; this fallback covers delay
                     *  slots and any caller that bypasses the exit path.) */
                    uint32_t rs = get_rs(instr);
                    if (rs == 31) {
                        code = "return;  /* jr $ra */";
                    } else {
                        code = fmt::format("call_by_address(cpu, {});  return;  /* jr {} */",
                                           reg_name(rs), reg_name(rs));
                    }
                }
                break;
            case 0x09:                                          // jalr
                {
                    uint32_t rs = get_rs(instr);
                    uint32_t rd = get_rd(instr);
                    code = fmt::format("{} = 0x{:08X};  cpu->pc = {};  /* jalr */",
                                      reg_name(rd), addr + 8, reg_name(rs));
                }
                break;
            case 0x0C:                                          // syscall
                {
                    uint32_t syscall_code = (instr >> 6) & 0xFFFFF;
                    code = fmt::format("psx_syscall(cpu, {});  /* syscall {} */", syscall_code, syscall_code);
                }
                break;
            case 0x0D:                                          // break
                {
                    uint32_t break_code = (instr >> 6) & 0xFFFFF;
                    code = fmt::format("/* break({}) — trap, no-op in recompiler */", break_code);
                }
                break;
            case 0x10: code = translate_mfhi(instr); break;    // mfhi
            case 0x11: code = translate_mthi(instr); break;    // mthi
            case 0x12: code = translate_mflo(instr); break;    // mflo
            case 0x13: code = translate_mtlo(instr); break;    // mtlo
            case 0x18: code = translate_mult(instr); break;    // mult
            case 0x19: code = translate_multu(instr); break;   // multu
            case 0x1A: code = translate_div(instr); break;     // div
            case 0x1B: code = translate_divu(instr); break;    // divu
            case 0x20:                                          // add
                /* m_WasDrawn distance-channel: box-cull lower-bound tests
                 * (channel no_lod_moby_distance_cull_verdict). The recompiler
                 * appends a CALL to psx_runtime_dist_gate_lower(rd) after the
                 * canonical add; that hook (core/features/widescreen.c) clears
                 * native_dist_pass if the canonical 4:3 cull would reject. The
                 * verdict math lives in the hook, not here — addresses + hook
                 * come from manifest/render_channels.toml via --channel-sites. */
                if (auto _cs = channel_sites_.find(addr);
                    _cs != channel_sites_.end()
                    && _cs->second.kind == ChannelSite::AppendCall) {
                    code = translate_add(instr)
                         + emit_channel_append_call(_cs->second, instr);
                } else {
                    code = translate_add(instr);
                }
                break;
            case 0x21: code = translate_addu(instr); break;    // addu
            case 0x22:                                          // sub
                {
                    /* Spyro r_moby.s func_8001F158 / func_800208FC inline
                     * cone-X cull at 0x8001F30C / 0x80020AB8:
                     *   sub t2, t2, a2    # t2 = |MAC1| - a2 (per-moby x-radius)
                     * is the start of the same shape as func_80022A2C's
                     * cone test, just with per-moby radius instead of
                     * fixed 102/77. The full chain:
                     *   sub t2, t2, a2; sll t2, 2; sub t2, t2, t3;
                     *   sll t3, 1; sub t2, t2, t3; bgez t2 → cull
                     *   (where t3 = MAC3 + a3)
                     * encodes slope 0.75 (= 4:3 horiz half-FOV). Substitute
                     * to also subtract (mac3p * extra_q12) >> 12 from t2 —
                     * after the subsequent sll/sub chain the final test
                     * becomes
                     *   bgez (4*(|x|-a2) - (3 + 4*extra)*mac3p)
                     * with slope (3 + 4*extra)/4, matching the shaded path
                     * at 0x80022C0C. Default extra=0 ⇒ slope 0.75 unchanged.
                     * Address-explicit so unrelated `sub r,r,r` stay
                     * untouched.
                     *
                     * Historical note: this site originally used `>> 14`
                     * (commit 7be4a6f) under the mistaken belief that it
                     * produced the same slope-widening as the shaded path.
                     * It actually under-widened by 4× — at extra_q12=2309
                     * (21:9 calibration) regular gave slope 0.891 vs the
                     * required 1.314, so regular-path enemies culled
                     * ~145 px inside the visible 21:9 viewport. Fixed to
                     * `>> 12` (Phase 7.1 follow-up). */
                    if (addr == 0x8001F30Cu || addr == 0x80020AB8u) {
                        /* Compute the *native* (4:3) cone-test verdict
                         * alongside the widened substitution. The full
                         * native chain after this sub + the sll/sub/sll/sub
                         * tail is `4*(|MAC1|-a2) - 3*(MAC3+a3)`. We have
                         * |MAC1|=$rs, a2=$rt, MAC3=cpu->v1, a3=cpu->a3
                         * here, so the test result can be evaluated in one
                         * expression. Stash to a host-side flag; the
                         * substituted m_WasDrawn=1 write at 0x8001F4CC /
                         * 0x80020C7C reads it. See psx_runtime.h
                         * `psx_runtime_native_cone_pass`.
                         *
                         * Widening clamp: when (MAC3+a3) < 0 (moby near or
                         * behind the camera plane) the widening multiply
                         * produces a NEGATIVE term, which would tighten
                         * the cone in 16:9 instead of widening it and
                         * cull mobys 4:3 admits. Clamp to >= 0 — the
                         * X-cone is meaningless for behind-Z anyway. */
                        uint32_t rs = get_rs(instr);
                        uint32_t rt = get_rt(instr);
                        uint32_t rd = get_rd(instr);
                        code = fmt::format(
                            "psx_runtime_native_cone_pass = "
                            "(uint8_t)((4 * ((int32_t){} - (int32_t){}) "
                            "- 3 * ((int32_t)cpu->v1 + (int32_t)cpu->a3)) < 0); "
                            "{{ int32_t mac3p = (int32_t)cpu->v1 + (int32_t)cpu->a3; "
                            "if (mac3p < psx_runtime_moby_cone_mac3p_floor) mac3p = psx_runtime_moby_cone_mac3p_floor;"
                            "{} = (int32_t){} - (int32_t){} "
                            "- (int32_t)(((int64_t)mac3p "
                            "* (int64_t)psx_runtime_moby_cone_extra_q12) >> 12) "
                            "- (int32_t)(((int64_t)(int32_t){} "
                            "* (int64_t)psx_runtime_moby_cone_bias_scale_q12) >> 12); }}",
                            reg_name(rs), reg_name(rt),
                            reg_name(rd), reg_name(rs), reg_name(rt),
                            reg_name(rt));
                    } else if (addr == 0x80057A04u || addr == 0x80057B14u
                            || addr == 0x80057CB4u || addr == 0x80057F28u
                            || addr == 0x8005801Cu) {
                        /* FIFO slot selector: class 4 (PC A04) and class 1
                         * (PC F28) do TWO RTPSes per particle and capture
                         * SXY2 BETWEEN them via mfc2, so the cull tests
                         * the OLDER projection — read sx1 (which holds the
                         * stash from RTPS#1 after RTPS#2 advanced the
                         * FIFO). Other classes do a single RTPS and test
                         * its SXY2 directly — read sx2. */
                        const char* canonical_slot =
                            (addr == 0x80057A04u || addr == 0x80057F28u)
                            ? "openpete_canonical_sx1_q16"
                            : "openpete_canonical_sx2_q16";
                        /* r_particles.s screen-cull verdict (single-vertex
                         * sites preceding `sb $s2, -0x1D($ra)` at
                         * 0x80057A10 / 0x80057B20 / 0x80057CDC). $rs holds
                         * (vertex_SX << 16); $rt is the widened
                         * psx_runtime_clip_x_threshold. Record canonical
                         * verdict against the unwidened threshold
                         * (0x02000000 == 4:3 X-edge), then perform the
                         * widened sub for the actual visual cull. The sb
                         * substitution gates on
                         * `psx_runtime_native_part_pass`. */
                        uint32_t rs = get_rs(instr);
                        uint32_t rt = get_rt(instr);
                        uint32_t rd = get_rd(instr);
                        code = fmt::format(
                            /* Canonical 4:3 verdict from gte.cpp's
                             * un-widened SX side channel. Replicate the
                             * asm cull's int-truncation: native asm reads
                             * `mfc2 C2_SXY2` which truncates to int16,
                             * then `sll 16; blez/bgez` tests SX_int > 0
                             * and SX_int < 512. Match by truncating
                             * canonical_q16 to int via `>> 16`. */
                            "{{ int32_t cs = (int32_t)({} >> 16); "
                            "psx_runtime_native_part_pass = "
                            "(uint8_t)((cs > 0) & (cs < 512)); }} "
                            "{} = (int32_t){} - (int32_t){};",
                            canonical_slot,
                            reg_name(rd), reg_name(rs), reg_name(rt));
                    /* Note: the 4-vertex OR chain leading to
                     * sb at 0x80057720 (subs at 0x800576F0/FC/708/714)
                     * is intentionally NOT substituted here — the chain
                     * short-circuits on widened-pass which would skip
                     * later canonical tests. Instead the entry `sll`
                     * at 0x800576EC computes the canonical OR over all
                     * 4 vertices unconditionally (see translate sll
                     * handler). */
                    } else if (auto _cs = channel_sites_.find(addr);
                               _cs != channel_sites_.end()
                               && _cs->second.kind == ChannelSite::AppendCall) {
                        /* m_WasDrawn distance-channel: box-cull upper-bound +
                         * post-MVMVA proj-Z cull tests (channel
                         * no_lod_moby_distance_cull_verdict). The recompiler
                         * appends a CALL to psx_runtime_dist_gate_upper(rd)
                         * after the canonical sub; the hook
                         * (core/features/widescreen.c) clears native_dist_pass
                         * if the canonical 4:3 cull rejects, closing the
                         * --no-lod m_Distance-saturation leak (a saturated
                         * far-boundary moby would otherwise spawn its gem a
                         * tick early and leak into g_Sparkles). The verdict
                         * math is in the hook; addresses + hook come from
                         * manifest/render_channels.toml via --channel-sites. */
                        code = translate_sub(instr)
                             + emit_channel_append_call(_cs->second, instr);
                    } else {
                        code = translate_sub(instr);
                    }
                }
                break;
            case 0x23: code = translate_subu(instr); break;    // subu
            case 0x24: code = translate_and(instr); break;     // and
            case 0x25: code = translate_or(instr); break;      // or
            case 0x26:                                          // xor
                /* §7.5-R3 dominant-terrain world-geo tag hook (channel
                 * world_geo_strip_tag). r_environment.s 0x80026238
                 * `xor $t7,$t7,$s1` yields the aligned strip-base pointer in
                 * $t7 (= rd); the recompiler appends
                 * openpete_world_geo_set_current(rd) so gte_rtps_internal can
                 * stamp a stable cross-frame WORLD batch tag. Address +
                 * operand + hook live in manifest/render_channels.toml via
                 * --channel-sites (AppendCall, operand="rd"). READ-ONLY
                 * host-side stash; no gameplay byte touched. */
                if (auto _cs = channel_sites_.find(addr);
                    _cs != channel_sites_.end()
                    && _cs->second.kind == ChannelSite::AppendCall) {
                    code = translate_xor(instr)
                         + emit_channel_append_call(_cs->second, instr);
                } else {
                    code = translate_xor(instr);
                }
                break;
            case 0x27: code = translate_nor(instr); break;     // nor
            case 0x2A: code = translate_slt(instr); break;     // slt
            case 0x2B: code = translate_sltu(instr); break;    // sltu
            default:
                code = fmt::format("/* TODO: SPECIAL funct=0x{:02X} */", funct);
        }
    }
    // Immediate and load/store instructions
    else {
        switch (opcode) {
            case 0x08:                                         // addi
                {
                    /* Spyro r_moby.s func_80022A2C inline cone-X cull at
                     * 0x80022C0C: `addi a1, a1, -0x66` (a1 = |MAC1| - 102)
                     * is part of the test
                     *   bgez (4*(|MAC1|-102) - 3*(MAC3+77)) → cull
                     * with slope 3/4 = 4:3 horizontal half-FOV. Substitute
                     * to also subtract `extra*(MAC3+77)` from a1 so the
                     * after-shifts test becomes
                     *   bgez (4*|MAC1| - 408 - (3 + 4*extra)*(MAC3+77))
                     * which has slope (3+4*extra)/4. Default 0 = native;
                     * widescreen sets psx_runtime_moby_cone_extra_q12 > 0.
                     * MAC3 = cpu->gte_data[27]. Address-explicit because
                     * `addi $r, $r, -0x66` appears in many unrelated
                     * contexts. */
                    uint32_t rs = get_rs(instr);
                    uint32_t rt = get_rt(instr);
                    int16_t imm = (int16_t)get_imm16_u(instr);
                    /* env-renderer + moby queue-head/limit computations are
                     * `addi $rt, $rs, +imm` sites where $rs holds a pool base
                     * and the addi shifts to a queue head/limit. Their
                     * addresses, immediates and knob symbols live in
                     * manifest/render_channels.toml (channels
                     * no_lod_env_queue_offsets / no_lod_moby_reg_queue),
                     * loaded via --channel-sites — NOT hardcoded here, so this
                     * shape carries zero Spyro addresses in the recompiler.
                     * The OffsetAdd kind emits `rt = rs + <knob>`, identity at
                     * default knob value. PC+imm keyed (the imm disambiguates a
                     * PC whose `addi $r,$r,0x2000` recurs in non-pool integer
                     * arithmetic — e.g. Z-saturation chains at 0x80026DEC). See
                     * .claude/plans/openpete-generic-hook-anchor-refactor.md. */
                    if (auto _cs = channel_sites_.find(addr);
                        _cs != channel_sites_.end()
                        && _cs->second.kind == ChannelSite::OffsetAdd
                        && (int32_t)imm == _cs->second.imm)
                    {
                        code = fmt::format(
                            "{} = (int32_t){} + (int32_t){};"
                            "  /* channel offset_add: {} */",
                            reg_name(rt), reg_name(rs),
                            _cs->second.symbol, _cs->second.symbol);
                    } else if (addr == 0x80026A34u && imm == 0x100) {
                        /* r_environment.s func_800258F0 per-vertex
                         * lateral-bound cull, +0x100 site (X axis).
                         * See psx_runtime.h `psx_runtime_env_cell_x_bound`. */
                        code = fmt::format(
                            "{} = (int32_t){} + (int32_t)psx_runtime_env_cell_x_bound;",
                            reg_name(rt), reg_name(rs));
                    } else if (addr == 0x80026A3Cu && imm == -0x200) {
                        /* Companion -0x200 site for the +0x100 above.
                         * Subtract 2× bound to land at (X - bound). */
                        code = fmt::format(
                            "{} = (int32_t){} - 2 * (int32_t)psx_runtime_env_cell_x_bound;",
                            reg_name(rt), reg_name(rs));
                    } else if (addr == 0x80026F94u
                            || addr == 0x80027064u
                            || addr == 0x80027484u) {
                        /* Phase 3 (texture LOD): force HQ tier in HP-walk.
                         *
                         * The env renderer's `m_LQTexturePointer` is
                         * misnamed: each 16-byte entry packs *both* a
                         * HQ descriptor (bytes 0..7) and an LQ
                         * descriptor (bytes 8..15). HP-walk does a
                         * per-quad screen-size test (`bgtz $t0` at PC
                         * 0x80026F88 / 0x80027058 / 0x80027478); when
                         * the polygon is below the threshold, it falls
                         * through to one of these three identical
                         *   addi $t5, $t5, 0x8
                         * instructions, bumping $t5 from the HQ half
                         * of the entry to the LQ half.
                         *
                         * Suppressing the +8 bump under --no-lod
                         * forces every quad to read the HQ half,
                         * regardless of screen size. Identity at
                         * default. Pure renderer-side: $t5 is consumed
                         * only by the GTE-side texture-descriptor
                         * fetches that follow. */
                        code = fmt::format(
                            "{} = openpete_no_lod ? (int32_t){} : ((int32_t){} + ({}));"
                            "  /* env HP-walk LQ-tier select (no-lod) */",
                            reg_name(rt), reg_name(rs),
                            reg_name(rs), (int)imm);
                    } else if (addr == 0x80022C0Cu && imm == -0x66) {
                        /* Native (4:3) cone-test verdict for the m_WasDrawn
                         * gating at 0x80022D98. Native chain after this
                         * addi is `4*(|MAC1|-102) - 3*(MAC3+77)`; here
                         * |MAC1|=$rs, MAC3=cpu->gte_data[27]. See
                         * psx_runtime.h `psx_runtime_native_cone_pass`.
                         *
                         * Widening: subtract `extra*(MAC3+77)` from the
                         * X-cone test so it admits more mobys. Crucially,
                         * clamp (MAC3+77) to >= 0 — when the moby is near
                         * or behind the camera plane (MAC3+77 < 0), a
                         * naive multiply flips the widening sign and
                         * TIGHTENS the cone, culling slot 81 in 16:9 at
                         * frame ~8363 (moby_pool_state leak). Widening
                         * by zero in that regime preserves the canonical
                         * 4:3 verdict, which is what we want anyway —
                         * the X-cone is meaningless when Z is behind. */
                        code = fmt::format(
                            "psx_runtime_native_cone_pass = "
                            "(uint8_t)((4 * ((int32_t){} - 102) "
                            "- 3 * ((int32_t)cpu->gte_data[27] + 77)) < 0); "
                            "{{ int32_t mac3p = (int32_t)cpu->gte_data[27] + 77; "
                            "if (mac3p < psx_runtime_moby_cone_mac3p_floor) mac3p = psx_runtime_moby_cone_mac3p_floor;"
                            "{} = (int32_t){} + ({}) "
                            "- (int32_t)(((int64_t)mac3p "
                            "* (int64_t)psx_runtime_moby_cone_extra_q12) >> 12) "
                            "- (int32_t)((102 "
                            "* (int64_t)psx_runtime_moby_cone_bias_scale_q12) >> 12); }}",
                            reg_name(rs),
                            reg_name(rt), reg_name(rs), (int)imm);
                    } else {
                        code = translate_addiu(instr);
                    }
                }
                break;
            case 0x09:                                         // addiu
                {
                    /* `addiu $rt, $zero, 0x200` constant-load sites that load a
                     * hardcoded 512 clip/clamp bound (cyclorama portal clip-
                     * rect X-upper at func_80050BD0; 2D full-bleed prim X-width
                     * for letterbox/fade at func_80018F30 / func_800190D4).
                     * Same ConstLoad kind as the lui clip-threshold sites
                     * (`rt = <knob>`); addresses + knob live in
                     * manifest/render_channels.toml (channels
                     * widescreen_portal_clip_rect / widescreen_2d_fullbleed_x_max),
                     * loaded via --channel-sites. Identity at default knob (512). */
                    uint32_t rt = get_rt(instr);
                    uint16_t imm = get_imm16_u(instr);
                    if (auto _cs = channel_sites_.find(addr);
                        _cs != channel_sites_.end()
                        && _cs->second.kind == ChannelSite::ConstLoad
                        && (int32_t)imm == _cs->second.imm)
                    {
                        if (config_.optimize_zero_reg && rt == 0) {
                            code = "/* nop: write to $zero */";
                        } else {
                            code = fmt::format(
                                "{} = {};  /* channel const_load: {} */",
                                reg_name(rt), _cs->second.symbol,
                                _cs->second.symbol);
                        }
                    } else {
                        code = translate_addiu(instr);
                    }
                }
                break;
            case 0x0A:                                         // slti
                {
                    /* Same func_80050BD0 portal clip-rect site, test
                     * branch at 0x800517F4 / 0x80051E88. */
                    uint32_t rs = get_rs(instr);
                    uint32_t rt = get_rt(instr);
                    int16_t imm = (int16_t)get_imm16_u(instr);
                    if ((addr == 0x800517F4u || addr == 0x80051E88u)
                        && imm == 0x201) {
                        code = fmt::format(
                            "{} = (int32_t){} < (int32_t)psx_runtime_portal_clip_x_max_plus_1;",
                            reg_name(rt), reg_name(rs));
                    } else {
                        code = translate_slti(instr);
                    }
                }
                break;
            case 0x0B: code = translate_sltiu(instr); break;  // sltiu
            case 0x0C: code = translate_andi(instr); break;   // andi
            case 0x0D:                                         // ori
                {
                    /* Spyro polybuf slot size (0x1C000) appears at four
                     * specific PCs as `ori $X, $X, 0xC000` (after a
                     * preceding `lui $X, 0x1`) producing the positive
                     * constant, or as `ori $X, $X, 0x4000` (after
                     * `lui $X, 0xFFFE`) producing the two's-complement
                     * negation `-0x1C000` used by AllocateBuffers.
                     * Substitute with psx_runtime_polybuf_size so
                     * widescreen builds can grow the relocated buffer
                     * without re-recompiling. Default 0x1C000 = native
                     * 4:3 byte-identical. See psx_runtime.h. */
                    uint32_t rt = get_rt(instr);
                    uint16_t imm = get_imm16_u(instr);
                    if (imm == 0xC000u && (
                        addr == 0x8001A608u ||  /* func_8001A40C, draw.c:947 */
                        addr == 0x8001C6ACu ||  /* func_8001C694, draw.c:1561 */
                        addr == 0x8001ED90u))   /* func_8001ED5C, GamestateDraw */
                    {
                        /* End-cursor sites only. The companion AllocateBuffers
                         * substitution (0x8005B73C) was REMOVED — widening the
                         * canonical layout via that site shifts m_LowerPolyBuffer
                         * down by (size-0x1C000)*2 bytes, pulling the canonical
                         * polybuf range into the area where the level loader's
                         * CdRead destinations land (e.g. 0x8016313C in level_10).
                         * Keep canonical 0x1C000 layout intact; widen only the
                         * end-cursor in the host-redirected buffer. The cursor
                         * BASE comes from psx_runtime_polybuf_lower at the LW
                         * substitution sites, so D_80075780 = host_lower + size. */
                        code = fmt::format(
                            "{} = psx_runtime_polybuf_size;",
                            reg_name(rt));
                    } else {
                        code = translate_ori(instr);
                    }
                }
                break;
            case 0x0E: code = translate_xori(instr); break;   // xori
            case 0x0F:                                         // lui
                {
                    /* Spyro's cyclorama renderer (r_cyclorama.s) builds a
                     * per-vertex left/right clip via:
                     *   sll $a1, SXY2_packed, 16  (a1 = SX << 16)
                     *   sub $a1, $a1, $tN          (tN preloaded = 0x2000000)
                     *   bltz $a1, ok               (SX < 512: pass)
                     * The threshold 0x2000000 == 512<<16 is hardcoded via
                     * `lui $tN, 0x200`. For widescreen, SX legitimately
                     * exceeds 512; substitute the constant with a runtime-
                     * tunable threshold so openpete can widen to e.g.
                     * 2048<<16 = 0x8000000 without re-recompiling. Default
                     * == identity. Addresses enumerated explicitly because
                     * `lui $X, 0x200` appears in unrelated contexts (color
                     * packets, pointer math) and a blanket substitution
                     * would corrupt those. See psx_runtime.h
                     * `psx_runtime_clip_x_threshold`. */
                    uint32_t rt = get_rt(instr);
                    uint16_t imm = get_imm16_u(instr);
                    /* g_SonyImage base redirect. Detect a `lui $X, 0x8007`
                     * paired with the canonical `addiu $X, $X, lo` at
                     * addr+4 whose composed value falls in the splash /
                     * scratch pool span [0x8006FCF4, 0x80072894). For
                     * those pairs, emit code that places `(new_base +
                     * offset) - sext(lo)` into $X so the canonical addiu
                     * (translated normally) reconstitutes
                     * `psx_runtime_g_sony_image_base + offset`.
                     *
                     * Identity when `psx_runtime_g_sony_image_base ==
                     * 0x8006FCF4u` (default), so leaving the knob alone
                     * means byte-identical recompiled behaviour. The
                     * substitution composes by always lifting by a fixed
                     * delta (`base - 0x8006FCF4`) on the lui half — the
                     * unchanged addiu adds back its imm, recovering the
                     * pool-relative offset.
                     *
                     * Pool span ends at 0x80072E94 = 0x8006FCF4 + 0x31A0
                     * (sizeof g_SonyImage); no other globals live in the
                     * 0x8007XXXX low band before this point (next
                     * labelled BSS sym is g_MobyShadows at 0x80075EF8),
                     * so value-keying by composed address is unambiguous. */
                    if (imm == 0x8007u) {
                        auto next_opt = exe_.read_word(addr + 4);
                        if (next_opt.has_value()) {
                            uint32_t next = *next_opt;
                            uint32_t next_op = (next >> 26) & 0x3F;
                            uint32_t next_rs = (next >> 21) & 0x1F;
                            uint32_t next_rt = (next >> 16) & 0x1F;
                            int32_t  next_imm = (int32_t)(int16_t)(next & 0xFFFF);
                            if (next_op == 0x09 /* addiu */ &&
                                next_rs == rt && next_rt == rt && rt != 0)
                            {
                                uint32_t composed =
                                    (uint32_t)((int64_t)0x80070000 + next_imm);
                                if (composed >= 0x8006FCF4u &&
                                    composed <  0x80072E94u)
                                {
                                    uint32_t pool_off = composed - 0x8006FCF4u;
                                    /* The unchanged addiu at addr+4
                                     * adds sext(next_imm); compute the
                                     * lui half such that lui_value +
                                     * sext(next_imm) ==
                                     * psx_runtime_g_sony_image_base +
                                     * pool_off. */
                                    code = fmt::format(
                                        "{} = (uint32_t)(psx_runtime_g_sony_image_base + 0x{:X}u) - (uint32_t){};"
                                        "  /* g_SonyImage redirect: lui half of (D_8006FCF4 + 0x{:X}) */",
                                        reg_name(rt), pool_off, next_imm, pool_off);
                                    break;
                                }
                            }
                        }
                    }
                    /* Per-vertex right-edge SX clip thresholds (`lui $tN,
                     * 0x200` == 512<<16) across the cyclorama / moby /
                     * particle / environment renderers. Their addresses + knob
                     * live in manifest/render_channels.toml (channel
                     * widescreen_clip_x_threshold), loaded via --channel-sites
                     * — NOT hardcoded here. The ConstLoad kind emits
                     * `rt = <knob>`, replacing the hardcoded constant with a
                     * runtime-tunable threshold; identity at default knob
                     * (0x02000000 == 512<<16). PC+imm keyed because
                     * `lui $X, 0x200` recurs in unrelated contexts (color
                     * packets, pointer math) that must NOT be widened. */
                    if (auto _cs = channel_sites_.find(addr);
                        _cs != channel_sites_.end()
                        && _cs->second.kind == ChannelSite::ConstLoad
                        && (int32_t)imm == _cs->second.imm)
                    {
                        if (config_.optimize_zero_reg && rt == 0) {
                            code = "/* nop: write to $zero */";
                        } else {
                            code = fmt::format(
                                "{} = {};  /* channel const_load: {} */",
                                reg_name(rt), _cs->second.symbol,
                                _cs->second.symbol);
                        }
                    } else {
                        code = translate_lui(instr);
                    }
                }
                break;
            case 0x10:                                         // COP0
                {
                    uint32_t cop_op = (instr >> 21) & 0x1F;
                    uint32_t rt = get_rt(instr);
                    uint32_t rd = get_rd(instr);
                    if (cop_op == 0x00) { // MFC0 - move from COP0
                        code = fmt::format("{} = cpu->cop0[{}];  /* mfc0 */", reg_name(rt), rd);
                    } else if (cop_op == 0x04) { // MTC0 - move to COP0
                        code = fmt::format("cpu->cop0[{}] = {};  /* mtc0 */", rd, reg_name(rt));
                    } else if (cop_op == 0x10 && (instr & 0x3F) == 0x10) { // RFE
                        // Restore interrupt enable bits: shift bits 5:2 → 3:0
                        code = "{ uint32_t sr = cpu->cop0[12]; "
                               "cpu->cop0[12] = (sr & 0xFFFFFFF0u) | ((sr >> 2) & 0x0Fu); }  /* rfe */";
                    } else {
                        code = fmt::format("/* cop0: 0x{:08X} */", instr);
                    }
                }
                break;
            case 0x12:                                         // COP2 (GTE)
                {
                    uint32_t cop_op = (instr >> 21) & 0x1F;
                    uint32_t rt = get_rt(instr);
                    uint32_t rd = get_rd(instr);
                    if (cop_op == 0x00) { // MFC2 - move from COP2 data
                        code = fmt::format("{} = cpu->gte_data[{}];  /* mfc2 */", reg_name(rt), rd);
                    } else if (cop_op == 0x02) { // CFC2 - move from COP2 control
                        code = fmt::format("{} = cpu->gte_ctrl[{}];  /* cfc2 */", reg_name(rt), rd);
                    } else if (cop_op == 0x04) { // MTC2 - move to COP2 data
                        // Reg 30 (LZCS) updates LZCR side-effect; reg 28 (IRGB)
                        // unpacks into IR1/IR2/IR3. A direct array write would
                        // skip both — that's how Spyro 1's func_8004BE4C produced
                        // bogus collision points (LZCR stale → bad isqrt scale).
                        if (rd == 30) {
                            code = fmt::format(
                                "{{ uint32_t _v = {}; cpu->gte_data[30] = _v; "
                                "uint32_t _u = ((int32_t)_v < 0) ? ~_v : _v; "
                                "uint32_t _c = 0; if (_u == 0) {{ _c = 32; }} "
                                "else {{ while (!(_u & 0x80000000u)) {{ _u <<= 1; _c++; }} }} "
                                "cpu->gte_data[31] = _c; }}  /* mtc2 LZCS+LZCR */",
                                reg_name(rt));
                        } else if (rd == 28) {
                            code = fmt::format(
                                "{{ uint32_t _v = {}; cpu->gte_data[28] = _v & 0x7FFFu; "
                                "cpu->gte_data[9]  = (_v & 0x1Fu) << 7; "
                                "cpu->gte_data[10] = ((_v >> 5) & 0x1Fu) << 7; "
                                "cpu->gte_data[11] = ((_v >> 10) & 0x1Fu) << 7; }}  /* mtc2 IRGB+IR1-3 */",
                                reg_name(rt));
                        } else {
                            code = fmt::format("cpu->gte_data[{}] = {};  /* mtc2 */", rd, reg_name(rt));
                        }
                    } else if (cop_op == 0x06) { // CTC2 - move to COP2 control
                        code = fmt::format("cpu->gte_ctrl[{}] = {};  /* ctc2 */", rd, reg_name(rt));
                    } else if ((cop_op & 0x10) != 0) { // GTE command (bit 25 set)
                        uint32_t gte_cmd = instr & 0x1FFFFFF;
                        // Route ALL GTE commands through gte_execute() for correct behavior.
                        // openpete Day 4 Phase 1.1: stash issuing PC for the render cache to
                        // tag each vertex with its calling MIPS function — used by
                        // per-prim-class discrimination (cyclorama vs world-moby vs HUD vs
                        // Spyro mesh). One extra store per GTE op; negligible cost.
                        code = fmt::format(
                            "openpete_gte_caller_pc = 0x{:08X}u; gte_execute(cpu, 0x{:07X});  /* gte cmd 0x{:02X} */",
                            addr, gte_cmd, gte_cmd & 0x3F);
                    } else {
                        code = fmt::format("/* cop2: 0x{:08X} */", instr);
                    }
                }
                break;
            case 0x20: code = translate_lb(instr); break;     // lb
            case 0x21: code = translate_lh(instr); break;     // lh
            case 0x22: code = translate_lwl(instr); break;    // lwl
            case 0x23:                                         // lw
                {
                    /* Per-moby tracking hook for openpete UI-moby +
                     * transform-interp identity tagging. r_moby.s has
                     * three render functions, each with its own per-
                     * moby loop iteration boundary:
                     *
                     *   0x80022B08  func_80022A2C (shaded)   $s8
                     *               lw $s8,0($ra) ; addi $ra,$ra,4
                     *               ; beqz $s8,exit ; iter via $ra
                     *
                     *   0x8001F800  func_8001F798 (regular)  $ra
                     *               (loop top .L8001F7FC at 0x8001F7FC:
                     *                lw $s3,0($gp) ; lw $ra,4($gp) ;
                     *                beqz $s3,exit ; iter via $gp+=0x38)
                     *               After 0x8001F800, $ra holds the
                     *               current moby pointer.
                     *
                     *   0x80020F9C  func_80020F34 (specular) $ra
                     *               (same structure as func_8001F798,
                     *                loop top .L80020F98 at 0x80020F98)
                     *
                     * After the lw, the named register holds the fresh
                     * moby pointer; stash it host-side via
                     * openpete_obj_id_set so gte_rtps_internal can stamp
                     * a stable per-moby obj_tag (matcher pairs by real
                     * vaddr instead of falling back to world-ordinal —
                     * fixes the visible jitter on regular and specular
                     * mobys that the §7.2 Increment 1 "graceful
                     * fall-through" path produced once PHASE-C inflated
                     * the world-ordinal stream 2.7×).
                     *
                     * The identity channel (openpete_obj_id) is kept
                     * SEPARATE from the widescreen UI classifier
                     * (openpete_ui_moby): only the shaded loop
                     * (0x80022B08), the one renderer HUD/UI mobys flow
                     * through, additionally feeds the UI classifier. See
                     * the per-PC handlers below and openpete_obj_id.h.
                     *
                     * Game-agnostic from core/'s perspective: the
                     * function names and addresses are Spyro-specific,
                     * but the ranges that classify a moby as UI are
                     * registered by games/spyro-1/. */
                    if (addr == 0x8001F800u || addr == 0x80020F9Cu) {
                        /* Regular (func_8001F798) and specular
                         * (func_80020F34) render passes: at the loop
                         * iter top, POP the cull-pass queue and tag
                         * with the popped per-instance moby vaddr.
                         *
                         * The lw at this PC loads a class-shared
                         * pointer (m_Props / model template) from the
                         * render-list slot, NOT a per-instance vaddr —
                         * so we don't pass the lw's destination
                         * register. Instead, the cull pass
                         * (func_8001F158 at PC 0x8001F268, hooked in
                         * case 0x25 below) pushed each cull-survivor's
                         * Moby instance ptr ($sp) onto
                         * openpete_moby_queue in cull-pass-confirmed
                         * order. Pop here in render-pass order, which
                         * must match by FIFO invariant (the cull pass
                         * fully populates before the render pass
                         * begins; draw.c sequences them as separate
                         * function calls). Pop-on-empty returns 0 →
                         * gte falls back to world-ordinal pairing
                         * (same as the pre-queue baseline).
                         *
                         * Also clears the world-geo strip tag so a
                         * stale terrain tag doesn't bleed into moby
                         * projections.
                         *
                         * Channel split: regular/specular mobys are never
                         * HUD/UI elements, so they feed ONLY the
                         * transform-interp identity channel
                         * (openpete_obj_id) — NOT the widescreen UI
                         * classifier (openpete_ui_moby). Feeding popped
                         * gameplay-moby vaddrs into s_current let
                         * is_current_ui() mis-classify any moby whose
                         * address fell in the polybuf/HUD range as UI,
                         * breaking Hor+ widening under --aspect. */
                        code = fmt::format(
                            "{} openpete_obj_id_set("
                            "openpete_moby_queue_pop()); "
                            "openpete_world_geo_clear();",
                            translate_lw(instr));
                    } else if (addr == 0x80022B08u
                            || addr == 0x80023B0Cu
                            || addr == 0x8004F088u) {
                        /* Per-renderer identity tag.
                         *
                         * Per-moby loop iter boundary (r_moby.s):
                         *   0x80022B08  func_80022A2C (shaded)   $fp = *iter
                         * After the lw, $fp holds the current shaded
                         * moby's PSX vaddr (the shaded-moby array
                         * stores instance ptrs directly, unlike the
                         * regular/specular render-list slots — see
                         * the 0x8001F800/0x80020F9C handler above).
                         *
                         * Per-singleton renderer tags (r_pete.s,
                         * r_cyclorama.s) — the lw at the hook PC reads
                         * from an *address register* that the prologue
                         * loaded with the singleton's vaddr; that
                         * register (not the lw's destination) is what
                         * we want as identity:
                         *   0x80023B0C  func_80023AC4 (Spyro)
                         *               $ra was set to g_Spyro at the
                         *               prologue addiu (0x80023B00);
                         *               this lw is `lw $fp, 0($fp)`
                         *               (reads g_Models), which leaves
                         *               $ra untouched. Pass `cpu->ra`.
                         *   0x8004F088  func_8004F000 (cyclorama)
                         *               $at was set to g_Cyclorama at
                         *               the prologue addiu (0x8004F078);
                         *               this lw is `lw $t3, 4($at)`,
                         *               which reads from $at but leaves
                         *               it intact. Pass `cpu->at`.
                         *
                         * Each appended call stashes the vaddr host-
                         * side so gte_rtps_internal can stamp the cache
                         * entry's obj_tag with a stable cross-frame
                         * per-object identity. */
                        const char* tag_reg;
                        if (addr == 0x80023B0Cu)      tag_reg = "cpu->ra";
                        else if (addr == 0x8004F088u) tag_reg = "cpu->at";
                        else                          tag_reg = nullptr;
                        std::string reg_expr = tag_reg
                            ? std::string(tag_reg)
                            : reg_name(get_rt(instr));
                        /* Also clear the world-geo strip tag (§7.5-R3):
                         * once per-moby/Spyro/cyclorama render begins,
                         * world-strip state is stale, so projections
                         * fall back to the per-renderer identity in
                         * gte_rtps_internal.
                         *
                         * Channel split: the shaded loop (0x80022B08) is
                         * the ONLY renderer HUD/UI mobys flow through, so
                         * it alone feeds the widescreen UI classifier
                         * (openpete_ui_moby) — in ADDITION to the
                         * transform-interp identity channel
                         * (openpete_obj_id). Spyro and cyclorama are never
                         * UI, so they feed obj_id only; mixing their
                         * vaddrs into s_current was the widescreen-
                         * widening regression. */
                        if (addr == 0x80022B08u) {
                            code = fmt::format(
                                "{0} openpete_ui_moby_set_current({1}); "
                                "openpete_obj_id_set({1}); "
                                "openpete_world_geo_clear();",
                                translate_lw(instr), reg_expr);
                        } else {
                            code = fmt::format(
                                "{} openpete_obj_id_set({}); "
                                "openpete_world_geo_clear();",
                                translate_lw(instr), reg_expr);
                        }
                    } else if (addr == 0x80025A34u) {
                        /* §7.5 terrain-reproject validation probe.
                         * r_environment.s per-vertex transform:
                         *   0x80025A30 lw $v0,0($s1)  ; word0 = X<<16|Y
                         *   0x80025A34 lw $a0,4($s1)  ; word1 = Z<<16|.
                         *   ... sub $s7/$t8/$t9 (scaled campos) → MVMVA.
                         * After this lw, $v0/$a0 hold the CAMERA-FREE
                         * world vertex, $s1 the vert ptr, $s7/$t8/$t9
                         * the scaled camera position. Read-only dump
                         * (OPENPETE_ENVVERT_LOG) to validate the
                         * PSX-RAM camera-reproject forward model. */
                        code = fmt::format(
                            "{} openpete_envvert_probe(cpu, cpu->v0, "
                            "cpu->a0, cpu->s1, cpu->s7, cpu->t8, "
                            "cpu->t9);",
                            translate_lw(instr));
                    } else if (addr == 0x800276B4u) {
                        /* LP-walk loop top in r_environment.s. When the
                         * OPENPETE_DBG_KILL_LP debug knob is set, force
                         * the loaded value to 0 so the following beqz
                         * exits the loop immediately — disables ground-
                         * plane (LP-walk → HQ atlas) rendering for
                         * visual identification of which cells use it.
                         *
                         * Also one of the 7 §7.5-R3 PHASE-C strip-list
                         * loop heads: stamps a stable per-strip identity
                         * into openpete_world_geo so gte_rtps_internal
                         * can tag terrain verts produced by the four
                         * non-dominant RTPS variants (0x80027A64 +
                         * 0x80028180) inside this strip's inner unrolled
                         * vertex loops. When the LP-kill knob fires the
                         * loaded value is 0 and the strip-loop exits
                         * immediately, so the stale tag is harmless. */
                        uint32_t rs = get_rs(instr);
                        uint32_t rt = get_rt(instr);
                        int16_t off = get_imm16(instr);
                        code = fmt::format(
                            "{} = psx_runtime_dbg_kill_env_lp_walk ? 0u : "
                            "cpu->read_word({} + {});"
                            " openpete_world_geo_set_current({});"
                            "  /* LP-walk kill knob + world strip tag */",
                            reg_name(rt), reg_name(rs), off,
                            reg_name(rt));
                    } else if (auto _cs = channel_sites_.find(addr);
                               _cs != channel_sites_.end()
                               && _cs->second.kind == ChannelSite::AppendCall) {
                        /* §7.5-R3 PHASE-C — strip-list loop heads in
                         * r_environment.s func_800258F0 (channel
                         * world_geo_strip_tag): six `lw $reg,0($ra)` loop
                         * heads (0x800267E4/27720/28590/285FC/29500/2956C),
                         * each followed by openpete_world_geo_set_current(rt)
                         * to stamp a stable cross-frame per-strip identity for
                         * the non-dominant RTPS variants. Address + operand
                         * (rt = the loaded value) + hook live in
                         * manifest/render_channels.toml via --channel-sites.
                         * READ-ONLY host-side stash; no gameplay byte touched.
                         * (0x800276B4 — a 7th strip head — is NOT here: it is
                         * a hybrid co-owned with the dbg_kill_env_lp_walk knob
                         * and stays hardcoded above.) */
                        code = translate_lw(instr)
                             + emit_channel_append_call(_cs->second, instr);
                    } else if (addr == 0x800261E0u) {
                        /* DBG: env-renderer's read of g_SkipLowPolyWorld.
                         * When psx_runtime_dbg_kill_env_lp_walk is set,
                         * OR in a 1 so the following bnez at 0x800261F8
                         * always branches and the LP-walk is skipped.
                         * Scoped to the env-renderer (not r_moby /
                         * r_cyclorama, which read the same flag). */
                        uint32_t rs = get_rs(instr);
                        uint32_t rt = get_rt(instr);
                        int16_t off = get_imm16(instr);
                        code = fmt::format(
                            "{} = cpu->read_word({} + {}) | (psx_runtime_dbg_kill_env_lp_walk ? 1u : 0u);"
                            "  /* env-renderer SkipLowPolyWorld (debug LP-kill) */",
                            reg_name(rt), reg_name(rs), off);
                    } else if (addr == 0x8001FE9Cu || addr == 0x80021674u) {
                        /* Phase 4 moby model LOD collapse. r_moby.s LP-band
                         * branch loads `lw $fp, 0x1C($ra)` — the AnimationHeader
                         * .m_LpFaces field. Default knob 0x1C preserves
                         * canonical behaviour; --no-lod swings to 0x14
                         * so the LP path reads m_Faces (high-poly) instead.
                         * Two sites: 0x8001FE9C and 0x80021674. */
                        uint32_t rs = get_rs(instr);
                        uint32_t rt = get_rt(instr);
                        code = fmt::format(
                            "{} = cpu->read_word({} + psx_runtime_moby_lp_face_offset);"
                            "  /* moby LP face load (no-lod knob) */",
                            reg_name(rt), reg_name(rs));
                    } else if (addr == 0x80026210u) {
                        /* --no-lod also saturates m_CullingDistance
                         * (g_Environment offset 0x28) at this PC, the
                         * LP-walk pre-pass's radial-distance gate. Cone-
                         * admitted cells beyond the canonical 0x14000/
                         * 0x1c000/0x28000 radius then survive and reach
                         * HP-walk. Renderer-only — collision.s and math.s
                         * also read offset 0x28 (10 PCs) and must keep
                         * the canonical value so gameplay state is
                         * unaffected. Identity when openpete_no_lod==0. */
                        uint32_t rs = get_rs(instr);
                        uint32_t rt = get_rt(instr);
                        int16_t off = get_imm16(instr);
                        code = fmt::format(
                            "{{ uint32_t _v = cpu->read_word({} + {}); "
                            "{} = openpete_no_lod ? 0x7FFFFFFFu : _v; }}"
                            "  /* m_CullingDistance INT32_MAX (no-lod) */",
                            reg_name(rs), off, reg_name(rt));
                    } else if (addr == 0x800259B0u
                            || addr == 0x80026214u
                            || addr == 0x800267B0u) {
                        /* Phase 6 (m_LodDistance doubler): r_environment.s
                         * reads g_Environment.m_LodDistance (offset 0x24)
                         * at three PCs:
                         *   0x800259B0 → $s6 (front-walk routing gate)
                         *   0x80026214 → $s3 (LP-walk screen margin)
                         *   0x800267B0 → $s5 (HP-walk OT-slot + cull math)
                         *
                         * Under --no-lod, return INT32_MAX (0x7FFFFFFF).
                         * Saturates the front-walk routing gate (admits
                         * every cell), the LP-walk screen margin, and
                         * HP-walk's screen-margin cull. Identity at
                         * default. Earlier saturation experiment
                         * (b3f17ee) regressed cyclorama Z because the
                         * OT-slot math `srl $t5, $s5, 2` shifts env
                         * cells into closer buckets than cyclorama's
                         * authored slot — watch for that visually. */
                        uint32_t rs = get_rs(instr);
                        uint32_t rt = get_rt(instr);
                        int16_t off = get_imm16(instr);
                        code = fmt::format(
                            "{{ uint32_t _v = cpu->read_word({} + {}); "
                            "{} = openpete_no_lod ? 0x7FFFFFFFu : _v; }}"
                            "  /* m_LodDistance INT32_MAX (no-lod) */",
                            reg_name(rs), off, reg_name(rt));
                    } else if (addr == 0x8001A618u || addr == 0x8001C6A0u) {
                        /* m_LowerPolyBuffer cursor-reset reads. When
                         * psx_runtime_polybuf_lower is non-zero (widescreen
                         * polybuf relocated to a host-backed region), redirect
                         * the loaded pointer so D_800757B0 = host_lower and
                         * D_80075780 = host_lower + polybuf_size land in the
                         * relocated buffer. Default 0 sentinel falls through
                         * to the canonical LW for byte-identical 4:3.
                         * See psx_runtime.h. */
                        uint32_t rt = get_rt(instr);
                        code = fmt::format(
                            "{} if (psx_runtime_polybuf_lower != 0u) {{ {} = psx_runtime_polybuf_lower; }}",
                            translate_lw(instr), reg_name(rt));
                    } else {
                        code = translate_lw(instr);
                    }
                }
                break;
            case 0x24: code = translate_lbu(instr); break;    // lbu
            case 0x25:                                         // lhu
                {
                    if (addr == 0x80022B20u
                            || addr == 0x8001F1F4u
                            || addr == 0x800209A0u) {
                        /* Phase 8: per-moby render distance saturation.
                         * r_moby.s reads the packed m_Renderer half-word
                         * at moby offset 0x50 in three parallel paths:
                         *   0x8001F1F4 → regular  (lhu $a0, 0x50($sp))
                         *   0x800209A0 → specular (lhu $a0, 0x50($sp))
                         *   0x80022B20 → shaded   (lhu $a0, 0x50($fp))
                         * Low 7 bits: m_Distance, bit 7: m_ShinyRenderer;
                         * byte 0x51: m_SpecularMetalColor[0], used as a
                         * +0x200 cull bias via its LSB on non-shiny
                         * mobys and cleared by the immediately following
                         * sb $zero, 0x51. The shaded path bltz-routes
                         * shiny mobys away before using $a0; the
                         * regular/specular paths use the full low byte
                         * as the distance (their moby lists are
                         * authored so m_ShinyRenderer is clear).
                         *
                         * Box-cull at PCs 0x80022B48..0x80022B90 (and
                         * the analogous PCs in the other two paths)
                         * rejects mobys outside |delta/4| < (low_byte
                         * << 8) + bias, so authored m_Distance=16
                         * gives ~16 K-unit range. Under --no-lod,
                         * force m_Distance to 0x7F (max 7-bit) so the
                         * threshold rises to ~130 K units, matching the
                         * saturated env cull radius (Phase 6.B).
                         * Preserve the shiny bit and byte 0x51.
                         *
                         * CRITICAL: only bump mobys whose current
                         * m_Distance is non-zero. m_Distance == 0 is
                         * the "uninitialized / disabled moby slot"
                         * sentinel — forcing it to 0x7F admits stale /
                         * uninit moby memory into the renderer and
                         * hangs during the post-level-load fade-in.
                         * Identity when openpete_no_lod==0. */
                        uint32_t rs = get_rs(instr);
                        uint32_t rt = get_rt(instr);
                        int16_t off = get_imm16(instr);
                        code = fmt::format(
                            "{{ uint32_t _v = cpu->read_half({} + {}); "
                            "if (openpete_no_lod && (_v & 0x7Fu) != 0u) {{ "
                            "psx_runtime_mdist_thresh_delta = (int32_t)((0x7Fu - (_v & 0x7Fu)) << 8); "
                            "_v = (_v & 0xFF80u) | 0x7Fu; }} "
                            "else psx_runtime_mdist_thresh_delta = 0; "
                            "{} = _v; }}"
                            "  /* m_Distance saturate + canon-dist delta (no-lod) */",
                            reg_name(rs), off, reg_name(rt));
                    } else if (addr == 0x8001F268u) {
                        /* transform-interp moby-queue push.
                         * r_moby.s func_8001F158 (the cull pass) walks
                         * mobys via `lw $sp, 0($ra)` at the loop top
                         * (0x8001F1E8). The cascade of cull-skip
                         * back-branches all jump back to the loop top;
                         * the first instruction past the last back-
                         * branch is `lhu $t7, 0x36($sp)` at 0x8001F268
                         * — i.e. THIS PC is reached iff the moby
                         * passed cull and is about to be added to the
                         * render-list. At this point $sp still holds
                         * the cull-survivor's Moby instance vaddr.
                         *
                         * Push $sp onto openpete_moby_queue; the
                         * regular/specular render passes
                         * (func_8001F798 at 0x8001F800,
                         * func_80020F34 at 0x80020F9C) pop in the
                         * same order to recover per-instance identity
                         * (their render-list slots only carry a
                         * class-shared m_Props ptr, not the moby
                         * vaddr). READ-ONLY: stashes the vaddr in a
                         * host-side ring; the canonical lhu is emitted
                         * verbatim before the push. */
                        code = fmt::format(
                            "{} openpete_moby_queue_push(cpu->sp);",
                            translate_lhu(instr));
                    } else {
                        code = translate_lhu(instr);
                    }
                }
                break;
            case 0x26: code = translate_lwr(instr); break;    // lwr
            case 0x28: code = translate_sb(instr, addr); break;     // sb
            case 0x29: code = translate_sh(instr); break;     // sh
            case 0x2A: code = translate_swl(instr); break;    // swl
            case 0x2B:                                         // sw
                {
                    if (addr == 0x8001D760u || addr == 0x8001D768u) {
                        /* Gamestate 11 (func_8001D718, draw.c:2051/2054):
                         *   D_800756FC = g_HudMobys = D_800757B0 + 0x1BA00;
                         * D_800757B0 is the per-frame write cursor, which is
                         * redirected into the host-backed poly buffer when the
                         * polybuf is relocated. g_HudMobys (0x80075710) is a
                         * hashed gameplay-state pointer, so it must keep the
                         * value real hardware holds. Map the stored host
                         * address back to its canonical PSX vaddr (the 0x1BA00
                         * within-slot offset is preserved by the translator).
                         * This also repairs the `D_800756FC == m_HudOTStart`
                         * predicate at draw.c:2049 — host vs canonical made it
                         * always-false under relocation, silently breaking the
                         * Gamestate-11 HUD. Identity at 4:3 default. */
                        uint32_t rs = get_rs(instr);
                        uint32_t rt = get_rt(instr);
                        int16_t off = get_imm16(instr);
                        code = fmt::format(
                            "cpu->write_word({} + {}, psx_runtime_polybuf_to_canon({}));"
                            "  /* g_HudMobys/D_800756FC canonical (polybuf relocate) */",
                            reg_name(rs), off, reg_name(rt));
                    } else if (addr == 0x8001EDCCu || addr == 0x8001EDD4u) {
                        /* GamestateDraw frame-start reset (draw.c:2666/2667):
                         *   D_800756FC = g_HudMobys = D_800757B0 + 0x1C000;
                         * The compiler reuses the end-cursor value
                         * (D_800757B0 + size) for these stores. Under polybuf
                         * relocation `size` is the *grown* host slot size
                         * (psx_runtime_polybuf_size), which D_80075780 needs but
                         * the HUD pointers must NOT inherit — the HUD-moby pool
                         * stays one canonical 0x1C000 slot above the cursor, as
                         * on hardware. So recover the cursor base (stored value
                         * minus the grown size), map it back to canonical, and
                         * re-add the canonical 0x1C000 slot stride. Identity at
                         * 4:3 default (size == 0x1C000, translator is identity).
                         * g_HudMobys (0x80075710) is hashed gameplay state. */
                        uint32_t rs = get_rs(instr);
                        uint32_t rt = get_rt(instr);
                        int16_t off = get_imm16(instr);
                        code = fmt::format(
                            "cpu->write_word({} + {}, psx_runtime_polybuf_to_canon({} - psx_runtime_polybuf_size) + 0x1C000u);"
                            "  /* g_HudMobys/D_800756FC canonical (polybuf relocate) */",
                            reg_name(rs), off, reg_name(rt));
                    } else {
                        code = translate_sw(instr);
                    }
                }
                break;
            case 0x2E: code = translate_swr(instr); break;    // swr
            case 0x32:                                         // LWC2 - load word to COP2
                {
                    uint32_t rs = get_rs(instr);
                    uint32_t rt = get_rt(instr);
                    int16_t offset = get_imm16(instr);
                    std::string addr_expr = (offset == 0)
                        ? std::string(reg_name(rs))
                        : fmt::format("{} + {}", reg_name(rs), offset);
                    // Like MTC2, regs 28/30 have side-effects (IRGB→IR1-3, LZCS→LZCR).
                    if (rt == 30) {
                        code = fmt::format(
                            "{{ uint32_t _v = cpu->read_word({}); cpu->gte_data[30] = _v; "
                            "uint32_t _u = ((int32_t)_v < 0) ? ~_v : _v; "
                            "uint32_t _c = 0; if (_u == 0) {{ _c = 32; }} "
                            "else {{ while (!(_u & 0x80000000u)) {{ _u <<= 1; _c++; }} }} "
                            "cpu->gte_data[31] = _c; }}  /* lwc2 LZCS+LZCR ({}) */",
                            addr_expr, addr_expr);
                    } else if (rt == 28) {
                        code = fmt::format(
                            "{{ uint32_t _v = cpu->read_word({}); cpu->gte_data[28] = _v & 0x7FFFu; "
                            "cpu->gte_data[9]  = (_v & 0x1Fu) << 7; "
                            "cpu->gte_data[10] = ((_v >> 5) & 0x1Fu) << 7; "
                            "cpu->gte_data[11] = ((_v >> 10) & 0x1Fu) << 7; }}  /* lwc2 IRGB+IR1-3 ({}) */",
                            addr_expr, addr_expr);
                    } else if (offset == 0) {
                        code = fmt::format("cpu->gte_data[{}] = cpu->read_word({});  /* lwc2 gte[{}], ({}) */",
                                          rt, reg_name(rs), rt, reg_name(rs));
                    } else {
                        code = fmt::format("cpu->gte_data[{}] = cpu->read_word({} + {});  /* lwc2 gte[{}], {}({}) */",
                                          rt, reg_name(rs), offset, rt, offset, reg_name(rs));
                    }
                }
                break;
            case 0x3A:                                         // SWC2 - store word from COP2
                {
                    uint32_t rs = get_rs(instr);
                    uint32_t rt = get_rt(instr);
                    int16_t offset = get_imm16(instr);
                    if (offset == 0) {
                        code = fmt::format("cpu->write_word({}, cpu->gte_data[{}]);  /* swc2 gte[{}], ({}) */",
                                          reg_name(rs), rt, rt, reg_name(rs));
                    } else {
                        code = fmt::format("cpu->write_word({} + {}, cpu->gte_data[{}]);  /* swc2 gte[{}], {}({}) */",
                                          reg_name(rs), offset, rt, rt, offset, reg_name(rs));
                    }
                }
                break;
            default:
                code = fmt::format("/* TODO: opcode=0x{:02X} */", opcode);
        }
    }

    /* Per-instruction PC tracking. When enabled, prepend `cpu->pc = X;`
     * so write-watchers and the state-diff harness can attribute every
     * memory write to the exact MIPS instruction that produced it. Costs
     * one store per emitted instruction; off by default. */
    std::string pc_emit;
    if (config_.emit_pc_tracking) {
        pc_emit = fmt::format("cpu->pc = 0x{:08X}; ", addr);
    }
    return config_.indent + pc_emit + code + comment;
}

std::string CodeGenerator::translate_basic_block(
    const BasicBlock& block,
    const ControlFlowGraph& cfg) {

    std::stringstream ss;

    // Block label. Trailing `;` (null statement) so the label can be
    // followed directly by a declaration without tripping the C-language
    // "a label can only be part of a statement" constraint — GCC 10
    // (MinGW) treats it as an error; GCC 11 only warns. The mid-block
    // branch-target emitter further down already does this.
    ss << fmt::format("block_{:08X}:;\n", block.start_addr);

    // Translate each instruction in the block
    uint32_t addr = block.start_addr;

    // Determine if the exit instruction uses a delay slot pattern.
    // Normal case: branch at end_addr-4, delay slot at end_addr.
    // Split-function edge case: exit instruction IS at end_addr (delay slot
    // is outside the function), so exit_branch_addr = end_addr not end_addr-4.
    bool exit_has_delay = (block.exit_instr.has_delay_slot &&
                           block.exit_instr.type != ControlFlowType::None);
    bool delay_slot_in_block = (exit_has_delay &&
                                block.exit_instr.address < block.end_addr);
    bool exit_uses_delay_slot = delay_slot_in_block;
    uint32_t exit_branch_addr;
    if (delay_slot_in_block) {
        exit_branch_addr = block.end_addr - 4;  // branch at end-4, delay slot at end
    } else if (exit_has_delay) {
        exit_branch_addr = block.end_addr;       // branch at end, delay slot outside
    } else {
        exit_branch_addr = block.end_addr;
    }

    while (addr <= block.end_addr) {
        auto instr_opt = exe_.read_word(addr);
        if (!instr_opt.has_value()) {
            break;
        }

        uint32_t instr = *instr_opt;

        // Skip the delay slot at end_addr - it's emitted as part of the branch handling below
        if (exit_uses_delay_slot && addr == block.end_addr) {
            addr += 4;
            break;
        }

        // Don't emit control flow instructions here (handled separately)
        bool is_cf = ControlFlowAnalyzer::is_control_flow(instr);

        // Emit inline label for mid-block jump table targets
        if (addr != block.start_addr && extra_labels_.count(addr)) {
            ss << fmt::format("block_{:08X}:;\n", addr);
        }

        // Instruction-level annotation (skipped at function start — already shown above signature)
        if (annotations_ && addr != cfg.function_start) {
            const std::string& inote = annotations_->lookup(addr);
            if (!inote.empty())
                ss << config_.indent << fmt::format("/* [NOTE] {} */\n", inote);
        }

        if (!is_cf) {
            ss << translate_instruction(addr, instr) << "\n";
        } else {
            // Control flow is handled at block exit
            if (addr == exit_branch_addr) {
                // MIPS delay slot handling: emit delay slot instruction BEFORE branch/jump
                std::string delay_saved_cond;  // non-empty if condition was pre-captured
                if (block.exit_instr.has_delay_slot) {
                    uint32_t delay_slot_addr = addr + 4;
                    auto delay_instr_opt = exe_.read_word(delay_slot_addr);

                    if (delay_instr_opt.has_value()) {
                        uint32_t delay_instr = *delay_instr_opt;

                        // For branch-likely variants, delay slot is conditional
                        if (block.exit_instr.is_likely) {
                            ss << config_.indent << "/* delay slot (likely) - conditional execution */\n";
                            ss << config_.indent << "if (" << generate_branch_condition(block.exit_instr.instruction) << ") {\n";
                            ss << config_.indent << translate_instruction(delay_slot_addr, delay_instr) << "\n";
                            ss << config_.indent << "}\n";
                        } else {
                            // Normal delay slot - always executes.
                            // In MIPS, the branch condition is evaluated at the branch
                            // instruction (before the delay slot). If the delay slot
                            // modifies a condition register we must save the result first.
                            if (block.exit_instr.type == ControlFlowType::Branch) {
                                std::string cond = generate_branch_condition(block.exit_instr.instruction);
                                delay_saved_cond = fmt::format("_bc_{:08X}", addr);
                                ss << config_.indent
                                   << fmt::format("int {} = ({});  /* save branch cond before delay slot */\n",
                                                  delay_saved_cond, cond);
                            }
                            ss << config_.indent << "/* delay slot (always executes) */\n";
                            ss << translate_instruction(delay_slot_addr, delay_instr) << "\n";
                        }
                    }
                }

                // Now emit the branch/jump
                if (block.exit_instr.type == ControlFlowType::Branch) {
                    // Check if this is a branch-and-link (bgezal/bltzal)
                    // For REGIMM (opcode=0x01), rt=0x10 (bltzal) or rt=0x11 (bgezal)
                    // The link always happens unconditionally (even if branch not taken)
                    {
                        uint32_t branch_instr = block.exit_instr.instruction;
                        uint32_t b_opcode = (branch_instr >> 26) & 0x3F;
                        if (b_opcode == 0x01) {
                            uint32_t regimm_op = (branch_instr >> 16) & 0x1F;
                            if (regimm_op == 0x10 || regimm_op == 0x11) {
                                // bltzal or bgezal: link register always set
                                ss << config_.indent
                                   << fmt::format("cpu->ra = 0x{:08X};  /* branch-and-link return addr */\n", addr + 8);
                            }
                        }
                    }
                    // Conditional branch - use pre-captured condition if available
                    std::string condition = delay_saved_cond.empty()
                        ? generate_branch_condition(block.exit_instr.instruction)
                        : delay_saved_cond;
                    if (block.successors.size() == 2) {
                        ss << config_.indent << "if (" << condition << ") {\n";
                        ss << config_.indent << config_.indent
                           << fmt::format("goto block_{:08X};  /* taken */\n", block.successors[0]);
                        ss << config_.indent << "} else {\n";
                        ss << config_.indent << config_.indent
                           << fmt::format("goto block_{:08X};  /* not taken */\n", block.successors[1]);
                        ss << config_.indent << "}\n";
                    } else {
                        // Split-function: one or both branch targets are outside
                        // this function piece. Emit function calls for out-of-function
                        // targets to reconnect the split pieces.
                        uint32_t branch_target = block.exit_instr.target;
                        uint32_t fall_through_addr = block.exit_instr.address + 8;
                        bool target_in = cfg.blocks.count(branch_target) > 0;
                        bool fallthru_in = cfg.blocks.count(fall_through_addr) > 0;

                        ss << config_.indent << "if (" << condition << ") {\n";
                        if (target_in) {
                            ss << config_.indent << config_.indent
                               << fmt::format("goto block_{:08X};  /* taken */\n", branch_target);
                        } else if (known_functions_.count(branch_target)) {
                            ss << config_.indent << config_.indent
                               << fmt::format("{}{:08X}(cpu); return;  /* taken: split piece */\n", config_.func_prefix, branch_target);
                        } else {
                            ss << config_.indent << config_.indent
                               << fmt::format("call_by_address(cpu, 0x{:08X}u); return;  /* taken: split (mid-func) */\n", branch_target);
                        }
                        ss << config_.indent << "} else {\n";
                        if (fallthru_in) {
                            ss << config_.indent << config_.indent
                               << fmt::format("goto block_{:08X};  /* not taken */\n", fall_through_addr);
                        } else if (known_functions_.count(fall_through_addr)) {
                            ss << config_.indent << config_.indent
                               << fmt::format("{}{:08X}(cpu); return;  /* not taken: split piece */\n", config_.func_prefix, fall_through_addr);
                        } else {
                            ss << config_.indent << config_.indent
                               << fmt::format("call_by_address(cpu, 0x{:08X}u); return;  /* not taken: split (mid-func) */\n", fall_through_addr);
                        }
                        ss << config_.indent << "}\n";
                    }
                } else if (block.exit_instr.type == ControlFlowType::Jump) {
                    // Unconditional jump
                    uint32_t jt = block.exit_instr.target;
                    if (!block.successors.empty()) {
                        ss << config_.indent
                           << fmt::format("goto block_{:08X};  /* j */\n", block.successors[0]);
                    } else if (jt != 0 && external_name_for(jt)) {
                        ss << config_.indent
                           << fmt::format("{}(cpu); return;  /* j -> external */\n",
                                          *external_name_for(jt));
                    } else if (jt != 0 && manual_name_for(jt)) {
                        ss << config_.indent
                           << fmt::format("{}(cpu); return;  /* j -> manual */\n",
                                          *manual_name_for(jt));
                    } else if (jt != 0 && known_functions_.count(jt)) {
                        // Jump target is out-of-function and is a known function start
                        ss << config_.indent
                           << fmt::format("{}{:08X}(cpu); return;  /* j to split piece */\n", config_.func_prefix, jt);
                    } else if (jt != 0) {
                        // Jump target is a mid-function address — dispatch dynamically
                        ss << config_.indent
                           << fmt::format("call_by_address(cpu, 0x{:08X}u); return;  /* j to split (mid-func) */\n", jt);
                    }
                } else if (block.exit_instr.type == ControlFlowType::Return) {
                    // Return instruction
                    ss << config_.indent << "return;  /* jr $ra */\n";
                } else if (block.exit_instr.type == ControlFlowType::JumpRegister) {
                    // jr $rs — may be: BIOS call (jr $t2, t2=0xA0/0xB0/0xC0)
                    //                  or: jump table (jr $v0, v0 loaded from LW+ADDU+LUI)
                    //                  or: saved-ra return (addi $X,$ra,0; ...; jr $X)
                    uint32_t jr_rs = get_rs(block.exit_instr.instruction);

                    // Saved-ra pattern: handwritten asm functions sometimes save
                    // $ra into a non-$ra register and return via `jr <reg>`
                    // (e.g. spyro1's func_80053570 saves to $a3 because it needs
                    // to clobber $ra with `jal rand` mid-body). Translating this
                    // as `call_by_address(cpu->X)` dispatches into the *caller*'s
                    // function tail as a separate C call, runs the caller's
                    // epilogue, then returns to the caller's recompiled `goto
                    // block_<ra>` which runs the same tail again — sp drifts up
                    // by the caller's frame size each iteration, eventually
                    // landing past 2 MB and silently corrupting reads/writes.
                    //
                    // Detect by back-scanning for `addi/addiu $jr_rs, $ra, 0`
                    // (op=0x08/0x09, rs=31, rt=jr_rs, imm=0) without any
                    // intervening write to jr_rs. When matched, emit `return;`
                    // — the C call returns naturally to the caller, whose
                    // `goto block_<ra>` then runs the tail exactly once.
                    bool jr_is_saved_ra = false;
                    if (jr_rs != 31u) {  // skip the trivial jr $ra
                        // Scan up to 200 instructions back. Don't stop at
                        // cfg.function_start: forced-entry manifests can split
                        // the original function (e.g. func_800530C0 saves to
                        // $t7 at 0x8005325C, but the `jr $t7` at 0x80053348 is
                        // in a separately-split piece starting at 0x80053274).
                        // exe_.read_word bounds the scan to the EXE extent.
                        for (int back = 1; back <= 200; back++) {
                            uint32_t sa = block.exit_instr.address - (uint32_t)(back * 4);
                            auto sopt = exe_.read_word(sa);
                            if (!sopt) break;
                            uint32_t si   = *sopt;
                            uint32_t s_op = (si >> 26) & 0x3F;
                            uint32_t s_rs = (si >> 21) & 0x1F;
                            uint32_t s_rt = (si >> 16) & 0x1F;
                            int16_t  s_im = (int16_t)(si & 0xFFFF);
                            // The save: addi/addiu jr_rs, $ra, 0
                            if ((s_op == 0x08 || s_op == 0x09) &&
                                s_rs == 31u && s_rt == jr_rs && s_im == 0) {
                                jr_is_saved_ra = true;
                                break;
                            }
                            // Any other instruction that writes to jr_rs
                            // invalidates the pattern. Conservatively bail on
                            // the common write-form opcodes (R-type with rd ==
                            // jr_rs, or I-type with rt == jr_rs that isn't a
                            // store/branch). Stores are op 0x28..0x2B/0x38;
                            // branches are op 0x01/0x04..0x07 and don't write
                            // GPRs.
                            uint32_t s_rd = (si >> 11) & 0x1F;
                            uint32_t s_fn = si & 0x3F;
                            bool writes_jr_rs = false;
                            if (s_op == 0x00 && s_rd == jr_rs && s_fn != 0x08 && s_fn != 0x09) {
                                // R-type ALU/shift writing rd
                                writes_jr_rs = true;
                            } else if (s_op >= 0x08 && s_op <= 0x0F && s_rt == jr_rs) {
                                // I-type ALU (addi/addiu/slti/sltiu/andi/ori/xori/lui) writing rt
                                writes_jr_rs = true;
                            } else if (s_op >= 0x20 && s_op <= 0x27 && s_rt == jr_rs) {
                                // Loads write rt
                                writes_jr_rs = true;
                            }
                            if (writes_jr_rs) break;
                        }
                    }
                    if (jr_is_saved_ra) {
                        ss << config_.indent
                           << fmt::format("return;  /* jr {} ({} = saved $ra) */\n",
                                          reg_name(jr_rs), reg_name(jr_rs));
                    } else {

                    // Detect MIPS jump-table pattern by scanning backwards from jr.
                    // Handles two common patterns:
                    //   P1: lui $R,hi; addiu $R,$R,lo; addu $D,$idx,$R; lw $D,0($D)
                    //   P2: lui $R,hi;                 addu $D,$R,$idx; lw $D,off($D)
                    // table_base = lui_val + addiu_val + lw_offset
                    //
                    // Also tracks the SLL feeding the ADDU so we can emit a
                    // switch on the *index* register (cases 0..N-1) instead of
                    // on the loaded address. Switching on the index makes the
                    // dispatch immune to runtime corruption of the jump-table
                    // bytes (see plans/psxrecomp-jumptable-baking.md).
                    uint32_t table_base  = 0;
                    uint32_t table_count = 0;
                    uint32_t index_reg   = 0xFF;  // source of `sll _, idx, 2`
                    bool     found_sll   = false;
                    {
                        uint32_t lw_base_reg   = 0xFF;   // base reg in the LW
                        int32_t  lw_offset     = 0;      // offset in the LW instruction
                        uint32_t addu_cand[2]  = {0xFF, 0xFF}; // rs, rt of ADDU
                        uint32_t addu_dest     = 0xFF;   // rd of ADDU (= sll dest target)
                        uint32_t table_reg     = 0xFF;   // candidate for table base reg
                        uint32_t lui_val       = 0;
                        int16_t  addiu_val[2]  = {0, 0}; // per-candidate ADDIU
                        bool     found_addiu[2]= {false, false};
                        bool     found_lui     = false;

                        // Back-scan up to 40 instructions. Don't stop at
                        // cfg.function_start: forced-entry manifests can
                        // split a JT's SLTIU bound check off into the
                        // preceding function, and we still need to find it.
                        // exe_.read_word() naturally bounds the scan to the
                        // EXE/overlay extent.
                        for (int back = 1; back <= 40; back++) {
                            uint32_t sa = block.exit_instr.address - (uint32_t)(back * 4);
                            auto sopt = exe_.read_word(sa);
                            if (!sopt) break;
                            uint32_t si   = *sopt;
                            uint32_t s_op = (si >> 26) & 0x3F;
                            uint32_t s_rs = (si >> 21) & 0x1F;
                            uint32_t s_rt = (si >> 16) & 0x1F;
                            uint32_t s_rd = (si >> 11) & 0x1F;
                            uint32_t s_sa = (si >> 6)  & 0x1F;
                            uint32_t s_fn = si & 0x3F;

                            // LW $jr_reg, offset($base)
                            if (s_op == 0x23 && s_rt == jr_rs && lw_base_reg == 0xFF) {
                                lw_base_reg = s_rs;
                                lw_offset   = (int32_t)(int16_t)(si & 0xFFFF);
                                continue;
                            }
                            // ADDU $lw_base, $a, $b  — save both operands as candidates
                            if (s_op == 0x00 && s_fn == 0x21 &&
                                s_rd == lw_base_reg && lw_base_reg != 0xFF &&
                                addu_cand[0] == 0xFF) {
                                addu_cand[0] = s_rs;
                                addu_cand[1] = s_rt;
                                addu_dest    = s_rd;
                                table_reg    = s_rt;  // initial guess; may flip on LUI match
                                continue;
                            }
                            // ADDIU $cand, $cand, imm  (scanning back → before LUI)
                            if (s_op == 0x09 && addu_cand[0] != 0xFF) {
                                for (int c = 0; c < 2; c++) {
                                    if (!found_addiu[c] && addu_cand[c] != 0xFF &&
                                        s_rs == addu_cand[c] && s_rt == addu_cand[c]) {
                                        addiu_val[c]   = (int16_t)(si & 0xFFFF);
                                        found_addiu[c] = true;
                                        break;
                                    }
                                }
                                continue;
                            }
                            // LUI $cand, hi  — determines which candidate is the table base
                            if (s_op == 0x0F && addu_cand[0] != 0xFF && !found_lui) {
                                for (int c = 0; c < 2; c++) {
                                    if (addu_cand[c] != 0xFF && s_rt == addu_cand[c]) {
                                        lui_val   = ((uint32_t)(si & 0xFFFF)) << 16;
                                        table_reg = addu_cand[c];
                                        // carry over the ADDIU for this candidate
                                        if (!found_addiu[c]) { addiu_val[c] = 0; }
                                        // store addiu in slot 0 for convenience
                                        addiu_val[0]   = addiu_val[c];
                                        found_addiu[0] = found_addiu[c];
                                        found_lui      = true;
                                        break;
                                    }
                                }
                                continue;
                            }
                            // SLL $rd, $rt, 2  — the ×4 shift that turns the
                            // index into a byte offset. The destination must
                            // match one of the ADDU operands; the OTHER operand
                            // is the table-base register. NOPs (sa=0) are
                            // skipped because they fail the sa==2 check.
                            if (s_op == 0x00 && s_fn == 0x00 && s_sa == 2 &&
                                addu_cand[0] != 0xFF && !found_sll) {
                                for (int c = 0; c < 2; c++) {
                                    if (addu_cand[c] != 0xFF && s_rd == addu_cand[c]) {
                                        index_reg = s_rt;
                                        found_sll = true;
                                        break;
                                    }
                                }
                                continue;
                            }
                            // SLTIU — table entry count
                            if (s_op == 0x0B && table_count == 0) {
                                table_count = si & 0xFFFF;
                                continue;
                            }
                            if (found_lui && table_count != 0 && found_sll) break;
                        }

                        if (found_lui && table_count > 0 && table_count < 512) {
                            uint32_t base = lui_val +
                                (found_addiu[0] ? (uint32_t)(int32_t)addiu_val[0] : 0u);
                            table_base = base + (uint32_t)lw_offset;
                        }
                        // If SLL's matched candidate is the same as table_reg,
                        // detection is bogus (the ×4 result can't also be the
                        // base address). Drop the SLL match; old path applies.
                        if (found_sll && index_reg == table_reg && table_reg != 0xFF) {
                            found_sll = false;
                        }
                        (void)addu_dest;
                    }

                    // If we found a valid table, emit switch.
                    // Preferred path: when found_sll, switch on the index register
                    // with cases 0..N-1, baking the table targets into the C source.
                    // This dispatch is immune to runtime corruption of the jump-table
                    // bytes — the loaded value into jr_rs is ignored.
                    bool emitted_switch = false;
                    if (found_sll && table_base != 0 && table_count > 0) {
                        // Read all entries up-front; abort to the old path if any read fails.
                        std::vector<uint32_t> entries;
                        entries.reserve(table_count);
                        bool ok = true;
                        for (uint32_t i = 0; i < table_count; i++) {
                            auto eopt = exe_.read_word(table_base + i * 4);
                            if (!eopt) { ok = false; break; }
                            entries.push_back(*eopt);
                        }
                        if (ok) {
                            ss << config_.indent << fmt::format(
                                "/* jump table 0x{:08X}, {} entries — switching on index {} */\n",
                                table_base, table_count, reg_name(index_reg));
                            ss << config_.indent << fmt::format("switch ({}) {{\n", reg_name(index_reg));
                            for (uint32_t i = 0; i < table_count; i++) {
                                uint32_t t = entries[i];
                                if (cfg.blocks.count(t) || extra_labels_.count(t)) {
                                    ss << config_.indent << fmt::format(
                                        "    case {}u: goto block_{:08X};\n", i, t);
                                } else if (auto* ext = external_name_for(t)) {
                                    ss << config_.indent << fmt::format(
                                        "    case {}u: {}(cpu); return;\n", i, *ext);
                                } else if (auto* man = manual_name_for(t)) {
                                    ss << config_.indent << fmt::format(
                                        "    case {}u: {}(cpu); return;\n", i, *man);
                                } else if (known_functions_.count(t)) {
                                    ss << config_.indent << fmt::format(
                                        "    case {}u: {}{:08X}(cpu); return;\n",
                                        i, config_.func_prefix, t);
                                } else {
                                    ss << config_.indent << fmt::format(
                                        "    case {}u: call_by_address(cpu, 0x{:08X}u); return;\n", i, t);
                                }
                            }
                            ss << config_.indent << fmt::format(
                                "    default: call_by_address(cpu, {}); return;  /* out-of-range index */\n",
                                reg_name(jr_rs));
                            ss << config_.indent << "}\n";
                            emitted_switch = true;
                        }
                    }
                    // Fallback: old switch-on-loaded-address. Used when the
                    // back-scan couldn't identify the index register, or when
                    // the new emission aborted on a table-read failure.
                    if (!emitted_switch && table_base != 0 && table_count > 0) {
                        std::set<uint32_t>    seen;
                        std::vector<uint32_t> targets;
                        for (uint32_t i = 0; i < table_count; i++) {
                            auto eopt = exe_.read_word(table_base + i * 4);
                            if (eopt && seen.insert(*eopt).second &&
                                (cfg.blocks.count(*eopt) || extra_labels_.count(*eopt))) {
                                targets.push_back(*eopt);
                            }
                        }
                        if (!targets.empty()) {
                            ss << config_.indent << fmt::format("/* jump table 0x{:08X}, {} entries */\n", table_base, table_count);
                            ss << config_.indent << fmt::format("switch ({}) {{\n", reg_name(jr_rs));
                            for (uint32_t t : targets) {
                                ss << config_.indent << fmt::format("    case 0x{:08X}u: goto block_{:08X};\n", t, t);
                            }
                            ss << config_.indent << "    default: call_by_address(cpu, " << reg_name(jr_rs) << "); return;\n";
                            ss << config_.indent << "}\n";
                            emitted_switch = true;
                        }
                    }
                    if (!emitted_switch) {
                        // BIOS call or unrecognised indirect jump
                        ss << config_.indent << fmt::format("call_by_address(cpu, {});  /* jr {} */\n",
                                                            reg_name(jr_rs), reg_name(jr_rs));
                        ss << config_.indent << "return;\n";
                    }
                    }  // end of !jr_is_saved_ra branch
                } else if (block.exit_instr.type == ControlFlowType::JumpLink) {
                    // Function call (jal)
                    ss << config_.indent << "cpu->ra = " << fmt::format("0x{:08X};  /* return address */\n", addr + 8);
                    uint32_t target = block.exit_instr.target;
                    if (auto* ext = external_name_for(target)) {
                        ss << config_.indent << fmt::format("{}(cpu);  /* jal -> external */\n", *ext);
                    } else if (auto* man = manual_name_for(target)) {
                        ss << config_.indent << fmt::format("{}(cpu);  /* jal -> manual */\n", *man);
                    } else if (known_functions_.count(target) > 0) {
                        ss << config_.indent << fmt::format("{}{:08X}(cpu);  /* jal */\n", config_.func_prefix, target);
                    } else {
                        ss << config_.indent << fmt::format("call_by_address(cpu, 0x{:08X}u);  /* external jal */\n", target);
                    }
                    if (!block.successors.empty()) {
                        ss << config_.indent
                           << fmt::format("goto block_{:08X};  /* continue after call */\n", block.successors[0]);
                    } else {
                        // Split-function: JAL continuation is outside this function piece.
                        // Tail-call to the continuation piece (at exit_addr + 8, past delay slot).
                        uint32_t cont_addr = block.exit_instr.address + 8;
                        if (auto* ext = external_name_for(cont_addr)) {
                            ss << config_.indent
                               << fmt::format("{}(cpu); return;  /* jal cont: external */\n", *ext);
                        } else if (auto* man = manual_name_for(cont_addr)) {
                            ss << config_.indent
                               << fmt::format("{}(cpu); return;  /* jal cont: manual */\n", *man);
                        } else if (known_functions_.count(cont_addr)) {
                            ss << config_.indent
                               << fmt::format("{}{:08X}(cpu); return;  /* jal cont: split piece */\n", config_.func_prefix, cont_addr);
                        } else {
                            ss << config_.indent
                               << fmt::format("call_by_address(cpu, 0x{:08X}u); return;  /* jal cont: split */\n", cont_addr);
                        }
                    }
                } else if (block.exit_instr.type == ControlFlowType::JumpLinkReg) {
                    // Register indirect call (jalr $rs, $rd) — call function at rs, return to rd
                    uint32_t rs = get_rs(block.exit_instr.instruction);
                    uint32_t rd = get_rd(block.exit_instr.instruction);

                    // Set link register to return address (PC + 8, past delay slot)
                    ss << config_.indent << fmt::format("{} = 0x{:08X};  /* jalr return addr */\n",
                                                        reg_name(rd), addr + 8);

                    // Dispatch indirect call — target is a runtime register value
                    ss << config_.indent << fmt::format("call_by_address(cpu, {});  /* jalr {} */\n",
                                                        reg_name(rs), reg_name(rs));

                    // Continue to successor block (instruction at return address)
                    if (!block.successors.empty()) {
                        ss << config_.indent
                           << fmt::format("goto block_{:08X};  /* continue after jalr */\n",
                                          block.successors[0]);
                    } else {
                        // Split-function: JALR continuation is outside this function piece.
                        uint32_t cont_addr = block.exit_instr.address + 8;
                        if (known_functions_.count(cont_addr)) {
                            ss << config_.indent
                               << fmt::format("{}{:08X}(cpu); return;  /* jalr cont: split piece */\n", config_.func_prefix, cont_addr);
                        } else {
                            ss << config_.indent
                               << fmt::format("call_by_address(cpu, 0x{:08X}u); return;  /* jalr cont: split */\n", cont_addr);
                        }
                    }
                }
            }
        }

        addr += 4;
    }

    // If no explicit control flow, fall through to next block
    if (block.exit_instr.type == ControlFlowType::None && !block.successors.empty()) {
        ss << config_.indent << fmt::format("/* fall through to block_{:08X} */\n",
                                           block.successors[0]);
    }

    return ss.str();
}

GeneratedFunction CodeGenerator::generate_function(
    const Function& func,
    const ControlFlowGraph& cfg,
    const std::string& fallthrough_name) {

    GeneratedFunction result;
    result.function_name = func.name;
    result.signature = fmt::format("void {}(CPUState* cpu)", func.name);

    std::stringstream body_ss;
    body_ss << "{\n";

    // Override preamble: allows external code to intercept this function
    // without patching the generated file. psx_override_dispatch() is
    // implemented in tomba_runtime.c and checks the registered override table.
    body_ss << fmt::format("    if (psx_override_dispatch(cpu, 0x{:08X}u)) return;\n",
                           func.start_addr);

    // Add function comment
    if (config_.emit_comments) {
        body_ss << config_.indent
                << fmt::format("/* Address: 0x{:08X}, Size: {} bytes, Blocks: {} */\n",
                              func.start_addr, func.size, cfg.blocks.size());
    }

    // Pre-scan: find jump table targets that land mid-block (not a block boundary).
    // These need inline labels so goto statements in the switch can reach them.
    extra_labels_.clear();
    for (const auto& [baddr, blk] : cfg.blocks) {
        if (blk.exit_instr.type != ControlFlowType::JumpRegister) continue;
        uint32_t jr_r = get_rs(blk.exit_instr.instruction);
        // Run same backward scan as translate_basic_block to find table_base/count
        uint32_t tb = 0, tc = 0;
        {
            uint32_t lw_base = 0xFF; int32_t lw_off = 0;
            uint32_t ac[2] = {0xFF,0xFF}; uint32_t lui_v = 0;
            int16_t av[2] = {0,0}; bool fa[2] = {false,false}, fl = false;
            for (int b = 1; b <= 40; b++) {
                uint32_t sa = blk.exit_instr.address - (uint32_t)(b*4);
                auto so = exe_.read_word(sa); if (!so) break;
                uint32_t si=*so, s_op=(si>>26)&0x3F, s_rs=(si>>21)&0x1F,
                         s_rt=(si>>16)&0x1F, s_rd=(si>>11)&0x1F, s_fn=si&0x3F;
                if (s_op==0x23 && s_rt==jr_r && lw_base==0xFF) { lw_base=s_rs; lw_off=(int32_t)(int16_t)(si&0xFFFF); continue; }
                if (s_op==0x00 && s_fn==0x21 && s_rd==lw_base && lw_base!=0xFF && ac[0]==0xFF) { ac[0]=s_rs; ac[1]=s_rt; continue; }
                if (s_op==0x09 && ac[0]!=0xFF) { for(int c=0;c<2;c++){if(!fa[c]&&ac[c]!=0xFF&&s_rs==ac[c]&&s_rt==ac[c]){av[c]=(int16_t)(si&0xFFFF);fa[c]=true;break;}} continue; }
                if (s_op==0x0F && ac[0]!=0xFF && !fl) { for(int c=0;c<2;c++){if(ac[c]!=0xFF&&s_rt==ac[c]){lui_v=((uint32_t)(si&0xFFFF))<<16;if(!fa[c]){av[c]=0;}av[0]=av[c];fa[0]=fa[c];fl=true;break;}} continue; }
                if (s_op==0x0B && tc==0) { tc=si&0xFFFF; continue; }
                if (fl && tc!=0) break;
            }
            if (fl && tc>0 && tc<512) tb = (lui_v+(fa[0]?(uint32_t)(int32_t)av[0]:0u)) + (uint32_t)lw_off;
        }
        if (tb==0 || tc==0) continue;
        for (uint32_t i = 0; i < tc; i++) {
            auto eopt = exe_.read_word(tb + i*4);
            if (!eopt) continue;
            uint32_t t = *eopt;
            if (t >= cfg.function_start && t < cfg.function_end && !cfg.blocks.count(t))
                extra_labels_.insert(t);
        }
    }

    // Generate code for each basic block
    for (uint32_t block_addr : cfg.block_order) {
        const BasicBlock& block = cfg.blocks.at(block_addr);
        body_ss << "\n" << translate_basic_block(block, cfg);
    }

    // Emit fallthrough call if the function ends without explicit control flow.
    // A MIPS function truly falls through when:
    //   1. It has NO jr/jr $ra block anywhere (no explicit return or indirect jump)
    //   2. Its last basic block (by address) ends with no control flow (ControlFlowType::None)
    //      OR it ends with a branch/jump whose targets are all outside this function
    //      (indicating a split-function where the branch targets are in the next piece)
    // Functions that have dead code after a jr $ra (e.g., padding) must NOT get a
    // fallthrough call — condition 1 guards against that.
    if (!fallthrough_name.empty() && !cfg.block_order.empty()) {
        const BasicBlock& last_block = cfg.blocks.at(cfg.block_order.back());
        bool needs_fallthrough = false;

        if (last_block.exit_instr.type == ControlFlowType::None) {
            needs_fallthrough = true;
        } else if ((last_block.exit_instr.type == ControlFlowType::Branch ||
                    last_block.exit_instr.type == ControlFlowType::Jump) &&
                   last_block.successors.empty()) {
            // Branch/jump with no in-function successors — likely a split-function bug.
            // All targets are in the next function piece. Emit fallthrough as a safety net.
            fmt::print("  WARNING: func_{:08X} has out-of-function {} at block_{:08X}, "
                       "emitting fallthrough to {}\n",
                       func.start_addr,
                       last_block.exit_instr.type == ControlFlowType::Branch ? "branch" : "jump",
                       last_block.start_addr, fallthrough_name);
            needs_fallthrough = true;
        }

        if (needs_fallthrough) {
            // Emit fallthrough if the last block is reachable (has predecessors
            // or is the entry block). Dead code after a return (e.g., padding
            // nops) has no predecessors and should NOT get a fallthrough call.
            //
            // Jump-table dispatch (jr $v0 with computed target) doesn't get
            // recorded as predecessor edges in control_flow.cpp's CFG build
            // (JumpRegister adds no successors). A block that's only reachable
            // via a JT entry therefore appears unreachable here. If this
            // function contains any JumpRegister exit, treat the orphan block
            // as JT-reachable rather than dead padding. Spyro's
            // SetOverlayPointers (0x8005A470) hits this — case 10's body falls
            // sequentially into the next split function 0x8005A75C, but the
            // JT-only path leaves block_8005A718 with no predecessors.
            const BasicBlock& last = cfg.blocks.at(cfg.block_order.back());
            bool has_jr_dispatch = false;
            for (const auto& [_, blk] : cfg.blocks) {
                if (blk.exit_instr.type == ControlFlowType::JumpRegister) {
                    has_jr_dispatch = true;
                    break;
                }
            }
            bool is_reachable = last.is_entry || !last.predecessors.empty()
                                || has_jr_dispatch;
            if (is_reachable) {
                body_ss << fmt::format("    {}(cpu);  /* fallthrough to next function */\n",
                                       fallthrough_name);
            }
        }
    }
    body_ss << "    ;  /* label compatibility: C requires a statement after the last label */\n";
    body_ss << "}\n";

    result.body = body_ss.str();

    // Prepend annotation comment if one exists for this function's address
    std::string annotation_prefix;
    if (annotations_) {
        const std::string& note = annotations_->lookup(func.start_addr);
        if (!note.empty())
            annotation_prefix = fmt::format("/* [NOTE] {} */\n", note);
    }

    result.full_code = annotation_prefix + result.signature + "\n" + result.body;
    result.line_count = std::count(result.full_code.begin(), result.full_code.end(), '\n');

    return result;
}

std::vector<GeneratedFunction> CodeGenerator::generate_all_functions(
    const std::vector<Function>& functions,
    const std::map<uint32_t, ControlFlowGraph>& cfgs) {

    std::vector<GeneratedFunction> results;
    results.reserve(functions.size());

    fmt::print("\n=== C Code Generation ===\n\n");
    fmt::print("Generating C code for {} functions...\n", functions.size());

    // Build address -> function name map for fallthrough detection.
    // When a function ends without explicit control flow (ControlFlowType::None)
    // and the very next address is another function's start, it's a MIPS
    // fall-through: the recompiler must emit a tail call to the continuation.
    std::map<uint32_t, std::string> func_name_by_addr;
    for (const Function& f : functions) {
        func_name_by_addr[f.start_addr] = f.name;
    }
    // Also seed the fallthrough lookup with external (PSYQ) and manual-impl
    // symbols. A non-PSYQ function whose body falls into a PSYQ symbol must
    // emit a tail call by the *external* name (e.g. `MulMatrix0`), not by
    // the address-based `func_XXXXXXXX` (which has no body — it's an
    // extern). Address-only names cause undefined references at link time.
    for (const auto& [addr, name] : external_symbols_) {
        func_name_by_addr[addr] = name;
    }
    for (const auto& [addr, name] : manual_impls_) {
        func_name_by_addr[addr] = name;
    }

    int total_lines = 0;
    int n_external = 0, n_manual = 0, n_skipped_err = 0;

    for (const Function& func : functions) {
        // External (PSYQ) symbols — body is supplied at link time by PsyCross.
        // Do not emit a definition; the JAL emit path already routes calls to
        // the named extern.
        if (addr_is_external(func.start_addr)) {
            n_external++;
            continue;
        }

        // Manual implementation — same treatment as external, but the body is
        // a hand-written C function the link step pulls in.
        if (manual_name_for(func.start_addr)) {
            n_manual++;
            continue;
        }

        if (func.is_data_section) {
            // Emit a stub for data sections - do not decode instructions
            GeneratedFunction stub;
            stub.function_name = func.name;
            stub.signature = fmt::format("void {}(CPUState* cpu)", func.name);
            stub.body = fmt::format(
                "{{\n    if (psx_override_dispatch(cpu, 0x{:08X}u)) return;\n    /* data section 0x{:08X}: {} bytes - not executable code */\n}}\n",
                func.start_addr, func.start_addr, func.size);
            stub.full_code = stub.signature + "\n" + stub.body;
            stub.line_count = 4;
            results.push_back(stub);
            continue;
        }

        if (cfgs.count(func.start_addr) == 0) {
            continue; // Skip if no CFG
        }

        const ControlFlowGraph& cfg = cfgs.at(func.start_addr);

        // Detect fallthrough: is there a function immediately after this one?
        std::string fallthrough_name;
        uint32_t next_addr = func.start_addr + func.size;
        auto ft_it = func_name_by_addr.find(next_addr);
        if (ft_it != func_name_by_addr.end()) {
            fallthrough_name = ft_it->second;
        }

        GeneratedFunction gen_func;
        bool ok = false;
        if (skip_on_error_) {
            try {
                gen_func = generate_function(func, cfg, fallthrough_name);
                ok = true;
            } catch (const std::exception& e) {
                fmt::print(stderr,
                    "[skip-on-error] {} at 0x{:08X}: {}\n",
                    func.name, func.start_addr, e.what());
            } catch (...) {
                fmt::print(stderr,
                    "[skip-on-error] {} at 0x{:08X}: <unknown exception>\n",
                    func.name, func.start_addr);
            }
            if (!ok) {
                gen_func.function_name = func.name;
                gen_func.signature = fmt::format("void {}(CPUState* cpu)", func.name);
                gen_func.body = fmt::format(
                    "{{\n"
                    "    /* STUB: codegen failed for 0x{:08X}; --skip-on-error active. */\n"
                    "    fprintf(stderr, \"PSXRecomp stub: {} (0x{:08X}) called but not translated\\n\");\n"
                    "    abort();\n"
                    "}}\n",
                    func.start_addr, func.name, func.start_addr);
                gen_func.full_code = gen_func.signature + "\n" + gen_func.body;
                gen_func.line_count = 5;
                n_skipped_err++;
            }
        } else {
            gen_func = generate_function(func, cfg, fallthrough_name);
        }

        total_lines += gen_func.line_count;
        results.push_back(gen_func);
    }

    fmt::print("✓ Generated {} functions ({} external, {} manual, {} stubbed via --skip-on-error)\n",
               results.size(), n_external, n_manual, n_skipped_err);
    fmt::print("✓ Total C code: {} lines\n\n", total_lines);

    return results;
}

std::string CodeGenerator::generate_file(
    const std::vector<Function>& functions,
    const std::map<uint32_t, ControlFlowGraph>& cfgs) {

    std::stringstream ss;

    // File header
    ss << "/* Generated by PSXRecomp - PlayStation 1 Static Recompiler */\n\n";

    // Include the generic PSX runtime header.
    // This provides: CPUState typedef, gte_execute, call_by_address,
    // psx_syscall, psx_override_dispatch, and all other runtime API.
    ss << "#include \"psx_runtime.h\"\n\n";

    // Emit reference implementations for unaligned memory helpers.
    // These implement the MIPS lwl/lwr/swl/swr semantics.
    // The runtime may override these by setting the corresponding function pointers
    // in CPUState, but these standalone functions provide correct reference behavior.
    ss << "/* --- Unaligned memory access helper implementations --- */\n";
    ss << "\n";
    ss << "/* LWL: Load Word Left - merges high bytes from aligned word into rt.\n";
    ss << " * addr is the effective (possibly unaligned) address.\n";
    ss << " * rt_value is the current value of the destination register.\n";
    ss << " * MIPS big-endian convention: byte 0 is the most-significant byte.\n";
    ss << " * The PSX (little-endian) maps addr&3==0 -> load entire word,\n";
    ss << " *   addr&3==1 -> load 3 bytes into high 24 bits, etc.\n";
    ss << " * Formula:\n";
    ss << " *   aligned_addr = addr & ~3;\n";
    ss << " *   shift        = (addr & 3) * 8;\n";
    ss << " *   mask         = 0xFFFFFFFF << shift;\n";
    ss << " *   result       = (rt_value & ~mask) | (word << shift);\n";
    ss << " *   where word   = read_word(aligned_addr).\n";
    ss << " */\n";
    ss << "static uint32_t psx_lwl(CPUState* cpu, uint32_t addr, uint32_t rt_value) {\n";
    ss << "    uint32_t aligned_addr = addr & ~3u;\n";
    ss << "    uint32_t word  = cpu->read_word(aligned_addr);\n";
    ss << "    uint32_t shift = (addr & 3u) * 8u;\n";
    ss << "    uint32_t mask  = 0xFFFFFFFFu << shift;\n";
    ss << "    return (rt_value & ~mask) | (word << shift);\n";
    ss << "}\n";
    ss << "\n";
    ss << "/* LWR: Load Word Right - merges low bytes from aligned word into rt.\n";
    ss << " * addr is the effective (possibly unaligned) address.\n";
    ss << " * rt_value is the current value of the destination register.\n";
    ss << " * Formula:\n";
    ss << " *   aligned_addr = addr & ~3;\n";
    ss << " *   shift        = (3 - (addr & 3)) * 8;\n";
    ss << " *   mask         = 0xFFFFFFFF >> shift;\n";
    ss << " *   result       = (rt_value & ~mask) | (word >> shift);\n";
    ss << " *   where word   = read_word(aligned_addr).\n";
    ss << " */\n";
    ss << "static uint32_t psx_lwr(CPUState* cpu, uint32_t addr, uint32_t rt_value) {\n";
    ss << "    uint32_t aligned_addr = addr & ~3u;\n";
    ss << "    uint32_t word  = cpu->read_word(aligned_addr);\n";
    ss << "    uint32_t shift = (3u - (addr & 3u)) * 8u;\n";
    ss << "    uint32_t mask  = 0xFFFFFFFFu >> shift;\n";
    ss << "    return (rt_value & ~mask) | (word >> shift);\n";
    ss << "}\n";
    ss << "\n";
    ss << "/* SWL: Store Word Left - stores the high bytes of rt_value into memory.\n";
    ss << " * addr is the effective (possibly unaligned) address.\n";
    ss << " * rt_value is the value of the source register.\n";
    ss << " * Formula:\n";
    ss << " *   aligned_addr  = addr & ~3;\n";
    ss << " *   shift         = (addr & 3) * 8;\n";
    ss << " *   mem_mask      = 0xFFFFFFFF >> (32 - shift)  [i.e. low 'shift' bits preserved]\n";
    ss << " *   Actually:\n";
    ss << " *   mask          = 0xFFFFFFFF >> shift;  [bits to take from rt]\n";
    ss << " *   write_word(aligned_addr,\n";
    ss << " *       (read_word(aligned_addr) & ~mask) | (rt_value >> shift));\n";
    ss << " */\n";
    ss << "static void psx_swl(CPUState* cpu, uint32_t addr, uint32_t rt_value) {\n";
    ss << "    uint32_t aligned_addr = addr & ~3u;\n";
    ss << "    uint32_t existing = cpu->read_word(aligned_addr);\n";
    ss << "    uint32_t shift = (addr & 3u) * 8u;\n";
    ss << "    uint32_t mask  = 0xFFFFFFFFu >> shift;\n";
    ss << "    cpu->write_word(aligned_addr, (existing & ~mask) | (rt_value >> shift));\n";
    ss << "}\n";
    ss << "\n";
    ss << "/* SWR: Store Word Right - stores the low bytes of rt_value into memory.\n";
    ss << " * addr is the effective (possibly unaligned) address.\n";
    ss << " * rt_value is the value of the source register.\n";
    ss << " * Formula:\n";
    ss << " *   aligned_addr  = addr & ~3;\n";
    ss << " *   shift         = (3 - (addr & 3)) * 8;\n";
    ss << " *   mask          = 0xFFFFFFFF << shift;\n";
    ss << " *   write_word(aligned_addr,\n";
    ss << " *       (read_word(aligned_addr) & ~mask) | (rt_value << shift));\n";
    ss << " */\n";
    ss << "static void psx_swr(CPUState* cpu, uint32_t addr, uint32_t rt_value) {\n";
    ss << "    uint32_t aligned_addr = addr & ~3u;\n";
    ss << "    uint32_t existing = cpu->read_word(aligned_addr);\n";
    ss << "    uint32_t shift = (3u - (addr & 3u)) * 8u;\n";
    ss << "    uint32_t mask  = 0xFFFFFFFFu << shift;\n";
    ss << "    cpu->write_word(aligned_addr, (existing & ~mask) | (rt_value << shift));\n";
    ss << "}\n";
    ss << "\n";

    // Make mutable copies for potential function splitting
    std::vector<Function> functions_mut(functions);
    std::map<uint32_t, ControlFlowGraph> cfgs_mut(cfgs);

    // Build initial set of known functions
    std::set<uint32_t> known_addrs;
    for (const auto& func : functions_mut) {
        known_addrs.insert(func.start_addr);
    }

    // ---- Pre-pass: find mid-function branch/jump targets and split ----
    // Scan all CFGs for branch/jump targets that fall within a function's
    // range but are NOT at any function start. Split containing functions
    // at discovered targets. Iterate up to 3 times since splitting creates
    // new pieces whose branches may target other mid-piece addresses.
    // Each subsequent pass only scans CFGs that were rebuilt in the prior pass.
    {
        std::set<uint32_t> cfgs_to_scan;  // Which CFGs to scan (empty = all)
        uint32_t exe_start = exe_.header.load_address;
        uint32_t exe_end = exe_.end_address();
        int total_new = 0;

        for (int pass = 0; pass < 3; pass++) {
            std::set<uint32_t> mid_targets;

            // Scan either all CFGs (pass 0) or only rebuilt CFGs (pass 1+)
            auto scan_cfg = [&](const ControlFlowGraph& cfg) {
                for (const auto& [baddr, block] : cfg.blocks) {
                    auto check_target = [&](uint32_t target) {
                        if (target == 0) return;
                        if (cfg.blocks.count(target)) return;
                        if (known_addrs.count(target)) return;
                        if (target < exe_start || target >= exe_end) return;
                        if (target & 3) return;
                        mid_targets.insert(target);
                    };

                    if (block.exit_instr.type == ControlFlowType::Branch) {
                        check_target(block.exit_instr.target);
                        check_target(block.exit_instr.address + 8);
                    } else if (block.exit_instr.type == ControlFlowType::Jump) {
                        check_target(block.exit_instr.target);
                    }
                }
            };

            if (cfgs_to_scan.empty()) {
                for (const auto& [cfg_addr, cfg] : cfgs_mut) scan_cfg(cfg);
            } else {
                for (uint32_t addr : cfgs_to_scan) {
                    auto it = cfgs_mut.find(addr);
                    if (it != cfgs_mut.end()) scan_cfg(it->second);
                }
            }

            if (mid_targets.empty()) break;

            fmt::print("Pre-pass {}: {} mid-function branch targets\n", pass + 1, mid_targets.size());

            // Sort functions for binary search
            std::sort(functions_mut.begin(), functions_mut.end(),
                [](const Function& a, const Function& b) { return a.start_addr < b.start_addr; });
            std::vector<uint32_t> func_starts;
            for (const auto& f : functions_mut) func_starts.push_back(f.start_addr);

            // Group targets by containing function
            std::map<uint32_t, std::vector<uint32_t>> splits_by_func;
            for (uint32_t target : mid_targets) {
                auto it = std::upper_bound(func_starts.begin(), func_starts.end(), target);
                if (it == func_starts.begin()) continue;
                --it;
                splits_by_func[*it].push_back(target);
            }

            // Split each affected function
            std::vector<Function> new_funcs;
            std::set<uint32_t> affected;

            for (auto& [func_start, targets] : splits_by_func) {
                std::sort(targets.begin(), targets.end());

                for (auto& f : functions_mut) {
                    if (f.start_addr != func_start) continue;
                    if (targets.back() >= f.end_addr) break;

                    uint32_t orig_end = f.end_addr;
                    bool orig_data = f.is_data_section;

                    f.end_addr = targets[0];
                    f.size = f.end_addr - f.start_addr;
                    affected.insert(f.start_addr);

                    for (size_t ti = 0; ti < targets.size(); ti++) {
                        Function nf;
                        nf.start_addr = targets[ti];
                        nf.end_addr = (ti + 1 < targets.size()) ? targets[ti + 1] : orig_end;
                        nf.size = nf.end_addr - nf.start_addr;
                        nf.name = fmt::format("{}{:08X}", config_.func_prefix, nf.start_addr);
                        nf.has_prologue = false;
                        nf.has_epilogue = false;
                        nf.stack_frame_size = 0;
                        nf.is_data_section = orig_data;
                        new_funcs.push_back(nf);
                        known_addrs.insert(nf.start_addr);
                        affected.insert(nf.start_addr);
                    }
                    break;
                }
            }

            functions_mut.insert(functions_mut.end(), new_funcs.begin(), new_funcs.end());
            std::sort(functions_mut.begin(), functions_mut.end(),
                [](const Function& a, const Function& b) { return a.start_addr < b.start_addr; });

            // Rebuild CFGs for affected functions
            ControlFlowAnalyzer cfg_analyzer(exe_);
            cfgs_to_scan.clear();
            for (auto& f : functions_mut) {
                if (affected.count(f.start_addr)) {
                    cfgs_mut[f.start_addr] = cfg_analyzer.analyze_function(f);
                    cfgs_to_scan.insert(f.start_addr);
                }
            }

            total_new += (int)new_funcs.size();
            fmt::print("  Created {} new entry points\n", new_funcs.size());
        }

        if (total_new > 0) {
            fmt::print("Pre-pass total: {} new entry points, {} functions now\n",
                       total_new, functions_mut.size());
        }
    }

    set_known_functions(known_addrs);

    // Generate all functions first (to collect names). Externals and manual
    // impls are filtered out inside generate_all_functions; their JAL targets
    // resolve via extern declarations emitted below.
    auto gen_funcs = generate_all_functions(functions_mut, cfgs_mut);

    // Externs for PSYQ (external) symbols — bodies supplied by PsyCross.
    if (!external_symbols_.empty()) {
        ss << "/* --- External (PSYQ) symbol declarations --- */\n";
        ss << "/* Bodies supplied by PsyCross at link time. Function bodies are\n";
        ss << " * NOT emitted in this translation unit; only externs and a\n";
        ss << " * dispatch entry. */\n";
        for (const auto& [addr, name] : external_symbols_) {
            ss << fmt::format("extern void {}(CPUState* cpu);  /* PSYQ @ 0x{:08X} */\n",
                              name, addr);
        }
        ss << "\n";
    }

    // Externs for manual-impl functions — bodies supplied by hand-written C.
    if (!manual_impls_.empty()) {
        ss << "/* --- Manual-implementation declarations --- */\n";
        for (const auto& [addr, name] : manual_impls_) {
            ss << fmt::format("extern void {}(CPUState* cpu);  /* manual @ 0x{:08X} */\n",
                              name, addr);
        }
        ss << "\n";
    }

    // Forward declarations for all translated functions
    if (!gen_funcs.empty()) {
        ss << "/* Forward declarations */\n";
        for (const auto& gen_func : gen_funcs) {
            ss << gen_func.signature << ";\n";
        }
        ss << "\n";
    }

    // Function definitions
    for (const auto& gen_func : gen_funcs) {
        ss << gen_func.full_code << "\n";
    }

    return ss.str();
}

} // namespace PSXRecomp
