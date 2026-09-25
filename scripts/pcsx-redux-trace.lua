-- pcsx-redux-trace.lua — PCSX-Redux .psxtrace v2 oracle dumper.
--
-- On every hit of PCSXR_TRACE_PC (default 0x8001ED5C, GamestateDraw entry —
-- once per frame in Spyro 1) computes per-region FNV-1a-64 hashes that
-- mirror openpete's gameplay_hash_compute_ex(), and writes one record to
-- PCSXR_TRACE_OUT. Format spec: tools/psxtrace_format.py.
--
-- Hash-only is the default (small, ~150 B/frame). PCSXR_TRACE_DUMP_RAM=1
-- additionally bakes the full 2 MB RAM into each record for forensic
-- byte-attribution; ~1000× larger.
--
-- Env vars:
--   PCSXR_TRACE_OUT       output path (required)
--   PCSXR_TRACE_REGIONS   path to openpete_state_regions.gen.lua (required;
--                         emitted by tools/gen_state_regions.py during the
--                         CMake build into build/<preset>/games/<game>/gen/)
--   PCSXR_TRACE_PC        anchor PC (default 0x8001ED5C, GamestateDraw)
--   PCSXR_TRACE_FRAMES    frames to capture (default 600 = ~10s)
--   PCSXR_TRACE_HIT       capture starts on Nth hit of PC (default 1)
--   PCSXR_TRACE_PAD_ADDR  optional RAM addr to peek for pad mirror
--   PCSXR_TRACE_DUMP_RAM  if set non-empty, embed full RAM in every record
--
-- Seed RAM/CPU (for openpete --seed-ram/--seed-cpu) are captured in a
-- separate Redux run via scripts/pcsx-redux-ram-dump.lua, NOT here.
-- Two BPs at the same PC crash Redux, and folding the seed I/O into the
-- trace BP wedges the breakpoint after the first hit. The two runs are
-- deterministic so the seed state at the Nth hit matches the trace at
-- the Nth hit. See tests/harness/check_psx_oracle.sh for the orchestrated
-- workflow.
--
-- Memcards are flipped off + hard-reset for parity with openpete (no
-- BIOS-side memcard state); pcsx-redux must be launched with --debugger.

local function envnum(name, default)
    local v = os.getenv(name)
    if not v or v == '' then return default end
    return tonumber(v) or tonumber(v, 16) or default
end
local function envstr(name, default)
    local v = os.getenv(name)
    if not v or v == '' then return default end
    return v
end

local CFG = {
    output_path  = envstr('PCSXR_TRACE_OUT', '/tmp/pcsxr.psxtrace'),
    regions_path = envstr('PCSXR_TRACE_REGIONS'),
    entry_pc     = envnum('PCSXR_TRACE_PC',  0x8001ED5C),
    frames       = envnum('PCSXR_TRACE_FRAMES', 600),
    hit_target   = envnum('PCSXR_TRACE_HIT', 1),
    pad_addr     = envnum('PCSXR_TRACE_PAD_ADDR', 0),
    dump_ram     = envstr('PCSXR_TRACE_DUMP_RAM') ~= nil,
}

if not CFG.regions_path then
    error('[trace] PCSXR_TRACE_REGIONS env var not set; ' ..
          'point at build/<preset>/games/<game>/gen/openpete_state_regions.gen.lua')
end

if PCSX and PCSX.settings and PCSX.settings.emulator then
    PCSX.settings.emulator.Mcd1Inserted = false
    PCSX.settings.emulator.Mcd2Inserted = false
    if PCSX.hardResetEmulator then PCSX.hardResetEmulator() end
end

local ffi      = require 'ffi'
local bit      = require 'bit'
local RAM_SIZE = 2 * 1024 * 1024
local MAGIC    = 'PSXTRACE'
local FRMR     = 0x524D5246
local VERSION  = 2
local FLAG_HAS_RAM = 0x2

-- Load the region table emitted by tools/gen_state_regions.py.
local regions
do
    local chunk, err = loadfile(CFG.regions_path)
    if not chunk then
        error(string.format('[trace] loadfile %s: %s', CFG.regions_path, err))
    end
    regions = chunk()
    if type(regions) ~= 'table' or #regions == 0 then
        error('[trace] regions file did not return a non-empty table')
    end
end

-- Lua hash module. Try a few load strategies because the redux Lua VM may
-- be invoked from arbitrary cwds.
local H
do
    local script_dir = (CFG.regions_path:match('(.*/)') or '')
    -- The hash module lives next to this file. Use the directory of the
    -- regions path as a *fallback*, but prefer the openpete checkout
    -- inferred from the regions path: regions are built from
    -- <openpete>/build/.../gen/, so the source scripts/ dir is two levels up
    -- from gen/ → ../../scripts/.
    local guesses = {
        script_dir .. '../../scripts/?.lua',
        script_dir .. '../scripts/?.lua',
        script_dir .. '?.lua',
        './scripts/?.lua',
    }
    for _, g in ipairs(guesses) do
        package.path = package.path .. ';' .. g
    end
    H = require 'openpete_state_hash'
