// SPDX-FileCopyrightText: 2026 Tyson Cleary
// SPDX-License-Identifier: MIT

#include "probe_capture.h"

#include <ctime>

#include "core/bus.h"
#include "core/cpu_core.h"
#include "core/openpete_hooks.h"
#include "core/interrupt_controller.h"
#include "core/system.h"
#include "core/timing_event.h"

#include "common/log.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

LOG_CHANNEL(Host);

namespace PcHooks {
namespace {
std::unordered_map<u32, std::vector<Fn>> s_hooks;

bool Dispatch(CPU::BreakpointType type, VirtualMemoryAddress pc, VirtualMemoryAddress memaddr)
{
  (void)type;
  (void)memaddr;
  const u32 key = pc & 0x0FFFFFFFu;
  auto it = s_hooks.find(key);
  if (it == s_hooks.end())
    return false;
  // A callback may add hooks (at this address too); run over a moved-out
  // list and merge what was added while it ran.
  std::vector<Fn> run = std::move(it->second);
  it->second.clear();
  std::vector<Fn> keep;
  for (Fn& fn : run)
    if (fn(pc))
      keep.push_back(std::move(fn));
  auto again = s_hooks.find(key);
  for (Fn& fn : again->second)
    keep.push_back(std::move(fn));
  again->second = std::move(keep);
  if (again->second.empty())
  {
    s_hooks.erase(again);
    return false;
  }
  return true;
}
} // namespace

bool Add(u32 pc, Fn fn)
{
  const u32 key = pc & 0x0FFFFFFFu;
  auto it = s_hooks.find(key);
  if (it != s_hooks.end())
  {
    it->second.push_back(std::move(fn));
    return true;
  }
  if (!CPU::AddBreakpointWithCallback(CPU::BreakpointType::Execute, pc, &Dispatch))
    return false;
  s_hooks[key].push_back(std::move(fn));
  return true;
}

} // namespace PcHooks

