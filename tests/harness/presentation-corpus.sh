#!/usr/bin/env bash
#
# presentation-corpus.sh — capture the native -vk presentation corpus.
#
# For every bk2 in the manifest (filtered), boot the game headless under
# software Vulkan (llvmpipe, deterministic) and capture two legs on the
# native-only path:
#   canonical   VBL-keyed frames                -> <name>__canon__<frame>-vk.png
#   intermediate deterministic alpha=0.5 frames  -> <name>__inter__<frame>-vk.png
#   identity     alpha=1 re-extract on the canon frames -> <name>__ident__<frame>-vk.png
#                (not a golden tile: presentation-ident-check.py diffs it against the
#                 canon tile of the same frame from the same capture)
# Both legs are gameplay-neutral display captures; the same-binary depth kill
# switch (OPENPETE_DEPTH_AFFINE) selects legacy vs candidate depth so golden and
# candidate differ ONLY by the change under test.
#
# Usage:
#   tests/harness/presentation-corpus.sh <off|on> <outdir> [bk2-filter] [--game SLUG]
#     off|on     OPENPETE_DEPTH_AFFINE for the run (default off = HEAD behavior)
#     outdir     where the -vk PNGs are written (created)
#     bk2-filter substring; only manifest rows whose name contains it are run
#                (default: cliff-town-vista, the single-bk2 bring-up scope)
#     --game     game slug (default spyro-1); selects binary, disc, bk2 dir
#                and manifest through scripts/presentation-game.sh
#
# Exit: 0 = all captures produced, 1 = a capture is missing (hard failure) or
#           the corpus manifest is missing (the message names the path),
#       64 = SKIP (binary / disc / Vulkan loader missing),
#       65 = no rows (the manifest holds none, or the filter matched none) —
#            the caller decides whether an empty corpus is a skip or a
#            vacuous run.
#
# See notes/plans/openpete-presentation-golden.md.
set -u

# Unattended launcher: silence controller vibration for every run this script
# starts. Vibration ships on by default, which is right for a player and wrong
# for a machine-driven run — nobody is holding the pad, and the buzzing shows
# up in no log, so it would never surface on its own. Set here rather than at
# each invocation because this script builds its argument list in more than one
# place, and a flag appended to the wrong one is a silent miss.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0


cd "$(dirname "$0")/../.."
# shellcheck source=scripts/corpus-jobs.sh
source scripts/corpus-jobs.sh
# shellcheck source=scripts/presentation-game.sh
source scripts/presentation-game.sh

AFFINE="${1:-off}"
OUTDIR="${2:-}"
FILTER="${3:-}"   # bk2-name substring; empty = the whole manifest (the fanned-out corpus)

# Cap the canonical samples per bk2 to this many evenly-spaced frames (0 = the
# full every-`interval` grid). Keeps the golden broad across bk2s without a dense
# per-bk2 grid; the intermediate leg mirrors the kept canonical frames.
MAX_SAMPLES="${OPENPETE_MAX_SAMPLES:-5}"

# Capture aspect. The golden is ALWAYS widescreen so it catches Hor+ artifacts
# the 4:3 view can't show; override for a 4:3 spot-check if ever needed.
CAP_ASPECT="${OPENPETE_CAP_ASPECT:-16:9}"

# LOD modes swept per bk2/leg — both by default (original = authentic PSX band,
# extended = --no-lod full detail). Override to one mode for a faster spot-check.
CAP_LODS="${OPENPETE_CAP_LODS:-original extended}"

# --build-dir=DIR override (ctest passes this); else the default release tree.
BUILD_DIR="build/linux-release"
for a in "$@"; do case "$a" in --build-dir=*) BUILD_DIR="${a#--build-dir=}";; esac; done

GAME="$(pg_parse_game "$@")" || exit 64
PG_BUILD_DIR="$BUILD_DIR" pg_resolve "$GAME"
BIN="$PG_BIN"
DISC="$PG_DISC"
MANIFEST="$PG_CORPUS_MANIFEST"

if [[ -z "$OUTDIR" ]]; then echo "usage: $0 <off|on|default> <outdir> [bk2-filter]" >&2; exit 64; fi
# The golden is captured at `default` (the compiled OPENPETE_DEPTH_AFFINE default,
# now screen-affine), so it tracks whatever ships. off/on force the env for the
# manual depth A/B (off = legacy encoding, the kill switch).
case "$AFFINE" in
  off|0)        AFFINE_ENV=0;;
  on|1)         AFFINE_ENV=1;;
  default|def)  AFFINE_ENV=default;;
  *) echo "presentation-corpus: first arg must be off|on|default (got '$AFFINE')" >&2; exit 64;; esac

