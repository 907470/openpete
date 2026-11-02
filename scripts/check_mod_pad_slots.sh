#!/usr/bin/env bash
# check_mod_pad_slots.sh - api->pad_read against the synthetic pad stream.
#
# The mods/pad-probe fixture reads slots 0-3 once per tick and recomputes,
# from each record's own VBL, the bytes the engine's synthetic pad stream
# defines for that VBL and slot. The formula exists twice - once in
# core/runtime/host_input_tick.c, once in the mod, which links no engine symbol - so a
# mismatch means the record a mod reads is not the record the log carries.
# The expected slot set is the fixture's `expect` config row, written per
# leg into the leg's own copy of the mod directory.
#
# Legs:
#   a  synth      OPENPETE_SYNTH_PADS=1: slots 1-3 checked every tick.
#   b  runahead   the same under --runahead 2: speculative ticks re-execute
#                 on the last committed record and must carry ITS bytes.
#   c  rewind     --rewind-hold: the burst restages every slot from the log.
#   d  movie      a recorded two-player movie replayed under
#                 OPENPETE_SYNTH_PADS=verify, which stages nothing
#                 synthetic: slot 1 comes off disk and is checked against
#                 the stream it was recorded from.
#   e  negative   the same replay against a ONE-player movie: slot 1 is
#                 absent on every tick and every check must fail. Without
#                 it a comparator that only ever agrees with its own
#                 formula would report the same PASS.
#   f  rumble     OPENPETE_INPUT_VGAMEPAD=2 with the probe driving
#                 pad_rumble on slot 1: the per-pad rumble recorders must
#                 show the published pair arriving on virtual pad 1 and
#                 nothing on pad 0, and slot 0 must be refused.
#   g  no device  the same with no virtual pad: the calls are accepted and
#                 no motor command is issued, because no device holds the
#                 slot.
#   h  strength   leg f with the vibration strength at 0: commands still
#                 issue and every one of them is zero, so a mod cannot
#                 drive motors past the user's setting.
#
# Leg b's window is idle attract-loop gameplay, where the runahead loop
# speculates but no speculation is ever adopted into the presented
# timeline; the leg proves what a speculative tick reads, not what
# adoption does with it. Its engagement is measured against leg a rather
# than against zero, because a tick hook can run twice inside one staged
# record during a transition, which the probe also counts as a repeat.
#
# Usage: scripts/check_mod_pad_slots.sh [--game SLUG]   (repo root)
# Env:   OPENPETE_BIN / OPENPETE_DISC - forwarded through scripts/lib/game.sh.
# Exit:  0 all legs PASS, 1 any leg FAIL, 64 wrong game.

set -u

cd "$(dirname "$0")/.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"
[ "$PG_GAME" = "spyro-1" ] || {
    echo "[pad-slots] SKIP: the fixture and level names are spyro-1's," \
         "not $PG_GAME's"; exit 64; }

WORK="$(mktemp -d /tmp/openpete-padslots.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# The synthetic legs need > 2000 checked ticks per slot. --skip-to-level
# spends about 1580 VBLs loading before the first tick hook runs, and one
# tick is two VBLs, so 6500 VBLs leave about 2450 ticks.
FRAMES=6500
REPORT_EVERY=100
REWIND_FRAMES=4000
RUMBLE_FRAMES=2500
REWIND_AT=2500
REWIND_ITERS=40

fail=0
say() { printf '\n[pad-slots] %s\n' "$*"; }
ok()  { printf '[pad-slots]   PASS: %s\n' "$*"; }
bad() { printf '[pad-slots]   FAIL: %s\n' "$*"; fail=1; }

