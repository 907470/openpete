// SPDX-FileCopyrightText: 2026 Tyson Cleary
// SPDX-License-Identifier: MIT

// CD-ROM recorder calls, compiled inside cdrom.cpp after its state so they
// read the parameter and response FIFOs directly. cdrom.cpp includes
// cpu_core.h and openpete_hooks.h at its top.

/// Reports an interrupt raised to the CPU with the response FIFO's contents, when the interrupt hook is set.
static void OpenpeteTraceInterrupt(u8 interrupt)
{
  if (!CDROM::g_interrupt_hook) [[likely]]
    return;
  u8 resp[RESPONSE_FIFO_SIZE];
  const u32 n = std::min<u32>(s_state.response_fifo.GetSize(), RESPONSE_FIFO_SIZE);
  for (u32 i = 0; i < n; i++)
    resp[i] = s_state.response_fifo.Peek(i);
  CDROM::g_interrupt_hook(interrupt, resp, n);
}

/// Reports a command byte with the writing instruction's address and the parameter FIFO, when the command hook is
/// set.
static void OpenpeteTraceCommand(u8 command)
{
  if (!CDROM::g_command_hook) [[likely]]
    return;
  u8 params[PARAM_FIFO_SIZE];
  const u32 n = std::min<u32>(s_state.param_fifo.GetSize(), PARAM_FIFO_SIZE);
  for (u32 i = 0; i < n; i++)
    params[i] = s_state.param_fifo.Peek(i);
  CDROM::g_command_hook(command, CPU::g_state.current_instruction_pc, params, n);
}

/// Reads register `offset` through CDROM::ReadRegister and reports the value to the read hook. Returns false without
/// reading when called from inside that read, so the register read's own body runs.
static bool OpenpeteTraceRead(u32 offset, u8* value)
{
  static bool s_inside = false;
  if (s_inside)
    return false;
  s_inside = true;
  *value = CDROM::ReadRegister(offset);
  s_inside = false;
  CDROM::g_read_hook(offset, *value, CPU::g_state.current_instruction_pc);
  return true;
}
