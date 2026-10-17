#!/usr/bin/env bash
# check_vbl_hold.sh - the engagement cases of VBL delivery's hold path and
# of the per-VBL split of an off-schedule VSync(n).
#
# Row hold/on-schedule (spyro-2, cardless oracle, full 20k): two runs under
# OPENPETE_VBL_HOLD=charge:6000:4 with OPENPETE_VBL_LEAF_CHECK=1, one holding
# the window and one dry (the same four deliveries counted and taken). Pass:
# the dry leg passes the oracle against the stock golden; the hold leg's
# oracle run diverges in no region and fails only on the anchor record after
# the flush, with openpete ahead of the reference by exactly D VBLs (the
# held VBLs the schedule does not place); the hold leg holds 4, flushes once, leaves D = 3 after the flush; each
# leg's in-binary hold window check passes (device VBL count advanced by 4,
# drive-head time at that count, openpete_vbl +1 held or +4 dry); the drive
# head at the window's end (device VBL, position, base LBA, start time,
# rate) is the same in both legs; and the device-half leaf check ran its
# rows (nonzero runs, the countdown row included in the hold leg) with no
# device row writing guest RAM or the scratchpad. The head compare is blind
# while the head is at rest: a window whose head rate is 0 adds the line
# `hold-on/moving-head VACUOUS`, which reports the gap and leaves the
# exit status to the hold-on verdict.
# No off-schedule row covers a blocking CdSync: every guest CdSync site of
# the three games passes mode 1, the poll, which cd_stream_pause_answer
# answers CdlNoIntr while a stream read's closing CdlPause is owed; the
# blocking wait that delivers VBLs (cd_stream_pause_wait) runs for mode 0
# only, so no input can engage such a row.
# Row cadence (spyro-2, headless under OPENPETE_FORCE_PACING=1 with
# --pace-log): every VSync(n > 1) the game makes off the schedule after the
# first paced VBL. Pass: the wait's n VBLs each have a VBL record, and the
# records' deadlines step one VBL period each (n periods for the wait).
# Engagement: waits checked, nonzero; a wait with no paced VBL record
# counts as unpaced, and checked = 0 is VACUOUS, which fails the harness.
#
# Verdict line per row: [vbl-hold] <row> <verdict> <numbers>.
# Usage: tests/harness/check_vbl_hold.sh [--build-dir=DIR] [--row=hold-on|cadence]
# Exit 0 PASS, 2 FAIL or VACUOUS, 64 SKIP (binary, disc or golden missing).
set -u
export OPENPETE_VIBRATION=0
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

only=""
for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
        --row=*) only="${a#--row=}";;
        *) echo "check_vbl_hold: unknown argument $a" >&2; exit 64;;
    esac
done
ROOT="${PG_BUILD_DIR:-build/linux-release}/vbl-hold"
mkdir -p "$ROOT"
failed=0 ran=0
verdict() { echo "[vbl-hold] $*"; ran=$((ran + 1)); case "$2" in PASS) ;; *) failed=1;; esac; }
want() { [[ -z "$only" || "$only" == "$1" ]]; }

# field <log> <sed expression>: the last match of a census field.
field() { sed -n "$2" "$1" | tail -1; }

