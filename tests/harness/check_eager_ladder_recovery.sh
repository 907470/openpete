#!/usr/bin/env bash
# check_eager_ladder_recovery.sh - the eager decimation level returns to the
# level the span budget affords within one headroom run after a stall.
#
# Class: the eager planner divides each tick's span into display slots and
# produces every level-th one, the level chosen from how many scenes the
# span's budget affords. A level load runs one VBL far past its period. A
# budget priced from the plan instant then reads the lateness that VBL left
# as capacity (budget 0, one scene afforded), the level jumps to the slot
# count, and a ladder that falls one level per headroom run (8 spans) shows
# 480/level presents per second for seconds after every level entry. The row
# fails while the level is held above what the plan's own budget needs for
# longer than one headroom run plus the stalled span.
#
# Windowed only: eager production needs a window, so neither the
# presentation golden nor the oracle (both headless) reaches the planner.
#
# Row: spyro-3 portal-117-midday-gardens-icy-peak.bk2 (the title loads slot 1
# of a scratch copy of games/spyro-3/cards/117-percent.mcr; the level entry
# at VBL ~1864 runs one update VBL ~47 ms; two later loads stall again).
#
# Instrument: OPENPETE_EAGER_DBG=1 plan lines,
#   [eager f=<vbl>] plan: slots=<n> budget=<ms>ms tail=<ms>ms afford=<n>
#                   level=<n> cut=<0|1> lag=<ms>ms vbl=<ms>ms [late=<ms>ms]
# written on a cut span, a level change, or (where the build prints late=) a
# tick that started late. A stall plan is one with budget=0.00ms (the
# collapse) or late= at least half a tick. For each stall plan at VBL s the
# base level is the level in force before s; recovery is the first later
# plan line whose level is at most the base or at most the level its own
# afford covers with the planner's quarter margin (ceil(slots / floor(afford
# * 4 / 5))); recovery spans = (that VBL - s) / 2, 0 when no line within 8
# VBLs of s raises the level above the base. Lines are written on every
# level change, so the level between two lines is the earlier line's.
#
# Pass: every stall plan recovers within MAX_SPANS (9: the stalled span's own
# cut plus one 8-span headroom run). Engagement: at least one stall plan after
# the first WARMUP VBLs of eager production, else VACUOUS. The steady level
# change rate (changes per minute outside the 600 VBLs after each stall) is
# reported, not gated: on a software rasteriser it measures host load.
#
# Every launch passes --input-lock --vibration 0 --background 1, so physical
# input cannot reach the run and the window takes no focus. Without a display
# the run is wrapped in xvfb-run when it is installed.
#
# Verdict line:
#   [ladder-recovery] game=spyro-3 stalls=<n> worst_spans=<n> steady_changes_per_min=<x>
#                     eager=<0|1> verdict=PASS|FAIL|VACUOUS log=<path>
#
# --analyze=LOG skips the run and grades an existing log of the same row
# (one the Windows exe wrote, say).
#
# Usage: tests/harness/check_eager_ladder_recovery.sh [--build-dir=DIR] [--out=DIR]
#            [--render-fps=N] [--analyze=LOG]
# Exit 0 PASS, 1 FAIL or VACUOUS, 64 SKIP (no display, binary, disc or card).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

# Unattended launcher: every run this script starts is silenced.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

OUT=""
RENDER_FPS=480
ANALYZE=""
for a in "$@"; do
    case "$a" in
        --build-dir=*)  PG_BUILD_DIR="${a#--build-dir=}";;
        --out=*)        OUT="${a#--out=}";;
        --render-fps=*) RENDER_FPS="${a#--render-fps=}";;
        --analyze=*)    ANALYZE="${a#--analyze=}";;
    esac
done

GAME=spyro-3
pg_resolve "$GAME"
BK2=games/spyro-3/bk2/portal-117-midday-gardens-icy-peak.bk2
CARD_SRC=games/spyro-3/cards/117-percent.mcr
FRAME_LIMIT=4700
MAX_SPANS=9
WARMUP=120
ROW_RUNTIME=900
ROW_TIMEOUT=960

if [[ -n "$ANALYZE" ]]; then
    [[ -s "$ANALYZE" ]] || { echo "ladder-recovery: FAIL (no log at $ANALYZE)"; exit 1; }
    LOG="$ANALYZE"
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
LOG="$OUT/$GAME.log"
CARD="$OUT/$GAME-card1.mcr"
rm -f "$LOG" "$CARD"
cp "$CARD_SRC" "$CARD"

