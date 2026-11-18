// SPDX-FileCopyrightText: 2026 Tyson Cleary
// SPDX-License-Identifier: MIT

// The openpete options of the regtest binary: the trace and probe
// recorders' arguments, the pad script, the movie player, the motor log,
// the card import and export. regtest_host.cpp calls the five entry points
// declared in openpete_regtest.h.

#include "openpete_regtest.h"

#include "core/analog_controller.h"
#include "core/bus.h"
#include "core/controller.h"
#include "core/core_private.h"
#include "core/cpu_core.h"
#include "core/memory_card.h"
#include "core/openpete_hooks.h"
#include "core/pad.h"
#include "core/settings.h"
#include "core/system.h"

#include "psxtrace_capture.h"
#include "probe_capture.h"

#include "scmversion/scmversion.h"

#include "util/input_manager.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/minizip_helpers.h"
#include "common/path.h"
#include "common/string_util.h"

#include "fmt/format.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

LOG_CHANNEL(Host);

/// Writes the analog controller's private input state the way a movie frame gives it.
struct OpenpetePadAccess
{
  /// Sets the pressed buttons (active low, the pad's own bit order) and the four stick bytes (LX, LY, RX, RY).
  static void SetRawInput(AnalogController* pad, u16 buttons_active_low, const u8 axes[4])
  {
    pad->m_button_state = buttons_active_low;
    for (u32 i = 0; i < static_cast<u32>(AnalogController::Axis::Count); i++)
      pad->m_axis_state[i] = axes[i];
  }
};

// PSX-trace oracle capture (openpete). Off by default; armed via
// -psxtrace PATH. See src/duckstation-regtest/psxtrace_capture.cpp.
static std::string s_psxtrace_path;
static u32 s_psxtrace_anchor_pc = 0x8001ED5Cu; // Spyro 1 GamestateDraw
static u32 s_psxtrace_anchor_hit = 1;
static u32 s_psxtrace_frames = 0; // 0 = bound by -frames
static std::string s_psxtrace_ram_dump_path; // -dump-ram-at-anchor PATH
static std::string s_psxtrace_regions_path;  // -psxtrace-regions PATH (required with -psxtrace)
static u32 s_psxtrace_cd_read_pc = 0;        // -psxtrace-cd-read-pc HEX
static u32 s_psxtrace_cd_read_cb_slot = 0;   // -psxtrace-cd-read-cb-slot HEX
static u32 s_psxtrace_cd_control_pc = 0;     // -psxtrace-cd-control-pc HEX
static std::string s_card_window_path;        // -card-window PATH
static bool s_card1_none = false;              // -card1 none: no memory card in slot 1
static std::string s_card1_image;              // -card1 PATH: slot 1's card loaded from an image
static std::string s_motor_log_path;           // -motor-log PATH
static u32 s_psxtrace_vsync_pc = 0;            // -psxtrace-vsync-pc HEX
static u32 s_psxtrace_srand_pc = 0;            // -psxtrace-srand-pc HEX
static u32 s_psxtrace_rand_pc = 0;             // -psxtrace-rand-pc HEX
static bool s_psxtrace_cd_irq = false;          // -psxtrace-cd-irq
static bool s_psxtrace_pad_cmds = false;        // -psxtrace-pad-cmds
static u32 s_psxtrace_cdsync_pc = 0;            // -psxtrace-cdsync-pc HEX
static u32 s_psxtrace_sio0_addr = 0;            // -psxtrace-sio0-word HEX
static u32 s_psxtrace_card_active = 0;          // -psxtrace-card-active HEX
static u32 s_psxtrace_rcnt2_pc[8];              // -psxtrace-rcnt2-pc HEX (repeatable)
static u32 s_psxtrace_rcnt2_npc = 0;
static u32 s_psxtrace_vsync_skip_lo = 0, s_psxtrace_vsync_skip_hi = 0; // -psxtrace-vsync-query-skip LO:HI
static u32 s_psxtrace_cd_read_pc_min = 0;       // -psxtrace-cd-read-pc-min HEX
static u32 s_psxtrace_vsync_cb_pc[4];           // -psxtrace-vsync-cb-pc HEX (repeatable)
static int s_psxtrace_vsync_cb_n = 0;
static u32 s_psxtrace_watch_addr = 0;          // -psxtrace-watch-word HEX
static u32 s_psxtrace_words_addr = 0, s_psxtrace_words_n = 0; // -psxtrace-words HEX:N
static std::string s_psxtrace_gte_log_path;    // -psxtrace-gte-log PATH
static bool s_psxtrace_gte_hash = false;           // -psxtrace-gte
static bool s_psxtrace_gte_dump = false;           // -psxtrace-gte-dump LO-HI
static u32 s_psxtrace_gte_dump_lo = 0, s_psxtrace_gte_dump_hi = 0;
static u32 s_psxtrace_gte_lo = 0, s_psxtrace_gte_hi = 0; // -psxtrace-gte-vbl LO:HI
static std::string s_psxtrace_vblpos_lib;          // -psxtrace-vblpos LIBRARY_SPANS
static std::string s_psxtrace_vblpos_skip;         // -psxtrace-vblpos-skip SELF_ENTRY_LIST
static u32 s_psxtrace_spuq_pc[5] = {};             // -psxtrace-spuq KIND:HEX (repeatable)
static bool s_psxtrace_spuq = false;
static std::vector<std::pair<u32, u8>> s_psxtrace_spureg_pc; // -psxtrace-spureg KIND:HEX (repeatable)