# ── hold/on-schedule ─────────────────────────────────────────────────────
if want hold-on; then
    # The oracle runs the binary of --build-dir; its verdict decides with the
    # census (the dry leg passes; the hold leg fails only on the held VBLs).
    [[ -n "${PG_BUILD_DIR:-}" && -z "${OPENPETE_BIN:-}" ]] && export OPENPETE_BIN="$PG_BUILD_DIR/openpete"
    declare -A orc_of
    for leg in hold dry; do
        rm -f "${ROOT:?}/hold-on-$leg.log" "${ROOT:?}/hold-on-$leg.oracle"
        spec="charge:6000:4"; [[ $leg == dry ]] && spec="charge:6000:4:dry"
        OPENPETE_VBL_HOLD="$spec" OPENPETE_VBL_LEAF_CHECK=1 \
            OPENPETE_ORACLE_LOG="$ROOT/hold-on-$leg.log" \
            tests/harness/check_psx_oracle.sh --game spyro-2 >"$ROOT/hold-on-$leg.oracle" 2>&1
        orc=$?
        [[ $orc -eq 64 ]] && { echo "[vbl-hold] hold-on: $leg oracle SKIP ($ROOT/hold-on-$leg.oracle)" >&2; exit 64; }
        [[ -s "$ROOT/hold-on-$leg.log" ]] || { echo "[vbl-hold] hold-on: no $leg log" >&2; exit 64; }
        orc_of[$leg]=$orc
    done
    h="$ROOT/hold-on-hold.log" d="$ROOT/hold-on-dry.log"
    held=$(field "$h" 's/.*hold window closed: \([0-9]*\) held.*/\1/p')
    dry=$(field "$d" 's/.*hold window closed: \([0-9]*\) taken (dry).*/\1/p')
    flushes=$(field "$h" 's/.*hold window: held=[0-9]* flushes=\([0-9]*\).*/\1/p')
    dval=$(field "$h" 's/.*hold window closed: .*D=\(-\{0,1\}[0-9]*\)$/\1/p')
    head_h=$(field "$h" 's/.*hold window head: \(.*\)/\1/p')
    head_d=$(field "$d" 's/.*hold window head: \(.*\)/\1/p')
    chk_h=$(field "$h" 's/.*hold window check: \([A-Z]*\) .*/\1/p')
    chk_d=$(field "$d" 's/.*hold window check: \([A-Z]*\) .*/\1/p')
    leaf_runs=0 leaf_bad=0
    for f in "$h" "$d"; do
        r=$(field "$f" 's/.*leaf check: \([0-9]*\) device-row runs, [0-9]* wrote.*/\1/p')
        b=$(field "$f" 's/.*leaf check: [0-9]* device-row runs, \([0-9]*\) wrote.*/\1/p')
        leaf_runs=$((leaf_runs + ${r:-0})); leaf_bad=$((leaf_bad + ${b:-0}))
    done
    leaf_cd=$(field "$h" 's/.*leaf check row countdown: \([0-9]*\) runs.*/\1/p')
    # The hold leg's oracle: no region diverges, one hard fail, a vbl-delta
    # whose openpete-minus-reference VBL count equals D.
    nonneutral=$(field "$h" 's/.*first_non_neutral_div=\([a-z0-9]*\).*/\1/p')
    hard=$(grep -c 'psxtrace:   hard fail:' "$h")
    hard_vd=$(grep -c 'psxtrace:   hard fail: vbl-delta' "$h")
    vd_ref=$(field "$h" 's/.*first vbl-delta mismatch: record [0-9]* reference \([0-9]*\) openpete [0-9]*.*/\1/p')
    vd_op=$(field "$h" 's/.*first vbl-delta mismatch: record [0-9]* reference [0-9]* openpete \([0-9]*\).*/\1/p')
    vd_ahead=$(( ${vd_op:-0} - ${vd_ref:-0} ))
    oracle_ok=0
    [[ ${orc_of[dry]} -eq 0 && ${orc_of[hold]} -eq 2 && "$nonneutral" == none && $hard -eq 1 &&
       $hard_vd -eq 1 && -n "$vd_op" && "$vd_ahead" == "${dval:-x}" ]] && oracle_ok=1
    nums="oracle(dry)=rc${orc_of[dry]} oracle(hold)=rc${orc_of[hold]} non-neutral=${nonneutral:-?} hard=$hard vbl-delta ahead=$vd_ahead held=${held:-0} dry=${dry:-0} flushes=${flushes:-?} D=${dval:-?} check(hold)=${chk_h:-none} check(dry)=${chk_d:-none} leaf runs=$leaf_runs countdown=${leaf_cd:-0} writes=$leaf_bad head(hold)=[${head_h:-none}] head(dry)=[${head_d:-none}]"
    if [[ $oracle_ok -eq 1 && "${held:-0}" == 4 && "${dry:-0}" == 4 && "${flushes:-}" == 1 && "${dval:-}" == 3 &&
          "$chk_h" == PASS && "$chk_d" == PASS && $leaf_runs -gt 0 && "${leaf_cd:-0}" -gt 0 &&
          $leaf_bad -eq 0 && -n "$head_h" && "$head_h" == "$head_d" ]]; then
        verdict hold-on PASS "$nums"
    elif [[ "${held:-0}" == 0 ]]; then
        verdict hold-on VACUOUS "$nums"
    else
        verdict hold-on FAIL "$nums"
    fi
    rate=$(sed -n 's/.* rate=\([0-9]*\)$/\1/p' <<<"$head_h")
    [[ -n "$head_h" && "${rate:-0}" == 0 ]] &&
        echo "[vbl-hold] hold-on/moving-head VACUOUS head rate=${rate:-?}: the head compare ran on a head at rest"
fi

# ── cadence ──────────────────────────────────────────────────────────────
if want cadence; then
    pg_resolve spyro-2
    log="$ROOT/cadence.log" pace="$ROOT/cadence.pacelog"
    rm -f "$pace"
    PG_CAPTURE_ENV=(OPENPETE_FORCE_PACING=1)
    pg_capture "$log" "$pace" -- --no-card --frame-limit 1200 --pace-log "$pace" >/dev/null 2>&1
    line=$(python3 - "$log" "$pace" <<'EOF'
import re, sys
sys.path.insert(0, "scripts/pacing")
import pacelog
log, pace = sys.argv[1], sys.argv[2]
# One VBL period, 37609/2250000 s, in ns.
period = 2250000 / 37609
period_ns = 1e9 / period
waits = []
for ln in open(log, errors="replace"):
    m = re.search(r"VSync\((\d+)\) off the schedule: (\d+) VBLs from vbl (\d+) frame (\d+)", ln)
    if m:
        waits.append((int(m.group(2)), int(m.group(4))))
from pathlib import Path
L = pacelog.read(Path(pace))
recs = {}
for r in L.records:
    if r.type == pacelog.VBL and not (r.flags & pacelog.F_UNLOCK):
        recs.setdefault(r.index, r)
checked = fails = unpaced = 0
detail = []
for n, f in waits:
    idx = [f + k for k in range(n + 1)]
    if not all(i in recs for i in idx):
        unpaced += 1
        continue
    d = [recs[idx[k + 1]].deadline_ns - recs[idx[k]].deadline_ns for k in range(n)]
    checked += 1
    total = recs[idx[n]].deadline_ns - recs[idx[0]].deadline_ns
    ok = all(abs(x - period_ns) < 1000 for x in d)
    if not ok:
        fails += 1
    detail.append("frame %d n=%d: %.3f ms (%.2f periods)" % (f, n, total / 1e6, total / period_ns))
v = "VACUOUS" if checked == 0 else ("PASS" if fails == 0 else "FAIL")
print(v, "waits=%d checked=%d unpaced=%d fails=%d" % (len(waits), checked, unpaced, fails), "; ".join(detail))
EOF
)
    verdict cadence ${line:-FAIL the pace-log check printed no verdict}
fi

[[ $ran -gt 0 ]] || exit 64
[[ $failed -eq 0 ]] && exit 0 || exit 2
