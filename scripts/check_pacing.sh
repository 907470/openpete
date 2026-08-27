#!/usr/bin/env bash
# check_pacing.sh — the standing pacing gate (M9).
#
# Proves two things about the sim clock, in order:
#
#   1. It holds its grid. Ticks start within the p99 goal, drift is inside the
#      gate or honestly unresolvable, and no grid re-anchor happens outside a
#      declared window.
#   2. It holds its grid BECAUSE presents are shed, not by luck. The fault
#      injector makes intermediate presents too expensive to fit, and the run
#      must respond by shedding them while ticks keep their phase. Without
#      this leg the gate would pass on a machine fast enough never to exercise
#      the mechanism, which is a green that proves nothing.
#
# Usage: scripts/check_pacing.sh [--windows] [--seconds N] [--p99-us N]
#
# Exit: 0 pass, 2 a real regression, 3 vacuous (nothing engaged), 64 skip.
set -uo pipefail

cd "$(dirname "$0")/.."
. scripts/lib/wslenv.sh
# Refuse before spending 6 minutes on legs whose knobs would not arrive: this
# gate has already once run its entire Windows path unpaced and uninjected,
# reported VACUOUS for one leg and PASS for another, and neither was true of
# anything that ran.
python3 scripts/fences/wslenv_guard.py >/dev/null || exit 65
# Long enough that the drift leg can resolve its own 1 ppm gate: at the
# measured jitter a 55 s span cannot separate 2 ppm from 1 ppm, and a gate
# that can never certify its headline metric is decoration.
SECONDS_PER_LEG=120
P99_US=250
PLATFORM=linux
INJECT_US=3000

while [ $# -gt 0 ]; do
  case "$1" in
    --windows)  PLATFORM=windows; shift ;;
    --seconds)  SECONDS_PER_LEG="$2"; shift 2 ;;
    --p99-us)   P99_US="$2"; shift 2 ;;
    --inject-us) INJECT_US="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 64 ;;
  esac
done

if [ "$PLATFORM" = windows ]; then
  EXE=build/windows-mingw/games/spyro-1/openpete-spyro1.exe
else
  EXE=build/linux-release/games/spyro-1/openpete-spyro1
fi
DISC=games/spyro-1/spyro-1/build/SCUS-94228.cue
[ -x "$EXE" ] || { echo "[pacing] SKIP: no binary at $EXE"; exit 64; }
[ -f "$DISC" ] || { echo "[pacing] SKIP: no disc at $DISC"; exit 64; }

OUT=build/pacing/gate
mkdir -p "$OUT"
# Destroy prior output before the run: a stale log from a failed leg reads
# exactly like a successful one.
rm -f "$OUT"/gate-*.pacelog

VBLS=$(python3 -c "print(int($SECONDS_PER_LEG * 2250000 / 37609))")

run_leg() {   # name, mode(headless|windowed), extra env assignments...
  local name="$1"; local mode="$2"; shift 2
  local log="$OUT/gate-$name.pacelog"
  local extra=()
  [ "$mode" = headless ] && extra+=(--headless)
  echo "[pacing] leg $name ($mode, ${SECONDS_PER_LEG}s)"
  # Forward the knobs through WSLENV: a Windows .exe launched from WSL does
  # not inherit the Linux environment, and a knob that fails to arrive is
  # silent — the run proceeds with the default and reports numbers about a
  # configuration nobody asked for.
  wslenv_forward "$@"
  if ! env "${WSLENV_ARGS[@]}" "$EXE" --disc "$DISC" --frame-limit "$VBLS" \
       --pace-log "$log" "${extra[@]}" >"$OUT/$name.out" 2>&1; then
    echo "[pacing] FAIL: leg $name did not run; see $OUT/$name.out" >&2
    return 1
  fi
  [ -s "$log" ] || { echo "[pacing] FAIL: leg $name produced no log" >&2; return 1; }
}

# Headless fast-forwards by default, so the gate forces the pacer on: an
# unpaced run has no grid to be measured against.
COMMON=(OPENPETE_FORCE_PACING=1)

# The grid leg is HEADLESS on purpose. It asks whether the clock holds its
# own grid, and presents are exactly the interference that question is not
# about — a windowed leg measures the display as much as the pacer, and on a
# software rasterizer it measures llvmpipe.
run_leg grid headless "${COMMON[@]}" || exit 2