# A leg's own copy of the fixture, so its config.toml cannot reach another
# leg or the checked-in mod directory.
fixture() { # fixture <dir> <expect-set> [rumble-slot]
    local dir="$1" expect="$2" rumble="${3:-}"
    rm -rf "$dir"
    mkdir -p "$dir"
    cp -r mods/pad-probe "$dir/pad-probe"
    rm -rf "$dir/pad-probe/.build"
    printf 'expect = "%s"\nreport_every = %d\n' "$expect" "$REPORT_EVERY" \
        > "$dir/pad-probe/config.toml"
    [ -n "$rumble" ] &&
        printf 'rumble = true\nrumble_slot = %s\n' "$rumble" \
            >> "$dir/pad-probe/config.toml"
    echo pad-probe > "$dir/enabled.txt"
}

# The last report line's field for a slot. The probe logs its tallies every
# REPORT_EVERY ticks because the mod API has no shutdown callback, so the
# last line of a run carries its final counts.
slot_field() { # slot_field <log> <slot> <field>
    local v
    v="$(sed -n "s/.*pad-probe: slot $2:.*[ :]$3=\([0-9]*\).*/\1/p" "$1" | tail -1)"
    echo "${v:-0}"
}
run_field() { # run_field <log> <field>
    local v
    v="$(sed -n "s/.*pad-probe: ticks=.* $2=\([0-9]*\).*/\1/p" "$1" | tail -1)"
    echo "${v:-0}"
}

# One headless run. pg_capture deletes the log and every named output
# before the run and asserts them after, so no leg can read the previous
# leg's capture.
capture() { # capture <log> <outputs> <env-assignments...> -- <run args...>
    local log="$1" outs="$2"; shift 2
    local -a env_arr=()
    while [ "$1" != "--" ]; do env_arr+=("$1"); shift; done
    shift
    local -a PG_CAPTURE_ENV=("${env_arr[@]}")
    local PG_CAPTURE_TIMEOUT=600
    pg_capture "$log" "$outs" -- "$@"
}

# Every slot of a set: present, checked at least `floor` times, never
# mismatched.
check_slots() { # check_slots <log> <label> <floor> <slot>...
    local log="$1" label="$2" floor="$3"; shift 3
    local sl checks mism spec_mism libpad
    for sl in "$@"; do
        checks="$(slot_field "$log" "$sl" checks)"
        mism="$(slot_field "$log" "$sl" mismatches)"
        spec_mism="$(slot_field "$log" "$sl" spec_mismatches)"
        libpad="$(slot_field "$log" "$sl" libpad_bad)"
        if [ "$checks" -ge "$floor" ] && [ "$mism" -eq 0 ] &&
           [ "$spec_mism" -eq 0 ] && [ "$libpad" -eq 0 ]; then
            ok "$label slot $sl: checks=$checks mismatches=0 spec_mismatches=0 libpad_bad=0"
        else
            bad "$label slot $sl: checks=$checks (floor $floor) mismatches=$mism" \
                "spec_mismatches=$spec_mism libpad_bad=$libpad"
        fi
    done
}

SYNTH_ARGS=(--mute 1 --no-card --skip-to-level artisans)

# ── a. synthetic stream, live staging ──────────────────────────────────
say "leg a: OPENPETE_SYNTH_PADS=1, $FRAMES VBLs on artisans"
fixture "$WORK/a" "1,2,3"
capture "$WORK/a.log" "" OPENPETE_SYNTH_PADS=1 -- \
    "${SYNTH_ARGS[@]}" --mods-dir "$WORK/a" --frame-limit $FRAMES
check_slots "$WORK/a.log" "leg a" 2000 1 2 3
a_reads0="$(slot_field "$WORK/a.log" 0 reads)"
a_present0="$(slot_field "$WORK/a.log" 0 present)"
if [ "$a_reads0" -gt 0 ] && [ "$a_present0" -eq "$a_reads0" ]; then
    ok "leg a slot 0: present on all $a_reads0 reads"
else
    bad "leg a slot 0: reads=$a_reads0 present=$a_present0"
fi
a_repeat="$(run_field "$WORK/a.log" repeat)"