end

local fp, err = io.open(CFG.output_path, 'wb')
if not fp then
    error(string.format('[trace] open %s: %s', CFG.output_path, err))
end

local function w8(v) fp:write(string.char(bit.band(v, 0xFF))) end
local function w32(v)
    v = v or 0
    fp:write(string.char(
        bit.band(v, 0xFF),
        bit.band(bit.rshift(v, 8),  0xFF),
        bit.band(bit.rshift(v, 16), 0xFF),
        bit.band(bit.rshift(v, 24), 0xFF)))
end
-- uint64 → 8 LE bytes. Accepts uint64_t cdata.
local function w64(u)
    local hi32 = tonumber(ffi.cast('uint32_t', u / 0x100000000ULL))
    local lo32 = tonumber(ffi.cast('uint32_t', u % 0x100000000ULL))
    w32(lo32); w32(hi32)
end

-- Compute names blob length up front.
local names_blob_len = 0
for _, r in ipairs(regions) do
    local n = #r.name
    if n > 255 then n = 255 end
    names_blob_len = names_blob_len + 1 + n
end

local flags = CFG.dump_ram and FLAG_HAS_RAM or 0

-- Header (40 bytes fixed + names blob).
fp:write(MAGIC)              -- 8
w32(VERSION)                 -- 4 version=2
w32(0)                       -- 4 frame_count (streaming)
w32(RAM_SIZE)                -- 4
w32(flags)                   -- 4
w32(#regions)                -- 4 region_count
w32(names_blob_len)          -- 4
w32(0)                       -- 4 reserved
for _, r in ipairs(regions) do
    local nm = r.name
    if #nm > 255 then nm = nm:sub(1, 255) end
    w8(#nm)
    fp:write(nm)
end

local hit_count   = 0
local capturing   = false
local frames_done = 0
local closed      = false

local function read_pad_word(addr)
    if addr == 0 then return 0 end
    local mem = PCSX.getMemPtr()
    if not mem then return 0 end
    local off = bit.band(addr, 0x1FFFFF)
    return mem[off]
        + mem[off + 1] * 0x100
        + mem[off + 2] * 0x10000
        + mem[off + 3] * 0x1000000
end

local function close_trace()
    if closed then return end
    closed = true
    fp:close()
    -- Patch frame_count at offset 12.
    local fix = io.open(CFG.output_path, 'r+b')
    if fix then
        fix:seek('set', 12)
        fix:write(string.char(
            bit.band(frames_done, 0xFF),
            bit.band(bit.rshift(frames_done, 8),  0xFF),
            bit.band(bit.rshift(frames_done, 16), 0xFF),
            bit.band(bit.rshift(frames_done, 24), 0xFF)))
        fix:close()
    end
    print(string.format('[trace] wrote %d frames to %s, quitting',
                        frames_done, CFG.output_path))
    if PCSX and PCSX.quit then PCSX.quit(0) end
end

local function emit_record(pc_at)
    local mem  = PCSX.getMemPtr()
    local pad1 = read_pad_word(CFG.pad_addr)
    local r    = PCSX.getRegisters()
    local total, per = H.compute(mem, regions, RAM_SIZE)

    w32(FRMR)
    w32(frames_done)
    w32(pad1)
    w32(0)               -- pad2 unused for now
    w32(pc_at or ((r.GPR.n and r.GPR.n.pc) or 0))
    w64(total)
    for i = 1, #regions do w64(per[i]) end

    if CFG.dump_ram then
        w32(RAM_SIZE)
        fp:write(ffi.string(mem, RAM_SIZE))
    end

    frames_done = frames_done + 1
    if frames_done % 60 == 0 then
        print(string.format('[trace] %d frames, total_hash=%s',
                            frames_done, tostring(total)))
    end
    if frames_done >= CFG.frames then close_trace() end
end

PCSX.addBreakpoint(CFG.entry_pc, 'Exec', 4, 'trace-entry',
    function(addr)
        if closed then return true end
        hit_count = hit_count + 1
        if not capturing then
            if hit_count >= CFG.hit_target then
                capturing = true
                print(string.format(
                    '[trace] capture armed at hit #%d, pc=0x%08X, '
                    ..'will capture %d frames (regions=%d, dump_ram=%s)',
                    hit_count, CFG.entry_pc, CFG.frames,
                    #regions, tostring(CFG.dump_ram)))
            else
                return true
            end
        end
        emit_record(addr or CFG.entry_pc)
        return true
    end,
    'psxtrace-capture')

print(string.format('[trace] armed: entry_pc=0x%08X frames=%d output=%s',
                    CFG.entry_pc, CFG.frames, CFG.output_path))
