#!/usr/bin/env bash
# check_occl_flicker.sh — terrain occlusion-cull flicker census on a moving
# camera, windowed on the real GPU.
#
# The occluder depth grid advances once per present; the canonical terrain
# walk runs once per tick. A cull verdict taken from a grid built under a
# different camera removes sectors that camera never proved hidden, and the
# terrain they carry blinks out for part of a tick. The query therefore
# refuses the depth verdict outside a camera bound, and this census measures
# what is left.
#
# Windowed, not headless, and not a ctest: a headless run holds no occlusion
# grid and emits no intermediate presents, so the class is structurally
# invisible to the deterministic gates. Run it by hand, or from a script that
# has a display and a GPU.
#
# The display and the CPU must be the run's alone. A second windowed game process on the
# same display costs this one two orders of magnitude of walk rate, and it
# also pushes the camera delta between presents past the bound, so every
# query is refused and the run reports VACUOUS instead of a verdict. Heavy
# CPU load from other processes does the same: the recompiled CPU is one
# thread, and at a machine load above the core count it walks at 3 to 25
# per minute against the roughly 150 the legs need.
#
# Legs: the camera flies one waypoint segment over 240 ticks and over 60
# ticks, a per-tick step of about 183 and about 730 world units, both at
# --render-fps 60 with vsync off so several presents fall inside a tick. Each
# leg is written "<segment ticks>:<ticks to census>:<engagement class>".
#
# Instruments (stderr):
#   OPENPETE_TERRAIN_CAMDBG=1   [cam f=N] pos=(x, y, z) yaw=.. pitch=..
#   OPENPETE_TERRAIN_FARDBG=<frame> OPENPETE_TERRAIN_FARDBG_N=<walks>
#                               [fardbg-hdr] one line per walk, carrying the
#                               occlusion-cull count and the grid age, and
#                               [fardbg-secend] one line per emitted sector
#   OPENPETE_OCCL_CAMDBG=1      [occl-inbound] one line per admitted query,
#                               carrying the camera delta d and the rotation
#                               as a matrix trace tr
#   OPENPETE_MOBY_OCCL_DBG=1    [moby-occl] a query-outcome census each second,
#                               including the queries refused because the
#                               camera had moved
#
# Counted per leg:
#   holes       a sector an intermediate walk of a tick drops although its
#               tick's canonical walk and the previous tick's both emitted
#               it. A sector the previous tick did not emit is entering the
#               view, counted separately as `entering`.
#   flicker     a sector two neighbouring canonical walks emit and the walk
#               between them does not.
#   latoff      sectors a re-extract culls whose canonical walk rejected them
#               off screen while the re-extract's own rect, under its own
#               camera, is on screen ([fardbg-hdr] latoff=). The interpolated
#               camera lags the tick camera, so a reused frustum verdict drops
#               terrain still in frame. Reported summed over the re-extracts,
#               as their per-walk maximum, and summed over the canonical walks
#               (0 by construction: a canonical walk tests its own rect).
#               offadm is the same set kept, the engagement of the per-walk
#               frustum test.
#   wrong       the holes and flicker events at a tick that admitted a query
#               whose camera was outside the shipped bound, so the grid it
#               read was rasterized under another camera and the sectors it
#               removed were never proved hidden. An event at a tick whose
#               queries were all in bound is a sound cull: the grid and the
#               querying camera agree, and the sector is genuinely hidden.
#
# Game-visibility bk2 leg: a movie replay under --terrain-visibility game,
# where the terrain cull's candidate set is the game's own visibility list and
# an inflated rect can lie wholly off screen, so the per-walk frustum test
# engages. Written "<bk2>:<first VBL>:<last VBL>:<tick scale>"; the replay runs
# at --tick-scale so each tick carries many re-extracts. Counted over the
# window: latoff and offadm summed over the re-extracts, latoffCan over the
# canonical walks, and admitted queries. Pass: latoff == 0 with offadm > 0 and
# admitted > 0; VACUOUS when either count is 0, when fewer than MIN_WALKS
# walks fall in the window, or when the build carries no latoff column.
#
# Pass criterion: wrong == 0 on every freecam leg; the bk2 leg as above. holes, entering and flicker are
# reported as diagnostics; they carry sound culls and view entries as well as
# defects, so they name where to look rather than decide.
# Engagement: each leg needs at least MIN_WALKS walks, at least one
# admitted query for the classifier to judge, a mean occlusion count per walk
# above 0 (a run whose cull never removed anything proves nothing), and a
# camera that held its waypoints, checked against the flown segment.
#
# Negative control, restoring the pre-bound behaviour in the same binary:
#   OPENPETE_OCCL_CAM_MOVE=1000000 OPENPETE_OCCL_CAM_TURN=180 \
#     scripts/check_occl_flicker.sh
# The classifier keeps the shipped bound whatever those are set to, so the
# control's admitted out-of-bound queries are scored as wrong verdicts.
#
# Usage: scripts/check_occl_flicker.sh [--game SLUG] [--build-dir=DIR]
#   OPENPETE_OCCL_FLICKER_EXE   the exe to drive (default: the worktree's
#                               windows-mingw build of the game); a Linux
#                               binary runs windowed on $DISPLAY
#   OPENPETE_OCCL_FLICKER_ONLY  freecam or bk2: run that leg kind alone
#   OPENPETE_OCCL_FLICKER_OUT   output directory (default: the build tree)
# Exit 0 PASS, 1 FAIL, 64 SKIP (exe, disc or python3 missing, or the game has
# no measured flicker path).
set -u
cd "$(dirname "$0")/.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh
# shellcheck source=scripts/lib/wslenv.sh
source scripts/lib/wslenv.sh

