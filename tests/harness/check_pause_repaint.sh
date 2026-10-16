#!/usr/bin/env bash
# check_pause_repaint.sh - the paused repaint draws the retained terrain.
#
# Class: while the sim is paused (the pause key, or the dev overlay's Pause
# sim row) each host iteration presents the held scene without a re-extract.
# Under eager production the held scene sits in the scene-ring slot the last
# publish moved it to, and its retained terrain is kept under that scene's
# id. A present whose selection returns the scene's triangles but another
# stamp finds no retained-terrain state and draws none of the cache-owned
# sectors: sky, far geometry and mobys show, terrain does not.
#
# Windowed only. The paused repaint returns under --headless and eager
# production needs a window, so neither the presentation golden nor any
# headless capture reaches the path.
#
# Driver: OPENPETE_PAUSE_AT_VBL=N,M,K (a harness env var, not a registered
# knob) pauses at VBL N through the pause request the dev overlay raises,
# requests one step after every M paused repaints, K steps in all, then
# resumes after M more. Each transition logs a frame-step line:
#   frame-step: paused at openpete_frame=<n> canon_tick=<n>
#   frame-step: step requested at openpete_frame=<n> canon_tick=<n>
#   frame-step: stepped to openpete_frame=<n> canon_tick=<n> vbls=<n>
#   frame-step: resumed at openpete_frame=<n> canon_tick=<n>
#
# Instrument: OPENPETE_PRESENT_TRACE=1, one line per present:
#   [present] #<n> kind=<k> shows=<s> frame=<n> stamp=<hex> n=<n> tc=<n> ...
# where tc is the retained-terrain (terrain cache) draw count of the present.
# OPENPETE_TERRAIN_CACHE_FORCE=1 brings the cache up on a software rasteriser.
#
# Metric: a paused segment runs from a "paused at" or "stepped to" line to the
# next "step requested" or "resumed" line; its paused presents are its
# kind=inline lines. The paused repaint holds the newest scene the sim
# produced, which the present thread may not have shown yet, so the
# reference is the tc of the present that showed that scene (the present
# outside any segment carrying the paused presents' stamp: before the pause,
# the stepped tick's own present, or the first present after the resume).
# A scene superseded before any present showed it has no such present
# (counted as unshown); its reference is its first paused present's tc, so
# the segment must hold one tc. Pass, per row: every paused present's tc
# equals its segment's reference tc, and every reference tc is above zero. Engagement: at least
# MIN_PAUSED paused presents, and eager production engaged ("eager: engaged")
# before the pause; else VACUOUS.
#
# Rows (one per game, level 10 of each, paused after the level has run):
#   spyro-1 artisans        pause at VBL 2600
#   spyro-2 summer_forest   pause at VBL 2673
#   spyro-3 sunrise_spring  pause at VBL 2773
#
# Every launch passes --input-lock --vibration 0 --background 1: physical input
# cannot reach the run and the window takes no focus.
#
# Verdict line per row:
#   [pause-repaint] game=<g> paused=<n> segments=<n> ref_tc=<list>
#                   unshown=<n> bad=<n> eager=<0|1> resumed=<0|1>
#                   verdict=PASS|FAIL|VACUOUS
#
# Usage: tests/harness/check_pause_repaint.sh [--game SLUG] [--build-dir=DIR]
#                                             [--out=DIR]
# Exit 0 PASS, 1 FAIL or VACUOUS, 64 SKIP (no display, binary or disc).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

# Unattended launcher: every run this script starts is silenced.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

OUT=""
for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
        --out=*)       OUT="${a#--out=}";;
    esac
done

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"

case "$GAME" in
  spyro-1) LEVEL=artisans;       PAUSE_VBL=2600;;
  spyro-2) LEVEL=summer_forest;  PAUSE_VBL=2673;;
  spyro-3) LEVEL=sunrise_spring; PAUSE_VBL=2773;;
  *) echo "pause-repaint: SKIP ($GAME has no row)"; exit 64;;
