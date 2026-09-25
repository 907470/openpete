#!/usr/bin/env bash
# check_spyro_records.sh — the multi-Spyro sub-tick harness
# (notes/plans/openpete-native-multi-spyro-subtick.md "Instruments").
#
# The spyro-coop mod draws extra dragons by calling the game's own player
# renderers once per player with the player's state swapped into g_Spyro.
# This harness runs each case of tests/harness/spyro-records-corpus.txt headless
# with the mod loaded and asserts the OPENPETE_SPYRO_RECDBG numbers (numbers from the instrument, not pixels):
#
#   canonical leg (--screenshot-on-frame F):
#     the epoch line at f=F reports the expected model / shadow / flame hook
#     call counts, epochs_vbl=1, spec=0 and mm=0/0/0 (the last hook's bytes
#     equal the DIRECT ring's tick-end bytes for every region);
#   both legs, run-wide:
#     the summary reports spec_refused=0 and no byte mismatch, the subtick
#     view census reports cpu_writes_while_open=0 (the intermediate leg), and
#     no RESTORE MISMATCH line (OPENPETE_BRACKET_CHECK=1 on the intermediate
#     leg).
#   intermediate leg (--screenshot-intermediate-on-frame F): the per-present
#     record lines at f=F (spyro-records-check.py): the drawn record count
#     equals the model-call count, each record's pairing index equals the
#     case's `pairs`, a paired record interpolates and stages within B/2 of
#     its canonical position.
#
# The run is VACUOUS (exit 64) unless some case reports two or more model
# calls on its canonical tick (the multi-record path engaged; cases expecting
# fewer are the pairing-only rows), and the store-clock record-only arm is
# reported VACUOUS unless some case's summary reports roll_records >= 1.
#
# The mod is NOT in-tree. Name its source dir with SPYRO_COOP_DIR (the dir
# holding mod.toml); the harness copies it to a scratch mods root so the
# players setting it writes never touches the named copy. Refuses (exit 64)
# without it.
#
# Usage: tests/harness/check_spyro_records.sh [--game SLUG] [--filter=SUBSTR]
#                                       [--build-dir=DIR] [--keep]
# Exit: 0 PASS, 1 FAIL, 64 SKIP/VACUOUS (prereqs missing or the corpus
# cannot engage the asserts).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

GAME="$(pg_parse_game "$@")" || exit 64

FILTER=""; BUILD_DIR="build/linux-release"; KEEP=0
for a in "$@"; do case "$a" in
  --filter=*) FILTER="${a#--filter=}";;
  --build-dir=*) BUILD_DIR="${a#--build-dir=}";;
  --keep) KEEP=1;;
  --game|--game=*) ;;   # consumed by pg_parse_game above
  spyro-*) ;;           # the two-word --game SLUG spelling
  *) echo "usage: $0 [--game SLUG] [--filter=SUBSTR] [--build-dir=DIR] [--keep]" >&2; exit 64;;
esac; done

PG_BUILD_DIR="$BUILD_DIR"
pg_resolve "$GAME"
BIN="$PG_BIN"
DISC="$PG_DISC"
CORPUS="tests/harness/spyro-records-corpus.txt"
COOP="${SPYRO_COOP_DIR:-}"

if [[ ! -x "$BIN" ]]; then echo "[spyro-records] SKIP: no binary at $BIN"; exit 64; fi
if [[ ! -f "$DISC" ]]; then echo "[spyro-records] SKIP: no disc at $DISC"; exit 64; fi
if [[ -z "$COOP" || ! -f "$COOP/mod.toml" ]]; then
  echo "[spyro-records] SKIP: SPYRO_COOP_DIR must name the spyro-coop mod dir (mod.toml); got '${COOP}'"
  exit 64
fi

OUT="${SPYRO_RECORDS_OUT:-build/spyro-records}"
rm -rf "$OUT"; mkdir -p "$OUT/mods" "$OUT/shots"
cp -r "$COOP" "$OUT/mods/spyro-coop"
rm -rf "$OUT/mods/spyro-coop/.build"
[[ $KEEP -eq 0 ]] && trap 'rm -rf "$OUT/mods"' EXIT

