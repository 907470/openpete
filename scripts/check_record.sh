#!/usr/bin/env bash
# check_record.sh — smoke test for `--record` true-VFR video capture.
#
# Boots the headless attract demo with --record and asserts, via ffprobe,
# that the output is a playable VFR H.264 stream with the expected
# properties. Two runs:
#
#   1. Plain 4:3: a single H.264 video stream, even dimensions, the frame
#      count the runtime reported captured, and strictly-increasing
#      per-frame PTS with non-uniform deltas (true VFR — not a fixed rate).
#   2. 21:9 into a 640-wide window: the recorded width must be the *widened
#      internal* width (well over 640), NOT the window width — proving the
#      capture reads the present FBO at internal resolution, bars-free and
#      independent of window size. (See openpete-record-video.md.)
#
# Headless runs unpaced, so wall-clock duration reflects host speed, not
# 30 fps — this test asserts frame *count* + codec + VFR structure, never
# duration. See the plan's "headless VFR caveat".
#
# Usage:
#   scripts/check_record.sh [--build-dir=build/linux-release] [--frames=300]
#                           [--out=/tmp/openpete-record-test]
#
# Exit codes:
#   0  — all assertions passed
#   1  — an assertion failed
#  64  — skipped (missing binary / disc / ffprobe) — CTest SKIP_RETURN_CODE

set -uo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="build/linux-release"
FRAMES=300
OUT_DIR="/tmp/openpete-record-test"

for arg in "$@"; do
    case "$arg" in
        --build-dir=*) BUILD_DIR="${arg#*=}" ;;
        --frames=*)    FRAMES="${arg#*=}" ;;
        --out=*)       OUT_DIR="${arg#*=}" ;;
        --help|-h)     sed -n '1,27p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown arg: $arg" >&2; exit 1 ;;
    esac
done

BIN="$BUILD_DIR/games/spyro-1/openpete-spyro1"
DISC="games/spyro-1/spyro-1/build/SCUS-94228.cue"

[[ -x "$BIN"  ]] || { echo "SKIP: missing binary: $BIN" >&2; exit 64; }
[[ -f "$DISC" ]] || { echo "SKIP: missing disc: $DISC" >&2; exit 64; }
command -v ffprobe >/dev/null || { echo "SKIP: ffprobe not on PATH" >&2; exit 64; }

mkdir -p "$OUT_DIR"
fail=0
note() { printf '  %s\n' "$*"; }
bad()  { printf 'FAIL: %s\n' "$*" >&2; fail=1; }

probe() { ffprobe -v error "$@"; }

# ---- Run 1: plain 4:3, full property check -------------------------------
OUT1="$OUT_DIR/plain.mkv"
rm -f "$OUT1"
LOG1="$OUT_DIR/plain.log"
"$BIN" --headless --disc "$DISC" --run-recomp 0x80012204 \
       --frame-limit "$FRAMES" --record "$OUT1" >"$LOG1" 2>&1

[[ -s "$OUT1" ]] || { bad "no output file written ($OUT1)"; }

if [[ -s "$OUT1" ]]; then
    # Exactly one video stream, H.264.
    codec="$(probe -select_streams v -show_entries stream=codec_name -of csv=p=0 "$OUT1")"
    [[ "$codec" == "h264" ]] || bad "expected h264, got '$codec'"

    read -r W H < <(probe -select_streams v:0 -show_entries stream=width,height -of csv=p=0 "$OUT1" | tr ',' ' ')
    [[ -n "${W:-}" && -n "${H:-}" ]] || bad "could not read dimensions"
    if [[ -n "${W:-}" ]]; then
        (( W > 0 && H > 0 ))      || bad "non-positive dimensions ${W}x${H}"
        (( W % 2 == 0 && H % 2 == 0 )) || bad "odd dimensions ${W}x${H} (yuv420p needs even)"
        note "plain: ${W}x${H} $codec"
    fi

    # Frame count: the runtime logs "N frame(s) captured"; decoded count
    # must match it (nothing lost in the mux).
    cap="$(grep -oE '[0-9]+ frame\(s\) captured' "$LOG1" | grep -oE '^[0-9]+' | head -1)"
    dec="$(probe -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$OUT1")"
    [[ -n "$cap" ]] || bad "runtime did not report a capture count"
    (( ${dec:-0} > 0 )) || bad "decoded 0 frames"
    if [[ -n "$cap" && -n "$dec" ]]; then
        (( cap == dec )) || bad "captured ($cap) != decoded ($dec)"
        note "plain: $dec frames (captured=$cap)"
    fi

    # VFR: strictly-increasing PTS with at least one non-uniform delta.
    mapfile -t PTS < <(probe -select_streams v:0 \
        -show_entries frame=best_effort_timestamp_time -of csv=p=0 "$OUT1" | grep -vE '^\s*$')
    if (( ${#PTS[@]} >= 3 )); then
        python3 - "$OUT_DIR/pts.txt" <<'PY' < <(printf '%s\n' "${PTS[@]}")
import sys
pts = [float(x) for x in sys.stdin if x.strip()]
mono = all(b > a for a, b in zip(pts, pts[1:]))
deltas = [round(b - a, 6) for a, b in zip(pts, pts[1:])]
nonuniform = len(set(deltas)) > 1
sys.exit(0 if (mono and nonuniform) else 1)
PY
        if (( $? != 0 )); then bad "PTS not strictly-increasing-VFR"; else note "plain: PTS monotonic + VFR"; fi
    else
        bad "too few frames to check PTS (${#PTS[@]})"
    fi
fi

# ---- Run 2: window-independence (21:9 into a narrow window) ---------------
OUT2="$OUT_DIR/wide.mkv"
rm -f "$OUT2"
WIN_W=640
"$BIN" --headless --disc "$DISC" --run-recomp 0x80012204 \
       --frame-limit 120 --aspect 21:9 --window ${WIN_W}x480 \
       --record "$OUT2" >"$OUT_DIR/wide.log" 2>&1

if [[ -s "$OUT2" ]]; then
    read -r WW WH < <(probe -select_streams v:0 -show_entries stream=width,height -of csv=p=0 "$OUT2" | tr ',' ' ')
    # The 21:9 internal width is ~1198 here; the key assertion is that it is
    # NOT the 640-pixel window width (i.e. no letterbox-window capture).
    if (( ${WW:-0} > WIN_W + 200 )); then
        note "wide: ${WW}x${WH} (>> ${WIN_W} window — bars-free internal res)"
    else
        bad "21:9 recorded ${WW}x${WH}; expected width >> ${WIN_W} (window-size capture?)"
    fi
else
    bad "no widescreen output written"
fi

echo
if (( fail == 0 )); then
    echo "check_record: PASS"
else
    echo "check_record: FAIL"
fi
exit "$fail"
