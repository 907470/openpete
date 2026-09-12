#!/usr/bin/env bash
#
# ident-audit.sh — sweep the sub-tick spine's identity residual over the bk2 corpus.
#
# The canonical dump at VBL F composes the current scene: the newest tick's feed.
# The explicit sub-tick re-extract phased onto that same tick's stamp (alpha 1)
# must rebuild the identical scene. Where it does not, the spine is failing to
# carry some piece of gameplay state across the re-extract.
#
# The presentation corpus's `ident` leg tests that on a handful of sampled frames
# by diffing PNGs. This runs the in-binary audit (OPENPETE_IDENT_AUDIT) instead,
# which composes both scenes on EVERY canonical present and reports their pixel
# difference as one line per tick — so a whole movie is swept without writing
# thousands of images, and the residual can be read as a distribution rather than
# a handful of samples.
#
# Usage:
#   scripts/ident-audit.sh [--filter=SUBSTR] [--jobs=N] [--build-dir=DIR]
#     --filter     only manifest rows whose name contains SUBSTR (default: all 53)
#     --jobs       concurrent captures (default: corpus-jobs.sh's nproc/2)
#     --build-dir  binary tree to run (default build/linux-release)
#
# Per-row output: build/ident-audit/<name>.tsv, columns vbl/tick/px/maxch.
# Then a corpus summary sorted by each row's worst tick.
#
# Exit: 0 = every matched row produced a TSV, 1 = a row produced none,
#       64 = SKIP (binary / disc / Vulkan loader / manifest missing).
set -u

# Unattended launcher: silence controller vibration for every run this script
# starts. Nobody is holding the pad and the buzzing shows up in no log, so it
# would never surface on its own. (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

cd "$(dirname "$0")/.."
# shellcheck source=scripts/corpus-jobs.sh
source scripts/corpus-jobs.sh

FILTER=""
BUILD_DIR="build/linux-release"
for a in "$@"; do
  case "$a" in
    --filter=*)    FILTER="${a#--filter=}";;
    --jobs=*)      CORPUS_JOBS="${a#--jobs=}";;
    --build-dir=*) BUILD_DIR="${a#--build-dir=}";;
    *) echo "ident-audit: unknown argument '$a'" >&2; exit 64;;
  esac
done
[[ "$CORPUS_JOBS" -lt 1 ]] && CORPUS_JOBS=1

BIN="${OPENPETE_BIN:-$BUILD_DIR/games/spyro-1/openpete-spyro1}"
DISC="${OPENPETE_DISC:-games/spyro-1/spyro-1/build/SCUS-94228.cue}"
MANIFEST="scripts/presentation-corpus.manifest"
OUTDIR="build/ident-audit"

# The LOD band each row is swept at. One run per row by default; set to
# "original extended" to sweep both, as the presentation corpus does.
CAP_LODS="${OPENPETE_CAP_LODS:-original}"
# Capture aspect — 16:9 like the corpus, so the audit sees the Hor+ reveal too.
CAP_ASPECT="${OPENPETE_CAP_ASPECT:-16:9}"

if [[ ! -x "$BIN" ]];    then echo "[ident-audit] SKIP: no binary at $BIN"; exit 64; fi
if [[ ! -f "$DISC" ]];   then echo "[ident-audit] SKIP: no disc at $DISC"; exit 64; fi
if [[ ! -f "$MANIFEST" ]]; then echo "[ident-audit] SKIP: no manifest $MANIFEST"; exit 64; fi
if ! ldconfig -p 2>/dev/null | grep -q 'libvulkan\.so\.1'; then
  echo "[ident-audit] SKIP: no Vulkan loader (libvulkan.so.1)"; exit 64; fi

# Deterministic software raster, matching the presentation corpus.
LVP_ICD="/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
if [[ -e "$LVP_ICD" ]]; then export VK_ICD_FILENAMES="$LVP_ICD"; fi
export MESA_LOADER_DRIVER_OVERRIDE="${MESA_LOADER_DRIVER_OVERRIDE:-llvmpipe}"

mkdir -p "$OUTDIR"
echo "[ident-audit] bin=$BIN aspect=$CAP_ASPECT lods=[$CAP_LODS] jobs=$CORPUS_JOBS filter='$FILTER' -> $OUTDIR"

lod_tag() { [[ "$1" == "extended" ]] && echo "__lodext" || echo ""; }

