// SPDX-FileCopyrightText: 2026 Tyson Cleary
// SPDX-License-Identifier: MIT

#include "psxtrace_capture.h"

#include <ctime>

#include "core/bus.h"
#include "core/cpu_core.h"
#include "core/openpete_hooks.h"
#include "core/timers.h"
#include "core/cdrom.h"
#include "core/analog_controller.h"
#include "core/gpu.h"
#include "core/gte.h"
#include "core/spu.h"
#include "core/interrupt_controller.h"
#include "core/system.h"
#include "core/timing_event.h"

#include "common/error.h"
#include "common/log.h"

#include "scmversion/scmversion.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

LOG_CHANNEL(Host);

namespace PSXTraceCapture {

// ─── Format constants (mirror openpete tools/psxtrace_format.py) ─────
static constexpr uint32_t MAGIC_LEN = 8;
static constexpr const char MAGIC[MAGIC_LEN + 1] = "PSXTRACE";
static constexpr uint32_t VERSION = 2;
static constexpr uint32_t RAM_SIZE = 0x200000;
static constexpr uint32_t RECORD_MAGIC = 0x524D5246u; // 'FRMR'

// FNV-1a-64 — must match core/include/openpete_fnv1a64.h byte-for-byte.
static constexpr uint64_t FNV_OFFSET = 0xCBF29CE484222325ULL;
static constexpr uint64_t FNV_PRIME = 0x100000001B3ULL;

// ─── Region table ─────────────────────────────────────────────────────
// Read at Initialize() from the flat file openpete's
// tools/gen_state_regions.py --out-flat writes from the game's manifest:
//
//   psxtrace-regions <format_version> <field_count>
//   <kind> <start> <length> <aux> <elem_size> <mask_per_element> <mask> <name>
//
// kind: DIRECT | INDIRECT_ARRAY | INDIRECT_ARRAY_END_PTR |
// INDIRECT_ARRAY_COUNT; hex fields are 0x-prefixed; mask is a comma list
// of 0x offsets or `-`. The FNV-1a-64 of the file's bytes goes into the
// trace header so the verifier can refuse a golden taken from a table
// other than the one it registered.
static constexpr uint32_t FLAT_FORMAT_VERSION = 1;
static constexpr uint32_t FLAT_FIELD_COUNT = 8;
// The verifier's table bound (OPENPETE_PSXTRACE_MAX_REGIONS) and name
// buffer (64 bytes, compared whole).
static constexpr size_t MAX_REGIONS = 128;
static constexpr size_t NAME_MAX_BYTES = 63;

enum RegionKind : uint8_t {
  REGION_DIRECT = 0,
  REGION_INDIRECT_ARRAY = 1,
  REGION_INDIRECT_ARRAY_END_PTR = 2,
  REGION_INDIRECT_ARRAY_COUNT = 3,
  // aux = sentinel offset | value << 8; live = elements before the first
  // whose byte at the offset equals the value (terminator excluded).
  REGION_INDIRECT_ARRAY_SENTINEL = 4,
};

struct Region {
  RegionKind kind;
  uint32_t start;     // PSX vaddr (or base-pointer addr for indirect)
  uint32_t length;    // bytes (or elem_size * max_elements for indirect)
  uint32_t aux;       // end-ptr or count addr (indirect variants)
  uint32_t elem_size; // for INDIRECT_ARRAY_COUNT, OR for any region with a per-element mask
  std::string name;
  // Bytes at masked offsets are excluded from BOTH the region hash and
  // the rolling total hash, matching openpete's mask-aware compute
  // (core/runtime/run.c). Empty means "hash all".
  std::vector<uint16_t> mask_offsets;
  uint8_t mask_per_element; // 0=absolute offsets, 1=repeat per elem_size
};

// ─── State ───────────────────────────────────────────────────────────
namespace {

std::FILE* s_fp = nullptr;
uint32_t s_anchor_pc = 0;
uint32_t s_anchor_hit_target = 1;
uint32_t s_max_frames = 0;       // 0 = unlimited
uint32_t s_anchor_hits = 0;
uint32_t s_records_written = 0;
bool s_armed = false;
bool s_complete = false;
std::string s_ram_dump_path;
bool s_ram_dumped = false;
std::vector<Region> s_regions;
uint64_t s_table_hash = 0;

// CD read recorder: Execute breakpoints at the game's CdRead entry and at
// the handler libcd's CdReadCallback stored in the game's slot, each event
// stamped with the record index, VBLANK count, CPU tick and scanline, so
// the sidecar <trace>.loads places every read against the trace.
struct PendingRead {
  uint32_t issue_rec, issue_vbl, issue_line;
  uint64_t issue_tick;
  uint32_t sectors, dst, mode;
};
std::FILE* s_loads_fp = nullptr;
std::FILE* s_cdctl_fp = nullptr;
uint32_t s_cd_read_pc = 0;
uint32_t s_cd_read_cb_slot = 0;
uint32_t s_cd_read_cb_pc = 0;       // handler currently breakpointed (read from the slot)
uint32_t s_cd_control_pc = 0;
uint32_t s_reads_issued = 0;
uint32_t s_reads_done = 0;
uint32_t s_cb_stray = 0;
bool s_read_pending = false;
PendingRead s_pending{};

// The CPU tick of the current instruction: the global counter at the last
// event commit plus the ticks the CPU has run since. In CPU context (a
// breakpoint, the instruction or exception hook) the commit left the global
// counter equal to the counter pending ticks are relative to; inside an
// event callback pending ticks were reset before the commit and the global
// counter is the event's own tick. Every sidecar tick column uses it.
inline uint64_t ExactTick()
{
  return TimingEvents::GetGlobalTickCounter() + CPU::GetPendingTicks();
}

// ─── Little-endian writers ───────────────────────────────────────────
void Write8(uint8_t v) { std::fwrite(&v, 1, 1, s_fp); }
void Write32(uint32_t v) {
  uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16),
                  uint8_t(v >> 24)};
  std::fwrite(b, 1, 4, s_fp);
}
void Write64(uint64_t v) {
  Write32(uint32_t(v));
  Write32(uint32_t(v >> 32));
}

// ─── FNV-1a-64 ───────────────────────────────────────────────────────
uint64_t FnvUpdate(uint64_t h, const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= FNV_PRIME;
  }
  return h;
}

uint32_t ReadU32(const uint8_t* ram, uint32_t off) {
  return uint32_t(ram[off]) | (uint32_t(ram[off + 1]) << 8) |
         (uint32_t(ram[off + 2]) << 16) | (uint32_t(ram[off + 3]) << 24);
}

bool ParseHex(const std::string& tok, uint32_t* out) {
  if (tok.size() < 3 || tok[0] != '0' || (tok[1] != 'x' && tok[1] != 'X'))
    return false;
  char* endp = nullptr;
  unsigned long long v = std::strtoull(tok.c_str() + 2, &endp, 16);
  if (!endp || *endp != 0 || v > 0xFFFFFFFFull)
    return false;
  *out = uint32_t(v);
  return true;
}

std::vector<std::string> SplitSpaces(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : line) {
    if (c == ' ') {
      if (!cur.empty()) { out.push_back(cur); cur.clear(); }
    } else if (c != '\r' && c != '\n') {
      cur.push_back(c);
    }
  }
  if (!cur.empty())
    out.push_back(cur);
  return out;
}

// Parse the flat table into s_regions and hash the file's bytes into
// s_table_hash. Refuses an unreadable or empty file, a header line with
// another version or field count, a malformed row, a name the verifier
// could not compare whole, or more rows than the verifier holds.
bool LoadRegionsFile(const std::string& path, std::string* err) {
  std::FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) {
    *err = "regions file unreadable: " + path;
    return false;
  }
  std::string text;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0)
    text.append(buf, n);
  std::fclose(fp);
  if (text.empty()) {
    *err = "regions file empty: " + path;
    return false;
  }
  s_table_hash = FnvUpdate(FNV_OFFSET, reinterpret_cast<const uint8_t*>(text.data()), text.size());

  std::vector<std::string> lines;
  {
    size_t pos = 0;
    while (pos < text.size()) {
      size_t nl = text.find('\n', pos);
      if (nl == std::string::npos) nl = text.size();
      lines.push_back(text.substr(pos, nl - pos));
      pos = nl + 1;
    }
  }
  if (lines.empty()) {
    *err = "regions file has no header line";
    return false;
  }
  {
    std::vector<std::string> h = SplitSpaces(lines[0]);
    if (h.size() != 3 || h[0] != "psxtrace-regions") {
      *err = "regions file header is not `psxtrace-regions <version> <fields>`: " + lines[0];
      return false;
    }
    if (std::strtoul(h[1].c_str(), nullptr, 10) != FLAT_FORMAT_VERSION ||
        std::strtoul(h[2].c_str(), nullptr, 10) != FLAT_FIELD_COUNT) {
      *err = "regions file format " + h[1] + "/" + h[2] + " but this module reads " +
             std::to_string(FLAT_FORMAT_VERSION) + "/" + std::to_string(FLAT_FIELD_COUNT);
      return false;
    }
  }
  std::vector<Region> regions;
  for (size_t li = 1; li < lines.size(); li++) {
    std::vector<std::string> f = SplitSpaces(lines[li]);
    if (f.empty())
      continue;
    if (f.size() != FLAT_FIELD_COUNT) {
      *err = "regions file line " + std::to_string(li + 1) + ": " +
             std::to_string(f.size()) + " fields, wanted " + std::to_string(FLAT_FIELD_COUNT);
      return false;
    }
    Region r{};
    if (f[0] == "DIRECT") r.kind = REGION_DIRECT;
    else if (f[0] == "INDIRECT_ARRAY") r.kind = REGION_INDIRECT_ARRAY;
    else if (f[0] == "INDIRECT_ARRAY_END_PTR") r.kind = REGION_INDIRECT_ARRAY_END_PTR;
    else if (f[0] == "INDIRECT_ARRAY_COUNT") r.kind = REGION_INDIRECT_ARRAY_COUNT;
    else if (f[0] == "INDIRECT_ARRAY_SENTINEL") r.kind = REGION_INDIRECT_ARRAY_SENTINEL;
    else {
      *err = "regions file line " + std::to_string(li + 1) + ": unknown kind " + f[0];
      return false;
    }
    if (!ParseHex(f[1], &r.start) || !ParseHex(f[2], &r.length) ||
        !ParseHex(f[3], &r.aux) || !ParseHex(f[4], &r.elem_size)) {
      *err = "regions file line " + std::to_string(li + 1) + ": bad hex field";
      return false;
    }
    if (f[5] != "0" && f[5] != "1") {
      *err = "regions file line " + std::to_string(li + 1) + ": mask_per_element must be 0 or 1";
      return false;
    }
    r.mask_per_element = uint8_t(f[5] == "1");
    if (f[6] != "-") {
      size_t pos = 0;
      while (pos <= f[6].size()) {
        size_t comma = f[6].find(',', pos);
        if (comma == std::string::npos) comma = f[6].size();
        uint32_t off;
        if (!ParseHex(f[6].substr(pos, comma - pos), &off) || off > 0xFFFFu) {
          *err = "regions file line " + std::to_string(li + 1) + ": bad mask offset";
          return false;
        }
        r.mask_offsets.push_back(uint16_t(off));
        pos = comma + 1;
      }
    }
    r.name = f[7];
    if (r.name.size() > NAME_MAX_BYTES) {
      *err = "regions file line " + std::to_string(li + 1) + ": name longer than " +
             std::to_string(NAME_MAX_BYTES) + " bytes";
      return false;
    }
    regions.push_back(std::move(r));
  }
  if (regions.empty()) {
    *err = "regions file lists no regions";
    return false;
  }
  if (regions.size() > MAX_REGIONS) {
    *err = "regions file lists " + std::to_string(regions.size()) + " regions, verifier holds " +
           std::to_string(MAX_REGIONS);
    return false;
  }
  s_regions = std::move(regions);
  return true;
}

// Mirror openpete's gameplay_hash_compute_ex.
uint64_t ComputeRegionHashes(uint64_t* per_region) {
  const uint8_t* ram = Bus::g_ram;
  if (!ram)
    return 0;
  const uint32_t RAM_BYTES = RAM_SIZE;
  uint64_t total = FNV_OFFSET;
  constexpr uint32_t MASK = 0x1FFFFFu;
  for (size_t r = 0; r < s_regions.size(); r++) {
    const Region& reg = s_regions[r];
    per_region[r] = 0;
    const bool is_indirect = (reg.kind == REGION_INDIRECT_ARRAY ||
                              reg.kind == REGION_INDIRECT_ARRAY_END_PTR ||
                              reg.kind == REGION_INDIRECT_ARRAY_COUNT ||
                              reg.kind == REGION_INDIRECT_ARRAY_SENTINEL);
    uint32_t base_off;
    uint32_t live_len = reg.length;
    uint32_t base_vaddr = 0;
    if (is_indirect) {
      uint32_t ptr_off = reg.start & MASK;
      if (ptr_off + 4u > RAM_BYTES)
        continue;
      base_vaddr = ReadU32(ram, ptr_off);
      if (base_vaddr == 0u)
        continue;
      base_off = base_vaddr & MASK;
    } else {
      base_off = reg.start & MASK;
    }
    if (reg.kind == REGION_INDIRECT_ARRAY_END_PTR) {
      uint32_t aux_off = reg.aux & MASK;
      if (aux_off + 4u <= RAM_BYTES) {
        uint32_t end_vaddr = ReadU32(ram, aux_off);
        /* end<base => allocator reset, pool empty; skip residue. */
        if (end_vaddr >= base_vaddr) {
          uint32_t live = end_vaddr - base_vaddr;
          if (live < live_len)
            live_len = live;
        } else {
          live_len = 0;
        }
      }
    } else if (reg.kind == REGION_INDIRECT_ARRAY_COUNT) {
      uint32_t aux_off = reg.aux & MASK;
      if (aux_off + 4u <= RAM_BYTES && reg.elem_size > 0) {
        uint32_t cnt = ReadU32(ram, aux_off);
        uint64_t live = uint64_t(cnt) * uint64_t(reg.elem_size);
        if (live < uint64_t(live_len))
          live_len = uint32_t(live);
      }
    } else if (reg.kind == REGION_INDIRECT_ARRAY_SENTINEL) {
      const uint32_t es = reg.elem_size;
      const uint32_t s_off = reg.aux & 0xFFu;
      const uint8_t s_val = uint8_t((reg.aux >> 8) & 0xFFu);
      if (es > 0 && s_off < es) {
        uint32_t n = 0;
        while (uint64_t(n + 1u) * es <= live_len &&
               base_off + uint64_t(n) * es + s_off < RAM_BYTES &&
               ram[base_off + n * es + s_off] != s_val)
          n++;
        live_len = n * es;
      }
    }
    uint32_t end = base_off + live_len;
    if (end > RAM_BYTES)
      end = RAM_BYTES;
    if (end <= base_off)
      continue;
    size_t len = end - base_off;
    const uint8_t* p = ram + base_off;
    // Mask-aware hash: must mirror openpete core/runtime/run.c
    // gameplay_hash_compute_ex byte-for-byte, including the rolling-
    // total semantics (masked bytes are excluded from BOTH per-region
    // and total).
    if (reg.mask_offsets.empty()) {
      per_region[r] = FnvUpdate(FNV_OFFSET, p, len);
      total = FnvUpdate(total, p, len);
    } else {
      const uint16_t* m = reg.mask_offsets.data();
      const size_t     mc = reg.mask_offsets.size();
      const uint32_t   es = reg.elem_size;
      const bool per_el = (reg.mask_per_element != 0u && es != 0u);
      uint64_t per = FNV_OFFSET;
      for (size_t i = 0; i < len; i++) {
        uint32_t off = per_el ? uint32_t(i % es) : uint32_t(i);
        bool masked = false;
        for (size_t mi = 0; mi < mc; mi++) {
          if (uint32_t(m[mi]) == off) { masked = true; break; }
        }
        if (masked) continue;
        per   ^= uint64_t(p[i]); per   *= FNV_PRIME;
        total ^= uint64_t(p[i]); total *= FNV_PRIME;
      }
      per_region[r] = per;
    }
  }
  return total;
}

