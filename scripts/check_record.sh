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
#   2. Letterbox-freedom: the same 21:9 headless record at two --window
#      sizes must produce the SAME recorded aspect ratio, and dimensions
#      that are not the raw window dimensions — the capture scales with the
#      window (present-FBO readback) but its shape is content-derived,
#      never the window's letterboxed shape. (Headless capture forces the
#      PsyCross present, which no longer aspect-widens since the Phase-6
#      --aspect substitution-stack retirement — so the recorded aspect is
#      the PSX-derived present shape, not 21:9; the windowed native record
#      DOES widen — 21:9 records 1120x448 on a real GPU.)
#   3. --record-cfr: the opposite of run 1 — an exact NTSC rational rate, a
#      single PTS delta (perfectly even cadence), exactly --record-frames
#      frames, and no audio track.
#
# Headless runs unpaced, so wall-clock duration reflects host speed, not
# 30 fps — this test asserts frame *count* + codec + VFR structure, never
# duration. See the plan's "headless VFR caveat".
#
# Usage:
#   scripts/check_record.sh [--game SLUG] [--build-dir=build/linux-release]
#                           [--frames=300] [--out=/tmp/openpete-record-test]
#
# Every assertion is structural (codec, dimensions, PTS shape), so the run
# works on any game the resolver knows; --game selects which one (default
# spyro-1).
#
# Exit codes:
#   0  — all assertions passed
#   1  — an assertion failed
#  64  — skipped (missing binary / disc / ffprobe) — CTest SKIP_RETURN_CODE

set -uo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

GAME="$(pg_parse_game "$@")" || exit 64

BUILD_DIR="build/linux-release"
FRAMES=300
OUT_DIR="/tmp/openpete-record-test"

for arg in "$@"; do
    case "$arg" in
        --build-dir=*)   BUILD_DIR="${arg#*=}" ;;
        --frames=*)      FRAMES="${arg#*=}" ;;
        --out=*)         OUT_DIR="${arg#*=}" ;;
        --game|--game=*) ;;   # consumed by pg_parse_game above
        spyro-*)         ;;   # the two-word --game SLUG spelling
        --help|-h)       sed -n '1,34p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown arg: $arg" >&2; exit 1 ;;
    esac
done

PG_BUILD_DIR="$BUILD_DIR"
pg_resolve "$GAME"
BIN="$PG_BIN"
DISC="$PG_DISC"

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
LOG1="$OUT_DIR/plain.log"
pg_capture "$LOG1" "$OUT1" -- \
       --frame-limit "$FRAMES" --mute off --record "$OUT1"

# A build configured without libav accepts --record and warns that it is
# ignoring it. Every later assertion would then be about a file that was
# never written — and Run 3 would never even end, because its stopping
# condition is a count of RECORDED frames that stays at zero. Treat it as
# the missing prerequisite it is, alongside the binary and ffprobe. Checked
# before the missing output is reported, since that absence is this skip's
# symptom, not a failed assertion.
if grep -q "this build has no video encoder" "$LOG1"; then
    echo "SKIP: $BIN was built without a video encoder (libav* not linked)" >&2
    exit 64
fi

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

# ---- Audio: a second (AAC) stream carrying non-silent SPU mix -------------
if [[ -s "$OUT1" ]]; then
    acodec="$(probe -select_streams a -show_entries stream=codec_name -of csv=p=0 "$OUT1")"
    achan="$(probe -select_streams a -show_entries stream=channels -of csv=p=0 "$OUT1")"
    if [[ -z "$acodec" ]]; then
        bad "no audio stream (expected aac); loopback capture may have failed"
    else
        note "audio: $acodec ${achan}ch"
        # Non-silence check needs ffmpeg's volumedetect; skip if absent.
        if command -v ffmpeg >/dev/null; then
            # volumedetect logs its summary at info level, so -v info (not
            # -v error) is required to capture mean_volume.
            mean="$(ffmpeg -v info -hide_banner -i "$OUT1" -af volumedetect -f null - 2>&1 \
                    | grep -oE 'mean_volume: -?[0-9.]+ dB' | grep -oE '\-?[0-9.]+' | head -1)"
            if [[ -n "$mean" ]]; then
                # Silence is ~ -91 dB (s16) / -inf. Real game audio sits well
                # above; require louder than -60 dB.
                if awk -v m="$mean" 'BEGIN{exit !(m > -60)}'; then
                    note "audio: mean_volume ${mean} dB (non-silent)"
                else
                    bad "audio mean_volume ${mean} dB looks like silence (capture produced no signal)"
                fi
            fi
        fi
    fi
fi