pg_manifest "$MANIFEST" pipe

if [[ ! -x "$BIN" ]]; then echo "[presentation-corpus] SKIP: no binary at $BIN"; exit 64; fi
if [[ ! -f "$DISC" ]]; then echo "[presentation-corpus] SKIP: no disc at $DISC"; exit 64; fi
if ! ldconfig -p 2>/dev/null | grep -q 'libvulkan\.so\.1'; then
  echo "[presentation-corpus] SKIP: no Vulkan loader (libvulkan.so.1)"; exit 64; fi

# Deterministic software raster (see the plan's pinned-rasterizer requirement).
LVP_ICD="/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
if [[ -e "$LVP_ICD" ]]; then export VK_ICD_FILENAMES="$LVP_ICD"; fi
export MESA_LOADER_DRIVER_OVERRIDE="${MESA_LOADER_DRIVER_OVERRIDE:-llvmpipe}"
# `default` leaves OPENPETE_DEPTH_AFFINE unset so the binary's compiled default
# applies (the golden basis); off/on force the A/B legs.
if [[ "$AFFINE_ENV" != "default" ]]; then export OPENPETE_DEPTH_AFFINE="$AFFINE_ENV"; else unset OPENPETE_DEPTH_AFFINE; fi

mkdir -p "$OUTDIR"
pg_clear_crash_logs "$OUTDIR"
echo "[presentation-corpus] game=$GAME bin=$BIN affine=$AFFINE_ENV aspect=$CAP_ASPECT filter='$FILTER' -> $OUTDIR"

# The golden filename encodes the LOD mode: `original` (the authentic PSX band)
# stays UNTAGGED so its golden is unchanged, `extended` (--no-lod, full detail at
# any distance) gets a `lodext__` tag between the bk2 name and the leg. Extended
# LOD pulls in far terrain / moby tiers the original band culls — an
# independently-regressable surface the depth work is most likely to disturb.
lod_tag() { [[ "$1" == "extended" ]] && echo "lodext__" || echo ""; }

# A row name is the bk2 basename optionally followed by `@<variant>`. The whole
# name is the file key, so the variant's tiles carry it and never collide with
# the plain row's; the bk2 is everything before the first `@`.
row_bk2() { echo "${1%%@*}"; }

run_one() {  # name frame_limit leg flag targets lodmode freeze launch
  local name="$1" flim="$2" leg="$3" flag="$4" targets="$5" lod="$6" freeze="$7" launch="${8:-}"
  local bk2name; bk2name="$(row_bk2 "$name")"
  local -a ROW_ENV=() ROW_ARGS=(); pg_split_launch "$launch"
  local tag; tag="$(lod_tag "$lod")"
  local out="$OUTDIR/${name}__${tag}${leg}__{n}.png"
  local log; log="$(mktemp)"
  # A freeze-marked row (a bk2 that reaches a pause / flight-results / level-exit
  # state) needs the green freeze backdrop maintained during capture, else the
  # freeze frames dump dark (frozen_tex is a live-window-only snapshot). Off
  # elsewhere — it renders an extra frame per present.
  local freeze_arg=(); [[ -n "$freeze" ]] && freeze_arg=(--screenshot-freeze-backdrop)
  # A row with no movie file is the plain boot: the title screen and the attract
  # demo, which need no input. Everything else about the row is the same.
  local bk2_arg=(); [[ -f "$PG_BK2_DIR/${bk2name}.bk2" ]] && bk2_arg=(--bk2 "$PG_BK2_DIR/${bk2name}.bk2")
  # The identity leg is the intermediate re-extract phased onto the newest tick's
  # stamp (alpha 1), which must reproduce the canonical dump of the same frame.
  local -a leg_env=(); [[ "$leg" == ident ]] && leg_env=(OPENPETE_INTERMEDIATE_ALPHA=1)
  # Always capture at 16:9 so the golden catches widescreen (Hor+) artifacts —
  # the revealed side world and the reveal-edge seams a 4:3 dump can't see. The
  # headless native-only capture honors --aspect (openpete_native_widescreen_factor
  # surfaces the fixed aspect when there is no window) and renders the widened
  # view into a dedicated capture target, so both legs are true Hor+ (853x448).
  # --mute 1: harness runs still open the host audio device; keep them
  # silent (a fanned-out corpus otherwise plays dozens of instances).
  # --no-config: golden images are compared pixel-for-pixel, so the run has
  # to be independent of any openpete.toml on the machine capturing them.
  # The row's own launch env and arguments go last, on every leg of the row, so
  # a variant row differs from its plain twin by exactly that field.
  env "${leg_env[@]}" "${ROW_ENV[@]}" timeout 300 "$BIN" --headless --no-config --disc "$DISC" --disc-cache 0 --renderer vk \
    --mute 1 --aspect "$CAP_ASPECT" --lod "$lod" "${freeze_arg[@]}" "${PG_RUN_ARGS[@]}" \
    "${bk2_arg[@]}" --screenshot-native-only \
    "$flag" "$targets" --screenshot-output "$out" \
    --frame-limit "$flim" --max-runtime-seconds 280 "${ROW_ARGS[@]}" >"$log" 2>&1
  local rc=$?
  # Assert every requested target produced a -vk PNG (fail loudly, never silently
  # capture a partial corpus that would read as "all clean" downstream).
  local miss=0 t
  IFS=',' read -ra TS <<<"$targets"
  for t in "${TS[@]}"; do
    if [[ ! -f "$OUTDIR/${name}__${tag}${leg}__${t}-vk.png" ]]; then
      echo "  MISSING ${name}__${tag}${leg}__${t}-vk.png (rc=$rc)"; miss=1; fi
  done
  if [[ $rc -ne 0 || $miss -ne 0 ]]; then
    pg_keep_crash_log "$log" "$OUTDIR" "${name}__${tag}${leg}" "$rc"
  fi
  rm -f "$log"
  return $miss
}