// Header flag bits (tools/psxtrace_format.py). Bits 2 and 3 each add a
// block after the names blob, in bit order; bits 4 and 5 change the meaning
// of the record's pad2 and pad1 fields.
static constexpr uint32_t FLAG_TABLE_HASH = 1u << 2;  // u64 FNV-1a-64 of the regions file
static constexpr uint32_t FLAG_SCM_HASH = 1u << 3;    // u8 len + ASCII git hash of this build
static constexpr uint32_t FLAG_VBL_STAMP = 1u << 4;   // record pad2 = VBLANK count at the hit
static constexpr uint32_t FLAG_ANCHOR_TICK = 1u << 5; // record pad1 = CPU ticks from that VBLANK to the hit

void WriteHeader() {
  // 8 magic + 4 version + 4 frame_count + 4 ram_size + 4 flags
  // + 4 region_count + 4 names_total_len + 4 reserved
  std::fwrite(MAGIC, 1, MAGIC_LEN, s_fp);
  Write32(VERSION);
  Write32(0); // frame_count, finalised at close
  Write32(RAM_SIZE);
  Write32(FLAG_TABLE_HASH | FLAG_SCM_HASH | FLAG_VBL_STAMP | FLAG_ANCHOR_TICK); // hash-only records
  Write32(uint32_t(s_regions.size()));
  uint32_t names_blob_len = 0;
  for (const auto& r : s_regions)
    names_blob_len += 1u + uint32_t(r.name.size());
  Write32(names_blob_len);
  Write32(0); // reserved
  for (const auto& r : s_regions) {
    Write8(uint8_t(r.name.size()));
    std::fwrite(r.name.data(), 1, r.name.size(), s_fp);
  }
  Write64(s_table_hash);
  size_t scm_len = std::strlen(g_scm_hash_str);
  if (scm_len > 255)
    scm_len = 255;
  Write8(uint8_t(scm_len));
  std::fwrite(g_scm_hash_str, 1, scm_len, s_fp);
}

// ─── GTE hash recorder ───────────────────────────────────────────────
// With -psxtrace-gte, every GTE instruction executed between consecutive
// records is folded into one hash per record interval and written to
// <trace>.gte. Row k covers the instructions after record k's anchor hit up
// to record k+1's; instructions before record 0 belong to no row, and the
// interval after the last record is not written.
//
// Per instruction the fold takes the instruction word, then the 64
// registers (32 data, 32 control) after it in their read view: the value
// mfc2/cfc2 returns on hardware, a pure function of the stored words, so an
// implementation that stores a 16-bit register unextended until its next
// use hashes the same as one that extends on write:
//   data 1,3,5 (VZ0-2), 8-11 (IR0-3)     sign-extended 16 bits
//   data 7 (OTZ), 16-19 (SZ0-3)          zero-extended 16 bits
//   data 15 (SXYP)                       reads SXY2 (data 14)
//   data 23 (RES1)                       the stored word
//   data 28, 29 (IRGB, ORGB)             ORGB from IR1-3: clamp(IRn >> 7, 0, 0x1F)
//   data 31 (LZCR)                       leading-bit count of LZCS (data 30)
//   ctrl 4, 12, 20 (RT33, L33, LR33),
//   26 (H), 27 (DQA), 29, 30 (ZSF3/4)    sign-extended 16 bits
//   ctrl 31 (FLAG)                       bits 30-12, bit 31 = OR of 30-23, 18-13
//   every other register                 the stored word
// The fold is FNV-1a-64 over 32-bit words (h ^= w; h *= prime).
//
// A register is "written" once the game changed its read view: an
// instruction whose read view of it differs before and after, or a value
// before an instruction that differs from the value after the previous one
// (a store in between). Instructions run from the BIOS ROM update the
// baseline without marking. An unwritten register enters the fold as 0, so
// reset and BIOS residue never reaches the hash; each row carries the
// written bitmap and a separate hash of the unwritten registers' values.
std::FILE* s_gteh_fp = nullptr;
std::FILE* s_gted_fp = nullptr;
uint32_t s_gted_lo = 0, s_gted_hi = 0; // record window of the dump, inclusive
uint64_t s_gted_rows = 0;
uint64_t s_gteh_hash = FNV_OFFSET;
uint32_t s_gteh_ops = 0;
uint64_t s_gteh_written = 0;
uint32_t s_gteh_last[64] = {};
uint32_t s_gteh_rec_vbl = 0;
uint64_t s_gteh_ops_total = 0;
uint32_t s_gteh_rows = 0;
uint64_t s_gteh_rom_ops = 0;

inline uint32_t Sx16(uint32_t w) { return uint32_t(int32_t(int16_t(uint16_t(w)))); }

uint32_t LeadingBits(uint32_t v) {
  const uint32_t x = (v & 0x80000000u) ? ~v : v;
  uint32_t n = 0;
  while (n < 32 && !(x & (0x80000000u >> n)))
    n++;
  return n;
}

uint32_t ClampOrgb(uint32_t ir) {
  int32_t c = int32_t(Sx16(ir)) >> 7;
  return uint32_t(c < 0 ? 0 : (c > 0x1F ? 0x1F : c));
}

void GteReadView(const uint32_t raw[64], uint32_t v[64]) {
  for (int i = 0; i < 64; i++)
    v[i] = raw[i];
  static constexpr int kSext[] = {1, 3, 5, 8, 9, 10, 11, 32 + 4, 32 + 12, 32 + 20, 32 + 26, 32 + 27, 32 + 29, 32 + 30};
  for (int i : kSext)
    v[i] = Sx16(raw[i]);
  static constexpr int kZext[] = {7, 16, 17, 18, 19};
  for (int i : kZext)
    v[i] = raw[i] & 0xFFFFu;
  v[15] = raw[14];
  v[28] = v[29] = ClampOrgb(raw[9]) | (ClampOrgb(raw[10]) << 5) | (ClampOrgb(raw[11]) << 10);
  v[31] = LeadingBits(raw[30]);
  v[63] = (raw[63] & 0x7FFFF000u) | ((raw[63] & 0x7F87E000u) ? 0x80000000u : 0u);
}

inline uint64_t FoldWord(uint64_t h, uint32_t w) { return (h ^ w) * FNV_PRIME; }

void GteHashWriteRow(uint32_t rec) {
  uint64_t residue = FNV_OFFSET;
  for (uint32_t i = 0; i < 64; i++) {
    if (s_gteh_written & (1ull << i))
      continue;
    residue = FoldWord(FoldWord(residue, i), s_gteh_last[i]);
  }
  std::fprintf(s_gteh_fp, "%u %u %016llx %016llx %016llx %u\n", rec, s_gteh_ops, (unsigned long long)s_gteh_hash,
               (unsigned long long)s_gteh_written, (unsigned long long)residue, s_gteh_rec_vbl);
  s_gteh_rows++;
}

// At an anchor hit, before record `rec` is written: closes the interval
// that began at record rec-1 and opens the next one.
void GteHashOnRecord(uint32_t rec, uint32_t vbl) {
  if (!s_gteh_fp)
    return;
  if (rec > 0)
    GteHashWriteRow(rec - 1);
  s_gteh_hash = FNV_OFFSET;
  s_gteh_ops = 0;
  s_gteh_rec_vbl = vbl;
}

void GteHashHook(u32 inst_bits, const u32 pre_raw[64]) {
  uint32_t post_raw[64], pre[64], post[64];
  for (u32 i = 0; i < 64; i++)
    post_raw[i] = *GTE::GetRegisterPtr(i);
  GteReadView(pre_raw, pre);
  GteReadView(post_raw, post);
  const uint32_t pc = CPU::g_state.current_instruction_pc;
  if ((pc & 0x1FFFFFFFu) >= 0x1FC00000u) {
    s_gteh_rom_ops++;
  } else {
    for (uint32_t i = 0; i < 64; i++)
      if (pre[i] != s_gteh_last[i] || post[i] != pre[i])
        s_gteh_written |= 1ull << i;
  }
  std::memcpy(s_gteh_last, post, sizeof(post));
  if (!s_armed || s_complete)
    return;
  const uint32_t rec = s_records_written - 1u;
  if (s_gted_fp && rec >= s_gted_lo && rec <= s_gted_hi) {
    const uint32_t head[5] = {rec, pc, inst_bits, uint32_t(s_gteh_written), uint32_t(s_gteh_written >> 32)};
    std::fwrite(head, 4, 5, s_gted_fp);
    std::fwrite(pre, 4, 64, s_gted_fp);
    std::fwrite(post, 4, 64, s_gted_fp);
    s_gted_rows++;
  }
  uint64_t h = FoldWord(s_gteh_hash, inst_bits);
  for (uint32_t i = 0; i < 64; i++)
    h = FoldWord(h, (s_gteh_written & (1ull << i)) ? post[i] : 0u);
  s_gteh_hash = h;
  s_gteh_ops++;
  s_gteh_ops_total++;
}

void VblPosOnRecord();

// -psxtrace-words: <trace>.words, the guest words [addr, addr + 4 * count)
// at every record, read at the anchor hit the record hashes.
std::FILE* s_words_fp = nullptr;
uint32_t s_words_addr = 0;
uint32_t s_words_count = 0;

void WordsOnRecord()
{
  if (!s_words_fp)
    return;
  std::fprintf(s_words_fp, "%u %u", s_records_written, System::GetFrameNumber());
  for (uint32_t i = 0; i < s_words_count; i++)
    std::fprintf(s_words_fp, " %d", static_cast<int32_t>(ReadU32(Bus::g_ram, (s_words_addr + 4 * i) & 0x1FFFFFu)));
  std::fputc('\n', s_words_fp);
}

bool BreakpointCB(CPU::BreakpointType type, VirtualMemoryAddress pc,
                  VirtualMemoryAddress memaddr) {
  (void)type;
  (void)memaddr;
  if (s_complete || !s_fp)
    return true; // keep BP armed; we'll close via Shutdown()
  s_anchor_hits++;
  if (!s_armed) {
    if (s_anchor_hits < s_anchor_hit_target)
      return true;
    s_armed = true;
    INFO_LOG("psxtrace: capture armed at hit #{}, pc=0x{:08X}",
             s_anchor_hits, pc);
  }
  if (!s_ram_dumped && !s_ram_dump_path.empty()) {
    std::FILE* rfp = std::fopen(s_ram_dump_path.c_str(), "wb");
    if (rfp) {
      std::fwrite(Bus::g_ram, 1, RAM_SIZE, rfp);
      std::fclose(rfp);
      INFO_LOG("psxtrace: dumped 2MB RAM at hit #{} -> {}",
               s_anchor_hits, s_ram_dump_path);
    } else {
      ERROR_LOG("psxtrace: failed to open ram dump path: {}",
                s_ram_dump_path);
    }
    s_ram_dumped = true;
  }
  std::vector<uint64_t> per(s_regions.size());
  uint64_t total = ComputeRegionHashes(per.data());
  GteHashOnRecord(s_records_written, System::GetFrameNumber());
  VblPosOnRecord();
  WordsOnRecord();
  Write32(RECORD_MAGIC);
  Write32(s_records_written);
  // pad1: the hit's exact tick less the tick the VBLANK interrupt line rose
  // for the frame pad2 counts.
  Write32(uint32_t(ExactTick() - System::GetFrameStartTick()));
  Write32(System::GetFrameNumber()); // pad2: VBLANK count at this hit
  Write32(uint32_t(pc));
  Write64(total);
  for (size_t i = 0; i < s_regions.size(); i++)
    Write64(per[i]);
  s_records_written++;
  if ((s_records_written % 200) == 0) {
    INFO_LOG("psxtrace: {} frames, total=0x{:016X}", s_records_written,
             total);
  }
  if (s_max_frames != 0 && s_records_written >= s_max_frames) {
    s_complete = true;
    INFO_LOG("psxtrace: captured {} frames; flagging complete",
             s_records_written);
  }
  return true;
}

void BeamLine(uint32_t* line) {
  uint32_t ticks = 0;
  g_gpu.GetBeamPosition(&ticks, line);
}

bool CdReadDoneCB(CPU::BreakpointType type, VirtualMemoryAddress pc,
                  VirtualMemoryAddress memaddr);

// VSync recorder: every libetc VSync call with its mode, the VBLANK count
// at entry and at return, and the counter value it returned; the return is
// caught by a breakpoint at the caller's $ra, placed at entry. Calls nest:
// the vblank interrupt lands inside a call and its handler calls VSync
// itself, so the pending calls form a stack and each return pops the
// innermost pending call made from that return address.
std::FILE* s_vsync_fp = nullptr;
uint32_t s_vsync_pc = 0;
// Query calls (mode < 0) from return addresses in [lo, hi) are counted and
// not written: a library's own timeout polls, which a host that answers
// the library's calls itself never makes.
uint32_t s_vsync_skip_lo = 0, s_vsync_skip_hi = 0, s_vsync_skipped = 0;
uint32_t s_vsync_calls = 0;
struct VSyncPending { int32_t mode; uint32_t rec, entry_vbl, ra, idx; uint64_t entry_tick; };
static constexpr int VSYNC_NEST_MAX = 8;
VSyncPending s_vsync_stack[VSYNC_NEST_MAX];
int s_vsync_depth = 0;
uint32_t s_vsync_dropped = 0;
// Return breakpoints in place, one per distinct return address, with the
// number of pending calls that return there.
struct VSyncRetBP { uint32_t pc; int uses; };
VSyncRetBP s_vsync_ret[VSYNC_NEST_MAX];
int s_vsync_ret_n = 0;
// Consecutive queries from the same call site with the same record and
// counter are one row with a repeat count (a game polls the counter while
// it works or waits). Every row carries the caller's return address: the
// consumer matches a call by its site, since the library's own polls
// (libcd's sync timeouts) have no counterpart on a host that thunks them.
struct { uint32_t idx, rec, entry_vbl, ret_vbl, v0, n, ra; uint64_t entry_tick, ret_tick; } s_vsync_run;
bool s_vsync_run_open = false;

