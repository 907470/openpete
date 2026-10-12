#!/usr/bin/env bash

# tests/harness/check_rebind.sh - the binding page's capture, swap, save and
# input-ownership gate.
#
# Each leg boots Spyro 1 headless in Artisans with its own --config file (or
# --no-config for a replay), drives the binding page through the injection
# script's `rebind`, `unbind` and `restore` verbs (the requests the overlay's
# buttons post) and the SDL virtual pad (`pad <button> 0|1`, `pad <axis>
# <value>`; OPENPETE_INPUT_VGAMEPAD=1, --pad-unfocused 1), and records port 0
# with --bk2-record. Game-side checks use the virtual pad: an injected key
# never reaches PsyCross's keyboard read, so keys serve the hotkey and
# saved-row checks only; a script without a pad line scripts the pad
# released at iteration 1, or the virtual pad's synthetic pattern would
# press buttons through the capture. A script time is a pump iteration, which headless
# is one VBL, so record N-1 is the VBL of script line N.
#
#   capture   rebind pad.cross 1, pad east as the candidate; later east and
#             south pressed apart. Pass: [saved.keys.pad] cross = ["C",
#             "pad:east"], circle = ["V", "pad:south"]; the east press
#             records cross and not circle, the south press circle and not
#             cross; the capture press records nothing; one `rebind swap
#             pad.circle` line; bytes above the marker equal the original.
#   key       rebind pad.cross 0, then V. Pass: cross = ["V", "pad:south"],
#             circle = ["C", "pad:east"]; one swap line.
#   swap      rebind host.screenshot 0, then F7 (pause's key), then F12
#             twice (pause, resume: a paused run never reaches its limit).
#             Pass: screenshot = ["F7"], pause = ["F12"]; the swap line; one
#             pause line, at the F12 press (the F7 press of the capture logs
#             none).
#   restore   the file holds the capture leg's rows; restore pad.cross and
#             pad.circle, then south and east. Pass: the zone holds no
#             binding row, south records cross and east circle.
#   refuse    a wheel notch onto host.turbo (KM_LEVEL), pad south onto
#             pad.stick_up (key only), pad south onto host.pause (a
#             controller button binds a hotkey only as a hold). Pass: three
#             refusals, the third with the hold reason; no binding row.
#   hold      rebind host.pause add hold, pad south; a one-VBL south tap;
#             south held 300 VBLs, then F7 resumes. Pass: pause = ["F7", "hold:pad:south"];
#             the tap records cross and logs no pause line; the hold logs
#             one.
#   cancel_*  rebind pad.cross 1 three times: east held through an Escape
#             (cancel_esc) or a left click (cancel_click; headless draws no
#             prompt, so every click is outside) and released after it;
#             a left-stick throw held past the 5 s timeout (cancel_time;
#             VACUOUS without the timeout line). Pass: no binding row, the
#             cancel logged with its cause, and every record neutral (no
#             button, sticks centred) from the capture until the held
#             input's release.
#   fail      the capture leg with OPENPETE_WRITE_FAIL on the config. Pass:
#             a failed save, the file byte-identical.
#   replay    artisans-balloonist.bk2 replayed with a capture and an east
#             press mid-movie, recorded. Pass: the recording's port-0
#             records equal the movie's over every movie frame.
#   record    a capture with east as the candidate and square pressed
#             during it and held past the commit, recorded; the recording
#             replayed --no-config and recorded again. Pass: the recording
#             holds no circle or square bit from the capture until
#             square's release; the replay's records equal it.
#   control   OPENPETE_REBIND_MUTATE_NO_CAPTURE_TERM=1 reruns cancel_esc,
#             cancel_time, record and swap, which must FAIL (east, the stick
#             and square reach the records; F7 pauses during the capture);
#             OPENPETE_REBIND_MUTATE_NO_V22=1 reruns hold, which must FAIL.
#
# Engagement: commits logged > 0 over the capture leg, and every leg's
# expected log line present, or the leg is VACUOUS.
#
# Usage: tests/harness/check_rebind.sh [--build-dir=DIR]
# Env:   REBIND_LEGS (space-separated) limits a rerun; REBIND_OUT the output dir.
# Exit:  0 pass, 1 fail, 2 vacuous, 64 skip (no binary / disc).

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

for a in "$@"; do
    case "$a" in --build-dir=*) export PG_BUILD_DIR="${a#--build-dir=}" ;; esac
