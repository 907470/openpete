#!/usr/bin/env bash
# check_levelwarp_all.sh - the level warp into every level, with Cross taps.
#
# One headless session per (game, target level). The target list is the
# game's level table, read from the binary's --skip-to-level listing, so a
# level added to the table joins the check. Each session boots into the
# game's start level (--skip-to-level; the alternate start when the target
# is the start level), replays a generated movie that holds Cross for 2 VBLs
# of every 40, which dismisses the speeches some levels open on arrival, and
# fires the level warp through OPENPETE_LEVELWARP_TEST=<trigger>:<target>
# (the trigger counts openpete_frame, the state lines' frame=).
#
# Instrument: the warp's log lines, and OPENPETE_STATE_WATCH over the game
# state word (gs, 0 = gameplay), the level number word (lvl: Spyro 1
# g_LevelId, Spyro 2 g_NextLevelNum, Spyro 3 currentLevel) and the
# attract-demo word (demo).
#
# Per row:
#   engaged  the log has a line of the warp's drain that starts a warp
#            ("leaving level", "exiting level", "queued", "ending the
#            attract demo"). A row whose request found no route ("ignored
#            (no route") or never drained is VACUOUS.
#   arrived  the log has "arrived in level N" (N the target), no "landed in"
#            or "aborted" line, and a state line after the trigger reads
#            lvl = N, gs = 0 and demo = 0 (the game itself is in play in the
#            target level, not only the warp's log).
#   held     the last state line still reads lvl = N: nothing after the
#            arrival took the session out of the level.
# A row passes when engaged, arrived and held. A drop is an engaged row with
# no arrival.
#
#   tests/harness/check_levelwarp_all.sh [--game=G] [--build-dir=DIR] [--filter=LEVEL]
#
# Without --game all three games run (101 sessions, about 17 minutes at 2
# jobs on the reference machine). Exit 0 PASS, 1 FAIL (a failing or
# VACUOUS row, or zero engaged rows), 64 SKIP (no game's binary and disc
# present). At most LEVELWARP_JOBS (default 2) game processes run at once.
set -uo pipefail
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh
export OPENPETE_VIBRATION=0

BUILD="build/linux-release"; FILTER=""; ONLY=""
for a in "$@"; do
    case "$a" in
        --build-dir=*) BUILD="${a#--build-dir=}";;
        --filter=*)    FILTER="${a#--filter=}";;
        --game=*)      ONLY="${a#--game=}";;
        *) echo "check_levelwarp_all: unknown argument $a" >&2; exit 64;;
    esac
done
OUT="${LEVELWARP_OUT:-$BUILD/levelwarp-all}"
JOBS="${LEVELWARP_JOBS:-2}"

# Per game: the state watch, the movie's LoadDelay header (replay requires
# it to equal the run's level-load delay), the start level and its
# alternate, the trigger frame (in gameplay in the start level) and the
# session's VBL limit.
declare -A WATCH=(
    [spyro-1]="gs=800757D8,lvl=8007596C,demo=80075714"
    [spyro-2]="gs=800681C8,lvl=80066F90,demo=80067024"
    [spyro-3]="gs=8006E344,lvl=8006C5BC,demo=8006C658"
)
declare -A DELAY=([spyro-1]=0 [spyro-2]=0 [spyro-3]=66)
declare -A START=([spyro-1]=artisans [spyro-2]=summer_forest [spyro-3]=sunrise_spring)
declare -A ALT=([spyro-1]=stone_hill [spyro-2]=glimmer [spyro-3]=sunny_villa)
declare -A TRIGGER=([spyro-1]=2000 [spyro-2]=2000 [spyro-3]=1700)
declare -A LIMIT=([spyro-1]=4500 [spyro-2]=5000 [spyro-3]=4500)

mkdir -p "$OUT"
rm -rf "${OUT:?}/rows" "${OUT:?}/movies"
mkdir -p "$OUT/rows" "$OUT/movies"

