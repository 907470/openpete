#!/usr/bin/env bash
# check_screenshot_async.sh - a player screenshot does not stall the sim
# thread long enough to reach the audio device feed.
#
# The player screenshot (the screenshot key, the injection script's `shot`
# verb) reads the presented image back on the sim thread and hands the PNG
# encode and the file write to the PNG writer thread. Written on the sim
# thread instead, the encode and the write delay the next VBL: the feed's
# producer stops pushing, and a deadline lag over 3 sim periods (or a slip)
# engages the feed's time stretch, which plays the music slower for a few
# hundred milliseconds and keeps the stretch engaged for 8 s.
#
# Run: two Spyro 3 windowed runs on a private Xvfb display in Lost Fleet
# (level 32, streamed XA music), OpenAL on its null device (no sound
# reaches a speaker), three `shot` lines at 30, 38 and 46 s, the write
# lengthened by OPENPETE_SHOT_WRITE_DELAY_MS=40 (a slow file system or an
# on-access scanner, which the writer must absorb):
#   async  the shipped path: encode and write on the writer thread
#   sync   OPENPETE_SHOT_SYNC=1: encode and write inline on the sim thread,
#          the path before the writer existed (the engagement leg)
# Instrument: the feed timeline (OPENPETE_AUDIO_FEED_LOG) `stretch` and
# `slip` events whose tag (the VBL count) lies within 180 VBL after a
# screenshot's key; the screenshot log line's thread (`on the writer` or
# `inline`); each PNG's header size against the line.
#
# Pass: both legs log three screenshots whose PNGs exist at the logged size;
# the async leg's lines say `on the writer` and no stretch or slip event
# follows any of its screenshots; the sync leg's lines say `inline` and a
# stretch or slip event follows at least one screenshot (VACUOUS otherwise:
# the stall this check guards against did not reach the feed).
#
# Usage: tests/harness/check_screenshot_async.sh [--out DIR]
# Env: OPENPETE_BIN (default build/linux-release/openpete), OPENPETE_DISC.
# Exit 0 PASS, 2 FAIL, 3 VACUOUS, 64 SKIP (xvfb-run, binary or disc missing).
set -uo pipefail

export OPENPETE_VIBRATION=0
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/sandbox.sh
source scripts/lib/sandbox.sh

OUT="build/screenshot-async"
while (( $# )); do
    case "$1" in
        --out) OUT="$2"; shift 2 ;;
        *) echo "[screenshot-async] unknown argument: $1" >&2; exit 64 ;;
    esac
done

BIN="${OPENPETE_BIN:-build/linux-release/openpete}"
DISC="${OPENPETE_DISC:-games/spyro-3/spyro-3/build/SCUS-94467.cue}"
LIMIT=4800
WINDOW=180

if ! command -v xvfb-run >/dev/null 2>&1; then
    echo "[screenshot-async] SKIP: xvfb-run not installed; the screenshot needs a windowed present" >&2
    exit 64
fi
[[ -x "$BIN" ]] || { echo "[screenshot-async] SKIP: no binary at $BIN" >&2; exit 64; }
[[ -f "$DISC" ]] || { echo "[screenshot-async] SKIP: no disc at $DISC" >&2; exit 64; }

# The window goes to the Xvfb display: SDL would pick a Wayland compositor
# (WSLg) over X11 when WAYLAND_DISPLAY is set.
unset WAYLAND_DISPLAY
export SDL_VIDEODRIVER=x11

mkdir -p "$OUT"
inject="$OUT/inject.txt"
rm -f "${inject:?}"
printf '%s\n' "30000ms shot" "38000ms shot" "46000ms shot" >"$inject"

