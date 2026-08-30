#!/usr/bin/env bash

# scripts/check_rumble.sh — the controller-vibration gate.
#
# Vibration has no pixels and no gameplay-state bytes, so the presentation
# golden, the oracle and the determinism batteries are all structurally blind
# to it. Reading them harder proves nothing; the instrument has to be built
# in-binary. This is it.
#
# HOW IT SEES ANYTHING. SDL's virtual joystick descriptor carries a Rumble
# callback, so OPENPETE_INPUT_VGAMEPAD attaches a pad that records every motor
# command delivered to it. That covers the whole chain — guest actuator write ->
# libpad registration -> per-tick sample -> pump-side apply -> SDL -> device —
# with no hardware and no window.
#
# WHY IT CANNOT REPORT A VACUOUS PASS. Three separate zeros would otherwise all
# look alike, and each is guarded:
#
#   1. A recorder that was never wired records nothing, exactly like a run
#      where nothing rumbled. So the binary self-probes at attach: one
#      deliberate non-zero pair, asserted to arrive. `live=1` below is that
#      probe, and no count is believed without it.
#   2. A virtual pad SDL declines to expose as a gamepad never reaches decode
#      or the actuator path. That is a harness FAILURE, not a quiet zero, and
#      the run's own log says so.
#   3. A gate that passes whatever the code does proves nothing about the code.
#      So this runs BOTH sides of a one-variable A/B: with the actuator hook
#      installed (expect motor commands) and with OPENPETE_RUMBLE_LEGACY=1,
#      which restores PsyCross's one-shot PadSetAct and nothing else (expect
#      none). A gate that cannot fail is not a gate.
#
# The assertions are engagement invariants, never an exact command count. Two
# independent reasons, both measured rather than assumed:
#
#   - SDL injects traffic this script does not control (a resend every
#     SDL_RUMBLE_RESEND_MS, a (0,0) at duration expiry) and hides repeats via
#     dedup. A 4000-VBL leg issued 3999 apply calls and produced 4 driver
#     callbacks; the ratio is SDL's business, not this gate's.
#   - The virtual pad is not gameplay-neutral. Measured on the same movie and
#     the same frame limit, a plain run reaches four NORMAL-rumble pulses and a
#     vgamepad run reaches one. So the movie's rumble events are NOT a fixed
#     quantity across the two configurations, and any assertion counting them
#     would be pinning down a number that legitimately moves.
#
# Hence: "at least one non-zero motor command in the fixed leg, none in the
# legacy leg". That is enough to separate a working actuator path from a broken
# one, which is the only question this gate is entitled to answer.
#
# Usage: scripts/check_rumble.sh [bk2-name]
# Exit:  0 pass, 1 fail, 64 skip (no binary / no disc).

set -euo pipefail

# rumble-guard: intentional — this gate's whole purpose is to make the motors
# fire, and the only device that can receive them is the virtual pad it
# attaches itself. No human's controller is reachable from here: the actuator
# path lifts its headless/focus refusals for that one device id and re-checks
# it against the handle actually driven, so a real pad occupying slot 0 gets
# the normal treatment and stays silent.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BIN="${OPENPETE_BIN:-build/linux-release/games/spyro-1/openpete-spyro1}"
DISC="${OPENPETE_DISC:-games/spyro-1/spyro-1/build/SCUS-94228.cue}"

# gnasty_gnorc_attack drives the NORMAL rumble ({1, 120}: both motors) four
# separate times, which is the effect most of the game uses. Movies that only
# trigger the custom amount exercise one motor.
MOVIE="${1:-gnasty_gnorc_attack}"
BK2="games/spyro-1/bk2/${MOVIE}.bk2"

# A frame limit is mandatory, not a speed optimization: the game does not stop
# when a movie's input runs out, it keeps playing with a neutral pad. Left
# unbounded a leg runs until something kills it, which is both slow and a lie —
# whatever it eventually reports would cover an arbitrary amount of gameplay
# nobody chose. 4000 VBLs clears the first rumble events of this movie (the
# earliest lands near VBL 1800) with room to spare.
FRAMES="${OPENPETE_RUMBLE_FRAMES:-4000}"

[[ -x "$BIN"  ]] || { echo "[rumble] SKIP: no binary at $BIN"; exit 64; }
[[ -f "$DISC" ]] || { echo "[rumble] SKIP: no disc at $DISC"; exit 64; }
[[ -f "$BK2"  ]] || { echo "[rumble] SKIP: no movie at $BK2"; exit 64; }

OUT="${TMPDIR:-/tmp}/openpete-rumble-check"
rm -rf "$OUT"; mkdir -p "$OUT"