done
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh
pg_resolve spyro-1 || exit 64
[[ -x "$PG_BIN" ]]  || { echo "[rebind] SKIP: no binary $PG_BIN"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "[rebind] SKIP: no disc $PG_DISC"; exit 64; }
MOVIE="$PG_BK2_DIR/artisans-balloonist.bk2"

OUT="${REBIND_OUT:-${TMPDIR:-/tmp}/openpete-check-rebind}"
mkdir -p "$OUT"
MARKER='# >>> openpete managed settings - written by the in-game menus <<<'
LEGS=" ${REBIND_LEGS:-capture key swap restore refuse hold cancel fail replay record control} "
want() { [[ "$LEGS" == *" $1 "* ]]; }

HAND='# hand-written settings, kept byte for byte by every save
fov = 0
'

fails=0
verdict() { # verdict LEG PASS|FAIL|VACUOUS detail...
    local leg="$1" res="$2"; shift 2
    printf '[rebind] %-12s %s  %s\n' "$leg" "$res" "$*"
    case "$res" in PASS) ;; VACUOUS) [[ "$fails" == 0 ]] && fails=-1 ;; *) fails=1 ;; esac
}

# run TAG SCRIPT [ENV...] -- [ARGS...]: one headless run; the config is
# $OUT/TAG.toml unless ARGS name another. Removes the leg's outputs first.
run() {
    local tag="$1" script="$2"; shift 2
    local -a env=()
    while [[ $# -gt 0 && "$1" != "--" ]]; do env+=("$1"); shift; done
    [[ "${1:-}" == "--" ]] && shift
    rm -f "${OUT:?}/$tag.log" "${OUT:?}/$tag.bk2" "${OUT:?}/$tag.bk2.args"
    env OPENPETE_VIBRATION=0 OPENPETE_INPUT_VGAMEPAD=1 OPENPETE_MOUSE_INJECT="$script" \
        ${env[@]+"${env[@]}"} timeout 600 "$PG_BIN" --headless \
        --disc "$PG_DISC" --disc-cache 0 ${PG_RUN_ARGS[@]+"${PG_RUN_ARGS[@]}"} \
        --renderer vk --no-card --mute 1 --vibration 0 --pad-unfocused 1 \
        --bk2-record "$OUT/$tag.bk2" "$@" >"$OUT/$tag.log" 2>&1
    local rc=$?
    [[ -f "$OUT/$tag.bk2" ]] || echo "[rebind] $tag: exit $rc, no recording (see $OUT/$tag.log)"
}

count() { grep -c -- "$2" "$OUT/$1.log" 2>/dev/null || true; }
zone() { python3 - "$1" "$MARKER" <<'EOF'
import sys
b = open(sys.argv[1]).read()
i = b.find(sys.argv[2])
print(b[i:] if i >= 0 else "")
EOF
}
head_of() { python3 - "$1" "$MARKER" <<'EOF'
import sys
b = open(sys.argv[1], 'rb').read()
i = b.find(sys.argv[2].encode())
sys.stdout.buffer.write(b if i < 0 else b[:i])
EOF
}

# recs BK2 FIRST LAST CHECK: evaluate the port-0 records FIRST..LAST (record
# indices) with CHECK, a python expression over `b` (the 16 digital columns:
# 0 Up 1 Down 2 Left 3 Right 4 Select 5 Start 6 Square 7 Triangle 8 Circle
# 9 Cross) and `ax` (LX, LY, RX, RY); prints "ok N" or "bad N first=I".
recs() {
    python3 - "$@" <<'EOF'
import sys, zipfile
path, first, last, check = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
z = zipfile.ZipFile(path)
rows = [l.split("|")[2] for l in z.read("Input Log.txt").decode().splitlines()
        if l.startswith("|") and l.count("|") >= 3]
bad, firstbad, n = 0, -1, 0
for i in range(first, min(last + 1, len(rows))):
    f = rows[i].split(",")
    ax = [int(x) for x in f[:4]]
    b = [c != "." for c in f[4]]
    n += 1
    if not eval(check):
        bad += 1
        if firstbad < 0: firstbad = i
print(("ok %d" % n) if not bad and n else ("bad %d first=%d of %d" % (bad, firstbad, n)))
EOF
}
NEUTRAL='not any(b) and ax == [128, 128, 128, 128]'

# port0 BK2: the port-0 field of every record.
port0() { python3 - "$1" <<'EOF'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
for l in z.read("Input Log.txt").decode().splitlines():
    if l.startswith("|") and l.count("|") >= 3: print(l.split("|")[2])
EOF
}
same() { # same A B [N]: equal/differing/compared over the first N (default min) records
    python3 - <(port0 "$1") <(port0 "$2") "${3:-0}" <<'EOF'
import sys
a = open(sys.argv[1]).read().splitlines(); b = open(sys.argv[2]).read().splitlines()
n = min(len(a), len(b)); m = int(sys.argv[3])
if m: n = min(n, m)
d = sum(1 for i in range(n) if a[i] != b[i])
print(n - d, d, n)
EOF
}

script() { cat >"$OUT/$1.txt"; }

# ---- capture -----------------------------------------------------------
CAPTURE_SCRIPT='1500 rebind pad.cross 1
1520 pad east 1
1530 pad east 0
1700 pad east 1
1720 pad east 0
1800 pad south 1
1820 pad south 0'
cap_rows='cross = ["C", "pad:east"]
circle = ["V", "pad:south"]'
if want capture || want restore || want fail; then
    printf '%s\n' "$CAPTURE_SCRIPT" | script capture
    rm -f "${OUT:?}/capture.toml" "${OUT:?}/capture.orig"
    printf '%s' "$HAND" >"$OUT/capture.toml"; cp "$OUT/capture.toml" "$OUT/capture.orig"
    run capture "$OUT/capture.txt" -- --config "$OUT/capture.toml" --skip-to-level artisans --frame-limit 2000
fi
if want capture; then
    commits=$(count capture 'rebind commit pad.cross pad:east')
    swaps=$(count capture 'rebind swap pad.circle pad:east -> pad:south')
    z=$(zone "$OUT/capture.toml")
    e=$(recs "$OUT/capture.bk2" 1700 1718 'b[9] and not b[8]')
    s=$(recs "$OUT/capture.bk2" 1800 1818 'b[8] and not b[9]')
    c=$(recs "$OUT/capture.bk2" 1499 1540 "$NEUTRAL")
    same_head=0; cmp -s <(head_of "$OUT/capture.toml") "$OUT/capture.orig" && same_head=1
    detail="commits=$commits swaps=$swaps east:[$e] south:[$s] capture-press:[$c] head_same=$same_head"
    if [[ "$commits" == 0 ]]; then verdict capture VACUOUS "$detail"
    elif [[ "$z" == *"$cap_rows"* && "$swaps" == 1 && "$e" == ok* && "$s" == ok* &&
            "$c" == ok* && "$same_head" == 1 ]]; then verdict capture PASS "$detail"
    else verdict capture FAIL "$detail; zone: $z"; fi
fi

# ---- key ---------------------------------------------------------------
if want key; then
    script key <<'EOF'
1 pad south 0
1500 rebind pad.cross 0
1520 keydown V
1530 keyup V
EOF
    rm -f "${OUT:?}/key.toml"; printf '%s' "$HAND" >"$OUT/key.toml"
    run key "$OUT/key.txt" -- --config "$OUT/key.toml" --skip-to-level artisans --frame-limit 1700
    z=$(zone "$OUT/key.toml")
    swaps=$(count key 'rebind swap pad.circle V -> C')
    detail="commits=$(count key 'rebind commit pad.cross V') swaps=$swaps"
    if [[ $(count key 'rebind commit') == 0 ]]; then verdict key VACUOUS "$detail"
    elif [[ "$z" == *'cross = ["V", "pad:south"]'* && "$z" == *'circle = ["C", "pad:east"]'* &&
            "$swaps" == 1 ]]; then verdict key PASS "$detail"
    else verdict key FAIL "$detail; zone: $z"; fi
fi

# ---- swap --------------------------------------------------------------
SWAP_SCRIPT='1 pad south 0
1500 rebind host.screenshot 0
1520 keydown F7
1530 keyup F7
1600 keydown F12
1610 keyup F12
1620 keydown F12
1630 keyup F12'
swap_leg() { # swap_leg TAG [EXTRA_SCRIPT] ENV...
    local tag="$1" extra="${2:-}"; shift; [[ $# -gt 0 ]] && shift
    printf '%s\n%s\n' "$SWAP_SCRIPT" "$extra" | script "$tag"
    rm -f "${OUT:?}/$tag.toml"; printf '%s' "$HAND" >"$OUT/$tag.toml"
    run "$tag" "$OUT/$tag.txt" "$@" -- --config "$OUT/$tag.toml" --skip-to-level artisans --frame-limit 1700
}
if want swap; then
    swap_leg swap ""
    z=$(zone "$OUT/swap.toml")
    pauses=$(grep -o 'frame-step: paused at openpete_frame=[0-9]*' "$OUT/swap.log" | sed 's/.*=//' | tr '\n' ' ')
    detail="swaps=$(count swap 'rebind swap host.pause F7 -> F12') pause_frames=[$pauses]"
    set -- $pauses
    if [[ $(count swap 'rebind commit host.screenshot F7') == 0 ]]; then verdict swap VACUOUS "$detail"
    elif [[ "$z" == *'screenshot = ["F7"]'* && "$z" == *'pause = ["F12"]'* && $# == 1 &&
            $1 -ge 1600 && $(count swap 'rebind swap host.pause F7 -> F12') == 1 ]]; then
        verdict swap PASS "$detail"
    else verdict swap FAIL "$detail; zone: $z"; fi
fi

# ---- restore -----------------------------------------------------------
if want restore; then
    script restore <<'EOF'
1500 restore pad.cross
1510 restore pad.circle
1700 pad south 1
1720 pad south 0
1800 pad east 1
1820 pad east 0
EOF
    rm -f "${OUT:?}/restore.toml"; cp "$OUT/capture.toml" "$OUT/restore.toml"
    run restore "$OUT/restore.txt" -- --config "$OUT/restore.toml" --skip-to-level artisans --frame-limit 2000
    z=$(zone "$OUT/restore.toml")
    s=$(recs "$OUT/restore.bk2" 1700 1718 'b[9] and not b[8]')
    e=$(recs "$OUT/restore.bk2" 1800 1818 'b[8] and not b[9]')
    detail="restores=$(count restore 'rebind restore') south:[$s] east:[$e]"
    if [[ $(count restore 'rebind restore') != 2 ]]; then verdict restore VACUOUS "$detail"
    elif [[ "$z" != *'cross ='* && "$z" != *'circle ='* && "$s" == ok* && "$e" == ok* ]]; then
        verdict restore PASS "$detail"
    else verdict restore FAIL "$detail; zone: $z"; fi
fi

# ---- refuse ------------------------------------------------------------
if want refuse; then
    script refuse <<'EOF'
1500 rebind host.turbo add
1520 wheel 1
1600 rebind pad.stick_up add
1620 pad south 1
1630 pad south 0
1700 rebind host.pause add
1720 pad south 1
1730 pad south 0
EOF
    rm -f "${OUT:?}/refuse.toml"; printf '%s' "$HAND" >"$OUT/refuse.toml"
    run refuse "$OUT/refuse.txt" -- --config "$OUT/refuse.toml" --skip-to-level artisans --frame-limit 1800
    r=$(count refuse 'rebind refused')
    v22=$(count refuse 'rebind refused host.pause .*only as a hold')
    detail="refusals=$r hold_reason=$v22 commits=$(count refuse 'rebind commit')"
    if [[ $(count refuse 'rebind capture') != 3 ]]; then verdict refuse VACUOUS "$detail"
    elif [[ "$r" == 3 && "$v22" == 1 && $(count refuse 'rebind commit') == 0 &&
            "$(zone "$OUT/refuse.toml")" != *'[saved.keys'* ]]; then verdict refuse PASS "$detail"
    else verdict refuse FAIL "$detail"; fi
fi

# ---- hold --------------------------------------------------------------
HOLD_SCRIPT='1500 rebind host.pause add hold
1520 pad south 1
1530 pad south 0
1600 pad south 1
1601 pad south 0
1700 pad south 1
2000 pad south 0
2010 keydown F7
2020 keyup F7'
hold_leg() { # hold_leg TAG ENV...
    local tag="$1"; shift
    printf '%s\n' "$HOLD_SCRIPT" | script "$tag"
    rm -f "${OUT:?}/$tag.toml"; printf '%s' "$HAND" >"$OUT/$tag.toml"
    run "$tag" "$OUT/$tag.txt" "$@" -- --config "$OUT/$tag.toml" --skip-to-level artisans --frame-limit 2100
}
hold_ok() { # hold_ok TAG -> prints PASS or the failing detail
    local tag="$1"
    local z pauses tap
    z=$(zone "$OUT/$tag.toml")
    pauses=$(grep -o 'frame-step: paused at openpete_frame=[0-9]*' "$OUT/$tag.log" | sed 's/.*=//' | tr '\n' ' ')
    tap=$(recs "$OUT/$tag.bk2" 1599 1599 'b[9]')
    set -- $pauses
    if [[ "$z" == *'pause = ["F7", "hold:pad:south"]'* && "$tap" == ok* && $# == 1 &&
          $1 -ge 1700 ]]; then echo PASS
    else echo "pause_frames=[$pauses] tap:[$tap]"; fi
}
if want hold; then
    hold_leg hold
    res=$(hold_ok hold)
    if [[ $(count hold 'rebind capture host.pause') == 0 ]]; then verdict hold VACUOUS "no capture"
    elif [[ "$res" == PASS ]]; then verdict hold PASS "$(grep -o 'frame-step: paused at openpete_frame=[0-9]*' "$OUT/hold.log")"
    else verdict hold FAIL "$res; zone: $(zone "$OUT/hold.toml")"; fi
fi

# ---- cancel ------------------------------------------------------------
cancel_leg() { # cancel_leg TAG SCRIPT_KIND ENV...
    local tag="$1" kind="$2"; shift 2
    case "$kind" in
        esc)   script "$tag" <<'EOF'
1500 rebind pad.cross 1
1520 pad east 1
1540 keydown Escape
1545 keyup Escape
1600 pad east 0
EOF
               ;;
        click) script "$tag" <<'EOF'
1500 rebind pad.cross 1
1520 pad east 1
1540 down left
1545 up left
1600 pad east 0
EOF
               ;;
        time)  script "$tag" <<'EOF'
1500 rebind pad.cross 1
1520 pad leftx 32000
3800 pad leftx 0
EOF
               ;;
    esac
    rm -f "${OUT:?}/$tag.toml"; printf '%s' "$HAND" >"$OUT/$tag.toml"
    local limit=1700; [[ "$kind" == time ]] && limit=3900
    run "$tag" "$OUT/$tag.txt" "$@" -- --config "$OUT/$tag.toml" --skip-to-level artisans --frame-limit "$limit"
}
cancel_check() { # cancel_check TAG CAUSE LAST -> verdict word and detail
    local tag="$1" cause="$2" last="$3"
    local n r
    n=$(count "$tag" "rebind cancel pad.cross frame=[0-9]* cause=$cause")
    r=$(recs "$OUT/$tag.bk2" 1500 "$last" "$NEUTRAL")
    if [[ "$n" == 0 ]]; then echo "VACUOUS no '$cause' cancel"
    elif [[ "$r" == ok* && $(count "$tag" 'rebind commit') == 0 &&
            "$(zone "$OUT/$tag.toml")" != *'[saved.keys'* ]]; then echo "PASS records:[$r]"
    else echo "FAIL records:[$r] commits=$(count "$tag" 'rebind commit')"; fi
}
if want cancel; then
    for k in esc click time; do
        cancel_leg "cancel_$k" "$k"
        case "$k" in esc) c=escape; l=1598 ;; click) c="click outside the prompt"; l=1598 ;; time) c=timeout; l=3798 ;; esac
        res=$(cancel_check "cancel_$k" "$c" "$l")
        verdict "cancel_$k" ${res%% *} "${res#* } menu_opens=$(count "cancel_$k" 'menu: opened')"
    done