static void VSyncFlushRun()
{
  if (!s_vsync_run_open)
    return;
  std::fprintf(s_vsync_fp, "%u %u -1 %u %u %llu %llu %u %u 0x%08X\n", s_vsync_run.idx, s_vsync_run.rec,
               s_vsync_run.entry_vbl, s_vsync_run.ret_vbl, (unsigned long long)s_vsync_run.entry_tick,
               (unsigned long long)s_vsync_run.ret_tick, s_vsync_run.v0, s_vsync_run.n, s_vsync_run.ra);
  s_vsync_run_open = false;
}

static void VSyncRetRelease(uint32_t pc)
{
  for (int i = 0; i < s_vsync_ret_n; i++) {
    if (s_vsync_ret[i].pc != pc)
      continue;
    if (--s_vsync_ret[i].uses == 0) {
      CPU::RemoveBreakpoint(CPU::BreakpointType::Execute, pc);
      s_vsync_ret[i] = s_vsync_ret[--s_vsync_ret_n];
    }
    return;
  }
}

static void VSyncResetRecorder()
{
  for (int i = 0; i < s_vsync_ret_n; i++)
    CPU::RemoveBreakpoint(CPU::BreakpointType::Execute, s_vsync_ret[i].pc);
  s_vsync_ret_n = 0;
  s_vsync_depth = 0;
  s_vsync_dropped = 0;
  s_vsync_run_open = false;
}

// Callback registrations (libetc VSyncCallback and the game's own slot
// registrar): a row with mode -2 in the vsync sidecar, the VBLANK count at
// the call, its first argument in the value column and the caller's return
// address, so a consumer can charge the VBLs a tick's work spent before the
// registration instead of after it.
static constexpr int VSYNC_CB_MAX = 4;
uint32_t s_vsync_cb_pc[VSYNC_CB_MAX];
int s_vsync_cb_n = 0;

bool VSyncCbCB(CPU::BreakpointType type, VirtualMemoryAddress pc, VirtualMemoryAddress memaddr)
{
  (void)type; (void)memaddr; (void)pc;
  if (!s_vsync_fp)
    return true;
  const CPU::Registers& r = CPU::GetRegs();
  VSyncFlushRun();
  std::fprintf(s_vsync_fp, "%u %u -2 %u %u %llu %llu %u 1 0x%08X\n", s_vsync_calls++, s_records_written,
               System::GetFrameNumber(), System::GetFrameNumber(),
               (unsigned long long)ExactTick(),
               (unsigned long long)ExactTick(), r.a0, r.ra);
  return true;
}

// srand recorder: the seed argument of every libc srand call.
std::FILE* s_srand_fp = nullptr;
uint32_t s_srand_calls = 0;

bool VSyncRetCB(CPU::BreakpointType type, VirtualMemoryAddress pc, VirtualMemoryAddress memaddr)
{
  (void)type; (void)memaddr;
  if (!s_vsync_fp)
    return true;
  int k = s_vsync_depth - 1;
  while (k >= 0 && s_vsync_stack[k].ra != pc)
    k--;
  if (k < 0)
    return true; // the address is reached by ordinary control flow too
  const VSyncPending p = s_vsync_stack[k];
  for (int i = k; i + 1 < s_vsync_depth; i++)
    s_vsync_stack[i] = s_vsync_stack[i + 1];
  s_vsync_depth--;
  VSyncRetRelease(pc);
  const CPU::Registers& r = CPU::GetRegs();
  // Every call: a waited call's row carries the VBLANK count at entry and
  // return (its value is the horizontal count); a query's row carries the
  // counter the game read, so a consumer can answer the game's reads in
  // the reference's sequence, including the reads inside a tick's work
  // that saw the interrupt land.
  if (p.mode < 0 && p.ra >= s_vsync_skip_lo && p.ra < s_vsync_skip_hi) {
    s_vsync_skipped++;
  } else if (p.mode < 0) {
    if (s_vsync_run_open && s_vsync_run.rec == p.rec && s_vsync_run.v0 == r.v0 && s_vsync_run.ra == p.ra) {
      s_vsync_run.n++;
    } else {
      VSyncFlushRun();
      s_vsync_run.idx = p.idx;
      s_vsync_run.rec = p.rec;
      s_vsync_run.ra = p.ra;
      s_vsync_run.entry_vbl = p.entry_vbl;
      s_vsync_run.ret_vbl = System::GetFrameNumber();
      s_vsync_run.entry_tick = p.entry_tick;
      s_vsync_run.ret_tick = ExactTick();
      s_vsync_run.v0 = r.v0;
      s_vsync_run.n = 1;
      s_vsync_run_open = true;
    }
  } else {
    VSyncFlushRun();
    std::fprintf(s_vsync_fp, "%u %u %d %u %u %llu %llu %u 1 0x%08X\n", p.idx, p.rec, p.mode, p.entry_vbl,
                 System::GetFrameNumber(), (unsigned long long)p.entry_tick,
                 (unsigned long long)ExactTick(), r.v0, p.ra);
  }
  return true;
}

bool VSyncCB(CPU::BreakpointType type, VirtualMemoryAddress pc, VirtualMemoryAddress memaddr)
{
  (void)type; (void)memaddr; (void)pc;
  if (!s_vsync_fp)
    return true;
  const CPU::Registers& r = CPU::GetRegs();
  if (s_vsync_depth == VSYNC_NEST_MAX) {
    std::fprintf(s_vsync_fp, "# call %u dropped: %d calls pending\n", s_vsync_calls, s_vsync_depth);
    s_vsync_dropped++;
    s_vsync_calls++;
    return true;
  }
  VSyncPending& p = s_vsync_stack[s_vsync_depth++];
  p.mode = static_cast<int32_t>(r.a0);
  p.rec = s_records_written;
  p.entry_vbl = System::GetFrameNumber();
  p.entry_tick = ExactTick();
  p.ra = r.ra;
  p.idx = s_vsync_calls++;
  for (int i = 0; i < s_vsync_ret_n; i++) {
    if (s_vsync_ret[i].pc == r.ra) {
      s_vsync_ret[i].uses++;
      return true;
    }
  }
  if (s_vsync_ret_n == VSYNC_NEST_MAX) {
    ERROR_LOG("psxtrace: VSync return breakpoint table full at 0x{:08X}", r.ra);
    return true;
  }
  if (CPU::AddBreakpointWithCallback(CPU::BreakpointType::Execute, r.ra, &VSyncRetCB))
    s_vsync_ret[s_vsync_ret_n++] = {r.ra, 1};
  else
    ERROR_LOG("psxtrace: breakpoint at VSync return 0x{:08X} failed", r.ra);
  return true;
}

// CdSync recorder: every libcd CdSync call with its mode and the value it
// returned, so a host that answers the drive's completion itself can
// answer the game's polls in the reference's sequence and timing.
// Consecutive calls with the same record, VBLANK, mode, value and site
// are one row with a repeat count (a game spins on the drive).
std::FILE* s_cdsync_fp = nullptr;
uint32_t s_cdsync_calls = 0;
struct CdSyncPending { int32_t mode; uint32_t rec, vbl, ra; uint64_t tick; };
static constexpr int CDSYNC_NEST_MAX = 4;
CdSyncPending s_cdsync_stack[CDSYNC_NEST_MAX];
int s_cdsync_depth = 0;
struct { uint32_t idx, rec, vbl, ra, n; int32_t mode, ret; uint64_t tick; } s_cdsync_run;
bool s_cdsync_run_open = false;

static void CdSyncFlushRun()
{
  if (!s_cdsync_run_open)
    return;
  std::fprintf(s_cdsync_fp, "%u %u %u %d %d %u 0x%08X %llu\n", s_cdsync_run.idx, s_cdsync_run.rec, s_cdsync_run.vbl,
               s_cdsync_run.mode, s_cdsync_run.ret, s_cdsync_run.n, s_cdsync_run.ra,
               (unsigned long long)s_cdsync_run.tick);
  s_cdsync_run_open = false;
}

bool CdSyncRetCB(CPU::BreakpointType type, VirtualMemoryAddress pc, VirtualMemoryAddress memaddr)
{
  (void)type; (void)memaddr;
  if (!s_cdsync_fp)
    return true;
  int k = s_cdsync_depth - 1;
  while (k >= 0 && s_cdsync_stack[k].ra != pc)
    k--;
  if (k < 0)
    return true;
  const CdSyncPending p = s_cdsync_stack[k];
  for (int i = k; i + 1 < s_cdsync_depth; i++)
    s_cdsync_stack[i] = s_cdsync_stack[i + 1];
  s_cdsync_depth--;
  bool still_pending = false;
  for (int i = 0; i < s_cdsync_depth; i++)
    if (s_cdsync_stack[i].ra == pc)
      still_pending = true;
  if (!still_pending)
    CPU::RemoveBreakpoint(CPU::BreakpointType::Execute, pc);
  const CPU::Registers& r = CPU::GetRegs();
  const uint32_t vbl = System::GetFrameNumber();
  const int32_t ret = static_cast<int32_t>(r.v0);
  if (s_cdsync_run_open && s_cdsync_run.rec == p.rec && s_cdsync_run.vbl == vbl && s_cdsync_run.mode == p.mode &&
      s_cdsync_run.ret == ret && s_cdsync_run.ra == p.ra) {
    s_cdsync_run.n++;
  } else {
    CdSyncFlushRun();
    s_cdsync_run.idx = s_cdsync_calls;
    s_cdsync_run.rec = p.rec;
    s_cdsync_run.vbl = vbl;
    s_cdsync_run.mode = p.mode;
    s_cdsync_run.ret = ret;
    s_cdsync_run.n = 1;
    s_cdsync_run.ra = p.ra;
    s_cdsync_run.tick = p.tick;
    s_cdsync_run_open = true;
  }
  s_cdsync_calls++;
  return true;
}

bool CdSyncCB(CPU::BreakpointType type, VirtualMemoryAddress pc, VirtualMemoryAddress memaddr)
{
  (void)type; (void)memaddr; (void)pc;
  if (!s_cdsync_fp || s_cdsync_depth == CDSYNC_NEST_MAX)
    return true;
  const CPU::Registers& r = CPU::GetRegs();
  CdSyncPending& p = s_cdsync_stack[s_cdsync_depth++];
  p.mode = static_cast<int32_t>(r.a0);
  p.rec = s_records_written;
  p.vbl = System::GetFrameNumber();
  p.tick = ExactTick();
  p.ra = r.ra;
  bool have = false;
  for (int i = 0; i + 1 < s_cdsync_depth; i++)
    if (s_cdsync_stack[i].ra == r.ra)
      have = true;
  if (!have && !CPU::AddBreakpointWithCallback(CPU::BreakpointType::Execute, r.ra, &CdSyncRetCB))
    ERROR_LOG("psxtrace: breakpoint at CdSync return 0x{:08X} failed", r.ra);
  return true;
}

// CD-ROM controller recorder: every command byte the CPU writes to the
// controller (with the writing instruction's address and the parameter
// FIFO) and every interrupt the controller raises (with the response
// FIFO), so a host with no controller model can answer a game's direct
// register traffic in the reference's sequence and timing.
std::FILE* s_padcmd_fp = nullptr;
uint32_t s_padcmd_count = 0;

static void PadCommandHook(u32 index, u8 command)
{
  if (!s_padcmd_fp)
    return;
  std::fprintf(s_padcmd_fp, "%u %u %u %u 0x%02X\n", s_padcmd_count++, s_records_written, System::GetFrameNumber(), index,
               command);
}

std::FILE* s_cdirq_fp = nullptr;
uint32_t s_cdirq_commands = 0, s_cdirq_interrupts = 0, s_cdirq_reads = 0;
uint32_t s_cdirq_read_pc_min = 0;   // reads from instructions at or above this address are logged

static void CdReadHook(u32 offset, u8 value, u32 pc)
{
  if (!s_cdirq_fp || !s_cdirq_read_pc_min || pc < s_cdirq_read_pc_min || (pc & 0x1FFFFFFFu) >= 0x00200000u)
    return;
  std::fprintf(s_cdirq_fp, "R %u %u %u 0x%08X %u 0x%02X\n", s_cdirq_reads++, s_records_written, System::GetFrameNumber(), pc,
               offset, value);
}

static void CdCommandHook(u8 command, u32 pc, const u8* params, u32 n)
{
  if (!s_cdirq_fp)
    return;
  std::fprintf(s_cdirq_fp, "C %u %u %u 0x%08X 0x%02X %u", s_cdirq_commands++, s_records_written, System::GetFrameNumber(),
               pc, command, n);
  for (u32 i = 0; i < n; i++)
    std::fprintf(s_cdirq_fp, " %02X", params[i]);
  std::fputc('\n', s_cdirq_fp);
}

static void CdInterruptHook(u8 interrupt, const u8* response, u32 n)
{
  if (!s_cdirq_fp)
    return;
  std::fprintf(s_cdirq_fp, "I %u %u %u %u %u", s_cdirq_interrupts++, s_records_written, System::GetFrameNumber(),
               interrupt, n);
  for (u32 i = 0; i < n; i++)
    std::fprintf(s_cdirq_fp, " %02X", response[i]);
  std::fputc('\n', s_cdirq_fp);
}

// rand recorder: the value every libc rand call returned, caught at the
// caller's $ra.
std::FILE* s_rand_fp = nullptr;
uint32_t s_rand_ret_pc = 0;
uint32_t s_rand_calls = 0;

bool RandRetCB(CPU::BreakpointType type, VirtualMemoryAddress pc, VirtualMemoryAddress memaddr)
{
  (void)type; (void)memaddr; (void)pc;
  if (s_rand_ret_pc) {
    CPU::RemoveBreakpoint(CPU::BreakpointType::Execute, s_rand_ret_pc);
    s_rand_ret_pc = 0;
  }
  if (!s_rand_fp)
    return true;
  const CPU::Registers& r = CPU::GetRegs();
  std::fprintf(s_rand_fp, "%u %u %u 0x%08X\n", s_rand_calls - 1, s_records_written, System::GetFrameNumber(), r.v0);
  return true;
}