// -pad-script PATH: scripted presses on the port 1 controller, one step per
// line, advanced once per frame on the core thread so a run reaches a game
// state by its RAM rather than by a frame count (load times then do not
// move the presses). Steps:
//   wait N                                  release all buttons for N frames
//   tap BUTTONS HOLD                        hold BUTTONS for HOLD frames
//   mash BUTTONS HOLD PERIOD until COND     hold BUTTONS for the first HOLD
//                                           frames of every PERIOD until COND
//   until COND                              release all buttons until COND
//   quit                                    shut the system down
// BUTTONS is a '+'-joined list of cross, circle, square, triangle, start,
// select, up, down, left, right, l1, r1, l2, r2. COND is W:HEX OP VALUE,
// W one of b, h, w (byte, halfword, word read of the guest address HEX),
// OP eq or ne, VALUE in C notation; it is tested at the start of each frame.
// '#' starts a comment. Each completed step is logged with its frame.
namespace PadScript {
enum class Op : u8 { Wait, Tap, Mash, Until, Quit };
struct Cond
{
  u32 addr = 0;
  u8 width = 4;
  bool ne = false;
  u32 value = 0;
};
struct Step
{
  Op op = Op::Wait;
  u32 buttons = 0;
  u32 hold = 0;
  u32 period = 1;
  u32 frames = 0;
  Cond cond;
  std::string text;
};
static std::vector<Step> s_steps;
static size_t s_step = 0;
static u32 s_step_frames = 0;
static u32 s_held = 0;

static bool ParseButtons(const std::string& list, u32* out)
{
  static constexpr std::pair<const char*, AnalogController::Button> names[] = {
    {"cross", AnalogController::Button::Cross},     {"circle", AnalogController::Button::Circle},
    {"square", AnalogController::Button::Square},   {"triangle", AnalogController::Button::Triangle},
    {"start", AnalogController::Button::Start},     {"select", AnalogController::Button::Select},
    {"up", AnalogController::Button::Up},           {"down", AnalogController::Button::Down},
    {"left", AnalogController::Button::Left},       {"right", AnalogController::Button::Right},
    {"l1", AnalogController::Button::L1},           {"r1", AnalogController::Button::R1},
    {"l2", AnalogController::Button::L2},           {"r2", AnalogController::Button::R2}};
  *out = 0;
  size_t pos = 0;
  while (pos <= list.size())
  {
    const size_t plus = list.find('+', pos);
    const std::string name = list.substr(pos, plus == std::string::npos ? std::string::npos : plus - pos);
    bool found = false;
    for (const auto& [n, b] : names)
      if (name == n)
      {
        *out |= 1u << static_cast<u32>(b);
        found = true;
      }
    if (!found)
      return false;
    if (plus == std::string::npos)
      break;
    pos = plus + 1;
  }
  return *out != 0;
}

static bool ParseCond(std::istringstream& in, Cond* c)
{
  std::string where, op, value;
  if (!(in >> where >> op >> value) || where.size() < 3 || where[1] != ':')
    return false;
  c->width = where[0] == 'b' ? 1 : where[0] == 'h' ? 2 : where[0] == 'w' ? 4 : 0;
  if (!c->width || (op != "eq" && op != "ne"))
    return false;
  c->addr = static_cast<u32>(std::strtoul(where.c_str() + 2, nullptr, 16));
  c->ne = (op == "ne");
  c->value = static_cast<u32>(std::strtoul(value.c_str(), nullptr, 0));
  return true;
}

static bool Load(const std::string& path)
{
  std::FILE* fp = std::fopen(path.c_str(), "r");
  if (!fp)
  {
    ERROR_LOG("pad-script: cannot open {}", path);
    return false;
  }
  char line[512];
  u32 lineno = 0;
  while (std::fgets(line, sizeof(line), fp))
  {
    lineno++;
    std::string text(line);
    if (const size_t hash = text.find('#'); hash != std::string::npos)
      text.resize(hash);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
      text.pop_back();
    std::istringstream in(text);
    std::string verb;
    if (!(in >> verb))
      continue;
    Step s;
    s.text = text;
    std::string buttons, until;
    bool ok = true;
    if (verb == "wait")
      ok = static_cast<bool>(in >> s.frames);
    else if (verb == "tap")
    {
      s.op = Op::Tap;
      ok = (in >> buttons >> s.hold) && ParseButtons(buttons, &s.buttons);
    }
    else if (verb == "mash")
    {
      s.op = Op::Mash;
      ok = (in >> buttons >> s.hold >> s.period >> until) && until == "until" && s.period > 0 &&
           ParseButtons(buttons, &s.buttons) && ParseCond(in, &s.cond);
    }
    else if (verb == "until")
    {
      s.op = Op::Until;
      ok = ParseCond(in, &s.cond);
    }
    else if (verb == "quit")
      s.op = Op::Quit;
    else
      ok = false;
    if (!ok)
    {
      ERROR_LOG("pad-script: {}:{}: cannot parse '{}'", path, lineno, text);
      std::fclose(fp);
      return false;
    }
    s_steps.push_back(std::move(s));
  }
  std::fclose(fp);
  INFO_LOG("pad-script: {} steps from {}", s_steps.size(), path);
  return !s_steps.empty();
}

static bool CondHolds(const Cond& c)
{
  u32 v = 0;
  const bool read = c.width == 1 ? CPU::SafeReadMemoryByte(c.addr, reinterpret_cast<u8*>(&v)) :
                    c.width == 2 ? CPU::SafeReadMemoryHalfWord(c.addr, reinterpret_cast<u16*>(&v)) :
                                   CPU::SafeReadMemoryWord(c.addr, &v);
  return read && ((v == c.value) != c.ne);
}

static void SetButtons(u32 buttons)
{
  if (buttons == s_held)
    return;
  Controller* pad = System::GetController(0);
  if (!pad)
    return;
  for (u32 b = 0; b < static_cast<u32>(AnalogController::Button::Analog); b++)
    if (((buttons ^ s_held) >> b) & 1u)
      pad->SetBindState(b, ((buttons >> b) & 1u) ? 1.0f : 0.0f);
  s_held = buttons;
}

// Advances the script by one frame; returns true when a quit step ran.
static bool Tick()
{
  while (s_step < s_steps.size())
  {
    const Step& s = s_steps[s_step];
    bool done = false;
    u32 want = 0;
    switch (s.op)
    {
      case Op::Wait: done = s_step_frames >= s.frames; break;
      case Op::Tap:
        done = s_step_frames >= s.hold;
        want = s.buttons;
        break;
      case Op::Mash:
        done = CondHolds(s.cond);
        want = (s_step_frames % s.period) < s.hold ? s.buttons : 0;
        break;
      case Op::Until: done = CondHolds(s.cond); break;
      case Op::Quit:
        INFO_LOG("pad-script: step {} '{}' at frame {}", s_step, s.text, System::GetFrameNumber());
        SetButtons(0);
        s_step = s_steps.size();
        return true;
    }
    if (!done)
    {
      SetButtons(want);
      s_step_frames++;
      return false;
    }
    INFO_LOG("pad-script: step {} '{}' done at frame {} after {} frames", s_step, s.text, System::GetFrameNumber(),
             s_step_frames);
    s_step++;
    s_step_frames = 0;
  }
  SetButtons(0);
  return false;
}
} // namespace PadScript

