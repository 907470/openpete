-- pcsx-redux RAM dumper for openpete <-> PCSX-Redux comparison.
--
-- Arms an Exec breakpoint at CFG.entry_pc; once that fires (game's main has
-- been entered), counts CFG.frames_after_entry VSyncs, then writes the full
-- 2MB main RAM to CFG.output_path and quits. Pair with openpete's
-- --ram-dump-on-frame / --ram-dump-output for byte-diff validation.
--
-- Default frames_after_entry = 1 matches openpete's `--ram-dump-on-frame 0`,
-- which fires after frame 0's game code yields at its first VSync (so main
-- has executed up to its first VSync on both sides at the dump instant).
-- Crank to e.g. 850 to dump at title-screen idle without writing a movie.
--
-- Usage:
--   1. Launch pcsx-redux with --debugger and --iso /path/to/SCUS-94228.cue
--      (or load disc via the GUI; breakpoints fire only under
--      --debugger).
--   2. Open the Lua console, paste this file (or use File > Open Lua…),
--      hit Enter to arm.
--   3. Boot the game. Once main is entered + the configured number of
--      VSyncs have ticked, dump lands at CFG.output_path, emulator quits.
--
-- Tweak CFG below for a different entry PC / vsync count / output path.

-- Env-var overrides (no edit-and-rerun cycle):
--   PCSXR_DUMP_PC       hex PC, e.g. "0x80012204"
--   PCSXR_DUMP_OUT      RAM output path (2 MB binary)
--   PCSXR_DUMP_CPU      CPU output path (528-byte binary, 'CSv1' format
--                       consumed by openpete's --seed-cpu). Optional —
--                       omit to skip CPU dump.
--   PCSXR_DUMP_VSYNCS   integer count (default 0 = immediate)
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
    entry_pc            = envnum('PCSXR_DUMP_PC', 0x8003385c),  -- GamestateUpdate
    frames_after_entry  = envnum('PCSXR_DUMP_VSYNCS', 0),
    output_path         = envstr('PCSXR_DUMP_OUT', '/tmp/pcsxr_ram.bin'),
    cpu_output_path     = envstr('PCSXR_DUMP_CPU', nil),
    quit_after_dump     = true,
}

local RAM_SIZE = 2 * 1024 * 1024

local ffi = require 'ffi'
local main_entered = false
local vsync_count  = 0
local dumped       = false

-- Little-endian uint32 writer (LuaJIT bit ops).
local function w32(fp, v)
    v = v or 0
    fp:write(string.char(
        bit.band(v, 0xFF),
        bit.band(bit.rshift(v, 8), 0xFF),
        bit.band(bit.rshift(v, 16), 0xFF),
        bit.band(bit.rshift(v, 24), 0xFF)))
end

-- Pull 32 uint32s from a register file accessor with multiple fallbacks.
-- PCSX-Redux's regs API exposes COP/GTE banks as `bank.r[0..31]`; GPRs are
-- `regs.GPR.r[0..31]`. If a bank is unavailable, write zeros.
local function bank32(bank)
    local out = {}
    for i = 0, 31 do
        out[#out + 1] = (bank and bank.r and bank.r[i]) or 0
    end
    return out
end

local function dump_cpu(pc_at_dump)
    if not CFG.cpu_output_path then return end
    local fp, err = io.open(CFG.cpu_output_path, 'wb')
    if not fp then
        print(string.format('[ram-dump] cpu-dump open %s failed: %s',
                            CFG.cpu_output_path, err))
        return
    end
    local r   = PCSX.getRegisters()
    local gpr = bank32(r.GPR)
    local cp0 = bank32(r.CP0)
    local cp2d = bank32(r.CP2D)
    local cp2c = bank32(r.CP2C)
    -- 4-byte magic 'CSv1' (LE 0x31765343)
    w32(fp, 0x31765343)
    for i = 1, 32 do w32(fp, gpr[i]) end
    -- pc at the dump instant: prefer the BP's addr (pc_at_dump arg) since
    -- regs.GPR.n.pc may not have been committed by the interpreter yet.
    w32(fp, pc_at_dump or (r.GPR.n and r.GPR.n.pc) or 0)
    w32(fp, (r.GPR.n and r.GPR.n.hi) or r.hi or 0)
    w32(fp, (r.GPR.n and r.GPR.n.lo) or r.lo or 0)
    for i = 1, 32 do w32(fp, cp0[i])  end
    for i = 1, 32 do w32(fp, cp2d[i]) end
    for i = 1, 32 do w32(fp, cp2c[i]) end
    fp:close()
    print(string.format('[ram-dump] cpu wrote 528 bytes to %s '
        ..'(pc=0x%08X gp=0x%08X sp=0x%08X ra=0x%08X)',
        CFG.cpu_output_path,
        pc_at_dump or 0,
        gpr[29] or 0,    -- gp = r28, 1-indexed slot 29
        gpr[30] or 0,    -- sp = r29, 1-indexed slot 30
        gpr[32] or 0))   -- ra = r31, 1-indexed slot 32
end

local function dump_ram(pc_at_dump)
    if dumped then return end
    dumped = true

    local mem = PCSX.getMemPtr()
    local fp, err = io.open(CFG.output_path, 'wb')
    if not fp then
        print(string.format('[ram-dump] open %s failed: %s',
                            CFG.output_path, err))
    else
        fp:write(ffi.string(mem, RAM_SIZE))
        fp:close()
        print(string.format(
            '[ram-dump] wrote %d bytes to %s (entry_pc=0x%08X, +%d vsyncs)',
            RAM_SIZE, CFG.output_path, CFG.entry_pc, CFG.frames_after_entry))
    end

    dump_cpu(pc_at_dump)

    if CFG.quit_after_dump then
        PCSX.quit(0)
    end
end

PCSX.addBreakpoint(CFG.entry_pc, 'Exec', 4, 'main-entry',
    function(addr)
        if not main_entered then
            main_entered = true
            print(string.format('[ram-dump] entry hit at PC=0x%08X, '
                                ..'will dump after %d vsync(s)',
                                CFG.entry_pc, CFG.frames_after_entry))
            -- frames_after_entry == 0 means "dump immediately at entry,
            -- before any of main's instructions have executed."
            if CFG.frames_after_entry <= 0 then
                dump_ram(addr or CFG.entry_pc)
            end
        end
        return true  -- keep BP armed (but main_entered guard makes re-hits no-ops)
    end,
    'ram-dump-entry')

PCSX.Events.createEventListener('GPU::Vsync', function()
    if not main_entered or dumped then return end
    vsync_count = vsync_count + 1
    if vsync_count >= CFG.frames_after_entry then
        dump_ram(CFG.entry_pc)
    end
end)

print(string.format('[ram-dump] armed: entry_pc=0x%08X, +%d vsyncs, output=%s',
                    CFG.entry_pc, CFG.frames_after_entry, CFG.output_path))