# run_leg <tag> [VAR=VALUE...]: one windowed run in its own sandbox root.
run_leg() {
    local tag="$1"; shift
    local root
    root="$(op_sandbox_run_root)" || return 1
    rm -f "${OUT:?}/${tag:?}.log" "${OUT:?}/${tag:?}-feed.log"
    rm -rf "${OUT:?}/${tag:?}-shots"
    env OPENPETE_SANDBOX="$root" OPENPETE_CARD_DIR="$root/cards" ALSOFT_DRIVERS=null \
        OPENPETE_PRECOMPILE_FORCE=1 OPENPETE_MOUSE_INJECT="$inject" \
        OPENPETE_AUDIO_FEED_LOG="$OUT/$tag-feed.log" OPENPETE_SHOT_WRITE_DELAY_MS=40 "$@" \
        timeout -k 10 600 xvfb-run -a -s "-screen 0 1024x768x24" \
        "$BIN" --game spyro-3 --no-config --disc "$DISC" --disc-cache 0 --no-card --mute 0 \
            --background --input-lock --vibration 0 --renderer vk --aspect 4:3 \
            --skip-to-level 32 --frame-limit "$LIMIT" >"$OUT/$tag.log" 2>&1
    local rc=$?
    mkdir -p "$OUT/$tag-shots"
    find "$root" -name 'openpete-*.png' -exec cp {} "$OUT/$tag-shots/" \;
    rm -rf "${root:?}"
    return "$rc"
}

run_leg async & pa=$!
run_leg sync OPENPETE_SHOT_SYNC=1 & ps=$!
wait "$pa"; rca=$?
wait "$ps"; rcs=$?

# leg_verdict <tag> <thread phrase> <rc>: prints "<shots> <followed> <faults>".
leg_verdict() {
    python3 -I - "$OUT/$1.log" "$OUT/$1-feed.log" "$OUT/$1-shots" "$2" "$WINDOW" "$3" <<'EOF'
import os, re, struct, sys
log, feed, shots, phrase, window, rc = sys.argv[1:7]
window = int(window)
faults = []
if rc != "0":
    faults.append(f"run exited {rc}")
lines = [l for l in open(log, errors="replace") if "player screenshot" in l]
keys = []
for l in lines:
    m = re.search(r"player screenshot (\d+)x(\d+) -> (\S+) key=(\d+)", l)
    if not m:
        faults.append("unparsed screenshot line"); continue
    w, h, path, key = int(m[1]), int(m[2]), m[3], int(m[4])
    keys.append(key)
    if phrase not in l:
        faults.append(f"key {key}: not written {phrase}")
    png = os.path.join(shots, os.path.basename(path))
    try:
        with open(png, "rb") as f:
            head = f.read(24)
        pw, ph = struct.unpack(">II", head[16:24])
        if head[:8] != b"\x89PNG\r\n\x1a\n" or (pw, ph) != (w, h):
            faults.append(f"key {key}: PNG {pw}x{ph}, line {w}x{h}")
    except OSError:
        faults.append(f"key {key}: no PNG {os.path.basename(path)}")
events = []
try:
    for l in open(feed, errors="replace"):
        p = l.split()
        if len(p) > 2 and p[1] in ("stretch", "slip") and p[2].startswith("tag="):
            events.append((p[1], int(p[2][4:])))
except OSError:
    faults.append("no feed log")
followed = sum(1 for k in keys if any(k <= t <= k + window for _, t in events))
if len(keys) != 3:
    faults.append(f"{len(keys)} screenshot line(s), 3 requested")
print(len(keys), followed, "; ".join(faults) or "-")
EOF
}

read -r na fa xa < <(leg_verdict async "on the writer" "$rca")
read -r ns fs xs < <(leg_verdict sync "inline" "$rcs")
echo "[screenshot-async] async: shots=$na followed_by_stretch_or_slip=$fa faults=$xa"
echo "[screenshot-async] sync:  shots=$ns followed_by_stretch_or_slip=$fs faults=$xs"
fail=()
[[ "$xa" != "-" ]] && fail+=("async: $xa")
[[ "$xs" != "-" ]] && fail+=("sync: $xs")
[[ "$fa" != 0 ]] && fail+=("async: $fa screenshot(s) followed by a stretch or slip")
if (( ${#fail[@]} )); then
    echo "[screenshot-async] verdict=FAIL: $(IFS=';'; echo "${fail[*]}")"
    exit 2
fi
if [[ "$fs" == 0 ]]; then
    echo "[screenshot-async] verdict=VACUOUS: the inline leg's stall never reached the feed"
    exit 3
fi
echo "[screenshot-async] verdict=PASS"
exit 0
