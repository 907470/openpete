#!/usr/bin/env bash
# check_modsave.sh - the per-mod save-record harness (Spyro 1).
#
# Replays the two card recordings under the save-record fixtures
# (mods/modsave-fixture-a: portal-randomizer's PRND pad format,
# mods/modsave-fixture-b: spyro-japanese-extras' JPE1 format,
# mods/modsave-probe: the stock GamestateUpdate counter, listed first so it
# is the innermost override, mods/modsave-driver: fixture-driven saves and
# loads) and asserts the card each leg leaves and the probe's count:
#
#   save legs   games/spyro-1/bk2/fairy-save.bk2 on a blank provisioned
#               card (New Game creates the file, the fairy saves record 0);
#   load legs   games/spyro-1/bk2/load-title.bk2 on the card a save leg
#               left, chained: the no-mod save leg's card is first asserted
#               equal to load-title.bk2.mcr in every record byte;
#   repair legs the no-mod card with record 1 or 2 corrupted: the title
#               retries, repairs without a write and still loads record 0;
#   driver leg  modsave-driver saves, loads and checksum-fails record 1
#               through the MemCardWriteFile / SaveLoad path mid-game.
#
# Pass criteria: each leg's card and log facts below, and the probe's last
# count equal to the no-mod run of the same recording (the stock update ran
# on every tick), except the freeze legs, whose count must fall short.
#
# Without the save-record api (the SDK header defines no
# OPENPETE_MOD_SAVE_API) the fixtures run their pad path and the load legs
# are the pre-fix record: fixture A freezes the game on a pad it cannot
# decode (exit 2 from the hang watchdog), fixture B locks. With the api the
# same pair forced onto the pad path is the legacy-collision row: A no
# longer carries the freeze path, so the leg must lock without a freeze. The
# api legs need the engine's save records; without the api each is reported
# SKIP with the engine piece it waits on, and with the api a leg this
# script does not yet implement is reported FAIL (not built), never PASS.
#
# Usage: tests/harness/check_modsave.sh [--section record|repair|driver|api|calibrate|all]
#        (repo root or anywhere; default all except calibrate)
#   --calibrate   seeded faults: each must make its check fail (LAW §13)
# Env:   OPENPETE_BIN / OPENPETE_DISC as for every pg_capture harness;
#        MODSAVE_WORK=<dir> keeps the run directories there (default: a
#        temporary directory removed at exit).
# Exit:  0 every run leg passed, 1 a leg failed, 64 bad usage or no build.

set -u
export OPENPETE_VIBRATION=0

cd "$(dirname "$0")/../.."
REPO="$PWD"
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh
pg_resolve spyro-1

SECTION="default"
for a in "$@"; do
    case "$a" in
        --section=*) SECTION="${a#--section=}" ;;
        --calibrate) SECTION="calibrate" ;;
        --section) ;;
        record|repair|driver|api|calibrate|all) SECTION="$a" ;;
        *) echo "check_modsave: unknown argument $a" >&2; exit 64 ;;
    esac
done
case "$SECTION" in default|record|repair|driver|api|calibrate|all) ;;
    *) echo "check_modsave: unknown section $SECTION" >&2; exit 64 ;; esac
on() { [ "$SECTION" = "$1" ] || { [ "$SECTION" = default ] && [ "$1" != calibrate ]; } ||
       { [ "$SECTION" = all ]; }; }

[ -x "$PG_BIN" ] || { echo "check_modsave: no binary at $PG_BIN" >&2; exit 64; }
SDK_HDR="$(dirname "$PG_BIN")/sdk/openpete_mod_api.h"
[ -f "$SDK_HDR" ] || { echo "check_modsave: no staged mod SDK at $SDK_HDR (build openpete_sdk_stage)" >&2; exit 64; }
HAVE_API=0
grep -q '#define OPENPETE_MOD_SAVE_API' "$SDK_HDR" && HAVE_API=1

BK2_DIR="$REPO/games/spyro-1/bk2"
CARD_PY="$REPO/tests/harness/modsave_card.py"
SAVE_FRAMES=3700
LOAD_FRAMES=4300