// -bk2 PATH: a BizHawk Octoshock movie played on the port 1 analog
// controller. Line k of the movie's Input Log.txt (the k-th line starting
// with '|') is the input of frame k: frame 0's input is set before the
// first frame runs and frame k's at the end of frame k-1. Each line is
// |system|P1|...|: the P1 segment holds four comma-terminated stick values
// (LX, LY, RX, RY) and 17 button characters (Up, Down, Left, Right,
// Select, Start, Square, Triangle, Circle, Cross, L1, R1, L2, R2, L3, R3,
// MODE), '.' released; MODE is ignored (the controller stays in the mode
// the game sets). After the last line every button is released and the
// sticks centred.
//
// A movie whose Header.txt carries `OpenpeteFeedKey consume` is keyed on
// (E, j) instead of the frame: E counts the entries of the game's pad-ring
// consumer (-bk2-consume-pc HEX, an Execute hook) and j the inputs set
// since that entry (one per frame end, plus the one before the first
// frame). "Input Groups.txt" holds each group's row count, group 0 first.
// Each input takes row j of group E, holding the group's last row past its
// count; an empty group holds the row before it; past the last group, or
// the last group's count, everything is released. A group's run of inputs
// longer or shorter than the movie's count is counted at the consumer's
// next entry.
//
// -bk2-lead 1 sets each input one frame early: the input set at a frame end
// is the row of the position the next frame end takes, (E, j + 1) inside
// the group's count and (E + 1, 0) past it. The kernel's pad handler
// writes the pad buffer from the previous vsync's exchange, so a row set
// one frame early reaches the game's VSync callbacks at the frame openpete
// delivers it. Where the consumer's next entry falls on another frame than
// the movie's count predicts, the predicted row differs from the row of the
// position actually taken; those frames whose buttons or sticks differ are
// counted.
//
// -bk2-groups-out PATH writes the inputs each group took in this run, one
// count per line, group 0 first; the last line is the group open at exit.
// -bk2-lead-groups PATH reads such a file and the lead predicts from its
// counts instead of the movie's: a run whose consumer enters on the frames
// the file's run did predicts every position. Groups past the file's last
// line predict from the movie's count. The movie's rows and the row each
// position takes are unchanged.
namespace Bk2Play {
struct Frame
{
  u16 buttons = 0xFFFF;
  u8 axes[4] = {0x80, 0x80, 0x80, 0x80};
  s32 raw[4] = {0, 0, 0, 0}; // the stick columns as written, for -bk2-phys
  bool in_movie = false;     // decoded from a movie line; a default Frame stands past the movie's end
};
static std::vector<Frame> s_frames;
static bool s_phys = false; // -bk2-phys: stick columns are SDL axis values sent through the live input path
static u32 s_phys_applied = 0;    // rows delivered through ApplyPhys
static u32 s_phys_off_centre = 0; // of those, rows with a left stick axis off 0
static size_t s_next = 0;
static u32 s_malformed = 0;
static bool s_consume_key = false;            // Header.txt: OpenpeteFeedKey consume
static u32 s_consume_pc = 0;                  // -bk2-consume-pc HEX
static std::vector<u32> s_group_rows;         // rows per group E
static std::vector<size_t> s_group_first;     // first row of group E
static u32 s_e = 0;                           // consumer entries since boot
static u32 s_j = 0;                           // inputs set since the last entry
static u32 s_groups_closed = 0;               // groups inside the movie the consumer closed
static u32 s_groups_mismatched = 0;           // of those, closed after a count other than the movie's
static s64 s_first_mismatch = -1;             // the first such group
static u32 s_first_mismatch_taken = 0;
static u32 s_lead = 0;                        // -bk2-lead: frames each input is set early (0 or 1)
static size_t s_lead_guess = SIZE_MAX;        // the row set at the last frame end for the position after it
static u32 s_lead_misses = 0;                 // frames whose predicted row differed in content from the taken row
static s64 s_lead_first_miss_e = -1;
static u32 s_lead_first_miss_j = 0;
static std::vector<u32> s_taken;               // inputs each closed group took in this run
static std::string s_groups_out;               // -bk2-groups-out PATH
static std::vector<u32> s_lead_groups;         // -bk2-lead-groups: counts the lead predicts from
static bool s_have_lead_groups = false;

static bool DecodeLine(const std::string& line, Frame* out)
{
  // Pad bit per button position, the controller's active-low layout.
  static constexpr u16 masks[16] = {0x0010, 0x0040, 0x0080, 0x0020, 0x0001, 0x0008, 0x8000, 0x1000,
                                    0x2000, 0x4000, 0x0400, 0x0800, 0x0100, 0x0200, 0x0002, 0x0004};
  *out = Frame();
  if (line.size() < 3 || line[0] != '|')
    return false;
  const size_t s1 = line.find('|', 1);
  if (s1 == std::string::npos)
    return false;
  const size_t s2 = line.find('|', s1 + 1);
  if (s2 == std::string::npos)
    return false;
  const std::string seg = line.substr(s1 + 1, s2 - s1 - 1);
  size_t pos = 0;
  for (int a = 0; a < 4; a++)
  {
    const size_t comma = seg.find(',', pos);
    if (comma == std::string::npos)
      return false;
    out->raw[a] = static_cast<s32>(std::strtol(seg.substr(pos, comma - pos).c_str(), nullptr, 10));
    out->axes[a] = static_cast<u8>(out->raw[a]);
    pos = comma + 1;
  }
  if (seg.size() < pos + 16)
    return false;
  for (int i = 0; i < 16; i++)
    if (seg[pos + i] != '.')
      out->buttons &= static_cast<u16>(~masks[i]);
  out->in_movie = true;
  return true;
}

// Reads the zip entry NAME into *out; false when the entry is absent.
static bool ReadEntry(unzFile zf, const char* name, std::string* out)
{
  out->clear();
  if (unzLocateFile(zf, name, 0) != UNZ_OK || unzOpenCurrentFile(zf) != UNZ_OK)
    return false;
  char buf[65536];
  int n;
  while ((n = unzReadCurrentFile(zf, buf, sizeof(buf))) > 0)
    out->append(buf, static_cast<size_t>(n));
  unzCloseCurrentFile(zf);
  return true;
}

static bool Load(const std::string& path)
{
  unzFile zf = MinizipHelpers::OpenUnzFile(path.c_str());
  if (!zf)
  {
    ERROR_LOG("bk2: cannot open {} as zip", path);
    return false;
  }
  std::string text, header, groups;
  if (!ReadEntry(zf, "Input Log.txt", &text))
  {
    ERROR_LOG("bk2: {} has no Input Log.txt", path);
    unzClose(zf);
    return false;
  }
  ReadEntry(zf, "Header.txt", &header);
  s_consume_key = false;
  for (size_t hp = 0; hp < header.size();)
  {
    size_t nl = header.find('\n', hp);
    if (nl == std::string::npos)
      nl = header.size();
    std::string hl = header.substr(hp, nl - hp);
    while (!hl.empty() && (hl.back() == '\r' || hl.back() == ' '))
      hl.pop_back();
    if (hl == "OpenpeteFeedKey consume")
      s_consume_key = true;
    hp = nl + 1;
  }
  if (s_consume_key && !ReadEntry(zf, "Input Groups.txt", &groups))
  {
    ERROR_LOG("bk2: {} is keyed on consume but has no Input Groups.txt", path);
    unzClose(zf);
    return false;
  }
  unzClose(zf);
  size_t pos = 0;
  while (pos < text.size())
  {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos)
      nl = text.size();
    std::string line = text.substr(pos, nl - pos);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.size() >= 2 && line[0] == '|' && line[1] != '[')
    {
      Frame f;
      if (!DecodeLine(line, &f))
        s_malformed++;
      s_frames.push_back(f);
    }
    pos = nl + 1;
  }
  INFO_LOG("bk2: {} frames from {} ({} malformed, played released)", s_frames.size(), path, s_malformed);
  if (s_consume_key)
  {
    size_t first = 0;
    for (size_t gp = 0; gp < groups.size();)
    {
      size_t nl = groups.find('\n', gp);
      if (nl == std::string::npos)
        nl = groups.size();
      std::string gl = groups.substr(gp, nl - gp);
      while (!gl.empty() && (gl.back() == '\r' || gl.back() == ' '))
        gl.pop_back();
      gp = nl + 1;
      if (gl.empty())
        continue;
      char* end = nullptr;
      const unsigned long rows = std::strtoul(gl.c_str(), &end, 10);
      if (!end || *end != '\0')
      {
        ERROR_LOG("bk2: Input Groups.txt line '{}' is not a row count", gl);
        return false;
      }
      s_group_first.push_back(first);
      s_group_rows.push_back(static_cast<u32>(rows));
      first += rows;
    }
    if (first != s_frames.size())
    {
      ERROR_LOG("bk2: Input Groups.txt counts {} rows in {} groups; the Input Log holds {}", first,
                s_group_rows.size(), s_frames.size());
      return false;
    }
    INFO_LOG("bk2: keyed on consume, {} groups", s_group_rows.size());
  }
  return !s_frames.empty();
}

// The row (E, j) takes: s_frames.size() when released.
static size_t GroupRow(u32 g, u32 j)
{
  if (g >= s_group_rows.size())
    return s_frames.size();
  const u32 n = s_group_rows[g];
  if (g + 1 == s_group_rows.size() && j >= n)
    return s_frames.size();
  if (n == 0)
    return s_group_first[g] > 0 ? s_group_first[g] - 1 : 0;
  return s_group_first[g] + (j < n ? j : n - 1);
}

// Reads -bk2-lead-groups PATH: one inputs count per line, group 0 first.
static bool LoadLeadGroups(const char* path)
{
  std::FILE* fp = std::fopen(path, "r");
  if (!fp)
  {
    ERROR_LOG("bk2: cannot open lead groups {}", path);
    return false;
  }
  char line[64];
  while (std::fgets(line, sizeof(line), fp))
  {
    char* end = nullptr;
    const unsigned long n = std::strtoul(line, &end, 10);
    if (end == line)
    {
      ERROR_LOG("bk2: lead groups line '{}' is not a count", line);
      std::fclose(fp);
      return false;
    }
    s_lead_groups.push_back(static_cast<u32>(n));
  }
  std::fclose(fp);
  s_have_lead_groups = true;
  INFO_LOG("bk2: lead predicts from {} groups of {}", s_lead_groups.size(), path);
  return true;
}

