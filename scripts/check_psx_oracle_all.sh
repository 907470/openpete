#!/usr/bin/env bash
# scripts/check_psx_oracle_all.sh — run every game's oracle gate
# (scripts/check_psx_oracle.sh --game SLUG for each games/*/oracle.conf) in
# parallel and print one table: game, verdict, records, first divergence.
#
# Usage:
#   scripts/check_psx_oracle_all.sh [-- <extra openpete flags>]
#
# Exit 0 when every game PASSes, 2 when any diverges or errors, 64 when no
# game ran (every gate SKIPped). Logs: $OPENPETE_ORACLE_LOG_DIR/<game>.log
# (default a temporary directory, deleted on exit).
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

# One gate per game, concurrently; each gate's own log is its record.
declare -A PID
for g in "${GAMES[@]}"; do
    rm -f "$LOGDIR/$g.log"
    scripts/check_psx_oracle.sh --game "$g" "${PASSTHRU[@]}" >"$LOGDIR/$g.log" 2>&1 &
    PID[$g]=$!
done

declare -A RC
for g in "${GAMES[@]}"; do
    set +e; wait "${PID[$g]}"; RC[$g]=$?; set -e
done

worst=0; ran=0
printf '%-10s %-8s %-13s %s\n' game verdict records first_divergence
for g in "${GAMES[@]}"; do
    rc=${RC[$g]}
    log="$LOGDIR/$g.log"
    records=$(sed -n 's/.*psxtrace: summary: records=\([0-9]*\) .*/\1/p' "$log" | tail -1)
    # The first divergence outside the neutral rows (a healed CD-load
    # transient on a PASS, the failing row on a FAIL).
    first=$(sed -n 's/.*first_non_neutral_div=\([^ ]*\) frame=\([0-9]*\).*/\1@\2/p' "$log" | tail -1)
    [[ "$first" == none@* ]] && first=none
    case $rc in
        0)  verdict=PASS; ran=1 ;;
        64) verdict=SKIP; first=$(grep -m1 '^\[psx-oracle\]' "$log" | cut -c14-80) ;;
        2)  verdict=FAIL; ran=1 ;;
        *)  verdict="ERR($rc)"; ran=1 ;;
    esac
    [[ $rc != 0 && $rc != 64 ]] && worst=2
    printf '%-10s %-8s %-13s %s\n' "$g" "$verdict" "${records:--}" "${first:-none}"
done
(( ran )) || exit 64
exit $worst
