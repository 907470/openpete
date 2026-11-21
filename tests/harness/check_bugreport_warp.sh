#!/usr/bin/env bash
# check_bugreport_warp.sh - a dev-overlay level warp inside a bug-report
# bundle's window replays from the bundle's warps.txt.
#
# Recording (Spyro 2, headless, --skip-to-level summer_forest): the harness
# knob schedule OPENPETE_BUGREPORT_KNOB_AT selects Glimmer in the warp_level
# row and presses the warp_go button at feed key WARP_AT, the setters a
# click on the dev overlay calls; the press (OPENPETE_BUGREPORT_AT) is at
# PRESS_AT, after the level load. The bundle's anchor is the session's second
# (near VBL 1796, in Summer Forest), before the warp.
#
# A second recording (hold) presses the same warp with an extra anchor due at
# feed key HOLD_AT (OPENPETE_BUGREC_ANCHOR_AT), while the warp is in flight,
# and no press.
#
# Replays:
#   warp      the bundle as written
#   warp-old  the bundle without warps.txt and its manifest entry, the shape
#             a bundle written before the member has (negative control: the
#             replay cannot warp)
#
# Instrument: the hold recording's `bugrec: anchors taken=` exit line (its
# hold= deferral count); the recording's `[levelwarp] arrived in level 11` line and
# warps.txt; the replay's verdict line (`bug-report-replay: IDENTICAL |
# DIVERGED ...`), its `warp warp_go to 11 queued at frame F` line and its
# `warps:` summary line.
#
# Pass criterion: the recording arrives in Glimmer before the press and
# warps.txt holds one warp_go line to 11 after the bundle's anchor frame;
# the hold recording defers at least one final yield on hold (zero reads
# VACUOUS: the anchor hold never engaged);
# warp IDENTICAL with the warp queued at the recorded frame (queued 1, 0
# past their frame, 0 not reached); warp-old DIVERGED at a tick at or after
# the recorded warp frame, labelled warps-unrecorded.
#
# Verdict line:
#   [bugreport-warp] frame=<F> hold=<deferrals> warp=<verdict> warp-old=<verdict>@<tick> verdict=PASS|FAIL
#
# Usage: tests/harness/check_bugreport_warp.sh [--build-dir=DIR]
# Exit 0 PASS, 1 FAIL, 64 SKIP (binary or disc missing).
# xvfb-guard: no-launch (every run is headless)
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
    esac
done

GAME=spyro-2
pg_resolve "$GAME"
[[ -x "$PG_BIN" && -f "$PG_DISC" ]] ||
    { echo "bugreport-warp: SKIP (binary or disc missing)"; exit 64; }

OUT="${PG_BUILD_DIR:-build/linux-release}/bugreport-warp"
rm -rf "${OUT:?}"
mkdir -p "$OUT"
ABS_OUT="$(cd "$OUT" && pwd)"
PG_CAPTURE_TIMEOUT=600
WARP_AT=2600
PRESS_AT=4700
HOLD_AT=2700

fail=()
PG_CAPTURE_ENV=("OPENPETE_SANDBOX=$ABS_OUT/xdg-rec" "OPENPETE_BUGREPORT_AT=$PRESS_AT" "OPENPETE_BUGREPORT_NOTE=0"
                "OPENPETE_BUGREPORT_KNOB_AT=$WARP_AT:warp_level=Glimmer,$WARP_AT:warp_go=1")
pg_capture "$OUT/rec.log" "" -- --game "$GAME" --mute 1 --frame-limit $((PRESS_AT + 20)) \
    --skip-to-level summer_forest </dev/null || fail+=("recording failed (see $OUT/rec.log)")
