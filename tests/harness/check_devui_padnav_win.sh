#!/usr/bin/env bash
# check_devui_padnav_win.sh - dev overlay pad navigation on the Windows exe.
#
# The dev overlay exists only in a windowed run, so every leg launches the
# windows-mingw exe with --background (window created without activation),
# --input-lock (physical devices hidden; the virtual pad and the injection
# script pass) and --pad-unfocused 1 (SDL otherwise drops the virtual pad's
# events while the window has no focus). The SDL virtual gamepad
# (OPENPETE_INPUT_VGAMEPAD=1) is driven by `pad <button> <0|1>` lines of the
# injection script (OPENPETE_MOUSE_INJECT), which replace its synthetic
# pattern for the run.
#
#   nav     d-pad down before the overlay opens (reaches the game: the
#           recorder sees the virtual pad), hold Back to open the overlay,
#           d-pad and south while it is open, hold Back to close it, d-pad
#           down after it closed. Pass: the overlay opened and closed once by
#           pad; its nav counters (engaged iterations, host moves and
#           activations, imgui moves and activations) are above zero; the
#           recorded port-0 records hold no Cross and exactly two runs of
#           Down (the presses before and after the overlay).
#   keys    M opens and closes the overlay, Escape opens and closes the
#           settings menu (the keyboard paths), then a 700 ms hold of Start
#           opens the settings menu and East closes it. Pass: two overlay opens and
#           closes and two menu opens and closes by key, and the order of the
#           Start-hold part in log order: the menu opens after the Start
#           press fires and before the East press, no close lands between
#           that open and the East press (the hold's release must not close
#           the menu it opened), and the menu closes after the East press.
#           The probe logs each fired script line ("inject ... line=N").
#   save    savestate_save bound to pad:misc1 by a harness config file
#           (--config: no other file is read). misc1 before the overlay
#           opens saves; misc1 while it is open does not. Pass: one save.
#   replay  a movie recorded from the synthetic virtual pad, replayed with
#           --devui-open 1 and the settings menu opened by the script, while
#           recording. Pass: the replay's port-0 records equal the movie's
#           over the movie's length, the overlay engaged and the menu
#           opened. Leg `control` replays the same way on CONTROL_EXE when
#           given (a build without the replay-precedence fix) and must
#           differ, or the replay leg proves nothing.
#
# Usage (from the openpete checkout root, a current windows-mingw build):
#   tests/harness/check_devui_padnav_win.sh [EXE] [CONTROL_EXE]
# Exit: 0 PASS, 1 FAIL, 2 VACUOUS, 64 SKIP (no exe or disc).

set -u
cd "${OPENPETE_ROOT:-$(pwd)}"

EXE="${1:-build/windows-mingw/games/spyro-1/openpete-spyro1.exe}"
CONTROL_EXE="${2:-}"
DISC="games/spyro-1/spyro-1/build/SCUS-94228.cue"
OUT="build/devui-padnav-win"
[ -x "$EXE" ] || { echo "[padnav] SKIP: no exe at $EXE"; exit 64; }
[ -f "$DISC" ] || { echo "[padnav] SKIP: no disc at $DISC"; exit 64; }
# A worktree reaches the disc through a WSL symlink, which a Windows process
# cannot follow; wslpath resolves it to the target's Windows path.
DISC_ARG="$(wslpath -w "$DISC" 2>/dev/null || echo "$DISC")"

# shellcheck source=scripts/lib/wslenv.sh
. scripts/lib/wslenv.sh
mkdir -p "$OUT"
rm -f build/devui-padnav-win/nav.bk2 build/devui-padnav-win/nav.bk2.args \
      build/devui-padnav-win/nav.log build/devui-padnav-win/nav.txt \
      build/devui-padnav-win/keys.log build/devui-padnav-win/keys.txt \
      build/devui-padnav-win/save.log build/devui-padnav-win/save.txt \
      build/devui-padnav-win/save.toml \
      build/devui-padnav-win/movie.bk2 build/devui-padnav-win/movie.bk2.args \
      build/devui-padnav-win/movie.log \
      build/devui-padnav-win/replay.bk2 build/devui-padnav-win/replay.bk2.args \
      build/devui-padnav-win/replay.log build/devui-padnav-win/replay.txt \
      build/devui-padnav-win/control.bk2 build/devui-padnav-win/control.bk2.args \
      build/devui-padnav-win/control.log

