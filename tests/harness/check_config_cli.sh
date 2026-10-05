#!/usr/bin/env bash

# tests/harness/check_config_cli.sh - option-parse exit codes and the
# shared value validator: a value refused by type or range fails when typed,
# warns and keeps the default from the config file or an env spelling at
# boot, and fails under --config-report.
#
# Legs, each a parse-only run of PG_BIN except `boot`. --keys-report is the
# parse-only run with boot behaviour; --config-report refuses.
#   help          --help and -h exit 0 and print no "could not start" line.
#   cli-refuse    --fov nan and --fov abc typed exit 3 (the negative control:
#                 the same values from the config file must not).
#   cfg-fallback  fov = "abc", fog_scale = "nan" (bare keys),
#                 [args] "--fov" = "inf", ssaa = 0 and lod = "bogus" exit 0
#                 under --keys-report with one warning naming the key, the
#                 value and the default kept.
#   cfg-report    the same fov = "abc" under --config-report exits 3.
#   cfg-and-cli   fog_scale = "abc" in the file plus --fov abc typed exits 3:
#                 the typed value is refused even when its text equals a
#                 spliced config value.
#   env-fallback  OPENPETE_MOUSE=bogus exits 0 under --keys-report with the
#                 warning; under --config-report it exits 3.
#   run-recomp    --run-recomp is no flag (OPENPETE_RUN_RECOMP is its
#                 spelling): exit 3.
#   fov-zero      fov = 0 parses as the game's projection: exit 0, no clamp
#                 message; fov = 10 still clamps to 30.
#   boot          a headless boot under fov = "abc" and an [env] table setting
#                 OPENPETE_LOAD_DELAY = "7" reaches --frame-limit (exit 0) and
#                 logs the load delay 7. Skipped without a disc.
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
write_toml ssaa0   'ssaa = 0'
write_toml lodbad  'lod = "bogus"'
write_toml bootenv $'fov = "abc"\n[env]\nOPENPETE_LOAD_DELAY = "7"'

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

leg cfg_fov 0 --config "$OUT/fovabc.toml" --keys-report || true
has cfg_fov 'fov = "abc" is not accepted; keeping the default'
leg cfg_fog 0 --config "$OUT/fognan.toml" --keys-report || true
has cfg_fog 'fog_scale = "nan" is not accepted; keeping the default'
leg cfg_args 0 --config "$OUT/fovargs.toml" --keys-report || true
has cfg_args 'fov = "inf" is not accepted'
leg cfg_ssaa 0 --config "$OUT/ssaa0.toml" --keys-report || true
has cfg_ssaa 'ssaa = "0" is not accepted; keeping the default'
leg cfg_lod 0 --config "$OUT/lodbad.toml" --keys-report || true
has cfg_lod 'lod = "bogus" is not accepted; keeping the default'

leg cfg_report 3 --config "$OUT/fovabc.toml" --config-report || true
lacks cfg_report "keeping the default"

leg cfg_and_cli 3 --config "$OUT/fogabc.toml" --fov abc --keys-report || true
has cfg_and_cli "--fov expects a number"

OPENPETE_MOUSE=bogus leg env_mouse 0 --no-config --keys-report || true
has env_mouse 'OPENPETE_MOUSE = "bogus" is not accepted; keeping the default'
OPENPETE_MOUSE=bogus leg env_report 3 --no-config --config-report || true

leg run_recomp 3 --no-config --run-recomp 0x80012204 || true
has run_recomp "unknown arg: --run-recomp"

leg fov_zero 0 --config "$OUT/fovzero.toml" --config-report || true
lacks fov_zero "clamped"
leg fov_ten 0 --config "$OUT/fovten.toml" --config-report || true
has fov_ten "--fov clamped to [30,120]; got 10.00"

if [[ -f "$PG_DISC" ]]; then
    leg boot 0 --headless --config "$OUT/bootenv.toml" --disc "$PG_DISC" \
        --no-card --mute 1 --vibration 0 --frame-limit 120 || true
    has boot 'fov = "abc" is not accepted'
    has boot 'load delay: 7 VBL(s) per level load'
else
    echo "[config-cli] boot leg skipped: no disc $PG_DISC"
fi

if (( control != 2 )); then
    echo "[config-cli] VACUOUS: the typed refusals did not exit 3"; exit 2
fi
(( fail == 0 )) || { echo "[config-cli] FAIL ($PG_GAME)"; exit 1; }
echo "[config-cli] PASS ($PG_GAME)"
