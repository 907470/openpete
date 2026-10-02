#!/usr/bin/env bash
# bench-fps.sh - measure native-Vulkan present FPS over the attract demo.
#
# Runs the recompiled game (no bk2 => the built-in attract/demo, same content
# as the 20k oracle golden) with --frame-time-log, then summarizes the present
# cadence with scripts/measure-pacing.py. Every actual present is logged -
# canonical sim-tick AND high-FPS sub-tick - so the reported rate is the true
# achieved present FPS at --render-fps>30.
#
# Usage:
#   scripts/bench-fps.sh <exe> <out-log> [--fps N] [--frames N] [-- <extra exe args>]
#
# Example A/B (before vs after a change, same preset):
#   scripts/bench-fps.sh build/windows-mingw/games/spyro-1/openpete-spyro1.exe \
#       /tmp/ft-base.bin --fps 480 --frames 20000
#   scripts/bench-fps.sh build/windows-mingw/games/spyro-1/openpete-spyro1.exe \
#       /tmp/ft-after.bin --fps 480 --frames 20000
#
# Windows .exe paths are auto-translated for the disc + log args (wslpath -w);
# pass a Linux out-log path and read the summary on the Linux side.
set -euo pipefail



cd "$(dirname "$0")/.."

EXE="${1:?usage: bench-fps.sh <exe> <out-log> [--fps N] [--frames N] [-- <extra>]}"
LOG="${2:?usage: bench-fps.sh <exe> <out-log> [--fps N] [--frames N] [-- <extra>]}"
shift 2

FPS=480
FRAMES=20000
EXTRA=()
while [ $# -gt 0 ]; do
  case "$1" in
    --fps)    FPS="$2";    shift 2 ;;
    --frames) FRAMES="$2"; shift 2 ;;
    --)       shift; EXTRA+=("$@"); break ;;
    *)        EXTRA+=("$1"); shift ;;
  esac
done

DISC_LX="$(realpath games/spyro-1/spyro-1/build/SCUS-94228.cue)"
# .exe is a Windows binary under WSL: translate the path args it receives.
if [[ "$EXE" == *.exe ]]; then
  DISC_ARG="$(wslpath -w "$DISC_LX")"
  LOG_ARG="$(wslpath -w "$LOG")"
else
  DISC_ARG="$DISC_LX"
  LOG_ARG="$LOG"
fi

echo "bench-fps: exe=$EXE  fps=$FPS  frames=$FRAMES  log=$LOG"
# --vsync 0: present mode immediate so the true achieved rate shows (FIFO would
# pin the average to the monitor refresh). Pass --vsync 1 after -- to override.
# --vibration 0: unattended bench, no player uses the pad. The FLAG rather
# than the env spelling because this script is routinely pointed at the
# Windows .exe, and an exported knob does not cross that boundary without
# WSLENV - an argument cannot be dropped that way.
# (scripts/fences/rumble_guard.py enforces that this is here.)
# --input-lock: the bench window ignores the host keyboard, mouse and pads, so
# host input cannot open a menu or end the run mid-measurement. A flag
# for the same reason. (scripts/fences/input_lock_guard.py)
"$EXE" --disc "$DISC_ARG" --renderer vk \
       --render-fps "$FPS" --frame-limit "$FRAMES" --vsync 0 \
       --max-runtime-seconds 1200 --vibration 0 --input-lock \
       --frame-time-log "$LOG_ARG" "${EXTRA[@]}"

echo "=== measure-pacing: $LOG ==="
python3 scripts/measure-pacing.py "$LOG"
