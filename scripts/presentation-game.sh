#!/usr/bin/env bash
#
# presentation-game.sh — per-game paths for the presentation-golden scripts.
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
#   PG_GOLDEN            the committed golden image directory
#   PG_IDENT_BASELINE    the identity leg's per-tile baseline
#   PG_CORPUS_MANIFEST   the bk2 corpus manifest (may not exist)
#   PG_FREECAM_MANIFEST  the freecam pose manifest (may not exist)
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
  PG_GOLDEN="tests/golden/$PG_GAME/presentation"
  PG_IDENT_BASELINE="$PG_GOLDEN/ident-baseline.tsv"

  # Every game's corpus manifests sit with the rest of its per-game data under
  # games/<slug>/, where the oracle recipe and the state-region manifest live.
  PG_CORPUS_MANIFEST="games/$PG_GAME/presentation-corpus.manifest"
  PG_FREECAM_MANIFEST="games/$PG_GAME/freecam-corpus.manifest"
  return 0
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