namespace RamDump {
namespace {

struct Spec
{
  u32 pc = 0;
  bool has_t1 = false, has_a0 = false;
  u32 t1 = 0, a0 = 0;
  u32 hit = 1;
  bool ret = false;
  std::string path;
  u32 hits = 0;
  bool done = false;
};

std::vector<Spec> s_specs;

u64 ExactTick()
{
  return TimingEvents::GetGlobalTickCounter() + CPU::GetPendingTicks();
}

void WriteRegs(std::FILE* fp, const char* label)
{
  const CPU::Registers& r = CPU::g_state.regs;
  std::fprintf(fp, "%s", label);
  for (u32 i = 0; i < 32; i++)
    std::fprintf(fp, " %08x", r.r[i]);
  std::fprintf(fp, "\n");
}

void Dump(Spec& s, u32 pc, const CPU::Registers* at_call)
{
  s.done = true;
  std::FILE* fp = std::fopen(s.path.c_str(), "wb");
  if (!fp || std::fwrite(Bus::g_ram, 1, Bus::g_ram_size, fp) != Bus::g_ram_size)
  {
    ERROR_LOG("ram-dump: cannot write {}", s.path);
    if (fp)
      std::fclose(fp);
    return;
  }
  std::fclose(fp);
  const std::string txt = s.path + ".txt";
  fp = std::fopen(txt.c_str(), "w");
  if (!fp)
  {
    ERROR_LOG("ram-dump: cannot write {}", txt);
    return;
  }
  std::fprintf(fp, "pc %08x frame %u tick %llu hit %u ret %d\n", pc, System::GetFrameNumber(),
               static_cast<unsigned long long>(ExactTick()), s.hits, s.ret ? 1 : 0);
  std::fprintf(fp, "# columns: r0..r31\n");
  WriteRegs(fp, "regs");
  if (at_call)
  {
    std::fprintf(fp, "call");
    for (u32 i = 0; i < 32; i++)
      std::fprintf(fp, " %08x", at_call->r[i]);
    std::fprintf(fp, "\n");
  }
  std::fclose(fp);
  INFO_LOG("ram-dump: {} at pc {:08X} frame {} (hit {})", s.path, pc, System::GetFrameNumber(), s.hits);
}

bool OnHit(size_t idx, u32 pc)
{
  Spec& s = s_specs[idx];
  if (s.done)
    return false;
  const CPU::Registers& r = CPU::g_state.regs;
  if ((s.has_t1 && r.t1 != s.t1) || (s.has_a0 && r.a0 != s.a0))
    return true;
  if (++s.hits < s.hit)
    return true;
  if (!s.ret)
  {
    Dump(s, pc, nullptr);
    return false;
  }
  const CPU::Registers call = r;
  const u32 ra = r.ra;
  if (!PcHooks::Add(ra, [idx, call](u32 ret_pc) {
        Dump(s_specs[idx], ret_pc, &call);
        return false;
      }))
  {
    ERROR_LOG("ram-dump: cannot hook the return address {:08X} of {}", ra, s.path);
    s.done = true;
  }
  return false;
}

} // namespace

bool AddSpec(const std::string& spec, std::string* error_out)
{
  const size_t eq = spec.rfind('=');
  if (eq == std::string::npos || eq + 1 >= spec.size())
  {
    *error_out = "-ram-dump-at wants PC[,t1=HEX][,a0=HEX][,hit=N][,ret]=PATH, got " + spec;
    return false;
  }
  Spec s;
  s.path = spec.substr(eq + 1);
  const std::string keys = spec.substr(0, eq);
  size_t pos = 0;
  bool first = true;
  while (pos <= keys.size())
  {
    const size_t comma = keys.find(',', pos);
    const std::string tok = keys.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    if (first)
      s.pc = static_cast<u32>(std::strtoul(tok.c_str(), nullptr, 16));
    else if (tok.rfind("t1=", 0) == 0)
    {
      s.has_t1 = true;
      s.t1 = static_cast<u32>(std::strtoul(tok.c_str() + 3, nullptr, 16));
    }
    else if (tok.rfind("a0=", 0) == 0)
    {
      s.has_a0 = true;
      s.a0 = static_cast<u32>(std::strtoul(tok.c_str() + 3, nullptr, 16));
    }
    else if (tok.rfind("hit=", 0) == 0)
      s.hit = static_cast<u32>(std::strtoul(tok.c_str() + 4, nullptr, 10));
    else if (tok == "ret")
      s.ret = true;
    else
    {
      *error_out = "-ram-dump-at: unknown key '" + tok + "' in " + spec;
      return false;
    }
    first = false;
    if (comma == std::string::npos)
      break;
    pos = comma + 1;
  }
  if (s.hit == 0)
  {
    *error_out = "-ram-dump-at: hit counts from 1 in " + spec;
    return false;
  }
  s_specs.push_back(std::move(s));
  return true;
}

bool Install(std::string* error_out)
{
  for (size_t i = 0; i < s_specs.size(); i++)
  {
    if (!PcHooks::Add(s_specs[i].pc, [i](u32 pc) { return OnHit(i, pc); }))
    {
      *error_out = "-ram-dump-at: cannot hook " + s_specs[i].path;
      return false;
    }
    INFO_LOG("ram-dump: {} keyed on pc {:08X} hit {}{}", s_specs[i].path, s_specs[i].pc, s_specs[i].hit,
             s_specs[i].ret ? " (at return)" : "");
  }
  return true;
}

void Shutdown()
{
  for (const Spec& s : s_specs)
    if (!s.done)
      WARNING_LOG("ram-dump: {} never dumped ({} of {} hits)", s.path, s.hits, s.hit);
}

} // namespace RamDump

