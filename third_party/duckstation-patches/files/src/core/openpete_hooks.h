// SPDX-FileCopyrightText: 2026 Tyson Cleary
// SPDX-License-Identifier: MIT

// Hook points of the openpete trace recorders. Each hook is null until a
// recorder sets it; a call site tests it and calls it only when set. The
// variables are C++17 inline variables, so every translation unit shares
// one definition and no build list names this file.

#pragma once

#include "common/types.h"
#include "types.h"

#include <cstdlib>
#include <cstring>

namespace CPU {
/// Called by the interpreter's debug dispatcher (the path any breakpoint selects) before each instruction executes,
/// with its address and word. The recompilers and the non-debug interpreter do not call it.
using InstructionHook = void (*)(u32 pc, u32 bits);
inline InstructionHook g_instruction_hook = nullptr;

/// Called whenever an exception is raised, with the CAUSE bits and EPC as stored (EPC already moved back to the
/// branch when the exception hit a branch delay slot), before the mode bits are pushed and the vector is fetched.
using ExceptionHook = void (*)(u32 cause_bits, u32 epc);
inline ExceptionHook g_exception_hook = nullptr;
} // namespace CPU

namespace CDROM {
/// A command byte written to the controller's command register, with the writing instruction's address and the
/// parameter FIFO's contents.
using CommandHook = void (*)(u8 command, u32 pc, const u8* params, u32 param_count);
/// An interrupt raised to the CPU (INT1..INT5), with the response FIFO's contents at that moment.
using InterruptHook = void (*)(u8 interrupt, const u8* response, u32 response_count);
/// A register read by the CPU: the register offset (0..3), the value it returned and the reading instruction's
/// address.
using ReadHook = void (*)(u32 offset, u8 value, u32 pc);
inline CommandHook g_command_hook = nullptr;
inline InterruptHook g_interrupt_hook = nullptr;
inline ReadHook g_read_hook = nullptr;
} // namespace CDROM

namespace GTE {
/// Called after every instruction the interpreter path executes, with the instruction word and the 64 data and
/// control registers as they were before it (the registers after it are readable through GetRegisterPtr). The
/// recompilers dispatch instruction implementations directly and bypass it.
using TraceHook = void (*)(u32 inst_bits, const u32 pre_regs[64]);
inline TraceHook g_trace_hook = nullptr;

/// Runs one instruction through `execute` with the registers `regs` copied first, then calls the trace hook.
/// Returns false without running anything when called from inside that run, so the caller's own body executes it.
inline bool TraceInstruction(u32 inst_bits, const u32* regs, void (*execute)(u32))
{
  static bool s_inside = false;
  if (s_inside)
    return false;
  u32 pre[64];
  std::memcpy(pre, regs, sizeof(pre));
  s_inside = true;
  execute(inst_bits);
  s_inside = false;
  g_trace_hook(inst_bits, pre);
  return true;
}
} // namespace GTE

namespace Timers {
/// Called with the timer index and the value on every read of a timer's current-count register.
inline void (*g_count_read_hook)(u32 timer, u32 value) = nullptr;
} // namespace Timers

namespace OpenpetePad {
/// Every command byte an analog controller accepts (0x42 poll, 0x43 config mode, 0x44..0x4D configuration commands)
/// with the controller's index.
using CommandHook = void (*)(u32 index, u8 command);
inline CommandHook g_command_hook = nullptr;

/// Every change of an analog controller's motor state, with the controller's index, the motor (0 small, 1 large),
/// the new state byte and the intensity handed to the host input layer.
using MotorHook = void (*)(u32 index, u32 motor, u8 value, float intensity);
inline MotorHook g_motor_hook = nullptr;
} // namespace OpenpetePad

namespace SPU {
/// Playback head of one voice, for trace recorders.
struct VoiceHead
{
  u32 voice;
  u32 start_address;   ///< ADPCM start address register, in bytes.
  u32 current_address; ///< Address of the ADPCM block being decoded, in bytes.
  u32 counter;         ///< 4.12 position: bits 12-16 sample in the block, bits 4-11 interpolation fraction.
  bool on;             ///< The envelope is not in the off phase.
};
static constexpr u32 VOICE_COUNT = 24;

/// Fills `out` with voice `voice`'s head as of the last generated sample; false for an index past the last voice.
bool GetVoiceHead(u32 voice, VoiceHead* out);
} // namespace SPU

namespace System {
/// Global tick of the latest frame-number increment (the VBLANK interrupt line's rise); written by the frame counter.
inline GlobalTicks g_openpete_frame_start_tick = 0;

/// The global tick at which the current frame number was reached (the VBLANK interrupt line's rise).
inline GlobalTicks GetFrameStartTick()
{
  return g_openpete_frame_start_tick;
}
} // namespace System