# ── b. the same under runahead ─────────────────────────────────────────
say "leg b: leg a under --runahead 2"
fixture "$WORK/b" "1,2,3"
capture "$WORK/b.log" "" OPENPETE_SYNTH_PADS=1 -- \
    "${SYNTH_ARGS[@]}" --mods-dir "$WORK/b" --runahead 2 --frame-limit $FRAMES
check_slots "$WORK/b.log" "leg b" 2000 1 2 3
b_repeat="$(run_field "$WORK/b.log" repeat)"
b_spec="$(slot_field "$WORK/b.log" 1 spec_checks)"
if grep -q "runahead: armed" "$WORK/b.log"; then
    ok "leg b: runahead armed"
else
    bad "leg b: runahead never armed"
fi
if [ "$b_spec" -ge 1 ] && [ "$b_repeat" -gt "$a_repeat" ]; then
    ok "leg b: speculative reads $b_spec, repeats $b_repeat vs $a_repeat without runahead"
else
    bad "leg b: speculative reads $b_spec, repeats $b_repeat vs $a_repeat without runahead (no engagement)"
fi

# ── c. the rewind burst ────────────────────────────────────────────────
say "leg c: --rewind-hold $REWIND_AT:$REWIND_ITERS on level 10"
fixture "$WORK/c" "1,2,3"
capture "$WORK/c.log" "" OPENPETE_SYNTH_PADS=1 -- \
    --mute 1 --no-card --skip-to-level 10 --mods-dir "$WORK/c" \
    --rewind 30 --rewind-hold "$REWIND_AT:$REWIND_ITERS" \
    --frame-limit $REWIND_FRAMES