namespace CardWindow {
namespace {

std::FILE* s_fp = nullptr;
CPU::InstructionHook s_prev_insn = nullptr;
CPU::ExceptionHook s_prev_exc = nullptr;
u32 s_depth = 0;                       // exceptions being serviced
std::vector<u32> s_handlers;           // card event handler entry addresses
// An active card handler or card-class DeliverEvent call: the return address and the stack pointer at
// entry. The call has returned when execution reaches ret with sp equal to sp at entry.
struct ActiveCall
{
  u32 ret, sp;
};
std::vector<ActiveCall> s_handler_ret;
size_t s_handler_ret_max = 0;          // deepest s_handler_ret over the run
u32 s_entries_uncalled = 0;            // executions of a handler address not reached by a call
// The two instructions executed before the current one, for the call test at a handler address; saved
// per exception level and restored at the instruction after its rfe.
struct History
{
  u32 pc, bits, pc2, bits2;
};
History s_hist{};
std::vector<History> s_hist_saved;
bool s_hist_restore = false;
std::vector<u8> s_handler_written;     // one byte per RAM byte: bit 0 a card handler or a card-class
                                       // DeliverEvent stored it, bit 1 an SIO interrupt's service did
bool s_exc_sio = false;                // the exception being serviced was raised by IRQ7 alone
u32 s_deliver_pc = 0;                  // the kernel's DeliverEvent, from the B0h table
bool s_deliver_hooked = false;
// SIO0 transfer decode: bytes stored to JOY_TX_DATA after a 0x81 select.
u32 s_tx_index = 0;
u8 s_tx_cmd = 0;
u32 s_tx_sector = 0;
u32 s_joy_ctrl = 0;
// Window state.
bool s_window_open = false;
bool s_window_card = false;            // a card command was issued since the window's VBLANK
u32 s_window_frame = 0;
struct FirstRead
{
  u32 pc, frame;
  u8 width;
};
std::unordered_map<u32, FirstRead> s_window_reads; // this window's main-path loads
struct Committed
{
  u32 pc, frame, count;
  u8 width;
};
std::unordered_map<u32, Committed> s_reads;        // loads of committed windows
u32 s_rows_e = 0, s_rows_d = 0, s_rows_c = 0, s_rows_h = 0, s_rows_s = 0, s_rows_t = 0, s_windows = 0,
    s_windows_committed = 0;
u64 s_window_loads = 0;

u64 Tick()
{
  return TimingEvents::GetGlobalTickCounter() + CPU::GetPendingTicks();
}

inline u32 Frame()
{
  return System::GetFrameNumber();
}

bool IsCardClass(u32 cls)
{
  return cls == 0xF0000011u || cls == 0xF4000001u || cls == 0xF4000002u;
}

inline bool RamAddr(u32 addr, u32* off)
{
  const u32 phys = addr & 0x1FFFFFFFu;
  if (phys >= 0x800000u)
    return false;
  *off = phys & (Bus::g_ram_size - 1u);
  return true;
}

constexpr u32 kWrittenRowFrames = 2000; // a W row at every VBLANK whose frame is a multiple of this

// Bytes with bit 0 (card handler or card-class delivery) and bit 1 (IRQ7-only service) set.
std::pair<u32, u32> CountWritten()
{
  u32 written = 0, written_sio = 0;
  for (u8 b : s_handler_written)
  {
    written += b & 1u;
    written_sio += (b >> 1) & 1u;
  }
  return {written, written_sio};
}

void CommitWindow()
{
  if (!s_window_open || !s_window_card)
    return;
  s_windows_committed++;
  for (const auto& [addr, fr] : s_window_reads)
  {
    auto it = s_reads.find(addr);
    if (it == s_reads.end())
      s_reads.emplace(addr, Committed{fr.pc, fr.frame, 1u, fr.width});
    else
      it->second.count++;
  }
  s_window_reads.clear();
  s_window_open = false;
}

void PushCall(u32 ret, u32 sp)
{
  s_handler_ret.push_back(ActiveCall{ret & 0x1FFFFFFFu, sp});
  s_handler_ret_max = std::max(s_handler_ret_max, s_handler_ret.size());
}

// True when the instruction at pc is reached as the target of the jal/jalr two instructions before it in
// h (the link register holds the address after that jump's delay slot). A handler address reached any
// other way is code occupying that address at the time (an overlay loaded over the handler), not a call.
bool ReachedByCall(const History& h, u32 pc)
{
  const u32 op = h.bits2 >> 26;
  const bool jal = op == 0x03u && ((h.pc2 & 0xF0000000u) | ((h.bits2 & 0x03FFFFFFu) << 2)) == pc;
  const bool jalr = op == 0x00u && (h.bits2 & 0x3Fu) == 0x09u;
  return (jal || jalr) && h.pc == h.pc2 + 4u && CPU::g_state.regs.ra == h.pc2 + 8u;
}

bool OnDeliver(u32 pc)
{
  (void)pc;
  const CPU::Registers& r = CPU::g_state.regs;
  std::fprintf(s_fp, "D %u %llu %08x %04x %08x\n", Frame(), static_cast<unsigned long long>(Tick()), r.a0, r.a1,
               r.ra);
  s_rows_d++;
  // The delivery's own stores (the event table) count with the handler's.
  if (IsCardClass(r.a0))
    PushCall(r.ra, r.sp);
  return true;
}

bool OnOpenEvent(u32 pc)
{
  (void)pc;
  const CPU::Registers& r = CPU::g_state.regs;
  if (r.t1 != 0x08u)
    return true;
  std::fprintf(s_fp, "E %u %llu %08x %04x %04x %08x\n", Frame(), static_cast<unsigned long long>(Tick()), r.a0, r.a1,
               r.a2, r.a3);
  s_rows_e++;
  if (IsCardClass(r.a0) && r.a3 != 0 &&
      std::find(s_handlers.begin(), s_handlers.end(), r.a3 & 0x1FFFFFFFu) == s_handlers.end())
    s_handlers.push_back(r.a3 & 0x1FFFFFFFu);
  if (!s_deliver_hooked)
  {
    // B0h table: GetB0Table's answer, entry 07h DeliverEvent.
    u32 table = 0, fn = 0;
    if (CPU::SafeReadMemoryWord(0x874u, &table) && table == 0x874u)
      table = 0x874u;
    if (CPU::SafeReadMemoryWord(0x874u + 7u * 4u, &fn) && fn != 0)
    {
      s_deliver_pc = fn;
      s_deliver_hooked = PcHooks::Add(fn, &OnDeliver);
      INFO_LOG("card-window: DeliverEvent at {:08X}{}", fn, s_deliver_hooked ? "" : " (hook refused)");
    }
  }
  return true;
}

// The card driver's kernel bytes an S row logs: the fast track's count, the
// port-flip byte, the driver block 0x74A0-0x75CF, the bu layer's callback
// flags and sector buffers, and the exception chain and event table.
bool IsCardDriverByte(u32 phys)
{
  return (phys >= 0x72F0u && phys < 0x72F4u) || phys == 0x7264u || (phys >= 0x74A0u && phys < 0x75D0u) ||
         (phys >= 0xB9D0u && phys < 0xB9E4u) || (phys >= 0xBE48u && phys < 0xBF48u) ||
         (phys >= 0xE000u && phys < 0xE290u);
}

// CPU ticks since the VBLANK interrupt line rose for the current frame.
inline u64 FrameOffset()
{
  return Tick() - System::GetFrameStartTick();
}

void OnStore(u32 pc, u32 bits, u32 addr, u32 width)
{
  const u32 rt = (bits >> 16) & 31u;
  const u32 value = CPU::g_state.regs.r[rt];
  const u32 phys = addr & 0x1FFFFFFFu;
  if (IsCardDriverByte(phys))
  {
    std::fprintf(s_fp, "S %u %llu %llu %04x %u %08x %08x\n", Frame(), static_cast<unsigned long long>(Tick()),
                 static_cast<unsigned long long>(FrameOffset()), phys, width, value, pc);
    s_rows_s++;
  }
  if (phys == 0x1F801040u)
  {
    std::fprintf(s_fp, "T %u %llu %llu %02x\n", Frame(), static_cast<unsigned long long>(Tick()),
                 static_cast<unsigned long long>(FrameOffset()), value & 0xFFu);
    s_rows_t++;
  }
  if (phys == 0x1F80104Au)
    s_joy_ctrl = value & 0xFFFFu;
  else if (phys == 0x1F801040u)
  {
    const u8 b = static_cast<u8>(value);
    if (s_tx_index == 0)
    {
      if (b == 0x81u)
        s_tx_index = 1;
      return;
    }
    if (s_tx_index == 1 && (b == 0x81u || b == 0x01u))
    {
      // A select byte where the command byte belongs: the previous select
      // got no acknowledge (empty slot), logged as command 00 sector -2.
      std::fprintf(s_fp, "C %u %llu %u 00 -2\n", Frame(), static_cast<unsigned long long>(Tick()),
                   (s_joy_ctrl >> 13) & 1u);
      s_rows_c++;
      s_tx_index = (b == 0x81u) ? 1u : 0u;
      return;
    }
    if (s_tx_index == 1)
    {
      s_tx_cmd = b;
      s_tx_sector = 0;
      if (b != 'R' && b != 'W')
      {
        std::fprintf(s_fp, "C %u %llu %u %02x -1\n", Frame(), static_cast<unsigned long long>(Tick()),
                     (s_joy_ctrl >> 13) & 1u, b);
        s_rows_c++;
        s_tx_index = 0;
        if (s_window_open)
          s_window_card = true;
        return;
      }
      s_tx_index = 2;
      return;
    }
    // 'R'/'W': bytes 2 and 3 are the card ID bytes (0x00), 4 and 5 the sector.
    s_tx_index++;
    if (s_tx_index == 5)
      s_tx_sector = static_cast<u32>(b) << 8;
    else if (s_tx_index == 6)
    {
      s_tx_sector |= b;
      std::fprintf(s_fp, "C %u %llu %u %02x %u\n", Frame(), static_cast<unsigned long long>(Tick()),
                   (s_joy_ctrl >> 13) & 1u, s_tx_cmd, s_tx_sector);
      s_rows_c++;
      s_tx_index = 0;
      if (s_window_open)
        s_window_card = true;
    }
    (void)pc;
    return;
  }
  const u8 cat = static_cast<u8>((s_handler_ret.empty() ? 0u : 1u) | ((s_depth > 0 && s_exc_sio) ? 2u : 0u));
  if (!cat)
    return;
  u32 off;
  if (!RamAddr(addr, &off))
    return;
  for (u32 i = 0; i < width && off + i < s_handler_written.size(); i++)
    s_handler_written[off + i] |= cat;
}

void InstructionHook(u32 pc, u32 bits)
{
  if (s_prev_insn)
    s_prev_insn(pc, bits);
  // The previous instruction was the rfe: this one resumes the interrupted context, whose history the
  // exception saved.
  if (s_hist_restore)
  {
    s_hist_restore = false;
    if (!s_hist_saved.empty())
    {
      s_hist = s_hist_saved.back();
      s_hist_saved.pop_back();
    }
  }
  const History hist = s_hist; // the two instructions before this one
  s_hist = History{pc, bits, hist.pc, hist.bits};
  const u32 op = bits >> 26;
  if (op == 0x10u && (bits & (1u << 25)) && (bits & 0x3Fu) == 0x10u)
  {
    // rfe, in the delay slot of the return jump.
    if (s_depth > 0)
      s_depth--;
    s_hist_restore = true;
    return;
  }
  const u32 masked = pc & 0x1FFFFFFFu;
  if (!s_handler_ret.empty() && masked == s_handler_ret.back().ret &&
      CPU::g_state.regs.sp == s_handler_ret.back().sp)
    s_handler_ret.pop_back();
  if (std::find(s_handlers.begin(), s_handlers.end(), masked) != s_handlers.end())
  {
    if (ReachedByCall(hist, pc))
    {
      std::fprintf(s_fp, "H %u %llu %08x\n", Frame(), static_cast<unsigned long long>(Tick()), pc);
      s_rows_h++;
      PushCall(CPU::g_state.regs.ra, CPU::g_state.regs.sp);
      CommitWindow();
    }
    else
    {
      s_entries_uncalled++;
    }
  }
  u32 width = 0;
  bool store = false;
  switch (op)
  {
    case 0x20: case 0x24: width = 1; break;              // lb, lbu
    case 0x21: case 0x25: width = 2; break;              // lh, lhu
    case 0x22: case 0x23: case 0x26: case 0x32: width = 4; break; // lwl, lw, lwr, lwc2
    case 0x28: width = 1; store = true; break;           // sb
    case 0x29: width = 2; store = true; break;           // sh
    case 0x2A: case 0x2B: case 0x2E: case 0x3A: width = 4; store = true; break; // swl, sw, swr, swc2
    default: return;
  }
  const u32 rs = (bits >> 21) & 31u;
  u32 addr = CPU::g_state.regs.r[rs] + static_cast<u32>(static_cast<s32>(static_cast<s16>(bits & 0xFFFFu)));
  if (op == 0x22 || op == 0x26 || op == 0x2A || op == 0x2E)
    addr &= ~3u;
  if (store)
  {
    OnStore(pc, bits, addr, width);
    return;
  }
  if (s_depth != 0 || !s_window_open)
    return;
  u32 off;
  if (!RamAddr(addr, &off))
    return;
  s_window_loads++;
  s_window_reads.try_emplace(off, FirstRead{pc, Frame(), static_cast<u8>(width)});
}

void ExceptionHook(u32 cause_bits, u32 epc)
{
  if (s_prev_exc)
    s_prev_exc(cause_bits, epc);
  s_depth++;
  // The handler starts a fresh instruction history; the interrupted one resumes after its rfe.
  s_hist_saved.push_back(s_hist);
  s_hist = History{};
  if (((cause_bits >> 2) & 0x1Fu) != 0u)
  {
    if (s_depth == 1)
      s_exc_sio = false;
    return; // not an interrupt
  }
  const u32 irqs = InterruptController::ReadRegister(0) & InterruptController::ReadRegister(4);
  if (s_depth == 1)
    s_exc_sio = irqs == 0x80u;
  const u32 pending = irqs & (1u << static_cast<u32>(InterruptController::IRQ::VBLANK));
  if (!pending || (s_window_open && s_window_frame == Frame()))
    return;
  // A new VBLANK: the previous window closes without a card completion.
  s_window_reads.clear();
  s_window_open = true;
  s_window_card = false;
  s_window_frame = Frame();
  s_windows++;
  if (s_window_frame % kWrittenRowFrames == 0)
  {
    const auto [written, written_sio] = CountWritten();
    std::fprintf(s_fp, "W %u %u %u %zu %zu %u\n", s_window_frame, written, written_sio, s_handler_ret.size(),
                 s_handler_ret_max, s_entries_uncalled);
  }
}

} // namespace

bool Initialize(const std::string& path, std::string* error_out)
{
  s_fp = std::fopen(path.c_str(), "w");
  if (!s_fp)
  {
    *error_out = "card-window: cannot open " + path;
    return false;
  }
  std::fprintf(s_fp, "# card-window rows: E OpenEvent, D DeliverEvent, C card command, H card handler entry, "
                     "R main-path load of a card-handler byte inside a VBLANK-to-completion window, "
                     "W written-set size, S store to a card-driver byte (frame tick offset addr width value pc), "
                     "T byte stored to JOY_TX_DATA (frame tick offset byte); offset = ticks since the frame's VBLANK\n");
  s_handler_written.assign(Bus::g_ram_size, 0);
  if (!PcHooks::Add(0xB0u, &OnOpenEvent))
  {
    *error_out = "card-window: cannot hook the B0h vector";
    return false;
  }
  s_prev_insn = CPU::g_instruction_hook;
  s_prev_exc = CPU::g_exception_hook;
  CPU::g_instruction_hook = &InstructionHook;
  CPU::g_exception_hook = &ExceptionHook;
  return true;
}

void Shutdown()
{
  if (!s_fp)
    return;
  u32 hits = 0, hits_handler = 0;
  std::vector<std::pair<u32, Committed>> rows(s_reads.begin(), s_reads.end());
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& [off, c] : rows)
  {
    u8 cat = 0;
    for (u32 i = 0; i < c.width && off + i < s_handler_written.size(); i++)
      cat |= s_handler_written[off + i];
    if (!cat)
      continue;
    hits++;
    hits_handler += (cat & 1u) ? 1u : 0u;
    std::fprintf(s_fp, "R %08x %u %u %08x %u %u\n", 0x80000000u | off, c.width, c.frame, c.pc, c.count, cat);
  }
  const auto [written, written_sio] = CountWritten();
  std::fprintf(s_fp,
               "# summary open_events=%u card_handlers=%zu deliveries=%u card_commands=%u handler_entries=%u "
               "windows=%u windows_with_card_completion=%u window_loads=%llu handler_written_bytes=%u "
               "sio_irq_written_bytes=%u window_read_hits=%u window_read_hits_handler=%u "
               "handler_entries_uncalled=%u active_calls_max=%zu active_calls_end=%zu stores=%u tx_bytes=%u\n",
               s_rows_e, s_handlers.size(), s_rows_d, s_rows_c, s_rows_h, s_windows, s_windows_committed,
               static_cast<unsigned long long>(s_window_loads), written, written_sio, hits, hits_handler,
               s_entries_uncalled, s_handler_ret_max, s_handler_ret.size(), s_rows_s, s_rows_t);
  INFO_LOG("card-window: {} card commands, {} handler entries, {} windows with a card completion, {} hits ({} on "
           "handler bytes)",
           s_rows_c, s_rows_h, s_windows_committed, hits, hits_handler);
  std::fclose(s_fp);
  s_fp = nullptr;
}

} // namespace CardWindow
