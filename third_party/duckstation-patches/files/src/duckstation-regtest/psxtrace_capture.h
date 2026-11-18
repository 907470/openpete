// SPDX-FileCopyrightText: 2026 Tyson Cleary
// SPDX-License-Identifier: MIT
//
// psxtrace_capture — emit per-frame .psxtrace v2 oracle records.
//
// Hooks an Execute breakpoint at a configurable anchor PC (defaults to
// Spyro 1's GamestateDraw entry, 0x8001ED5C). On each hit, computes
// FNV-1a-64 hashes of the gameplay-state regions listed in the file
// -psxtrace-regions names (written per game by openpete's
// tools/gen_state_regions.py --out-flat) in Bus::g_ram and writes one
// record to the output trace. Format matches openpete's
// tools/psxtrace_format.py v2 so the same C/Python codec can consume
// traces produced here; the header carries the FNV-1a-64 of the regions
// file and this build's git hash, and each record's pad2 the VBLANK count
// at the hit.
//
// With -psxtrace-cd-read-pc (the game's CdRead entry) and
// -psxtrace-cd-read-cb-slot (the guest word libcd's CdReadCallback stores
// the game's handler in), every read is recorded to <trace>.loads: issue
// and completion each stamped with record index, VBLANK count, CPU tick
// and scanline, plus sectors, destination and mode. -psxtrace-cd-control-pc
// logs every CdControl command to <trace>.cdctl.

#pragma once

#include "common/types.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace PSXTraceCapture {

bool Initialize(const std::string& path,
                uint32_t anchor_pc,
                uint32_t anchor_hit_target,
                uint32_t max_frames,
                const std::string& ram_dump_path,
                const std::string& regions_path,
                uint32_t cd_read_pc, uint32_t cd_read_cb_slot,
                uint32_t cd_control_pc,
                uint32_t vsync_pc, uint32_t srand_pc, uint32_t rand_pc,
                uint32_t watch_addr, bool cd_irq, uint32_t cd_read_pc_min,
                const uint32_t* vsync_cb_pcs, int vsync_cb_count, bool pad_cmds, uint32_t cdsync_pc,
                uint32_t sio0_addr, uint32_t vsync_skip_lo, uint32_t vsync_skip_hi,
                std::string* error_out);

void Shutdown();

// GTE log: every GTE instruction the interpreter executes while the VBLANK
// count is in [lo, hi) is written to `path` as one CSV row in the layout of
// openpete's OPENPETE_GTE_TRACE (engine column "ds"), for a row-by-row diff
// of the two engines on the same tick. Independent of the trace; requires
// `-cpu interpreter` (the recompilers bypass GTE::ExecuteInstruction).
bool InitializeGTELog(const std::string& path, uint32_t vbl_lo, uint32_t vbl_hi,
                      std::string* error_out);
void ShutdownGTELog();

// GTE hash: every GTE instruction the interpreter executes between two
// consecutive records is folded into one row of <trace>.gte (record index,
// instruction count, hash over each instruction word and the 64 registers'
// read view after it, the bitmap of registers the game has written, a hash
// of the unwritten registers' values, VBLANK count at the opening record).
// With `dump`, every instruction of the records in [rec_lo, rec_hi] is also
// written to <trace>.gtedump as {u32 rec, pc, instr, written_lo,
// written_hi, pre[64], post[64]} in read view, little-endian. Called after
// Initialize(); closed by Shutdown(). The capture's Execute breakpoints
// keep the CPU on the interpreter, the only path through the hook.
bool InitializeGTEHash(const std::string& trace_path, bool dump, uint32_t rec_lo, uint32_t rec_hi,
                       std::string* error_out);

// VBLANK positions: every VBLANK interrupt taken outside a waited VSync
// call is written to <trace>.vblpos with its EPC, the innermost active
// call's entry and instruction count, the first game-code call target t
// entered after the handler returns and t's ordinal since its record's
// anchor (column list in psxtrace_capture.cpp). `library_path` lists the
// PsyQ spans that are not game code (`0x<lo> 0x<hi>` per line, hi
// exclusive); `self_entry_path` (optional) lists functions never chosen
// as t (`<overlay> 0x<addr>` per line). Needs Initialize() with a VSync
// recorder; installs the interpreter's instruction and exception hooks.
bool InitializeVblPos(const std::string& trace_path, const std::string& library_path,
                      const std::string& self_entry_path, std::string* error_out);

// libspu calls: every SpuSetKey, SpuGetKeyStatus, SpuGetAllKeysStatus,
// envelope- or pitch-setting SpuSetVoiceAttr and SpuWrite call (entry
// addresses in `pcs`, that order; 0 = absent) is written to <trace>.spuq
// with its arguments and the exact tick; a SpuWrite row adds the head of
// every sounding voice.
bool InitializeSpuq(const std::string& trace_path, const uint32_t pcs[5], std::string* error_out);

// -psxtrace-card-active HEX: <trace>.card, one row per store of a nonzero
// value to the card driver's transfer-active byte at HEX (the read, write
// and info handlers' first state sets it as the select byte goes out), with
// the ticks from that store of each step of the transfer (the SCPH-1001
// kernel's stores to mcHandler's byte counter 0x80007514, values 2 onward,
// and to the fast track's byte count 0x800072F0, values 1 onward) after its
// record, VBLANK count and the store's tick offset from that VBLANK.
bool InitializeCard(const std::string& trace_path, uint32_t active_addr, std::string* error_out);

// -psxtrace-rcnt2-pc HEX (repeatable, up to 8): <trace>.rcnt2, one row per
// read of root counter 2's current-count register (0x1F801120) by an
// instruction at one of `pcs`: record, VBLANK count, the instruction's
// address and the value it read.
bool InitializeRcnt2(const std::string& trace_path, const uint32_t* pcs, uint32_t npcs, std::string* error_out);

// -psxtrace-words HEX:N: <trace>.words, one row per record with its index,
// its VBLANK count and the N guest words from HEX (signed decimal), read at
// the anchor hit the record hashes.
bool InitializeWords(const std::string& trace_path, uint32_t addr, uint32_t count, std::string* error_out);

// SPU register accesses: every execution of an instruction at one of `pcs`
// (address, kind 0 kon store, 1 pitch store, 2 envx load; an sh, lh or
// lhu) is written to <trace>.spureg with its effective address, the stored
// value, the index of the libspu call row in progress and the exact tick.
bool InitializeSpureg(const std::string& trace_path, const std::vector<std::pair<uint32_t, uint8_t>>& pcs,
                      std::string* error_out);

// Returns true once max_frames have been captured (causes regtest to
// exit cleanly).
bool IsComplete();

// Stats for the exit summary.
uint32_t GetRecordsWritten();
uint32_t GetAnchorHitCount();

} // namespace PSXTraceCapture
