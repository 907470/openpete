#!/usr/bin/env bash
#
# presentation-game.sh - per-game paths for the presentation-golden scripts.
# SOURCED, not executed.
#
# The presentation gate and its capture, diff and promote scripts are one
# implementation across the three games; everything that differs between them
# is a path derived from the game slug here, so no script carries a second
# copy of the layout.
#
# Usage:
#   source scripts/presentation-game.sh
#   GAME="$(pg_parse_game "$@")" || exit 64     # --game SLUG / --game=SLUG
#   pg_resolve "$GAME"           || exit 64     # sets the PG_* variables
#
# pg_resolve sets:
#   PG_GAME              the slug
#   PG_BIN               the game's openpete binary (OPENPETE_BIN overrides)
#   PG_DISC              the game's disc image (OPENPETE_DISC overrides)
#   PG_RUN_ARGS          per-game boot flags the game needs (array)
#   PG_BK2_DIR           where the corpus bk2 movies live
#   PG_TIER              the corpus tier (PRESENTATION_TIER, default presentation)
#   PG_GOLDEN            the tier's golden directory (manifest and baseline)
#   PG_BLESSED           the tier's verdict manifest, $PG_GOLDEN/blessed.tsv
#   PG_IDENT_BASELINE    the identity leg's per-tile baseline
#   PG_CORPUS_DIR        the game's corpus manifest directory
#   PG_CORPUS_MANIFEST   the tier's row manifest (bk2 and pose rows)
#   PG_FREECAM_MANIFEST  the freecam pose manifest; empty for a tier without one
#   PG_CAND_DIR          the tier's default kept-candidate directory
#
# PG_BUILD_DIR defaults to build/linux-release and is honoured by pg_resolve,
# so a caller parsing --build-dir=DIR sets it before calling.

PG_GAMES="spyro-1 spyro-2 spyro-3"

# Read the game slug out of an argument list. Both spellings are accepted;
# check_psx_oracle.sh uses the two-word form and the capture scripts take
# positional arguments, where an extra word would shift them.
pg_parse_game() {
  local a next=0 game="spyro-1"
  for a in "$@"; do
    if [[ "$a" == "--help" || "$a" == "-h" ]]; then
      # Every harness that shares this parser prints its usage header and
      # exits 64 instead of starting a run; the caller treats the failed
      # parse as exit 64.
      sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//' >&2
      return 1
    fi
    if   (( next ));                then game="$a"; next=0
    elif [[ "$a" == "--game" ]];    then next=1
    elif [[ "$a" == --game=* ]];    then game="${a#--game=}"
    fi
  done
  case " $PG_GAMES " in
    *" $game "*) ;;
    *) echo "unknown --game '$game' (expected one of: $PG_GAMES)" >&2; return 1;;
  esac
  echo "$game"
}

# The binary is named after the slug with the separator dropped, matching the
# per-game executable target name.
pg_bin_name() { echo "openpete-${1//-/}"; }