# One headless run. $1 case, $2 leg (canon|inter), $3 bk2-or-level, $4 players,
# $5 frame. Log to $OUT/<case>-<leg>.log. Headless native (the hooks) comes up
# only when a capture is requested (native_render.c W1.1), so every leg names one.
run_leg() {
  local name=$1 leg=$2 src=$3 players=$4 frame=$5 extra=${6:-}
  local log="$OUT/$name-$leg.log"
  # The mod reads and REWRITES its settings.txt (coop_settings.c), so pin the
  # players count before every run.
  printf 'players = %s\nrespawn = modern\nsplit = vertical\nextra_controls = controller\ndraw_p2 = 1\n' \
    "$players" > "$OUT/mods/spyro-coop/data/settings.txt"
  local -a srcargs capargs
  if [[ $src == level:* ]]; then srcargs=(--skip-to-level "${src#level:}")
  else srcargs=(--bk2 "$PG_BK2_DIR/$src.bk2"); fi
  if [[ $leg == canon ]]; then capargs=(--screenshot-on-frame "$frame" --screenshot-native-only)
  else capargs=(--screenshot-intermediate-on-frame "$frame"); fi
  # --render-fps above the sim rate arms the DIRECT ring (openpete_interp.h);
  # without it the hook-vs-ring column is vacuous (the summary's no_ring
  # counts those epochs and the harness refuses them).
  local -a PG_CAPTURE_ENV=(OPENPETE_SPYRO_RECDBG=1 OPENPETE_BRACKET_CHECK=1)
  local PG_CAPTURE_TIMEOUT=600 PG_CAPTURE_STDOUT=/dev/null
  # The screenshot names carry the frame number, so no output is asserted by
  # name here; the asserts are the RECDBG lines the log must carry.
  pg_capture "$log" "" -- --renderer vk --mute 1 --render-fps 60 \
    --mods-dir "$OUT/mods" --mods spyro-coop "${srcargs[@]}" "${capargs[@]}" \
    --screenshot-output "$OUT/shots/$name-$leg-{n}.png" \
    --frame-limit "$((frame + 1))" --max-runtime-seconds 560 ${extra:+$extra}
  echo $?
}