// The inputs group g is predicted to take: the lead groups' count, else the movie's.
static u32 PredictedCount(u32 g)
{
  if (g < s_lead_groups.size())
    return s_lead_groups[g];
  return g < s_group_rows.size() ? s_group_rows[g] : 0u;
}

// The first group from g on that the lead predicts takes an input: a group
// predicted to take none closes before the next input is set, so the input
// after group E's last is the first row of the next group that takes one.
// Only counts a previous run took (-bk2-lead-groups) say a group takes
// none; the movie's own counts keep the group.
static u32 NextTakingGroup(u32 g)
{
  if (!s_have_lead_groups)
    return g;
  const size_t end = std::max(s_lead_groups.size(), s_group_rows.size());
  while (g < end && PredictedCount(g) == 0)
    g++;
  return g;
}

// Execute hook at the consumer's entry: closes group E and opens E + 1.
static bool OnConsume(u32 pc)
{
  (void)pc;
  s_taken.push_back(s_j);
  if (s_e < s_group_rows.size())
  {
    s_groups_closed++;
    if (s_j != s_group_rows[s_e])
    {
      if (s_groups_mismatched++ == 0)
      {
        s_first_mismatch = s_e;
        s_first_mismatch_taken = s_j;
      }
    }
  }
  s_e++;
  s_j = 0;
  return true;
}

// Checks the key against -bk2-consume-pc; installs the consumer hook once the system has booted.
static bool Install(std::string* error_out)
{
  if (s_frames.empty())
    return true;
  if (s_consume_key != (s_consume_pc != 0))
  {
    *error_out = s_consume_key ? "bk2: the movie is keyed on consume; -bk2-consume-pc HEX is required" :
                                 "bk2: -bk2-consume-pc needs a movie keyed on consume";
    return false;
  }
  if (s_lead && !s_consume_key)
  {
    *error_out = "bk2: -bk2-lead needs a movie keyed on consume";
    return false;
  }
  if ((s_have_lead_groups || !s_groups_out.empty()) && !s_consume_key)
  {
    *error_out = "bk2: -bk2-lead-groups and -bk2-groups-out need a movie keyed on consume";
    return false;
  }
  if (s_consume_key && !PcHooks::Add(s_consume_pc, &OnConsume))
  {
    *error_out = fmt::format("bk2: cannot hook the consumer at 0x{:08X}", s_consume_pc);
    return false;
  }
  return true;
}

// Logs the consume key's counters: entries, and groups closed off the movie's count.
static void Report()
{
  if (s_phys)
    INFO_LOG("bk2: phys rows applied={} with the left stick off centre={}", s_phys_applied, s_phys_off_centre);
  if (!s_consume_key)
    return;
  INFO_LOG("bk2: consume summary: entries={} groups={} closed={} mismatched={} first_mismatch={} "
           "(movie rows {}, taken {}) lead={} lead_misses={} first_lead_miss={}:{}",
           s_e, s_group_rows.size(), s_groups_closed, s_groups_mismatched, s_first_mismatch,
           s_first_mismatch >= 0 ? s_group_rows[static_cast<size_t>(s_first_mismatch)] : 0u,
           s_first_mismatch_taken, s_lead, s_lead_misses, s_lead_first_miss_e, s_lead_first_miss_j);
  if (s_have_lead_groups)
  {
    // Groups whose count the lead predicted from differs from the count this run took.
    u32 differ = 0;
    s64 first = -1;
    for (size_t g = 0; g < s_taken.size() && g < s_lead_groups.size(); g++)
    {
      if (s_taken[g] != s_lead_groups[g] && differ++ == 0)
        first = static_cast<s64>(g);
    }
    INFO_LOG("bk2: lead groups: predicted={} compared={} differ={} first_differ={}", s_lead_groups.size(),
             std::min(s_taken.size(), s_lead_groups.size()), differ, first);
  }
  if (!s_groups_out.empty())
  {
    std::FILE* fp = std::fopen(s_groups_out.c_str(), "w");
    if (!fp)
    {
      ERROR_LOG("bk2: cannot write groups {}", s_groups_out);
      return;
    }
    for (const u32 n : s_taken)
      std::fprintf(fp, "%u\n", n);
    std::fprintf(fp, "%u\n", s_j);
    std::fclose(fp);
    INFO_LOG("bk2: groups taken: {} closed and the open group -> {}", s_taken.size(), s_groups_out);
  }
}

// -bk2-phys: delivers a row the way the live input path delivers a host
// pad's state. Each stick column is an SDL axis value (-32768..32767). It is
// scaled to v / 32768 when negative and v / 32767 otherwise; the positive
// part drives the "+" half-axis binding and the negated negative part the
// "-" one (0 to the other), each clamped to [0, 1] (binding scale 1, dead
// zone 0), and the controller merges the two halves. The default bindings:
// +LeftX LRight, -LeftX LLeft, +LeftY LDown, -LeftY LUp, the same for the
// right stick. Buttons are bound at 1 (pressed) or 0. A row past the movie's
// end holds every stick at 0 and releases every button.
static void ApplyPhys(Controller* pad, const Frame& f)
{
  s_phys_applied++;
  static constexpr AnalogController::HalfAxis pos[4] = {
    AnalogController::HalfAxis::LRight, AnalogController::HalfAxis::LDown, AnalogController::HalfAxis::RRight,
    AnalogController::HalfAxis::RDown};
  static constexpr AnalogController::HalfAxis neg[4] = {
    AnalogController::HalfAxis::LLeft, AnalogController::HalfAxis::LUp, AnalogController::HalfAxis::RLeft,
    AnalogController::HalfAxis::RUp};
  const bool past_end = !f.in_movie;
  for (u32 a = 0; a < 4; a++)
  {
    const s32 v = past_end ? 0 : std::clamp<s32>(f.raw[a], -32768, 32767);
    const float value = static_cast<float>(v) / (v < 0 ? 32768.0f : 32767.0f);
    const float p = std::clamp(value > 0.0f ? value : 0.0f, 0.0f, 1.0f);
    const float n = std::clamp(value < 0.0f ? -value : 0.0f, 0.0f, 1.0f);
    pad->SetBindState(static_cast<u32>(AnalogController::Button::Count) + static_cast<u32>(pos[a]), p);
    pad->SetBindState(static_cast<u32>(AnalogController::Button::Count) + static_cast<u32>(neg[a]), n);
  }
  for (u32 b = 0; b < 16; b++)
    pad->SetBindState(b, ((f.buttons >> b) & 1u) ? 0.0f : 1.0f);
  if (!past_end && (f.raw[0] != 0 || f.raw[1] != 0))
    s_phys_off_centre++;
}

// Sets the next input on the port 1 controller: the next frame's row, or
// under the consume key row j of group E.
static void Apply()
{
  Controller* pad = System::GetController(0);
  if (!pad || pad->GetType() != ControllerType::AnalogController)
    return;
  size_t row = s_consume_key ? GroupRow(s_e, s_j) : s_next;
  if (s_consume_key && s_lead)
  {
    // The row set at the previous frame end stood for this position.
    if (s_lead_guess != SIZE_MAX)
    {
      const Frame g = s_lead_guess < s_frames.size() ? s_frames[s_lead_guess] : Frame();
      const Frame a = row < s_frames.size() ? s_frames[row] : Frame();
      if (g.buttons != a.buttons || std::memcmp(g.axes, a.axes, sizeof(g.axes)) != 0 ||
          (s_phys && std::memcmp(g.raw, a.raw, sizeof(g.raw)) != 0))
      {
        if (s_lead_misses++ == 0)
        {
          s_lead_first_miss_e = s_e;
          s_lead_first_miss_j = s_j;
        }
      }
    }
    const u32 n = PredictedCount(s_e);
    row = (s_j + 1 < n) ? GroupRow(s_e, s_j + 1) : GroupRow(NextTakingGroup(s_e + 1), 0);
    s_lead_guess = row;
  }
  if (s_consume_key)
    s_j++;
  const Frame f = row < s_frames.size() ? s_frames[row] : Frame();
  if (s_phys)
    ApplyPhys(pad, f);
  else
    OpenpetePadAccess::SetRawInput(static_cast<AnalogController*>(pad), f.buttons, f.axes);
  if (row >= s_frames.size() && s_next <= s_frames.size())
  {
    INFO_LOG("bk2: movie ended at frame {}", System::GetFrameNumber());
    s_next = s_frames.size() + 1;
  }
  else if (!s_consume_key)
    s_next++;
}
} // namespace Bk2Play