# PADNAV_LEGS (space-separated, default every leg) limits a rerun.
LEGS=" ${PADNAV_LEGS:-nav keys save replay} "
want() { [[ "$LEGS" == *" $1 "* ]]; }

COMMON=(--background --input-lock --vibration 0 --mute 1 --pad-unfocused 1)

# run <exe> <log> <env...> -- <args...>
run() {
    local exe="$1" log="$2"; shift 2
    local envs=()
    while [ "$#" -gt 0 ] && [ "$1" != "--" ]; do envs+=("$1"); shift; done
    shift
    wslenv_forward OPENPETE_VIBRATION=0 "${envs[@]}"
    env "${WSLENV_ARGS[@]}" "$exe" --disc "$DISC_ARG" "${COMMON[@]}" "$@" \
        >"$log" 2>&1
    echo "[padnav] $(basename "$log" .log) exit $?"
}

fails=0
verdict() { # verdict LEG PASS|FAIL|VACUOUS detail...
    local leg="$1" res="$2"; shift 2
    printf '[padnav] %-8s %s  %s\n' "$leg" "$res" "$*"
    case "$res" in PASS) ;; VACUOUS) [ "$fails" = 0 ] && fails=-1 ;; *) fails=1 ;; esac
}

# Port-0 field of every record of a bk2's input log, one per line.
port0() { # port0 <bk2>
    python3 - "$1" <<'PY'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
for line in z.read("Input Log.txt").decode("ascii", "replace").splitlines():
    if line.startswith("|") and line.count("|") >= 3:
        print(line.split("|")[2])
PY
}

# Runs of records whose button field holds column COL set (0 Up, 1 Down,
# 4 Select, 9 Cross), and the number of records with it set.
runs() { # runs <bk2> <col>
    port0 "$1" | python3 -c '
import sys
col = int(sys.argv[1]); runs = n = 0; prev = False
for f in sys.stdin:
    b = f.rstrip("\n").split(",")[-1]
    on = len(b) > col and b[col] != "."
    n += on; runs += on and not prev; prev = on
print(runs, n)' "$2"
}

count() { grep -c -- "$2" "$1" 2>/dev/null || true; }

# Order of the settings menu around a pad hold, in log order. Script lines
# PRESS, RELEASE and NEXT are the hold's press, its release and the press
# meant to close the menu. Prints hold_open (a menu open between PRESS and
# NEXT), closes_before_east (closes after that open and before NEXT) and
# close_after_east (a close after NEXT).
keys_order() { # keys_order <log> <press line> <release line> <next line>
    python3 - "$@" <<'PY'
import re, sys
log, press, release, nxt = sys.argv[1], *map(int, sys.argv[2:5])
fired = {}
seq = []
for i, l in enumerate(open(log, errors="replace")):
    m = re.search(r"inject iter=\d+ ms=\d+ line=(\d+) ", l)
    if m:
        fired.setdefault(int(m.group(1)), i)
    elif "menu: opened" in l:
        seq.append((i, "open"))
    elif re.search(r"\bmenu: closed\b", l):
        seq.append((i, "close"))
p, n = fired.get(press), fired.get(nxt)
if p is None or n is None or fired.get(release) is None:
    print("hold_open=0 closes_before_east=0 close_after_east=0 (script lines missing)")
    sys.exit()
opens = [i for i, k in seq if k == "open" and p < i < n]
o = opens[0] if opens else None
before = sum(1 for i, k in seq if k == "close" and o is not None and o < i < n)
after = 1 if any(k == "close" and i > n for i, k in seq) else 0
print(f"hold_open={1 if o is not None else 0} closes_before_east={before} close_after_east={after}")
PY
}