pg_resolve() {
  PG_GAME="$1"
  local build="${PG_BUILD_DIR:-build/linux-release}"
  PG_BIN="${OPENPETE_BIN:-$build/games/$PG_GAME/$(pg_bin_name "$PG_GAME")}"

  # The disc path and the game's own boot flags already have one authority,
  # the oracle recipe. It is sourced in a subshell so its other variables
  # (GOLDEN, MANIFEST) cannot shadow the caller's.
  local conf="games/$PG_GAME/oracle.conf"
  local disc="" runargs=""
  if [[ -f "$conf" ]]; then
    disc="$(source "$conf" >/dev/null 2>&1; echo "$DISC")"
    runargs="$(source "$conf" >/dev/null 2>&1; echo "${RUN_ARGS[*]-}")"
  fi
  PG_DISC="${OPENPETE_DISC:-$disc}"
  PG_RUN_ARGS=()
  [[ -n "$runargs" ]] && read -ra PG_RUN_ARGS <<<"$runargs"

  PG_BK2_DIR="games/$PG_GAME/bk2"
  PG_CORPUS_DIR="$(pg_corpus_dir "$PG_GAME")"

  # A tier is a corpus with its own manifest, golden and candidate directory,
  # captured and diffed by the same scripts. `presentation` is the gate the
  # default chains run; `dragons` (Spyro 1's dragon-rescue cutscenes) runs on
  # its trigger policy through tests/harness/check_dragon_golden.sh and has no
  # freecam leg.
  PG_TIER="${PRESENTATION_TIER:-presentation}"
  case "$PG_TIER" in
    presentation)
      PG_GOLDEN="tests/golden/$PG_GAME/presentation"
      PG_CORPUS_MANIFEST="$PG_CORPUS_DIR/presentation-corpus.manifest"
      PG_FREECAM_MANIFEST="$PG_CORPUS_DIR/freecam-corpus.manifest"
      PG_CAND_DIR="build/presentation-candidate"
      [[ "$PG_GAME" != "spyro-1" ]] && PG_CAND_DIR="build/presentation-candidate-$PG_GAME"
      ;;
    dragons)
      PG_GOLDEN="tests/golden/$PG_GAME/dragons"
      PG_CORPUS_MANIFEST="$PG_CORPUS_DIR/dragon-rescue-corpus.manifest"
      PG_FREECAM_MANIFEST=""
      PG_CAND_DIR="build/dragon-candidate"
      [[ "$PG_GAME" != "spyro-1" ]] && PG_CAND_DIR="build/dragon-candidate-$PG_GAME"
      ;;
    *) echo "unknown PRESENTATION_TIER '$PG_TIER' (expected presentation or dragons)" >&2; return 1;;
  esac
  PG_BLESSED="$PG_GOLDEN/blessed.tsv"
  PG_IDENT_BASELINE="$PG_GOLDEN/ident-baseline.tsv"
  return 0
}

# ---- capture image ---------------------------------------------------------
#
# A presentation verdict is defined over the pinned capture image
# (tests/harness/linux-capture.Dockerfile), which carries its recipe at
# PG_IMAGE_RECIPE; pg_in_capture_image is true inside it.
#
# pg_enter_capture_image [--no-build|--no-engine] SCRIPT ARGS... runs SCRIPT
# with ARGS inside the image through `scripts/golden-capture-container.sh
# gate`, which builds the engine into PG_CONTAINER_BUILD_DIR with the image's
# toolchain under an exclusive lock, then runs SCRIPT there; a --build-dir=
# argument is replaced by PG_CONTAINER_BUILD_DIR, since the host's build tree
# holds host-compiled objects. --no-build enters through the wrapper's `run`
# instead, for a SCRIPT that runs no engine but reads the build directory's
# name (its preset); `run` builds the image when it is absent or stale rather
# than skipping. When OPENPETE_CAPTURE_BINARIES is set (a tree whose engine
# was built elsewhere, its binaries' hashes in that file), both forms enter
# through `gate --no-build`, which builds nothing and runs SCRIPT only after
# the shipped binaries match their hashes. --no-engine enters through `run`
# whatever OPENPETE_CAPTURE_BINARIES holds and passes ARGS unchanged, with no
# --build-dir= argument, for a SCRIPT that takes no build directory and runs
# no engine. The machine's
# hostname enters as OPENPETE_CAPTURE_HOST, since inside the container the
# hostname is the container id (scripts/presentation-blessed.py host_here).
# The caller stays the host-side parent for the whole run, so a process
# listing on the host shows the gate in flight. Returns the inner
# run's status; 64 (SKIP) when no container engine or no current image exists.
PG_IMAGE_RECIPE=/etc/openpete/capture.Dockerfile
PG_CONTAINER_BUILD_DIR=build/linux-release-container

pg_in_capture_image() { [[ -f "$PG_IMAGE_RECIPE" ]]; }

