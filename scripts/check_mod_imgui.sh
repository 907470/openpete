#!/usr/bin/env bash
# check_mod_imgui.sh — the ImGui annex's containment, proved in a window.
#
# Everything the annex guards runs only inside a mod UI section body, and a
# body runs only while the dev overlay is open. Headless never opens it, so
# the mods CI fixture can prove the annex compiles, loads and binds and
# nothing more. This script is the other half: it launches the game windowed
# with --devui-open (which opens the overlay and expands the mod's node at
# the first present) and the imgui-panel fixture, and reads the verdict off
# the log.
#
# Three legs, each its own session:
#   engaged     stock fixture -> "imgui annex engaged (N calls)", N > 0.
#                               Without it every other leg is vacuous: a
#                               bound table that nothing calls proves nothing.
#   bracket     break_end=1   -> the attributed "Missing End()" error, and
#                               the section refused after three presents.
#   guard       hook_call=1   -> "called outside the mod's UI section",
#                               naming the mod, once for the present hook's
#                               ImGui_Text and once for the sim-thread
#                               override's ImGui_TextUnformatted. Two
#                               functions, because the engine reports each
#                               (mod, function) pair once and the two
#                               dispatch sites can share a thread.
#
# Each leg opens a real 640x480 window on the current display and drives it
# unattended. Clicking one steers the overlay it is measuring, so leave the
# windows alone while it runs.
#
# Usage: scripts/check_mod_imgui.sh [--out=DIR] [--frames=N] [--keep]
# Exit:  0 all three legs pass | 1 a leg failed | 64 skipped (missing
#        binary / disc / display / Vulkan loader)

set -uo pipefail

# Unattended launcher: silence controller vibration for every run this script
# starts. Vibration ships on by default, which is right for a player and wrong
# for a machine-driven run — nobody is holding the pad, and the buzzing shows
# up in no log, so it would never surface on its own.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

cd "$(dirname "$0")/.."

OUT_DIR="/tmp/openpete-mod-imgui"
FRAMES=900
KEEP=0
for arg in "$@"; do
    case "$arg" in
        --out=*)    OUT_DIR="${arg#--out=}" ;;
        --frames=*) FRAMES="${arg#--frames=}" ;;
        --keep)     KEEP=1 ;;
        --help|-h)  sed -n '2,33p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown arg: $arg" >&2; exit 64 ;;
    esac
done

BIN="${OPENPETE_BIN:-build/linux-release/games/spyro-1/openpete-spyro1}"
DISC="${OPENPETE_DISC:-games/spyro-1/spyro-1/build/SCUS-94228.cue}"
FIXTURE="mods/imgui-panel"

[[ -x "$BIN"  ]] || { echo "SKIP: missing binary $BIN (build the preset first)" >&2; exit 64; }
[[ -f "$DISC" ]] || { echo "SKIP: missing disc $DISC" >&2; exit 64; }
[[ -d "$FIXTURE" ]] || { echo "SKIP: missing fixture $FIXTURE" >&2; exit 64; }
[[ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]] || {
    echo "SKIP: no display — the dev overlay exists only in a windowed run" >&2
    exit 64; }
if ! ldconfig -p 2>/dev/null | grep -q 'libvulkan\.so'; then
    [[ -e /usr/lib/x86_64-linux-gnu/libvulkan.so.1 ]] || {
        echo "SKIP: no Vulkan loader (libvulkan.so.1)" >&2; exit 64; }
fi

# The verdict is log text, not pixels, so the software rasterizer is the
# right device: it keeps the run off whatever GPU the machine happens to
# have and off the user's compositor budget.
LVP_ICD="/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
if [[ -e "$LVP_ICD" ]]; then export VK_ICD_FILENAMES="$LVP_ICD"; fi
export MESA_LOADER_DRIVER_OVERRIDE="${MESA_LOADER_DRIVER_OVERRIDE:-llvmpipe}"

# Stale outputs and a stale staged fixture both produce false verdicts, so
# both go before the first run.
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/mods"
cp -r "$FIXTURE" "$OUT_DIR/mods/imgui-panel"
rm -rf "$OUT_DIR/mods/imgui-panel/.build"

fails=0
verdict() {  # verdict LEG PASS|FAIL detail...
    local leg="$1" res="$2"; shift 2
    printf '[mod-imgui] %-8s %s  %s\n' "$leg" "$res" "$*"
    [[ "$res" == PASS ]] || fails=$((fails + 1))
}