run_one() {  # name start frame_limit lodmode freeze
  local name="$1" start="$2" flim="$3" lod="$4" freeze="$5"
  local tsv="$OUTDIR/${name}$(lod_tag "$lod").tsv"
  local log; log="$(mktemp)"
  # Stale outputs produce false verdicts: drop this row's TSV before the run and
  # assert it exists after.
  rm -f "$tsv"
  local freeze_arg=(); [[ -n "$freeze" ]] && freeze_arg=(--screenshot-freeze-backdrop)
  # Same command line as the presentation corpus's capture legs, minus the
  # screenshot targets: --screenshot-native-only keeps PsyCross out of the
  # compose (the audit's two composes are both the native-only widescreen path),
  # --no-config makes the run independent of any openpete.toml on the machine,
  # --mute keeps a fanned-out sweep silent. --screenshot-output only names where
  # OPENPETE_IDENT_AUDIT_DUMP would put its PNGs; no dump is taken without it.
  OPENPETE_IDENT_AUDIT="$start" timeout 900 "$BIN" \
    --headless --no-config --disc "$DISC" --renderer vk \
    --mute 1 --aspect "$CAP_ASPECT" --lod "$lod" "${freeze_arg[@]}" \
    --bk2 "games/spyro-1/bk2/${name}.bk2" --screenshot-native-only \
    --screenshot-output "$OUTDIR/${name}__{n}.png" \
    --frame-limit "$flim" --max-runtime-seconds 880 >"$log" 2>&1
  local rc=$?
  { printf 'vbl\ttick\tpx\tmaxch\n'
    sed -n 's/^\[ident-audit f=\([0-9]*\) tick=\([0-9]*\) px=\([0-9]*\) maxch=\([0-9]*\)\]$/\1\t\2\t\3\t\4/p' "$log"
  } > "$tsv"
  # A TSV holding only its header means the audit never engaged: report it rather
  # than letting an empty row read as a clean one.
  if [[ "$(wc -l < "$tsv")" -le 1 ]]; then
    echo "  NO AUDIT TICKS for $name/$lod (rc=$rc)"
    echo "  --- last log lines ---"; tail -5 "$log" | sed 's/^/  /'
    rm -f "$log"; return 1
  fi
  rm -f "$log"
  return 0
}

# The rows THIS invocation ran. build/ident-audit accumulates TSVs across
# filtered runs, so the summary is scoped to this list rather than to whatever
# the directory holds — a TSV left by an earlier run of a different binary would
# otherwise be reported as part of this verdict.
RUNLIST="$(mktemp)"
trap 'rm -f "$RUNLIST"' EXIT

fail=0 ran=0
while IFS='|' read -r name start interval freeze pins; do
  name="$(echo "$name" | tr -d ' \t')"
  [[ -z "$name" || "${name:0:1}" == "#" ]] && continue
  [[ "$name" == *"$FILTER"* ]] || continue
  start="$(echo "$start" | tr -d ' \t')"
  freeze="$(echo "$freeze" | tr -d ' \t')"
  bk2="games/spyro-1/bk2/${name}.bk2"
  if [[ ! -f "$bk2" ]]; then echo "  SKIP row: no bk2 $bk2"; continue; fi
  # Authoritative movie length = the input-frame lines in the bk2's Input Log,
  # matching the binary's own bk2-end frame. Run one cycle past it so the last
  # input's result is audited too.
  fc="$(python3 - "$bk2" <<'PY'
import zipfile, sys
try:
    z = zipfile.ZipFile(sys.argv[1])
    print(sum(1 for l in z.read('Input Log.txt').decode('latin-1').splitlines()
              if l.startswith('|')))
except Exception:
    print(0)
PY
)"
  if ! [[ "$fc" =~ ^[0-9]+$ ]] || [[ "$fc" -le "$start" ]]; then
    echo "  SKIP row $name: bad/short bk2 length ($fc) vs start ($start)"; continue; fi
  flim=$((fc + 1))
  ran=$((ran+1))
  echo "[$name] movie=$fc audit from f=$start to f=$flim  lods=[$CAP_LODS]${freeze:+  freeze}"
  for lod in $CAP_LODS; do
    echo "$OUTDIR/${name}$(lod_tag "$lod").tsv" >> "$RUNLIST"
    cj_run run_one "$name" "$start" "$flim" "$lod" "$freeze"
  done
done < "$MANIFEST"
cj_wait || fail=1

if [[ $ran -eq 0 ]]; then echo "[ident-audit] SKIP: no manifest rows matched '$FILTER'"; exit 64; fi

python3 scripts/ident-audit-summary.py "$RUNLIST" || fail=1
if [[ $fail -ne 0 ]]; then echo "[ident-audit] FAIL: a row produced no audit ticks"; exit 1; fi
exit 0