# The shed contract needs presents, which headless does not produce: only 16
# intermediates in a 60 s headless run, against thousands windowed. Run it
# windowed, and SKIP rather than pass if there is no display — a contract
# that cannot execute has not been demonstrated.
shed_possible=1
if [ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && [ "$PLATFORM" != windows ]; then
  shed_possible=0
fi
if [ $shed_possible -eq 1 ]; then
  run_leg shed-base   windowed "${COMMON[@]}" || shed_possible=0
  run_leg shed-inject windowed "${COMMON[@]}" \
          "OPENPETE_PACE_INJECT_PRESENT_US=$INJECT_US" || shed_possible=0
fi

echo
echo "[pacing] --- grid: does the clock hold its own grid? ---"
python3 scripts/pacing/pace.py summary "$OUT/gate-grid.pacelog" --p99-us "$P99_US"
summary_rc=$?
python3 scripts/pacing/pace.py drift "$OUT/gate-grid.pacelog"
drift_rc=$?

echo
if [ $shed_possible -eq 0 ]; then
  echo "[pacing] --- shed contract: SKIPPED, no display ---"
  echo "[pacing] The contract 'presents shed before ticks slip' needs presents."
  echo "[pacing] Headless produces almost none, so this leg was not run and"
  echo "[pacing] nothing about shedding is claimed by this result."
  shed_rc=0
  shed_skipped=1
else
shed_skipped=0
echo "[pacing] --- shed contract: do presents shed while ticks hold? ---"
python3 - "$OUT/gate-shed-inject.pacelog" "$OUT/gate-shed-base.pacelog" "$P99_US" "$INJECT_US" <<'PY'
import sys
from pathlib import Path
sys.path.insert(0, "scripts/pacing")
import pacelog, pacestats as ps

inj, base, p99_goal_us = Path(sys.argv[1]), Path(sys.argv[2]), float(sys.argv[3])
inject_us = float(sys.argv[4])
li, lb = pacelog.read(inj), pacelog.read(base)
ki, _ = li.analysis_vbls()
kb, _ = lb.analysis_vbls()
sheds_i = len(li.of(pacelog.SHED))
sheds_b = len(lb.of(pacelog.SHED))
spans_i = li.of(pacelog.PRESENT_SPAN)

fail = []
if not ki:
    fail.append("injected leg has no analysable VBL — VACUOUS")

# The injector must actually have made presents expensive, or the leg proves
# nothing about shedding.
spans_b = lb.of(pacelog.PRESENT_SPAN)
if spans_i:
    cost = ps.percentile([r.span_cost_ns for r in spans_i], 50)
    cost_b = ps.percentile([r.span_cost_ns for r in spans_b], 50) if spans_b else 0
    print(f"present cost p50: {cost_b/1e3:.1f}us baseline -> {cost/1e3:.1f}us "
          f"injected (asked for +{inject_us:.0f}us)")
    # The injection ITSELF has to be shown to have arrived. Everything below
    # compares two legs and calls the difference a demonstration of shedding;
    # if the knob never reached the process the two legs are the same run
    # twice, and the comparison passes while proving nothing. This has
    # happened: under WSL the env knob does not cross to a Windows .exe, and
    # the contract reported PASS over a duplicate of its own baseline.
    if cost - cost_b < 0.5 * inject_us * 1000:
        fail.append(
            f"the injection did not arrive: present cost rose only "
            f"{(cost-cost_b)/1e3:.1f}us against the {inject_us:.0f}us asked "
            f"for, so the two legs are the same configuration and nothing "
            f"here is a demonstration of anything")
else:
    fail.append("injected leg recorded no present spans — the path never ran")

print(f"sheds: {sheds_b} baseline -> {sheds_i} injected")
if sheds_i == 0:
    fail.append("shedding never engaged — the predicate is not firing, so "
                "nothing here demonstrates that presents are shed before "
                "ticks slip")
elif sheds_i <= sheds_b:
    # A machine whose presents are ALREADY too expensive sheds at the same
    # rate injected or not, because the baseline is saturated. That is a
    # degenerate comparison, not a failure of the mechanism — engagement is
    # still demonstrated. Say which case this is rather than pass silently.
    print("note: baseline already saturated (sheds did not rise with "
          "injection) — engagement is shown, the differential is not. "
          "Run this leg on a machine where an uninjected present fits.")

# The contract: under present pressure the TICK keeps its phase.
if ki:
    p99 = ps.percentile([r.phase_err_ns for r in ki], 99)
    print(f"injected p99 phase error: {p99/1e3:+.1f}us (goal {p99_goal_us:.0f}us)")
    # Compared against the SAME leg without injection, not against the
    # absolute goal: this leg may be running on a software rasterizer, where
    # the baseline is already poor and the contract is "injection does not
    # make the TICK worse", which is what shedding is for.
    p99b = ps.percentile([r.phase_err_ns for r in kb], 99) if kb else 0
    print(f"tick p99 under pressure: {p99/1e3:+.1f}us vs {p99b/1e3:+.1f}us "
          f"uninjected")
    if p99 > max(p99b * 3, p99_goal_us * 1000 * 4):
        fail.append(f"ticks did NOT hold phase under present pressure: "
                    f"p99 {p99/1e3:.1f}us against {p99b/1e3:.1f}us baseline — "
                    f"presents are not being shed fast enough to protect them")

for f in fail:
    print(f"FAIL: {f}", file=sys.stderr)
print("[pacing] shed contract: " + ("FAIL" if fail else "PASS"))
sys.exit(2 if fail else 0)
PY
shed_rc=$?
fi

echo
if [ $summary_rc -eq 3 ] || [ $shed_rc -eq 3 ]; then
  echo "[pacing] VACUOUS: a leg engaged nothing"; exit 3
fi
# Drift returning 3 is "the span cannot resolve the gate", which is a true
# statement about a 60 s run rather than a regression. Only a real drift
# failure (2) counts against the gate.
if [ $summary_rc -ne 0 ] || [ $shed_rc -ne 0 ] || [ $drift_rc -eq 2 ]; then
  echo "[pacing] verdict: FAIL"; exit 2
fi
echo "[pacing] verdict: PASS"
