#!/usr/bin/env bash
# tests/harness/check_livepad.sh - live controller input against DuckStation.
#
# One timed pad stream per game (tests/livepad/<game>.stream, written by
# tools/livepad.py gen) is fed to both emulators' live input paths and the
# gameplay state they reach is compared record by record:
#
#   1. openpete runs the stream headless through its live path: the stream
#      drives the first SDL virtual pad (OPENPETE_PAD_STREAM with
#      OPENPETE_INPUT_VGAMEPAD=1), so each axis value and button passes the
#      SDL gamepad state, the PsyCross decode and its stick mapping
#      (--stick-response), the keymap, the latch and the pad write. The run
#      records what the guest read (--bk2-record, consume-keyed rows) and the
#      in-run staging check proves the movie holds the bytes the guest read.
#   2. tools/livepad.py phys turns each recorded stick byte back into the
#      stream's axis value that produced it (the stream's values are chosen
#      so both mappings are invertible on them), keeping the rows' consume
#      groups.
#   3. DuckStation (scripts/capture_psx_oracle_ds.sh, the game's
#      games/<game>/livepad.conf recipe) plays those rows with regtest
#      -bk2-phys: every axis value goes through DuckStation's own live
#      input path at its default settings, keyed on the game's pad-ring
#      consumer one VBL early as every consume-keyed reference. -psxtrace-words records Spyro's
#      position at every record.
#   4. openpete replays the recorded bytes against that capture on the
#      clock row (tests/harness/check_psx_oracle.sh, OPENPETE_ORACLE_CLOCK=1,
#      keep-going, every divergent region logged) and writes Spyro's
#      position at the same records (OPENPETE_PSXTRACE_WORDS).
#
# The DuckStation side reads the rows openpete's guest consumed, so the
# harness compares what each emulator makes of the same live stick and
# button state (mapping and gameplay), not the VBL at which a live sample is
# latched.
#
# Verdict (tools/livepad.py report): Spyro's position error per record is
# the headline; a region that diverges at any record fails the run unless
# games/<game>/livepad-exclusions.txt lists it with its records and reason
# (each listed exclusion is printed on every run, and one no run matches is
# reported stale); a GTE hash mismatch fails it. Engagement (LAW §8): the
# stream's events delivered and those inside a VBL, the recorded rows with
# the left stick off centre, the rows DuckStation applied through -bk2-phys,
# and the records compared; zero in any is VACUOUS (exit 3).
#
# Usage: tests/harness/check_livepad.sh [--game G] [--mapping console|linear]
#   --mapping (default console): openpete's --stick-response for the
#   live run. console (the reference's measured live response,
#   tests/unit/data/stick-response/console.runs) needs a build with
#   --stick-response (else exit 64); linear on such a build passes the flag,
#   on an older build relies on the built-in mapping. A linear run is the
#   negative control: it diverges.
# Env: OPENPETE_BIN (default build/linux-release/openpete), OPENPETE_DISC,
#   OPENPETE_DS_DIR (DuckStation checkout, default third_party/duckstation),
#   OPENPETE_LIVEPAD_OUT (default build/livepad/<game>-<mapping>).
# Exit: 0 PASS; 2 divergence (names the first region and record, or the
#   first record whose position differs); 3 VACUOUS; 64 SKIP (binary, disc,
#   DuckStation fork with -bk2-phys and -psxtrace-words, or --stick-response
#   missing); 1 a step failed.
set -euo pipefail
export OPENPETE_VIBRATION=0
cd "$(dirname "$0")/../.."
# The player's memory cards are never opened: runs boot on scratch cards.
source scripts/lib/scratch_cards.sh

GAME=spyro-1
MAPPING=console
while [[ $# -gt 0 ]]; do
    case "$1" in
        --game) GAME="$2"; shift 2 ;;
        --mapping) MAPPING="$2"; shift 2 ;;
        -h|--help) sed -n '2,/^set -euo/s/^# \{0,1\}//p' "$0" | sed '$d'; exit 0 ;;
        *) echo "[livepad] unknown argument: $1" >&2; exit 64 ;;
    esac