fi

# ---- fail --------------------------------------------------------------
if want fail; then
    rm -f "${OUT:?}/fail.toml"; printf '%s' "$HAND" >"$OUT/fail.toml"
    run fail "$OUT/capture.txt" OPENPETE_WRITE_FAIL=fail.toml -- --config "$OUT/fail.toml" \
        --skip-to-level artisans --frame-limit 2000
    failed=$(count fail 'save FAILED')
    detail="commits=$(count fail 'rebind commit') failed_saves=$failed"
    if [[ $(count fail 'rebind commit') == 0 ]]; then verdict fail VACUOUS "$detail"
    elif [[ "$failed" -ge 1 ]] && cmp -s "$OUT/fail.toml" "$OUT/capture.orig"; then
        verdict fail PASS "$detail"
    else verdict fail FAIL "$detail"; fi
fi

# ---- replay ------------------------------------------------------------
if want replay; then
    if [[ ! -f "$MOVIE" ]]; then verdict replay VACUOUS "no movie $MOVIE"; else
    script replay <<'EOF'
1500 rebind pad.cross 1
1520 pad east 1
1530 pad east 0
EOF
    run replay "$OUT/replay.txt" -- --no-config --bk2 "$MOVIE" --skip-to-level artisans --frame-limit 2500
    read -r eq df n < <(same "$MOVIE" "$OUT/replay.bk2" 2500)
    detail="commits=$(count replay 'rebind commit') records=$n equal=$eq differing=$df"
    if [[ $(count replay 'rebind commit') == 0 || "${n:-0}" == 0 ]]; then verdict replay VACUOUS "$detail"
    elif [[ "$df" == 0 ]]; then verdict replay PASS "$detail"
    else verdict replay FAIL "$detail"; fi
    fi
