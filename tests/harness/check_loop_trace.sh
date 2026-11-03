#!/usr/bin/env bash
# check_loop_trace.sh - the host loop's per-iteration decisions, run against a
# blessed digest.
#
# OPENPETE_LOOP_TRACE=<path> writes one record per host-loop iteration (the
# gates, the present made, the runahead outcome, the exit reason, the pace
# flags, the deadlines), a header per process and the teardown steps
# (core/include/openpete_host_trace.h). Two builds that make the same decision
# at every iteration of one run write the same trace on the compared fields;
# off the --record-cfr virtual clock the wall-clock fields (pace flags SLIPPED
# and NOT_LIVE, the deadlines, the eager world-live bit) are masked
# (tests/harness/looptrace.py).
#
# Rows: games/<slug>/corpus/loop-trace.manifest (row | kind | source | launch |
# covers; the manifest header defines the fields). Each row is one run; its
# trace's digest (sha256 of the compared fields of every record), record count
# and exit code must equal the row's line of
# tests/golden/<slug>/loop-trace-baseline.tsv. The digest is equal exactly when
# `looptrace.py compare` reports the two traces identical, so the manifest
# pins the reference without committing the traces (2.5 MB for a 40k-VBL
# boot row).
#
# Engagement: each row reports its counts per present kind, runahead outcome
# and shed verdict, and its covers terms must hold; a row that misses one is
# VACUOUS, which fails. A setup-ftl row also asserts the movie it was asked to
# record over is byte-unchanged; a setup-pace row asserts no `verdict: PASS`
# line (it replays the oracle golden, and SKIPs when the golden is absent).
#
# Windowed rows (Xvfb, the virtual clock, 32-56 min each for t3) run only
# with --windowed and need xvfb-run; without it they are listed as not run.
#
# Divergence: --ref-bin=PATH reruns a diverged row with that binary (a build
# of the baseline's revision) and prints `looptrace.py compare`'s first
# differing record. The candidate trace stays in the output directory.
#
# Bless: --bless writes the rows this run made (none VACUOUS) into the
# baseline file, keeping the other rows; review the diff before committing.
# Re-bless only for an intended change to a loop decision.
#
# Verdict line, one per row:
#   [loop-trace] game=<g> row=<r> rc=<n> records=<n> digest=<12 hex>
#                <counts> verdict=IDENTICAL|DIVERGED|VACUOUS|FAIL|SKIP
# then `[loop-trace] game=<g> verdict=PASS|FAIL|SKIP rows=<n> ...`.
#
# Usage: tests/harness/check_loop_trace.sh [--game SLUG] [--build-dir=DIR]
#            [--row NAME]... [--windowed] [--bless] [--ref-bin=PATH]
# Outputs: <build-dir>/loop-trace/<slug>/<row>/{trace.lt,run.log}.
# Exit 0 PASS, 1 FAIL (a DIVERGED, VACUOUS, FAIL or unblessed row), 64 SKIP
# (binary, disc or manifest missing, or no row ran).

set -u
set -o pipefail
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

ROWS=() WINDOWED=0 BLESS=0 REF_BIN=""
prev=""
for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}" ;;
        --windowed) WINDOWED=1 ;;
        --bless) BLESS=1 ;;
        --ref-bin=*) REF_BIN="${a#--ref-bin=}" ;;
    esac
    [[ "$prev" == --row ]] && ROWS+=("$a")
    prev="$a"
done

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME" || exit 64
MANIFEST="$(pg_corpus_manifest "$GAME" loop-trace.manifest)"
[[ -f "$MANIFEST" ]] || { echo "loop-trace: SKIP (no manifest $MANIFEST)"; exit 64; }
pg_manifest "$MANIFEST" pipe
[[ -x "$PG_BIN" && -f "$PG_DISC" ]] || { echo "loop-trace: SKIP (binary or disc missing: $PG_BIN $PG_DISC)"; exit 64; }
BASELINE="tests/golden/$GAME/loop-trace-baseline.tsv"
LT="tests/harness/looptrace.py"
OUT_ROOT="${PG_BUILD_DIR:-build/linux-release}/loop-trace/$GAME"
mkdir -p "$OUT_ROOT"