// -motor-log PATH: one row per change of an analog controller's motor state
// (OpenpetePad::g_motor_hook): the frame, the controller's index, the
// motor (0 small, 1 large), the new state byte, the other motor's state byte
// at that moment, and the intensity the controller hands
// InputManager::SetPadVibrationIntensity, as its IEEE-754 single bits and in
// decimal. The other motor's state is tracked from earlier rows; both start
// at 0, the controller's reset state.
namespace MotorLog {
static std::FILE* s_fp = nullptr;
static u8 s_state[2][2] = {};
static u32 s_rows = 0;

static void OnMotor(u32 index, u32 motor, u8 value, float intensity)
{
  if (index >= 2 || motor >= 2)
    return;
  s_state[index][motor] = value;
  u32 bits;
  std::memcpy(&bits, &intensity, sizeof(bits));
  std::fprintf(s_fp, "%u %u %u %u %u %08X %.9g\n", System::GetFrameNumber(), index, motor, value,
               s_state[index][motor ^ 1u], bits, static_cast<double>(intensity));
  s_rows++;
}

static bool Open(const std::string& path)
{
  s_fp = std::fopen(path.c_str(), "w");
  if (!s_fp)
    return false;
  std::fprintf(s_fp, "motor-log 1 build=%s\n# frame pad motor value other bits intensity\n", g_scm_hash_str);
  OpenpetePad::g_motor_hook = OnMotor;
  return true;
}

static void Shutdown()
{
  if (!s_fp)
    return;
  OpenpetePad::g_motor_hook = nullptr;
  std::fprintf(s_fp, "# rows=%u\n", s_rows);
  std::fclose(s_fp);
  s_fp = nullptr;
  INFO_LOG("motor-log: {} rows", s_rows);
}
} // namespace MotorLog

// -card-export SLOT:FRAME:PATH: the card in SLOT (1 or 2) written to PATH
// as its raw 131072 bytes once FRAME frames have run (0: after boot,
// before the first frame; `end`: at shutdown). Repeatable.
namespace CardExport {
struct Spec
{
  u32 slot = 0;
  u32 frame = 0;
  bool at_end = false;
  bool done = false;
  std::string path;
};
static std::vector<Spec> s_specs;

static bool Parse(const char* arg)
{
  const std::string v(arg);
  const size_t c1 = v.find(':');
  const size_t c2 = c1 == std::string::npos ? std::string::npos : v.find(':', c1 + 1);
  if (c2 == std::string::npos || c2 + 1 >= v.size())
    return false;
  Spec s;
  s.slot = static_cast<u32>(std::strtoul(v.substr(0, c1).c_str(), nullptr, 10));
  const std::string f = v.substr(c1 + 1, c2 - c1 - 1);
  s.at_end = (f == "end");
  s.frame = s.at_end ? 0u : static_cast<u32>(std::strtoul(f.c_str(), nullptr, 10));
  s.path = v.substr(c2 + 1);
  if (s.slot < 1 || s.slot > 2)
    return false;
  s_specs.push_back(std::move(s));
  return true;
}

static void Write(Spec& s)
{
  s.done = true;
  MemoryCard* mc = Pad::GetMemoryCard(s.slot - 1);
  if (!mc)
  {
    ERROR_LOG("card-export: slot {} holds no card; {} not written", s.slot, s.path);
    return;
  }
  const MemoryCardImage::DataArray& data = mc->GetData();
  std::FILE* fp = std::fopen(s.path.c_str(), "wb");
  if (!fp || std::fwrite(data.data(), 1, data.size(), fp) != data.size())
  {
    ERROR_LOG("card-export: cannot write {}", s.path);
    if (fp)
      std::fclose(fp);
    return;
  }
  std::fclose(fp);
  INFO_LOG("card-export: slot {} -> {} at frame {}", s.slot, s.path, System::GetFrameNumber());
}

// Writes every spec due at the current frame (at_end: only when `end`).
static void Poll(bool end)
{
  for (Spec& s : s_specs)
    if (!s.done && (end ? s.at_end : (!s.at_end && System::GetFrameNumber() >= s.frame)))
      Write(s);
}
} // namespace CardExport

