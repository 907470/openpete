#!/usr/bin/env bash
# check_sfx_pack.sh - the sfx pack census (--dump-sfx-pack) on the Spyro 1
# retail disc, read through the disc reader in direct mode.
#
# Run: one headless `--dump-sfx-pack DIR` over the disc's WAD level and
# cutscene entries, under OPENPETE_DISC_TRACE.
#
# Instrument: the census counters line (`sfxdump: entries=... failed=...`),
# the WAV files written (content-named: label, sample hash, pitch), and the
# trace rows.
#
# Pass criterion: exit 0; the counters equal the retail disc's (entries 39,
# sources 2554, dumped 488, no_end 0, failed 0); 488 WAV files whose sorted
# names hash to the pinned digest (a changed sample byte changes its name);
# every trace row tagged sfx and direct, at least one. SKIP when the disc
# image is not named SCUS-94228, since the pins are that disc's.
#
# Verdict line:
#   [sfx-pack] entries=<n> dumped=<n> wavs=<n> sfx_rows=<n> verdict=PASS|FAIL
#
# Usage: tests/harness/check_sfx_pack.sh [--build-dir=DIR]
# Exit 0 PASS, 1 FAIL, 64 SKIP (binary or disc missing, or another disc).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
    esac
done

GAME=spyro-1
pg_resolve "$GAME"

[[ -x "$PG_BIN" && -f "$PG_DISC" ]] ||
    { echo "sfx-pack: SKIP (binary or disc missing)"; exit 64; }
[[ "$(basename "$PG_DISC")" == SCUS-94228.* ]] ||
    { echo "sfx-pack: SKIP (the pins are SCUS-94228's)"; exit 64; }

WANT_COUNTERS="entries=39 sources=2554 dumped=488 dup=2066 no_end=0 failed=0"
WANT_WAVS=488
WANT_DIGEST=7682903de48cfd83d60afb133ac33f1d5a5ce9b2f204cfd0b36b52421bd2f45a

OUT="${PG_BUILD_DIR:-build/linux-release}/sfx-pack/$GAME"
DIR="$OUT/pack"
LOG="$OUT/run.log"
TSV="$OUT/trace.tsv"
mkdir -p "$OUT"
rm -rf "$DIR"
PG_CAPTURE_ENV=("OPENPETE_DISC_TRACE=$TSV")
PG_CAPTURE_TIMEOUT=600
pg_capture "$LOG" "$TSV" -- --dump-sfx-pack "$DIR" </dev/null
rc=$?

bad=""
(( rc )) && bad="run exited $rc (see $LOG)"
counters="$(grep -oE 'entries=[0-9]+ sources=[0-9]+ dumped=[0-9]+ dup=[0-9]+ no_end=[0-9]+ failed=[0-9]+' "$LOG" | tail -n 1)"
[[ -z "$bad" && "$counters" != "$WANT_COUNTERS" ]] && bad="counters '$counters', want '$WANT_COUNTERS'"
wavs=0
digest=""
if [[ -d "$DIR" ]]; then
    wavs="$(find "$DIR" -maxdepth 1 -name '*.wav' | wc -l)"
    digest="$(find "$DIR" -maxdepth 1 -name '*.wav' -printf '%f\n' | LC_ALL=C sort | sha256sum | cut -d' ' -f1)"
fi
[[ -z "$bad" && "$wavs" != "$WANT_WAVS" ]] && bad="$wavs WAV files, want $WANT_WAVS"
[[ -z "$bad" && "$digest" != "$WANT_DIGEST" ]] && bad="WAV name digest $digest differs"
rows="$(awk -F'\t' 'NR > 1' "$TSV" 2>/dev/null | wc -l)"
odd="$(awk -F'\t' 'NR > 1 && ($3 != "sfx" || $6 != "direct")' "$TSV" 2>/dev/null | wc -l)"
[[ -z "$bad" && "$rows" == 0 ]] && bad="no trace rows"
[[ -z "$bad" && "$odd" != 0 ]] && bad="$odd trace rows not sfx/direct"

entries="$(sed -n 's/.*entries=\([0-9]*\).*/\1/p' <<<"$counters")"
dumped="$(sed -n 's/.*dumped=\([0-9]*\).*/\1/p' <<<"$counters")"
echo "[sfx-pack] entries=${entries:-0} dumped=${dumped:-0} wavs=$wavs sfx_rows=$rows" \
     "verdict=$([[ -z "$bad" ]] && echo PASS || echo "FAIL: $bad")"
[[ -z "$bad" ]]