zip="$(find "$OUT/xdg-rec" -name 'bug-*.zip' | head -n 1)"
[[ -n "$zip" ]] || fail+=("the recording wrote no bundle (see $OUT/rec.log)")
grep -q 'harness knob warp_go = 1 at feed key' "$OUT/rec.log" || fail+=("the warp button was not pressed")
grep -q 'levelwarp: arrived in level 11 ' "$OUT/rec.log" || fail+=("the recording did not arrive in Glimmer")
if (( ${#fail[@]} )); then
    printf 'bugreport-warp: %s\n' "${fail[@]}"
    echo "[bugreport-warp] verdict=FAIL"
    exit 1
fi

PG_CAPTURE_ENV=("OPENPETE_SANDBOX=$ABS_OUT/xdg-hold" "OPENPETE_BUGREC_ANCHOR_AT=$HOLD_AT"
                "OPENPETE_BUGREPORT_KNOB_AT=$WARP_AT:warp_level=Glimmer,$WARP_AT:warp_go=1")
pg_capture "$OUT/rec-hold.log" "" -- --game "$GAME" --mute 1 --frame-limit $((HOLD_AT + 300)) \
    --skip-to-level summer_forest </dev/null || fail+=("hold recording failed (see $OUT/rec-hold.log)")
hold="$(grep -oE 'bugrec: anchors taken=.* hold=[0-9]+ ' "$OUT/rec-hold.log" | grep -oE 'hold=[0-9]+' | head -n 1)"
hold="${hold#hold=}"
if (( ${hold:-0} == 0 )); then
    fail+=("hold: VACUOUS: no final yield was deferred on the warp's anchor hold (see $OUT/rec-hold.log)")
    hold=VACUOUS
fi

# The warp line after the anchor: its frame, or empty.
frame="$(python3 -I - "$zip" <<'PY'
import json, sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
anchor = json.loads(z.read("manifest.json"))["anchor"]["frame"]
rows = [l.split("\t") for l in z.read("warps.txt").decode().splitlines() if l.startswith("warp\t")]
hit = [r for r in rows if r[4] == "warp_go" and r[5] == "11" and int(r[1]) > anchor]
print(hit[0][1] if len(hit) == 1 and len(rows) == 1 else "")
PY
)"
[[ -n "$frame" ]] || fail+=("warps.txt holds no single warp_go line to 11 after the anchor")

# replay TAG BUNDLE: one replay; sets LOG.
replay() {
    local tag="$1" bundle="$2"
    LOG="$OUT/replay-$tag.log"
    PG_CAPTURE_ENV=("OPENPETE_BUGREPORT_REPLAY_DIR=$OUT/scratch-$tag")
    pg_capture "$LOG" "" -- --game "$GAME" --mute 1 --bug-report-replay "$bundle" </dev/null
}

# verdict LOG: the verdict word of a replay log, or "none".
verdict() {
    local v
    v="$(grep -oE 'bug-report-replay: (IDENTICAL|DIVERGED|REFUSED|VACUOUS|REPRODUCED)' "$1" | tail -n 1)"
    v="${v#bug-report-replay: }"
    echo "${v:-none}"
}

replay warp "$zip"
v_warp="$(verdict "$LOG")"
[[ "$v_warp" == IDENTICAL ]] || fail+=("warp: $v_warp, want IDENTICAL (see $LOG)")
grep -q "bug-report-replay: warp warp_go to 11 queued at frame ${frame:-x} (drained at frame ${frame:-x} " "$LOG" ||
    fail+=("warp: the warp was not queued at the recorded frame ${frame:-?}")
grep -q 'bug-report-replay: warps: 1 queued (0 past their frame), 0 not reached' "$LOG" ||
    fail+=("warp: no 'warps: 1 queued (0 past their frame), 0 not reached' line")

python3 -I - "$zip" "$OUT/warp-old.zip" <<'PY'
import json, sys, zipfile
src = zipfile.ZipFile(sys.argv[1])
m = json.loads(src.read("manifest.json"))
m["members"] = [x for x in m["members"] if x["name"] != "warps.txt"]
with zipfile.ZipFile(sys.argv[2], "w", zipfile.ZIP_DEFLATED) as out:
    for n in src.namelist():
        if n == "manifest.json":
            out.writestr(n, json.dumps(m))
        elif n != "warps.txt":
            out.writestr(n, src.read(n))
PY
replay warp-old "$OUT/warp-old.zip"
v_old="$(verdict "$LOG")"
t_old="$(grep -oE '^bug-report-replay: DIVERGED tick=[0-9]+' "$LOG" | grep -oE '[0-9]+$')"
[[ "$v_old" == DIVERGED ]] || fail+=("warp-old: $v_old, want DIVERGED (see $LOG)")
(( ${t_old:-0} >= ${frame:-1} )) || fail+=("warp-old: diverged at tick ${t_old:-none}, before the warp frame ${frame:-?}")
grep -q 'labels=[^ ]*warps-unrecorded' "$LOG" || fail+=("warp-old: not labelled warps-unrecorded")

res=PASS
if (( ${#fail[@]} )); then
    printf 'bugreport-warp: %s\n' "${fail[@]}"
    res=FAIL
fi
echo "[bugreport-warp] frame=${frame:-none} hold=$hold warp=$v_warp warp-old=$v_old@${t_old:-none} verdict=$res"
[[ "$res" == PASS ]]