void OpenpeteRegtest::PrintHelp()
{
  std::fprintf(stderr, "  -psxtrace PATH: Capture per-frame .psxtrace v2 oracle records to PATH.\n");
  std::fprintf(stderr, "  -psxtrace-anchor-pc HEX: PSX vaddr where each frame fires (default 0x8001ED5C).\n");
  std::fprintf(stderr, "  -psxtrace-anchor-hit N: Skip first N-1 hits before recording (default 1).\n");
  std::fprintf(stderr, "  -psxtrace-frames N: Stop after capturing N records (default: bound by -frames).\n");
  std::fprintf(stderr, "  -psxtrace-regions PATH: Region table to hash (gen_state_regions.py --out-flat). Required with -psxtrace.\n");
  std::fprintf(stderr, "  -psxtrace-cd-read-pc HEX: Record every CdRead (entry PC) to <trace>.loads.\n");
  std::fprintf(stderr, "  -psxtrace-cd-read-cb-slot HEX: Guest word holding the game's read-complete handler; stamps completions.\n");
  std::fprintf(stderr, "  -psxtrace-cd-control-pc HEX: Log every CdControl command to <trace>.cdctl.\n");
  std::fprintf(stderr, "  -psxtrace-vsync-pc HEX: Log every waited VSync call (mode >= 0: VBLANK at entry and return, value) to <trace>.vsync.\n");
  std::fprintf(stderr, "  -psxtrace-srand-pc HEX: Log every srand seed to <trace>.srand.\n");
  std::fprintf(stderr, "  -psxtrace-rand-pc HEX: Log every rand return value to <trace>.rand.\n");
  std::fprintf(stderr, "  -psxtrace-cd-irq: Log every CD-ROM controller command and interrupt to <trace>.cdirq.\n");
  std::fprintf(stderr, "  -psxtrace-pad-cmds: Log every command byte the analog controllers accept to <trace>.padcmd.\n");
  std::fprintf(stderr, "  -psxtrace-cdsync-pc HEX: Log every CdSync call (mode, return value) to <trace>.cdsync.\n");
  std::fprintf(stderr, "  -psxtrace-cd-read-pc-min HEX: With -psxtrace-cd-irq, also log every controller register read by an instruction at or above HEX.\n");
  std::fprintf(stderr, "  -psxtrace-vsync-cb-pc HEX: Log every call of a callback registrar as a mode -2 row of <trace>.vsync (up to 4).\n");
  std::fprintf(stderr, "  -psxtrace-watch-word HEX: Log every store to one guest word to <trace>.watch.\n");
  std::fprintf(stderr, "  -psxtrace-words HEX:N: Write the N guest words from HEX at every record to <trace>.words.\n");
  std::fprintf(stderr, "  -pad-script PATH: Drive the port 1 controller from a script of RAM-conditioned presses.\n");
  std::fprintf(stderr, "  -motor-log PATH: Log every change of an analog controller's motor state and the intensity sent to the host.\n");
  std::fprintf(stderr, "  -psxtrace-vsync-query-skip LO:HI: Do not record VSync(-1) calls returning to [LO, HI) (a library's own timeout polls).\n");
  std::fprintf(stderr, "  -psxtrace-sio0-word HEX: Log the vblanks the kernel's SIO0 handler served (its stores to this word) to <trace>.sio0.\n");
  std::fprintf(stderr, "  -psxtrace-rcnt2-pc HEX: Log every read of root counter 2's current-count register by the instruction at HEX (repeatable, up to 8) with its record, VBLANK and value to <trace>.rcnt2.\n");
  std::fprintf(stderr, "  -psxtrace-card-active HEX: Log each card transfer's start (a nonzero store to the driver's transfer-active byte at HEX) with its tick offset from the VBLANK to <trace>.card.\n");
  std::fprintf(stderr, "  -psxtrace-gte-log PATH: Log every GTE instruction (interpreter only) to a CSV.\n");
  std::fprintf(stderr, "  -psxtrace-gte-vbl LO:HI: VBLANK window of the GTE log.\n");
  std::fprintf(stderr, "  -psxtrace-gte: Fold the GTE instructions between records into <trace>.gte, one hash per record.\n");
  std::fprintf(stderr, "  -psxtrace-gte-dump LO-HI: With -psxtrace-gte, write every GTE instruction of records LO..HI to <trace>.gtedump.\n");
  std::fprintf(stderr, "  -psxtrace-vblpos PATH: Write every VBLANK interrupt taken outside a waited VSync to <trace>.vblpos; PATH lists the library spans that are not game code.\n");
  std::fprintf(stderr, "  -psxtrace-vblpos-skip PATH: Functions -psxtrace-vblpos never chooses as the call target (<overlay> 0x<addr> per line).\n");
  std::fprintf(stderr, "  -psxtrace-spuq KIND:HEX: Log every call of the libspu entry HEX to <trace>.spuq; KIND setkey|keystatus|allkeys|voiceattr|spuwrite (repeatable; spuwrite rows add the sounding voices' heads).\n");
  std::fprintf(stderr, "  -psxtrace-spureg KIND:HEX: Log every execution of the SPU register access at HEX to <trace>.spureg; KIND kon|pitch|envx (repeatable).\n");
  std::fprintf(stderr, "  -dump-ram-at-anchor PATH: Write 2MB Bus::g_ram to PATH on first armed anchor hit.\n");
  std::fprintf(stderr, "  -ram-dump-at PC[,t1=HEX][,a0=HEX][,hit=N][,ret]=PATH: Dump main RAM and registers at the Nth execution of PC (repeatable).\n");
  std::fprintf(stderr, "  -bk2 PATH: Play a BizHawk Octoshock movie on the port 1 analog controller, one Input Log line per frame.\n");
  std::fprintf(stderr, "  -bk2-phys: The -bk2 movie's stick columns are SDL axis values, delivered through the live input path's mapping.\n");
  std::fprintf(stderr, "  -bk2-consume-pc HEX: The game's pad-ring consumer, whose entries key a movie with OpenpeteFeedKey consume.\n");
  std::fprintf(stderr, "  -bk2-lead 0|1: Set each consume-keyed input this many frames early.\n");
  std::fprintf(stderr, "  -bk2-groups-out PATH: Write the inputs each consume group took, one count per line.\n");
  std::fprintf(stderr, "  -bk2-lead-groups PATH: Predict the lead from these group counts instead of the movie's.\n");
  std::fprintf(stderr, "  -card-export SLOT:FRAME:PATH: Write the card in SLOT (1|2) as 131072 raw bytes after FRAME frames (0: before the first; end: at shutdown).\n");
  std::fprintf(stderr, "  -card-window PATH: Log card commands, card event handler entries and main-path loads of handler-written bytes between a VBLANK and the card completion.\n");
  std::fprintf(stderr, "  -card1 none|nonpersistent|PATH: Slot 1 empty, a formatted non-persistent card (default), or a non-persistent card holding the image at PATH.\n");
}

