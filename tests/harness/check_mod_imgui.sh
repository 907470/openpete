#!/usr/bin/env bash
# check_mod_imgui.sh - the mod ImGui containment, proved in a window.
#
# Everything the table guards runs only inside a mod UI section body, and a
# body runs only while the dev overlay is open. Headless never opens it, so
# the mods CI fixture can prove the table compiles, loads and binds and
# nothing more. This script is the other half: it launches the game windowed
# with --devui-open (which opens the overlay and expands the mod's node at
# the first present) and the imgui-panel fixture, and reads the verdict off
# the log.
#
# Six legs, each its own session:
#   engaged     stock fixture -> "ImGui engaged (N calls)", N > 0.
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
#   closed      overlay never opened, the mouse parked on the always
#                               window's button -> the engaged line (the
#                               body ran with no overlay at all) and a
#                               hovered count of 0: an always window is
#                               display-only while the overlay is closed.
#   hover       the same injection with the overlay open -> a hovered count
#                               above 0. The positive control: without it a
#                               zero from the closed leg could be a
#                               mis-aimed cursor rather than NoInputs.
#   composite   crt-beans enabled alongside the fixture -> the engaged line
#                               under a COMPOSITE postfx chain, which moves
#                               the overlay to the post-chain render site.
#
# Each leg opens a real 640x480 window on the current display and drives it
# unattended. Clicking one steers the overlay it is measuring, so leave the
# windows alone while it runs.
#
# Usage: tests/harness/check_mod_imgui.sh [--game SLUG] [--out=DIR] [--frames=N]
#                                   [--keep]
# Exit:  0 all six legs pass | 1 a leg failed | 64 skipped (missing
#        binary / disc / display / Vulkan loader)
#
# The fixture mod is Spyro 1's, so --game names the game whose binary and
# disc are used and refuses every other slug rather than loading a mod the
# other game's loader would reject.
#
# Every leg opens a window, so the runs do not go through pg_capture, whose
# launcher is headless; they take their binary and disc from the same
# resolver and set --vibration 0 themselves.

set -uo pipefail

# Unattended launcher: silence controller vibration for every run this script
# starts. Vibration ships on by default, which is right for a player and wrong
# for a scripted run - no player uses the pad, and the buzzing shows
# up in no log, so it would never surface on its own.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

GAME="$(pg_parse_game "$@")" || exit 64

OUT_DIR="/tmp/openpete-mod-imgui"
FRAMES=900
KEEP=0
for arg in "$@"; do
    case "$arg" in
        --out=*)    OUT_DIR="${arg#--out=}" ;;
        --frames=*) FRAMES="${arg#--frames=}" ;;
        --keep)     KEEP=1 ;;
        --game|--game=*) ;;   # consumed by pg_parse_game above
        spyro-*)         ;;   # the two-word --game SLUG spelling
        --help|-h)  sed -n '2,45p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown arg: $arg" >&2; exit 64 ;;
    esac
done

pg_resolve "$GAME"
BIN="$PG_BIN"
DISC="$PG_DISC"
FIXTURE="mods/imgui-panel"
COMPOSITE_MOD="mods/crt-beans"

# Window size, and the cursor coordinate that lands on the always window's
# "hover me" button. The fixture pins that window at x=16, 68px up from the
# bottom edge, with no title bar and a 200x36 button one window padding (8px)
# in - so the button spans x 24..224, y 420..456 in a 640x480 window and its
# centre is (124, 438). Below the dev config panel, which ends 96px short of
# the bottom, so the button stays hoverable while the panel is up.
WIN_W=640
WIN_H=480
HOVER_XY="124,438"

[[ "$PG_GAME" == "spyro-1" ]] || {
    echo "SKIP: the imgui-panel fixture is spyro-1's, not $PG_GAME's" >&2; exit 64; }
[[ -x "$BIN"  ]] || { echo "SKIP: missing binary $BIN (build the preset first)" >&2; exit 64; }
[[ -f "$DISC" ]] || { echo "SKIP: missing disc $DISC" >&2; exit 64; }
[[ -d "$FIXTURE" ]] || { echo "SKIP: missing fixture $FIXTURE" >&2; exit 64; }
[[ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]] || {
    echo "SKIP: no display - the dev overlay exists only in a windowed run" >&2
    exit 64; }