done
[[ "$MAPPING" == console || "$MAPPING" == linear ]] || { echo "[livepad] --mapping console|linear" >&2; exit 64; }

RECIPE="games/$GAME/livepad.conf"
[[ -f "$RECIPE" ]] || { echo "[livepad] no recipe $RECIPE" >&2; exit 64; }
GOLDEN=""; DISC=""; LIVEPAD_STREAM=""; LIVEPAD_CONSUME_PC=""; LIVEPAD_POS_WORDS=""; LIVEPAD_RECORDS=""; LIVEPAD_EXCLUSIONS=""
# shellcheck disable=SC1090
source "$RECIPE"
BIN="${OPENPETE_BIN:-build/linux-release/openpete}"
DISC="${OPENPETE_DISC:-$DISC}"
DS_DIR="${OPENPETE_DS_DIR:-third_party/duckstation}"
DS_BIN="$DS_DIR/build-regtest/bin/duckstation-regtest"
OUT="${OPENPETE_LIVEPAD_OUT:-build/livepad/$GAME-$MAPPING}"

[[ -x "$BIN" ]] || { echo "[livepad] SKIP: binary missing: $BIN" >&2; exit 64; }
[[ -f "$DISC" ]] || { echo "[livepad] SKIP: disc missing: $DISC" >&2; exit 64; }
[[ -x "$DS_BIN" ]] || { echo "[livepad] SKIP: DuckStation not built: $DS_BIN (scripts/setup_duckstation.sh)" >&2; exit 64; }
for _f in -bk2-phys -psxtrace-words; do
    grep -aq -- "$_f" "$DS_BIN" || { echo "[livepad] SKIP: $DS_BIN lacks $_f (patch 0047; rerun scripts/setup_duckstation.sh)" >&2; exit 64; }
done
STICK_FLAG=()
HELP="$("$BIN" --help 2>&1 || true)"
if [[ "$HELP" == *--stick-response* ]]; then
    STICK_FLAG=(--stick-response "$MAPPING")
elif [[ "$MAPPING" == console ]]; then
    echo "[livepad] SKIP: $BIN has no --stick-response; the console response cannot be selected" >&2
    exit 64
fi
python3 tools/livepad.py check "$LIVEPAD_STREAM"
STREAM_END="$(python3 tools/livepad.py check "$LIVEPAD_STREAM" | sed -n 's/.*last at \([0-9]*\).*/\1/p')"

mkdir -p "$OUT"
TRACE="$OUT/trace.bin"
rm -f "${OUT:?}/live.bk2" "${OUT:?}/live.bk2.args" "${OUT:?}/live.pos" "${OUT:?}/live.log" \
      "${OUT:?}/phys.bk2" "${OUT:?}/capture.log" "${OUT:?}/verify.log" "${OUT:?}/verify.out" \
      "${OUT:?}/op.words" "${OUT:?}/replay.pos" "${OUT:?}/report.txt"
rm -f "${TRACE:?}" "${TRACE:?}".*

# ---- 1. openpete live run -----------------------------------------------------
echo "[livepad] $GAME mapping=$MAPPING stream=$LIVEPAD_STREAM (last event at VBL $STREAM_END)"
set +e
OPENPETE_INPUT_VGAMEPAD=1 OPENPETE_PAD_STREAM="$LIVEPAD_STREAM" \
    "$BIN" --game "$GAME" --headless --no-config --mods "" --disc "$DISC" --no-card --mute 1 \
    --vibration 0 --pad-unfocused 1 "${STICK_FLAG[@]}" \
    --bk2-record "$OUT/live.bk2" --position-log "$OUT/live.pos" \
    --frame-limit "$((STREAM_END + 120))" > "$OUT/live.log" 2>&1