# Unattended launcher: no one is holding the pad and the buzzing appears in no
# log, so every run this script starts is silenced.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

PG_BUILD_DIR="build/windows-mingw"
for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
    esac
done

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"

# Per game: the level, the two waypoints of the flown segment, the frame the
# per-walk dump starts at, and the walk-count floor. A game with no row has no
# measured flicker path and the check skips.
case "$GAME" in
  spyro-3)
    LEVEL="sunrise_spring"
    WP_A="39414,13687,18388,1206,-67"
    WP_B="27442,55589,22914,1206,-67"
    LEGS="240:240:admitted 60:120:queries"
    DUMP_FROM=1100
    MIN_WALKS=200
    BK2_LEGS="sunrise-spring-camera-swing:1500:2300:0.1"
    ;;
  *) LEVEL="";;
esac
BK2_LEGS="${BK2_LEGS:-}"
ONLY="${OPENPETE_OCCL_FLICKER_ONLY:-}"
case "$ONLY" in
  "")      ;;
  freecam) BK2_LEGS="";;
  bk2)     LEGS="";;
  *) echo "occl-flicker: OPENPETE_OCCL_FLICKER_ONLY must be freecam or bk2"; exit 64;;
esac
[[ -n "$LEVEL" ]] || { echo "occl-flicker: SKIP ($GAME has no measured flicker path)"; exit 64; }

# The shipped bound the classifier judges against, independent of what the run
# is configured with, so the negative control's admitted queries are scored as
# the wrong verdicts they are. Mirrors occl_cam_move and occl_cam_turn_trace.
SHIPPED_MOVE=24.0
SHIPPED_TURN_DEG=0.35

EXE="${OPENPETE_OCCL_FLICKER_EXE:-$PG_BUILD_DIR/games/$GAME/$(pg_bin_name "$GAME").exe}"
# The disc reaches the exe as a link-free absolute path. A relative path is
# resolved against the Windows view of the working directory, and a path with
# a link in it is not resolved at all: either way the game aborts part way
# through the load, after it has already printed the EXE it found.
DISC_ABS="$(cd "$(dirname "$PG_DISC")" 2>/dev/null && pwd -P)/$(basename "$PG_DISC")"
[[ -x "$EXE" && -f "$DISC_ABS" ]] ||
    { echo "occl-flicker: SKIP (exe or disc missing: $EXE)"; exit 64; }
python3 -c '' 2>/dev/null || { echo "occl-flicker: SKIP (python3 missing)"; exit 64; }

OUT="${OPENPETE_OCCL_FLICKER_OUT:-$PG_BUILD_DIR/occl-flicker}"
mkdir -p "$OUT"