fail=0; cases=0; engaged=0; recroll=0
while IFS='|' read -r name src players frame em es ef pairs note extra; do
  [[ -z "$name" || "$name" == \#* ]] && continue
  [[ -n "$FILTER" && "$name" != *"$FILTER"* ]] && continue
  cases=$((cases + 1))
  # Optional 10th field: extra launch args (a --runahead N row exercises the
  # led lists; its staged positions lead the canonical ones by up to N ticks,
  # so the B/2 bound scales by 1+2N, and the summary must report led_match
  # >= 1 with led_miss <= 1 — the level-entry tick draws the player twice
  # and has no led counterpart of that shape).
  ra=$(sed -n 's/.*--runahead \([0-9]*\).*/\1/p' <<<"${extra:-}")
  rc=$(run_leg "$name" canon "$src" "$players" "$frame" "${extra:-}")
  log="$OUT/$name-canon.log"
  line=$(grep -m1 "^\[recdbg f=$frame " "$log" || true)
  if [[ $rc -ne 0 || -z "$line" ]]; then
    echo "FAIL     $name: canonical leg rc=$rc, no RECDBG epoch line at f=$frame (see $log)"; fail=1; continue
  fi
  got_m=$(sed -n 's/.* calls m=\([0-9]*\).*/\1/p' <<<"$line")
  got_s=$(sed -n 's/.* s=\([0-9]*\) f=.*/\1/p' <<<"$line")
  got_f=$(sed -n 's/.* f=\([0-9]*\) spec=.*/\1/p' <<<"$line")
  spec=$(sed -n 's/.* spec=\([0-9]*\).*/\1/p' <<<"$line")
  ev=$(sed -n 's/.* epochs_vbl=\([0-9]*\).*/\1/p' <<<"$line")
  mm=$(sed -n 's/.* mm=\([0-9\/]*\).*/\1/p' <<<"$line")
  sum=$(grep -m1 'recdbg-summary' "$log" || true)
  sum_spec=$(sed -n 's/.* spec_refused=\([0-9]*\).*/\1/p' <<<"$sum")
  sum_mm=$(sed -n 's/.* mismatch m=\([0-9]*\) s=\([0-9]*\) f=\([0-9]*\).*/\1\/\2\/\3/p' <<<"$sum")
  sum_rr=$(sed -n 's/.* roll_records=\([0-9]*\).*/\1/p' <<<"$sum")
  sum_nr=$(sed -n 's/.* no_ring=\([0-9]*\).*/\1/p' <<<"$sum")
  verdict=ok
  [[ "$got_m/$got_s/$got_f" != "$em/$es/$ef" ]] && verdict="FAIL calls m/s/f=$got_m/$got_s/$got_f expected $em/$es/$ef"
  [[ "$spec" != 0 ]] && verdict="FAIL spec=$spec"
  [[ "$ev" != 1 ]] && verdict="FAIL epochs_vbl=$ev"
  # Under --runahead the committed hooks and the led ring differ by design
  # (the led lists carry the difference); the memcmp columns are not asserted.
  [[ -z "$ra" && "$mm" != "0/0/0" ]] && verdict="FAIL hook-vs-ring mm=$mm"
  [[ "$sum_spec" != 0 ]] && verdict="FAIL run spec_refused=$sum_spec"
  if [[ -n "$ra" ]]; then
    led_match=$(sed -n 's/.* led_match=\([0-9]*\).*/\1/p' <<<"$sum")
    led_miss=$(sed -n 's/.* led_miss=\([0-9]*\).*/\1/p' <<<"$sum")
    [[ "${led_match:-0}" -lt 1 ]] && verdict="FAIL runahead: led_match=${led_match:-0} (no led list applied)"
    [[ "${led_miss:-0}" -gt 1 ]] && verdict="FAIL runahead: led_miss=${led_miss:-0}"
    note="$note (led_match=$led_match led_miss=$led_miss)"
  fi
  [[ -z "$ra" && "$sum_mm" != "0/0/0" ]] && verdict="FAIL run mismatch=$sum_mm"
  [[ "${sum_nr:-x}" != 0 ]] && verdict="FAIL ring never captured on ${sum_nr:-?} epoch(s): hook-vs-ring column vacuous"
  [[ "${sum_rr:-0}" -ge 1 ]] && recroll=1
  [[ "$got_m" -ge 2 ]] && engaged=1
  if [[ $verdict != ok ]]; then echo "FAIL     $name (canon f=$frame): $verdict"; fail=1; continue; fi
  echo "ok       $name (canon f=$frame): calls m/s/f=$got_m/$got_s/$got_f mm=$mm — $note"

  # Intermediate leg: run-wide staging checks, then the per-record asserts.
  rc=$(run_leg "$name" inter "$src" "$players" "$frame" "${extra:-}")
  log="$OUT/$name-inter.log"
  if [[ $rc -ne 0 ]]; then echo "FAIL     $name: intermediate leg rc=$rc (see $log)"; fail=1; continue; fi
  writes=$(sed -n 's/.*cpu_writes_while_open=\([0-9]*\).*/\1/p' "$log" | tail -1)
  if [[ "${writes:-x}" != 0 ]]; then echo "FAIL     $name (inter): view write census cpu_writes_while_open=${writes:-missing}"; fail=1; continue; fi
  if grep -q 'RESTORE MISMATCH' "$log"; then echo "FAIL     $name (inter): RESTORE MISMATCH (see $log)"; fail=1; continue; fi
  # Per-record asserts: drawn count, pairing index per record, paired records
  # interpolate and stage within B/2 of their canonical position; shadow and
  # flame record counts, each bound to a record of the list, lerping when it is.
  bscale=$(( 1 + 2 * ${ra:-0} ))
  iv=$(python3 tests/harness/spyro-records-check.py "$log" "$frame" "$em" "$pairs" "$es" "$ef" "$(( 400 * bscale ))"); irc=$?
  case $irc in
    0)  echo "ok       $name (inter): ${iv#ok       }";;
    64) echo "vacuous  $name (inter): ${iv#vacuous  }";;
    *)  echo "FAIL     $name (inter): ${iv#FAIL     }"; fail=1;;
  esac
done < "$CORPUS"

if [[ $cases -eq 0 ]]; then echo "[spyro-records] SKIP: no case matched '$FILTER'"; exit 64; fi
[[ $recroll -eq 0 ]] && echo "[spyro-records] VACUOUS: store-clock record-only arm not reached by any case (roll_records=0 everywhere; covered by the injected-state probe, see the plan)"
if [[ $fail -ne 0 ]]; then echo "[spyro-records] FAIL ($cases case(s); logs in $OUT)"; exit 1; fi
if [[ $engaged -eq 0 ]]; then echo "[spyro-records] VACUOUS: no case reported >= 2 model calls on its canonical tick"; exit 64; fi
echo "[spyro-records] PASS: $cases case(s)"
exit 0