bool RandCB(CPU::BreakpointType type, VirtualMemoryAddress pc, VirtualMemoryAddress memaddr)
{
  (void)type; (void)memaddr; (void)pc;
  if (!s_rand_fp)
    return true;
  const CPU::Registers& r = CPU::GetRegs();
  s_rand_calls++;
  if (s_rand_ret_pc && s_rand_ret_pc != r.ra) {
    CPU::RemoveBreakpoint(CPU::BreakpointType::Execute, s_rand_ret_pc);
    s_rand_ret_pc = 0;
  }
  if (!s_rand_ret_pc) {
    if (CPU::AddBreakpointWithCallback(CPU::BreakpointType::Execute, r.ra, &RandRetCB))
      s_rand_ret_pc = r.ra;
  }
  return true;
}

// Watched word: every store to one guest word, with the value stored (the
// rt register of the store at pc; the breakpoint fires before the store)
// and the value it replaces.
std::FILE* s_watch_fp = nullptr;
uint32_t s_watch_addr = 0;
uint32_t s_watch_writes = 0;

bool WatchCB(CPU::BreakpointType type, VirtualMemoryAddress pc, VirtualMemoryAddress memaddr)
{
  (void)type; (void)memaddr;
  if (!s_watch_fp)
    return true;
  (void)pc;
  const CPU::Registers& r = CPU::GetRegs();
  // The breakpoint fires inside the store's execution; the store is the
  // current instruction, not g_state.pc (already the next one).
  const u32 store_pc = CPU::g_state.current_instruction_pc;
  const u32 inst = ReadU32(Bus::g_ram, store_pc & 0x1FFFFFu);
  const u32 rt = (inst >> 16) & 31u;
  const u32 old_value = ReadU32(Bus::g_ram, s_watch_addr & 0x1FFFFFu);
  std::fprintf(s_watch_fp, "%u %u %u 0x%08X 0x%08X 0x%08X %llu\n", s_watch_writes, s_records_written,
               System::GetFrameNumber(), store_pc, old_value, r.r[rt],
               static_cast<unsigned long long>(ExactTick()));
  s_watch_writes++;
  return true;
}

// SIO0 handler word: the kernel's controller/card vblank handler stores to
// one of its own words on every vblank it runs (the memory card port-flip
// word in the SCPH-100x kernel); each store names a vblank the handler
// served. Consecutive vblanks are run-length encoded.
std::FILE* s_sio0_fp = nullptr;
uint32_t s_sio0_addr = 0;
uint32_t s_sio0_rows = 0, s_sio0_served = 0;
uint32_t s_sio0_run_rec = 0, s_sio0_run_vbl = 0, s_sio0_run_n = 0;

static void Sio0FlushRun()
{
  if (!s_sio0_run_n)
    return;
  std::fprintf(s_sio0_fp, "%u %u %u %u\n", s_sio0_rows++, s_sio0_run_rec, s_sio0_run_vbl, s_sio0_run_n);
  s_sio0_run_n = 0;
}

bool Sio0CB(CPU::BreakpointType type, VirtualMemoryAddress pc, VirtualMemoryAddress memaddr)
{
  (void)type; (void)pc; (void)memaddr;
  if (!s_sio0_fp)
    return true;
  const u32 vbl = System::GetFrameNumber();
  // One row per vblank: the handler stores the word once per run.
  if (s_sio0_run_n && s_sio0_run_vbl + s_sio0_run_n - 1u == vbl)
    return true;
  s_sio0_served++;
  if (s_sio0_run_n && s_sio0_run_rec == s_records_written && s_sio0_run_vbl + s_sio0_run_n == vbl) {
    s_sio0_run_n++;
    return true;
  }
  Sio0FlushRun();
  s_sio0_run_rec = s_records_written;
  s_sio0_run_vbl = vbl;
  s_sio0_run_n = 1;
  return true;
}

bool SrandCB(CPU::BreakpointType type, VirtualMemoryAddress pc, VirtualMemoryAddress memaddr)
{
  (void)type; (void)memaddr; (void)pc;
  if (!s_srand_fp)
    return true;
  const CPU::Registers& r = CPU::GetRegs();
  std::fprintf(s_srand_fp, "%u %u %u 0x%08X\n", s_srand_calls, s_records_written, System::GetFrameNumber(), r.a0);
  s_srand_calls++;
  return true;
}

bool CdReadCB(CPU::BreakpointType type, VirtualMemoryAddress pc,
              VirtualMemoryAddress memaddr) {
  (void)type;
  (void)memaddr;
  (void)pc;
  if (!s_loads_fp)
    return true;
  const CPU::Registers& r = CPU::GetRegs();
  if (s_read_pending) {
    // libcd holds one read at a time; a second issue before the callback
    // means the game re-issued after its own timeout. Close the first as
    // never completed.
    std::fprintf(s_loads_fp, "%u %u %u %llu %u %u 0x%08X 0x%02X - - - - reissued\n",
                 s_reads_issued - 1, s_pending.issue_rec, s_pending.issue_vbl,
                 (unsigned long long)s_pending.issue_tick, s_pending.issue_line,
                 s_pending.sectors, s_pending.dst, s_pending.mode);
  }
  s_pending.issue_rec = s_records_written;
  s_pending.issue_vbl = System::GetFrameNumber();
  s_pending.issue_tick = ExactTick();
  BeamLine(&s_pending.issue_line);
  s_pending.sectors = r.a0;
  s_pending.dst = r.a1;
  s_pending.mode = r.a2;
  s_read_pending = true;
  s_reads_issued++;
  // The handler libcd will call: the game's function stored in the slot.
  // Breakpoint it now; re-point when the slot changes.
  if (s_cd_read_cb_slot) {
    uint32_t handler = ReadU32(Bus::g_ram, s_cd_read_cb_slot & 0x1FFFFFu);
    if (handler != s_cd_read_cb_pc) {
      if (s_cd_read_cb_pc)
        CPU::RemoveBreakpoint(CPU::BreakpointType::Execute, s_cd_read_cb_pc);
      s_cd_read_cb_pc = 0;
      if (handler) {
        if (CPU::AddBreakpointWithCallback(CPU::BreakpointType::Execute, handler, &CdReadDoneCB)) {
          s_cd_read_cb_pc = handler;
          std::fprintf(s_loads_fp, "# read callback handler 0x%08X (slot 0x%08X) at read %u\n",
                       handler, s_cd_read_cb_slot, s_reads_issued - 1);
        } else {
          ERROR_LOG("psxtrace: breakpoint at read handler 0x{:08X} failed", handler);
        }
      } else {
        std::fprintf(s_loads_fp, "# read %u issued with an empty callback slot\n",
                     s_reads_issued - 1);
      }
    }
  }
  return true;
}

bool CdReadDoneCB(CPU::BreakpointType type, VirtualMemoryAddress pc,
                  VirtualMemoryAddress memaddr) {
  (void)type;
  (void)memaddr;
  (void)pc;
  if (!s_loads_fp)
    return true;
  const CPU::Registers& r = CPU::GetRegs();
  uint32_t line = 0;
  BeamLine(&line);
  if (!s_read_pending) {
    s_cb_stray++;
    std::fprintf(s_loads_fp, "# stray callback rec=%u vbl=%u tick=%llu line=%u intr=%u\n",
                 s_records_written, System::GetFrameNumber(),
                 (unsigned long long)ExactTick(), line, r.a0);
    return true;
  }
  std::fprintf(s_loads_fp, "%u %u %u %llu %u %u 0x%08X 0x%02X %u %u %llu %u %u\n",
               s_reads_issued - 1, s_pending.issue_rec, s_pending.issue_vbl,
               (unsigned long long)s_pending.issue_tick, s_pending.issue_line,
               s_pending.sectors, s_pending.dst, s_pending.mode,
               s_records_written, System::GetFrameNumber(),
               (unsigned long long)ExactTick(), line, r.a0);
  s_read_pending = false;
  s_reads_done++;
  return true;
}

bool CdControlCB(CPU::BreakpointType type, VirtualMemoryAddress pc,
                 VirtualMemoryAddress memaddr) {
  (void)type;
  (void)memaddr;
  (void)pc;
  if (!s_cdctl_fp)
    return true;
  const CPU::Registers& r = CPU::GetRegs();
  std::fprintf(s_cdctl_fp, "%u %u 0x%02X 0x%08X\n", s_records_written,
               System::GetFrameNumber(), r.a0, r.a1);
  return true;
}

// ─── VBLANK position recorder (<trace>.vblpos) ───────────────────────
// For each VBLANK interrupt the CPU takes outside a waited VSync call (no
// call with mode >= 0 pending on the VSync recorder's stack), one row
// placing the interrupt in the game's execution, so a host that runs the
// game's code without a cycle clock can deliver the interrupt at the same
// point:
//   idx    row number
//   rec    record being accumulated at the dispatch (records written)
//   vbl    VBLANK count at the dispatch
//   via    irq: the dispatch found VBLANK pending; nested: VBLANK was
//          raised while another interrupt's handler ran and that handler
//          served it (the row takes that interrupt's EPC and tick)
//   epc    EPC (the branch when the interrupt hit a delay slot)
//   entry  entry address of the innermost active call at the dispatch
//          (shadow call stack, below)
//   insns  instructions executed outside exception handlers from that
//          entry to the EPC, callees included
//   t      the first call target entered after the handler's rfe whose
//          address and call site are both game code and which the
//          self-entry list does not name
//   t_rec  record being accumulated when t was entered
//   k      t's ordinal in t_rec: executions at pc == t since t_rec's
//          anchor, counted outside exception handlers and only when the
//          instruction executed before it is game code
//   tick   exact CPU tick at the dispatch
//   rt     the RAM word (KSEG0, word-aligned) of the first load, between
//          the handler's rfe and t, by game code outside exception
//          handlers of a word whose last store was made by game code
//          inside the exception that opened the row or a later row-opening
//          one (the game's VSync callback: its VBL counters), in t_rec;
//          `-` when there is none: the interrupt's work is observable at
//          that load, before t
//   rk     rt's ordinal in t_rec: game-code loads of that word outside
//          exception handlers since t_rec's anchor, the load included
// Any store other than a marking one (outside exception handlers, by
// kernel or library code, or inside an exception that opened no row)
// clears the word's mark. Library and kernel stores are left out because a
// host that replaces those functions keeps their state its own way. A host
// delivers at rt's rk-th load or t's k-th entry, whichever it reaches
// first; t stays the fallback for a host whose path skips the load.
// Game code is RAM at or above 0x10000 outside the library spans file
// (the thunked PsyQ functions); the kernel and the BIOS ROM are not game
// code. A row whose t is never reached before capture ends prints `-` for
// t, t_rec and k. Rows of interrupts taken before one t is reached share
// that t.
//
// Detection uses the interpreter's instruction and exception hooks: an
// exception raises the handler depth, an rfe executed inside one lowers
// it; a jal or jalr outside handlers marks its target as the instruction
// two after it (after the delay slot); the shadow stack pushes {return
// address, target} at each such target and pops when the return address
// executes. Counts reset at each anchor hit, before the anchor's own
// instruction executes, so the anchor's entry counts in the new record.
std::FILE* s_vblpos_fp = nullptr;
std::vector<uint8_t> s_game_code;   // one byte per RAM word: 1 = game code
std::vector<uint8_t> s_self_entry;  // one byte per RAM word: 1 = never chosen as t
std::vector<uint32_t> s_pc_count, s_pc_epoch;
uint32_t s_epoch = 1;
uint32_t s_exc_depth = 0;
uint64_t s_insns = 0;
bool s_prev_game = false;
uint32_t s_call_pc = 0;
int s_call_phase = 0;
struct ShadowFrame { uint32_t ret, entry; uint64_t insns_at_entry; };
static constexpr int SHADOW_MAX = 256;
ShadowFrame s_shadow[SHADOW_MAX];
int s_shadow_n = 0;
uint32_t s_shadow_dropped = 0;
struct VblPosRow
{
  uint32_t idx, rec, vbl, epc, entry, serial, rt, rk, rt_rec;
  uint64_t insns, tick;
  bool nested, after_rfe, has_rt;
};
// Handler-written words: the serial of the outermost exception whose game
// code last stored each RAM word, when that exception opened a row; 0 when
// the word's last store was not such a store.
std::vector<uint32_t> s_hw_serial;
std::vector<uint32_t> s_ld_count, s_ld_epoch;  // game-code loads per RAM word since the anchor
std::vector<uint32_t> s_exc_stores;            // RAM words stored inside the current outermost exception, bit 31 = by game code
uint32_t s_exc_serial = 0, s_exc_rows_at_entry = 0;
uint32_t s_vblpos_read_resolved = 0;
// The outermost exception in progress, when it is an interrupt: where it
// interrupted the game, for a VBLANK its handler serves without a dispatch
// of its own.
struct { bool active; uint32_t epc, entry; uint64_t insns, tick; } s_vblpos_exc;
std::vector<VblPosRow> s_vblpos_open;
uint32_t s_vblpos_rows = 0, s_vblpos_resolved = 0, s_vblpos_unresolved = 0;
uint32_t s_vblpos_irqs = 0, s_vblpos_nested = 0, s_vblpos_in_wait = 0, s_vblpos_self_skipped = 0, s_vblpos_last_vbl = 0xFFFFFFFFu;

inline uint32_t RamWord(uint32_t addr)
{
  return (addr & 0x1FFFFFu) >> 2;
}

inline bool IsGameCode(uint32_t addr)
{
  const uint32_t m = addr & 0x1FFFFFFFu;
  return m >= 0x10000u && m < RAM_SIZE && s_game_code[m >> 2];
}

void VblPosWriteRow(const VblPosRow& r, uint32_t t, uint32_t t_rec, uint32_t k, bool resolved)
{
  std::fprintf(s_vblpos_fp, "%u %u %u %s 0x%08X 0x%08X %llu ", r.idx, r.rec, r.vbl, r.nested ? "nested" : "irq", r.epc,
               r.entry, (unsigned long long)r.insns);
  if (resolved)
    std::fprintf(s_vblpos_fp, "0x%08X %u %u", t, t_rec, k);
  else
    std::fprintf(s_vblpos_fp, "- - -");
  std::fprintf(s_vblpos_fp, " %llu", (unsigned long long)r.tick);
  if (resolved && r.has_rt && r.rt_rec == t_rec)
  {
    std::fprintf(s_vblpos_fp, " 0x%08X %u\n", r.rt, r.rk);
    s_vblpos_read_resolved++;
  }
  else
    std::fprintf(s_vblpos_fp, " - -\n");
}

// At an anchor hit: counts restart for the record about to open.
void VblPosOnRecord()
{
  if (!s_vblpos_fp)
    return;
  if (++s_epoch == 0)
  {
    std::fill(s_pc_epoch.begin(), s_pc_epoch.end(), 0u);
    s_epoch = 1;
  }
}

