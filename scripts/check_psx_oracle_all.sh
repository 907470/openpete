#!/usr/bin/env bash
# scripts/check_psx_oracle_all.sh - run every game's oracle gate
# (tests/harness/check_psx_oracle.sh --game SLUG for each games/*/oracle.conf) in
# parallel and print one table: game, row, verdict, records, GTE events
# over GTE mismatching records, VBL events over VBL-delta mismatches,
# gameplay bursts (cd_load_transient bursts opening outside a load span),
# clock replay engagement, first divergence. Each row's events and bursts
# are held to its table in games/<game>/manifest/oracle_ratchet.toml.
#
# Each game runs two rows. `stock`: the shipped path with the caller's
# flags; its GTE events, VBL events and gameplay bursts measure the in-tick
# interrupt and envelope timing the shipped engine does not replay.
# `clock`: OPENPETE_ORACLE_CLOCK=1 with OPENPETE_ORACLE_GTE_STRICT=1 and
# stock flags only, the reference's in-work VBLANK positions and libspu
# clocks replayed; the clock column is `.vblpos` rows delivered, `.spuq`
# rows charged and `.spureg` rows charged. The stock rows run first, all games at once, then the
# clock rows.
#
# Usage:
#   scripts/check_psx_oracle_all.sh [-- <extra openpete flags>]
#
# Exit 0 when every row PASSes, 2 when any diverges or errors, 64 when no
# row ran (every gate SKIPped). Logs: $OPENPETE_ORACLE_LOG_DIR/<game>.log
# and <game>.clock.log, <game>.card.log, <game>.card+save.log,
# <game>.card-load.log (default a temporary directory, deleted on exit).
set -euo pipefail
cd "$(dirname "$0")/.."

PASSTHRU=("$@")
GAMES=()
for c in games/*/oracle.conf; do GAMES+=("$(basename "$(dirname "$c")")"); done
[[ ${#GAMES[@]} -gt 0 ]] || { echo "[psx-oracle-all] no games/*/oracle.conf" >&2; exit 64; }

if [[ -n "${OPENPETE_ORACLE_LOG_DIR:-}" ]]; then
    LOGDIR="$OPENPETE_ORACLE_LOG_DIR"; mkdir -p "$LOGDIR"
else
    LOGDIR="$(mktemp -d "${TMPDIR:-/tmp}/psx-oracle-all.XXXXXX")"
    trap 'rm -rf "$LOGDIR"' EXIT
fi

# One gate per game and row, the games of a row concurrently; each gate's
# own log is its record. A card row runs only the games carrying its recipe.
declare -A RC
declare -A CARD_RECIPE=([.card]=oracle-card.conf [.card+save]=oracle-card-with-save.conf
                        [.card-load]=oracle-card-load.conf)
ROWS=("" .clock .card .card+save .card-load)
run_row() {
    local row=$1; shift
    local -A pid
    local g
    local recipe_args
    for g in "${GAMES[@]}"; do
        recipe_args=()
        if [[ -n "$row" && -n "${CARD_RECIPE[$row]:-}" ]]; then
            [[ -f "games/$g/${CARD_RECIPE[$row]}" ]] || continue
            recipe_args=(--recipe "games/$g/${CARD_RECIPE[$row]}")
        fi
        rm -f "${LOGDIR:?}/$g$row.log"
        "$@" tests/harness/check_psx_oracle.sh --game "$g" "${recipe_args[@]}" "${ROW_ARGS[@]}" >"$LOGDIR/$g$row.log" 2>&1 &
        pid[$g]=$!
    done
    for g in "${!pid[@]}"; do
        set +e; wait "${pid[$g]}"; RC[$g$row]=$?; set -e
    done
}
ROW_ARGS=("${PASSTHRU[@]}")
run_row "" env
ROW_ARGS=()
run_row .clock env OPENPETE_ORACLE_CLOCK=1 OPENPETE_ORACLE_GTE_STRICT=1
ROW_ARGS=("${PASSTHRU[@]}")
for row in .card .card+save .card-load; do run_row "$row" env; done

worst=0; ran=0
printf '%-10s %-9s %-8s %-8s %-7s %-7s %-5s %-14s %s\n' game row verdict records gte vbl gp_b clock first_divergence
for g in "${GAMES[@]}"; do
    for row in "${ROWS[@]}"; do
        [[ -n "${RC[$g$row]+set}" ]] || continue
        rc=${RC[$g$row]}
        log="$LOGDIR/$g$row.log"
        records=$(sed -n 's/.*psxtrace: summary: records=\([0-9]*\) .*/\1/p' "$log" | tail -1)
        gte=$(sed -n 's/.*gte: records=[0-9]* mismatches=\([0-9]*\) events=\([0-9]*\) .*/\2\/\1/p' "$log" | tail -1)
        vbld=$(sed -n 's/.*vbl deltas: policy=[a-z]* mismatches=\([0-9]*\).*/\1/p' "$log" | tail -1)
        vble=$(sed -n 's/.*ratchet: key=.* vbl events=\([0-9]*\) .*/\1/p' "$log" | tail -1)
        [[ -n "$vbld" ]] && vbld="${vble:-?}/$vbld"
        gpb=$(sed -n 's/.*ratchet: key=.* gameplay bursts=\([0-9]*\);.*/\1/p' "$log" | tail -1)
        clock=-
        if [[ "$row" == .clock ]]; then
            clock=$(sed -n 's/.*clock: vblpos delivered=\([0-9]*\) .*spuq charged=\([0-9]*\) .*spureg charged=\([0-9]*\) .*/\1\/\2\/\3/p' "$log" | tail -1)
        fi
        # The first divergence outside the neutral rows (a healed CD-load
        # transient on a PASS, the failing row on a FAIL).
        first=$(sed -n 's/.*first_non_neutral_div=\([^ ]*\) frame=\([0-9]*\).*/\1@\2/p' "$log" | tail -1)
        [[ "$first" == none@* ]] && first=none
        case $rc in
            0)  verdict=PASS; ran=1 ;;
            64) verdict=SKIP; first=$(grep -m1 '^\[psx-oracle\]' "$log" | cut -c14-80) ;;
            2)  verdict=FAIL; ran=1 ;;
            3)  verdict=VACUOUS; ran=1 ;;
            *)  verdict="ERR($rc)"; ran=1 ;;
        esac
        [[ $rc != 0 && $rc != 64 ]] && worst=2
        printf '%-10s %-9s %-8s %-8s %-7s %-7s %-5s %-14s %s\n' "$g" "$(r=${row:-.stock}; echo "${r#.}")" "$verdict" "${records:--}" \
            "${gte:--}" "${vbld:--}" "${gpb:--}" "${clock:--}" "${first:-none}"
    done
done
(( ran )) || exit 64
exit $worst