int OpenpeteRegtest::ParseArgument(int argc, char* argv[], int& i)
{
// OP_FLAG: argv[i] is the option; OP_OPTION: argv[i] is the option and a value follows it.
#define OP_FLAG(name) (std::strcmp(argv[i], (name)) == 0)
#define OP_OPTION(name) (OP_FLAG(name) && i + 1 < argc)

  if (OP_OPTION("-psxtrace"))
  {
    s_psxtrace_path = argv[++i];
    return 1;
  }
  else if (OP_OPTION("-psxtrace-anchor-pc"))
  {
    const char* hex = argv[++i];
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X'))
      hex += 2;
    s_psxtrace_anchor_pc = static_cast<u32>(std::strtoul(hex, nullptr, 16));
    return 1;
  }
  else if (OP_OPTION("-psxtrace-anchor-hit"))
  {
    s_psxtrace_anchor_hit = StringUtil::FromChars<u32>(argv[++i]).value_or(1);
    return 1;
  }
  else if (OP_OPTION("-psxtrace-frames"))
  {
    s_psxtrace_frames = StringUtil::FromChars<u32>(argv[++i]).value_or(0);
    return 1;
  }
  else if (OP_OPTION("-psxtrace-regions"))
  {
    s_psxtrace_regions_path = argv[++i];
    return 1;
  }
  else if (OP_OPTION("-psxtrace-cd-read-pc"))
  {
    s_psxtrace_cd_read_pc = static_cast<u32>(std::strtoul(argv[++i], nullptr, 16));
    return 1;
  }
  else if (OP_OPTION("-psxtrace-cd-read-cb-slot"))
  {
    s_psxtrace_cd_read_cb_slot = static_cast<u32>(std::strtoul(argv[++i], nullptr, 16));
    return 1;
  }
  else if (OP_OPTION("-psxtrace-cd-control-pc"))
  {
    s_psxtrace_cd_control_pc = static_cast<u32>(std::strtoul(argv[++i], nullptr, 16));
    return 1;
  }
  else if (OP_OPTION("-psxtrace-vsync-pc"))
  {
    s_psxtrace_vsync_pc = static_cast<u32>(std::strtoul(argv[++i], nullptr, 16));
    return 1;
  }
  else if (OP_OPTION("-psxtrace-srand-pc"))
  {
    s_psxtrace_srand_pc = static_cast<u32>(std::strtoul(argv[++i], nullptr, 16));
    return 1;
  }
  else if (OP_OPTION("-psxtrace-vsync-cb-pc"))
  {
    if (s_psxtrace_vsync_cb_n < 4)
      s_psxtrace_vsync_cb_pc[s_psxtrace_vsync_cb_n++] = static_cast<u32>(std::strtoul(argv[++i], nullptr, 16));
    else
      ++i;
    return 1;
  }
  else if (OP_OPTION("-psxtrace-cd-read-pc-min"))
  {
    s_psxtrace_cd_read_pc_min = static_cast<u32>(std::strtoul(argv[++i], nullptr, 16));
    return 1;
  }
  else if (OP_FLAG("-psxtrace-cd-irq"))
  {
    s_psxtrace_cd_irq = true;
    return 1;
  }
  else if (OP_FLAG("-psxtrace-pad-cmds"))
  {
    s_psxtrace_pad_cmds = true;
    return 1;
  }
  else if (OP_OPTION("-psxtrace-cdsync-pc"))
  {
    s_psxtrace_cdsync_pc = static_cast<u32>(std::strtoul(argv[++i], nullptr, 16));
    return 1;
  }
  else if (OP_OPTION("-psxtrace-rand-pc"))
  {
    s_psxtrace_rand_pc = static_cast<u32>(std::strtoul(argv[++i], nullptr, 16));
    return 1;
  }
  else if (OP_OPTION("-psxtrace-vsync-query-skip"))
  {
    const char* v = argv[++i];
    s_psxtrace_vsync_skip_lo = static_cast<u32>(std::strtoul(v, nullptr, 16));
    const char* colon = std::strchr(v, ':');
    s_psxtrace_vsync_skip_hi = colon ? static_cast<u32>(std::strtoul(colon + 1, nullptr, 16)) : 0u;
    return 1;
  }
  else if (OP_OPTION("-psxtrace-rcnt2-pc"))
  {
    if (s_psxtrace_rcnt2_npc >= 8)
    {
      ERROR_LOG("-psxtrace-rcnt2-pc: at most 8");
      return -1;
    }
    s_psxtrace_rcnt2_pc[s_psxtrace_rcnt2_npc++] = static_cast<u32>(std::strtoul(argv[++i], nullptr, 16));
    return 1;
  }
  else if (OP_OPTION("-psxtrace-card-active"))
  {
    s_psxtrace_card_active = static_cast<u32>(std::strtoul(argv[++i], nullptr, 16));
    return 1;
  }
  else if (OP_OPTION("-psxtrace-sio0-word"))
  {
    s_psxtrace_sio0_addr = static_cast<u32>(std::strtoul(argv[++i], nullptr, 16));
    return 1;
  }
  else if (OP_OPTION("-pad-script"))
  {
    if (!PadScript::Load(argv[++i]))
      return -1;
    return 1;
  }
  else if (OP_OPTION("-psxtrace-words"))
  {
    char* end = nullptr;
    s_psxtrace_words_addr = static_cast<u32>(std::strtoul(argv[++i], &end, 16));
    if (!end || *end != ':')
    {
      ERROR_LOG("-psxtrace-words: expected HEX:N");
      return -1;
    }
    s_psxtrace_words_n = static_cast<u32>(std::strtoul(end + 1, nullptr, 10));
    return 1;
  }
  else if (OP_OPTION("-motor-log"))
  {
    s_motor_log_path = argv[++i];
    return 1;
  }
  else if (OP_OPTION("-psxtrace-watch-word"))
  {
    s_psxtrace_watch_addr = static_cast<u32>(std::strtoul(argv[++i], nullptr, 16));
    return 1;
  }
  else if (OP_OPTION("-psxtrace-gte-log"))
  {
    s_psxtrace_gte_log_path = argv[++i];
    return 1;
  }
  else if (OP_FLAG("-psxtrace-gte"))
  {
    s_psxtrace_gte_hash = true;
    return 1;
  }
  else if (OP_OPTION("-psxtrace-gte-dump"))
  {
    const char* spec = argv[++i];
    char* end = nullptr;
    s_psxtrace_gte_dump_lo = static_cast<u32>(std::strtoul(spec, &end, 10));
    if (!end || *end != '-')
    {
      ERROR_LOG("-psxtrace-gte-dump wants LO-HI (record indices), got {}", spec);
      return -1;
    }
    s_psxtrace_gte_dump_hi = static_cast<u32>(std::strtoul(end + 1, nullptr, 10));
    s_psxtrace_gte_dump = true;
    return 1;
  }
  else if (OP_OPTION("-psxtrace-vblpos"))
  {
    s_psxtrace_vblpos_lib = argv[++i];
    return 1;
  }
  else if (OP_OPTION("-psxtrace-vblpos-skip"))
  {
    s_psxtrace_vblpos_skip = argv[++i];
    return 1;
  }
  else if (OP_OPTION("-psxtrace-spuq"))
  {
    const std::string spec = argv[++i];
    static constexpr const char* kinds[5] = {"setkey", "keystatus", "allkeys", "voiceattr", "spuwrite"};
    const size_t colon = spec.find(':');
    int kind = -1;
    for (int k = 0; k < 5 && colon != std::string::npos; k++)
      if (spec.compare(0, colon, kinds[k]) == 0)
        kind = k;
    if (kind < 0)
    {
      ERROR_LOG("-psxtrace-spuq wants KIND:HEX with KIND setkey|keystatus|allkeys|voiceattr|spuwrite, got {}", spec);
      return -1;
    }
    s_psxtrace_spuq_pc[kind] = static_cast<u32>(std::strtoul(spec.c_str() + colon + 1, nullptr, 16));
    s_psxtrace_spuq = true;
    return 1;
  }
  else if (OP_OPTION("-psxtrace-spureg"))
  {
    const std::string spec = argv[++i];
    static constexpr const char* kinds[3] = {"kon", "pitch", "envx"};
    const size_t colon = spec.find(':');
    int kind = -1;
    for (int k = 0; k < 3 && colon != std::string::npos; k++)
      if (spec.compare(0, colon, kinds[k]) == 0)
        kind = k;
    if (kind < 0)
    {
      ERROR_LOG("-psxtrace-spureg wants KIND:HEX with KIND kon|pitch|envx, got {}", spec);
      return -1;
    }
    s_psxtrace_spureg_pc.emplace_back(static_cast<u32>(std::strtoul(spec.c_str() + colon + 1, nullptr, 16)),
                                      static_cast<u8>(kind));
    return 1;
  }
  else if (OP_OPTION("-psxtrace-gte-vbl"))
  {
    const char* spec = argv[++i];
    char* end = nullptr;
    s_psxtrace_gte_lo = static_cast<u32>(std::strtoul(spec, &end, 10));
    s_psxtrace_gte_hi = (end && *end == ':') ? static_cast<u32>(std::strtoul(end + 1, nullptr, 10)) : 0;
    return 1;
  }
  else if (OP_OPTION("-ram-dump-at"))
  {
    std::string err;
    if (!RamDump::AddSpec(argv[++i], &err))
    {
      ERROR_LOG("{}", err);
      return -1;
    }
    return 1;
  }
  else if (OP_OPTION("-bk2"))
  {
    if (!Bk2Play::Load(argv[++i]))
      return -1;
    return 1;
  }
  else if (OP_FLAG("-bk2-phys"))
  {
    Bk2Play::s_phys = true;
    return 1;
  }
  else if (OP_OPTION("-bk2-consume-pc"))
  {
    Bk2Play::s_consume_pc = static_cast<u32>(std::strtoul(argv[++i], nullptr, 16));
    return 1;
  }
  else if (OP_OPTION("-bk2-lead"))
  {
    Bk2Play::s_lead = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
    if (Bk2Play::s_lead > 1)
    {
      ERROR_LOG("Invalid -bk2-lead value: {} (0 or 1)", argv[i]);
      return -1;
    }
    return 1;
  }
  else if (OP_OPTION("-bk2-lead-groups"))
  {
    if (!Bk2Play::LoadLeadGroups(argv[++i]))
      return -1;
    return 1;
  }
  else if (OP_OPTION("-bk2-groups-out"))
  {
    Bk2Play::s_groups_out = argv[++i];
    return 1;
  }
  else if (OP_OPTION("-card-export"))
  {
    if (!CardExport::Parse(argv[++i]))
    {
      ERROR_LOG("Invalid -card-export value: {} (SLOT:FRAME:PATH, SLOT 1|2, FRAME a number or end)", argv[i]);
      return -1;
    }
    return 1;
  }
  else if (OP_OPTION("-card-window"))
  {
    s_card_window_path = argv[++i];
    return 1;
  }
  else if (OP_OPTION("-dump-ram-at-anchor"))
  {
    s_psxtrace_ram_dump_path = argv[++i];
    return 1;
  }
  else if (OP_OPTION("-card1"))
  {
    const char* v = argv[++i];
    s_card1_none = (std::strcmp(v, "none") == 0);
    if (!s_card1_none && std::strcmp(v, "nonpersistent") != 0)
    {
      if (!FileSystem::FileExists(v))
      {
        ERROR_LOG("Invalid -card1 value: {} (none|nonpersistent|an existing card image)", v);
        return -1;
      }
      s_card1_image = v;
    }
    // The base settings layer is built before the arguments are parsed,
    // so the card type is rewritten here rather than read at init.
    {
      const auto lock = Core::GetSettingsLock();
      Core::GetBaseSettingsLayer()->SetStringValue(
        "MemoryCards", "Card1Type",
        Settings::GetMemoryCardTypeName(s_card1_none ? MemoryCardType::None
                                                     : MemoryCardType::NonPersistent));
    }
    INFO_LOG("regtest: memory card slot 1: {}",
             s_card1_none ? "none" : (s_card1_image.empty() ? "non-persistent" : s_card1_image.c_str()));
    return 1;
  }

#undef OP_FLAG
#undef OP_OPTION
  return 0;
}