# Authoritative movie length = the number of input-frame lines ('|'-prefixed) in
# the bk2's Input Log.txt — matches the binary's own bk2-end frame exactly. The
# frame-limit is one cycle past that (render the last input's result, then stop),
# and the sample grid runs up to it, so nothing is hardcoded to a bk2's length.
bk2_frames() {
  python3 - "$1" <<'PY'
import zipfile, sys
try:
    z = zipfile.ZipFile(sys.argv[1])
    print(sum(1 for l in z.read('Input Log.txt').decode('latin-1').splitlines()
              if l.startswith('|')))
except Exception:
    print(0)
PY
}

# Emit the sample grid for one bk2 as "<canon_csv>|<inter_csv>|<flim>". The base
# grid steps by `interval` from `start`, extended one point PAST the movie end so
# the final settled frame is captured. With MAX_SAMPLES>0 the canonical points are
# then subsampled to at most MAX_SAMPLES EVENLY-SPACED frames (first and last kept)
# — broad within-movie coverage without one dense golden per bk2. The intermediate
# leg is each kept canonical frame + interval/2 (the +half offset that lands the
# deterministic alpha=0.5 present between snapshot stamps).
#
# PINNED frames (the row's 5th field) are added on top of that grid and are never
# subsampled away. The evenly-spaced grid is derived from movie length, so it lands
# wherever the arithmetic puts it and routinely misses the frame a defect was actually
# observed at; a pin names that frame explicitly. A pin contributes BOTH legs at the
# SAME frame (canon@F and inter@F, not F+interval/2), because the point is to hold the
# exact frame someone looked at, on the canonical and the sub-tick surface alike.
# Pins are additive: the grid's own samples keep the numbers they already have, so
# adding a pin re-blesses nothing that already exists. args: start fc interval maxs pins
compute_grid() {
  python3 - "$1" "$2" "$3" "$4" "${5:-}" <<'PY'
import sys
start, fc, interval, maxs = (int(x) for x in sys.argv[1:5])
pins = sorted({int(x) for x in (sys.argv[5] if len(sys.argv) > 5 else "").split(",") if x.strip()})
end = start + interval * ((fc - start) // interval + 1)
grid = list(range(start, end + 1, interval))
if maxs > 0 and len(grid) > maxs:
    idx = sorted(set(round(i * (len(grid) - 1) / (maxs - 1)) for i in range(maxs)))
    grid = [grid[j] for j in idx]
canon = sorted(set(grid) | set(pins))
inter = sorted({p + interval // 2 for p in grid} | set(pins))
flim = max(canon[-1], inter[-1]) + 1
print(",".join(map(str, canon)) + "|" + ",".join(map(str, inter)) + "|" + str(flim))
PY
}

fail=0 ran=0
while IFS='|' read -r name start interval freeze pins launch; do
  name="$(echo "$name" | tr -d ' \t')"
  [[ -z "$name" || "${name:0:1}" == "#" ]] && continue
  [[ "$name" == *"$FILTER"* ]] || continue
  start="$(echo "$start" | tr -d ' \t')"
  interval="$(echo "$interval" | tr -d ' \t')"; interval="${interval:-100}"
  freeze="$(echo "$freeze" | tr -d ' \t')"   # 4th field non-empty => --screenshot-freeze-backdrop
  pins="$(echo "$pins" | tr -d ' \t')"       # 5th field: explicit frames, never subsampled
  # 6th field: per-row launch tokens (CLI args and KEY=VALUE env entries). Only
  # the surrounding whitespace is trimmed; the tokens keep their own spacing.
  launch="${launch:-}"; launch="$(echo "$launch" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"
  bk2="$PG_BK2_DIR/$(row_bk2 "$name").bk2"
  if [[ -f "$bk2" ]]; then
    fc="$(bk2_frames "$bk2")"
    # A row that starts at or past the movie end samples the game running on
    # without input (e.g. a scripted freecam engaged after the last input);
    # its length is its last pin, as for a movie-less row.
    if [[ -n "$pins" && "$fc" =~ ^[0-9]+$ && "$start" -ge "$fc" ]]; then
      fc="$(tr ',' '\n' <<<"$pins" | sort -n | tail -1)"
      echo "  [$name] starts past the movie end: length from the last pin ($fc)"
    fi
  else
    # No movie: the plain boot (title screen, attract demo). The row's length is
    # its last pin, so a movie-less row must pin every frame it wants.
    if [[ -z "$pins" ]]; then echo "  SKIP row $name: no bk2 and no pins"; continue; fi
    fc="$(tr ',' '\n' <<<"$pins" | sort -n | tail -1)"
    echo "  [$name] no bk2: plain boot, length from the last pin ($fc)"
  fi
  if ! [[ "$fc" =~ ^[0-9]+$ ]] || [[ "$fc" -le "$start" ]]; then
    echo "  SKIP row $name: bad/short bk2 length ($fc) vs start ($start)"; continue; fi
  grid="$(compute_grid "$start" "$fc" "$interval" "$MAX_SAMPLES" "$pins")"
  canon="${grid%%|*}"; rest="${grid#*|}"; inter="${rest%%|*}"; flim="${rest##*|}"
  ncanon=$(awk -F, '{print NF}' <<<"$canon"); ninter=$(awk -F, '{print NF}' <<<"$inter")
  # The binary caps ONE --screenshot list at OPENPETE_MAX_SCREENSHOT_FRAMES (64);
  # MAX_SAMPLES keeps us well under that, but guard anyway (never silently truncate).
  if [[ "$ncanon" -gt 64 || "$ninter" -gt 64 ]]; then
    echo "  FAIL row $name: $ncanon canon / $ninter inter samples exceed the 64-frame"
    echo "        screenshot cap — lower OPENPETE_MAX_SAMPLES or raise this row's interval."
    fail=1; continue; fi
  ran=$((ran+1))
  echo "[$name] movie=$fc frame-limit=$flim  canon=[$canon]  inter=[$inter]  lods=[$CAP_LODS]${freeze:+  freeze}${pins:+  pins=$pins}${launch:+  launch=$launch}"
  # Sweep both LOD modes: original (untagged golden) + extended (lodext__ tag).
  for lod in $CAP_LODS; do
    cj_run run_one "$name" "$flim" canon --screenshot-on-frame "$canon" "$lod" "$freeze" "$launch"
    cj_run run_one "$name" "$flim" inter --screenshot-intermediate-on-frame "$inter" "$lod" "$freeze" "$launch"
    cj_run run_one "$name" "$flim" ident --screenshot-intermediate-on-frame "$canon" "$lod" "$freeze" "$launch"
  done
done < "$MANIFEST"
cj_wait || fail=1

if [[ $fail -ne 0 ]]; then echo "[presentation-corpus] FAIL: missing captures"; exit 1; fi
if [[ $ran -eq 0 ]]; then
  echo "[presentation-corpus] $GAME: no manifest rows matched '$FILTER' — 0 rows"; exit 65; fi
echo "[presentation-corpus] OK: $(ls "$OUTDIR"/*-vk.png 2>/dev/null | wc -l) images across $ran bk2"
exit 0
