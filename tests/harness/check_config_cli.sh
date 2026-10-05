#!/usr/bin/env bash

# tests/harness/check_config_cli.sh - option-parse exit codes and the
# config-file fallback for --fov and --fog-scale.
#
# Legs, each a parse-only run of PG_BIN except `boot`:
#   help          --help and -h exit 0 and print no "could not start" line.
#   cli-refuse    --fov nan and --fov abc typed exit 3 (the negative control:
#                 the same values from the config file must not).
#   cfg-fallback  fov = "abc", fog_scale = "nan" (bare keys) and
#                 [args] "--fov" = "inf" exit 0 under --config-report with one
#                 warning naming the key, the value and the default kept.
#   cfg-and-cli   fog_scale = "abc" in the file plus --fov abc typed exits 3:
#                 the typed value is refused even when its text equals a
#                 spliced config value.
#   fov-zero      fov = 0 parses as the game's projection: exit 0, no clamp
#                 message; fov = 10 still clamps to 30.
#   boot          a headless boot under fov = "abc" reaches --frame-limit
#                 (exit 0). Skipped without a disc.
#
# Usage: tests/harness/check_config_cli.sh [--game SLUG] [--build-dir=DIR]
# Exit:  0 pass, 1 fail, 2 vacuous (the negative control did not refuse),
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
[[ -x "$PG_BIN" ]] || { echo "[config-cli] SKIP: no binary $PG_BIN"; exit 64; }

OUT="${CONFIG_CLI_OUT:-${TMPDIR:-/tmp}/openpete-check-config-cli}/$PG_GAME"
mkdir -p "$OUT"
rm -f "$OUT"/*.toml "$OUT"/*.err

write_toml() { printf '%s\n' "$2" >"$OUT/$1.toml"; [[ -s "$OUT/$1.toml" ]]; }
write_toml fovabc  'fov = "abc"'
write_toml fognan  'fog_scale = "nan"'
write_toml fogabc  'fog_scale = "abc"'
write_toml fovargs $'[args]\n"--fov" = "inf"'
write_toml fovzero 'fov = 0'
write_toml fovten  'fov = 10'

fail=0
# leg TAG WANT_RC [args...]: run PG_BIN, stderr to TAG.err, compare the exit code.
leg() {
    local tag="$1" want="$2"; shift 2
    local rc=0
    OPENPETE_VIBRATION=0 "$PG_BIN" "$@" >/dev/null 2>"$OUT/$tag.err" || rc=$?
    [[ -f "$OUT/$tag.err" ]] || { echo "[config-cli] FAIL: $tag wrote no stderr file"; fail=1; return 1; }
    if [[ "$rc" != "$want" ]]; then
        echo "[config-cli] FAIL: $tag exited $rc, expected $want"; fail=1; return 1
    fi
}
has()  { grep -qF -- "$2" "$OUT/$1.err" || { echo "[config-cli] FAIL: $1 lacks: $2"; fail=1; }; }
lacks() { ! grep -qF -- "$2" "$OUT/$1.err" || { echo "[config-cli] FAIL: $1 prints: $2"; fail=1; }; }

leg help 0 --no-config --help || true
lacks help "could not start"
has help "Usage:"
leg help_h 0 --no-config -h || true
lacks help_h "could not start"

control=0
leg cli_nan 3 --no-config --fov nan && control=$((control + 1)) || true
has cli_nan "--fov expects a number"
leg cli_abc 3 --no-config --fov abc && control=$((control + 1)) || true

leg cfg_fov 0 --config "$OUT/fovabc.toml" --config-report || true
has cfg_fov 'fov = "abc" is not a finite number; keeping 0'
leg cfg_fog 0 --config "$OUT/fognan.toml" --config-report || true
has cfg_fog 'fog_scale = "nan" is not a finite number; keeping 0'
leg cfg_args 0 --config "$OUT/fovargs.toml" --config-report || true
has cfg_args 'fov = "inf" is not a finite number'

leg cfg_and_cli 3 --config "$OUT/fogabc.toml" --fov abc --config-report || true
has cfg_and_cli "--fov expects a number"

leg fov_zero 0 --config "$OUT/fovzero.toml" --config-report || true
lacks fov_zero "clamped"
leg fov_ten 0 --config "$OUT/fovten.toml" --config-report || true
has fov_ten "--fov clamped to [30,120]; got 10.00"

if [[ -f "$PG_DISC" ]]; then
    leg boot 0 --headless --config "$OUT/fovabc.toml" --disc "$PG_DISC" \
        --no-card --mute 1 --vibration 0 --frame-limit 120 || true
    has boot 'fov = "abc" is not a finite number'
else
    echo "[config-cli] boot leg skipped: no disc $PG_DISC"
fi

if (( control != 2 )); then
    echo "[config-cli] VACUOUS: the typed refusals did not exit 3"; exit 2
fi
(( fail == 0 )) || { echo "[config-cli] FAIL ($PG_GAME)"; exit 1; }
echo "[config-cli] PASS ($PG_GAME)"