# ---- nav ---------------------------------------------------------------
if want nav; then
cat >"$OUT/nav.txt" <<'EOF'
8000ms pad dpdown 1
8150ms pad dpdown 0
10000ms pad back 1
11000ms pad back 0
12000ms pad dpdown 1
12150ms pad dpdown 0
12500ms pad dpdown 1
12650ms pad dpdown 0
13000ms pad south 1
13150ms pad south 0
13500ms pad dpdown 1
13650ms pad dpdown 0
14000ms pad south 1
14150ms pad south 0
16000ms pad back 1
17000ms pad back 0
19000ms pad dpdown 1
19150ms pad dpdown 0
EOF
run "$EXE" "$OUT/nav.log" OPENPETE_INPUT_VGAMEPAD=1 \
    OPENPETE_MOUSE_INJECT="$OUT/nav.txt" -- --no-config \
    --bk2-record "$OUT/nav.bk2" --frame-limit 1500
opens=$(count "$OUT/nav.log" "devui: open #")
summary=$(grep -m1 "devui: pad nav #1 closed" "$OUT/nav.log")
num() { sed -n "s/.* $1=\([0-9]*\).*/\1/p" <<<"$summary"; }
if ! grep -q "VIRTUAL gamepad attached" "$OUT/nav.log" || [ ! -f "$OUT/nav.bk2" ]; then
    verdict nav VACUOUS "no virtual pad or no recording; see $OUT/nav.log"
else
    read -r down_runs down_n < <(runs "$OUT/nav.bk2" 1)
    read -r cross_runs cross_n < <(runs "$OUT/nav.bk2" 9)
    eng=$(num engaged_iterations); mv=$(num moves); act=$(num activations)
    imv=$(num imgui_moves); iact=$(num imgui_activations)
    detail="opens=$opens engaged=$eng moves=$mv activations=$act imgui_moves=$imv imgui_activations=$iact down_runs=$down_runs cross_records=$cross_n"
    if [ "${down_runs:-0}" -lt 2 ]; then
        verdict nav VACUOUS "$detail (the presses outside the overlay did not reach the record)"
    elif [ "$opens" = 1 ] && [ -n "$summary" ] && [ "${eng:-0}" -gt 0 ] &&
         [ "${mv:-0}" -gt 0 ] && [ "${act:-0}" -gt 0 ] && [ "${imv:-0}" -gt 0 ] &&
         [ "${iact:-0}" -gt 0 ] && [ "$down_runs" = 2 ] && [ "${cross_n:-1}" = 0 ]; then
        verdict nav PASS "$detail"
    else
        verdict nav FAIL "$detail"
    fi
fi

fi

# ---- keys --------------------------------------------------------------
if want keys; then
cat >"$OUT/keys.txt" <<'EOF'
8000ms keydown M
8100ms keyup M
9000ms keydown M
9100ms keyup M
10000ms keydown Escape
10100ms keyup Escape
11000ms keydown Escape
11100ms keyup Escape
12000ms pad start 1
12700ms pad start 0
13000ms pad east 1
13150ms pad east 0
EOF
run "$EXE" "$OUT/keys.log" OPENPETE_INPUT_VGAMEPAD=1 OPENPETE_MOUSE_PROBE=1 \
    OPENPETE_MOUSE_INJECT="$OUT/keys.txt" -- --no-config --frame-limit 1100
d_open=$(count "$OUT/keys.log" "devui: open #")
d_close=$(count "$OUT/keys.log" "devui: pad nav #[0-9]* closed")
m_open=$(count "$OUT/keys.log" "menu: opened")
m_close=$(count "$OUT/keys.log" "menu: closed")
order=$(keys_order "$OUT/keys.log" 8 9 10)
detail="devui opens=$d_open closes=$d_close; menu opens=$m_open closes=$m_close; $order"
if ! grep -q "inject iter=.* line=10 " "$OUT/keys.log"; then
    verdict keys VACUOUS "$detail (the script's East press never fired)"
elif [ "$d_open" = 1 ] && [ "$d_close" = 1 ] && [ "$m_open" = 2 ] && [ "$m_close" = 2 ] &&
     [ "$order" = "hold_open=1 closes_before_east=0 close_after_east=1" ]; then
    verdict keys PASS "$detail (the Start hold opened the menu; East closed it)"
else
    verdict keys FAIL "$detail (want 1/1, 2/2 and hold_open=1 closes_before_east=0 close_after_east=1)"
fi

fi

