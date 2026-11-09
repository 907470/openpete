#!/usr/bin/env bash

# tests/harness/check_record_minimize.sh - a recording survives a minimized window.
#
# Minimizing the window gives the surface a zero extent. A recording must keep
# rendering and encoding through it at the size it started at, and after the
# restore. The window opens at the exe's default size, so it stays on one
# monitor. The Windows exe
# is the only build where a scripted minimize reaches the swapchain (WSLg and
# xvfb ignore the request), so this runs the windows-mingw exe through WSL
# interop and minimizes it with PowerShell ShowWindow.
#
#   leg rec    --record-cfr 60: the window is minimized for 1 s once the log
#              prints `record: armed`, then restored. PASS needs exit 0, the
#              swapchain's zero-extent deferral line (the minimize engaged;
#              without it the leg is VACUOUS), a `record: finalized` line with
#              frames captured and 0 dropped, no `native size changed`, and a
#              clip whose decoded frame count equals the captured count.
#   leg norec  the same run without recording: exit 0 and the deferral line.
#
# Usage: tests/harness/check_record_minimize.sh [--exe PATH] [--out DIR]
# Exit 0 PASS, 1 FAIL, 2 VACUOUS, 3 REFUSED (an openpete process already runs on
# Windows), 64 SKIP (no exe, no powershell.exe or no disc).

set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
EXE="$ROOT/build/windows-mingw/openpete.exe"
OUT="$ROOT/build/check-record-minimize"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --exe) EXE="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        *) echo "check_record_minimize: unknown argument $1" >&2; exit 64 ;;
    esac
done
DISC="$ROOT/games/spyro-2/spyro-2/build/SCUS-94425.cue"
[[ -x "$EXE" ]] || { echo "check_record_minimize: SKIP: no exe at $EXE"; exit 64; }
[[ -f "$DISC" ]] || { echo "check_record_minimize: SKIP: no disc at $DISC"; exit 64; }
command -v powershell.exe >/dev/null || { echo "check_record_minimize: SKIP: no powershell.exe (not WSL)"; exit 64; }

. "$ROOT/scripts/lib/wslenv.sh"
wslenv_forward OPENPETE_VIBRATION=0
mkdir -p "$OUT"
rm -f "${OUT:?}"/rec.log "${OUT:?}"/norec.log "${OUT:?}"/clip-rec.mkv "${OUT:?}"/minwin.ps1

cat > "$OUT/minwin.ps1" <<'EOF'
Add-Type -Name W -Namespace U -MemberDefinition '[DllImport("user32.dll")] public static extern bool ShowWindow(System.IntPtr h,int c);'
$p = Get-Process openpete | Where-Object { $_.MainWindowHandle -ne 0 } | Sort-Object StartTime -Descending | Select-Object -First 1
if (-not $p) { "no-window"; exit 1 }
[U.W]::ShowWindow($p.MainWindowHandle,6) | Out-Null; Start-Sleep 1
[U.W]::ShowWindow($p.MainWindowHandle,9) | Out-Null; "minimized-restored"
EOF

# run_leg LEG: launches the exe, waits for the arming line (rec) or for the
# level skip to resume presents (norec; during the skip's fast-forward no
# frame begins, so a minimize there never reaches the swapchain), minimizes
# and restores, and waits for the exit.
run_leg() {
    local leg=$1 log="$OUT/$1.log" extra=() pid i
    [[ $leg == rec ]] && extra=(--record-cfr 60 --record "$(wslpath -w "$OUT")\\clip-rec.mkv")
    : > "$log"
    env "${WSLENV_ARGS[@]}" "$EXE" --game spyro-2 --no-config --disc "$(wslpath -w "$DISC")" \
        --renderer vk --no-card --mute 1 --skip-to-level glimmer \
        --frame-limit 1700 --input-lock --vibration 0 --background 1 "${extra[@]}" > "$log" 2>&1 &
    pid=$!
    for ((i = 0; i < 120; i++)); do
        if [[ $leg == rec ]]; then grep -q "record: armed" "$log" && break
        else grep -q "level playable - presents resume" "$log" && break; fi
        sleep 0.5
    done
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$OUT/minwin.ps1")" > "$OUT/$leg.min" 2>&1
    wait "$pid"
    echo "exit=$?" >> "$OUT/$leg.min"
}

# Refuse while another openpete process runs on Windows: its window would be
# a minimize target and the legs would race a session in use (exit 3).
running=$(powershell.exe -NoProfile -Command "(Get-Process openpete -ErrorAction SilentlyContinue | Measure-Object).Count" 2>/dev/null | tr -d '\r')
if [[ ${running:-0} -gt 0 ]]; then
    echo "check_record_minimize: REFUSED: $running openpete process(es) already running on Windows"
    exit 3
fi

verdict=0
run_leg rec
run_leg norec
for leg in rec norec; do
    log="$OUT/$leg.log"
    grep -q "^exit=0" "$OUT/$leg.min" || { echo "FAIL $leg: $(grep exit= "$OUT/$leg.min")"; verdict=1; }
    if ! grep -q "surface extent 0x0" "$log"; then
        echo "VACUOUS $leg: no zero-extent deferral line (the minimize did not reach the swapchain)"
        (( verdict == 0 )) && verdict=2
    fi
done
fin=$(grep -oE "[0-9]+ frame\(s\) captured, [0-9]+ dropped" "$OUT/rec.log" | tail -1)
cap=${fin%% *}; drop=$(sed -E 's/.*captured, ([0-9]+) dropped/\1/' <<< "$fin")
if [[ -z "$fin" || "$drop" != 0 || "${cap:-0}" -eq 0 ]]; then echo "FAIL rec: finalized '${fin:-none}'"; verdict=1; fi
grep -q "native size changed" "$OUT/rec.log" && { echo "FAIL rec: the recorder saw a size change"; verdict=1; }
if [[ -f "$OUT/clip-rec.mkv" ]]; then
    dec=$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$OUT/clip-rec.mkv")
    [[ "$dec" == "$cap" ]] || { echo "FAIL rec: clip decodes $dec frames, the recorder captured $cap"; verdict=1; }
else
    echo "FAIL rec: no clip"; verdict=1
fi
case $verdict in
    0) echo "check_record_minimize: PASS (captured $cap, dropped 0; both legs exit 0 through the minimize)" ;;
    1) echo "check_record_minimize: FAIL" ;;
    2) echo "check_record_minimize: VACUOUS" ;;
esac
exit $verdict