if [ -n "${MODSAVE_WORK:-}" ]; then
    WORK="$MODSAVE_WORK"; mkdir -p "$WORK"
else
    WORK="$(mktemp -d /tmp/openpete-modsave.XXXXXX)"
    trap 'rm -rf "${WORK:?}"' EXIT
fi

n_pass=0; n_fail=0; n_skip=0
say()  { printf '\n[modsave] %s\n' "$*"; }
pass() { printf '[modsave]   PASS: %s\n' "$*"; n_pass=$((n_pass + 1)); }
fail() { printf '[modsave]   FAIL: %s\n' "$*"; n_fail=$((n_fail + 1)); }
skip() { printf '[modsave]   SKIP: %s\n' "$*"; n_skip=$((n_skip + 1)); }
check() { # check <description> <command...>: PASS when the command exits 0
    local d="$1"; shift
    if "$@" >"$WORK/check.out" 2>&1; then pass "$d: $(tail -1 "$WORK/check.out")"
    else fail "$d: $(tail -1 "$WORK/check.out")"; fi
}

# leg <tag> <bk2: fairy|load> <card in or -> <frames> <mods...>
# Composes $WORK/<tag>/mods from copies of the fixtures (config.toml per
# fixture from CFG_A, CFG_B, CFG_DRV), enables <mods> in order, copies the
# input card to $WORK/<tag>/card1.mcr and runs the recording headless. The
# run directory is removed first; the log and the card are asserted after.
leg() {
    local tag="$1" bk="$2" card="$3" frames="$4"; shift 4
    local d="$WORK/$tag"
    rm -rf "${d:?}"; mkdir -p "$d/mods"
    : > "$d/mods/enabled.txt"
    local m
    for m in modsave-probe modsave-fixture-a modsave-fixture-b modsave-driver; do
        mkdir -p "$d/mods/$m"
        cp -r "$REPO/mods/$m/mod.toml" "$REPO/mods/$m/src" "$d/mods/$m/"
    done
    [ -n "${CFG_A:-}" ] && printf '%s\n' "$CFG_A" > "$d/mods/modsave-fixture-a/config.toml"
    [ -n "${CFG_B:-}" ] && printf '%s\n' "$CFG_B" > "$d/mods/modsave-fixture-b/config.toml"
    [ -n "${CFG_DRV:-}" ] && printf '%s\n' "$CFG_DRV" > "$d/mods/modsave-driver/config.toml"
    for m in "$@"; do echo "$m" >> "$d/mods/enabled.txt"; done
    [ "$card" != "-" ] && cp "$card" "$d/card1.mcr"
    local -a PG_CAPTURE_ENV=(OPENPETE_HANG_STALL_SECONDS=10)
    local PG_CAPTURE_TIMEOUT=300
    # pg_capture deletes the outputs it is given before the run, so the card
    # is named an output only when the leg starts without one.
    local outs=""
    [ "$card" = "-" ] && outs="$d/card1.mcr"
    pg_capture "$d/log.txt" "$outs" -- --mute 1 --mods-dir "$d/mods" \
        --bk2 "$BK2_DIR/$bk.bk2" --card1 "$d/card1.mcr" --no-card2 --no-log-file \
        --frame-limit "$frames"
    echo $? > "$d/rc"
    [ -f "$d/card1.mcr" ] || echo "check_modsave: leg $tag left no card" >&2
}
rc_of()    { cat "$WORK/$1/rc" 2>/dev/null || echo none; }
count_of() { # last stock count the probe logged, 0 when none
    local c
    c="$(grep -o 'modsave-probe: stock [0-9]*' "$WORK/$1/log.txt" 2>/dev/null | tail -1 | awk '{print $3}')"
    echo "${c:-0}"
}
has()      { grep -q -- "$2" "$WORK/$1/log.txt"; }
froze()    { [ "$(rc_of "$1")" = 2 ] && has "$1" 'watchdog: hang detected'; }
eq()       { [ "$1" = "$2" ] && echo "$1 == $2" || { echo "$1 != $2"; return 1; }; }
lt()       { [ "$1" -lt "$2" ] && echo "$1 < $2" || { echo "$1 >= $2"; return 1; }; }
not_froze(){ if froze "$1"; then echo "froze (exit 2, watchdog)"; return 1; fi; echo "exit $(rc_of "$1"), no hang"; }
yes_froze(){ if froze "$1"; then echo "exit 2, watchdog hang"; return 0; fi; echo "exit $(rc_of "$1"), no hang"; return 1; }
logged()   { if has "$1" "$2"; then echo "logged '$2'"; return 0; fi; echo "no '$2' line"; return 1; }
pads()     { # pads <tag> <r0> <r1> <r2>
    local t="$1" i=0 m ok=0; shift
    for m in "$@"; do python3 "$CARD_PY" pad-magic "$WORK/$t/card1.mcr" "$i" "$m" || ok=1; i=$((i + 1)); done
    return $ok
}

