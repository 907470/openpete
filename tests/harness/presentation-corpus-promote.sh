#!/usr/bin/env bash
#
# presentation-corpus-promote.sh — re-bless the presentation golden.
#
# Rewrites the tier's blessed.tsv rows for the promoted candidate tiles (verdict
# digest, blessed revision, recomp identity, origin), files those tiles into the
# reference store under their digests, and stages the manifest by name. No PNG
# enters the tree. Never a blind overwrite: it prints each row it adds or
# replaces and, unless --all is given, promotes only the named tiles (the
# adjudicated divergences). A tile whose digest equals its row's keeps its row.
#
# The blessed revision is read from the candidate's capture-record.txt, which
# the gate writes before it captures: HEAD is written when HEAD's tree over the
# build's source roots equals the recorded tree, and the promote is refused
# otherwise, naming the first differing path (scripts/presentation-blessed.py
# promote).
#
# Usage:
#   tests/harness/presentation-corpus-promote.sh <candidate_dir> [tile ...] [--game SLUG]
#   tests/harness/presentation-corpus-promote.sh <candidate_dir> --all
#     tile    a golden basename (e.g. cliff-town-vista__canon__300-vk.png) OR a
#             substring filter; only matching candidate tiles are promoted
#     --all   promote every -vk tile in candidate_dir (full re-bless)
#     --game  game slug (default spyro-1); selects the manifest
#     --exclude=SUBSTR
#             leave out tiles whose basename contains SUBSTR (repeatable), e.g.
#             rows held unblessed under --all
#     --environment-accept=REASON
#             admit a capture environment that departs from the manifest
#             header; the header records REASON
#   PRESENTATION_TIER=dragons selects the dragon-rescue tier's manifest.
#
# The promote runs inside the capture image, on a candidate captured there:
# the manifest header records the capture environment, and a promote whose
# capture departs from it, or which runs outside the image, is refused
# (scripts/presentation-blessed.py, Environment).
#
# After running, review `git -C <openpete> diff --cached` and commit deliberately.
#
# Exit: 0 promoted or nothing to promote; 1 refused; 2 usage.
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/presentation-game.sh
source scripts/presentation-game.sh

GAME="$(pg_parse_game "$@")" || exit 2
pg_resolve "$GAME" || exit 2
CAND="${1:-}"
shift || true

if [[ -z "$CAND" || ! -d "$CAND" ]]; then
  echo "usage: $0 <candidate_dir> [tile ...|--all]" >&2; exit 2; fi

# --game and its value are consumed above; everything else is a tile filter.
all=0; filters=(); skip=0; opts=()
for a in "$@"; do
  if   (( skip ));                then skip=0
  elif [[ "$a" == "--all" ]];     then all=1
  elif [[ "$a" == --exclude=* ]]; then opts+=(--exclude "${a#--exclude=}")
  elif [[ "$a" == --environment-accept=* ]]; then opts+=(--environment-accept "${a#--environment-accept=}")
  elif [[ "$a" == "--game" ]];    then skip=1
  elif [[ "$a" == --game=* ]];    then :
  else                                 filters+=("$a")
  fi
done
if [[ $all -eq 0 && ${#filters[@]} -eq 0 ]]; then
  echo "usage: $0 <candidate_dir> [tile ...|--all]" >&2; exit 2; fi

ALL_ARG=()
[[ $all -eq 1 ]] && ALL_ARG=(--all)
before="$(git hash-object "$PG_BLESSED" 2>/dev/null)"
python3 scripts/presentation-blessed.py promote "$GAME" --tier "$PG_TIER" \
  "$CAND" "${ALL_ARG[@]}" "${opts[@]}" -- "${filters[@]}" || exit 1
if [[ "$(git hash-object "$PG_BLESSED")" == "$before" ]]; then exit 0; fi

git add -- "$PG_BLESSED"
echo "[presentation-promote] $GAME: staged $PG_BLESSED — review 'git diff --cached' and commit"
