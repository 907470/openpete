#!/usr/bin/env bash
# scripts/check_psx_oracle_all.sh - run every oracle recipe of every game
# (tests/harness/check_psx_oracle.sh --game SLUG --recipe R for each
# games/*/oracle*.conf) and print one table: game, row, verdict, records,
# GTE events over GTE mismatching records, VBL events over VBL-delta
# mismatches, gameplay bursts (cd_load_transient bursts opening outside a
# load span), clock replay engagement, first divergence. Each row's events
# and bursts are held to its table in
# games/<game>/manifest/oracle_ratchet.toml.
#
# Rows, from the recipe's file name:
#   oracle.conf             `stock` and `clock`
#   oracle-*-clock.conf     one clock row named by the recipe (`card-load-clock`)
#   oracle-cutscene-*.conf  a stock row (`cutscene-intro`) and a clock row
#                           (`cutscene-intro-clock`)
#   oracle-card-with-save.conf  one stock row `card+save`
#   any other oracle-*.conf one stock row named by the recipe (`card-load`)
# A stock row: the shipped path with the caller's flags; its GTE events, VBL
# events and gameplay bursts measure the in-tick interrupt and envelope
# timing the shipped engine does not replay. A clock row:
# OPENPETE_ORACLE_CLOCK=1 with OPENPETE_ORACLE_GTE_STRICT=1 and stock flags
# only, the reference's in-work VBLANK positions and libspu clocks
# replayed; its clock column is `.vblpos` rows delivered, `.spuq` rows
# charged and `.spureg` rows charged. A recipe that sets STOCK_ROW_OUT to a
# reason has no stock row: the table prints the row OUT with that reason
# and does not run it (the stock run diverges for a class only the clock
# row closes, which no ratchet entry expresses).
#
# Usage:
#   scripts/check_psx_oracle_all.sh [--windows-smoke] [-- <extra openpete flags>]
#
# OPENPETE_ORACLE_JOBS (default 3) caps the gates running at once.
#
# --windows-smoke: the stock row of every game on the Windows exe of
# build/windows-mingw (OPENPETE_WIN_BUILD; the gate forwards its knobs
# through WSLENV), truncated at OPENPETE_FRAME_LIMIT (default 8000 VBLs,
# about 3000 records per game, no level-load coverage; about a minute),
# two games at a time (OPENPETE_ORACLE_JOBS default 2). It catches a
# Windows-only divergence (host input, CRT or compiler differences) that the
# Linux gate cannot see; it is not the full gate. It exits 64 before starting anything while
# another job holds the timing window (scripts/bench/timing_lock.py), when an exe is
# missing, or when an openpete exe already runs on Windows (or tasklist.exe
# cannot tell). This script starts no exe itself: each row's
# tests/harness/check_psx_oracle.sh forwards every OPENPETE_* variable
# through WSLENV and checks each crossed (wslenv-guard: no-launch).
#
# Exit 0 when every row ran and PASSed, 2 when any row diverged, was
# VACUOUS or errored, 64 when none did and at least one row SKIPped (its
# prerequisite missing: golden, sidecar, fixture, binary, disc); the
# SKIPped rows and their reasons are repeated on stderr. OUT rows do not
# count. Logs: $OPENPETE_ORACLE_LOG_DIR/<game>.log (stock),
# <game>.<row>.log otherwise (default a temporary directory, deleted on
# exit).
set -euo pipefail
cd "$(dirname "$0")/.."

WIN_SMOKE=0
if [[ "${1:-}" == --windows-smoke ]]; then WIN_SMOKE=1; shift; fi
PASSTHRU=("$@")
JOBS="${OPENPETE_ORACLE_JOBS:-$(( WIN_SMOKE ? 2 : 3 ))}"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo "[psx-oracle-all] OPENPETE_ORACLE_JOBS=$JOBS is not a positive count" >&2; exit 64; }

GAMES=()
for c in games/*/oracle.conf; do GAMES+=("$(basename "$(dirname "$c")")"); done
[[ ${#GAMES[@]} -gt 0 ]] || { echo "[psx-oracle-all] no games/*/oracle.conf" >&2; exit 64; }