bool OpenpeteRegtest::Start()
{
  if (!s_psxtrace_path.empty())
  {
    if (s_psxtrace_regions_path.empty())
    {
      ERROR_LOG("psxtrace init failed: -psxtrace-regions PATH is required with -psxtrace");
      return false;
    }
    std::string trace_err;
    if (!PSXTraceCapture::Initialize(s_psxtrace_path, s_psxtrace_anchor_pc,
                                     s_psxtrace_anchor_hit, s_psxtrace_frames,
                                     s_psxtrace_ram_dump_path, s_psxtrace_regions_path,
                                     s_psxtrace_cd_read_pc, s_psxtrace_cd_read_cb_slot,
                                     s_psxtrace_cd_control_pc,
                                     s_psxtrace_vsync_pc, s_psxtrace_srand_pc,
                                     s_psxtrace_rand_pc, s_psxtrace_watch_addr, s_psxtrace_cd_irq,
                                     s_psxtrace_cd_read_pc_min, s_psxtrace_vsync_cb_pc, s_psxtrace_vsync_cb_n,
                                     s_psxtrace_pad_cmds, s_psxtrace_cdsync_pc, s_psxtrace_sio0_addr,
                                     s_psxtrace_vsync_skip_lo, s_psxtrace_vsync_skip_hi, &trace_err))
    {
      ERROR_LOG("psxtrace init failed: {}", trace_err);
      return false;
    }
  }

  if (s_psxtrace_gte_hash || s_psxtrace_gte_dump)
  {
    std::string gte_err;
    if (s_psxtrace_path.empty() || !s_psxtrace_gte_hash)
    {
      ERROR_LOG("psxtrace gte hash: -psxtrace-gte needs -psxtrace, -psxtrace-gte-dump needs -psxtrace-gte");
      return false;
    }
    if (!PSXTraceCapture::InitializeGTEHash(s_psxtrace_path, s_psxtrace_gte_dump, s_psxtrace_gte_dump_lo,
                                            s_psxtrace_gte_dump_hi, &gte_err))
    {
      ERROR_LOG("psxtrace gte hash init failed: {}", gte_err);
      return false;
    }
  }

  if (!s_psxtrace_vblpos_lib.empty())
  {
    std::string err;
    if (!PSXTraceCapture::InitializeVblPos(s_psxtrace_path, s_psxtrace_vblpos_lib, s_psxtrace_vblpos_skip, &err))
    {
      ERROR_LOG("psxtrace vblpos init failed: {}", err);
      return false;
    }
  }

  if (s_psxtrace_words_n)
  {
    std::string err;
    if (!PSXTraceCapture::InitializeWords(s_psxtrace_path, s_psxtrace_words_addr, s_psxtrace_words_n, &err))
    {
      ERROR_LOG("psxtrace words init failed: {}", err);
      return false;
    }
  }

  if (s_psxtrace_rcnt2_npc)
  {
    std::string err;
    if (!PSXTraceCapture::InitializeRcnt2(s_psxtrace_path, s_psxtrace_rcnt2_pc, s_psxtrace_rcnt2_npc, &err))
    {
      ERROR_LOG("psxtrace rcnt2 init failed: {}", err);
      return false;
    }
  }

  if (s_psxtrace_card_active)
  {
    std::string err;
    if (!PSXTraceCapture::InitializeCard(s_psxtrace_path, s_psxtrace_card_active, &err))
    {
      ERROR_LOG("psxtrace card init failed: {}", err);
      return false;
    }
  }

  if (s_psxtrace_spuq)
  {
    std::string err;
    if (!PSXTraceCapture::InitializeSpuq(s_psxtrace_path, s_psxtrace_spuq_pc, &err))
    {
      ERROR_LOG("psxtrace spuq init failed: {}", err);
      return false;
    }
  }

  if (!s_psxtrace_spureg_pc.empty())
  {
    std::string err;
    if (!PSXTraceCapture::InitializeSpureg(s_psxtrace_path, s_psxtrace_spureg_pc, &err))
    {
      ERROR_LOG("psxtrace spureg init failed: {}", err);
      return false;
    }
  }

  if (!s_psxtrace_gte_log_path.empty())
  {
    std::string gte_err;
    if (!PSXTraceCapture::InitializeGTELog(s_psxtrace_gte_log_path, s_psxtrace_gte_lo, s_psxtrace_gte_hi,
                                           &gte_err))
    {
      ERROR_LOG("psxtrace gte log init failed: {}", gte_err);
      return false;
    }
  }

  {
    std::string err;
    if (!RamDump::Install(&err))
    {
      ERROR_LOG("{}", err);
      return false;
    }
    if (!Bk2Play::Install(&err))
    {
      ERROR_LOG("{}", err);
      return false;
    }
    if (!s_card_window_path.empty() && !CardWindow::Initialize(s_card_window_path, &err))
    {
      ERROR_LOG("{}", err);
      return false;
    }
  }

  if (!s_card1_image.empty())
  {
    // The image replaces the non-persistent card's contents before the
    // first frame; the card is never written back to the file.
    MemoryCard* mc = Pad::GetMemoryCard(0);
    MemoryCardImage::DataArray data;
    Error card_err;
    if (!mc || !MemoryCardImage::LoadFromFile(&data, s_card1_image.c_str(), &card_err))
    {
      ERROR_LOG("card import: cannot load {} into slot 1: {}", s_card1_image, card_err.GetDescription());
      return false;
    }
    mc->GetData() = data;
    INFO_LOG("card import: slot 1 <- {}", s_card1_image);
  }
  if (!s_motor_log_path.empty() && !MotorLog::Open(s_motor_log_path))
  {
    ERROR_LOG("motor-log: cannot open {}", s_motor_log_path);
    return false;
  }
  if (!Bk2Play::s_frames.empty())
    Bk2Play::Apply();
  CardExport::Poll(false);
  return true;
}

bool OpenpeteRegtest::OnFrame()
{
  const bool script_quit = !PadScript::s_steps.empty() && PadScript::Tick();
  if (!Bk2Play::s_frames.empty())
    Bk2Play::Apply();
  if (!CardExport::s_specs.empty())
    CardExport::Poll(false);
  return PSXTraceCapture::IsComplete() || script_quit;
}

void OpenpeteRegtest::OnSystemShutdown()
{
  CardExport::Poll(true);
}

void OpenpeteRegtest::Shutdown()
{
  Bk2Play::Report();
  MotorLog::Shutdown();
  RamDump::Shutdown();
  CardWindow::Shutdown();
  PSXTraceCapture::Shutdown();
  PSXTraceCapture::ShutdownGTELog();
}
