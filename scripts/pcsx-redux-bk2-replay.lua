-- pcsx-redux-bk2-replay.lua — drive PCSX-Redux's pad overrides from a
-- padstream produced by tools/bk2_to_padstream.py. Companion to
-- scripts/pcsx-redux-trace.lua: when both are loaded, the trace is captured
-- with the same input the openpete --bk2 driver replays, so .psxtrace +
-- bk2 form a closed input parity loop.
--
-- Env vars:
--   PCSXR_PADSTREAM       padstream path (required)
--   PCSXR_PADSTREAM_VERBOSE  if set non-empty, log every press/release edge
--
-- Format reference: tools/bk2_to_padstream.py header.
--
-- Caveat: PCSX-Redux's setOverride is force-press-only — clearOverride
-- returns the bit to consulting real input. In `--no-ui` headless, no
-- input device is attached so the baseline reads as released; pairing
-- with `--no-ui` is required for clean determinism.

local function envstr(name)
    local v = os.getenv(name)
    if v == nil or v == '' then return nil end
    return v
end

local PADSTREAM_PATH = envstr('PCSXR_PADSTREAM')
local VERBOSE        = envstr('PCSXR_PADSTREAM_VERBOSE') ~= nil

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

-- 16 PSX pad bits — applied per frame. We "double-buffer" by tracking the
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

-- Drive the override on every GPU::Vsync. The pcsxr trace dumper anchors
-- on a per-frame game-function BP; this runs on the underlying hardware
-- vblank. They tick at the same rate (one Vsync per frame), so the
-- frame_idx counter stays aligned. (If we ever need finer alignment, fold
-- this hook into the trace BP callback instead.)
PCSX.Events.createEventListener('GPU::Vsync', function()
    local w = frames[frame_idx + 1]  -- Lua 1-indexed
    if w == nil then
        -- Past the end of the recorded movie — hold last input. To match
        -- openpete's --bk2 default (which also holds), no clearOverride
        -- pass needed. The bk2 record itself usually ends with all
        -- released so this is mostly cosmetic.
        frame_idx = frame_idx + 1
        return
    end
    apply_word(w)
    frame_idx = frame_idx + 1
end)

print('[bk2-replay] armed: padstream-driven pad override on GPU::Vsync')
