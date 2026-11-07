#!/usr/bin/env bash
# check_audio_feed_latency.sh - the audio device feed returns to its queued
# latency bound after a stretch in which the producer ran slower than real
# time.
#
# Run: audio_feed_sim (tests/unit/audio_feed_sim.cpp) drives the feed on its
# synthetic device at drift 0 without a game process, through the
# producer-speed span of OPENPETE_AUDIO_FEED_SIM (DRIFT,SEED,STALL,
# SPEED@START+DUR, seconds of producer time):
#   strong  0.8 from 10 s for 120 s, then 1.0 to 250 s
#   mild    0.96 from 10 s for 60 s, then 1.0 to 150 s
#
# Instrument: the queued latency after every pump (device frames plus the
# FIFO at the resampling ratio) from 2 s after the span ends, and the
# largest frame-to-frame step across each drop's splice; reported, no
# criterion: the pitch deviation in cents (ratio against the nominal rate)
# inside the span and after it.
# Engagement: VBLs pushed inside the span and latency samples taken; the
# driver refuses (exit 3) at zero of either.
#
# Pass, per row: the latency's maximum at or below target + deadband + one
# stall's catch-up burst (5200 + 1761 + 3440 = 10401 frames, 235.9 ms).
#
# Usage: tests/harness/check_audio_feed_latency.sh [--build-dir=DIR]
# Exit 0 PASS, 2 FAIL, 64 SKIP (driver not built).
set -u
cd "$(dirname "$0")/../.."

BUILD="build/linux-release"
for a in "$@"; do
    case "$a" in
        --build-dir=*) BUILD="${a#--build-dir=}";;
        *) echo "check_audio_feed_latency: unknown argument $a" >&2; exit 64;;
    esac
done

BIN="$BUILD/tests/unit/audio_feed_sim"
if [[ ! -x "$BIN" ]]; then
    echo "[audio-feed-latency] SKIP ($BIN not built)"
    exit 64
fi

ROWS=("strong 0.0,1,0,0.8@10+120 250" "mild 0.0,1,0,0.96@10+60 150")
OUT="$BUILD/audio-feed-latency"
mkdir -p "$OUT"
ran=0 failed=0
for row in "${ROWS[@]}"; do
    read -r tag spec secs <<<"$row"
    rm -f "${OUT:?}/$tag.txt" "${OUT:?}/$tag.trace"
    OPENPETE_AUDIO_FEED_SIM="$spec" "$BIN" --seconds="$secs" --settle=2 \
        --trace="$OUT/$tag.trace" >"$OUT/$tag.txt" 2>&1
    rc=$?
    ran=$((ran + 1))
    if [[ ! -s "$OUT/$tag.trace" ]] || ! grep -q '^verdict=' "$OUT/$tag.txt"; then
        echo "[audio-feed-latency] $tag verdict=FAIL: no verdict or trace (exit $rc, see $OUT/$tag.txt)"
        failed=$((failed + 1))
        continue
    fi
    echo "[audio-feed-latency] $tag $(grep -E '^(engagement|latency|feed|pitch) ' "$OUT/$tag.txt" | tr '\n' ' ')"
    echo "[audio-feed-latency] $tag $(grep '^verdict=' "$OUT/$tag.txt")"
    (( rc )) && failed=$((failed + 1))
done

echo "[audio-feed-latency] rows ran=$ran failed=$failed verdict=$( ((failed)) && echo FAIL || echo PASS)"
((failed)) && exit 2
exit 0