# movie PATH LOADDELAY LIMIT: Cross held for VBLs 0-1 of every 40, LIMIT rows.
movie() {
    python3 - "$@" <<'EOF'
import sys, zipfile
path, delay, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
key = ("LogKey:#Disc Select|Open|Close|Reset|#P1 LStick X|P1 LStick Y|P1 RStick X|"
       "P1 RStick Y|P1 Up|P1 Down|P1 Left|P1 Right|P1 Select|P1 Start|P1 Square|"
       "P1 Triangle|P1 Circle|P1 Cross|P1 L1|P1 R1|P1 L2|P1 R2|P1 L3|P1 R3|P1 MODE|")
idle, cross = "." * 17, "." * 9 + "X" + "." * 7
rows = [f"|    1,...|  128,  128,  128,  128,{cross if i % 40 < 2 else idle}|" for i in range(n)]
header = ("MovieVersion BizHawk v2.0\nAuthor openpete\nemuVersion openpete-bk2-record\n"
          f"Platform PSX\nCore Octoshock\nrerecordCount 0\nLoadDelay {delay}\n"
          "OpenpeteFeedKey vbl\n")
with zipfile.ZipFile(path, "w") as z:
    z.writestr("Header.txt", header)
    z.writestr("Input Log.txt", "[Input]\n" + key + "\n" + "\n".join(rows) + "\n")
EOF
}

# run GAME BIN DISC START TARGET: one session; log and exit code under
# rows/GAME-TARGET. RUNARGS holds the game's PG_RUN_ARGS, set by the caller.
run() {
    local game="$1" bin="$2" disc="$3" start="$4" target="$5" d="$OUT/rows/$1-$5"
    mkdir -p "$d"
    env OPENPETE_LEVELWARP_TEST="${TRIGGER[$game]}:$target" \
        OPENPETE_STATE_WATCH="${WATCH[$game]}" \
        timeout -k 10 900 "$bin" --headless --no-config --disc "$disc" \
        "${RUNARGS[@]}" --no-card --mute 1 --input-lock --vibration 0 \
        --bk2 "$OUT/movies/$game.bk2" --skip-to-level "$start" \
        --frame-limit "${LIMIT[$game]}" >"$d/log.txt" 2>&1
    echo $? >"$d/rc"
}