LEGACY_A='mode = "legacy"'
LEGACY_B='mode = "legacy"'
A=modsave-fixture-a; B=modsave-fixture-b; P=modsave-probe; DRV=modsave-driver

# ── reference legs: no mod but the probe ──────────────────────────────────
need_reference() {
    [ -f "$WORK/s0/card1.mcr" ] && return 0
    say "reference: fairy-save.bk2 and load-title.bk2 with the probe alone"
    leg s0 fairy-save - "$SAVE_FRAMES" "$P"
    check "s0 ran to the frame limit" eq "$(rc_of s0)" 0
    N_SAVE="$(count_of s0)"
    check "s0 probe engaged (stock updates counted)" lt 0 "$N_SAVE"
    check "s0 record 0 saved, records 1-2 empty" \
        bash -c "python3 '$CARD_PY' records '$WORK/s0/card1.mcr' | tee /dev/stderr | awk '{print \$4}' | tr '\n' ' ' | grep -q '^0 1 1 '"
    check "s0 card equals load-title.bk2.mcr in every record byte (file header icon excluded)" \
        python3 "$CARD_PY" same-records "$WORK/s0/card1.mcr" "$BK2_DIR/load-title.bk2.mcr" --pads
    leg l0 load-title "$WORK/s0/card1.mcr" "$LOAD_FRAMES" "$P"
    check "l0 ran to the frame limit" eq "$(rc_of l0)" 0
    N_LOAD="$(count_of l0)"
    check "l0 probe engaged" lt 0 "$N_LOAD"
    check "l0 card unchanged by a load" python3 "$CARD_PY" same "$WORK/s0/card1.mcr" "$WORK/l0/card1.mcr"
}
N_SAVE=0; N_LOAD=0