# The Windows smoke's preconditions: no timing window open, every game's exe
# built, and no openpete process already on the Windows side.
declare -A WIN_BIN=()
if (( WIN_SMOKE )); then
    if ! _timing="$(python3 scripts/bench/timing_lock.py status)"; then
        echo "[psx-oracle-all] SKIP: $_timing (a timing run owns the machine)" >&2
        exit 64
    fi
    # shellcheck source=scripts/lib/wslenv.sh
    source scripts/lib/wslenv.sh
    for g in "${GAMES[@]}"; do
        WIN_BIN[$g]="$(win_tree_exe "${OPENPETE_WIN_BUILD:-build/windows-mingw}" "$g")"
        if [[ ! -f "${WIN_BIN[$g]}" ]]; then
            echo "[psx-oracle-all] SKIP: no Windows exe ${WIN_BIN[$g]} for $g" >&2
            exit 64
        fi
    done
    if ! _tl="$(tasklist.exe /FO CSV /NH 2>/dev/null | tr -d '\r')"; then
        echo "[psx-oracle-all] SKIP: tasklist.exe did not run; Windows-side openpete processes unchecked" >&2
        exit 64
    fi
    _busy="$(grep -oiE '^"openpete(-spyro[0-9])?\.exe","[0-9]+"' <<<"$_tl" || true)"
    if [[ -n "$_busy" ]]; then
        echo "[psx-oracle-all] SKIP: openpete already runs on Windows: $(tr '\n' ' ' <<<"$_busy")" >&2
        exit 64
    fi
    export OPENPETE_FRAME_LIMIT="${OPENPETE_FRAME_LIMIT:-8000}"
    echo "[psx-oracle-all] windows smoke: stock rows, frame limit $OPENPETE_FRAME_LIMIT, $JOBS at a time"
fi

if [[ -n "${OPENPETE_ORACLE_LOG_DIR:-}" ]]; then
    LOGDIR="$OPENPETE_ORACLE_LOG_DIR"; mkdir -p "$LOGDIR"
else
    LOGDIR="$(mktemp -d "${TMPDIR:-/tmp}/psx-oracle-all.XXXXXX")"
    trap 'rm -rf "${LOGDIR:?}"' EXIT
fi

# The row list: one "game row recipe mode" entry per row, in table order.
ROW_GAME=(); ROW_NAME=(); ROW_RECIPE=(); ROW_MODE=()
add_row() { ROW_GAME+=("$1"); ROW_NAME+=("$2"); ROW_RECIPE+=("$3"); ROW_MODE+=("$4"); }
for g in "${GAMES[@]}"; do
    add_row "$g" stock "games/$g/oracle.conf" stock
    (( WIN_SMOKE )) && continue
    add_row "$g" clock "games/$g/oracle.conf" clock
    for r in games/"$g"/oracle-*.conf; do
        [[ -f "$r" ]] || continue
        stem="$(basename "$r" .conf)"; stem="${stem#oracle-}"
        case "$stem" in
            *-clock)    add_row "$g" "$stem" "$r" clock ;;
            cutscene-*) add_row "$g" "$stem" "$r" stock; add_row "$g" "$stem-clock" "$r" clock ;;
            card-with-save) add_row "$g" card+save "$r" stock ;;
            *)          add_row "$g" "$stem" "$r" stock ;;
        esac
    done
done

# stock_out RECIPE: the recipe's STOCK_ROW_OUT reason, empty when it has a stock row.
stock_out() { bash -c 'source "$1" >/dev/null 2>&1; echo "${STOCK_ROW_OUT:-}"' _ "$1"; }

