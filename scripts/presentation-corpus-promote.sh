#!/usr/bin/env bash
#
# presentation-corpus-promote.sh — re-bless the presentation golden.
#
# Copies candidate -vk PNGs over the committed golden and stages them, so a
# reviewed re-bless commit records the deliberate image change. Never a blind
# overwrite: it prints the exact files it will replace/add and, unless --all is
# given, promotes only the named tiles (the adjudicated divergences).
#
# Usage:
#   scripts/presentation-corpus-promote.sh <candidate_dir> [tile ...]
#   scripts/presentation-corpus-promote.sh <candidate_dir> --all
#     tile    a golden basename (e.g. cliff-town-vista__canon__300-vk.png) OR a
#             substring filter; only matching candidate tiles are promoted
#     --all   promote every -vk PNG in candidate_dir (first-bless / full re-bless)
#
# After running, review `git -C <openpete> diff --stat` and commit deliberately.
#
# See notes/plans/openpete-presentation-golden.md.
set -u
cd "$(dirname "$0")/.."

CAND="${1:-}"
GOLDEN="tests/golden/spyro-1/presentation"
shift || true

if [[ -z "$CAND" || ! -d "$CAND" ]]; then
  echo "usage: $0 <candidate_dir> [tile ...|--all]" >&2; exit 2; fi

mkdir -p "$GOLDEN"
shopt -s nullglob
all=0; filters=()
for a in "$@"; do case "$a" in --all) all=1;; *) filters+=("$a");; esac; done

promoted=0
for f in "$CAND"/*-vk.png; do
  base="$(basename "$f")"
  if [[ $all -eq 0 ]]; then
    match=0
    for flt in "${filters[@]:-}"; do [[ -n "$flt" && "$base" == *"$flt"* ]] && match=1; done
    [[ $match -eq 1 ]] || continue
  fi
  if [[ -f "$GOLDEN/$base" ]] && cmp -s "$f" "$GOLDEN/$base"; then
    continue   # identical, nothing to promote
  fi
  action="add"; [[ -f "$GOLDEN/$base" ]] && action="replace"
  cp "$f" "$GOLDEN/$base"
  echo "  $action  $base"
  promoted=$((promoted+1))
done

if [[ $promoted -eq 0 ]]; then
  echo "[presentation-promote] nothing to promote (no matching / changed tiles)"; exit 0; fi

git add "$GOLDEN" 2>/dev/null || true
echo "[presentation-promote] promoted $promoted tile(s) into $GOLDEN — review 'git diff --cached --stat' and commit"
