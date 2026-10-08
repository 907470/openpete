#!/usr/bin/env bash
#
# check_corpus_resume.sh - the row completion stamps and their import
# (scripts/corpus-resume.sh) on a synthetic candidate directory: no binary,
# no disc, no capture image.
#
# An interrupted attempt is built leg by leg through rs_leg_done and
# rs_files_done (one bk2 row finished, one bk2 row with one leg missing, one
# freecam row finished, a passed credits leg, a stray file), then rs_import
# is run on copies of it under each condition:
#   resume      same run, tree, binary, manifest: the 3 finished rows and the
#               credits leg are imported, the partial row and the stray file
#               deleted, the census counts 6 tiles in 3 rows plus the credits
#   tampered    one tile's bytes changed: that row is refused, the others imported
#   new-run     another run id: nothing is imported, the directory is empty
#   binary      another binary: nothing is imported
#   tree        another capture-fields file: nothing is imported
#   params      another LOD set: no corpus row is imported
#   row-edit    the bk2 row's manifest line edited: that row is refused
#   filter      a bk2 filter that excludes the row: that row is refused
#   prev-left   a previous import interrupted (prev beside a partial outdir):
#               the prev directory is the import source and is consumed
#   race        the three legs of one row finish at once: one stamp, 3 tiles
#   off         OPENPETE_RESUME_RUN unset: no stamp is written
#
# Exit: 0 every case holds, 1 a case failed (named).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/presentation-game.sh
source scripts/presentation-game.sh
# shellcheck source=scripts/corpus-resume.sh
source scripts/corpus-resume.sh

W="$(mktemp -d -t corpus-resume.XXXXXX)"
trap 'rm -rf "$W"' EXIT
fails=0
pass() { echo "ok   $1"; }
fail() { echo "FAIL $1: $2"; fails=$((fails + 1)); }

export OPENPETE_CAP_LODS=original      # 3 legs per row
PG_GAME=spyro-1 PG_TIER=presentation
PG_CORPUS_MANIFEST="$W/presentation-corpus.manifest"
PG_FREECAM_MANIFEST="$W/freecam-corpus.manifest"
ROW_A='rowa     | 100 | 100 |  | 150'
ROW_B='rowb     | 100 | 100'
ROW_F='lvl_one  | pos=(1, 2, 3) yaw=0 pitch=0'
printf '# header\n%s\n%s\n' "$ROW_A" "$ROW_B" >"$PG_CORPUS_MANIFEST"
printf '%s\n' "$ROW_F" >"$PG_FREECAM_MANIFEST"
SLUG="$(pg_pose_slug lvl_one 'pos=(1, 2, 3) yaw=0 pitch=0')"
printf 'head: 0123\nsource-tree: abcd\n' >"$W/fields.txt"
head -c 4096 /dev/urandom >"$W/bin"
export OPENPETE_CAPTURE_FIELDS="$W/fields.txt"
echo "[rhi/vk] device='llvmpipe (LLVM 15.0.7, 256 bits)' api=1.3 driver-info='Mesa 23.2.1'" >"$W/leg.log"

# One leg: write its tiles, then record it. args: kind key row leg tile...
leg() {
  local kind="$1" key="$2" row="$3" lg="$4" t; shift 4
  for t in "$@"; do head -c 512 /dev/urandom >"$OUT/$t"; done
  rs_leg_done "$kind" "$key" "$row" "$lg" "$W/leg.log" "$@"
}