LIVE_RC=$?
set -e
[[ -s "$OUT/live.bk2" && -s "$OUT/live.pos" ]] || {
    echo "[livepad] live run (rc=$LIVE_RC) wrote no movie or position log; see $OUT/live.log" >&2; exit 1; }
PS_LINE="$(grep -o 'pad-stream: .*' "$OUT/live.log" | tail -1 || true)"
STAGE_LINE="$(grep -o 'slot-0 staging: .*' "$OUT/live.log" | tail -1 || true)"
echo "[livepad] live: ${PS_LINE:-no pad-stream summary (stream not exhausted)}"
echo "[livepad] live: ${STAGE_LINE:-no staging summary}"
grep -o 'slot-0 staging first mismatch.*' "$OUT/live.log" | sed 's/^/[livepad] live: /' || true
EV_APPLIED="$(sed -n 's/^pad-stream: \([0-9]*\) event.*/\1/p' <<<"$PS_LINE")"
EV_MID="$(sed -n 's/.*, \([0-9]*\) mid-VBL.*/\1/p' <<<"$PS_LINE")"

# ---- 2. the recorded rows as axis values ----------------------------------------
PHYS_LINE="$(python3 tools/livepad.py phys "$OUT/live.bk2" "$OUT/phys.bk2" "$MAPPING")"
echo "[livepad] $PHYS_LINE"
ROWS_OFF="$(sed -n 's/.*off_centre=\([0-9]*\).*/\1/p' <<<"$PHYS_LINE")"

# ---- 3. DuckStation through its live mapping -------------------------------------
set +e
OPENPETE_DS_DIR="$DS_DIR" scripts/capture_psx_oracle_ds.sh --game "$GAME" --recipe "$RECIPE" \
    --out "$TRACE" --frames "$LIVEPAD_RECORDS" --no-card \
    --bk2 "$OUT/phys.bk2" --bk2-phys --bk2-consume-pc "$LIVEPAD_CONSUME_PC" --bk2-lead 1 \
    --words "$LIVEPAD_POS_WORDS" > "$OUT/capture.log" 2>&1
CAP_RC=$?
set -e
grep -E '^\[capture-ds\] (bk2|words|FAIL)' "$OUT/capture.log" | sed 's/^\[capture-ds\]/[livepad] reference:/' || true
[[ $CAP_RC -eq 0 && -s "$TRACE" && -s "$TRACE.words" ]] || {
    echo "[livepad] reference capture failed (rc=$CAP_RC); see $OUT/capture.log" >&2; exit 1; }
DS_PHYS="$(sed -n 's/^.*bk2: phys rows applied=[0-9]* with the left stick off centre=\([0-9]*\).*/\1/p' "$TRACE.log" | tail -1)"
# openpete replays the bytes its guest read.
cp "$OUT/live.bk2" "$TRACE.bk2"

# ---- 4. openpete's replay against the capture, on the clock row -----------------
set +e
OPENPETE_ORACLE_CLOCK=1 OPENPETE_ORACLE_GTE_STRICT=1 OPENPETE_ORACLE_RECORDS="$LIVEPAD_RECORDS" \
PSXTRACE_KEEP_GOING=1 OPENPETE_PSXTRACE_DUMP=1 OPENPETE_ORACLE_LOG="$OUT/verify.log" \
OPENPETE_PSXTRACE_WORDS="${LIVEPAD_POS_WORDS}:$OUT/op.words" OPENPETE_BIN="$BIN" \
    tests/harness/check_psx_oracle.sh --game "$GAME" --recipe "$RECIPE" "$TRACE" \
    -- --position-log "$OUT/replay.pos" > "$OUT/verify.out" 2>&1
VERIFY_RC=$?
set -e
[[ $VERIFY_RC -eq 64 ]] && { tail -3 "$OUT/verify.out" >&2; echo "[livepad] SKIP: the verify run skipped" >&2; exit 64; }
[[ -s "$OUT/verify.log" && -s "$OUT/op.words" ]] || {
    echo "[livepad] verify run (rc=$VERIFY_RC) wrote no log or position rows; see $OUT/verify.out" >&2; exit 1; }
