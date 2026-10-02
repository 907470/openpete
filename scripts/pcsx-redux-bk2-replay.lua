-- pcsx-redux-bk2-replay.lua - drive PCSX-Redux's pad overrides from a
-- padstream produced by tools/bk2_to_padstream.py. Companion to
-- scripts/pcsx-redux-trace.lua: when both are loaded, the trace is captured
-- with the same input the openpete --bk2 driver replays, so .psxtrace +
-- bk2 form a closed input parity loop.
--
-- Env vars:
--   PCSXR_PADSTREAM            padstream path (required)
--   PCSXR_PADSTREAM_VERBOSE    if set non-empty, log every press/release edge
--   PCSXR_PADSTREAM_PC         anchor PC; bk2 frame N's input is applied on
--                              the Nth (1-indexed) hit of this PC. Defaults
--                              to 0x8001ED5C (Spyro 1 GamestateDraw) - the
--                              same default scripts/pcsx-redux-trace.lua
--                              uses, so both share an epoch.
--
-- Format reference: tools/bk2_to_padstream.py header.
--
-- Caveat: PCSX-Redux's setOverride is force-press-only - clearOverride
-- returns the bit to consulting real input. In `--no-ui` headless, no
-- input device is attached so the baseline reads as released; pairing
-- with `--no-ui` is required for clean determinism.
--
-- Anchor: rev 1 ticked on GPU::Vsync, which started counting from BIOS
-- boot. With trace capturing on a per-frame game-function BP, the two
-- counters had different epochs - bk2 frame 0 was BIOS-time, trace
-- frame 0 was post-init. Now we anchor on the same BP the trace uses,
-- so bk2 frame N drives the input that produces trace frame N's RAM
-- state (same instant in game-time on both engines).

local function envstr(name)
    local v = os.getenv(name)
    if v == nil or v == '' then return nil end
    return v
end

local function envnum(name, default)
    local v = os.getenv(name)
    if not v or v == '' then return default end
    return tonumber(v) or tonumber(v, 16) or default
end

local PADSTREAM_PATH = envstr('PCSXR_PADSTREAM')
local VERBOSE        = envstr('PCSXR_PADSTREAM_VERBOSE') ~= nil
local ANCHOR_PC      = envnum('PCSXR_PADSTREAM_PC', 0x8001ED5C)

if not PADSTREAM_PATH then
    error('[bk2-replay] PCSXR_PADSTREAM env var not set')
end

-- Load padstream. Layout: # comment lines + `<frame> <pad1_hex> <pad2_hex>`
-- one per line. Index by frame_no (1-indexed in Lua so we stash at +1).
local frames = {}    -- frames[frame_no+1] = pad1 word (uint16)
local fp, err = io.open(PADSTREAM_PATH, 'r')
if not fp then
    error(string.format('[bk2-replay] open %s: %s', PADSTREAM_PATH, err))
end
local n = 0
for line in fp:lines() do
    if line:sub(1, 1) ~= '#' and #line > 0 then
        local f, p1 = line:match('^(%d+)%s+(%x+)')
        if f then
            local fnum = tonumber(f)
            local pad1 = tonumber(p1, 16)
            -- store at fnum+1 so frame 0 lands at index 1 (Lua-friendly)
            frames[fnum + 1] = pad1
            if fnum + 1 > n then n = fnum + 1 end
        end
    end
end
fp:close()
print(string.format('[bk2-replay] loaded %d frames from %s', n, PADSTREAM_PATH))

-- 16 PSX pad bits - applied per frame. We "double-buffer" by tracking the
-- previous frame's word and only emitting set/clearOverride on edges.
-- Pad word convention: 0xFFFF = all released; bit cleared = pressed.
local pad = PCSX.SIO0.slots[1].pads[1]
local prev_word = 0xFFFF
local frame_idx = 0

-- Apply differences between prev_word and new_word. Bit cleared in word =
-- pressed in PSX convention. setOverride forces pressed; clearOverride
-- restores real-input behavior (and in --no-ui headless that == released).
local function apply_word(new_word)
    local diff = bit.bxor(prev_word, new_word)
    if diff == 0 then return end
    for b = 0, 15 do
        local m = bit.lshift(1, b)
        if bit.band(diff, m) ~= 0 then
            local now_pressed = (bit.band(new_word, m) == 0)
            if now_pressed then
                pad.setOverride(b)
                if VERBOSE then
                    print(string.format(
                        '[bk2-replay] frame=%d press bit=%d', frame_idx, b))
                end
            else
                pad.clearOverride(b)
                if VERBOSE then
                    print(string.format(
                        '[bk2-replay] frame=%d release bit=%d', frame_idx, b))
                end
            end
        end
    end
    prev_word = new_word
end

-- Drive the override on every BP hit at ANCHOR_PC - the same per-frame
-- game function the trace dumper anchors on. PCSX-Redux supports
-- multiple breakpoints at the same address, so this script and
-- pcsx-redux-trace.lua coexist; both fire on each hit and share the
-- frame_idx epoch by construction.
PCSX.addBreakpoint(ANCHOR_PC, 'Exec', 4, 'bk2-replay-anchor',
    function(addr)
        local w = frames[frame_idx + 1]  -- Lua 1-indexed
        if w == nil then
            -- Past the end of the recorded movie - hold last input.
            frame_idx = frame_idx + 1
            return true
        end
        apply_word(w)
        frame_idx = frame_idx + 1
        return true
    end,
    'psxtrace-bk2-replay')

print(string.format('[bk2-replay] armed: pad override on PC=0x%08X',
                    ANCHOR_PC))
