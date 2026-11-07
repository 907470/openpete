#!/usr/bin/env bash
# check_audio_feed_slip.sh - the host's sim slips reach the audio device
# feed's consumer end to end in the game binary.
#
# Run: one headless Spyro 1 attract run per row on the feed's synthetic
# device at drift 0 (OPENPETE_AUDIO_FEED_SIM=0.0,1, on the virtual clock),
# under OPENPETE_AUDIO_FEED_LOG, with host stalls injected through
# OPENPETE_PACE_INJECT_US=US,EVERY,FROM,COUNT as virtual-clock advances
# before the game fiber runs (paced VBLs counted from 0):
#   stall  one 500 ms stall at paced VBL 3000 (30 sim periods; the slip
#          threshold is 4)
#   span   1.25 sim periods (20894 us) on each of 7179 paced VBLs from VBL
#          3000: work takes no virtual time, so each VBL then spans 1.25
#          periods and the host runs at 0.8x for 150 s, then 1.0x
#
# Instrument: the pacer's exit line `slip totals: sim E event(s) / P
# period(s)`, and the feed timeline's slips= field (the count the pacer
# publishes to the feed as an atomic, read on the consumer thread) on its
# last line. Engagement: the feed played data (primes >= 1).
#
# Pass, per row: sim slip periods > 0 at the pacer, and the timeline's last
# slips= value equal to it.
#
# Usage: tests/harness/check_audio_feed_slip.sh [--build-dir=DIR]
# Exit 0 PASS, 2 FAIL, 64 SKIP (binary or disc missing).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
        *) echo "check_audio_feed_slip: unknown argument $a" >&2; exit 64;;
    esac
done

ROWS=("stall 500000,1,3000,1 3600" "span 20894,1,3000,7179 10800")
ROOT="${PG_BUILD_DIR:-build/linux-release}/audio-feed-slip"
game=spyro-1
pg_resolve "$game"
if [[ ! -x "$PG_BIN" || ! -f "$PG_DISC" ]]; then
    echo "[audio-feed-slip] SKIP (binary or disc missing)"
    exit 64
fi
ran=0 failed=0
for row in "${ROWS[@]}"; do
    read -r tag inject limit <<<"$row"
    out="$ROOT/$tag"
    mkdir -p "$out"
    rm -f "${out:?}/run.log" "${out:?}/feed.log"
    PG_CAPTURE_ENV=("OPENPETE_AUDIO_FEED_SIM=0.0,1" "OPENPETE_AUDIO_FEED_LOG=$out/feed.log"
                    "OPENPETE_PACE_INJECT_US=$inject")
    PG_CAPTURE_TIMEOUT=900
    pg_capture "$out/run.log" "$out/feed.log" -- --mute 0 --frame-limit "$limit" </dev/null
    rc=$?
    ran=$((ran + 1))
    if (( rc )); then
        echo "[audio-feed-slip] $tag verdict=FAIL: run exited $rc (see $out/run.log)"
        failed=$((failed + 1))
        continue
    fi
    pacer="$(grep -ao 'slip totals: sim [0-9]* event(s) / [0-9]* period(s)' "$out/run.log" | tail -n 1)"
    periods="$(sed -n 's/.*sim [0-9]* event(s) \/ \([0-9]*\) period(s).*/\1/p' <<<"$pacer")"
    feed_slips="$(grep -v '^#' "$out/feed.log" | tail -n 1 | grep -o 'slips=[0-9]*' | cut -d= -f2)"
    primes="$(grep -ao 'feed: pushed=.*' "$out/run.log" | tail -n 1 | grep -o 'primes=[0-9]*' | cut -d= -f2)"
    fails=()
    [[ -z "$primes" || "$primes" -lt 1 ]] && fails+=("VACUOUS (no data played)")
    [[ -z "$periods" || "$periods" -eq 0 ]] && fails+=("no sim slip at the pacer")
    [[ -n "$periods" && "$feed_slips" != "$periods" ]] && fails+=("feed slips=${feed_slips:-none} != pacer $periods")
    verdict=PASS
    (( ${#fails[@]} )) && { verdict="FAIL: $(IFS=';'; echo "${fails[*]}")"; failed=$((failed + 1)); }
    echo "[audio-feed-slip] $tag inject=$inject pacer_periods=${periods:-none} feed_slips=${feed_slips:-none}" \
         "primes=${primes:-none} verdict=$verdict"
done

echo "[audio-feed-slip] rows ran=$ran failed=$failed verdict=$( ((failed)) && echo FAIL || echo PASS)"
((failed)) && exit 2
exit 0