fi

# ---- record ------------------------------------------------------------
RECORD_SCRIPT='1500 rebind pad.cross 1
1520 pad east 1
1530 pad west 1
1560 pad east 0
1700 pad west 0'
record_leg() { # record_leg TAG ENV...
    local tag="$1"; shift
    printf '%s\n' "$RECORD_SCRIPT" | script "$tag"
    rm -f "${OUT:?}/$tag.toml"; printf '%s' "$HAND" >"$OUT/$tag.toml"
    run "$tag" "$OUT/$tag.txt" "$@" -- --config "$OUT/$tag.toml" --skip-to-level artisans --frame-limit 1900
}
if want record; then
    record_leg record
    r=$(recs "$OUT/record.bk2" 1500 1698 'not b[8] and not b[6] and not b[9]')
    rm -f "${OUT:?}/record_replay.txt"; : >"$OUT/record_replay.txt"
    run record_replay "$OUT/record_replay.txt" -- --no-config --bk2 "$OUT/record.bk2" \
        --skip-to-level artisans --frame-limit 1900
    read -r eq df n < <(same "$OUT/record.bk2" "$OUT/record_replay.bk2")
    detail="commits=$(count record 'rebind commit') span:[$r] replay equal=$eq differing=$df of $n"
    if [[ $(count record 'rebind commit') == 0 ]]; then verdict record VACUOUS "$detail"
    elif [[ "$r" == ok* && "$df" == 0 && "${n:-0}" -gt 0 ]]; then verdict record PASS "$detail"
    else verdict record FAIL "$detail"; fi
