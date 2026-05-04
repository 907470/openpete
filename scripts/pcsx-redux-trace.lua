-- pcsx-redux .psxtrace streaming capture for the openpete oracle.
--
-- Streams a per-frame record (frame_no, pad state, pc, full 2 MB main RAM)
-- to PCSXR_TRACE_OUT, starting once entry_pc is hit. Stops after
-- PCSXR_TRACE_FRAMES frames (default 600) and quits the emulator. Output is
-- raw (uncompressed) — post-process with `python tools/psxtrace_format.py
-- compress src.raw dst.zst` to get the ~50-100 MB shipping form.
--
-- Format spec: tools/psxtrace_format.py.
--
-- Env vars:
--   PCSXR_TRACE_OUT       output path (required)
--   PCSXR_TRACE_PC        entry PC; capture starts once hit (default
--                         0x8001ED5C, GamestateDraw — once-per-frame pin)
--   PCSXR_TRACE_FRAMES    frames to capture (default 600 = ~10s @60Hz)
--   PCSXR_TRACE_HIT       capture starts on Nth hit of entry_pc (default 1)
--   PCSXR_TRACE_PAD_ADDR  RAM addr to peek for pad-state mirror (optional;
--                         leave 0 to skip — controller bytes will be 0)
--
-- Same caveats as pcsx-redux-ram-dump.lua: launch with `--debugger`, and
-- memcards are flipped off + hard-reset for parity with openpete (no
-- BIOS-side card state).

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
    entry_pc     = envnum('PCSXR_TRACE_PC',  0x8001ED5C),
    frames       = envnum('PCSXR_TRACE_FRAMES', 600),
    hit_target   = envnum('PCSXR_TRACE_HIT', 1),
    pad_addr     = envnum('PCSXR_TRACE_PAD_ADDR', 0),
}

if PCSX and PCSX.settings and PCSX.settings.emulator then
    PCSX.settings.emulator.Mcd1Inserted = false
    PCSX.settings.emulator.Mcd2Inserted = false
    if PCSX.hardResetEmulator then PCSX.hardResetEmulator() end
end

local ffi      = require 'ffi'
local RAM_SIZE = 2 * 1024 * 1024
local MAGIC    = 'PSXTRACE'
local FRMR     = 0x524D5246  -- 'FRMR'
local VERSION  = 1

local fp, err = io.open(CFG.output_path, 'wb')
if not fp then
    error(string.format('[trace] open %s failed: %s', CFG.output_path, err))
end

-- Little-endian helpers.
local function w32(v)
    v = v or 0
    fp:write(string.char(
        bit.band(v, 0xFF),
        bit.band(bit.rshift(v, 8),  0xFF),
        bit.band(bit.rshift(v, 16), 0xFF),
        bit.band(bit.rshift(v, 24), 0xFF)))
end

-- File header (32 bytes) — frame_count = 0 for streaming; finalised by
-- post-processor, openpete tolerates 0 and reads to EOF.
fp:write(MAGIC)               -- 8 bytes
w32(VERSION)                  -- version
w32(0)                        -- frame_count (streaming)
w32(RAM_SIZE)                 -- ram_size
w32(0)                        -- flags (0 = raw RAM)
w32(0)                        -- record_size_hint
w32(0)                        -- reserved

local hit_count    = 0
local capturing    = false
local frames_done  = 0
local closed       = false

local function read_pad_word(addr)
    if addr == 0 then return 0 end
    local mem = PCSX.getMemPtr()
    if not mem then return 0 end
    local off = bit.band(addr, 0x1FFFFF)
    local b0 = mem[off]
    local b1 = mem[off + 1]
    local b2 = mem[off + 2]
    local b3 = mem[off + 3]
    return b0 + bit.lshift(b1, 8) + bit.lshift(b2, 16) + bit.lshift(b3, 24)
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
    local r = PCSX.getRegisters()
    local pad1 = read_pad_word(CFG.pad_addr)
    local mem  = PCSX.getMemPtr()

    w32(FRMR)
    w32(frames_done)
    w32(pad1)
    w32(0)               -- pad2 unused
    w32(pc_at or ((r.GPR.n and r.GPR.n.pc) or 0))
    w32(RAM_SIZE)        -- ram_payload_len
    fp:write(ffi.string(mem, RAM_SIZE))

    frames_done = frames_done + 1
    if frames_done % 60 == 0 then
        print(string.format('[trace] %d frames', frames_done))
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
                print(string.format('[trace] capture armed at hit #%d, '
                                    ..'pc=0x%08X, will capture %d frames',
                                    hit_count, CFG.entry_pc, CFG.frames))
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