# The knobs the run needs, plus whatever the caller set for the negative
# control. A Windows child reads no Linux environment, so each name crosses
# through WSLENV or the exe silently runs a different configuration.
ENVS=(OPENPETE_VIBRATION=0
      OPENPETE_MOBY_OCCL_DBG=1
      OPENPETE_TERRAIN_CAMDBG=1
      OPENPETE_TERRAIN_FARDBG="$DUMP_FROM"
      OPENPETE_TERRAIN_FARDBG_N=100000
      OPENPETE_TERRAIN_FARDBG_LEAN=1
      OPENPETE_OCCL_CAMDBG=1)
[[ -n "${OPENPETE_OCCL_CAM_MOVE:-}" ]] && ENVS+=(OPENPETE_OCCL_CAM_MOVE="$OPENPETE_OCCL_CAM_MOVE")
[[ -n "${OPENPETE_OCCL_CAM_TURN:-}" ]] && ENVS+=(OPENPETE_OCCL_CAM_TURN="$OPENPETE_OCCL_CAM_TURN")
wslenv_forward "${ENVS[@]}"

rc_all=0
legs_n=0
legs_skipped=0
for LEG in $LEGS; do
    TICKS="${LEG%%:*}"; ENGAGE="${LEG##*:}"
    CENSUS="${LEG#*:}"; CENSUS="${CENSUS%%:*}"
    LOG="$OUT/$GAME-t$TICKS.log"
    # the log is deleted before the run and asserted after it, so a
    # stale log can never be read as this run's result.
    rm -f "$LOG"
    LIMIT=$((DUMP_FROM + 2 * CENSUS + 20))
    # The per-face drop trail is hundreds of KB per walk and is not read here;
    # stderr is filtered to the census lines as it streams. The exe writes
    # CRLF, stripped before the filter.
    #
    # One retry: a windowed run started while the previous one is still
    # releasing the display sometimes never brings its window up and exits
    # having emitted no census at all. That is a launch failure, not a
    # reading, so it is retried once after the display has had time to
    # settle. A leg that produces a census is never retried, so a real
    # divergence cannot be retried away.
    attempt=1
    while :; do
        rm -f "$LOG"
        env "${WSLENV_ARGS[@]}" "$EXE" \
            --vibration 0 --input-lock --no-config --mute 1 --no-card --disc "$DISC_ABS" \
            --renderer vk --skip-to-level "$LEVEL" --lod extended \
            --vsync 0 --render-fps 60 \
            --freecam-path "ticks=$TICKS;$WP_A;$WP_B" \
            --frame-limit "$LIMIT" 2>&1 | tr -d '\r' |
            grep -E --line-buffered '^\[cam f=|^\[fardbg-hdr\]|^\[fardbg-secend\]|^\[occl-inbound\]|^\[moby-occl |exit: reason|panic|assert|freecam:|playable' \
                > "$LOG"
        grep -q '^\[fardbg-hdr\]' "$LOG" && break
        if [[ "$attempt" -ge 2 ]]; then
            echo "occl-flicker: FAIL (t$TICKS produced no census in 2 attempts, see $LOG)"
            exit 1
        fi
        echo "occl-flicker: t$TICKS emitted no census, retrying once after the display settles"
        attempt=2
        sleep 30
    done

    python3 - "$LOG" "t$TICKS" "$DUMP_FROM" "$MIN_WALKS" "$SHIPPED_MOVE" \
             "$SHIPPED_TURN_DEG" "$WP_A" "$WP_B" "$ENGAGE" \
             > "$OUT/$GAME-t$TICKS.verdict" <<'CENSUS_PY' 
import math, re, sys

log, leg = sys.argv[1], sys.argv[2]
dump_from, floor = int(sys.argv[3]), int(sys.argv[4])
move_bound, turn_deg = float(sys.argv[5]), float(sys.argv[6])
wp_a = [float(x) for x in sys.argv[7].split(",")]
wp_b = [float(x) for x in sys.argv[8].split(",")]
# "admitted": the leg must admit a query and cull something, else it is
# VACUOUS. "queries": the leg reports SKIP-ENGAGEMENT when it admitted none,
# which is what the bound does to a camera that outruns it.
engage = sys.argv[9]
# The rotation bound as a matrix trace: trace(Rq . Rg^T) = 1 + 2 cos(theta).
turn_trace = 1.0 + 2.0 * math.cos(math.radians(turn_deg))

