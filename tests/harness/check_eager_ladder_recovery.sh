#!/usr/bin/env bash
# check_eager_ladder_recovery.sh - the eager decimation ladder rises under an
# injected production stall and settles back at its pre-stall level within a
# bounded number of spans after the stall ends; a level-entry stall does not
# hold the level either.
#
# Class: the eager planner divides each tick's span into display slots and
# produces every level-th one, the level chosen from how many scenes the
# span's budget affords (the divisor ladder; vsync on). A stall raises the
# level; the ladder must give it back once the spans afford level 1 again. A
# ladder that holds the raised level (a budget priced from a stalled tick, a
# fall that waits on more than one headroom run, a fall that never comes)
# shows 480/level presents per second for seconds after every stall.
#
# Path: --vsync 1 on both legs. With --vsync 0 a span is free (no display
# grid) and the divisor ladder does not run. Under Xvfb the immediate and
# mailbox present modes complete about one present per second
# (vkQueuePresentKHR blocks 0.4-1.5 s with Xvfb idle), which leaves a free
# span one slot and fills the scene ring; FIFO presents at 2-120 per second.
# Xvfb reports no refresh rate, so the grid is the render grid floored at the
# present thread's cost (render plus the blocking present step): one slot per
# span whenever that cost passes about half a tick, as it does on a loaded
# host at --tick-scale 1.
#
# Leg inject: --tick-scale TICK_SCALE (0.25) stretches the tick and the
# render grid together, so a span holds 2-10 slots past the present thread's
# cost floor. OPENPETE_PACE_INJECT_PRESENT_US=INJECT_US (100 ms: 25 ms, most
# of an unscaled span's ~30 ms budget, stretched by 1 / TICK_SCALE) is spent
# in every intermediate scene produced while openpete_frame is in [FROM, TO)
# (OPENPETE_PACE_INJECT_PRESENT_VBL). The cost estimate then affords one or
# two scenes a span, the guard cuts spans, and the ladder rises. After TO the
# estimate decays toward the true cost and the level must fall.
#
# Leg entry: --tick-scale 1, no injection, through the level entry (VBL
# ~1864 runs one update VBL ~47 ms, past half a tick). A span flagged late or
# priced at budget 0 after the first WARMUP spans is a stall; one followed
# within 4 spans by a level above the level before it must settle within
# MAX_SPANS of the stall. A stretched tick would not classify the entry as a
# stall, so this leg runs unscaled.
#
# Row: spyro-3 portal-117-midday-gardens-icy-peak.bk2 (the title loads slot 1
# of a scratch copy of games/spyro-3/cards/117-percent.mcr). The window
# [2200, 2320) sits in icy-peak gameplay.
#
# Instrument: the pace log's EAGER_PLAN record, one per span (index =
# openpete_frame, the span's level, slots, afford, cut and late flags), read
# through tools/bugreport.py.
#   base       the level of the last span planned before FROM.
#   settle     the first span at or after TO from which every span planned
#              in the next SETTLE_SPANS spans is at or below base.
#   raised     spans planned at a level above base from FROM until settle.
#   recovery   spans planned from TO up to settle.
#
# Pass: inject raised > 0 and recovery <= MAX_SPANS; every raised entry-leg
# stall settles within MAX_SPANS. MAX_SPANS = 10: one headroom run (8 clean
# spans, EAGER_HEADROOM_RUNS in core/runtime/eager.c) after the cut the last
# stale estimate reports one or two spans past TO. VACUOUS (exit 2): eager
# production did not engage on the inject leg, no span was planned in its
# window, or none was raised. The entry leg's stall count is reported; an
# entry leg with no stall does not make the verdict VACUOUS.
#
# Every launch passes --input-lock --vibration 0 --background 1, so physical
# input cannot reach the run and the window takes no focus. Without a display
# the runs are wrapped in xvfb-run when it is installed.
#
# Verdict line:
#   [ladder-recovery] game=spyro-3 window=<from>,<to> base=<n> raised=<n>
#                     max_level=<n> recovery_spans=<n|never> max_spans=<n>
#                     stalls=<n> stall_worst_spans=<n> verdict=PASS|FAIL|VACUOUS
#
# --analyze=INJECT_PACE[,ENTRY_PACE] skips the runs and grades existing pace
# logs of the same legs (ones the Windows exe wrote, say).
#
# Usage: tests/harness/check_eager_ladder_recovery.sh [--build-dir=DIR] [--out=DIR]
#            [--render-fps=N] [--inject-us=N] [--tick-scale=F] [--window=FROM,TO]
#            [--analyze=INJECT_PACE[,ENTRY_PACE]]
# Exit 0 PASS, 1 FAIL, 2 VACUOUS, 64 SKIP (no display, binary, disc or card).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

