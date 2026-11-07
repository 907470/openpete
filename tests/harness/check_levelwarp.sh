#!/usr/bin/env bash
# check_levelwarp.sh - the level warp from guest states other than gameplay.
#
# Runs the rows of games/<game>/corpus/levelwarp.manifest headless, one
# session per row. Each row boots with its own arguments (--skip-to-level
# or --skip-to-cutscene), replays a generated movie that holds Start for 6
# VBLs at each VBL of the row's presses column (the first opens the pause
# menu, the second closes it), and fires the level warp through
# OPENPETE_LEVELWARP_TEST=<trigger>:<target>, whose trigger counts
# openpete_frame (the state lines' frame=), not VBLs.
#
# Instrument: the warp's log lines, and OPENPETE_STATE_WATCH over the game
# state word (gs), the attract-demo mode word (demo; on Spyro 2 0x80067024,
# which the in-level route stores) and, on Spyro 2, the loader word (ld).
#
# Per row:
#   engaged  the log has a line of the warp's drain (the request reached a
#            route decision: "ignored (no route", "leaving level", "exiting
#            level", "queued", "ending the attract demo").
#   at       the game state before the trigger frame (the last state line
#            ahead of it; the drain precedes the line of its own tick)
#            equals the row's at column
#            (4 the Spyro 2 and 3 pause menu, 2 the Spyro 1 pause menu,
#            7 a Spyro 2 level-exit loading screen); otherwise the row is
#            VACUOUS.
#   refuse   the log has "ignored (no route from this state)" and no
#            "leaving level" line, the demo word reads 0 on every state line
#            from the trigger on, and the last state line reads gs 0 (the
#            second Start closed the pause menu, or the load ended into
#            gameplay).
#   arrive   the log has "arrived in level N", N the trigger's target.
#
#   tests/harness/check_levelwarp.sh [--game=G] [--build-dir=DIR] [--filter=NAME]
#
# Without --game all three games run (about 30 s at 2 jobs). Exit 0
# PASS, 1 FAIL (a failing or VACUOUS row, or zero engaged rows), 64 SKIP (no
# game's binary and disc present). At most LEVELWARP_JOBS (default 2) game
# processes run at once.
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
        *) echo "check_levelwarp: unknown argument $a" >&2; exit 64;;
    esac
done
OUT="${LEVELWARP_OUT:-$BUILD/levelwarp}"
JOBS="${LEVELWARP_JOBS:-2}"

# Per game: the state watch and the movie's LoadDelay header, which replay
# requires to equal the run's level-load delay.
declare -A WATCH=(
    [spyro-1]="gs=800757D8,demo=80075714"
    [spyro-2]="gs=800681C8,demo=80067024,ld=80066EEC"
    [spyro-3]="gs=8006E344,demo=8006C658"
)
declare -A DELAY=([spyro-1]=0 [spyro-2]=0 [spyro-3]=66)

mkdir -p "$OUT"
rm -rf "${OUT:?}/rows"
mkdir -p "$OUT/rows"

# movie PATH LOADDELAY LIMIT PRESSES: Start held 6 VBLs from each VBL of the
# comma-separated PRESSES ("-": none), LIMIT rows.
movie() {
    python3 - "$@" <<'EOF'
import sys, zipfile
path, delay, n, presses = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
starts = [] if presses == "-" else [int(v) for v in presses.split(",")]
key = ("LogKey:#Disc Select|Open|Close|Reset|#P1 LStick X|P1 LStick Y|P1 RStick X|"
       "P1 RStick Y|P1 Up|P1 Down|P1 Left|P1 Right|P1 Select|P1 Start|P1 Square|"
       "P1 Triangle|P1 Circle|P1 Cross|P1 L1|P1 R1|P1 L2|P1 R2|P1 L3|P1 R3|P1 MODE|")
idle, start = "." * 17, "." * 5 + "X" + "." * 11
held = lambda i: any(s <= i < s + 6 for s in starts)
rows = [f"|    1,...|  128,  128,  128,  128,{start if held(i) else idle}|" for i in range(n)]
header = ("MovieVersion BizHawk v2.0\nAuthor openpete\nemuVersion openpete-bk2-record\n"
          f"Platform PSX\nCore Octoshock\nrerecordCount 0\nLoadDelay {delay}\n"
          "OpenpeteFeedKey vbl\n")
with zipfile.ZipFile(path, "w") as z:
    z.writestr("Header.txt", header)
    z.writestr("Input Log.txt", "[Input]\n" + key + "\n" + "\n".join(rows) + "\n")
EOF
}

# run GAME BIN DISC NAME BOOT TARGET PRESSES TRIGGER LIMIT: one session; log,
# exit code and movie under rows/GAME-NAME. RUNARGS holds the game's
# PG_RUN_ARGS, set by the caller.
run() {
    local game="$1" bin="$2" disc="$3" name="$4" boot="$5" target="$6"
    local presses="$7" trigger="$8" limit="$9" d="$OUT/rows/$1-$4"
    local -a bootv
    read -r -a bootv <<<"$boot"
    mkdir -p "$d"
    movie "$d/presses.bk2" "${DELAY[$game]}" "$limit" "$presses"
    if [[ ! -f "$d/presses.bk2" ]]; then echo 1 >"$d/rc"; return; fi
    env OPENPETE_LEVELWARP_TEST="$trigger:$target" OPENPETE_STATE_WATCH="${WATCH[$game]}" \
        timeout -k 10 600 "$bin" --headless --no-config --disc "$disc" \
        "${RUNARGS[@]}" --no-card --mute 1 --input-lock --vibration 0 \
        --bk2 "$d/presses.bk2" "${bootv[@]}" \
        --frame-limit "$limit" >"$d/log.txt" 2>&1
    echo $? >"$d/rc"
}