cam_re = re.compile(r"\[cam f=(\d+)\] pos=\((-?\d+), (-?\d+), (-?\d+)\)")
sec_re = re.compile(r"\[fardbg-secend\] s=(\d+) tris=(\d+)")
hdr_re = re.compile(r"\[fardbg-hdr\] frame=(\d+) walk=(\d+) sub=(\d) alpha=([-\d.]+) "
                    r"occl=(-?\d+) gridage=(-?\d+)")
lat_re = re.compile(r"latoff=(\d+) offadm=(\d+)")
inb_re = re.compile(r"\[occl-inbound\].* d=([\d.]+) tr=([-\d.]+)")
# The query-outcome census, printed once a second. vis and cull are the
# verdicts taken from the grid; moved is the refusal when the querying camera
# and the grid's disagree beyond the bound. Their sum is the number of queries
# that reached the depth verdict at all.
qry_re = re.compile(r"\[moby-occl .*? vis=(\d+) cull=(\d+) offscr=(\d+) moved=(\d+)")

walks, secs, inbound, poses = [], {}, [], []
q_vis = q_cull = q_moved = 0
for line in open(log, errors="replace"):
    m = cam_re.search(line)
    if m:
        if int(m.group(1)) >= dump_from:
            poses.append(tuple(int(m.group(i)) for i in (2, 3, 4)))
        secs, inbound = {}, []
        continue
    m = sec_re.search(line)
    if m:
        secs[int(m.group(1))] = int(m.group(2))
        continue
    m = inb_re.search(line)
    if m:
        inbound.append((float(m.group(1)), float(m.group(2))))
        continue
    m = qry_re.search(line)
    if m:
        q_vis += int(m.group(1))
        q_cull += int(m.group(2))
        q_moved += int(m.group(4))
        continue
    m = hdr_re.search(line)
    if m:
        la = lat_re.search(line)
        walks.append(dict(frame=int(m.group(1)), sub=int(m.group(3)),
                          occl=int(m.group(5)), gridage=int(m.group(6)),
                          latoff=int(la.group(1)) if la else -1,
                          offadm=int(la.group(2)) if la else -1,
                          secs=secs, inbound=inbound))
        secs, inbound = {}, []

# A tick is its canonical walk (sub 0) followed by that tick's re-extracts.
ticks = []
for w in walks:
    if w["sub"] == 0:
        ticks.append([w])
    elif ticks:
        ticks[-1].append(w)

def out_of_bound(group):
    """The queries this tick admitted whose camera disagreed with the grid's
    beyond the shipped bound. Their verdicts were read off a depth buffer
    rasterized under another camera, so the sectors they removed were never
    proved hidden."""
    return [q for w in group for q in w["inbound"]
            if q[0] > move_bound or q[1] < turn_trace]

cans = [t[0] for t in ticks]
holes = holes_wrong = entering = 0
flick_ticks = flick_events = wrong_flick = 0
wrong_detail = []
for i, t in enumerate(ticks):
    can = t[0]
    prev = cans[i - 1]["secs"] if i else {}
    oob = out_of_bound(t)
    # A sector an intermediate walk of this tick drops. One the previous tick
    # did not emit either is a sector entering the view, not a hole: the
    # canonical walk of this tick is the first to carry it.
    for w in t[1:]:
        for s in can["secs"]:
            if s in w["secs"]:
                continue
            if s in prev:
                holes += 1
                if oob:
                    holes_wrong += 1
            else:
                entering += 1
    # A sector the neighbouring canonical walks emit and this one does not.
    if 0 < i < len(cans) - 1:
        miss = sorted(s for s in cans[i - 1]["secs"]
                      if s in cans[i + 1]["secs"] and s not in can["secs"])
        if miss:
            flick_ticks += 1
            flick_events += len(miss)
            if oob:
                wrong_flick += len(miss)
                wrong_detail.append((can["frame"], miss, max(q[0] for q in oob)))

wrong = wrong_flick + holes_wrong
inter = sum(len(t) - 1 for t in ticks)
occl = [w["occl"] for w in walks]
occl_total = sum(occl)
occl_mean = occl_total / len(occl) if occl else 0.0
admitted = sum(len(w["inbound"]) for w in walks)
q_depth = q_vis + q_cull + q_moved
subs = [w for w in walks if w["sub"]]
lat_inter = sum(max(w["latoff"], 0) for w in subs)
lat_inter_max = max([w["latoff"] for w in subs], default=0)
lat_can = sum(max(w["latoff"], 0) for w in walks if not w["sub"])
off_adm = sum(max(w["offadm"], 0) for w in subs)
lat_walks = sum(1 for w in subs if w["latoff"] > 0)