# Unattended launcher: every run this script starts is silenced.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

OUT=""
RENDER_FPS=480
INJECT_US=100000
TICK_SCALE=0.25
WINDOW=2200,2320
ANALYZE=""
for a in "$@"; do
    case "$a" in
        --build-dir=*)  PG_BUILD_DIR="${a#--build-dir=}";;
        --out=*)        OUT="${a#--out=}";;
        --render-fps=*) RENDER_FPS="${a#--render-fps=}";;
        --inject-us=*)  INJECT_US="${a#--inject-us=}";;
        --tick-scale=*) TICK_SCALE="${a#--tick-scale=}";;
        --window=*)     WINDOW="${a#--window=}";;
        --analyze=*)    ANALYZE="${a#--analyze=}";;
    esac
done
FROM="${WINDOW%,*}"
TO="${WINDOW#*,}"

GAME=spyro-3
pg_resolve "$GAME"
BK2=games/spyro-3/bk2/portal-117-midday-gardens-icy-peak.bk2
CARD_SRC=games/spyro-3/cards/117-percent.mcr
# --frame-limit counts VBLs from launch; openpete_frame trails it by the
# ~573 VBLs before the guest's first frame: 3100 reaches frame ~2527, 2600
# frame ~2027.
INJECT_FRAME_LIMIT=3100
ENTRY_FRAME_LIMIT=2600
MAX_SPANS=10
SETTLE_SPANS=16
WARMUP=60
ROW_RUNTIME=900
ROW_TIMEOUT=960

if [[ -n "$ANALYZE" ]]; then
    PACE="${ANALYZE%%,*}"
    ENTRY_PACE=""
    [[ "$ANALYZE" == *,* ]] && ENTRY_PACE="${ANALYZE#*,}"
    [[ -s "$PACE" ]] || { echo "ladder-recovery: FAIL (no pace log at $PACE)"; exit 1; }
else
[[ -x "$PG_BIN" ]] || { echo "ladder-recovery: SKIP (no binary at $PG_BIN)"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "ladder-recovery: SKIP (no disc at $PG_DISC)"; exit 64; }
[[ -f "$CARD_SRC" && -f "$BK2" ]] || { echo "ladder-recovery: SKIP (no $CARD_SRC or $BK2)"; exit 64; }
WRAP=()
if [[ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]]; then
    command -v xvfb-run >/dev/null ||
        { echo "ladder-recovery: SKIP (no display and no xvfb-run)"; exit 64; }
    WRAP=(xvfb-run -a -s "-screen 0 1280x960x24")
fi

[[ -n "$OUT" ]] || OUT="${PG_BUILD_DIR:-build/linux-release}/ladder-recovery"
mkdir -p "$OUT"
PACE="$OUT/$GAME-inject.oppl"
ENTRY_PACE="$OUT/$GAME-entry.oppl"
rm -f "$OUT/$GAME-inject.log" "$OUT/$GAME-entry.log" "$PACE" "$ENTRY_PACE" "$OUT/$GAME-card1.mcr"