# ── record: the pad collision, pre-fix record or legacy-collision row ─────
section_record() {
    need_reference
    say "save legs (legacy pad path): the outermost fixture's pad reaches the card"
    CFG_A="$LEGACY_A" leg s_a fairy-save - "$SAVE_FRAMES" "$P" "$A"
    CFG_B="$LEGACY_B" leg s_b fairy-save - "$SAVE_FRAMES" "$P" "$B"
    CFG_A="$LEGACY_A" CFG_B="$LEGACY_B" leg s_ab fairy-save - "$SAVE_FRAMES" "$P" "$A" "$B"
    CFG_A="$LEGACY_A" CFG_B="$LEGACY_B" leg s_ba fairy-save - "$SAVE_FRAMES" "$P" "$B" "$A"
    CFG_A='mode = "baseline"' CFG_B='mode = "baseline"' leg s_base fairy-save - "$SAVE_FRAMES" "$P" "$A" "$B"
    local t
    for t in s_a s_b s_ab s_ba s_base; do
        check "$t replays the recording (probe count)" eq "$(count_of $t)" "$N_SAVE"
    done
    for t in s_a s_b s_ab s_ba; do
        check "$t card equals the no-mod card outside the pads" \
            python3 "$CARD_PY" same-outside-pads "$WORK/s0/card1.mcr" "$WORK/$t/card1.mcr"
    done
    check "s_a pads PRND / zero / zero" pads s_a PRND zero zero
    check "s_b pads JPE1 / JPE1 / JPE1 (B writes every record the title creates)" pads s_b JPE1 JPE1 JPE1
    check "s_ab (B outermost) pads JPE1 / JPE1 / JPE1" pads s_ab JPE1 JPE1 JPE1
    check "s_ba (A outermost) pads PRND / JPE1 / JPE1" pads s_ba PRND JPE1 JPE1
    check "s_base (baseline mode) card byte-identical to the no-mod card" \
        python3 "$CARD_PY" same "$WORK/s0/card1.mcr" "$WORK/s_base/card1.mcr"

    say "load legs, chained on the save legs' cards"
    CFG_A="$LEGACY_A" leg l_a_a load-title "$WORK/s_a/card1.mcr" "$LOAD_FRAMES" "$P" "$A"
    CFG_B="$LEGACY_B" leg l_b_b load-title "$WORK/s_b/card1.mcr" "$LOAD_FRAMES" "$P" "$B"
    CFG_A="$LEGACY_A" CFG_B="$LEGACY_B" leg l_0_ab load-title "$WORK/s0/card1.mcr" "$LOAD_FRAMES" "$P" "$A" "$B"
    CFG_A="$LEGACY_A" CFG_B="$LEGACY_B" leg l_ba_ba load-title "$WORK/s_ba/card1.mcr" "$LOAD_FRAMES" "$P" "$B" "$A"
    CFG_B="$LEGACY_B" leg l_a_b load-title "$WORK/s_a/card1.mcr" "$LOAD_FRAMES" "$P" "$B"
    CFG_A="$LEGACY_A" CFG_B="$LEGACY_B" leg l_ab_ab load-title "$WORK/s_ab/card1.mcr" "$LOAD_FRAMES" "$P" "$A" "$B"
    CFG_A="$LEGACY_A" leg l_b_a load-title "$WORK/s_b/card1.mcr" "$LOAD_FRAMES" "$P" "$A"
    check "l_a_a clean: A restores its own pad" logged l_a_a "$A: save_load gs 13 rc 1 pad own"
    check "l_a_a clean: probe count" eq "$(count_of l_a_a)" "$N_LOAD"
    check "l_b_b clean: B restores its own pad" logged l_b_b "$B: save_load gs 13 rc 1 pad own"
    check "l_b_b clean: probe count" eq "$(count_of l_b_b)" "$N_LOAD"
    check "l_0_ab clean: A reads a zero pad as legacy" logged l_0_ab "$A: save_load gs 13 rc 1 pad zero"
    check "l_0_ab clean: B reads a zero pad as legacy" logged l_0_ab "$B: save_load gs 13 rc 1 pad zero"
    check "l_0_ab clean: probe count" eq "$(count_of l_0_ab)" "$N_LOAD"
    check "l_ba_ba lock: B reads PRND as damaged" logged l_ba_ba "$B: pad damaged, record locked"
    check "l_ba_ba lock: probe count (no freeze)" eq "$(count_of l_ba_ba)" "$N_LOAD"
    check "l_a_b lock: B reads PRND as damaged" logged l_a_b "$B: pad damaged, record locked"
    check "l_a_b lock: probe count" eq "$(count_of l_a_b)" "$N_LOAD"
    for t in l_ab_ab l_b_a; do
        check "$t: A reads JPE1 as damaged" logged "$t" "$A: pad damaged, corrupt"
        if [ "$HAVE_API" = 0 ]; then
            check "$t pre-fix freeze: hang watchdog" yes_froze "$t"
            check "$t pre-fix freeze: stock updates stop short" lt "$(count_of "$t")" "$N_LOAD"
        else
            check "$t legacy collision: no freeze" not_froze "$t"
            check "$t legacy collision: probe count" eq "$(count_of "$t")" "$N_LOAD"
        fi
    done
    check "l_ab_ab: B reads its own pad (it wrote last)" logged l_ab_ab "$B: save_load gs 13 rc 1 pad own"
}