# The handoff checkpoint and any other per-user state the run writes land in
# a scratch state directory, never the player's.
STATE_HOME="$(mktemp -d "${TMPDIR:-/tmp}/loop-trace-state.XXXXXX")"
trap 'rm -rf "${STATE_HOME:?}"' EXIT

# row_wanted NAME: true when no --row was given or NAME is one of them.
row_wanted() {
    [[ ${#ROWS[@]} -eq 0 ]] && return 0
    local r
    for r in "${ROWS[@]}"; do [[ "$r" == "$1" ]] && return 0; done
    return 1
}

# run_windowed BIN LOG ARGS...: one Xvfb run of BIN under the env of
# PG_CAPTURE_ENV, on its own scratch cards, output to LOG.
run_windowed() {
    local bin="$1" log="$2"; shift 2
    local cards rc
    cards="$(mktemp -d "$OPENPETE_CARD_DIR/run.XXXXXX")" || return 1
    rm -f "$log"
    op_own_run env OPENPETE_VIBRATION=0 "OPENPETE_CARD_DIR=$cards" "${PG_CAPTURE_ENV[@]}" \
        xvfb-run -a -s "-screen 0 1280x960x24" \
        timeout -k "$OP_OWN_GRACE" 5400 "$bin" --no-config --disc "$PG_DISC" --disc-cache 0 \
        --mute 1 --vibration 0 --input-lock --background 1 --renderer vk --vsync 0 \
        --window 640x480 --aspect 4:3 --lod original --record-cfr 119.652 \
        "${PG_RUN_ARGS[@]}" "$@" </dev/null >"$log" 2>&1
    rc=$?
    rm -rf "${cards:?}"
    return $rc
}

# run_row BIN DIR KIND SOURCE LAUNCH: one run of a row into DIR/trace.lt and
# DIR/run.log; prints the exit code. Returns 2 when the row cannot run here
# (SKIP), 0 otherwise.
run_row() {
    local bin="$1" dir="$2" kind="$3" src="$4" launch="$5"
    mkdir -p "$dir"
    rm -f "${dir:?}/trace.lt" "${dir:?}/run.log" "${dir:?}/movie.bk2" "${dir:?}/movie.sha"
    pg_split_launch "$launch"
    local -a args=(--mute 1 --vibration 0)
    case "$kind" in
        bk2)
            args+=(--bk2 "$PG_BK2_DIR/$src" --bk2-stop-on-end --frame-limit 6000
                   --unlock-frames 0-6000) ;;
        setup-ftl)
            printf 'loop-trace setup-ftl sentinel movie\n' >"$dir/movie.bk2"
            sha256sum "$dir/movie.bk2" >"$dir/movie.sha"
            args+=(--bk2-record "$dir/movie.bk2" --frame-time-log "$dir/no-such-dir/ftl.bin") ;;
        setup-pace)
            local g="tests/golden/$GAME/psxtrace-demo-20k.bin"
            [[ -f "$g" ]] || return 2
            args+=(--psxtrace-verify "$g" --pace-log "$dir/no-such-dir/pace.oppl") ;;
    esac
    PG_CAPTURE_ENV=("XDG_STATE_HOME=$STATE_HOME" "OPENPETE_LOOP_TRACE=$dir/trace.lt" "${ROW_ENV[@]}")
    local rc
    if [[ "$kind" == windowed ]]; then
        command -v xvfb-run >/dev/null || return 2
        run_windowed "$bin" "$dir/run.log" "${ROW_ARGS[@]}"
        rc=$?
    else
        PG_CAPTURE_TIMEOUT=5400
        local saved="$PG_BIN"
        PG_BIN="$bin"
        pg_capture "$dir/run.log" "" -- "${args[@]}" "${ROW_ARGS[@]}" </dev/null
        rc=$?
        PG_BIN="$saved"
    fi
    echo "$rc"
    return 0
}

