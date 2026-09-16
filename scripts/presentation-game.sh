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

  # Spyro 1's manifests keep their original scripts/ paths so the corpus they
  # describe, and the golden captured from it, are untouched. A second game's
  # manifests sit with the rest of its per-game data under games/<slug>/,
  # where the oracle recipe and the state-region manifest already live.
  if [[ "$PG_GAME" == "spyro-1" ]]; then
    PG_CORPUS_MANIFEST="scripts/presentation-corpus.manifest"
    PG_FREECAM_MANIFEST="scripts/freecam-corpus.manifest"
  else
    PG_CORPUS_MANIFEST="games/$PG_GAME/presentation-corpus.manifest"
    PG_FREECAM_MANIFEST="games/$PG_GAME/freecam-corpus.manifest"
  fi
  return 0
}