# ── repair: checksum-failed records 2 and 3 are repaired without a write ──
section_repair() {
    need_reference
    say "checksum repair: record 1 or 2 corrupted, the title still loads record 0"
    local i
    for i in 1 2; do
        python3 "$CARD_PY" flip "$WORK/s0/card1.mcr" "$WORK/repair$i.mcr" "$i" 0x20 >/dev/null
        check "repair$i input record $i fails its checksum (engagement)" \
            bash -c "python3 '$CARD_PY' records '$WORK/repair$i.mcr' | grep -q '^record $i .*checksum bad'"
        leg "r$i" load-title "$WORK/repair$i.mcr" "$LOAD_FRAMES" "$P"
        check "r$i loads record 0 and replays (probe count)" eq "$(count_of "r$i")" "$N_LOAD"
        check "r$i card unchanged (the repair writes nothing)" \
            python3 "$CARD_PY" same "$WORK/repair$i.mcr" "$WORK/r$i/card1.mcr"
    done
}

# ── driver: fixture-driven save, load and checksum-failed load ────────────
section_driver() {
    need_reference
    say "driver: save, load and a checksum-failed load of record 1 mid-game"
    CFG_A="$LEGACY_A" CFG_DRV='actions = "save:1@1600,load:1@1700,badload:1@1800"' \
        leg drv load-title "$WORK/s0/card1.mcr" "$LOAD_FRAMES" "$P" "$A" "$DRV"
    check "drv save reached the card" logged drv "$DRV: save 1 at 1600 rc 0"
    check "drv record 1 non-empty with a valid checksum and A's pad" \
        bash -c "python3 '$CARD_PY' records '$WORK/drv/card1.mcr' | grep -q '^record 1 empty 0 checksum ok pad PRND'"
    check "drv load: stock verdict passes, A restores its value" logged drv "$DRV: load 1 at 1700 rc 1"
    check "drv load: A decoded its own pad" logged drv "$A: save_load gs 0 rc 1 pad own"
    check "drv badload: stock verdict fails" logged drv "$DRV: badload 1 at 1800 rc 0"
    check "drv records 0 and 2 untouched" \
        bash -c "python3 '$CARD_PY' records '$WORK/drv/card1.mcr' | sed -n '1p;3p' | awk '{print \$10}' > '$WORK/drv.h'; python3 '$CARD_PY' records '$WORK/s0/card1.mcr' | sed -n '1p;3p' | awk '{print \$10}' | diff - '$WORK/drv.h'"
    check "drv replays the recording (probe count)" eq "$(count_of drv)" "$N_LOAD"
}