# The interrupted attempt.
export OPENPETE_RESUME_RUN=run-1
OUT="$W/attempt1"; mkdir -p "$OUT"
rs_init "$OUT" "$W/bin" default
(( RS_ON )) || { echo "FAIL setup: resume did not switch on"; exit 1; }
for lg in canon inter ident; do leg bk2 rowa "$ROW_A" "original-$lg" "rowa__${lg}__100-vk.png"; done
for lg in canon inter; do leg bk2 rowb "$ROW_B" "original-$lg" "rowb__${lg}__100-vk.png"; done
for lg in canon inter ident; do leg freecam "$SLUG" "$ROW_F" "original-$lg" "${SLUG}__${lg}__1900-vk.png"; done
printf 'rc=0\ncredits-interp: PASS\n' >"$OUT/credits-verdict.txt"
rs_files_done credits credits credits-interp credits-verdict.txt
echo stray >"$OUT/stray.txt"
n=$(ls "$OUT"/resume/*.stamp | wc -l)
[[ $n -eq 3 && ! -e "$OUT/resume/bk2.rowb.stamp" ]] && pass "stamps: 3 finished rows, none for the partial row" \
  || fail stamps "$n stamps: $(ls "$OUT/resume")"
grep -q '^tile: .* rowa__ident__100-vk.png$' "$OUT/resume/bk2.rowa.stamp" \
  && grep -qx "row: $ROW_A" "$OUT/resume/bk2.rowa.stamp" \
  && [[ $(grep -c '^device: ' "$OUT/resume/bk2.rowa.stamp") -eq 3 ]] \
  && pass "stamp fields: row line, tiles, device lines" || fail "stamp fields" "$(cat "$OUT/resume/bk2.rowa.stamp")"
cp -a "$OUT" "$W/snapshot"

# Run rs_import on a fresh copy of the interrupted attempt. args: [filter]
import_copy() {
  OUT="$W/cand"; rm -rf "$OUT" "$OUT.resume-prev"; cp -a "$W/snapshot" "$OUT"
  rs_init "$OUT" "$W/bin" default
  rs_import "$OUT" "${1:-}" "" >"$W/import.log"
  rs_census "$OUT"
}
stamped() { [[ -f "$OUT/resume/$1.stamp" ]]; }   # args: kind.key

import_copy
if [[ $RS_IMPORTED_ROWS -eq 3 && $RS_IMPORTED_PNG -eq 6 ]] && stamped bk2.rowa && stamped "freecam.$SLUG" \
   && stamped credits.credits && [[ -f "$OUT/credits-verdict.txt" && ! -e "$OUT/rowb__canon__100-vk.png" \
   && ! -e "$OUT/stray.txt" && ! -e "$OUT.resume-prev" && $(ls "$OUT"/*-vk.png | wc -l) -eq 6 \
   && $(wc -l <"$OUT/capture-device.txt") -eq 6 ]]; then
  pass "resume: 3 rows, 6 tiles imported; partial row, stray file and the previous directory gone"
else fail resume "rows=$RS_IMPORTED_ROWS png=$RS_IMPORTED_PNG; $(cat "$W/import.log"); $(ls "$OUT")"; fi

# Tampered: one byte of a stamped tile.
OUT="$W/snapshot"; printf 'X' | dd of="$W/snapshot/rowa__inter__100-vk.png" bs=1 seek=7 conv=notrunc 2>/dev/null
import_copy
if ! stamped bk2.rowa && [[ ! -e "$OUT/rowa__canon__100-vk.png" && $RS_IMPORTED_ROWS -eq 2 ]] \
   && grep -q 'bk2.rowa.stamp: tile rowa__inter__100-vk.png digest differs' "$W/import.log"; then
  pass "tampered: the row with a changed tile is refused, the other rows imported"
else fail tampered "rows=$RS_IMPORTED_ROWS; $(cat "$W/import.log")"; fi
rm -rf "$W/snapshot"; cp -a "$W/attempt1" "$W/snapshot"

# Every other identity mismatch imports nothing (or nothing of the kind named).
expect_none() {  # case-name
  if [[ $RS_IMPORTED_ROWS -eq 0 && -z "$(ls -A "$OUT" | grep -v '^resume$')" ]]; then
    pass "$1: nothing imported, the directory holds no tile"
  else fail "$1" "rows=$RS_IMPORTED_ROWS; $(cat "$W/import.log")"; fi
}
OPENPETE_RESUME_RUN=run-2 import_copy; expect_none new-run
cp "$W/bin" "$W/bin.orig"; head -c 16 /dev/urandom >>"$W/bin"; import_copy; expect_none binary
cp "$W/bin.orig" "$W/bin"
cp "$W/fields.txt" "$W/fields.orig"; echo 'generated-tree: ef' >>"$W/fields.txt"; import_copy; expect_none tree
cp "$W/fields.orig" "$W/fields.txt"
OPENPETE_CAP_LODS="original extended" import_copy
[[ $RS_IMPORTED_ROWS -eq 1 ]] && stamped credits.credits && pass "params: no corpus row imported under another LOD set" \
  || fail params "rows=$RS_IMPORTED_ROWS; $(cat "$W/import.log")"
cp "$PG_CORPUS_MANIFEST" "$W/manifest.orig"
sed -i 's/| 150$/| 150,160/' "$PG_CORPUS_MANIFEST"; import_copy
! stamped bk2.rowa && grep -q 'bk2.rowa.stamp: row line is not a line of' "$W/import.log" && [[ $RS_IMPORTED_ROWS -eq 2 ]] \
  && pass "row-edit: the edited row is refused" || fail row-edit "$(cat "$W/import.log")"
cp "$W/manifest.orig" "$PG_CORPUS_MANIFEST"
import_copy rowb
! stamped bk2.rowa && [[ $RS_IMPORTED_ROWS -eq 2 ]] && pass "filter: a row outside the bk2 filter is refused" \
  || fail filter "$(cat "$W/import.log")"

# An interrupted import: prev beside a partial outdir.
OUT="$W/cand"; rm -rf "$OUT" "$OUT.resume-prev"; cp -a "$W/snapshot" "$OUT.resume-prev"
mkdir -p "$OUT"; echo partial >"$OUT/partial-vk.png"
rs_init "$OUT" "$W/bin" default; rs_import "$OUT" "" "" >"$W/import.log"; rs_census "$OUT"
[[ $RS_IMPORTED_ROWS -eq 3 && ! -e "$OUT/partial-vk.png" && ! -e "$OUT.resume-prev" ]] \
  && pass "prev-left: imported from the earlier directory, the partial outdir replaced" \
  || fail prev-left "rows=$RS_IMPORTED_ROWS; $(ls "$OUT")"

# Three legs finishing at once write one stamp.
OUT="$W/race"; mkdir -p "$OUT"; rs_init "$OUT" "$W/bin" default
for lg in canon inter ident; do leg bk2 rowa "$ROW_A" "original-$lg" "rowa__${lg}__100-vk.png" & done; wait
[[ $(ls "$OUT"/resume/*.stamp 2>/dev/null | wc -l) -eq 1 && $(grep -c '^tile: ' "$OUT/resume/bk2.rowa.stamp") -eq 3 \
   && ! -e "$OUT/resume/legs/bk2.rowa" ]] && pass "race: one stamp with 3 tiles" \
  || fail race "$(ls -R "$OUT/resume"; cat "$OUT/resume/bk2.rowa.stamp" 2>/dev/null)"

# Off: no run id, no stamp.
unset OPENPETE_RESUME_RUN
OUT="$W/off"; mkdir -p "$OUT"; rs_init "$OUT" "$W/bin" default
for lg in canon inter ident; do leg bk2 rowa "$ROW_A" "original-$lg" "rowa__${lg}__100-vk.png"; done
[[ $RS_ON -eq 0 && ! -e "$OUT/resume" ]] && pass "off: no stamp without OPENPETE_RESUME_RUN" || fail off "$(ls -R "$OUT")"

if [[ $fails -ne 0 ]]; then echo "[corpus-resume] FAIL: $fails case(s)"; exit 1; fi
echo "[corpus-resume] PASS: every case"
exit 0