esac
EVERY=30         # paused repaints before the step, and again before the resume
STEPS=1
MIN_PAUSED=20
ROW_RUNTIME=900
ROW_TIMEOUT=960

[[ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]] ||
    { echo "pause-repaint: SKIP (no display)"; exit 64; }
[[ -x "$PG_BIN" ]] || { echo "pause-repaint: SKIP (no binary at $PG_BIN)"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "pause-repaint: SKIP (no disc at $PG_DISC)"; exit 64; }

[[ -n "$OUT" ]] || OUT="${PG_BUILD_DIR:-build/linux-release}/pause-repaint"
mkdir -p "$OUT"
LOG="$OUT/$GAME.log"
rm -f "$LOG"

op_own_run env OPENPETE_VIBRATION=0 OPENPETE_PRESENT_TRACE=1 OPENPETE_TERRAIN_CACHE_FORCE=1 \
    OPENPETE_PAUSE_AT_VBL="$PAUSE_VBL,$EVERY,$STEPS" \
    timeout -k 10 "$ROW_TIMEOUT" "$PG_BIN" \
    --no-config --disc "$PG_DISC" --disc-cache 0 --no-card --mute 1 \
    --vibration 0 --input-lock --background 1 \
    --renderer vk --window 640x448 \
    "${PG_RUN_ARGS[@]}" --skip-to-level "$LEVEL" \
    --frame-limit $((PAUSE_VBL + 120)) --max-runtime-seconds "$ROW_RUNTIME" \
    </dev/null >"$LOG" 2>&1
rc=$?
[[ -s "$LOG" ]] || { echo "pause-repaint: FAIL (no log, rc=$rc)"; exit 1; }

# One pass over the log in order: frame-step lines open and close segments,
# present lines are classified against the open segment. Every present
# outside a segment records its tc under its stamp; the reference of a
# segment is resolved at the end from the stamp its paused presents carry.
read -r paused segments refs bad eager resumed unshown <<<"$(awk '
    /eager: engaged/ && !seen_pause { eager = 1 }
    /frame-step: (paused at|stepped to)/ { seen_pause = 1; open = 1; first = 1; next }
    /frame-step: step requested/ { open = 0; next }
    /frame-step: resumed/ { open = 0; resumed = 1; next }
    /^\[present\] / {
        tc = ""; kind = ""; st = "";
        for (i = 1; i <= NF; i++) {
            if ($i ~ /^tc=/)    tc = substr($i, 4);
            if ($i ~ /^kind=/)  kind = substr($i, 6);
            if ($i ~ /^stamp=/) st = substr($i, 7);
        }
        if (open && kind == "inline") {
            if (first) { first = 0; segs++; held[segs] = st; own[segs] = tc }
            paused++; ptc[paused] = tc; pseg[paused] = segs;
        } else {
            if (!(st in shown)) shown[st] = tc;
            last_tc = tc;
        }
    }
    END {
        for (k = 1; k <= segs; k++) {
            if (held[k] in shown) ref[k] = shown[held[k]];
            else { ref[k] = own[k]; unshown++ }
            refs = refs (refs == "" ? "" : ",") ref[k];
            if (ref[k] + 0 <= 0) bad++;
        }
        for (j = 1; j <= paused; j++) if (ptc[j] != ref[pseg[j]]) bad++;
        printf "%d %d %s %d %d %d %d\n", paused, segs, (refs == "" ? "-" : refs), bad, eager, resumed, unshown
    }
' "$LOG")"

verdict=PASS
if (( paused < MIN_PAUSED || eager == 0 || resumed == 0 )); then
    verdict=VACUOUS
elif (( bad > 0 )); then
    verdict=FAIL
fi
echo "[pause-repaint] game=$GAME paused=$paused segments=$segments ref_tc=$refs unshown=$unshown bad=$bad eager=$eager resumed=$resumed verdict=$verdict log=$LOG"
[[ "$verdict" == PASS ]]
