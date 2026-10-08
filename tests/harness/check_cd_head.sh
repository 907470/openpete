#!/usr/bin/env bash
# check_cd_head.sh - the CD drive head as the guest reads it (CdlGetlocP) and
# as the XA decoder delivers it, against the double-speed drive rate of
# 150 sectors per second = 37609/15000 LBA per VBL.
#
# Row getlocp/<game> (spyro-2, spyro-3): tools/getlocp_check.py over the
# game's 20k attract golden's .cdirq sidecar. Every reference CdlGetlocP
# answer of every streaming run is compared with openpete's drive head
# replayed over the reference's own commands (the seek from the head's
# position before the run's CdlReadS, then the drive rate). Pass: |model -
# reference| <= 2 LBA at every answer, at least the golden's run count
# (spyro-2 23, spyro-3 25). The instrument's positive control (--model
# calibrate: the drive rate from the fitted seek-latency VBL) must pass on
# the same golden; a failing control fails the row as an instrument fault.
# Engagement: answers compared, nonzero. The golden is generated, never
# committed (check_psx_oracle.sh captures it).
#
# Row xa/spyro-2: one headless boot run to --frame-limit 3400 under
# OPENPETE_XA_LOG (the decoder's next LBA once per VBL) and no audio dump:
# the decoder follows the drive head, so it advances whether or not any
# sink drains the render. tools/xa_head_check.py fits the head's LBA on VBL
# per stream segment of >= 1000 VBL (the title track, VBL ~1060-3250),
# skipping the first 120 VBL. Pass: slope within 0.0005 of 2.507267.
# Engagement: qualifying segments, nonzero.
#
# Verdict line per row: [cd-head] <row> <instrument verdict line>.
# Usage: tests/harness/check_cd_head.sh [--build-dir=DIR] [--golden-dir=DIR]
# Exit 0 PASS, 2 FAIL, 64 SKIP (no row ran: goldens, binary or disc missing).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

golden_dir="tests/golden"
for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
        --golden-dir=*) golden_dir="${a#--golden-dir=}";;
        *) echo "check_cd_head: unknown argument $a" >&2; exit 64;;
    esac
done

ROOT="${PG_BUILD_DIR:-build/linux-release}/cd-head"
mkdir -p "$ROOT"
ran=0 failed=0 skipped=0

for row in "spyro-2 23" "spyro-3 25"; do
    read -r game minruns <<<"$row"
    cdirq="$golden_dir/$game/psxtrace-demo-20k.bin.cdirq"
    out="$ROOT/getlocp-$game.txt" ctl="$ROOT/getlocp-$game-control.txt"
    rm -f "$out" "$ctl"
    if [[ ! -f "$cdirq" ]]; then
        echo "[cd-head] getlocp/$game SKIP ($cdirq missing)"
        skipped=$((skipped + 1))
        continue
    fi
    ran=$((ran + 1))
    python3 tools/getlocp_check.py "$cdirq" --model calibrate --min-runs "$minruns" >"$ctl" 2>&1
    if (( $? )) || [[ ! -s "$ctl" ]]; then
        echo "[cd-head] getlocp/$game verdict=FAIL: positive control failed (see $ctl)"
        failed=$((failed + 1))
        continue
    fi
    python3 tools/getlocp_check.py "$cdirq" --min-runs "$minruns" --tag "$game" >"$out" 2>&1
    rc=$?
    line="$(grep '^\[getlocp\]' "$out" | tail -n 1)"
    if [[ -z "$line" ]]; then
        echo "[cd-head] getlocp/$game verdict=FAIL: no verdict from the instrument (see $out)"
        failed=$((failed + 1))
        continue
    fi
    echo "[cd-head] getlocp/${line#\[getlocp\] }"
    (( rc )) && failed=$((failed + 1))
done

pg_resolve spyro-2
out="$ROOT/xa-spyro-2"
mkdir -p "$out"
rm -f "$out/run.log" "$out/xa.log" "$out/check.txt"
if [[ ! -x "$PG_BIN" || ! -f "$PG_DISC" ]]; then
    echo "[cd-head] xa/spyro-2 SKIP (binary or disc missing)"
    skipped=$((skipped + 1))
else
    ran=$((ran + 1))
    PG_CAPTURE_ENV=("OPENPETE_XA_LOG=$out/xa.log")
    PG_CAPTURE_TIMEOUT=900
    pg_capture "$out/run.log" "$out/xa.log" -- \
        --mute 1 --mods "" --frame-limit 3400 </dev/null
    rc=$?
    if (( rc )); then
        echo "[cd-head] xa/spyro-2 verdict=FAIL: run exited $rc (see $out/run.log)"
        failed=$((failed + 1))
    else
        python3 tools/xa_head_check.py "$out/xa.log" --tag spyro-2 >"$out/check.txt" 2>&1
        crc=$?
        line="$(grep '^\[xa-head\]' "$out/check.txt" | tail -n 1)"
        if [[ -z "$line" ]]; then
            echo "[cd-head] xa/spyro-2 verdict=FAIL: no verdict from the instrument (see $out/check.txt)"
            failed=$((failed + 1))
        else
            echo "[cd-head] xa/${line#\[xa-head\] }"
            (( crc )) && failed=$((failed + 1))
        fi
    fi
fi

echo "[cd-head] rows ran=$ran failed=$failed skipped=$skipped" \
     "verdict=$( ((ran == 0)) && echo SKIP || { ((failed)) && echo FAIL || echo PASS; })"
((ran == 0)) && exit 64
((failed)) && exit 2
exit 0