// Opens a row for the VBLANK numbered `vbl`, served by the outermost
// interrupt exception whose dispatch state is in s_vblpos_exc, unless a
// waited VSync call is pending (the wait, not the game's work, absorbs it).
void VblPosOpenRow(uint32_t vbl, bool nested)
{
  s_vblpos_last_vbl = vbl;
  s_vblpos_irqs++;
  if (nested)
    s_vblpos_nested++;
  for (int i = 0; i < s_vsync_depth; i++)
  {
    if (s_vsync_stack[i].mode >= 0)
    {
      s_vblpos_in_wait++;
      return;
    }
  }
  VblPosRow r{};
  r.idx = s_vblpos_rows++;
  r.rec = s_records_written;
  r.vbl = vbl;
  r.epc = s_vblpos_exc.epc;
  r.entry = s_vblpos_exc.entry;
  r.insns = s_vblpos_exc.insns;
  r.tick = s_vblpos_exc.tick;
  r.nested = nested;
  r.after_rfe = nested;
  r.serial = s_exc_serial;
  s_vblpos_open.push_back(r);
}

void VblPosExceptionHook(u32 cause_bits, u32 epc)
{
  s_exc_depth++;
  if (s_exc_depth != 1u)
    return; // raised inside a handler
  s_exc_serial++;
  s_exc_rows_at_entry = s_vblpos_rows;
  s_exc_stores.clear();
  s_vblpos_exc.active = ((cause_bits >> 2) & 0x1Fu) == 0u;
  if (!s_vblpos_exc.active)
    return; // not an interrupt
  s_vblpos_exc.epc = epc;
  s_vblpos_exc.entry = s_shadow_n > 0 ? s_shadow[s_shadow_n - 1].entry : 0u;
  s_vblpos_exc.insns = s_shadow_n > 0 ? s_insns - s_shadow[s_shadow_n - 1].insns_at_entry : 0u;
  s_vblpos_exc.tick = ExactTick();
  const uint32_t pending =
    InterruptController::ReadRegister(0) & InterruptController::ReadRegister(4) & (1u << static_cast<u32>(InterruptController::IRQ::VBLANK));
  const uint32_t vbl = System::GetFrameNumber();
  if (!pending || vbl == s_vblpos_last_vbl)
    return; // no VBLANK, or the same one still pending at a later dispatch
  if (s_armed && !s_complete)
    VblPosOpenRow(vbl, false);
  else
    s_vblpos_last_vbl = vbl;
}

// At the rfe that ends the outermost exception. A VBLANK raised while
// another interrupt's handler ran is served by that handler (the kernel
// walks every pending source before returning) and never dispatched with
// its own bit set at entry: the VBLANK count moved and the mask admits
// VBLANK, so the row takes the enclosing interrupt's EPC and tick.
void VblPosOnReturn()
{
  if (!s_vblpos_exc.active)
    return;
  s_vblpos_exc.active = false;
  const uint32_t vbl = System::GetFrameNumber();
  if (vbl == s_vblpos_last_vbl ||
      !(InterruptController::ReadRegister(4) & (1u << static_cast<u32>(InterruptController::IRQ::VBLANK))))
    return;
  if (s_armed && !s_complete)
    VblPosOpenRow(vbl, true);
  else
    s_vblpos_last_vbl = vbl;
}

// The instruction at `pc` is a call target entered from game code at a
// time no handler runs: resolves every open row whose handler returned.
void VblPosOnCallTarget(uint32_t pc, uint32_t k)
{
  if (s_self_entry[RamWord(pc)])
  {
    if (!s_vblpos_open.empty() && s_vblpos_open.front().after_rfe)
      s_vblpos_self_skipped++;
    return;
  }
  size_t n = 0;
  while (n < s_vblpos_open.size() && s_vblpos_open[n].after_rfe)
  {
    VblPosWriteRow(s_vblpos_open[n], pc, s_records_written, k, true);
    s_vblpos_resolved++;
    n++;
  }
  s_vblpos_open.erase(s_vblpos_open.begin(), s_vblpos_open.begin() + n);
}

// The outermost exception returned: its game-code stores mark their words
// with its serial when it opened a row; every other store clears its word.
void VblPosCommitStores()
{
  const uint32_t tag = s_vblpos_rows != s_exc_rows_at_entry ? s_exc_serial : 0u;
  for (uint32_t e : s_exc_stores)
    s_hw_serial[e & 0x7FFFFFFFu] = (e & 0x80000000u) ? tag : 0u;
  s_exc_stores.clear();
}

// A game-code load of RAM word `w` outside exception handlers: counts it
// and, when the word holds a marking store of an open row's exception or a
// later one, gives every such row whose handler returned and which has no
// load yet the load as rt.
void VblPosOnGameLoad(uint32_t w)
{
  if (s_ld_epoch[w] != s_epoch)
  {
    s_ld_epoch[w] = s_epoch;
    s_ld_count[w] = 0;
  }
  const uint32_t k = ++s_ld_count[w];
  const uint32_t tag = s_hw_serial[w];
  if (tag == 0u)
    return;
  for (VblPosRow& r : s_vblpos_open)
  {
    if (!r.after_rfe || r.has_rt || tag < r.serial)
      continue;
    r.has_rt = true;
    r.rt = 0x80000000u | (w << 2);
    r.rk = k;
    r.rt_rec = s_records_written;
  }
}

// The RAM word a load or store instruction `bits` accesses, from the
// registers before it executes; false for a non-memory instruction or an
// address outside main RAM.
bool VblPosMemWord(u32 bits, bool* store, uint32_t* w)
{
  switch (bits >> 26)
  {
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x32:
      *store = false; break; // lb lh lwl lw lbu lhu lwr lwc2
    case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2E: case 0x3A:
      *store = true; break;  // sb sh swl sw swr swc2
    default:
      return false;
  }
  const u32 addr = (CPU::g_state.regs.r[(bits >> 21) & 31u] +
                    static_cast<u32>(static_cast<s32>(static_cast<s16>(bits & 0xFFFFu)))) & 0x1FFFFFFFu;
  if (addr >= 0x800000u)
    return false;
  *w = RamWord(addr);
  return true;
}

void VblPosInstructionHook(u32 pc, u32 bits)
{
  const uint32_t op = bits >> 26;
  bool store = false;
  uint32_t mw = 0;
  const bool mem = VblPosMemWord(bits, &store, &mw);
  if (s_exc_depth > 0)
  {
    if (mem && store)
      s_exc_stores.push_back(mw | (IsGameCode(pc) ? 0x80000000u : 0u));
    // rfe: COP0, CO bit, function 0x10. It sits in the delay slot of the
    // handler's jr, so the next instruction is the return address.
    if (op == 0x10u && (bits & (1u << 25)) && (bits & 0x3Fu) == 0x10u && --s_exc_depth == 0)
    {
      VblPosOnReturn();
      VblPosCommitStores();
      for (VblPosRow& r : s_vblpos_open)
        r.after_rfe = true;
    }
    return;
  }
  const bool game = IsGameCode(pc);
  if (mem && store)
    s_hw_serial[mw] = 0u;
  else if (mem && game)
    VblPosOnGameLoad(mw);
  if (s_shadow_n > 0 && s_shadow[s_shadow_n - 1].ret == pc)
    s_shadow_n--;
  uint32_t k = 0;
  if (game && s_prev_game)
  {
    const uint32_t w = RamWord(pc);
    if (s_pc_epoch[w] != s_epoch)
    {
      s_pc_epoch[w] = s_epoch;
      s_pc_count[w] = 0;
    }
    k = ++s_pc_count[w];
  }
  if (s_call_phase == 1)
  {
    if (s_shadow_n == SHADOW_MAX)
    {
      std::memmove(s_shadow, s_shadow + 1, sizeof(ShadowFrame) * (SHADOW_MAX - 1));
      s_shadow_n--;
      s_shadow_dropped++;
    }
    s_shadow[s_shadow_n++] = {s_call_pc + 8u, pc, s_insns};
    if (game && k != 0 && IsGameCode(s_call_pc) && !s_vblpos_open.empty())
      VblPosOnCallTarget(pc, k);
  }
  if (s_call_phase > 0)
    s_call_phase--;
  if (op == 0x03u || (op == 0u && (bits & 0x3Fu) == 0x09u))
  {
    s_call_pc = pc;
    s_call_phase = 2;
  }
  s_prev_game = game;
  s_insns++;
}

// Reads `lo hi` hex pairs (half-open) into `bitmap` as `value`.
bool LoadSpans(const std::string& path, std::vector<uint8_t>& bitmap, uint8_t value, bool pairs, uint32_t* rows,
               std::string* err)
{
  std::FILE* fp = std::fopen(path.c_str(), "r");
  if (!fp)
  {
    *err = "cannot open " + path;
    return false;
  }
  char line[256];
  *rows = 0;
  while (std::fgets(line, sizeof(line), fp))
  {
    if (line[0] == '#' || line[0] == '\n')
      continue;
    std::vector<std::string> f = SplitSpaces(line);
    uint32_t lo = 0, hi = 0;
    // A self-entry row is `<overlay> 0x<addr>`; a span row is `0x<lo> 0x<hi>`.
    const bool ok = pairs ? (f.size() == 2 && ParseHex(f[0], &lo) && ParseHex(f[1], &hi) && hi > lo)
                          : (f.size() == 2 && ParseHex(f[1], &lo));
    if (!ok)
    {
      std::fclose(fp);
      *err = path + ": malformed row: " + line;
      return false;
    }
    if (!pairs)
      hi = lo + 4u;
    for (uint32_t a = lo & ~3u; a < hi; a += 4u)
    {
      const uint32_t m = a & 0x1FFFFFFFu;
      if (m < RAM_SIZE)
        bitmap[m >> 2] = value;
    }
    (*rows)++;
  }
  std::fclose(fp);
  return true;
}

// ─── libspu call recorder (<trace>.spuq) ─────────────────────────────
// One row per libspu call that reads or moves voice envelope state, at
// the function's entry: SpuSetKey (on/off, voice bits), SpuGetKeyStatus
// (voice bit), SpuGetAllKeysStatus (status buffer), and SpuSetVoiceAttr
// when its mask has a pitch field (PITCH, NOTE, SAMPLE_NOTE) or an ADSR
// field (AMODE..SL, ADSR1, ADSR2), with the SpuVoiceAttr fields those
// set. Rows are written from boot: a call before the anchor's first hit
// carries rec 0, as a .loads row does, so a voice keyed on before the
// first record has its reference clock. Columns: idx rec vbl kind pc ra
// tick, then per kind
//   setkey     on voices
//   keystatus  voice
//   allkeys    buf
//   voiceattr  voice mask pitch note sample_note a_mode s_mode r_mode ar dr sr rr sl adsr1 adsr2
//   spuwrite   src size n, then n voice heads v:start:cur:counter
// The spuwrite kind is a SpuWrite entry (a main-RAM to SPU-RAM transfer of
// size bytes from src). Its voice heads are taken after the SPU has
// generated every sample due at the call's tick, one per voice whose
// envelope is not off: the voice index, the start address register and
// the address of the ADPCM block the voice decodes (both in bytes), and
// the voice's 4.12 position counter (bits 12-16 the sample within the
// block's 28, bits 4-11 the interpolation fraction).
enum SpuqKind : uint8_t { SPUQ_SETKEY, SPUQ_KEYSTATUS, SPUQ_ALLKEYS, SPUQ_VOICEATTR, SPUQ_SPUWRITE, SPUQ_KINDS };
constexpr const char* kSpuqNames[SPUQ_KINDS] = {"setkey", "keystatus", "allkeys", "voiceattr", "spuwrite"};
constexpr uint32_t SPU_VOICE_ENVELOPE_MASK = 0x70u | 0xFF00u | 0x60000u;
std::FILE* s_spuq_fp = nullptr;
uint32_t s_spuq_pc[SPUQ_KINDS] = {};
uint32_t s_spuq_rows = 0, s_spuq_attr_skipped = 0, s_spuq_by_kind[SPUQ_KINDS] = {};

bool SpuqCB(CPU::BreakpointType type, VirtualMemoryAddress pc, VirtualMemoryAddress memaddr)
{
  (void)type; (void)memaddr;
  if (!s_spuq_fp || s_complete)
    return true;
  int kind = -1;
  for (int i = 0; i < SPUQ_KINDS; i++)
    if (s_spuq_pc[i] == pc)
      kind = i;
  if (kind < 0)
    return true;
  const CPU::Registers& r = CPU::GetRegs();
  u8 attr[64] = {};
  if (kind == SPUQ_VOICEATTR)
  {
    if (!CPU::SafeReadMemoryBytes(r.a0, attr, sizeof(attr)))
      return true;
    const uint32_t mask = ReadU32(attr, 4);
    if (!(mask & SPU_VOICE_ENVELOPE_MASK))
    {
      s_spuq_attr_skipped++;
      return true;
    }
  }
  std::fprintf(s_spuq_fp, "%u %u %u %s 0x%08X 0x%08X %llu", s_spuq_rows, s_records_written, System::GetFrameNumber(),
               kSpuqNames[kind], static_cast<uint32_t>(pc), r.ra, (unsigned long long)ExactTick());
  auto h16 = [&](int off) { return uint32_t(attr[off]) | (uint32_t(attr[off + 1]) << 8); };
  switch (kind)
  {
    case SPUQ_SETKEY: std::fprintf(s_spuq_fp, " %d 0x%06X", static_cast<int32_t>(r.a0), r.a1); break;
    case SPUQ_KEYSTATUS: std::fprintf(s_spuq_fp, " 0x%06X", r.a0); break;
    case SPUQ_ALLKEYS: std::fprintf(s_spuq_fp, " 0x%08X", r.a0); break;
    case SPUQ_SPUWRITE:
    {
      SPU::GeneratePendingSamples();
      SPU::VoiceHead heads[SPU::VOICE_COUNT];
      uint32_t n = 0;
      for (uint32_t v = 0; v < SPU::VOICE_COUNT; v++)
        if (SPU::GetVoiceHead(v, &heads[n]) && heads[n].on)
          n++;
      std::fprintf(s_spuq_fp, " 0x%08X %u %u", r.a0, r.a1, n);
      for (uint32_t i = 0; i < n; i++)
        std::fprintf(s_spuq_fp, " %u:0x%05X:0x%05X:0x%05X", heads[i].voice, heads[i].start_address,
                     heads[i].current_address, heads[i].counter);
      break;
    }
    default:
      std::fprintf(s_spuq_fp, " 0x%06X 0x%08X %u %u %u %d %d %d %u %u %u %u %u 0x%04X 0x%04X", ReadU32(attr, 0),
                   ReadU32(attr, 4), h16(20), h16(22), h16(24), static_cast<int32_t>(ReadU32(attr, 36)),
                   static_cast<int32_t>(ReadU32(attr, 40)), static_cast<int32_t>(ReadU32(attr, 44)), h16(48), h16(50),
                   h16(52), h16(54), h16(56), h16(58), h16(60));
      break;
  }
  std::fputc('\n', s_spuq_fp);
  s_spuq_rows++;
  s_spuq_by_kind[kind]++;
  return true;
}