# A bk2 is not optional. The virtual pad drives a frame-indexed button sweep of
# its own, which without a movie to displace it would steer Spyro and make the
# run mean nothing.
run_leg() {   # $1 = leg name, $2... = extra env assignments
    local leg="$1"; shift
    local log="$OUT/$leg.log"
    ( env OPENPETE_INPUT_VGAMEPAD=1 OPENPETE_RUMBLE_LOG=1 "$@" \
        "$BIN" --headless --disc "$DISC" --bk2 "$BK2" \
        --frame-limit "$FRAMES" ) > "$log" 2>&1 || true
    [[ -s "$log" ]] || { echo "[rumble] FAIL: $leg produced no output"; exit 1; }
    echo "$log"
}

# Pull one integer out of the census line the run prints at shutdown.
field() {     # $1 = log, $2 = sed capture on the recorder line
    sed -n "s/.*$2.*/\1/p" "$1" | tail -1
}

echo "[rumble] movie: $MOVIE, $FRAMES VBLs per leg"

FIXED_LOG="$(run_leg fixed)"
LEGACY_LOG="$(run_leg legacy OPENPETE_RUMBLE_LEGACY=1)"

fail=0
note() { echo "[rumble] $*"; }

for leg in fixed legacy; do
    log="$OUT/$leg.log"

    # Harness health first. A pad SDL will not expose as a gamepad never
    # reaches the actuator path, so every count below would be a zero about
    # the harness rather than about the code.
    if grep -q "does not expose it as a gamepad" "$log"; then
        note "FAIL ($leg): SDL would not expose the virtual pad as a gamepad —"
        note "            this platform cannot host the oracle; not a code result"
        fail=1; continue
    fi

    live="$(field "$log" 'recorder: live=\([0-9]*\)')"
    calls="$(field "$log" 'recorder: live=[0-9]*, \([0-9]*\) callback')"
    nonzero="$(field "$log" 'recorder: .*, \([0-9]*\) non-zero')"
    : "${live:=0}" "${calls:=0}" "${nonzero:=0}"

    if [[ "$live" != "1" ]]; then
        note "FAIL ($leg): the rumble recorder never saw its own self-probe."
        note "            Every motor count from this leg is meaningless — a"
        note "            zero here reads the same as a recorder that was"
        note "            never wired, which is the failure this gate exists"
        note "            to refuse."
        fail=1; continue
    fi

    note "$leg: recorder live, $calls callback(s), $nonzero non-zero"
done

[[ $fail -eq 0 ]] || exit 1

FIXED_NZ="$(field "$FIXED_LOG" 'recorder: .*, \([0-9]*\) non-zero')"
LEGACY_NZ="$(field "$LEGACY_LOG" 'recorder: .*, \([0-9]*\) non-zero')"
: "${FIXED_NZ:=0}" "${LEGACY_NZ:=0}"

# The diagnosis itself, re-asserted every run: libpad's PadSetAct is a
# registration, so every call must arrive carrying {0,0}. A non-zero
# registration would mean the game filled its buffer before handing it over,
# and the one-shot reading of PadSetAct was not the whole story after all.
REG_NZ="$(field "$FIXED_LOG" 'census: PadSetAct [0-9]* call(s), \([0-9]*\) carrying')"
: "${REG_NZ:=0}"
if [[ "$REG_NZ" != "0" ]]; then
    note "FAIL: $REG_NZ PadSetAct call(s) carried non-zero actuator bytes."
    note "      The root-cause model says every registration is {0,0}; this"
    note "      run disagrees with it, so the fix rests on a wrong premise."
    exit 1
fi

# Engagement. A run where the game never commanded a motor proves nothing
# about whether motors work, so it is VACUOUS rather than a pass.
if [[ "$FIXED_NZ" -eq 0 ]]; then
    note "FAIL (vacuous): the fixed leg delivered no non-zero motor command."
    note "      Either the actuator path is broken, or '$MOVIE' never triggers"
    note "      vibration — check the 'actuator bytes' lines in $FIXED_LOG"
    note "      before believing the first of those."
    exit 1
fi

# The other side of the A/B. Without this the gate would pass a build where
# the fix had been reverted, because it would only ever have looked at one leg.
if [[ "$LEGACY_NZ" -ne 0 ]]; then
    note "FAIL: the legacy (one-shot PadSetAct) leg delivered $LEGACY_NZ motor"
    note "      command(s). It is supposed to deliver none — that is the bug"
    note "      this work fixed. Either the A/B knob no longer isolates the"
    note "      hook, or the gate is measuring something other than the fix."
    exit 1
fi

note "PASS: fixed leg delivered $FIXED_NZ non-zero motor command(s);"
note "      legacy leg delivered none, so the gate can tell the two apart."
exit 0