if ! ldconfig -p 2>/dev/null | grep -q 'libvulkan\.so'; then
    [[ -e /usr/lib/x86_64-linux-gnu/libvulkan.so.1 ]] || {
        echo "SKIP: no Vulkan loader (libvulkan.so.1)" >&2; exit 64; }
fi

# The verdict is log text, not pixels, so the software rasterizer is the
# right device: it keeps the run off whatever GPU the machine happens to
# have and off the user's compositor budget.
# shellcheck source=scripts/lib/swrender.sh
source scripts/lib/swrender.sh
sw_render_env

# Stale outputs and a stale staged fixture both produce false verdicts, so
# both go before the first run.
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/mods"
cp -r "$FIXTURE" "$OUT_DIR/mods/imgui-panel"
rm -rf "$OUT_DIR/mods/imgui-panel/.build"
if [[ -d "$COMPOSITE_MOD" ]]; then
    cp -r "$COMPOSITE_MOD" "$OUT_DIR/mods/crt-beans"
    rm -rf "$OUT_DIR/mods/crt-beans/.build"
fi

fails=0
verdict() {  # verdict LEG PASS|FAIL detail...
    local leg="$1" res="$2"; shift 2
    printf '[mod-imgui] %-8s %s  %s\n' "$leg" "$res" "$*"
    [[ "$res" == PASS ]] || fails=$((fails + 1))
}

# run_leg NAME [KEY=VALUE ...] - one session with the fixture configured by
# the given config.toml rows. Leaves the log at $OUT_DIR/<name>.log.
# LEG_OPEN (default 1) drives --devui-open; LEG_MODS (default imgui-panel)
# the enabled modset; LEG_INJECT, when non-empty, OPENPETE_DEVUI_INJECT_MOUSE.
run_leg() {
    local name="$1"; shift
    local log="$OUT_DIR/$name.log"
    local cfg="$OUT_DIR/mods/imgui-panel/config.toml"
    rm -f "$log" "$cfg"
    : > "$cfg"
    local kv
    for kv in "$@"; do echo "${kv%%=*} = ${kv#*=}" >> "$cfg"; done
    local extra=()
    local -a envs=()
    [[ -n "${LEG_INJECT:-}" ]] && envs+=(OPENPETE_DEVUI_INJECT_MOUSE="$LEG_INJECT")
    env "${envs[@]}" "$BIN" --disc "$DISC" --disc-cache 0 \
           "${PG_RUN_ARGS[@]}" \
           --renderer vk --window "${WIN_W}x${WIN_H}" \
           --devui-open "${LEG_OPEN:-1}" \
           --mods-dir "$OUT_DIR/mods" --mods "${LEG_MODS:-imgui-panel}" \
           "${extra[@]}" \
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
    if ! grep -q "ImGui bound" "$log"; then
        echo "[mod-imgui] $name: the fixture never bound the table" >&2
        tail -5 "$log" | sed 's/^/    /' >&2
        return 2
    fi
    return 0
}

# ── leg 1: engagement ─────────────────────────────────────────────────────
if run_leg engaged; then
    line=$(grep -m1 "ImGui engaged" "$OUT_DIR/engaged.log")
    calls=$(sed -n 's/.*engaged (\([0-9]\+\) calls).*/\1/p' <<< "$line")
    if [[ -n "${calls:-}" && "$calls" -gt 0 ]]; then
        verdict engaged PASS "$calls call(s):$(sed 's/.*\] //' <<< "$line")"
    else
        verdict engaged FAIL "no 'ImGui engaged (N calls)' with N > 0 in $OUT_DIR/engaged.log"
    fi
else
    verdict engaged FAIL "session did not produce a usable log"
fi