grep -E '^\[psx-oracle\] (verdict|gte:|clock:)' "$OUT/verify.out" | sed 's/^\[psx-oracle\]/[livepad] oracle (information):/' || true
CONSUMED="$(sed -n 's/.*psxtrace: summary: records=\([0-9]*\) .*/\1/p' "$OUT/verify.log" | tail -1)"
GTE_MISM="$(sed -n 's/.*gte: records=[0-9]* mismatches=\([0-9]*\).*/\1/p' "$OUT/verify.log" | tail -1)"
EOF_OK=0; grep -q 'openpete-exit: reason=psxtrace-eof' "$OUT/verify.log" && EOF_OK=1

# The live run against its replay (information, not the verdict): the live
# run keeps openpete's own load and lag timing, the replay the reference's
# schedule, so ticks are aligned from the first one with Spyro placed and a
# difference there is a timing difference, not an input one.
LIVE_REPLAY="$(python3 - "$OUT/live.pos" "$OUT/replay.pos" <<'EOF'
import csv, math, sys
def placed(p):
    with open(p) as f:
        r = csv.reader(f); next(r)
        return [tuple(int(v) for v in row[2:5]) for row in r if any(int(v) for v in row[2:5])]
a, b = placed(sys.argv[1]), placed(sys.argv[2])
n = min(len(a), len(b))
d = [math.dist(a[i], b[i]) for i in range(n)]
print(n, sum(1 for x in d if x == 0), f"{max(d) if d else 0:.0f}")
EOF
)"
read -r LR_N LR_EQ LR_MAX <<<"$LIVE_REPLAY"
echo "[livepad] live vs replay (information): $LR_N ticks from Spyro's first placement, $LR_EQ equal, max distance $LR_MAX"

# ---- verdict -------------------------------------------------------------------
echo "[livepad] engagement: events=${EV_APPLIED:-0} mid_vbl=${EV_MID:-0} rows_off_centre=${ROWS_OFF:-0}" \
     "ds_rows_off_centre=${DS_PHYS:-0} records=${CONSUMED:-0}/$LIVEPAD_RECORDS"
set +e
python3 tools/livepad.py report --ref-words "$TRACE.words" --op-words "$OUT/op.words" \
    --log "$OUT/verify.log" ${LIVEPAD_EXCLUSIONS:+--exclude "$LIVEPAD_EXCLUSIONS"} | tee "$OUT/report.txt"
RC=${PIPESTATUS[0]}
set -e
[[ -s "$OUT/report.txt" ]] || { echo "[livepad] report wrote nothing" >&2; exit 1; }
if [[ "${EV_APPLIED:-0}" == 0 || "${EV_MID:-0}" == 0 || "${ROWS_OFF:-0}" == 0 || "${DS_PHYS:-0}" == 0 \
      || "${CONSUMED:-0}" == 0 || "$LR_N" == 0 ]]; then
    echo "[livepad] verdict: VACUOUS (a zero engagement counter)"
    exit 3
fi
if [[ "$EOF_OK" != 1 || "$CONSUMED" != "$LIVEPAD_RECORDS" ]]; then
    echo "[livepad] verdict: FAIL (the replay consumed ${CONSUMED:-?} of $LIVEPAD_RECORDS records)"
    exit 2
fi
if [[ $RC -ne 0 ]]; then
    echo "[livepad] verdict: FAIL"
    exit "$RC"
fi
if [[ "${GTE_MISM:-1}" != 0 ]]; then
    echo "[livepad] verdict: FAIL (GTE hash: ${GTE_MISM:-no summary} mismatching record(s))"
    exit 2
fi
echo "[livepad] verdict: PASS ($GAME, mapping $MAPPING)"
