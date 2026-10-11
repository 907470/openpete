#!/usr/bin/env bash

# tests/harness/check_tooltip_vocab.sh - OPENPETE_TOOLTIP_VOCAB: no tooltip,
# --help description, settings-menu hint or mod note carries a mutability
# class or a gameplay-neutrality claim, and no string a player reads carries
# an instrument term (core/config/tooltip_vocab.c holds the lists).
#
# Legs:
#   scan      the check exits 0 and its summary reports rows and strings
#             scanned above zero and no hit.
#   control   OPENPETE_TOOLTIP_VOCAB_BREAK=fov appends a barred term to the
#             fov row's tooltip; the check must exit 1 with a HIT line on fov.
#             Without it the scan leg could pass on a check that matches
#             nothing.
#
# Usage: tests/harness/check_tooltip_vocab.sh [--game SLUG] [--build-dir=DIR]
# Exit:  0 pass, 1 fail, 2 vacuous (the negative control did not fail),
#        64 skip (no binary).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

for a in "$@"; do
    case "$a" in --build-dir=*) export PG_BUILD_DIR="${a#--build-dir=}" ;; esac
done
ARGS=()
for a in "$@"; do
    case "$a" in --build-dir=*) ;; *) ARGS+=("$a") ;; esac
done

# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

GAME="$(pg_parse_game ${ARGS[@]+"${ARGS[@]}"})" || exit 64
pg_resolve "$GAME" || exit 64
[[ -x "$PG_BIN" ]] || { echo "[tooltip-vocab] SKIP: no binary $PG_BIN"; exit 64; }

OUT="${TOOLTIP_VOCAB_OUT:-${TMPDIR:-/tmp}/openpete-check-tooltip-vocab}/$PG_GAME"
mkdir -p "$OUT"
rm -f "$OUT/scan.txt" "$OUT/control.txt"

rc=0
OPENPETE_TOOLTIP_VOCAB=1 "$PG_BIN" --game "$PG_GAME" >"$OUT/scan.txt" 2>&1 || rc=$?
[[ -s "$OUT/scan.txt" ]] || { echo "[tooltip-vocab] FAIL: no output ($PG_GAME)"; exit 1; }
summary="$(grep '^tooltip-vocab: rows ' "$OUT/scan.txt" || true)"
read -r rows strings hits < <(sed -n 's/^tooltip-vocab: rows \([0-9]*\) strings \([0-9]*\) hits \([0-9]*\)$/\1 \2 \3/p' \
                              <<<"$summary")
echo "[tooltip-vocab] $PG_GAME rows ${rows:-?} strings ${strings:-?} hits ${hits:-?}"
if [[ "$rc" != 0 || -z "${rows:-}" || "${rows:-0}" -le 0 || "${strings:-0}" -le 0 || "${hits:-1}" != 0 ]]; then
    grep '^tooltip-vocab: HIT ' "$OUT/scan.txt" || true
    echo "[tooltip-vocab] FAIL: exit $rc ($PG_GAME)"
    exit 1
fi

rc=0
OPENPETE_TOOLTIP_VOCAB=1 OPENPETE_TOOLTIP_VOCAB_BREAK=fov \
    "$PG_BIN" --game "$PG_GAME" >"$OUT/control.txt" 2>&1 || rc=$?
if [[ "$rc" != 1 ]] || ! grep -q '^tooltip-vocab: HIT fov help: ' "$OUT/control.txt"; then
    echo "[tooltip-vocab] VACUOUS: a barred term in the fov tooltip did not fail the check (exit $rc)"
    exit 2
fi
echo "[tooltip-vocab] PASS ($PG_GAME)"