# covers_unmet DIR KIND COVERS COUNTS: prints each unmet covers term (empty
# when every term holds).
covers_unmet() {
    local dir="$1" kind="$2" covers="$3" counts=" $4 " t name want have
    for t in $covers; do
        case "$t" in
            log:*)
                grep -qF -- "${t#log:}" <(tr -s ' ' <"$dir/run.log" | sed 's/ /~/g') ||
                    echo "$t" ;;
            *=*)
                name="${t%%=*}"; want="${t#*=}"
                have="${counts#* "$name"=}"; have="${have%% *}"
                [[ "$counts" == *" $name="* && "$have" == "$want" ]] || echo "$t(have ${have:-?})" ;;
            *)
                have="${counts#* "$t"=}"; have="${have%% *}"
                [[ "$counts" == *" $t="* && "$have" -gt 0 ]] || echo "$t(have ${have:-?})" ;;
        esac
    done
    case "$kind" in
        setup-ftl) sha256sum -c --status "$dir/movie.sha" || echo "movie-unchanged" ;;
        setup-pace) grep -q 'verdict: PASS' "$dir/run.log" && echo "no-verdict-pass" ;;
    esac
    return 0
}

# One engagement summary for the verdict line: the non-zero present kinds,
# runahead outcomes, shed verdicts and the record shape counts.
brief_counts() {
    local kv out=""
    for kv in $1; do
        case "${kv%%=*}" in
            none|canon-*|intermediate|late-admit|probe|shed|eager-produce|paused-repaint|\
            speculated|adopted|left|v-*|rollbacks|rewound|headers|iterations|close_steps)
                [[ "${kv#*=}" != 0 ]] && out+="${kv} " ;;
        esac
    done
    echo "${out% }"
}

