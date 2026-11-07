#!/usr/bin/env bash
# check_audio_tick_scale.sh - the audio device feed holds its queued depth
# and plays without a gap through live tick-scale changes, and returns to
# its 1.0 depth after each return to 1.0.
#
# Run: headless attract runs per game to --frame-limit 6800 on the feed's
# synthetic device at drift 0, one per producer-model seed in SEEDS
# (OPENPETE_AUDIO_FEED_SIM=0.0,SEED). The synthetic device runs on the
# virtual clock: the device and the consumer thread advance inside each
# clock step and the producer model's jitter and stalls are clock advances,
# so the row is independent of host load and thread timing. Each run is
# under OPENPETE_AUDIO_FEED_LOG and the schedule
# OPENPETE_TICK_SCALE_AT (each entry calls openpete_set_tick_scale, the
# devui row's setter, at its VBL):
#   1.0 -> 0.5 (VBL 2000) -> 1.0 (2600) -> 1.5 (3200) -> 1.0 (3800)
#       -> 0.25 (4400) -> 1.0 (5000) -> 3.0 (5600) -> 1.0 (6200)
#
# Instrument: tools/audio_feed_tick_scale.py over the timeline: per segment
# of constant scale, the queued depth (frames at the device plus the FIFO
# at the resampling ratio, in ms of device time), the UNDERRUN and OVERFLOW
# events, and the recovery time from the change until the depth stays
# within the band around the segment's target.
# Engagement: data plays (a prime) and every segment has timeline lines.
#
# Pass, per run and segment: 0 underruns and 0 overflow events; depth from 1 s after
# the change within BAND_MS of the target (target 118 ms at scale >= 1;
# below 1 the target grows by twice and the band by once the excess of
# one VBL's push, 737/scale - 737 frames); recovery <= RECOVER_MS.
#
# Usage: tests/harness/check_audio_tick_scale.sh [--build-dir=DIR] [--game=G]
# Exit 0 PASS, 2 FAIL, 64 SKIP (no row ran: binaries or discs missing).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

only_game=""
for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
        --game=*) only_game="${a#--game=}";;
        *) echo "check_audio_tick_scale: unknown argument $a" >&2; exit 64;;
    esac
done

SCHEDULE="2000:0.5,2600:1.0,3200:1.5,3800:1.0,4400:0.25,5000:1.0,5600:3.0,6200:1.0"
LIMIT=6800
BAND_MS=45
RECOVER_MS=1000
SEEDS=(1 2 3)

ROOT="${PG_BUILD_DIR:-build/linux-release}/audio-tick-scale"
ran=0 failed=0 skipped=0
for game in spyro-1 spyro-2 spyro-3; do
    [[ -n "$only_game" && "$game" != "$only_game" ]] && continue
    pg_resolve "$game"
    if [[ ! -x "$PG_BIN" || ! -f "$PG_DISC" ]]; then
        echo "[tick-scale-audio] $game SKIP (binary or disc missing)"
        skipped=$((skipped + 1))
        continue
    fi
    for seed in "${SEEDS[@]}"; do
    out="$ROOT/$game/seed$seed"
    mkdir -p "$out"
    rm -f "${out:?}/run.log" "${out:?}/feed.log" "${out:?}/check.txt"
    PG_CAPTURE_ENV=("OPENPETE_AUDIO_FEED_SIM=0.0,$seed" "OPENPETE_AUDIO_FEED_LOG=$out/feed.log"
                    "OPENPETE_TICK_SCALE_AT=$SCHEDULE")
    PG_CAPTURE_TIMEOUT=900
    pg_capture "$out/run.log" "$out/feed.log" -- \
        --mute 0 --frame-limit "$LIMIT" </dev/null
    rc=$?
    ran=$((ran + 1))
    if (( rc )); then
        echo "[tick-scale-audio] $game/seed$seed verdict=FAIL: run exited $rc (see $out/run.log)"
        failed=$((failed + 1))
        continue
    fi
    python3 tools/audio_feed_tick_scale.py --log "$out/feed.log" --schedule "$SCHEDULE" \
        --band-ms "$BAND_MS" --recover-ms "$RECOVER_MS" --tag "$game/seed$seed" >"$out/check.txt" 2>&1
    crc=$?
    cat "$out/check.txt"
    grep -q 'verdict=' "$out/check.txt" || crc=2
    (( crc )) && failed=$((failed + 1))
    done
done

echo "[tick-scale-audio] rows ran=$ran failed=$failed skipped=$skipped" \
     "verdict=$( ((ran == 0)) && echo SKIP || { ((failed)) && echo FAIL || echo PASS; })"
((ran == 0)) && exit 64
((failed)) && exit 2
exit 0