// ─── SPU register access recorder (<trace>.spureg) ───────────────────
// One row per execution of a game instruction that stores the key-on
// register (KON, 0x1F801D88/0x1F801D8A), stores a voice's sample rate
// (voice base + 4) or loads a voice's envelope level (ENVX, voice base +
// 0xC), at the instruction's exact tick (before it executes): the SPU
// state a libspu call moves or reads changes at these accesses, not at the
// call's entry. Rows are written from boot, rec 0 before the anchor's
// first hit. Columns: idx rec vbl spuq kind pc addr value tick, where
// spuq is the index of the last <trace>.spuq row written (-1 when none or
// the .spuq recorder is off), kind kon|pitch|envx, addr the access's
// effective address from the instruction's base register and offset, and
// value the stored halfword (- for a load).
enum SpuregKind : uint8_t { SPUREG_KON, SPUREG_PITCH, SPUREG_ENVX, SPUREG_KINDS };
constexpr const char* kSpuregNames[SPUREG_KINDS] = {"kon", "pitch", "envx"};
std::FILE* s_spureg_fp = nullptr;
std::vector<std::pair<uint32_t, uint8_t>> s_spureg_pc;
uint32_t s_spureg_rows = 0, s_spureg_by_kind[SPUREG_KINDS] = {}, s_spureg_undecoded = 0;

bool SpuregCB(CPU::BreakpointType type, VirtualMemoryAddress pc, VirtualMemoryAddress memaddr)
{
  (void)type; (void)memaddr;
  if (!s_spureg_fp || s_complete)
    return true;
  int kind = -1;
  for (const auto& e : s_spureg_pc)
    if (e.first == pc)
      kind = e.second;
  u32 word = 0;
  if (kind < 0 || !CPU::SafeReadMemoryWord(pc, &word))
    return true;
  // sh (0x29) stores rt; lh (0x21) and lhu (0x25) load into rt.
  const uint32_t op = word >> 26;
  if (op != 0x29u && op != 0x21u && op != 0x25u)
  {
    s_spureg_undecoded++;
    return true;
  }
  const CPU::Registers& r = CPU::GetRegs();
  const uint32_t addr =
    r.r[(word >> 21) & 31u] + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(word & 0xFFFFu)));
  std::fprintf(s_spureg_fp, "%u %u %u %d %s 0x%08X 0x%08X ", s_spureg_rows, s_records_written, System::GetFrameNumber(),
               s_spuq_fp && s_spuq_rows ? static_cast<int>(s_spuq_rows - 1u) : -1, kSpuregNames[kind],
               static_cast<uint32_t>(pc), addr);
  if (op == 0x29u)
    std::fprintf(s_spureg_fp, "0x%04X", r.r[(word >> 16) & 31u] & 0xFFFFu);
  else
    std::fputc('-', s_spureg_fp);
  std::fprintf(s_spureg_fp, " %llu\n", (unsigned long long)ExactTick());
  s_spureg_rows++;
  s_spureg_by_kind[kind]++;
  return true;
}

} // namespace

bool Initialize(const std::string& path, uint32_t anchor_pc,
                uint32_t anchor_hit_target, uint32_t max_frames,
                const std::string& ram_dump_path,
                const std::string& regions_path,
                uint32_t cd_read_pc, uint32_t cd_read_cb_slot,
                uint32_t cd_control_pc,
                uint32_t vsync_pc, uint32_t srand_pc, uint32_t rand_pc,
                uint32_t watch_addr, bool cd_irq, uint32_t cd_read_pc_min,
                const uint32_t* vsync_cb_pcs, int vsync_cb_count, bool pad_cmds, uint32_t cdsync_pc,
                uint32_t sio0_addr, uint32_t vsync_skip_lo, uint32_t vsync_skip_hi, std::string* error_out) {
  if (s_fp) {
    if (error_out)
      *error_out = "already initialized";
    return false;
  }
  if (cd_read_cb_slot && !cd_read_pc) {
    if (error_out)
      *error_out = "-psxtrace-cd-read-cb-slot needs -psxtrace-cd-read-pc";
    return false;
  }
  {
    std::string err;
    if (!LoadRegionsFile(regions_path, &err)) {
      if (error_out)
        *error_out = err;
      return false;
    }
  }
  s_fp = std::fopen(path.c_str(), "wb");
  if (!s_fp) {
    if (error_out)
      *error_out = "fopen failed: " + path;
    return false;
  }
  s_anchor_pc = anchor_pc;
  s_anchor_hit_target = anchor_hit_target ? anchor_hit_target : 1;
  s_max_frames = max_frames;
  s_anchor_hits = 0;
  s_records_written = 0;
  s_armed = false;
  s_complete = false;
  s_ram_dump_path = ram_dump_path;
  s_ram_dumped = false;
  s_cd_read_pc = cd_read_pc;
  s_cd_read_cb_slot = cd_read_cb_slot;
  s_cd_read_cb_pc = 0;
  s_cd_control_pc = cd_control_pc;
  s_reads_issued = s_reads_done = s_cb_stray = 0;
  s_read_pending = false;
  WriteHeader();
  if (cd_read_pc) {
    const std::string loads_path = path + ".loads";
    s_loads_fp = std::fopen(loads_path.c_str(), "w");
    if (!s_loads_fp) {
      if (error_out)
        *error_out = "fopen failed: " + loads_path;
      std::fclose(s_fp);
      s_fp = nullptr;
      return false;
    }
    std::fprintf(s_loads_fp, "psxtrace-loads 1 build=%s cd_read_pc=0x%08X cd_read_cb_slot=0x%08X\n",
                 g_scm_hash_str, cd_read_pc, cd_read_cb_slot);
    std::fprintf(s_loads_fp, "# idx issue_rec issue_vbl issue_tick issue_line sectors dst mode "
                             "done_rec done_vbl done_tick done_line intr\n");
    if (!CPU::AddBreakpointWithCallback(CPU::BreakpointType::Execute, cd_read_pc, &CdReadCB)) {
      if (error_out)
        *error_out = "AddBreakpointWithCallback(cd_read_pc) failed";
      std::fclose(s_loads_fp);
      s_loads_fp = nullptr;
      std::fclose(s_fp);
      s_fp = nullptr;
      return false;
    }
  }
  if (cd_control_pc) {
    const std::string ctl_path = path + ".cdctl";
    s_cdctl_fp = std::fopen(ctl_path.c_str(), "w");
    if (s_cdctl_fp) {
      std::fprintf(s_cdctl_fp, "psxtrace-cdctl 1 cd_control_pc=0x%08X\n# rec vbl cmd a1\n", cd_control_pc);
      if (!CPU::AddBreakpointWithCallback(CPU::BreakpointType::Execute, cd_control_pc, &CdControlCB))
        ERROR_LOG("psxtrace: breakpoint at cd_control_pc 0x{:08X} failed", cd_control_pc);
    }
  }
  s_vsync_pc = vsync_pc;
  s_vsync_skip_lo = vsync_skip_lo; s_vsync_skip_hi = vsync_skip_hi; s_vsync_skipped = 0;
  s_vsync_calls = 0;
  VSyncResetRecorder();
  if (vsync_pc) {
    const std::string vs_path = path + ".vsync";
    s_vsync_fp = std::fopen(vs_path.c_str(), "w");
    if (!s_vsync_fp) {
      if (error_out)
        *error_out = "fopen failed: " + vs_path;
      return false;
    }
    std::fprintf(s_vsync_fp, "psxtrace-vsync 3 build=%s vsync_pc=0x%08X query_skip=0x%08X:0x%08X\n", g_scm_hash_str, vsync_pc,
                 vsync_skip_lo, vsync_skip_hi);
    std::fprintf(s_vsync_fp, "# idx rec mode entry_vbl ret_vbl entry_tick ret_tick v0 n ra\n");
    if (!CPU::AddBreakpointWithCallback(CPU::BreakpointType::Execute, vsync_pc, &VSyncCB)) {
      if (error_out)
        *error_out = "AddBreakpointWithCallback(vsync_pc) failed";
      return false;
    }
    s_vsync_cb_n = 0;
    for (int i = 0; i < vsync_cb_count && i < VSYNC_CB_MAX; i++) {
      s_vsync_cb_pc[s_vsync_cb_n++] = vsync_cb_pcs[i];
      if (!CPU::AddBreakpointWithCallback(CPU::BreakpointType::Execute, vsync_cb_pcs[i], &VSyncCbCB)) {
        if (error_out)
          *error_out = "AddBreakpointWithCallback(vsync_cb_pc) failed";
        return false;
      }
    }
  }
  s_rand_calls = 0;
  s_rand_ret_pc = 0;
  if (rand_pc) {
    const std::string rp = path + ".rand";
    s_rand_fp = std::fopen(rp.c_str(), "w");
    if (!s_rand_fp) {
      if (error_out)
        *error_out = "fopen failed: " + rp;
      return false;
    }
    std::fprintf(s_rand_fp, "psxtrace-rand 1 build=%s rand_pc=0x%08X\n# idx rec vbl value\n", g_scm_hash_str, rand_pc);
    if (!CPU::AddBreakpointWithCallback(CPU::BreakpointType::Execute, rand_pc, &RandCB)) {
      if (error_out)
        *error_out = "AddBreakpointWithCallback(rand_pc) failed";
      return false;
    }
  }
  s_cdirq_commands = 0;
  s_cdirq_interrupts = 0;
  if (cd_irq) {
    const std::string cp = path + ".cdirq";
    s_cdirq_fp = std::fopen(cp.c_str(), "w");
    if (!s_cdirq_fp) {
      if (error_out)
        *error_out = "fopen failed: " + cp;
      return false;
    }
    std::fprintf(s_cdirq_fp, "psxtrace-cdirq 2 build=%s read_pc_min=0x%08X\n"
                 "# C idx rec vbl pc cmd nparams params... | I idx rec vbl int nresp response... | R idx rec vbl pc offset value\n",
                 g_scm_hash_str, cd_read_pc_min);
    s_cdirq_reads = 0;
    s_cdirq_read_pc_min = cd_read_pc_min;
    CDROM::g_command_hook = &CdCommandHook;
    CDROM::g_interrupt_hook = &CdInterruptHook;
    CDROM::g_read_hook = &CdReadHook;
  }
  s_cdsync_calls = 0;
  s_cdsync_depth = 0;
  s_cdsync_run_open = false;
  if (cdsync_pc) {
    const std::string cp = path + ".cdsync";
    s_cdsync_fp = std::fopen(cp.c_str(), "w");
    if (!s_cdsync_fp) {
      if (error_out)
        *error_out = "fopen failed: " + cp;
      return false;
    }
    std::fprintf(s_cdsync_fp, "psxtrace-cdsync 1 build=%s cdsync_pc=0x%08X\n# idx rec vbl mode ret n ra tick\n",
                 g_scm_hash_str, cdsync_pc);
    if (!CPU::AddBreakpointWithCallback(CPU::BreakpointType::Execute, cdsync_pc, &CdSyncCB)) {
      if (error_out)
        *error_out = "AddBreakpointWithCallback(cdsync_pc) failed";
      return false;
    }
  }
  s_padcmd_count = 0;
  if (pad_cmds) {
    const std::string pp = path + ".padcmd";
    s_padcmd_fp = std::fopen(pp.c_str(), "w");
    if (!s_padcmd_fp) {
      if (error_out)
        *error_out = "fopen failed: " + pp;
      return false;
    }
    std::fprintf(s_padcmd_fp, "psxtrace-padcmd 1 build=%s\n# idx rec vbl port cmd\n", g_scm_hash_str);
    OpenpetePad::g_command_hook = &PadCommandHook;
  }
  s_sio0_addr = sio0_addr;
  s_sio0_rows = s_sio0_served = s_sio0_run_n = 0;
  if (sio0_addr) {
    const std::string sp = path + ".sio0";
    s_sio0_fp = std::fopen(sp.c_str(), "w");
    if (!s_sio0_fp) {
      if (error_out)
        *error_out = "fopen failed: " + sp;
      return false;
    }
    std::fprintf(s_sio0_fp, "psxtrace-sio0 1 build=%s addr=0x%08X\n# idx rec vbl n\n", g_scm_hash_str, sio0_addr);
    if (!CPU::AddBreakpointWithCallback(CPU::BreakpointType::Write, sio0_addr, &Sio0CB)) {
      if (error_out)
        *error_out = "AddBreakpointWithCallback(sio0_addr) failed";
      return false;
    }
  }
  s_watch_addr = watch_addr;
  s_watch_writes = 0;
  if (watch_addr) {
    const std::string wp = path + ".watch";
    s_watch_fp = std::fopen(wp.c_str(), "w");
    if (!s_watch_fp) {
      if (error_out)
        *error_out = "fopen failed: " + wp;
      return false;
    }
    std::fprintf(s_watch_fp, "psxtrace-watch 2 build=%s addr=0x%08X\n# idx rec vbl pc old new tick\n", g_scm_hash_str, watch_addr);
    if (!CPU::AddBreakpointWithCallback(CPU::BreakpointType::Write, watch_addr, &WatchCB)) {
      if (error_out)
        *error_out = "AddBreakpointWithCallback(watch_addr) failed";
      return false;
    }
  }
  s_srand_calls = 0;
  if (srand_pc) {
    const std::string sr_path = path + ".srand";
    s_srand_fp = std::fopen(sr_path.c_str(), "w");
    if (!s_srand_fp) {
      if (error_out)
        *error_out = "fopen failed: " + sr_path;
      return false;
    }
    std::fprintf(s_srand_fp, "psxtrace-srand 1 build=%s srand_pc=0x%08X\n# idx rec vbl seed\n", g_scm_hash_str, srand_pc);
    if (!CPU::AddBreakpointWithCallback(CPU::BreakpointType::Execute, srand_pc, &SrandCB)) {
      if (error_out)
        *error_out = "AddBreakpointWithCallback(srand_pc) failed";
      return false;
    }
  }
  if (!CPU::AddBreakpointWithCallback(CPU::BreakpointType::Execute,
                                      anchor_pc, &BreakpointCB)) {
    if (error_out)
      *error_out = "AddBreakpointWithCallback failed";
    std::fclose(s_fp);
    s_fp = nullptr;
    return false;
  }
  INFO_LOG("psxtrace: armed pc=0x{:08X} hit={} max_frames={} regions={} table=0x{:016X} build={} -> {}",
           anchor_pc, s_anchor_hit_target, s_max_frames, s_regions.size(), s_table_hash,
           g_scm_hash_str, path);
  return true;
}