# ---- save --------------------------------------------------------------
if want save; then
printf '[keys.host]\nsavestate_save = "pad:misc1"\n' >"$OUT/save.toml"
cat >"$OUT/save.txt" <<'EOF'
8000ms pad misc1 1
8150ms pad misc1 0
10000ms pad back 1
11000ms pad back 0
12000ms pad misc1 1
12150ms pad misc1 0
14000ms pad back 1
15000ms pad back 0
EOF
run "$EXE" "$OUT/save.log" OPENPETE_INPUT_VGAMEPAD=1 \
    OPENPETE_MOUSE_INJECT="$OUT/save.txt" -- --config "$OUT/save.toml" \
    --frame-limit 1100
saves=$(count "$OUT/save.log" "savestate.*saved ")
s_open=$(count "$OUT/save.log" "devui: open #")
if [ "$s_open" != 1 ]; then
    verdict save VACUOUS "the overlay opened $s_open times (want 1); see $OUT/save.log"
elif [ "$saves" = 1 ]; then
    verdict save PASS "saves=1 (misc1 before the overlay saved; misc1 under it did not)"
else
    verdict save FAIL "saves=$saves (want 1)"
fi

fi

# ---- replay ------------------------------------------------------------
if want replay; then
run "$EXE" "$OUT/movie.log" OPENPETE_INPUT_VGAMEPAD=1 -- --no-config \
    --bk2-record "$OUT/movie.bk2" --frame-limit 1500
cat >"$OUT/replay.txt" <<'EOF'
14000ms menu 1
18000ms menu 0
EOF
replay_leg() { # replay_leg <exe> <name>
    run "$1" "$OUT/$2.log" OPENPETE_MOUSE_INJECT="$OUT/replay.txt" -- --no-config \
        --bk2 "$OUT/movie.bk2" --devui-open 1 --bk2-record "$OUT/$2.bk2" \
        --frame-limit 1500
}
same_records() { # same_records <movie> <replay>: equal / differing / compared
    python3 - <(port0 "$1") <(port0 "$2") <<'PY'
import sys
a = open(sys.argv[1]).read().splitlines()
b = open(sys.argv[2]).read().splitlines()
n = min(len(a), len(b))
diff = sum(1 for i in range(n) if a[i] != b[i])
live = sum(1 for i in range(n) if a[i].split(",")[-1].strip(".") or i and a[i] != a[i-1])
print(n - diff, diff, n, live)
PY
}
replay_leg "$EXE" replay
if [ ! -f "$OUT/movie.bk2" ] || [ ! -f "$OUT/replay.bk2" ]; then
    verdict replay VACUOUS "movie or replay recording missing"
else
    read -r eq df n live < <(same_records "$OUT/movie.bk2" "$OUT/replay.bk2")
    rsum=$(grep -m1 "devui: pad nav #1 closed" "$OUT/replay.log")
    reng=$(sed -n 's/.* engaged_iterations=\([0-9]*\).*/\1/p' <<<"$rsum")
    rmenu=$(count "$OUT/replay.log" "menu: opened")
    detail="records=$n equal=$eq differing=$df movie_live=$live engaged=${reng:-0} menu_opens=$rmenu"
    if [ "${n:-0}" = 0 ] || [ "${live:-0}" = 0 ] || [ "${reng:-0}" = 0 ] || [ "$rmenu" = 0 ]; then
        verdict replay VACUOUS "$detail"
    elif [ "$df" = 0 ]; then
        verdict replay PASS "$detail"
    else
        verdict replay FAIL "$detail"
    fi
fi
if [ -n "$CONTROL_EXE" ]; then
    replay_leg "$CONTROL_EXE" control
    if [ -f "$OUT/control.bk2" ]; then
        read -r eq df n live < <(same_records "$OUT/movie.bk2" "$OUT/control.bk2")
        if [ "${df:-0}" -gt 0 ]; then
            verdict control PASS "the control build differs on $df of $n records"
        else
            verdict control VACUOUS "the control build matched the movie; the replay leg proves nothing"
        fi
    else
        verdict control VACUOUS "control recording missing"
    fi
fi

fi

case "$fails" in
    0)  echo "[padnav] PASS"; exit 0 ;;
    -1) echo "[padnav] VACUOUS"; exit 2 ;;
    *)  echo "[padnav] FAIL"; exit 1 ;;
esac