# ---- Run 2: letterbox-freedom (same record at two window shapes) ----------
# Two windows with DIFFERENT aspects (4:3 and 3:2), one game aspect: the
# recorded frames must share one content-derived aspect ratio (widths agree
# within 2 px after rescaling to a common height), and neither leg's
# dimensions may be the raw window dimensions (a naive window dump would
# record 640x480 / 900x600 and its aspect would follow the window).
OUT2A="$OUT_DIR/wide-a.mkv"; OUT2B="$OUT_DIR/wide-b.mkv"
pg_capture "$OUT_DIR/wide-a.log" "$OUT2A" -- \
       --frame-limit 120 --aspect 21:9 --window 640x480 \
       --record "$OUT2A"
pg_capture "$OUT_DIR/wide-b.log" "$OUT2B" -- \
       --frame-limit 120 --aspect 21:9 --window 900x600 \
       --record "$OUT2B"

if [[ -s "$OUT2A" && -s "$OUT2B" ]]; then
    read -r WA HA < <(probe -select_streams v:0 -show_entries stream=width,height -of csv=p=0 "$OUT2A" | tr ',' ' ')
    read -r WB HB < <(probe -select_streams v:0 -show_entries stream=width,height -of csv=p=0 "$OUT2B" | tr ',' ' ')
    # cross-multiplied aspect agreement: |WA/HA - WB/HB| <= 2px at height HB
    AR_DELTA=$(( WA * HB - WB * HA )); AR_DELTA=${AR_DELTA#-}
    if (( ${WA:-0} > 0 && ${HA:-0} > 0 && ${WB:-0} > 0 && ${HB:-0} > 0 )) \
       && (( AR_DELTA <= 2 * HA )) \
       && [[ "${WA}x${HA}" != "640x480" && "${WB}x${HB}" != "900x600" ]]; then
        note "letterbox-free: ${WA}x${HA} / ${WB}x${HB} share one content aspect across 4:3 and 3:2 windows"
    else
        bad "window-shaped record: ${WA:-?}x${HA:-?} (640x480 win) vs ${WB:-?}x${HB:-?} (900x600 win)"
    fi
else
    bad "missing record output (a: $([[ -s "$OUT2A" ]] && echo ok || echo MISSING), b: $([[ -s "$OUT2B" ]] && echo ok || echo MISSING))"
fi

# ---- Run 3: --record-cfr constant-rate capture ---------------------------
# Asserts the opposite of Run 1: a fixed NTSC rational rate, a PTS grid with
# ONE delta, the exact frame budget from --record-frames, and no audio track.
# .mp4 (not .mkv) because mkv's millisecond timescale cannot represent
# 37609/1125000 exactly and would quantise the grid.
OUT3="$OUT_DIR/cfr.mp4"
LOG3="$OUT_DIR/cfr.log"
CFR_FRAMES=90
pg_capture "$LOG3" "$OUT3" -- --mute 1 \
       --record-cfr 29.913 --record-frames "$CFR_FRAMES" \
       --record "$OUT3"

if [[ -s "$OUT3" ]]; then
    rate="$(probe -select_streams v:0 -show_entries stream=r_frame_rate -of csv=p=0 "$OUT3")"
    [[ "$rate" == "1125000/37609" ]] || bad "cfr: expected r_frame_rate 1125000/37609, got '$rate'"

    dec3="$(probe -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$OUT3")"
    (( ${dec3:-0} == CFR_FRAMES )) || bad "cfr: expected $CFR_FRAMES frames, got ${dec3:-0}"

    acodec3="$(probe -select_streams a -show_entries stream=codec_name -of csv=p=0 "$OUT3")"
    [[ -z "$acodec3" ]] || bad "cfr: expected no audio stream, got '$acodec3'"

    mapfile -t PTS3 < <(probe -select_streams v:0 \
        -show_entries frame=best_effort_timestamp -of csv=p=0 "$OUT3" | grep -vE '^\s*$')
    if (( ${#PTS3[@]} >= 3 )); then
        printf '%s\n' "${PTS3[@]}" | python3 -c '
import sys
pts = sorted(int(x) for x in sys.stdin if x.strip())
deltas = {b - a for a, b in zip(pts, pts[1:])}
sys.exit(0 if len(deltas) == 1 else 1)
'
        if (( $? != 0 )); then bad "cfr: PTS grid is not evenly spaced"
        else note "cfr: ${dec3} frames @ $rate, one PTS delta, no audio"; fi
    else
        bad "cfr: too few frames to check PTS (${#PTS3[@]})"
    fi
else
    bad "cfr: no output file written ($OUT3)"
fi

echo
if (( fail == 0 )); then
    echo "check_record: PASS"
else
    echo "check_record: FAIL"
fi
exit "$fail"