bool InitializeVblPos(const std::string& trace_path, const std::string& library_path, const std::string& self_entry_path,
                      std::string* error_out)
{
  if (!s_fp || !s_vsync_fp)
  {
    *error_out = "vblpos: needs -psxtrace and -psxtrace-vsync-pc (a waited VSync is read off the VSync recorder)";
    return false;
  }
  s_game_code.assign(RAM_SIZE / 4u, 1u);
  s_self_entry.assign(RAM_SIZE / 4u, 0u);
  uint32_t lib_rows = 0, self_rows = 0;
  if (!LoadSpans(library_path, s_game_code, 0u, true, &lib_rows, error_out))
    return false;
  if (!self_entry_path.empty() && !LoadSpans(self_entry_path, s_self_entry, 1u, false, &self_rows, error_out))
    return false;
  const std::string path = trace_path + ".vblpos";
  s_vblpos_fp = std::fopen(path.c_str(), "w");
  if (!s_vblpos_fp)
  {
    *error_out = "vblpos: cannot open " + path;
    return false;
  }
  s_pc_count.assign(RAM_SIZE / 4u, 0u);
  s_pc_epoch.assign(RAM_SIZE / 4u, 0u);
  s_hw_serial.assign(RAM_SIZE / 4u, 0u);
  s_ld_count.assign(RAM_SIZE / 4u, 0u);
  s_ld_epoch.assign(RAM_SIZE / 4u, 0u);
  s_exc_stores.clear();
  s_exc_serial = 0;
  s_epoch = 1;
  s_exc_depth = 0;
  s_insns = 0;
  s_prev_game = false;
  s_call_phase = 0;
  s_shadow_n = 0;
  s_vblpos_open.clear();
  s_vblpos_exc.active = false;
  std::fprintf(s_vblpos_fp, "psxtrace-vblpos 2 build=%s library_spans=%u self_entry=%u\n", g_scm_hash_str, lib_rows,
               self_rows);
  std::fprintf(s_vblpos_fp, "# idx rec vbl via epc entry insns t t_rec k tick rt rk\n");
  CPU::g_instruction_hook = &VblPosInstructionHook;
  CPU::g_exception_hook = &VblPosExceptionHook;
  return true;
}

// Card transfers (<trace>.card): per transfer the card driver began, the
// reference's tick offset of its start from the VBLANK that started it, and
// the tick of every step after the start. A step is one SIO0 interrupt the
// driver served: mcHandler's store of its byte counter (0x80007514, values
// 2 onward, one per handler state) or the fast-track patch's store of its
// byte count (0x800072F0, values 1 onward, one per data byte). A row is
// written when the transfer ends (a zero store to the active byte), when the
// next one starts, or at shutdown.
constexpr uint32_t kCardCounterAddr = 0x80007514u;
constexpr uint32_t kCardFtCountAddr = 0x800072F0u;
std::FILE* s_card_fp = nullptr;
uint32_t s_card_rows = 0;
bool s_card_live = false;
uint32_t s_card_rec = 0, s_card_vbl = 0;
uint64_t s_card_start = 0, s_card_off = 0;
std::vector<uint32_t> s_card_steps;

// The value the store at the current instruction writes (the breakpoint
// fires before the store): rt, cut to the store's width.
bool CardStoreValue(uint32_t* value_out)
{
  const u32 store_pc = CPU::g_state.current_instruction_pc;
  u32 inst = 0;
  if (!CPU::SafeReadMemoryWord(store_pc, &inst))
    return false;
  const u32 rt = CPU::GetRegs().r[(inst >> 16) & 31u];
  switch (inst >> 26)
  {
    case 0x28: *value_out = rt & 0xFFu; return true;   // sb
    case 0x29: *value_out = rt & 0xFFFFu; return true; // sh
    case 0x2B: *value_out = rt; return true;           // sw
    default: return false;
  }
}

void CardFlushRow()
{
  if (!s_card_live)
    return;
  std::fprintf(s_card_fp, "%u %u %u %llu %zu", s_card_rows++, s_card_rec, s_card_vbl,
               static_cast<unsigned long long>(s_card_off), s_card_steps.size());
  for (const uint32_t t : s_card_steps)
    std::fprintf(s_card_fp, " %u", t);
  std::fputc('\n', s_card_fp);
  s_card_live = false;
  s_card_steps.clear();
}

bool CardStepCB(CPU::BreakpointType type, VirtualMemoryAddress pc, VirtualMemoryAddress memaddr)
{
  (void)type; (void)pc;
  uint32_t v = 0;
  if (!s_card_fp || !s_card_live || !CardStoreValue(&v))
    return true;
  const bool counter = (memaddr & 0x0FFFFFFFu) == (kCardCounterAddr & 0x0FFFFFFFu);
  if (counter ? v >= 2u : v >= 1u)
    s_card_steps.push_back(static_cast<uint32_t>(ExactTick() - s_card_start));
  return true;
}

bool CardActiveCB(CPU::BreakpointType type, VirtualMemoryAddress pc, VirtualMemoryAddress memaddr)
{
  (void)type; (void)pc; (void)memaddr;
  uint32_t v = 0;
  if (!s_card_fp || !CardStoreValue(&v))
    return true;
  CardFlushRow();
  if ((v & 0xFFu) == 0)
    return true;
  s_card_live = true;
  s_card_rec = s_records_written;
  s_card_vbl = System::GetFrameNumber();
  s_card_start = ExactTick();
  s_card_off = s_card_start - System::GetFrameStartTick();
  return true;
}

// Root counter 2 read recorder.
std::FILE* s_rcnt2_fp = nullptr;
uint32_t s_rcnt2_rows = 0;
uint32_t s_rcnt2_pcs[8];
uint32_t s_rcnt2_npcs = 0;

void Rcnt2ReadHook(u32 timer, u32 value)
{
  if (!s_rcnt2_fp || timer != 2)
    return;
  const u32 pc = CPU::g_state.current_instruction_pc;
  bool listed = false;
  for (uint32_t i = 0; i < s_rcnt2_npcs; i++)
    listed = listed || s_rcnt2_pcs[i] == pc;
  if (!listed)
    return;
  std::fprintf(s_rcnt2_fp, "%u %u %u 0x%08X 0x%04X\n", s_rcnt2_rows++, s_records_written, System::GetFrameNumber(),
               CPU::g_state.current_instruction_pc, value & 0xFFFFu);
}

bool InitializeRcnt2(const std::string& trace_path, const uint32_t* pcs, uint32_t npcs, std::string* error_out)
{
  if (npcs == 0 || npcs > 8)
  {
    *error_out = "rcnt2: needs 1 to 8 instruction addresses";
    return false;
  }
  s_rcnt2_npcs = npcs;
  for (uint32_t i = 0; i < npcs; i++)
    s_rcnt2_pcs[i] = pcs[i];
  if (!s_fp)
  {
    *error_out = "rcnt2: needs -psxtrace";
    return false;
  }
  const std::string path = trace_path + ".rcnt2";
  s_rcnt2_fp = std::fopen(path.c_str(), "w");
  if (!s_rcnt2_fp)
  {
    *error_out = "rcnt2: cannot open " + path;
    return false;
  }
  s_rcnt2_rows = 0;
  std::fprintf(s_rcnt2_fp, "psxtrace-rcnt2 1 build=%s pcs=%u\n# idx rec vbl pc value\n", g_scm_hash_str, npcs);
  Timers::g_count_read_hook = &Rcnt2ReadHook;
  return true;
}

bool InitializeWords(const std::string& trace_path, uint32_t addr, uint32_t count, std::string* error_out)
{
  if (!s_fp)
  {
    *error_out = "words: needs -psxtrace";
    return false;
  }
  if (count == 0 || count > 64 || (addr & 3u) != 0)
  {
    *error_out = "words: needs a word-aligned address and 1 to 64 words";
    return false;
  }
  const std::string path = trace_path + ".words";
  s_words_fp = std::fopen(path.c_str(), "w");
  if (!s_words_fp)
  {
    *error_out = "words: cannot open " + path;
    return false;
  }
  s_words_addr = addr;
  s_words_count = count;
  std::fprintf(s_words_fp, "psxtrace-words 1 build=%s addr=0x%08X count=%u\n# rec vbl word...\n", g_scm_hash_str, addr,
               count);
  return true;
}

bool InitializeCard(const std::string& trace_path, uint32_t active_addr, std::string* error_out)
{
  if (!s_fp)
  {
    *error_out = "card: needs -psxtrace";
    return false;
  }
  const std::string path = trace_path + ".card";
  s_card_fp = std::fopen(path.c_str(), "w");
  if (!s_card_fp)
  {
    *error_out = "card: cannot open " + path;
    return false;
  }
  s_card_rows = 0;
  s_card_live = false;
  s_card_steps.clear();
  std::fprintf(s_card_fp, "psxtrace-card 2 build=%s active=0x%08X counter=0x%08X ft=0x%08X\n# idx rec vbl off n step...\n",
               g_scm_hash_str, active_addr, kCardCounterAddr, kCardFtCountAddr);
  if (!CPU::AddBreakpointWithCallback(CPU::BreakpointType::Write, active_addr, &CardActiveCB) ||
      !CPU::AddBreakpointWithCallback(CPU::BreakpointType::Write, kCardCounterAddr, &CardStepCB) ||
      !CPU::AddBreakpointWithCallback(CPU::BreakpointType::Write, kCardFtCountAddr, &CardStepCB))
  {
    *error_out = "card: breakpoint failed";
    return false;
  }
  return true;
}

bool InitializeSpuq(const std::string& trace_path, const uint32_t pcs[5], std::string* error_out)
{
  if (!s_fp)
  {
    *error_out = "spuq: needs -psxtrace";
    return false;
  }
  const std::string path = trace_path + ".spuq";
  s_spuq_fp = std::fopen(path.c_str(), "w");
  if (!s_spuq_fp)
  {
    *error_out = "spuq: cannot open " + path;
    return false;
  }
  std::fprintf(s_spuq_fp, "psxtrace-spuq 1 build=%s setkey=0x%08X keystatus=0x%08X allkeys=0x%08X voiceattr=0x%08X\n",
               g_scm_hash_str, pcs[0], pcs[1], pcs[2], pcs[3]);
  std::fprintf(s_spuq_fp, "# idx rec vbl kind pc ra tick args (setkey: on voices; keystatus: voice; allkeys: buf; "
                          "voiceattr: voice mask pitch note sample_note a_mode s_mode r_mode ar dr sr rr sl adsr1 adsr2");
  if (pcs[SPUQ_SPUWRITE])
    std::fprintf(s_spuq_fp, "; spuwrite=0x%08X: src size n v:start:cur:counter...", pcs[SPUQ_SPUWRITE]);
  std::fprintf(s_spuq_fp, ")\n");
  for (int i = 0; i < SPUQ_KINDS; i++)
  {
    s_spuq_pc[i] = pcs[i];
    if (pcs[i] && !CPU::AddBreakpointWithCallback(CPU::BreakpointType::Execute, pcs[i], &SpuqCB))
    {
      *error_out = "spuq: breakpoint failed";
      return false;
    }
  }
  return true;
}

