// SPDX-FileCopyrightText: 2026 Tyson Cleary
// SPDX-License-Identifier: MIT

// NTSC scanline pairs for the GPU's CRTC timing.
//
// An NTSC line is 227.5 colour subcarrier cycles, 3412.5 video clocks, so
// successive lines alternate between 3413 and 3412 clocks (6825 per pair)
// and a 263-line field lasts 897487.5 clocks on average: the hardware's
// 59.826 Hz VBL. A PAL line is 3405 clocks (6810 per pair). The first line
// after a reset is the longer one.
//
// The CRTC state's `horizontal_total` field holds the length of the current
// line. Code that reads it for one line is then exact unchanged; code that
// spans several lines calls the functions here, which know the pair length
// and the phase (whether the current line is the shorter one).

#pragma once

#include "common/types.h"
#include "types.h"

namespace OpenpeteCrtc {

static constexpr u32 NTSC_TICKS_PER_LINE_PAIR = 6825;
static constexpr u32 PAL_TICKS_PER_LINE_PAIR = 6810;

/// Clocks in two successive lines, scaled to the overclock; 0 before the first configuration.
inline u32 g_pair = 0;
/// Clocks in the longer line of a pair.
inline u32 g_long = 0;
/// 1 while the current line is the second, shorter line of its pair.
inline u8 g_short = 0;

/// Length in clocks of a line of phase `line_short`.
inline TickCount LineTicks(u8 line_short)
{
  return line_short ? static_cast<TickCount>(g_pair - g_long) : static_cast<TickCount>(g_long);
}

/// Sets the pair length (already scaled to the overclock) and returns the current line's length.
inline u16 Configure(u32 pair)
{
  g_pair = pair;
  g_long = (pair + 1u) / 2u;
  return static_cast<u16>(LineTicks(g_short));
}

/// Clears the phase at a GPU reset and returns the length of the line that then starts (`current` when the pair
/// length is not yet configured).
inline u16 Reset(u16 current)
{
  g_short = 0;
  return g_pair ? static_cast<u16>(LineTicks(0)) : current;
}

/// Clocks spanned by the next `lines` lines from the start of a line, the first of which is short if
/// `first_short` is set.
inline TickCount TicksForLines(u32 lines, u8 first_short)
{
  const TickCount short_ticks = static_cast<TickCount>(g_pair - g_long);
  const TickCount long_lines = static_cast<TickCount>((lines + (first_short ^ 1u)) / 2u);
  return static_cast<TickCount>(lines) * short_ticks + long_lines * (static_cast<TickCount>(g_long) - short_ticks);
}

/// Wraps a clock position counted from the start of a line of phase `line_short` into the line it falls in.
/// Returns the number of line starts crossed.
inline u32 Wrap(TickCount tick, u8 line_short, TickCount* out_tick, u8* out_short)
{
  const TickCount pos = tick + (line_short ? static_cast<TickCount>(g_long) : 0);
  const TickCount pairs = pos / static_cast<TickCount>(g_pair);
  const TickCount rem = pos % static_cast<TickCount>(g_pair);
  const u8 new_short = (rem >= static_cast<TickCount>(g_long)) ? 1u : 0u;
  *out_tick = rem - (new_short ? static_cast<TickCount>(g_long) : 0);
  *out_short = new_short;
  return static_cast<u32>(pairs) * 2u + new_short - line_short;
}

/// Moves the current line forward to the one holding clock `tick` (counted from the current line's start): stores
/// the clock within that line and its length, advances the phase, returns the lines crossed.
inline u32 Advance(TickCount tick, TickCount* tick_in_line, u16* line_ticks)
{
  const u32 lines = Wrap(tick, g_short, tick_in_line, &g_short);
  *line_ticks = static_cast<u16>(LineTicks(g_short));
  return lines;
}

/// Beam position at clock `tick` from the current line's start: the clock within its line and the line number.
inline void BeamPosition(TickCount tick, u32 current_line, u32 total_lines, u32* out_ticks, u32* out_line,
                         u8* out_short = nullptr)
{
  TickCount in_line;
  u8 line_short;
  const u32 lines = Wrap(tick, g_short, &in_line, &line_short);
  *out_line = (current_line + lines) % total_lines;
  *out_ticks = static_cast<u32>(in_line);
  if (out_short)
    *out_short = line_short;
}

/// Clocks from beam clock `tick` (counted from the current line's start) until clock `ticks` of line `line`.
inline TickCount TicksUntil(TickCount tick, u32 current_line, u32 total_lines, u32 ticks, u32 line)
{
  u32 current_tick, beam_line;
  u8 line_short;
  BeamPosition(tick, current_line, total_lines, &current_tick, &beam_line, &line_short);

  // Lines from the start of the beam's line to the start of the target line.
  u32 lines_to_target;
  if (ticks >= current_tick)
  {
    lines_to_target = (line >= beam_line) ? (line - beam_line) : ((total_lines - beam_line) + line);
  }
  else
  {
    const u32 next_line = (beam_line + 1) % total_lines;
    lines_to_target = 1 + ((line >= next_line) ? (line - next_line) : ((total_lines - next_line) + line));
  }
  return TicksForLines(lines_to_target, line_short) - static_cast<TickCount>(current_tick) +
         static_cast<TickCount>(ticks);
}

/// Mean line length in clocks.
inline double MeanLineTicks()
{
  return static_cast<double>(g_pair) * 0.5;
}

} // namespace OpenpeteCrtc
