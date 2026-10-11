#!/usr/bin/env bash
# check_cutscene_audio.sh - the Spyro 2/3 cutscene speech ring: every refill
# lands behind the voice head and the voice plays each chunk whole.
#
# Run: one headless `--skip-to-cutscene` run per row, to past the speech
# voice's key-off, under OPENPETE_AUDIO_HEAD_LOG, OPENPETE_AUDIO_DUMP and
# OPENPETE_SPU_WRITE_DUMP. Rows (game, cutscene, --frame-limit, ring runs,
# reference capture):
#   spyro-2 intro                 3673  1  psxtrace-cutscene-intro
#                                          (key-off at openpete_frame 2957)
#   spyro-2 ending                6573  1  -  (key-off at frame 5877)
#   spyro-3 the_second_warning    4973  1  -  (VBL 4907, frame 4334)
#   spyro-3 an_evil_plot_unfolds 10173  3  -  (last key-off at frame 9384)
# The --frame-limit column is the feed key's (VBLs on Spyro 2 and 3); the
# key-off columns are openpete_frame, 573 fast-boot VBLs behind it.
# A row whose ring run has no key-off stopped mid-cutscene and fails.
#
# Instrument: tools/audio_ring_check.py over the head log (the guest voice
# head and the OpenAL source offset at each ring SpuWrite): per chunk the
# overwrite (unplayed tail of chunk c-2 replaced) and stale (half played
# under old content) in exact decoder samples, the slip steps of the OpenAL
# offset against the guest head, and the body correlation of each chunk.
# Engagement: ring writes per row, nonzero.
#
# Pass criterion, per row: overwrite = 0 and stale = 0 samples on every
# chunk, 0 slip steps, body correlation >= 0.95 on every chunk with signal,
# the expected ring-run count, every run ending in a key-off.
#
# Body reference: a row with a DuckStation capture
# (REF_DIR/<game>/psxtrace-cutscene-<name>.bin with its .spuq, .watch and
# .wav; generated from games/<game>/oracle-cutscene-<name>.conf, never
# committed) correlates openpete's mix over each chunk against the
# reference mix, and the instrument prints the reference's own overwrite,
# stale and lead per chunk. A row without one falls back to the
# self-consistency body check (each chunk's own SpuWrite payload, decoded
# and resampled, against the engine's mix), which is not a hardware
# reference; the verdict line names which ran (body=reference or
# body=self-consistency). Only the S2 intro has a reference recipe: the
# other rows run under --skip-to-cutscene, an openpete-only flag.
#
# Verdict line per row:
#   [cutscene-audio] <game>/<cutscene> body=<kind> <counters> verdict=PASS|FAIL...
#
# Usage: tests/harness/check_cutscene_audio.sh [--build-dir=DIR] [--game=G]
#            [--row=CUTSCENE] [--ref-dir=DIR (default tests/golden)]
# Exit 0 PASS, 2 FAIL, 64 SKIP (no row ran: binaries or discs missing).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

only_game="" only_row="" ref_dir="tests/golden"
for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
        --game=*) only_game="${a#--game=}";;
        --row=*) only_row="${a#--row=}";;
        --ref-dir=*) ref_dir="${a#--ref-dir=}";;
        *) echo "check_cutscene_audio: unknown argument $a" >&2; exit 64;;
    esac
done

ROWS=(
    "spyro-2 intro 3673 1 psxtrace-cutscene-intro.bin"
    "spyro-2 ending 6573 1 -"
    "spyro-3 the_second_warning 4973 1 -"
    "spyro-3 an_evil_plot_unfolds 10173 3 -"
)

ROOT="${PG_BUILD_DIR:-build/linux-release}/cutscene-audio"
ran=0 failed=0 skipped=0
for row in "${ROWS[@]}"; do
    read -r game cut limit runs refname <<<"$row"
    [[ -n "$only_game" && "$game" != "$only_game" ]] && continue
    [[ -n "$only_row" && "$cut" != "$only_row" ]] && continue
    pg_resolve "$game"
    if [[ ! -x "$PG_BIN" || ! -f "$PG_DISC" ]]; then
        echo "[cutscene-audio] $game/$cut SKIP (binary or disc missing)"
        skipped=$((skipped + 1))
        continue
    fi
    out="$ROOT/$game-$cut"
    mkdir -p "$out"
    rm -f "$out/run.log" "$out/dump.raw" "$out/dump.raw.idx" "$out/head.log" "$out/check.txt"
    rm -rf "$out/writes"
    mkdir -p "$out/writes"
    PG_CAPTURE_ENV=("OPENPETE_AUDIO_DUMP=$out/dump.raw"
                    "OPENPETE_AUDIO_HEAD_LOG=$out/head.log"
                    "OPENPETE_SPU_WRITE_DUMP=$out/writes")
    PG_CAPTURE_TIMEOUT=900
    pg_capture "$out/run.log" "$out/dump.raw,$out/dump.raw.idx,$out/head.log" -- \
        --mute 0 --skip-to-cutscene "$cut" --frame-limit "$limit" </dev/null
    rc=$?
    ran=$((ran + 1))
    if (( rc )); then
        echo "[cutscene-audio] $game/$cut verdict=FAIL: run exited $rc (see $out/run.log)"
        failed=$((failed + 1))
        continue
    fi
    refargs=() body="body=self-consistency"
    ref="$ref_dir/$game/$refname"
    if [[ "$refname" != - && -f "$ref.spuq" && -f "$ref.wav" ]]; then
        refargs=(--ref "$ref") body="body=reference"
    fi
    python3 tools/audio_ring_check.py --head-log "$out/head.log" --dump "$out/dump.raw" \
        --writes "$out/writes" --min-corr 0.95 --expect-runs "$runs" \
        --tag "$game/$cut $body" "${refargs[@]}" >"$out/check.txt" 2>&1
    crc=$?
    line="$(grep '^\[audio-ring\]' "$out/check.txt" | tail -n 1)"
    if [[ -z "$line" ]]; then
        echo "[cutscene-audio] $game/$cut verdict=FAIL: no verdict from the instrument (see $out/check.txt)"
        failed=$((failed + 1))
        continue
    fi
    echo "[cutscene-audio] ${line#\[audio-ring\] }"
    (( crc )) && failed=$((failed + 1))
done

echo "[cutscene-audio] rows ran=$ran failed=$failed skipped=$skipped" \
     "verdict=$( ((ran == 0)) && echo SKIP || { ((failed)) && echo FAIL || echo PASS; })"
((ran == 0)) && exit 64
((failed)) && exit 2
exit 0