# The camera held its waypoints: every sampled position lies on the flown
# segment, and the samples span most of it. The pose is reported in whole
# world units, so a position on the segment still reads a few units off it;
# the tolerance is four orders of magnitude below the segment length and
# rejects any other path.
seg = [wp_b[i] - wp_a[i] for i in range(3)]
seg_len = math.sqrt(sum(v * v for v in seg))
off_max, tmin, tmax = 0.0, 1.0, 0.0
for p in poses:
    d = [p[i] - wp_a[i] for i in range(3)]
    t = sum(d[i] * seg[i] for i in range(3)) / (seg_len * seg_len)
    proj = [wp_a[i] + t * seg[i] for i in range(3)]
    off_max = max(off_max, math.sqrt(sum((p[i] - proj[i]) ** 2 for i in range(3))))
    tmin, tmax = min(tmin, t), max(tmax, t)
span = tmax - tmin

reasons = []
if len(walks) < floor:
    reasons.append("walks=%d<%d" % (len(walks), floor))
if q_depth < 1:
    reasons.append("no query reached the depth verdict")
if not poses or off_max > 8.0 or span < 0.5:
    reasons.append("camera left its waypoints (off=%.1f span=%.2f)" % (off_max, span))
if engage == "admitted" and not reasons:
    if admitted < 1:
        reasons.append("no query was admitted")
    elif occl_total < 1:
        reasons.append("the cull removed nothing")

if reasons:
    verdict, rc = "VACUOUS", 1
elif wrong:
    verdict, rc = "FAIL", 1
elif engage == "queries" and admitted < 1:
    # The bound refused every query, so nothing was judged and nothing could
    # flicker. Reported with its numbers rather than as a PASS it did not earn.
    verdict, rc = "SKIP-ENGAGEMENT", 64
else:
    verdict, rc = "PASS", 0

print("[occl-flicker] leg=%s ticks=%d walks=%d inter=%d holes=%d entering=%d "
      "flickerTicks=%d flickerEvents=%d wrong=%d admitted=%d qCull=%d "
      "qMoved=%d occlTotal=%d occlMean=%.3f poseOff=%.1f poseSpan=%.2f "
      "latoff=%d latoffMax=%d latoffWalks=%d latoffCan=%d offadm=%d "
      "verdict=%s%s"
      % (leg, len(ticks), len(walks), inter, holes, entering, flick_ticks,
         flick_events, wrong, admitted, q_cull, q_moved, occl_total,
         occl_mean, off_max, span, lat_inter, lat_inter_max, lat_walks,
         lat_can, off_adm, verdict,
         (" reason=" + "; ".join(reasons)) if reasons else ""))
for f, miss, d in wrong_detail[:12]:
    print("[occl-flicker]   wrong frame=%d sectors=%s maxQueryDelta=%.1f" % (f, miss, d))
sys.exit(rc)
CENSUS_PY
    leg_rc=$?
    legs_n=$((legs_n + 1))
    case "$leg_rc" in
        0)  ;;
        64) legs_skipped=$((legs_skipped + 1));;   # engaged nothing, judged nothing
        *)  rc_all=1;;
    esac
    cat "$OUT/$GAME-t$TICKS.verdict"
    sleep 15