# run_leg NAME [KEY=VALUE ...] — one session with the fixture configured by
# the given config.toml rows. Leaves the log at $OUT_DIR/<name>.log.
run_leg() {
    local name="$1"; shift
    local log="$OUT_DIR/$name.log"
    local cfg="$OUT_DIR/mods/imgui-panel/config.toml"
    rm -f "$log" "$cfg"
    : > "$cfg"
    local kv
    for kv in "$@"; do echo "${kv%%=*} = ${kv#*=}" >> "$cfg"; done
    "$BIN" --disc "$DISC" \
           --renderer vk --window 640x480 \
           --devui-open 1 \
           --mods-dir "$OUT_DIR/mods" --mods imgui-panel \
           --frame-limit "$FRAMES" --max-runtime-seconds 300 \
           --mute 1 --no-log-file \
           > "$log" 2>&1
    local rc=$?
    [[ -s "$log" ]] || { echo "[mod-imgui] $name: no log written" >&2; return 2; }
    if [[ $rc -ne 0 ]]; then
        echo "[mod-imgui] $name: run exited $rc" >&2
        tail -5 "$log" | sed 's/^/    /' >&2
        return 2
    fi
    if ! grep -q "imgui annex bound" "$log"; then
        echo "[mod-imgui] $name: the fixture never bound the annex" >&2
        tail -5 "$log" | sed 's/^/    /' >&2
        return 2
    fi
    return 0
}

# ── leg 1: engagement ─────────────────────────────────────────────────────
if run_leg engaged; then
    line=$(grep -m1 "imgui annex engaged" "$OUT_DIR/engaged.log")
    calls=$(sed -n 's/.*engaged (\([0-9]\+\) calls).*/\1/p' <<< "$line")
    if [[ -n "${calls:-}" && "$calls" -gt 0 ]]; then
        verdict engaged PASS "$calls call(s):$(sed 's/.*\] //' <<< "$line")"
    else
        verdict engaged FAIL "no 'imgui annex engaged (N calls)' with N > 0 in $OUT_DIR/engaged.log"
    fi
else
    verdict engaged FAIL "session did not produce a usable log"
fi

# ── leg 2: the error bracket and the three-strike refusal ─────────────────
if run_leg bracket break_end=true; then
    err=$(grep -m1 "Dear ImGui error inside the mod's UI section" "$OUT_DIR/bracket.log")
    ref=$(grep -m1 "UI section refused for this session" "$OUT_DIR/bracket.log")
    n_err=$(grep -c "Dear ImGui error inside the mod's UI section" "$OUT_DIR/bracket.log")
    if [[ -n "$err" && "$err" == *"imgui-panel"* && "$err" == *"Missing End()"* \
          && -n "$ref" && "$ref" == *"imgui-panel"* && "$n_err" -ge 3 ]]; then
        verdict bracket PASS "$n_err attributed error(s), then:$(sed 's/.*\] //' <<< "$ref")"
    else
        verdict bracket FAIL "expected >=3 attributed 'Missing End()' errors then the refusal; see $OUT_DIR/bracket.log"
    fi
else
    verdict bracket FAIL "session did not produce a usable log"
fi

# ── leg 3: the out-of-body guard, attributed to the mod ───────────────────
if run_leg guard hook_call=true; then
    drop=$(grep -m1 "called outside the mod's UI section" "$OUT_DIR/guard.log")
    n_present=$(grep -c "imgui-panel: ImGui_Text called outside the mod's UI section" "$OUT_DIR/guard.log")
    n_tick=$(grep -c "imgui-panel: ImGui_TextUnformatted called outside the mod's UI section" "$OUT_DIR/guard.log")
    if [[ "$n_present" -ge 1 && "$n_tick" -ge 1 ]]; then
        verdict guard PASS "present hook + sim-thread override both named:$(sed 's/.*\] //' <<< "$drop")"
    else
        verdict guard FAIL "expected refusals naming imgui-panel for ImGui_Text (present hook, saw $n_present) and ImGui_TextUnformatted (override, saw $n_tick); see $OUT_DIR/guard.log"
    fi
else
    verdict guard FAIL "session did not produce a usable log"
fi

if [[ $fails -eq 0 ]]; then
    echo "[mod-imgui] verdict: PASS (3/3 legs)"
    [[ $KEEP == 1 ]] || rm -rf "$OUT_DIR"
    exit 0
fi
echo "[mod-imgui] verdict: FAIL ($fails/3 legs); logs kept under $OUT_DIR"
exit 1