# ── leg 2: the error bracket and the three-strike refusal ─────────────────
if run_leg bracket break_end=true; then
    err=$(grep -m1 "ImGui error inside the mod's UI section" "$OUT_DIR/bracket.log")
    ref=$(grep -m1 "UI section refused for this session" "$OUT_DIR/bracket.log")
    n_err=$(grep -c "ImGui error inside the mod's UI section" "$OUT_DIR/bracket.log")
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

# ── legs 4-6: always-on sections ──────────────────────────────────────────
# The fixture's section carries OPENPETE_MOD_UI_ALWAYS, so its body runs on
# every present whatever the overlay is doing. It reports, once a second,
# the presents in which the always window's button was hovered; the last
# such line is the run's tally.

engaged_calls() {  # engaged_calls LOG -> N, empty when the line is absent
    sed -n 's/.*ImGui engaged (\([0-9]\+\) calls).*/\1/p' "$1" | head -1
}
hover_tally() {    # hover_tally LOG -> "N M" from the last hovered line
    sed -n 's/.*imgui-panel: hovered \([0-9]\+\)\/\([0-9]\+\) presents.*/\1 \2/p' \
        "$1" | tail -1
}

# ── leg 4: the body runs with the overlay closed, and takes no input ───────
if LEG_OPEN=0 LEG_INJECT="$HOVER_XY" run_leg closed; then
    calls=$(engaged_calls "$OUT_DIR/closed.log")
    read -r hov pres <<< "$(hover_tally "$OUT_DIR/closed.log")"
    if [[ -z "${calls:-}" || "$calls" -le 0 ]]; then
        verdict closed FAIL "no 'ImGui engaged (N calls)' with N > 0 without --devui-open; see $OUT_DIR/closed.log"
    elif [[ -z "${pres:-}" || "$pres" -le 0 ]]; then
        verdict closed FAIL "the always body never reported a present count; see $OUT_DIR/closed.log"
    elif [[ "$hov" -ne 0 ]]; then
        verdict closed FAIL "always window took the mouse with the overlay closed: hovered $hov/$pres presents"
    else
        verdict closed PASS "$calls call(s) with the overlay never opened, hovered $hov/$pres presents"
    fi
else
    verdict closed FAIL "session did not produce a usable log"
fi

# ── leg 5: the same cursor, overlay open - the positive control ───────────
if LEG_INJECT="$HOVER_XY" run_leg hover; then
    read -r hov pres <<< "$(hover_tally "$OUT_DIR/hover.log")"
    if [[ -n "${hov:-}" && "$hov" -gt 0 ]]; then
        verdict hover PASS "hovered $hov/$pres presents with the overlay open"
    else
        verdict hover FAIL "the parked cursor ($HOVER_XY) never hovered the always window's button with the overlay open (saw ${hov:-none}/${pres:-none}); the closed leg's zero would be vacuous - see $OUT_DIR/hover.log"
    fi
else
    verdict hover FAIL "session did not produce a usable log"
fi

# ── leg 6: an always window survives a COMPOSITE postfx chain ─────────────
if [[ -d "$OUT_DIR/mods/crt-beans" ]]; then
    if LEG_OPEN=0 LEG_MODS="imgui-panel,crt-beans" run_leg composite; then
        calls=$(engaged_calls "$OUT_DIR/composite.log")
        if [[ -n "${calls:-}" && "$calls" -gt 0 ]]; then
            verdict composite PASS "$calls call(s) under a COMPOSITE chain (post-chain render site)"
        else
            verdict composite FAIL "no 'ImGui engaged (N calls)' with crt-beans enabled; see $OUT_DIR/composite.log"
        fi
    else
        verdict composite FAIL "session did not produce a usable log"
    fi
else
    verdict composite FAIL "missing $COMPOSITE_MOD (the COMPOSITE chain leg has nothing to enable)"
fi

if [[ $fails -eq 0 ]]; then
    echo "[mod-imgui] verdict: PASS (6/6 legs)"
    [[ $KEEP == 1 ]] || rm -rf "$OUT_DIR"
    exit 0
fi
echo "[mod-imgui] verdict: FAIL ($fails/6 legs); logs kept under $OUT_DIR"
exit 1