# ── api: legs that need the save-record engine ────────────────────────────
API_LEGS=(
    "api pair: A only / B only / both in each order / A then B (statuses and payload hashes)"
    "api pair: mod removed then re-added (GAP)"
    "api pair: downgrade (NEWER)"
    "api pair: card swapped for an older copy (older snapshot's records)"
    "api pair: record edited outside openpete with its checksum fixed (STALE)"
    "api pair: New Game over a used slot (fresh lineage, no carried records)"
    "api pair: New Game, save, B toggled off, save again (B reads GAP)"
    "api pair: older card restored, save with a mod disabled (carried payload equals the older snapshot's)"
    "api: every api leg's card byte-identical to the baseline-mode card, snapshots written > 0"
    "legacy: two legacy-forced fixtures raise the duplicate-override warning"
    "legacy: one legacy and one api fixture, save then load (legacy reads its own pad)"
    "legacy: LEGACY_PAD import (own magic, foreign magic, zero)"
    "runahead: a save inside a runahead window (suppressed-flush line, one snapshot, card equals --runahead 0)"
    "runahead: read-back in the write tick and the tick before"
    "runahead: adoption of a save on the tick g_Gamestate leaves 0 (one snapshot, save on disk)"
    "rewind across a fixture-driven save (one snapshot)"
    "savestate file loaded in a new process (no read, arena from the state)"
    "delivery: load 1, New Game (ABSENT); load 1, disable, load 2, enable (reads 2); New Game disabled then enable (ABSENT)"
    "delivery: Quit Game and reload the same save (read again); New Game twice (ABSENT twice)"
    "delivery: New Game, save, disable, save, enable (no read); hot reload mid-save (no read)"
    "delivery: savestate from save 1 loaded on save 2 (no read); intro's second clear (second ABSENT)"
    "delivery: a load inside an adopted runahead window (read on the next tick); load 1 then a driven write to index 2 (no read)"
    "delivery: a savestate blob older than the key's block (key unknown, no read)"
    "hooks: attract-demo clear (ABSENT); one card image in both ports (multi-match from g_FairyCutscene)"
    "hooks: driven SaveLoad of a checksum-failed record (no read, key unchanged)"
    "hooks: file create over a card whose .modsave outlived its file (snapshots dropped)"
    "hooks: handoff (key, held identities and arena carried, no read); savestate with --card1/--card2 swapped (rebind)"
    "faults: failed adoption flush; partial rollback restore; sidecar write failure; unparseable sidecar"
    "hooks: snapshot eviction across two lineages; write returning -1; mod upgrade; deferred first entry"
    "hooks: savestate with no game-extra section; LEGACY_PAD with a zero pad"
)
section_api() {
    say "api legs (save-record engine)"
    local l
    for l in "${API_LEGS[@]}"; do
        if [ "$HAVE_API" = 0 ]; then
            skip "$l [needs the save-record api: save_register, the .modsave sidecar and the key's block]"
        else
            fail "$l [the engine has the save-record api; this leg is not built yet]"
        fi
    done
}

# ── calibrate: each seeded fault must trip the check that guards it ───────
section_calibrate() {
    need_reference
    say "calibration: seeded faults (each check must report the fault)"
    CFG_A="$LEGACY_A"$'\n''seed_fault = 1' leg cal_body fairy-save - "$SAVE_FRAMES" "$P" "$A"
    if python3 "$CARD_PY" same-outside-pads "$WORK/s0/card1.mcr" "$WORK/cal_body/card1.mcr" >"$WORK/check.out" 2>&1; then
        fail "seed_fault 1 (record byte 0x40 flipped) not caught: $(tail -1 "$WORK/check.out")"
    else
        pass "seed_fault 1 caught by same-outside-pads: $(tail -1 "$WORK/check.out")"
    fi
    CFG_A="$LEGACY_A"$'\n''seed_fault = 2' leg cal_skip load-title "$WORK/s0/card1.mcr" "$LOAD_FRAMES" "$P" "$A"
    if [ "$(count_of cal_skip)" = "$N_LOAD" ]; then
        fail "seed_fault 2 (base skipped every 500th update) not caught: count $(count_of cal_skip) == $N_LOAD"
    else
        pass "seed_fault 2 caught by the probe count: $(count_of cal_skip) != $N_LOAD"
    fi
}

[ "$HAVE_API" = 1 ] && say "save-record api present in $SDK_HDR" ||
    say "no save-record api in $SDK_HDR: pre-fix record; api legs SKIP"
on record && section_record
on repair && section_repair
on driver && section_driver
on api && section_api
on calibrate && section_calibrate

echo
echo "[modsave] engagement: no-mod save leg $N_SAVE stock updates, load leg $N_LOAD"
echo "[modsave] totals: $n_pass PASS, $n_fail FAIL, $n_skip SKIP"
if [ "$n_fail" -ne 0 ]; then echo "[modsave] verdict: FAIL"; exit 1; fi
if [ "$n_pass" -eq 0 ]; then echo "[modsave] verdict: VACUOUS (no leg ran)"; exit 1; fi
[ "$n_skip" -ne 0 ] && echo "[modsave] verdict: PASS for the legs run; $n_skip legs SKIP awaiting the save-record engine" ||
    echo "[modsave] verdict: PASS"
exit 0