SPECS=(); ran_games=0; running=0
for game in spyro-1 spyro-2 spyro-3; do
    [[ -n "$ONLY" && "$game" != "$ONLY" ]] && continue
    PG_BUILD_DIR="$BUILD" pg_resolve "$game" >/dev/null 2>&1 || continue
    if [[ ! -x "$PG_BIN" || ! -f "$PG_DISC" ]]; then
        echo "check_levelwarp: $game skipped ($PG_BIN or $PG_DISC missing)"; continue
    fi
    MANIFEST="$(pg_corpus_manifest "$game" levelwarp.manifest)"
    [[ -f "$MANIFEST" ]] || { echo "check_levelwarp: FAIL ($MANIFEST missing)"; exit 1; }
    ran_games=$((ran_games + 1))
    RUNARGS=("${PG_RUN_ARGS[@]}")
    while IFS='|' read -r name boot target presses trigger at limit expect; do
        name="${name//[[:space:]]/}"
        [[ -z "$name" || "$name" == \#* ]] && continue
        [[ -n "$FILTER" && "$name" != "$FILTER" ]] && continue
        boot="$(echo "$boot" | xargs)"
        target="${target//[[:space:]]/}"; presses="${presses//[[:space:]]/}"
        trigger="${trigger//[[:space:]]/}"; at="${at//[[:space:]]/}"
        limit="${limit//[[:space:]]/}"; expect="${expect//[[:space:]]/}"
        SPECS+=("$game-$name:$trigger:$at:$expect")
        run "$game" "$PG_BIN" "$PG_DISC" "$name" "$boot" "$target" "$presses" \
            "$trigger" "$limit" &
        running=$((running + 1))
        if (( running >= JOBS )); then wait -n; running=$((running - 1)); fi
    done < "$MANIFEST"
done
wait
if (( ran_games == 0 )); then
    echo "check_levelwarp: SKIP (no game binary and disc present)"; exit 64
fi
if (( ${#SPECS[@]} == 0 )); then
    echo "check_levelwarp: FAIL (no rows${FILTER:+ matching $FILTER})"; exit 1
fi

# classify: one line per row, "<VERDICT> <game-name>: <detail>".
CLASSIFY="$(cat <<'EOF'
import os, re, sys
out, specs = sys.argv[1], sys.argv[2:]
DRAIN = ("ignored (no route", "leaving level", "exiting level", "queued",
         "ending the attract demo")
for spec in specs:
    row, trigger, at, expect = spec.split(":")
    trigger, at = int(trigger), int(at)
    d = os.path.join(out, "rows", row)
    try:
        log = open(os.path.join(d, "log.txt"), errors="replace").read()
        rc = open(os.path.join(d, "rc")).read().strip()
    except OSError:
        print(f"FAIL {row}: no output"); continue
    warp = [l for l in log.splitlines() if "levelwarp: " in l]
    if not any(k in l for l in warp for k in DRAIN):
        print(f"NOTRUN {row}: the trigger never reached a route decision"); continue
    states = []
    for line in log.splitlines():
        if not line.startswith("[state] "): continue
        f = dict(kv.split("=", 1) for kv in line.split()[1:])
        states.append({k: int(v) for k, v in f.items()})
    before = [s for s in states if s["frame"] < trigger]
    gs_at = before[-1]["gs"] if before else None
    if gs_at != at:
        print(f"VACUOUS {row}: gs {gs_at} at trigger frame {trigger}, row wants {at}"); continue
    last = states[-1] if states else {}
    tail = f"last state vbl={last.get('vbl')} gs={last.get('gs')} demo={last.get('demo')}"
    if rc != "0":
        print(f"FAIL {row}: exit {rc}; {tail}"); continue
    if expect == "arrive":
        m = re.search(r"harness: warp to level (\d+)", log)
        a = m and re.search(rf"arrived in level {m.group(1)} at vbl=(\d+)", log)
        print(f"PASS {row}: arrived vbl {a.group(1)}" if a else f"FAIL {row}: no arrival; {tail}")
        continue
    why = []
    if not any("ignored (no route from this state)" in l for l in warp): why.append("not ignored")
    if any("leaving level" in l for l in warp): why.append("left the level")
    demo = [s for s in states if s["frame"] >= trigger and s.get("demo", 0) != 0]
    if demo: why.append(f"demo word {demo[0]['demo']} at vbl {demo[0]['vbl']}")
    if last.get("gs") != 0: why.append("not back in gameplay")
    print(f"FAIL {row}: {', '.join(why)}; {tail}" if why else f"PASS {row}: refused; {tail}")
EOF
)"
RESULT="$(python3 -c "$CLASSIFY" "$OUT" "${SPECS[@]}")"
echo "$RESULT"
engaged="$(grep -vc '^NOTRUN' <<<"$RESULT")"
failed="$(grep -Ec '^(FAIL|VACUOUS|NOTRUN)' <<<"$RESULT")"
echo "check_levelwarp: rows ${#SPECS[@]}, engaged $engaged, failed $failed"
if (( engaged == 0 )); then
    echo "check_levelwarp: FAIL (zero engaged rows: no warp trigger fired)"; exit 1
fi
(( failed == 0 )) || { echo "check_levelwarp: FAIL"; exit 1; }
echo "check_levelwarp: PASS"