fi

# ---- control -----------------------------------------------------------
if want control; then
    M=OPENPETE_REBIND_MUTATE_NO_CAPTURE_TERM=1
    cancel_leg ctl_esc esc "$M"
    res=$(cancel_check ctl_esc escape 1598)
    [[ "$res" == FAIL* ]] && verdict ctl_esc PASS "the mutation fails cancel_esc: $res" ||
        verdict ctl_esc VACUOUS "the mutation did not fail cancel_esc: $res"
    cancel_leg ctl_time time "$M"
    res=$(cancel_check ctl_time timeout 3798)
    [[ "$res" == FAIL* ]] && verdict ctl_time PASS "the mutation fails cancel_time: $res" ||
        verdict ctl_time VACUOUS "the mutation did not fail cancel_time: $res"
    record_leg ctl_record "$M"
    r=$(recs "$OUT/ctl_record.bk2" 1500 1698 'not b[8] and not b[6] and not b[9]')
    [[ "$r" != ok* ]] && verdict ctl_record PASS "the mutation fails record: [$r]" ||
        verdict ctl_record VACUOUS "the mutation did not fail record: [$r]"
    # The capture's F7 pauses under the mutation, so the two F12 presses
    # leave the run paused; a third resumes it.
    swap_leg ctl_swap $'1650 keydown F12\n1660 keyup F12' "$M"
    p=$(grep -o 'frame-step: paused at openpete_frame=[0-9]*' "$OUT/ctl_swap.log" | sed 's/.*=//' | head -1)
    [[ -n "$p" && "$p" -lt 1600 ]] && verdict ctl_swap PASS "F7 paused during the capture at $p" ||
        verdict ctl_swap VACUOUS "F7 did not pause during the capture (first pause ${p:-none})"
    hold_leg ctl_hold OPENPETE_REBIND_MUTATE_NO_V22=1
    res=$(hold_ok ctl_hold)
    [[ "$res" != PASS ]] && verdict ctl_hold PASS "the V22 mutation fails hold: $res" ||
        verdict ctl_hold VACUOUS "the V22 mutation did not fail hold"
fi

echo "[rebind] outputs in $OUT"
case "$fails" in
    0)  echo "[rebind] PASS"; exit 0 ;;
    -1) echo "[rebind] VACUOUS"; exit 2 ;;
    *)  echo "[rebind] FAIL"; exit 1 ;;
esac