SPECS=(); ran_games=0; running=0
for game in spyro-1 spyro-2 spyro-3; do
    [[ -n "$ONLY" && "$game" != "$ONLY" ]] && continue
    PG_BUILD_DIR="$BUILD" pg_resolve "$game" >/dev/null 2>&1 || continue
    if [[ ! -x "$PG_BIN" || ! -f "$PG_DISC" ]]; then
        echo "check_levelwarp_all: $game skipped ($PG_BIN or $PG_DISC missing)"; continue
    fi
    ran_games=$((ran_games + 1))
    RUNARGS=("${PG_RUN_ARGS[@]}")
    # The level table: the listing an unknown --skip-to-level name prints,
    # "  <name>  (<number>)" per row.
    mapfile -t TABLE < <("$PG_BIN" --headless --no-config --disc "$PG_DISC" \
        "${RUNARGS[@]}" --skip-to-level levelwarp_all_list --frame-limit 1 2>&1 |
        sed -nE 's/^ +([a-z0-9_]+) +\(([0-9]+)\)$/\1:\2/p')
    if (( ${#TABLE[@]} == 0 )); then
        echo "check_levelwarp_all: FAIL ($game printed no level table)"; exit 1
    fi
    movie "$OUT/movies/$game.bk2" "${DELAY[$game]}" "${LIMIT[$game]}"
    [[ -f "$OUT/movies/$game.bk2" ]] || { echo "check_levelwarp_all: FAIL (movie not written)"; exit 1; }
    for entry in "${TABLE[@]}"; do
        target="${entry%%:*}"; num="${entry##*:}"
        [[ -n "$FILTER" && "$target" != "$FILTER" ]] && continue
        start="${START[$game]}"; [[ "$target" == "$start" ]] && start="${ALT[$game]}"
        SPECS+=("$game-$target:$num:${TRIGGER[$game]}")
        run "$game" "$PG_BIN" "$PG_DISC" "$start" "$target" &
        running=$((running + 1))
        if (( running >= JOBS )); then wait -n; running=$((running - 1)); fi
    done
done
wait
if (( ran_games == 0 )); then
    echo "check_levelwarp_all: SKIP (no game binary and disc present)"; exit 64
fi
if (( ${#SPECS[@]} == 0 )); then
    echo "check_levelwarp_all: FAIL (no rows${FILTER:+ matching $FILTER})"; exit 1
fi

# classify: one line per row, "<VERDICT> <game-level>: <detail>".
CLASSIFY="$(cat <<'EOF'
import os, re, sys
out, specs = sys.argv[1], sys.argv[2:]
START = ("leaving level", "exiting level", "queued", "ending the attract demo")
for spec in specs:
    row, num, trigger = spec.split(":")
    num, trigger = int(num), int(trigger)
    d = os.path.join(out, "rows", row)
    try:
        log = open(os.path.join(d, "log.txt"), errors="replace").read()
        rc = open(os.path.join(d, "rc")).read().strip()
    except OSError:
        print(f"FAIL {row}: no output"); continue
    warp = [l for l in log.splitlines() if "levelwarp: " in l]
    states = []
    for line in log.splitlines():
        if not line.startswith("[state] "): continue
        f = dict(kv.split("=", 1) for kv in line.split()[1:])
        states.append({k: int(v) for k, v in f.items()})
    last = states[-1] if states else {}
    tail = f"last state vbl={last.get('vbl')} gs={last.get('gs')} lvl={last.get('lvl')} demo={last.get('demo')}"
    if not any(k in l for l in warp for k in START):
        why = "ignored, no route" if any("ignored (no route" in l for l in warp) else "never drained"
        print(f"VACUOUS {row}: {why}; {tail}"); continue
    if rc != "0":
        print(f"FAIL {row}: exit {rc}; {tail}"); continue
    bad = [l.split("levelwarp: ", 1)[1] for l in warp if "landed in" in l or "aborted" in l]
    m = re.search(rf"arrived in level {num} at vbl=(\d+)", log)
    play = [s for s in states if s["frame"] >= trigger and s.get("lvl") == num
            and s.get("gs") == 0 and s.get("demo", 0) == 0]
    if bad or not m or not play:
        why = bad or ([] if m else ["no arrival line"]) + ([] if play else ["no gameplay state in the level"])
        print(f"DROP {row}: {'; '.join(why)}; {tail}"); continue
    if last.get("lvl") != num:
        print(f"FAIL {row}: arrived vbl {m.group(1)}, then left the level; {tail}"); continue
    print(f"PASS {row}: arrived vbl {m.group(1)}, in play vbl {play[0]['vbl']}; {tail}")
EOF
)"
RESULT="$(python3 -c "$CLASSIFY" "$OUT" "${SPECS[@]}")"
echo "$RESULT"
attempted="$(grep -vc '^VACUOUS' <<<"$RESULT")"
arrived="$(grep -c '^PASS' <<<"$RESULT")"
failed="$(grep -vc '^PASS' <<<"$RESULT")"
for game in spyro-1 spyro-2 spyro-3; do
    n="$(grep -c " $game-" <<<"$RESULT")"
    (( n == 0 )) && continue
    echo "check_levelwarp_all: $game warps attempted $(grep -v '^VACUOUS' <<<"$RESULT" | grep -c " $game-"), arrived $(grep '^PASS' <<<"$RESULT" | grep -c " $game-") of $n"
done
echo "check_levelwarp_all: rows ${#SPECS[@]}, warps attempted $attempted, arrived $arrived, failed $failed"
if (( attempted == 0 )); then
    echo "check_levelwarp_all: FAIL (zero engaged rows: no warp started)"; exit 1
fi
(( failed == 0 )) || { echo "check_levelwarp_all: FAIL"; exit 1; }
echo "check_levelwarp_all: PASS"