done
# Game-visibility bk2 legs. The per-walk dump opens at the window's first VBL
# and the run stops just past its last.
for LEG in $BK2_LEGS; do
    IFS=: read -r BK2 VLO VHI TSCALE <<<"$LEG"
    MOVIE="$PG_BK2_DIR/$BK2.bk2"
    [[ -f "$MOVIE" ]] || { echo "occl-flicker: SKIP bk2 leg ($MOVIE missing)"; exit 64; }
    MOVIE_ABS="$(cd "$(dirname "$MOVIE")" && pwd -P)/$(basename "$MOVIE")"
    LOG="$OUT/$GAME-$BK2.log"
    BENVS=("${ENVS[@]}")
    for k in "${!BENVS[@]}"; do
        [[ "${BENVS[$k]}" == OPENPETE_TERRAIN_FARDBG=* ]] && BENVS[$k]="OPENPETE_TERRAIN_FARDBG=$VLO"
    done
    wslenv_forward "${BENVS[@]}"
    rm -f "$LOG"
    env "${WSLENV_ARGS[@]}" "$EXE" \
        --vibration 0 --input-lock --no-config --mute 1 --no-card --disc "$DISC_ABS" \
        --renderer vk --lod extended --terrain-visibility game \
        --tick-scale "$TSCALE" --vsync 0 --render-fps 60 \
        --bk2 "$MOVIE_ABS" --frame-limit "$((VHI + 10))" 2>&1 | tr -d '\r' |
        grep -E --line-buffered '^\[fardbg-hdr\]|^\[occl-inbound\]|^\[moby-occl |exit: reason|panic|assert' \
            > "$LOG"
    [[ -s "$LOG" ]] || { echo "occl-flicker: FAIL ($BK2 produced no output, see $LOG)"; exit 1; }

    python3 - "$LOG" "$BK2" "$VLO" "$VHI" "$MIN_WALKS" \
             > "$OUT/$GAME-$BK2.verdict" <<'BK2_PY'
import re, sys

log, leg = sys.argv[1], sys.argv[2]
lo, hi, floor = (int(x) for x in sys.argv[3:6])
hdr_re = re.compile(r"\[fardbg-hdr\] frame=(\d+) walk=(\d+) sub=(\d)")
lat_re = re.compile(r"latoff=(\d+) offadm=(\d+)")
occ_re = re.compile(r"occl=(-?\d+)")
# [occl-inbound] lines precede the [fardbg-hdr] of the walk that issued them.
walks, pending = [], 0
for line in open(log, errors="replace"):
    if line.startswith("[occl-inbound]"):
        pending += 1
        continue
    m = hdr_re.search(line)
    if not m:
        continue
    la = lat_re.search(line)
    oc = occ_re.search(line)
    walks.append(dict(frame=int(m.group(1)), sub=int(m.group(3)),
                      latoff=int(la.group(1)) if la else -1,
                      offadm=int(la.group(2)) if la else -1,
                      occl=int(oc.group(1)) if oc else 0, admitted=pending))
    pending = 0
walks = [w for w in walks if lo <= w["frame"] <= hi]
subs = [w for w in walks if w["sub"]]
cans = [w for w in walks if not w["sub"]]
latoff = sum(max(w["latoff"], 0) for w in subs)
latoff_max = max((w["latoff"] for w in subs), default=0)
latoff_walks = sum(1 for w in subs if w["latoff"] > 0)
latoff_can = sum(max(w["latoff"], 0) for w in cans)
offadm = sum(max(w["offadm"], 0) for w in subs)
admitted = sum(w["admitted"] for w in walks)
occl_total = sum(max(w["occl"], 0) for w in walks)
reasons = []
if len(walks) < floor:
    reasons.append("walks=%d<%d" % (len(walks), floor))
if any(w["latoff"] < 0 for w in walks):
    reasons.append("no latoff column")
if offadm < 1:
    reasons.append("offadm=0")
if admitted < 1:
    reasons.append("no admitted query")
if reasons:
    verdict, rc = "VACUOUS", 1
elif latoff:
    verdict, rc = "FAIL", 1
else:
    verdict, rc = "PASS", 0
print("[occl-flicker] leg=%s vbl=%d-%d walks=%d inter=%d canon=%d latoff=%d "
      "latoffMax=%d latoffWalks=%d latoffCan=%d offadm=%d admitted=%d "
      "occlTotal=%d verdict=%s%s"
      % (leg, lo, hi, len(walks), len(subs), len(cans), latoff, latoff_max,
         latoff_walks, latoff_can, offadm, admitted, occl_total, verdict,
         (" reason=" + "; ".join(reasons)) if reasons else ""))
sys.exit(rc)
BK2_PY
    [[ $? -eq 0 ]] || rc_all=1
    legs_n=$((legs_n + 1))
    cat "$OUT/$GAME-$BK2.verdict"
    sleep 15
done

# A leg that admitted no query judged no verdict, so it neither passes nor
# fails the check; a check whose every leg did that reports SKIP.
[[ "$rc_all" == 0 && "$legs_skipped" == "$legs_n" ]] && exit 64
exit $rc_all