pg_enter_capture_image() {  # [--no-build|--no-engine] script args...
  if [[ "$1" == "--no-engine" ]]; then
    shift
    scripts/golden-capture-container.sh run -- \
      env "OPENPETE_CAPTURE_HOST=$(uname -n)" "$@"
    return
  fi
  local -a sub=(gate)
  [[ "$1" == "--no-build" ]] && { sub=(run); shift; }
  [[ -n "${OPENPETE_CAPTURE_BINARIES:-}" ]] && sub=(gate --no-build)
  local script="$1" a; shift
  local -a args=()
  for a in "$@"; do [[ "$a" == --build-dir=* ]] || args+=("$a"); done
  scripts/golden-capture-container.sh "${sub[@]}" -- \
    env "OPENPETE_CAPTURE_HOST=$(uname -n)" "$script" \
    "--build-dir=$PG_CONTAINER_BUILD_DIR" "${args[@]}"
}

# Append the engine's Vulkan device line (`[rhi/vk] device='<name>' ...
# driver-info='<info>'`) from a capture log to OUTDIR/capture-device.txt, from
# which scripts/presentation-blessed.py reads the Mesa and device strings a
# manifest header records. args: log outdir
pg_note_device() {
  pg_device_line "$1" >>"$2/capture-device.txt"
  return 0
}

# Print a capture log's first Vulkan device line (empty when it has none).
# args: log
pg_device_line() {
  grep -a -o "\[rhi/vk\] device='[^']*'.* driver-info='[^']*'" "$1" 2>/dev/null | head -1
  return 0
}

# Capture settings both corpora read from the environment, defaults in one
# place (a resumed attempt compares them against a row's completion stamp):
#   PG_CAP_ASPECT     OPENPETE_CAP_ASPECT, default 16:9 (the golden is widescreen)
#   PG_CAP_LODS       OPENPETE_CAP_LODS, default "original extended"
#   PG_MAX_SAMPLES    OPENPETE_MAX_SAMPLES, canonical samples per bk2, default 5
#   PG_FREECAM_START  OPENPETE_FREECAM_START, a freecam row's default canonical VBL, 1900
pg_cap_settings() {
  PG_CAP_ASPECT="${OPENPETE_CAP_ASPECT:-16:9}"
  PG_CAP_LODS="${OPENPETE_CAP_LODS:-original extended}"
  PG_MAX_SAMPLES="${OPENPETE_MAX_SAMPLES:-5}"
  PG_FREECAM_START="${OPENPETE_FREECAM_START:-1900}"
}

# A freecam row's tile key: the row's level field plus the first six hex
# digits of the pose's md5, so two poses in one level never collide and the
# key survives re-ordering the manifest. args: level-field pose
pg_pose_slug() {
  local h; h="$(printf '%s' "$2" | md5sum | cut -c1-6)"
  echo "${1}-${h}"
}

# ---- corpus manifests ------------------------------------------------------
#
# A corpus manifest lists the rows a harness runs. Each game's corpus
# manifests sit in games/<slug>/corpus/; games/<slug>/manifest/ holds the
# recompiler's state manifests, which no harness row set reads.
#
# Every harness loads its manifest through pg_manifest_load, so a manifest is
# found, counted and reported one way:
#   - a missing manifest is FAIL: the message names the path and the script
#     exits 1. A wrong path is a defect in the harness, never a skip.
#   - the row count under the manifest's row rule is printed as
#     `rows=N path=<manifest>` and left in PG_MANIFEST_ROWS. A manifest with
#     zero rows is not refused here; the harness keeps its own zero-row exit.
#   - OPENPETE_MANIFEST_DRY_RUN=1 makes pg_manifest_dry_run_exit exit 0, so a
#     harness run under it loads and reports its manifests and runs nothing.
#
# Row rules:
#   line  strip from the first `#`; a row is a line left with a non-blank
#         character.
#   pipe  split on `|`; a row is a line whose trimmed first field is non-empty
#         and does not start with `#`.