# Run every row, at most JOBS gates at once; RC[i] is row i's exit status.
declare -A RC OUT_REASON PID_ROW
LOGS=()
running=0
reap_one() {
    local done_pid=""
    set +e; wait -n -p done_pid; local rc=$?; set -e
    RC[${PID_ROW[$done_pid]}]=$rc
    running=$((running - 1))
}
for i in "${!ROW_NAME[@]}"; do
    g=${ROW_GAME[$i]}; row=${ROW_NAME[$i]}; recipe=${ROW_RECIPE[$i]}
    if [[ "$row" == stock ]]; then logname="$g.log"; else logname="$g.$row.log"; fi
    LOGS[i]="$LOGDIR/$logname"
    if [[ "${ROW_MODE[$i]}" == stock && "$recipe" != */oracle.conf ]]; then
        reason="$(stock_out "$recipe")"
        if [[ -n "$reason" ]]; then OUT_REASON[$i]="$reason"; continue; fi
    fi
    recipe_args=(); [[ "$recipe" == */oracle.conf ]] || recipe_args=(--recipe "$recipe")
    if [[ "${ROW_MODE[$i]}" == clock ]]; then
        env_args=(OPENPETE_ORACLE_CLOCK=1 OPENPETE_ORACLE_GTE_STRICT=1); flags=()
    else
        env_args=(); flags=("${PASSTHRU[@]}")
    fi
    (( WIN_SMOKE )) && env_args+=(OPENPETE_BIN="${WIN_BIN[$g]}")
    (( running < JOBS )) || reap_one
    rm -f "${LOGDIR:?}/${logname:?}"
    env "${env_args[@]}" tests/harness/check_psx_oracle.sh --game "$g" "${recipe_args[@]}" "${flags[@]}" \
        >"${LOGS[i]}" 2>&1 &
    PID_ROW[$!]=$i
    running=$((running + 1))
done
while (( running > 0 )); do reap_one; done

worst=0; skipped=0; ran=0
SKIPS=()
fmt='%-10s %-21s %-8s %-8s %-7s %-7s %-5s %-16s %s\n'
printf "$fmt" game row verdict records gte vbl gp_b clock first_divergence
for i in "${!ROW_NAME[@]}"; do
    g=${ROW_GAME[$i]}; row=${ROW_NAME[$i]}
    if [[ -n "${OUT_REASON[$i]+set}" ]]; then
        printf "$fmt" "$g" "$row" OUT - - - - - "${OUT_REASON[$i]}"
        continue
    fi
    rc=${RC[$i]}
    log="${LOGS[i]}"
    records=$(sed -n 's/.*psxtrace: summary: records=\([0-9]*\) .*/\1/p' "$log" | tail -1)
    gte=$(sed -n 's/.*gte: records=[0-9]* mismatches=\([0-9]*\) events=\([0-9]*\) .*/\2\/\1/p' "$log" | tail -1)
    vbld=$(sed -n 's/.*vbl deltas: policy=[a-z]* mismatches=\([0-9]*\).*/\1/p' "$log" | tail -1)
    vble=$(sed -n 's/.*ratchet: key=.* vbl events=\([0-9]*\) .*/\1/p' "$log" | tail -1)
    [[ -n "$vbld" ]] && vbld="${vble:-?}/$vbld"
    gpb=$(sed -n 's/.*ratchet: key=.* gameplay bursts=\([0-9]*\);.*/\1/p' "$log" | tail -1)
    clock=-
    if [[ "${ROW_MODE[$i]}" == clock ]]; then
        clock=$(sed -n 's/.*clock: vblpos delivered=\([0-9]*\) .*spuq charged=\([0-9]*\) .*spureg charged=\([0-9]*\) .*/\1\/\2\/\3/p' "$log" | tail -1)
    fi
    # The first divergence outside the neutral rows (a healed CD-load
    # transient on a PASS, the failing row on a FAIL).
    first=$(sed -n 's/.*first_non_neutral_div=\([^ ]*\) frame=\([0-9]*\).*/\1@\2/p' "$log" | tail -1)
    [[ "$first" == none@* ]] && first=none
    case $rc in
        0)  verdict=PASS; ran=1 ;;
        64) verdict=SKIP; skipped=1
            first=$(grep -m1 '^\[psx-oracle\]' "$log" | cut -c14-120)
            SKIPS+=("$g $row: $(grep -m1 '^\[psx-oracle\]' "$log" | cut -c14-)") ;;
        2)  verdict=FAIL; ran=1 ;;
        3)  verdict=VACUOUS; ran=1 ;;
        *)  verdict="ERR($rc)"; ran=1 ;;
    esac
    [[ $rc != 0 && $rc != 64 ]] && worst=2
    printf "$fmt" "$g" "$row" "$verdict" "${records:--}" \
        "${gte:--}" "${vbld:--}" "${gpb:--}" "${clock:--}" "${first:-none}"
done
if (( skipped )); then
    echo "[psx-oracle-all] SKIPPED ${#SKIPS[@]} row(s); the table is not a full pass:" >&2
    printf '[psx-oracle-all]   %s\n' "${SKIPS[@]}" >&2
fi
(( worst )) && exit 2
(( skipped || ! ran )) && exit 64
exit 0