check_slots "$WORK/c.log" "leg c" 1 1 2 3
c_live="$(grep -c "rewind: hold .* (live=1 " "$WORK/c.log")"
c_back="$(run_field "$WORK/c.log" backward)"
if [ "$c_live" -gt 0 ] && [ "$c_back" -gt 0 ]; then
    ok "leg c: $c_live live rewind iteration(s), $c_back backward re-anchor(s) read"
else
    bad "leg c: live rewind iterations=$c_live backward reads=$c_back (no engagement)"
fi

# ── d. a recorded two-player movie, replayed under verify ──────────────
say "leg d: record a two-player movie of the synthetic stream, replay it"
capture "$WORK/rec2.log" "$WORK/two.bk2" OPENPETE_SYNTH_PADS=1 -- \
    "${SYNTH_ARGS[@]}" --bk2-record "$WORK/two.bk2" --frame-limit $FRAMES
fixture "$WORK/d" "1"
capture "$WORK/d.log" "" OPENPETE_SYNTH_PADS=verify -- \
    --mute 1 --no-card --mods-dir "$WORK/d" \
    --bk2 "$WORK/two.bk2" --bk2-frame-offset 0 --frame-limit $FRAMES
check_slots "$WORK/d.log" "leg d" 2000 1
d_present1="$(slot_field "$WORK/d.log" 1 present)"
if [ "$d_present1" -gt 0 ]; then
    ok "leg d: slot 1 present on $d_present1 read(s) off the movie"
else
    bad "leg d: slot 1 never present - the movie carried no second player"
fi

# ── e. negative control: the same replay against a one-player movie ────
say "leg e: negative control - one-player movie, slot 1 still expected"
capture "$WORK/rec1.log" "$WORK/one.bk2" OPENPETE_SYNTH_PADS=0 -- \
    "${SYNTH_ARGS[@]}" --bk2-record "$WORK/one.bk2" --frame-limit $FRAMES
fixture "$WORK/e" "1"
capture "$WORK/e.log" "" OPENPETE_SYNTH_PADS=verify -- \
    --mute 1 --no-card --mods-dir "$WORK/e" \
    --bk2 "$WORK/one.bk2" --bk2-frame-offset 0 --frame-limit $FRAMES
e_checks="$(slot_field "$WORK/e.log" 1 checks)"
e_mism="$(slot_field "$WORK/e.log" 1 mismatches)"
e_present="$(slot_field "$WORK/e.log" 1 present)"
if [ "$e_checks" -gt 0 ] && [ "$e_mism" -eq "$e_checks" ] && [ "$e_present" -eq 0 ]; then
    ok "leg e: slot 1 absent on all $e_checks check(s), every one a mismatch"
else
    bad "leg e: checks=$e_checks mismatches=$e_mism present=$e_present" \
        "(the comparator did not fail on an absent expected slot)"
fi

# ── rumble legs ────────────────────────────────────────────────────────
#
# rumble-guard: intentional - leg f measures motor commands arriving at a
# device, so it is the one leg here that must NOT silence vibration. The only
# device reachable from it is the virtual pad the run attaches itself: the
# actuator path lifts its headless and focus refusals for exactly the ids in
# its test-device set and re-checks each against the handle actually driven,
# so a connected real controller keeps the normal treatment and stays
# silent. The other legs go through pg_capture, which silences vibration for
# every run it starts.
#
# These legs do not use pg_capture for the same reason, so they delete their
# own log before the run and assert it after.
rumble_run() { # rumble_run <log> <env-assignments...> -- <run args...>
    local log="$1"; shift
    local -a env_arr=()
    while [ "$1" != "--" ]; do env_arr+=("$1"); shift; done
    shift
    rm -f "$log"
    ( env OPENPETE_VIBRATION=10 "${env_arr[@]}" timeout 600 \
        "$PG_BIN" --headless --no-config --disc "$PG_DISC" --disc-cache 0 \
        "${PG_RUN_ARGS[@]}" "$@" ) > "$log" 2>&1 || true
    [ -s "$log" ] || { bad "run produced no log at $log"; return 1; }
    return 0
}

# One integer out of a "recorder: pad N ..." or "mod slot N ..." census line.
census() { # census <log> <sed-expression-with-one-capture>
    local v
    v="$(sed -n "s/.*$2.*/\1/p" "$1" | tail -1)"
    echo "${v:-0}"
}

RUMBLE_ARGS=(--mute 1 --no-card --skip-to-level artisans
             --frame-limit $RUMBLE_FRAMES)

say "leg f: OPENPETE_INPUT_VGAMEPAD=2, probe drives pad_rumble on slot 1"
fixture "$WORK/f" "" 1
rumble_run "$WORK/f.log" OPENPETE_INPUT_VGAMEPAD=2 -- \
    --mods-dir "$WORK/f" "${RUMBLE_ARGS[@]}"
f_live0="$(census "$WORK/f.log" 'recorder: pad 0 live=\([0-9]*\)')"
f_live1="$(census "$WORK/f.log" 'recorder: pad 1 live=\([0-9]*\)')"
f_nz0="$(census "$WORK/f.log" 'recorder: pad 0 live=[0-9]*, [0-9]* callback(s), \([0-9]*\) non-zero')"
f_nz1="$(census "$WORK/f.log" 'recorder: pad 1 live=[0-9]*, [0-9]* callback(s), \([0-9]*\) non-zero')"
f_cmds="$(census "$WORK/f.log" 'mod slot 1: \([0-9]*\) motor command')"
f_pair="$(sed -n 's/.*mod slot 1: [0-9]* motor command(s), pair {low \([0-9]*\), high \([0-9]*\)}.*/\1,\2/p' "$WORK/f.log" | tail -1)"
f_last1="$(sed -n 's/.*recorder: pad 1 .*last pair {low \([0-9]*\), high \([0-9]*\)}.*/\1,\2/p' "$WORK/f.log" | tail -1)"
f_slot0rc="$(sed -n 's/.*pad-probe: rumble: .* slot0_rc=\(-\?[0-9]*\).*/\1/p' "$WORK/f.log" | tail -1)"
f_ok="$(sed -n 's/.*pad-probe: rumble: .* ok=\([0-9]*\).*/\1/p' "$WORK/f.log" | tail -1)"

# Harness health before any count is read: a recorder that never saw its own
# self-probe reports the same zeros as a silent run.
if [ "$f_live0" = "1" ] && [ "$f_live1" = "1" ]; then
    ok "leg f: both virtual pad recorders live (self-probe ok)"
else
    bad "leg f: recorder live pad0=$f_live0 pad1=$f_live1 - counts below prove nothing"
fi
if [ "${f_ok:-0}" -ge 1 ] && [ "$f_cmds" -ge 1 ]; then
    ok "leg f: ${f_ok:-0} accepted pad_rumble call(s), $f_cmds motor command(s) on slot 1"
else
    bad "leg f: accepted calls=${f_ok:-0} slot-1 motor commands=$f_cmds (no engagement)"
fi
if [ "$f_nz1" -ge 1 ] && [ "$f_nz0" -eq 0 ]; then
    ok "leg f: $f_nz1 non-zero command(s) on virtual pad 1, 0 on pad 0"
else
    bad "leg f: non-zero commands pad1=$f_nz1 pad0=$f_nz0 (wrong device, or none)"
fi
if [ -n "$f_pair" ] && [ "$f_pair" = "$f_last1" ]; then
    ok "leg f: pad 1 received the published pair {low,high}=$f_pair"
else
    bad "leg f: published pair '$f_pair' but pad 1 last received '$f_last1'"
fi
if [ "${f_slot0rc:-0}" = "-1" ]; then
    ok "leg f: pad_rumble(slot 0) refused (-1)"
else
    bad "leg f: pad_rumble(slot 0) returned '${f_slot0rc:-none}', expected -1"
fi

say "leg g: the same with no test device attached"
fixture "$WORK/g" "" 1
rumble_run "$WORK/g.log" OPENPETE_INPUT_VGAMEPAD=0 -- \
    --mods-dir "$WORK/g" "${RUMBLE_ARGS[@]}"
g_cmds="$(census "$WORK/g.log" 'mod slot 1: \([0-9]*\) motor command')"
g_ok="$(sed -n 's/.*pad-probe: rumble: .* ok=\([0-9]*\).*/\1/p' "$WORK/g.log" | tail -1)"
if [ "${g_ok:-0}" -ge 1 ] && [ "$g_cmds" -eq 0 ]; then
    ok "leg g: ${g_ok:-0} accepted call(s), 0 motor commands - no device, nothing driven"
else
    bad "leg g: accepted calls=${g_ok:-0} motor commands=$g_cmds (expected calls, no commands)"
fi

say "leg h: leg f with the vibration strength at 0"
fixture "$WORK/h" "" 1
rm -f "$WORK/h.log"
( env OPENPETE_VIBRATION=0 OPENPETE_INPUT_VGAMEPAD=2 timeout 600 \
    "$PG_BIN" --headless --no-config --disc "$PG_DISC" --disc-cache 0 \
    "${PG_RUN_ARGS[@]}" --mods-dir "$WORK/h" "${RUMBLE_ARGS[@]}" ) \
    > "$WORK/h.log" 2>&1 || true
[ -s "$WORK/h.log" ] || bad "leg h produced no log"
h_nz1="$(census "$WORK/h.log" 'recorder: pad 1 live=[0-9]*, [0-9]* callback(s), \([0-9]*\) non-zero')"
h_cmds="$(census "$WORK/h.log" 'mod slot 1: \([0-9]*\) motor command')"
if [ "$h_cmds" -ge 1 ] && [ "$h_nz1" -eq 0 ]; then
    ok "leg h: $h_cmds command(s) issued, 0 non-zero - the strength setting is not bypassable"
else
    bad "leg h: commands=$h_cmds non-zero on pad 1=$h_nz1 (strength 0 did not silence)"
fi

say "logs under $WORK (removed on exit); copy them out before rerunning"
if [ $fail -eq 0 ]; then
    echo "[pad-slots] verdict: PASS"
else
    echo "[pad-slots] verdict: FAIL"
fi
exit $fail