declare -A NEW_ROW=()
n_run=0 n_ident=0 n_bad=0 n_skip=0
while IFS='|' read -r row kind src launch covers; do
    row="${row//[[:space:]]/}"; kind="${kind//[[:space:]]/}"; src="${src//[[:space:]]/}"
    launch="$(echo "$launch" | xargs)"; covers="$(echo "$covers" | xargs)"
    [[ -z "$row" || "$row" == \#* ]] && continue
    row_wanted "$row" || continue
    if [[ "$kind" == windowed && "$WINDOWED" != 1 ]]; then
        echo "[loop-trace] game=$GAME row=$row not run (windowed; pass --windowed)"
        continue
    fi
    if [[ "$kind" == bk2 && ! -f "$PG_BK2_DIR/$src" ]]; then
        echo "[loop-trace] game=$GAME row=$row verdict=SKIP (no movie $PG_BK2_DIR/$src)"
        n_skip=$((n_skip + 1)); continue
    fi
    dir="$OUT_ROOT/$row"
    T0=$(date +%s)
    rc="$(run_row "$PG_BIN" "$dir" "$kind" "$src" "$launch")"
    if [[ -z "$rc" ]]; then
        echo "[loop-trace] game=$GAME row=$row verdict=SKIP (setup-pace needs the oracle golden; windowed needs xvfb-run)"
        n_skip=$((n_skip + 1)); continue
    fi
    wall=$(( $(date +%s) - T0 ))
    n_run=$((n_run + 1))
    if [[ ! -s "$dir/trace.lt" ]]; then
        echo "[loop-trace] game=$GAME row=$row rc=$rc ${wall}s verdict=FAIL (no trace; see $dir/run.log)"
        n_bad=$((n_bad + 1)); continue
    fi
    read -r dg recs < <(python3 "$LT" digest "$dir/trace.lt") ||
        { echo "[loop-trace] game=$GAME row=$row verdict=FAIL (malformed trace $dir/trace.lt)"; n_bad=$((n_bad + 1)); continue; }
    counts="$(python3 "$LT" counts "$dir/trace.lt")"
    brief="$(brief_counts "$counts")"
    unmet="$(covers_unmet "$dir" "$kind" "$covers" "$counts" | xargs)"
    line="[loop-trace] game=$GAME row=$row rc=$rc records=$recs digest=${dg:0:12} ${wall}s $brief"
    if [[ -n "$unmet" ]]; then
        echo "$line verdict=VACUOUS (unmet: $unmet)"
        n_bad=$((n_bad + 1)); continue
    fi
    NEW_ROW[$row]="$row	$rc	$recs	$dg	$brief"
    base="$(awk -F'\t' -v r="$row" '$1 == r { print $2 "\t" $3 "\t" $4 }' "$BASELINE" 2>/dev/null)"
    if [[ "$base" == "$rc	$recs	$dg" ]]; then
        echo "$line verdict=IDENTICAL"
        n_ident=$((n_ident + 1)); continue
    fi
    if [[ "$BLESS" == 1 ]]; then
        echo "$line verdict=BLESSED (was: ${base:-no row})"
        n_ident=$((n_ident + 1)); continue
    fi
    if [[ -z "$base" ]]; then
        echo "$line verdict=FAIL (no baseline row in $BASELINE; --bless writes it)"
        n_bad=$((n_bad + 1)); continue
    fi
    echo "$line verdict=DIVERGED (baseline rc records digest: ${base//	/ })"
    n_bad=$((n_bad + 1))
    if [[ -n "$REF_BIN" ]]; then
        rrc="$(run_row "$REF_BIN" "$dir/ref" "$kind" "$src" "$launch")"
        echo "  ref-bin rc=$rrc: $(python3 "$LT" compare "$dir/ref/trace.lt" "$dir/trace.lt" | tr '\n' ' ')"
    fi
done <"$MANIFEST"

if [[ "$BLESS" == 1 && ${#NEW_ROW[@]} -gt 0 ]]; then
    tmp="$BASELINE.tmp.$$"
    {
        printf '# Host-loop decision-trace baseline (tests/harness/check_loop_trace.sh --bless).\n'
        printf '# row\trc\trecords\tsha256 of the compared fields\tengagement (non-zero counts)\n'
        if [[ -f "$BASELINE" ]]; then
            grep -v '^#' "$BASELINE" | while IFS=$'\t' read -r r rest; do
                [[ -n "$r" && -z "${NEW_ROW[$r]+set}" ]] && printf '%s\t%s\n' "$r" "$rest"
            done
        fi
        for r in "${!NEW_ROW[@]}"; do printf '%s\n' "${NEW_ROW[$r]}"; done
    } >"$tmp"
    # Rows in manifest order.
    awk -F'|' '{ r = $1; gsub(/[[:space:]]/, "", r) } r != "" && r !~ /^#/ { print r }' "$MANIFEST" >"$tmp.order"
    { grep '^#' "$tmp"; awk -F'\t' 'NR == FNR { o[$1] = NR; next } !/^#/ { print o[$1] "\t" $0 }' \
        "$tmp.order" "$tmp" | sort -n -k1,1 | cut -f2-; } >"$BASELINE"
    rm -f "${tmp:?}" "${tmp:?}.order"
    echo "loop-trace: blessed ${#NEW_ROW[@]} row(s) into $BASELINE"
fi

summary="rows=$n_run identical=$n_ident failed=$n_bad skipped=$n_skip"
if (( n_bad > 0 )); then echo "[loop-trace] game=$GAME verdict=FAIL $summary"; exit 1; fi
if (( n_run == 0 )); then echo "[loop-trace] game=$GAME verdict=SKIP $summary"; exit 64; fi
echo "[loop-trace] game=$GAME verdict=PASS $summary"
exit 0