# run_leg TAG FRAME_LIMIT TICK_SCALE [ENV...]: one windowed row; its log and
# pace log are $OUT/$GAME-TAG.{log,oppl}.
run_leg() {
    local tag="$1" limit="$2" scale="$3"
    shift 3
    local card="$OUT/$GAME-card1.mcr" log="$OUT/$GAME-$tag.log" pace="$OUT/$GAME-$tag.oppl"
    cp "$CARD_SRC" "$card"
    op_own_run env OPENPETE_VIBRATION=0 OPENPETE_TERRAIN_CACHE_FORCE=1 "$@" \
        "${WRAP[@]}" timeout -k 10 "$ROW_TIMEOUT" "$PG_BIN" \
        --no-config --disc "$PG_DISC" --card1 "$card" --no-card2 --mute 1 \
        --vibration 0 --input-lock --background 1 \
        --renderer vk --render-fps "$RENDER_FPS" --vsync 1 --tick-scale "$scale" \
        --bk2 "$BK2" --frame-limit "$limit" --max-runtime-seconds "$ROW_RUNTIME" \
        --pace-log "$pace" </dev/null >"$log" 2>&1
    local rc=$?
    [[ -s "$log" && -s "$pace" ]] ||
        { echo "ladder-recovery: FAIL ($tag leg wrote no log or pace log, rc=$rc; $log)"; exit 1; }
}
run_leg inject "$INJECT_FRAME_LIMIT" "$TICK_SCALE" \
    OPENPETE_PACE_INJECT_PRESENT_US="$INJECT_US" OPENPETE_PACE_INJECT_PRESENT_VBL="$FROM,$TO"
run_leg entry "$ENTRY_FRAME_LIMIT" 1
fi

python3 - "$PACE" "$ENTRY_PACE" "$FROM" "$TO" "$MAX_SPANS" "$SETTLE_SPANS" "$WARMUP" "$GAME" <<'EOF'
import sys
sys.path.insert(0, "tools")
import bugreport as br

pace, entry_pace, frm, to, max_spans, settle_n, warmup, game = sys.argv[1:9]
frm, to, max_spans, settle_n, warmup = int(frm), int(to), int(max_spans), int(settle_n), int(warmup)


def plans(path):
    """The EAGER_PLAN records of a pace log, decoded, in span order."""
    _, recs = br.pace_records(open(path, "rb").read())
    return [br.plan_decode(r) for r in recs if r[0] == br.PACE_EAGER_PLAN]


def settle(P, start, base):
    """Index into P of the first span at or after P[start] from which settle_n spans stay at or below base; None if the log ends first."""
    for k in range(start, len(P)):
        run = P[k:k + settle_n]
        if len(run) < settle_n:
            return None
        if all(p["level"] <= base for p in run):
            return k
    return None


verdict, why = "PASS", ""
P = plans(pace)
pre = [k for k, p in enumerate(P) if p["index"] < frm]
win = [k for k, p in enumerate(P) if frm <= p["index"] < to]
after = [k for k, p in enumerate(P) if p["index"] >= to]
base = P[pre[-1]]["level"] if pre else 1
raised, rec, maxl = 0, None, base
if not P or not win:
    verdict, why = "VACUOUS", "no span planned in the window" if P else "no EAGER_PLAN record (eager did not engage)"
elif not after:
    verdict, why = "FAIL", "the log ends before the window does"
else:
    s = settle(P, after[0], base)
    rs = [p for p in P[win[0]:(s if s is not None else len(P))] if p["level"] > base]
    raised = len(rs)
    maxl = max([p["level"] for p in rs] or [base])
    rec = None if s is None else s - after[0]
    if raised == 0:
        verdict, why = "VACUOUS", "no span from the window on planned above the base level"
    elif rec is None or rec > max_spans:
        verdict, why = "FAIL", "level not back at base within the bound"

stalls, worst = 0, 0
E = plans(entry_pace) if entry_pace else []
for k, p in enumerate(E):
    if k < warmup or not (p["late"] or p["budget"] <= 0.0):
        continue
    stalls += 1
    b = E[k - 1]["level"]
    if not any(q["level"] > b for q in E[k:k + 4]):
        continue
    s = settle(E, k, b)
    spans = (s - k) if s is not None else len(E) - k
    print(f"  stall span VBL {p['index']}: level raised above {b}, settled in {spans} span(s)", file=sys.stderr)
    worst = max(worst, spans)
    if spans > max_spans and verdict != "FAIL":
        verdict, why = "FAIL", f"level-entry stall at VBL {p['index']} held the level {spans} spans"

print(f"[ladder-recovery] game={game} window={frm},{to} base={base} raised={raised} max_level={maxl} "
      f"recovery_spans={'never' if rec is None else rec} max_spans={max_spans} stalls={stalls} "
      f"stall_worst_spans={worst} verdict={verdict}{' (' + why + ')' if why else ''} "
      f"pace={pace}{',' + entry_pace if entry_pace else ''}")
sys.exit({"PASS": 0, "FAIL": 1, "VACUOUS": 2}[verdict])
EOF
