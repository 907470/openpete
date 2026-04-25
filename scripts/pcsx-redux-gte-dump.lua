-- pcsx-redux-gte-dump.lua
--
-- Lua dumper for pcsx-redux that emits a CSV of GTE state at user-specified
-- breakpoints. Format intentionally matches the openpete trace logger
-- (third_party/psxrecomp/runner/src/gte.cpp's emit_op_rows) so output diffs
-- can be done line-for-line.
--
-- USAGE
--   1. Open pcsx-redux, load Spyro 1 (SCUS-94228), reach the frame you care
--      about (titlescreen cutscene around frame 1100).
--   2. File → Lua console → paste this script, edit the CONFIG block below
--      so the breakpoint addresses correspond to the GTE ops you want to log
--      (typically: every cop2 instruction PC inside r_pete; find them with
--      `objdump -d` on PSX.EXE filtered for `cop2` mnemonic).
--   3. Run. Each breakpoint hit decodes CP2D/CP2C, classifies the op via the
--      previously-fetched cmd word at PC, and appends a row to the output.
--   4. Compare against /tmp/openpete-gte.csv with the same frame range:
--        awk -F, '$2=="psy"' /tmp/openpete-gte.csv > /tmp/openpete-psy.csv
--        diff <(sort -k1,4 -t, /tmp/pcsx-redux-gte.csv) \
--             <(sort -k1,4 -t, /tmp/openpete-psy.csv)
--      (PsyCross's "psy" engine in openpete should match pcsx-redux's GTE
--      bit-for-bit if both implement the spec; any divergence is a bug.)
--
-- CAVEATS
--   * pcsx-redux's Lua API has no per-COP2-op event hook, so this script
--     relies on Exec breakpoints at known cop2 PCs. You must enumerate
--     them upfront — there's no "every GTE op" catch-all.
--   * The frame counter we use here (`pcsxFrame`) ticks at vsync; it may
--     be one off from openpete's `openpete_frame`. Adjust LO/HI accordingly.

------------------------------------------------------------ CONFIG
local CFG = {
    -- Output file (will be overwritten).
    csv_path     = '/tmp/pcsx-redux-gte.csv',
    -- Frame window (inclusive lower, exclusive upper). pcsx-redux's frame
    -- counter is `PCSX.getCounters().frame` if exposed; otherwise hook
    -- VBlank manually below.
    frame_lo     = 1100,
    frame_hi     = 1111,
    -- PCs of the cop2 instructions to break on. Populate from a disasm:
    --   mipsel-linux-gnu-objdump -d PSX.EXE | awk '$3 ~ /^cop2/ {print "0x"$1}'
    -- and trim to the renderer functions you care about.
    cop2_pcs     = {
        -- 0x80024054, 0x80024090, ...
    },
}
------------------------------------------------------------

local ffi = require 'ffi'
local fp  = io.open(CFG.csv_path, 'w')
fp:write(
    'frame,engine,op,vidx,'
    .. 'Vx,Vy,Vz,'
    .. 'TRx,TRy,TRz,'
    .. 'RT00,RT01,RT02,RT10,RT11,RT12,RT20,RT21,RT22,'
    .. 'OFX,OFY,H,DQA,DQB,'
    .. 'MAC0,MAC1,MAC2,MAC3,IR0,IR1,IR2,IR3,'
    .. 'SZ0,SZ1,SZ2,SZ3,'
    .. 'SXY0,SXY1,SXY2,'
    .. 'RGB0,RGB1,RGB2,'
    .. 'FLAG\n')
fp:flush()

-- pcsx-redux's psxRegisters union exposes CP2D[r], CP2C[r] as uint32_t[32].
local function decode(regs)
    local cp2d, cp2c = regs.CP2D.r, regs.CP2C.r
    local function lo16(w) return bit.tobit(bit.band(w, 0xFFFF) - (bit.band(w, 0x8000) ~= 0 and 0x10000 or 0)) end
    local function hi16(w) return bit.tobit(bit.band(bit.rshift(w, 16), 0xFFFF) - (bit.band(w, 0x80000000) ~= 0 and 0x10000 or 0)) end
    return {
        V   = {{lo16(cp2d[0]), hi16(cp2d[0]), lo16(cp2d[1])},
               {lo16(cp2d[2]), hi16(cp2d[2]), lo16(cp2d[3])},
               {lo16(cp2d[4]), hi16(cp2d[4]), lo16(cp2d[5])}},
        TR  = {bit.tobit(cp2c[5]), bit.tobit(cp2c[6]), bit.tobit(cp2c[7])},
        RT  = {{lo16(cp2c[0]), hi16(cp2c[0]), lo16(cp2c[1])},
               {hi16(cp2c[1]), lo16(cp2c[2]), hi16(cp2c[2])},
               {lo16(cp2c[3]), hi16(cp2c[3]), lo16(cp2c[4])}},
        OFX = bit.tobit(cp2c[24]), OFY = bit.tobit(cp2c[25]),
        H   = bit.band(cp2c[26], 0xFFFF),
        DQA = lo16(cp2c[27]),     DQB = bit.tobit(cp2c[28]),
        MAC = {bit.tobit(cp2d[24]), bit.tobit(cp2d[25]),
               bit.tobit(cp2d[26]), bit.tobit(cp2d[27])},
        IR  = {lo16(cp2d[8]), lo16(cp2d[9]),
               lo16(cp2d[10]), lo16(cp2d[11])},
        SZ  = {bit.band(cp2d[16], 0xFFFF), bit.band(cp2d[17], 0xFFFF),
               bit.band(cp2d[18], 0xFFFF), bit.band(cp2d[19], 0xFFFF)},
        SXY = {cp2d[12], cp2d[13], cp2d[14]},
        RGB = {cp2d[20], cp2d[21], cp2d[22]},
        FLAG = cp2c[31],
    }
end

local function emit(frame, op_label, vidx, d)
    local vi = (vidx < 0 or vidx > 2) and 0 or vidx
    fp:write(string.format(
        '%d,psy,%s,%d,'
        .. '%d,%d,%d,%d,%d,%d,'
        .. '%d,%d,%d,%d,%d,%d,%d,%d,%d,'
        .. '%d,%d,%d,%d,%d,'
        .. '%d,%d,%d,%d,%d,%d,%d,%d,'
        .. '%d,%d,%d,%d,'
        .. '0x%08X,0x%08X,0x%08X,'
        .. '0x%08X,0x%08X,0x%08X,'
        .. '0x%08X\n',
        frame, op_label, vidx,
        d.V[vi+1][1], d.V[vi+1][2], d.V[vi+1][3],
        d.TR[1], d.TR[2], d.TR[3],
        d.RT[1][1], d.RT[1][2], d.RT[1][3],
        d.RT[2][1], d.RT[2][2], d.RT[2][3],
        d.RT[3][1], d.RT[3][2], d.RT[3][3],
        d.OFX, d.OFY, d.H, d.DQA, d.DQB,
        d.MAC[1], d.MAC[2], d.MAC[3], d.MAC[4],
        d.IR[1], d.IR[2], d.IR[3], d.IR[4],
        d.SZ[1], d.SZ[2], d.SZ[3], d.SZ[4],
        d.SXY[1], d.SXY[2], d.SXY[3],
        d.RGB[1], d.RGB[2], d.RGB[3],
        d.FLAG))
    fp:flush()
end

-- Frame counter: increment in a Pause/Resume listener if pcsx-redux exposes
-- one; otherwise use a watch on the BIOS VBlank counter (0x801FFF20-ish in
-- Spyro). For now: hardcoded global ticked by user before resuming each
-- breakpoint. Replace with the proper hook in your environment.
pcsxFrame = pcsxFrame or 0

-- The cmd word for cop2 is the instruction at PC; opcode = bits 0..5 of
-- the lower 25 bits (`cmd & 0x3F`).
local function fetch_cmd(pc)
    local mem = PCSX.getMemPtr()
    local off = bit.band(pc, 0x1FFFFFFF)
    return ffi.cast('uint32_t*', mem + off)[0]
end

local function emit_op_rows(frame, func, d)
    local function pair(op, vidx) emit(frame, op, vidx, d) end
    if     func == 0x01 then pair('RTPS',    0)
    elseif func == 0x30 then pair('RTPT_v0', 0); pair('RTPT_v1', 1); pair('RTPT_v2', 2)
    elseif func == 0x06 then pair('NCLIP',  -1)
    elseif func == 0x2D then pair('AVSZ3',  -1)
    elseif func == 0x2E then pair('AVSZ4',  -1)
    end
end

for _, pc in ipairs(CFG.cop2_pcs) do
    PCSX.addBreakpoint(pc + 4, 'Exec', 4, 'gte-dump-post',
        function(addr)
            if pcsxFrame < CFG.frame_lo or pcsxFrame >= CFG.frame_hi then
                return false  -- continue
            end
            local cmd = fetch_cmd(addr - 4)
            local func = bit.band(cmd, 0x3F)
            local d = decode(PCSX.getRegisters())
            emit_op_rows(pcsxFrame, func, d)
            return false  -- continue
        end,
        string.format('gte-dump-post-0x%08X', pc))
end

print(string.format('[gte-dump] writing %s frames [%d,%d) bps=%d',
                    CFG.csv_path, CFG.frame_lo, CFG.frame_hi, #CFG.cop2_pcs))