op_own_run env OPENPETE_VIBRATION=0 OPENPETE_EAGER_DBG=1 OPENPETE_TERRAIN_CACHE_FORCE=1 \
    "${WRAP[@]}" timeout -k 10 "$ROW_TIMEOUT" "$PG_BIN" \
    --no-config --disc "$PG_DISC" --card1 "$CARD" --no-card2 --mute 1 \
    --vibration 0 --input-lock --background 1 \
    --renderer vk --render-fps "$RENDER_FPS" --vsync 0 \
    --bk2 "$BK2" --frame-limit "$FRAME_LIMIT" --max-runtime-seconds "$ROW_RUNTIME" \
    </dev/null >"$LOG" 2>&1
rc=$?
[[ -s "$LOG" ]] || { echo "ladder-recovery: FAIL (no log, rc=$rc)"; exit 1; }
fi

read -r stalls worst rate eager <<<"$(awk -v warm="$WARMUP" '
    function num(field,   v) { v = field; sub(/^[a-z]+=/, "", v); sub(/ms$/, "", v); return v + 0 }
    /eager: engaged/ { eager = 1 }
    /^\[eager f=[0-9]+\] plan: / {
        f = $2; sub(/^f=/, "", f); sub(/\]$/, "", f); f += 0
        late = -1
        for (i = 3; i <= NF; i++) {
            if ($i ~ /^slots=/)  sl = num($i)
            if ($i ~ /^budget=/) bu = num($i)
            if ($i ~ /^afford=/) af = num($i)
            if ($i ~ /^level=/)  lv = num($i)
            if ($i ~ /^late=/)   late = num($i)
        }
        n++; F[n] = f; S[n] = sl; A[n] = af; L[n] = lv
        if (n == 1) first = f
        if (f >= first + warm && (bu == 0 || late >= 16.7)) { ns++; ST[ns] = n }
        last = f
    }
    function cover(k,   m) { m = int(A[k] * 4 / 5); return m >= 1 ? int((S[k] + m - 1) / m) : L[k] }
    END {
        worst = 0
        for (j = 1; j <= ns; j++) {
            k = ST[j]; s = F[k]
            base = 1; for (q = 1; q < k; q++) base = L[q]
            if (k == 1) base = 1
            # The stall raises the level at its own plan (a collapsed budget)
            # or at the next tick (the late span cut short): a raise written
            # within 8 VBLs of the stall plan is counted to it.
            up = 0
            for (q = k; q <= n && F[q] <= s + 8; q++) if (L[q] > base) { up = q; break }
            rec = 0
            if (up) {
                rec = -1
                for (q = up + 1; rec < 0 && q <= n; q++)
                    if (L[q] <= base || L[q] <= cover(q)) rec = int((F[q] - s) / 2)
                if (rec < 0) rec = int((last - s) / 2) + 1
            }
            if (rec > worst) worst = rec
            printf "  stall plan VBL %d: level %d (base %d), recovered in %d span(s)\n", s, L[k], base, rec > "/dev/stderr"
        }
        ch = 0; vbls = 0
        for (q = 2; q <= n; q++) {
            ex = 0
            for (j = 1; j <= ns; j++) if (F[q] >= F[ST[j]] && F[q] - F[ST[j]] <= 600) ex = 1
            if (!ex && F[q] >= first + warm && L[q] != L[q - 1]) ch++
        }
        span = last - first - warm; for (j = 1; j <= ns; j++) span -= 600
        mins = span > 0 ? span / (2250000 / 37609) / 60 : 0
        rate = (mins > 0) ? ch / mins : 0
        printf "%d %d %.1f %d\n", ns, worst, rate, eager
    }
' "$LOG")"

verdict=PASS
if (( eager == 0 || stalls == 0 )); then
    verdict=VACUOUS
elif (( worst > MAX_SPANS )); then
    verdict=FAIL
fi
echo "[ladder-recovery] game=$GAME render_fps=$RENDER_FPS stalls=$stalls worst_spans=$worst max_spans=$MAX_SPANS steady_changes_per_min=$rate eager=$eager verdict=$verdict log=$LOG"
[[ "$verdict" == PASS ]]