bool InitializeSpureg(const std::string& trace_path, const std::vector<std::pair<uint32_t, uint8_t>>& pcs,
                      std::string* error_out)
{
  if (!s_fp)
  {
    *error_out = "spureg: needs -psxtrace";
    return false;
  }
  const std::string path = trace_path + ".spureg";
  s_spureg_fp = std::fopen(path.c_str(), "w");
  if (!s_spureg_fp)
  {
    *error_out = "spureg: cannot open " + path;
    return false;
  }
  std::fprintf(s_spureg_fp, "psxtrace-spureg 1 build=%s", g_scm_hash_str);
  for (const auto& e : pcs)
    std::fprintf(s_spureg_fp, " %s=0x%08X", kSpuregNames[e.second], e.first);
  std::fprintf(s_spureg_fp, "\n# idx rec vbl spuq kind pc addr value tick\n");
  s_spureg_pc = pcs;
  for (const auto& e : pcs)
  {
    if (!CPU::AddBreakpointWithCallback(CPU::BreakpointType::Execute, e.first, &SpuregCB))
    {
      *error_out = "spureg: breakpoint failed";
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// GTE log
// ---------------------------------------------------------------------------
namespace {
std::FILE* s_gte_fp = nullptr;
uint32_t s_gte_lo = 0, s_gte_hi = 0;
uint64_t s_gte_rows = 0;

struct DecodedGTE {
  int16_t V[3][3];
  int16_t RT[3][3];
  int32_t TR[3];
  int32_t OFX, OFY;
  uint16_t H;
  int16_t DQA;
  int32_t DQB;
  int32_t MAC[4];
  int16_t IR[4];
  uint16_t SZ[4];
  uint32_t SXY[3];
  uint32_t RGB[3];
  uint32_t FLAG;
};

// Same field extraction as openpete's decode_gte (psxrecomp runner gte.cpp),
// over the 32 data and 32 control words.
void DecodeGTE(DecodedGTE& d, const u32* CP2D, const u32* CP2C)
{
  auto lo16 = [](u32 w) { return (int16_t)(w & 0xFFFFu); };
  auto hi16 = [](u32 w) { return (int16_t)((w >> 16) & 0xFFFFu); };
  d.V[0][0] = lo16(CP2D[0]); d.V[0][1] = hi16(CP2D[0]); d.V[0][2] = lo16(CP2D[1]);
  d.V[1][0] = lo16(CP2D[2]); d.V[1][1] = hi16(CP2D[2]); d.V[1][2] = lo16(CP2D[3]);
  d.V[2][0] = lo16(CP2D[4]); d.V[2][1] = hi16(CP2D[4]); d.V[2][2] = lo16(CP2D[5]);
  d.RT[0][0] = lo16(CP2C[0]); d.RT[0][1] = hi16(CP2C[0]); d.RT[0][2] = lo16(CP2C[1]);
  d.RT[1][0] = hi16(CP2C[1]); d.RT[1][1] = lo16(CP2C[2]); d.RT[1][2] = hi16(CP2C[2]);
  d.RT[2][0] = lo16(CP2C[3]); d.RT[2][1] = hi16(CP2C[3]); d.RT[2][2] = lo16(CP2C[4]);
  d.TR[0] = (int32_t)CP2C[5]; d.TR[1] = (int32_t)CP2C[6]; d.TR[2] = (int32_t)CP2C[7];
  d.OFX = (int32_t)CP2C[24]; d.OFY = (int32_t)CP2C[25];
  d.H = (uint16_t)(CP2C[26] & 0xFFFFu);
  d.DQA = lo16(CP2C[27]); d.DQB = (int32_t)CP2C[28];
  d.MAC[0] = (int32_t)CP2D[24]; d.MAC[1] = (int32_t)CP2D[25];
  d.MAC[2] = (int32_t)CP2D[26]; d.MAC[3] = (int32_t)CP2D[27];
  d.IR[0] = lo16(CP2D[8]); d.IR[1] = lo16(CP2D[9]);
  d.IR[2] = lo16(CP2D[10]); d.IR[3] = lo16(CP2D[11]);
  d.SZ[0] = (uint16_t)(CP2D[16] & 0xFFFFu);
  d.SZ[1] = (uint16_t)(CP2D[17] & 0xFFFFu);
  d.SZ[2] = (uint16_t)(CP2D[18] & 0xFFFFu);
  d.SZ[3] = (uint16_t)(CP2D[19] & 0xFFFFu);
  d.SXY[0] = CP2D[12]; d.SXY[1] = CP2D[13]; d.SXY[2] = CP2D[14];
  d.RGB[0] = CP2D[20]; d.RGB[1] = CP2D[21]; d.RGB[2] = CP2D[22];
  d.FLAG = CP2C[31];
}

void EmitGTE(const DecodedGTE& d, const char* op, int vidx, u32 frame, u32 pc)
{
  int vi = (vidx < 0 || vidx > 2) ? 0 : vidx;
  std::fprintf(s_gte_fp,
               "%u,ds,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u,%d,%d,"
               "%d,%d,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X\n",
               frame, op, vidx, d.V[vi][0], d.V[vi][1], d.V[vi][2], d.TR[0], d.TR[1], d.TR[2],
               d.RT[0][0], d.RT[0][1], d.RT[0][2], d.RT[1][0], d.RT[1][1], d.RT[1][2], d.RT[2][0],
               d.RT[2][1], d.RT[2][2], d.OFX, d.OFY, (unsigned)d.H, d.DQA, d.DQB, d.MAC[0], d.MAC[1],
               d.MAC[2], d.MAC[3], d.IR[0], d.IR[1], d.IR[2], d.IR[3], (unsigned)d.SZ[0],
               (unsigned)d.SZ[1], (unsigned)d.SZ[2], (unsigned)d.SZ[3], d.SXY[0], d.SXY[1], d.SXY[2],
               d.RGB[0], d.RGB[1], d.RGB[2], d.FLAG, pc);
  s_gte_rows++;
}

// The row set openpete's emit_op_rows writes per instruction: the same
// post-state, once per vertex for the triple ops.
void GTETraceHook(u32 inst_bits, const u32 pre_regs[64])
{
  (void)pre_regs;
  const u32 frame = System::GetFrameNumber();
  if (frame < s_gte_lo || frame >= s_gte_hi)
    return;
  u32 post[64];
  for (u32 i = 0; i < 64; i++)
    post[i] = *GTE::GetRegisterPtr(i);
  DecodedGTE d;
  DecodeGTE(d, post, post + 32);
  const u8 func = static_cast<u8>(inst_bits & 0x3Fu);
  const u32 pc = CPU::g_state.current_instruction_pc;
  auto one = [&](const char* op, int v) { EmitGTE(d, op, v, frame, pc); };
  auto three = [&](const char* base) {
    char buf[16];
    for (int v = 0; v < 3; v++) { std::snprintf(buf, sizeof(buf), "%s_v%d", base, v); one(buf, v); }
  };
  switch (func) {
    case 0x01: one("RTPS", 0); break;
    case 0x30: three("RTPT"); break;
    case 0x06: one("NCLIP", -1); break;
    case 0x2D: one("AVSZ3", -1); break;
    case 0x2E: one("AVSZ4", -1); break;
    case 0x0C: one("OP", -1); break;
    case 0x10: one("DPCS", -1); break;
    case 0x11: one("INTPL", -1); break;
    case 0x12: one("MVMVA", -1); break;
    case 0x13: one("NCDS", 0); break;
    case 0x14: one("CDP", -1); break;
    case 0x16: three("NCDT"); break;
    case 0x1B: one("NCCS", 0); break;
    case 0x1C: one("CC", -1); break;
    case 0x1E: one("NCS", 0); break;
    case 0x20: three("NCT"); break;
    case 0x28: one("SQR", -1); break;
    case 0x29: one("DPCL", -1); break;
    case 0x2A: one("DPCT", -1); break;
    case 0x3D: one("GPF", -1); break;
    case 0x3E: one("GPL", -1); break;
    case 0x3F: three("NCCT"); break;
    default: one("UNK", -1); break;
  }
}

// The one GTE trace hook: the hash recorder and the CSV log, each when open.
void GTEHooks(u32 inst_bits, const u32 pre_regs[64])
{
  if (s_gteh_fp)
    GteHashHook(inst_bits, pre_regs);
  if (s_gte_fp)
    GTETraceHook(inst_bits, pre_regs);
}
} // namespace

bool InitializeGTELog(const std::string& path, uint32_t vbl_lo, uint32_t vbl_hi, std::string* error_out)
{
  if (vbl_hi <= vbl_lo) {
    *error_out = "gte log: empty VBLANK window";
    return false;
  }
  s_gte_fp = std::fopen(path.c_str(), "w");
  if (!s_gte_fp) {
    *error_out = "gte log: cannot open " + path;
    return false;
  }
  std::fprintf(s_gte_fp, "frame,engine,op,vidx,Vx,Vy,Vz,TRx,TRy,TRz,RT00,RT01,RT02,RT10,RT11,RT12,RT20,RT21,RT22,"
                         "OFX,OFY,H,DQA,DQB,MAC0,MAC1,MAC2,MAC3,IR0,IR1,IR2,IR3,SZ0,SZ1,SZ2,SZ3,SXY0,SXY1,SXY2,"
                         "RGB0,RGB1,RGB2,FLAG,PC\n");
  s_gte_lo = vbl_lo;
  s_gte_hi = vbl_hi;
  GTE::g_trace_hook = &GTEHooks;
  return true;
}

bool InitializeGTEHash(const std::string& trace_path, bool dump, uint32_t rec_lo, uint32_t rec_hi,
                       std::string* error_out)
{
  const std::string path = trace_path + ".gte";
  s_gteh_fp = std::fopen(path.c_str(), "w");
  if (!s_gteh_fp) {
    *error_out = "gte hash: cannot open " + path;
    return false;
  }
  std::fprintf(s_gteh_fp, "# psxtrace-gte 1 fold=fnv1a64-u32 view=read\n"
                          "# rec ops hash written residue vbl\n");
  if (dump) {
    if (rec_hi < rec_lo) {
      *error_out = "gte dump: empty record window";
      return false;
    }
    const std::string dpath = trace_path + ".gtedump";
    s_gted_fp = std::fopen(dpath.c_str(), "wb");
    if (!s_gted_fp) {
      *error_out = "gte dump: cannot open " + dpath;
      return false;
    }
    s_gted_lo = rec_lo;
    s_gted_hi = rec_hi;
  }
  GTE::g_trace_hook = &GTEHooks;
  return true;
}

void ShutdownGTELog()
{
  if (!s_gte_fp)
    return;
  if (!s_gteh_fp)
    GTE::g_trace_hook = nullptr;
  std::fclose(s_gte_fp);
  s_gte_fp = nullptr;
  std::fprintf(stderr, "[psxtrace] gte log: %llu rows over VBLANK [%u, %u)\n",
               (unsigned long long)s_gte_rows, s_gte_lo, s_gte_hi);
}

void Shutdown() {
  if (s_vblpos_fp) {
    CPU::g_instruction_hook = nullptr;
    CPU::g_exception_hook = nullptr;
    for (const VblPosRow& r : s_vblpos_open) {
      VblPosWriteRow(r, 0, 0, 0, false);
      s_vblpos_unresolved++;
    }
    s_vblpos_open.clear();
    std::fprintf(s_vblpos_fp,
                 "# rows=%u resolved=%u unresolved=%u vbl_irqs=%u nested=%u in_wait=%u self_entry_skipped=%u "
                 "shadow_dropped=%u records=%u read_resolved=%u\n",
                 s_vblpos_rows, s_vblpos_resolved, s_vblpos_unresolved, s_vblpos_irqs, s_vblpos_nested, s_vblpos_in_wait,
                 s_vblpos_self_skipped, s_shadow_dropped, s_records_written, s_vblpos_read_resolved);
    std::fclose(s_vblpos_fp);
    s_vblpos_fp = nullptr;
  }
  if (s_spuq_fp) {
    std::fprintf(s_spuq_fp, "# rows=%u setkey=%u keystatus=%u allkeys=%u voiceattr=%u voiceattr_skipped=%u records=%u",
                 s_spuq_rows, s_spuq_by_kind[SPUQ_SETKEY], s_spuq_by_kind[SPUQ_KEYSTATUS], s_spuq_by_kind[SPUQ_ALLKEYS],
                 s_spuq_by_kind[SPUQ_VOICEATTR], s_spuq_attr_skipped, s_records_written);
    if (s_spuq_pc[SPUQ_SPUWRITE])
      std::fprintf(s_spuq_fp, " spuwrite=%u", s_spuq_by_kind[SPUQ_SPUWRITE]);
    std::fputc('\n', s_spuq_fp);
    std::fclose(s_spuq_fp);
    s_spuq_fp = nullptr;
  }
  if (s_spureg_fp) {
    std::fprintf(s_spureg_fp, "# rows=%u kon=%u pitch=%u envx=%u undecoded=%u records=%u\n", s_spureg_rows,
                 s_spureg_by_kind[SPUREG_KON], s_spureg_by_kind[SPUREG_PITCH], s_spureg_by_kind[SPUREG_ENVX],
                 s_spureg_undecoded, s_records_written);
    std::fclose(s_spureg_fp);
    s_spureg_fp = nullptr;
  }
  if (s_gteh_fp) {
    std::fprintf(s_gteh_fp, "# rows=%u ops=%llu rom_ops=%llu records=%u\n", s_gteh_rows,
                 (unsigned long long)s_gteh_ops_total, (unsigned long long)s_gteh_rom_ops, s_records_written);
    std::fclose(s_gteh_fp);
    s_gteh_fp = nullptr;
    if (!s_gte_fp)
      GTE::g_trace_hook = nullptr;
    INFO_LOG("psxtrace: gte hash rows={} ops={} rom_ops={}", s_gteh_rows, s_gteh_ops_total, s_gteh_rom_ops);
  }
  if (s_gted_fp) {
    std::fclose(s_gted_fp);
    s_gted_fp = nullptr;
    INFO_LOG("psxtrace: gte dump rows={} over records [{}, {}]", s_gted_rows, s_gted_lo, s_gted_hi);
  }
  if (s_vsync_fp) {
    VSyncFlushRun();
    std::fprintf(s_vsync_fp, "# calls=%u records=%u pending=%d dropped=%u query_skipped=%u\n", s_vsync_calls, s_records_written,
                 s_vsync_depth, s_vsync_dropped, s_vsync_skipped);
    std::fclose(s_vsync_fp);
    s_vsync_fp = nullptr;
  }
  CardFlushRow(); // a transfer the capture ended inside
  if (s_watch_fp) {
    std::fprintf(s_watch_fp, "# writes=%u records=%u\n", s_watch_writes, s_records_written);
    std::fclose(s_watch_fp);
    s_watch_fp = nullptr;
  }
  if (s_cdsync_fp) {
    CdSyncFlushRun();
    std::fprintf(s_cdsync_fp, "# calls=%u records=%u\n", s_cdsync_calls, s_records_written);
    std::fclose(s_cdsync_fp);
    s_cdsync_fp = nullptr;
  }
  if (s_words_fp) {
    std::fprintf(s_words_fp, "# records=%u\n", s_records_written);
    std::fclose(s_words_fp);
    s_words_fp = nullptr;
  }
  if (s_rcnt2_fp) {
    Timers::g_count_read_hook = nullptr;
    std::fprintf(s_rcnt2_fp, "# reads=%u records=%u\n", s_rcnt2_rows, s_records_written);
    std::fclose(s_rcnt2_fp);
    s_rcnt2_fp = nullptr;
  }
  if (s_card_fp) {
    std::fprintf(s_card_fp, "# starts=%u records=%u\n", s_card_rows, s_records_written);
    std::fclose(s_card_fp);
    s_card_fp = nullptr;
  }
  if (s_sio0_fp) {
    Sio0FlushRun();
    std::fprintf(s_sio0_fp, "# served=%u records=%u\n", s_sio0_served, s_records_written);
    std::fclose(s_sio0_fp);
    s_sio0_fp = nullptr;
  }
  if (s_padcmd_fp) {
    OpenpetePad::g_command_hook = nullptr;
    std::fprintf(s_padcmd_fp, "# commands=%u records=%u\n", s_padcmd_count, s_records_written);
    std::fclose(s_padcmd_fp);
    s_padcmd_fp = nullptr;
  }
  if (s_cdirq_fp) {
    CDROM::g_command_hook = nullptr;
    CDROM::g_interrupt_hook = nullptr;
    CDROM::g_read_hook = nullptr;
    std::fprintf(s_cdirq_fp, "# commands=%u interrupts=%u reads=%u records=%u\n", s_cdirq_commands, s_cdirq_interrupts,
                 s_cdirq_reads, s_records_written);
    std::fclose(s_cdirq_fp);
    s_cdirq_fp = nullptr;
  }
  if (s_rand_fp) {
    std::fprintf(s_rand_fp, "# calls=%u records=%u\n", s_rand_calls, s_records_written);
    std::fclose(s_rand_fp);
    s_rand_fp = nullptr;
  }
  if (s_srand_fp) {
    std::fprintf(s_srand_fp, "# calls=%u records=%u\n", s_srand_calls, s_records_written);
    std::fclose(s_srand_fp);
    s_srand_fp = nullptr;
  }
  if (!s_fp)
    return;
  // Patch frame_count at offset 12.
  if (std::fseek(s_fp, 12, SEEK_SET) == 0) {
    Write32(s_records_written);
  }
  std::fclose(s_fp);
  s_fp = nullptr;
  if (s_loads_fp) {
    if (s_read_pending) {
      std::fprintf(s_loads_fp, "%u %u %u %llu %u %u 0x%08X 0x%02X - - - - pending\n",
                   s_reads_issued - 1, s_pending.issue_rec, s_pending.issue_vbl,
                   (unsigned long long)s_pending.issue_tick, s_pending.issue_line,
                   s_pending.sectors, s_pending.dst, s_pending.mode);
    }
    std::fprintf(s_loads_fp, "# reads issued=%u done=%u stray_callbacks=%u records=%u\n",
                 s_reads_issued, s_reads_done, s_cb_stray, s_records_written);
    std::fclose(s_loads_fp);
    s_loads_fp = nullptr;
    INFO_LOG("psxtrace: reads issued={} done={} stray_callbacks={}",
             s_reads_issued, s_reads_done, s_cb_stray);
  }
  if (s_cdctl_fp) {
    std::fclose(s_cdctl_fp);
    s_cdctl_fp = nullptr;
  }
  INFO_LOG("psxtrace: closed (records={}, anchor_hits={})",
           s_records_written, s_anchor_hits);
}

bool IsComplete() { return s_complete; }
uint32_t GetRecordsWritten() { return s_records_written; }
uint32_t GetAnchorHitCount() { return s_anchor_hits; }

} // namespace PSXTraceCapture