pg_corpus_dir() { echo "games/$1/corpus"; }            # args: slug
pg_corpus_manifest() { echo "games/$1/corpus/$2"; }    # args: slug file-name

# args: path form(line|pipe)
pg_manifest_load() {
  local path="$1" form="$2"
  if [[ ! -f "$path" ]]; then
    echo "FAIL: corpus manifest missing: $path" >&2
    exit 1
  fi
  case "$form" in
    line) PG_MANIFEST_ROWS="$(awk '{ sub(/#.*/, "") } /[^[:space:]]/ { n++ }
                                   END { print n + 0 }' "$path")";;
    pipe) PG_MANIFEST_ROWS="$(awk -F'|' '{ f = $1; gsub(/^[[:space:]]+|[[:space:]]+$/, "", f) }
                                         f != "" && f !~ /^#/ { n++ }
                                         END { print n + 0 }' "$path")";;
    *) echo "FAIL: pg_manifest_load: unknown row form '$form' for $path" >&2; exit 1;;
  esac
  echo "rows=$PG_MANIFEST_ROWS path=$path"
}

pg_manifest_dry_run_exit() {
  if [[ "${OPENPETE_MANIFEST_DRY_RUN:-0}" == "1" ]]; then exit 0; fi
  return 0
}

# One manifest, then the dry-run exit. args: path form(line|pipe)
pg_manifest() {
  pg_manifest_load "$1" "$2"
  pg_manifest_dry_run_exit
}

# ---- per-row launch field --------------------------------------------------
#
# A corpus row's optional `launch` field holds whitespace-separated tokens
# applied to every leg of that row and to no other row. A token spelled
# KEY=VALUE with an upper-case key is an environment entry; every other token
# is a command-line argument. Results land in the caller's ROW_ENV and ROW_ARGS
# arrays, both emptied first so a row without the field adds nothing.
pg_split_launch() {  # launch-field
  ROW_ENV=(); ROW_ARGS=()
  local tok
  for tok in $1; do
    if [[ "$tok" == [A-Z_]*=* ]]; then ROW_ENV+=("$tok"); else ROW_ARGS+=("$tok"); fi
  done
}

# ---- failed-leg logs -------------------------------------------------------
#
# A capture leg's log is the only record of why it failed, and a crash writes
# its evidence to that log: the crash handler prints the fault address, the
# host RIP, the guest pc/ra and the FIRST backtrace frames (the faulting
# function) before the last frames (the dispatcher, which is the same for
# every guest-side fault). Keeping five tail lines therefore keeps the part
# that identifies nothing, and deleting the log loses a crash that does not
# reproduce. These two helpers keep the whole log beside the tiles the leg was
# supposed to write, and quote the identifying lines into the corpus log.

# Delete the crash logs of a previous run so a leg that passes this time
# cannot leave one behind to be read as this run's failure. args: outdir
pg_clear_crash_logs() {
  [[ -d "$1" ]] || return 0
  find "$1" -maxdepth 1 -type f -name '*.crash.log' -delete
}

# args: logfile outdir leg-name rc
pg_keep_crash_log() {
  local log="$1" outdir="$2" name="$3" rc="$4"
  local kept="$outdir/${name}.crash.log"
  cp -f "$log" "$kept"
  echo "  rc=$rc, full leg log kept: $kept"
  if grep -q '=== openpete crash:' "$log"; then
    echo "  --- crash handler ---"
    sed -n '/=== openpete crash:/,$p' "$log" \
      | grep -E '^=== openpete crash:|^  fault addr=|^  host RIP=|^  guest (pc|ra) =|^  MOD ATTRIBUTION:|^  host backtrace|^    #[0-2] ' \
      | sed 's/^/  /'
  else
    # No crash banner: a timeout, a refused launch or a clean exit that wrote
    # no tile. The tail is what says which.
    echo "  --- last log lines ---"
    tail -5 "$log" | sed 's/^/  /'
  fi
}
