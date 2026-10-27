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
# api legs need the engine's save records: without them every api leg
# fails (no read line, no sidecar). Rows no headless run can reach are
# reported CARVE-OUT with the reason.
#
# Spyro 2 and 3 (--game spyro-2|spyro-3) run their own section, s23:
# the game's save, load and autosave recordings under the save-record
# fixture mods/modsave-fixture-s23 (a second copy under another id for
# the two-mod rows), each card asserted byte-identical to the no-mod run of
# the same inputs, the asynchronous write rows of the save-record rules
# (issue and completion on different ticks, a write covering no record,
# file creation over three empty records, a savestate between issue and
# completion, a completion with no pending entry, a torn completion, the
# card-changed abort, the live pause save under runahead) and the
# delivered statuses and payloads.
#
# Usage: tests/harness/check_modsave.sh [--game spyro-1|spyro-2|spyro-3]
#        [--section record|repair|driver|api|more|calibrate|all]
#        (more: the api legs past the first save and load chain; needs the
#        a_s_ab leg of an earlier run in MODSAVE_WORK)
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

SECTION="default"
GAME=spyro-1
want_game=0
for a in "$@"; do
    if [ "$want_game" = 1 ]; then GAME="$a"; want_game=0; continue; fi
    case "$a" in
        --game) want_game=1 ;;
        --game=*) GAME="${a#--game=}" ;;
        --section=*) SECTION="${a#--section=}" ;;
        --calibrate) SECTION="calibrate" ;;
        --section) ;;
        record|repair|driver|api|more|calibrate|all|s23) SECTION="$a" ;;
        *) echo "check_modsave: unknown argument $a" >&2; exit 64 ;;
    esac
done
case "$GAME" in spyro-1|spyro-2|spyro-3) ;;
    *) echo "check_modsave: unknown game $GAME" >&2; exit 64 ;; esac
case "$SECTION" in default|record|repair|driver|api|more|calibrate|all|s23) ;;
    *) echo "check_modsave: unknown section $SECTION" >&2; exit 64 ;; esac
[ "$GAME" != spyro-1 ] && SECTION=s23
pg_resolve "$GAME"
on() { [ "$SECTION" = "$1" ] || { [ "$SECTION" = default ] && [ "$1" != calibrate ]; } ||
       { [ "$SECTION" = all ]; }; }

[ -x "$PG_BIN" ] || { echo "check_modsave: no binary at $PG_BIN" >&2; exit 64; }
SDK_HDR="$(dirname "$PG_BIN")/sdk/openpete_mod_api.h"
[ -f "$SDK_HDR" ] || { echo "check_modsave: no staged mod SDK at $SDK_HDR (build openpete_sdk_stage)" >&2; exit 64; }
HAVE_API=0
grep -q '#define OPENPETE_MOD_SAVE_API' "$SDK_HDR" && HAVE_API=1

BK2_DIR="$REPO/games/$GAME/bk2"
CARD_PY="$REPO/tests/harness/modsave_card.py"
SAVE_FRAMES=3700
LOAD_FRAMES=4300

if [ -n "${MODSAVE_WORK:-}" ]; then
    WORK="$MODSAVE_WORK"; mkdir -p "$WORK"
else
    WORK="$(mktemp -d /tmp/openpete-modsave.XXXXXX)"
    trap 'rm -rf "${WORK:?}"' EXIT
fi

n_pass=0; n_fail=0; n_skip=0; n_carve=0
say()  { printf '\n[modsave] %s\n' "$*"; }
pass() { printf '[modsave]   PASS: %s\n' "$*"; n_pass=$((n_pass + 1)); }
fail() { printf '[modsave]   FAIL: %s\n' "$*"; n_fail=$((n_fail + 1)); }
skip() { printf '[modsave]   SKIP: %s\n' "$*"; n_skip=$((n_skip + 1)); }
carve(){ printf '[modsave]   CARVE-OUT: %s\n' "$*"; n_carve=$((n_carve + 1)); }
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
    # The save records travel with the card: a chained leg starts from the
    # sidecar the leg that wrote its card left (SIDECAR=- starts without).
    local sc="${SIDECAR:-$card.modsave}"
    [ "$card" != "-" ] && [ "$sc" != "-" ] && [ -f "$sc" ] && cp "$sc" "$d/card1.mcr.modsave"
    local -a PG_CAPTURE_ENV=(OPENPETE_HANG_STALL_SECONDS=10)
    [ -n "${EXTRA_ENV:-}" ] && PG_CAPTURE_ENV+=($EXTRA_ENV)
    local -a bkarg=(--bk2 "$BK2_DIR/$bk.bk2")
    [ "$bk" = "-" ] && bkarg=()
    local -a extra=()
    [ -n "${EXTRA_ARGS:-}" ] && extra=($EXTRA_ARGS)
    local PG_CAPTURE_TIMEOUT=300
    # pg_capture deletes the outputs it is given before the run, so the card
    # is named an output only when the leg starts without one.
    local outs=""
    [ "$card" = "-" ] && outs="$d/card1.mcr"
    # CARD2=<image> mounts a copy in slot 2; CARD_ARGS replaces both card
    # flags (a leg that names another leg's card files).
    local -a cards=(--card1 "$d/card1.mcr" --no-card2)
    if [ -n "${CARD2:-}" ]; then
        cp "$CARD2" "$d/card2.mcr"
        cards=(--card1 "$d/card1.mcr" --card2 "$d/card2.mcr")
    fi
    [ -n "${CARD_ARGS:-}" ] && cards=($CARD_ARGS)
    # DURING_WAIT/DURING_CMD: once the log holds DURING_WAIT, run DURING_CMD
    # while the game runs (an edit the hot reload watches).
    local during=""
    if [ -n "${DURING_WAIT:-}" ]; then
        ( for _ in $(seq 1 600); do
              grep -q -- "$DURING_WAIT" "$d/log.txt" 2>/dev/null && { eval "$DURING_CMD"; break; }
              sleep 0.5
          done ) &
        during=$!
    fi
    pg_capture "$d/log.txt" "$outs" -- --mute 1 --mods-dir "$d/mods" \
        "${bkarg[@]}" "${cards[@]}" --no-log-file \
        --frame-limit "$frames" "${extra[@]}"
    echo $? > "$d/rc"
    [ -n "$during" ] && wait "$during"
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
eqnz()     { [ -n "$1" ] || { echo "empty value (no instrument line)"; return 1; }; eq "$1" "$2"; }
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
# Instrument: the fixtures' "read status <S> index <i> ... data <fnv>" and
# "write index <i> ... data <fnv>" lines, the sidecar dump
# (modsave_card.py modsave), and the card bytes. Without the engine the
# fixtures in auto mode take the pad path, so every leg below fails: no read
# line, no sidecar.
reads()    { grep -o "$2: read status [A-Z_]* index [0-9]*" "$WORK/$1/log.txt" | awk '{print $4}' | tr '\n' ' ' | sed 's/ $//'; }
reads_eq() { local got; got="$(reads "$1" "$2")"; [ "$got" = "$3" ] && echo "$2 reads [$got]" || { echo "$2 reads [$got], want [$3]"; return 1; }; }
last_write(){ grep -o "$2: write index [0-9]* value [0-9]* data [0-9a-f]*" "$WORK/$1/log.txt" | tail -1 | awk '{print $NF}'; }
last_read_data(){ grep -o "$2: read status [A-Z_]* index [0-9]* schema [0-9]* size [0-9]* data [0-9a-f]*" "$WORK/$1/log.txt" | tail -1 | awk '{print $NF}'; }
snapdump() { python3 "$CARD_PY" modsave "$WORK/$1/card1.mcr.modsave"; }
snaps_at() { snapdump "$1" 2>/dev/null | grep -c "^index $2 "; }
newest_rec(){ # newest_rec <tag> <index> <mod>: schema:gap:size:fnv of that mod's record, or none
    snapdump "$1" 2>/dev/null | grep "^index $2 snap 0 " | grep -o "$3:[0-9]*:[0-9]*:[0-9]*:[0-9a-f]*" | cut -d: -f2- || true; }
has_sidecar(){ [ -f "$WORK/$1/card1.mcr.modsave" ] && python3 "$CARD_PY" snapshots "$WORK/$1/card1.mcr.modsave"; }
same_base(){ python3 "$CARD_PY" same "$WORK/s_base/card1.mcr" "$WORK/$1/card1.mcr"; }

need_base() {
    [ -f "$WORK/s_base/card1.mcr" ] && return 0
    CFG_A='mode = "baseline"' CFG_B='mode = "baseline"' leg s_base fairy-save - "$SAVE_FRAMES" "$P" "$A" "$B"
}

section_api() {
    need_reference
    need_base
    local t
    say "api save legs: fairy-save.bk2 with the fixtures on the save-record api"
    leg a_s_ab fairy-save - "$SAVE_FRAMES" "$P" "$A" "$B"
    leg a_s_a fairy-save - "$SAVE_FRAMES" "$P" "$A"
    for t in a_s_ab a_s_a; do
        check "$t card byte-identical to the baseline-mode card (the card is never modified)" same_base "$t"
        check "$t sidecar written, snapshots > 0" has_sidecar "$t"
        check "$t replays the recording (probe count)" eq "$(count_of $t)" "$N_SAVE"
    done
    # Each func_8001277C clear on the way to New Game (the title's card
    # paths, its file create, New Game) delivers one ABSENT; the save
    # delivers nothing.
    check "a_s_ab A: ABSENT per new-playthrough clear, no other read" reads_eq a_s_ab "$A" "ABSENT ABSENT ABSENT"
    check "a_s_ab B: ABSENT per new-playthrough clear, no other read" reads_eq a_s_ab "$B" "ABSENT ABSENT ABSENT"
    check "a_s_ab A record at save 0 holds A's write" eqnz "$(newest_rec a_s_ab 0 "$A" | cut -d: -f4)" "$(last_write a_s_ab "$A")"
    check "a_s_ab B record at save 0 holds B's write" eqnz "$(newest_rec a_s_ab 0 "$B" | cut -d: -f4)" "$(last_write a_s_ab "$B")"
    check "a_s_ab one snapshot at save 0, none at 1 and 2 (empty records)" \
        eq "$(snaps_at a_s_ab 0)/$(snaps_at a_s_ab 1)/$(snaps_at a_s_ab 2)" "1/0/0"

    say "api load legs, chained on the api save legs' cards and sidecars"
    leg a_l_ab load-title "$WORK/a_s_ab/card1.mcr" "$LOAD_FRAMES" "$P" "$A" "$B"
    check "a_l_ab A reads OK" reads_eq a_l_ab "$A" "OK"
    check "a_l_ab B reads OK" reads_eq a_l_ab "$B" "OK"
    check "a_l_ab A reads the payload it wrote" eqnz "$(last_read_data a_l_ab "$A")" "$(last_write a_s_ab "$A")"
    check "a_l_ab B reads the payload it wrote" eqnz "$(last_read_data a_l_ab "$B")" "$(last_write a_s_ab "$B")"
    check "a_l_ab replays the recording (probe count)" eq "$(count_of a_l_ab)" "$N_LOAD"
    check "a_l_ab card unchanged by a load" python3 "$CARD_PY" same "$WORK/a_s_ab/card1.mcr" "$WORK/a_l_ab/card1.mcr"
    leg a_l_a_b load-title "$WORK/a_s_a/card1.mcr" "$LOAD_FRAMES" "$P" "$B"
    check "a_l_a_b (A then B): B reads ABSENT" reads_eq a_l_a_b "$B" "ABSENT"
    leg a_l_ba load-title "$WORK/a_s_ab/card1.mcr" "$LOAD_FRAMES" "$P" "$B" "$A"
    check "a_l_ba (other load order): A OK, B OK" bash -c "[ \"\$(grep -c '$A: read status OK' '$WORK/a_l_ba/log.txt')\" = 1 ] && [ \"\$(grep -c '$B: read status OK' '$WORK/a_l_ba/log.txt')\" = 1 ] && echo 'one OK each'"

    say "GAP: A's record carried while A is not loaded, read back as GAP"
    CFG_DRV='actions = "save:0@1600"' leg a_gap1 load-title "$WORK/a_s_ab/card1.mcr" "$LOAD_FRAMES" "$P" "$B" "$DRV"
    check "a_gap1 driver save reached the card" logged a_gap1 "$DRV: save 0 at 1600 rc 0"
    check "a_gap1 A's record carried with gap set (same bytes)" \
        eqnz "$(newest_rec a_gap1 0 "$A" | cut -d: -f2,4)" "1:$(last_write a_s_ab "$A")"
    check "a_gap1 two snapshots at save 0 (one lineage)" eq "$(snaps_at a_gap1 0)" 2
    leg a_gap2 load-title "$WORK/a_gap1/card1.mcr" "$LOAD_FRAMES" "$P" "$A" "$B"
    check "a_gap2 A reads GAP with its last payload" reads_eq a_gap2 "$A" "GAP"
    check "a_gap2 A's GAP payload is its own write" eqnz "$(last_read_data a_gap2 "$A")" "$(last_write a_s_ab "$A")"
    check "a_gap2 B reads OK" reads_eq a_gap2 "$B" "OK"

    say "card swapped for an older copy: the older snapshot's records"
    SIDECAR="$WORK/a_gap1/card1.mcr.modsave" leg a_old load-title "$WORK/a_s_ab/card1.mcr" "$LOAD_FRAMES" "$P" "$A" "$B"
    check "a_old A reads OK (the older snapshot, no gap)" reads_eq a_old "$A" "OK"
    check "a_old B reads the older snapshot's payload" eqnz "$(last_read_data a_old "$B")" "$(last_write a_s_ab "$B")"
    SIDECAR="$WORK/a_gap1/card1.mcr.modsave" CFG_DRV='actions = "save:0@1600"' \
        leg a_old_save load-title "$WORK/a_s_ab/card1.mcr" "$LOAD_FRAMES" "$P" "$A" "$DRV"
    check "a_old_save B's carried payload equals the older snapshot's" \
        eqnz "$(newest_rec a_old_save 0 "$B" | cut -d: -f4)" "$(last_write a_s_ab "$B")"

    say "NEWER: a schema-2 record read and kept by a schema-1 mod"
    CFG_A='schema = 2' CFG_DRV='actions = "save:0@1600"' leg a_up load-title "$WORK/a_s_a/card1.mcr" "$LOAD_FRAMES" "$P" "$A" "$DRV"
    check "a_up record schema 2" eqnz "$(newest_rec a_up 0 "$A" | cut -d: -f1)" 2
    CFG_DRV='actions = "save:0@1600"' leg a_down load-title "$WORK/a_up/card1.mcr" "$LOAD_FRAMES" "$P" "$A" "$DRV"
    check "a_down A reads NEWER" reads_eq a_down "$A" "NEWER"
    check "a_down refusal logged" logged a_down "schema 1 refused to overwrite"
    check "a_down stored record keeps schema 2" eqnz "$(newest_rec a_down 0 "$A" | cut -d: -f1)" 2

    say "STALE: the record edited outside openpete with its checksum fixed"
    python3 "$CARD_PY" edit "$WORK/a_s_ab/card1.mcr" "$WORK/stale.mcr" 0 0x58B >/dev/null
    SIDECAR="$WORK/a_s_ab/card1.mcr.modsave" leg a_stale load-title "$WORK/stale.mcr" "$LOAD_FRAMES" "$P" "$A" "$B"
    check "a_stale A reads STALE" reads_eq a_stale "$A" "STALE"
    check "a_stale B reads STALE with its newest payload" eqnz "$(last_read_data a_stale "$B")" "$(last_write a_s_ab "$B")"

    say "legacy: pad fixtures beside the api"
    [ -f "$WORK/s_a/card1.mcr" ] || CFG_A="$LEGACY_A" leg s_a fairy-save - "$SAVE_FRAMES" "$P" "$A"
    CFG_A="$LEGACY_A" CFG_B="$LEGACY_B" leg a_dup fairy-save - "$SAVE_FRAMES" "$P" "$A" "$B"
    check "a_dup duplicate-override warning" logged a_dup "override the save function at 0x80059594 without save records"
    CFG_A="$LEGACY_A" leg a_mix_s fairy-save - "$SAVE_FRAMES" "$P" "$A" "$B"
    CFG_A="$LEGACY_A" leg a_mix_l load-title "$WORK/a_mix_s/card1.mcr" "$LOAD_FRAMES" "$P" "$A" "$B"
    check "a_mix_l legacy A reads its own pad" logged a_mix_l "$A: save_load gs 13 rc 1 pad own"
    check "a_mix_l api B reads OK" reads_eq a_mix_l "$B" "OK"
    SIDECAR=- leg a_lp_own load-title "$WORK/s_a/card1.mcr" "$LOAD_FRAMES" "$P" "$A"
    check "a_lp_own A reads LEGACY_PAD (its PRND pad)" reads_eq a_lp_own "$A" "LEGACY_PAD"
    check "a_lp_own A imports a non-zero value" bash -c "grep -q '$A: read status LEGACY_PAD.* value [1-9]' '$WORK/a_lp_own/log.txt' && echo imported"
    SIDECAR=- leg a_lp_foreign load-title "$WORK/s_a/card1.mcr" "$LOAD_FRAMES" "$P" "$B"
    check "a_lp_foreign B reads LEGACY_PAD (A's pad), value 0" bash -c "grep -q '$B: read status LEGACY_PAD.* value 0 ' '$WORK/a_lp_foreign/log.txt' && echo 'foreign, value 0'"
    check "a_lp_foreign B reports no damage" bash -c "! grep -q 'damaged' '$WORK/a_lp_foreign/log.txt' && echo 'no damaged line'"
    SIDECAR=- leg a_lp_zero load-title "$WORK/s0/card1.mcr" "$LOAD_FRAMES" "$P" "$A"
    check "a_lp_zero A reads ABSENT (zero pad)" reads_eq a_lp_zero "$A" "ABSENT"

    say "delivery: driven reloads, a checksum-failed load, write to another index"
    # The write comes before the checksum-failed load: SaveLoad copies the
    # flipped m_LevelsVisited[0] before its verdict fails, after which a
    # SaveCreate record is empty.
    CFG_DRV='actions = "load:0@1600,load:0@1700,save:1@1750,badload:0@1800"' \
        leg a_deliv load-title "$WORK/a_s_ab/card1.mcr" "$LOAD_FRAMES" "$P" "$A" "$DRV"
    check "a_deliv A: title load OK, each driven reload OK, no read at the failed load or the write" \
        reads_eq a_deliv "$A" "OK OK OK"
    check "a_deliv badload: stock verdict fails" logged a_deliv "$DRV: badload 0 at 1800 rc 0"
    check "a_deliv write to save 1 snapshotted under the loaded playthrough's lineage" \
        bash -c "python3 '$CARD_PY' modsave '$WORK/a_deliv/card1.mcr.modsave' | awk '/^index 0 snap 0/{a=\$8}/^index 1 snap 0/{b=\$8}END{if(a==b&&a!=\"\"){print \"lineage \" a; exit 0} print \"lineages \" a \" \" b; exit 1}'"

    say "savestate file loaded in a new process: no read, arena from the state"
    EXTRA_ARGS="--state-save-on-frame 1500 --state-save-output $WORK/ss.state" \
        leg a_ss1 load-title "$WORK/a_s_ab/card1.mcr" 1600 "$P" "$A"
    check "a_ss1 state written" test -f "$WORK/ss.state"
    EXTRA_ARGS="--state-load-on-frame 200 --state-load-input $WORK/ss.state" \
        leg a_ss2 - "$WORK/a_s_ab/card1.mcr" 600 "$P" "$A"
    check "a_ss2 state loaded" logged a_ss2 "load"
    check "a_ss2 no read after the state load" reads_eq a_ss2 "$A" ""

    say "runahead: a driven save inside a runahead window"
    CFG_DRV='actions = "save:1@1600"' leg a_ra0 load-title "$WORK/a_s_ab/card1.mcr" "$LOAD_FRAMES" "$P" "$A" "$DRV"
    CFG_DRV='actions = "save:1@1600"' EXTRA_ARGS="--runahead 1" \
        leg a_ra1 load-title "$WORK/a_s_ab/card1.mcr" "$LOAD_FRAMES" "$P" "$A" "$DRV"
    check "a_ra1 engaged: a suppressed speculative flush" logged a_ra1 "flush during speculative tick suppressed"
    check "a_ra1 exactly one snapshot at save 1" eq "$(snaps_at a_ra1 1)" 1
    check "a_ra1 card equals the --runahead 0 card" python3 "$CARD_PY" same "$WORK/a_ra0/card1.mcr" "$WORK/a_ra1/card1.mcr"

    say "toggles: a mod toggled off at a save, a mod entered after a load, a refused write"
    CFG_DRV='actions = "save:0@1600"' EXTRA_ENV="OPENPETE_MODS_TOGGLE=900:$B:0" \
        leg a_tog1 load-title "$WORK/a_s_ab/card1.mcr" "$LOAD_FRAMES" "$P" "$A" "$B" "$DRV"
    # OPENPETE_MODS_TOGGLE counts canonical ticks: tick 900 falls after the
    # title load and before the driven save.
    check "a_tog1 B reads OK at the load, nothing after its toggle off" reads_eq a_tog1 "$B" "OK"
    check "a_tog1 B's record carried with gap set" \
        eqnz "$(newest_rec a_tog1 0 "$B" | cut -d: -f2,4)" "1:$(last_write a_s_ab "$B")"
    leg a_tog2 load-title "$WORK/a_tog1/card1.mcr" "$LOAD_FRAMES" "$P" "$A" "$B"
    check "a_tog2 B reads GAP" reads_eq a_tog2 "$B" "GAP"
    EXTRA_ENV="OPENPETE_MODS_TOGGLE=900:$A:0,2000:$A:1" \
        leg a_reen load-title "$WORK/a_s_ab/card1.mcr" "$LOAD_FRAMES" "$P" "$A" "$B"
    check "a_reen A reads OK at the load and nothing at its re-enable (same identity held)" \
        reads_eq a_reen "$A" "OK"
    check "a_reen B reads OK once" reads_eq a_reen "$B" "OK"
    CFG_A='write_fails = true' CFG_DRV='actions = "save:0@1600"' \
        leg a_wfail load-title "$WORK/a_s_ab/card1.mcr" "$LOAD_FRAMES" "$P" "$A" "$B" "$DRV"
    check "a_wfail A's write refused" logged a_wfail "$A: write index 0 refused"
    check "a_wfail A's previous record kept (no gap)" \
        eqnz "$(newest_rec a_wfail 0 "$A" | cut -d: -f2,4)" "0:$(last_write a_s_ab "$A")"

    say "faults: sidecar write failure, unparseable sidecar"
    CFG_DRV='actions = "save:0@1600"' EXTRA_ENV="OPENPETE_MODSAVE_FAULT_WRITE=1" \
        leg a_scfail load-title "$WORK/a_s_ab/card1.mcr" "$LOAD_FRAMES" "$P" "$A" "$DRV"
    check "a_scfail write failure logged" logged a_scfail "failed; the save records stay in memory"
    head -c 100 "$WORK/a_s_ab/card1.mcr.modsave" > "$WORK/corrupt.modsave"
    SIDECAR="$WORK/corrupt.modsave" leg a_corrupt load-title "$WORK/a_s_ab/card1.mcr" "$LOAD_FRAMES" "$P" "$A"
    check "a_corrupt unparseable sidecar set aside" bash -c "grep -q 'is unparseable; moved to' '$WORK/a_corrupt/log.txt' && test -f '$WORK/a_corrupt/card1.mcr.modsave.bad-1' && echo 'moved aside'"
    check "a_corrupt A reads ABSENT (no records, zero pad)" reads_eq a_corrupt "$A" "ABSENT"
    for t in a_l_ab a_gap2 a_old a_stale a_deliv; do
        check "$t replays the recording (probe count)" eq "$(count_of $t)" "$N_LOAD"
    done
    # Under --runahead 1 the probe's last line can come from the led tick.
    check "a_ra1 replays the recording (probe count, one led tick allowed)" \
        bash -c "c=$(count_of a_ra1); [ \$c -ge $N_LOAD ] && [ \$c -le \$(( $N_LOAD + 1 )) ] && echo \"\$c in [$N_LOAD, $N_LOAD+1]\""
}

section_api_more() {
    need_reference
    local t
    say "New Game over a used slot: a fresh lineage carries no record"
    CFG_DRV='actions = "newkeep:0@1600,save:0@1650"' leg a_ngused load-title "$WORK/a_s_ab/card1.mcr" "$LOAD_FRAMES" "$P" "$A" "$DRV"
    check "a_ngused A: OK at the load, ABSENT at the new playthrough" reads_eq a_ngused "$A" "OK ABSENT"
    check "a_ngused newest snapshot carries no B record (no carry under a fresh lineage)" \
        bash -c "[ -z \"\$(python3 '$CARD_PY' modsave '$WORK/a_ngused/card1.mcr.modsave' | grep '^index 0 snap 0 ' | grep -o '$B:')\" ] && echo 'no B record'"
    check "a_ngused new lineage at save 0" \
        bash -c "python3 '$CARD_PY' modsave '$WORK/a_ngused/card1.mcr.modsave' | awk '/^index 0 snap 0/{a=\$8}/^index 0 snap 1/{b=\$8}END{if(a!=\"\"&&b!=\"\"&&a!=b){print a\" != \"b;exit 0}print a\" \"b;exit 1}'"

    say "New Game twice with no save: two ABSENTs"
    CFG_DRV='actions = "newgame:0@1600,newgame:0@1700"' leg a_ng2 load-title "$WORK/a_s_ab/card1.mcr" "$LOAD_FRAMES" "$P" "$A" "$DRV"
    check "a_ng2 A: OK, ABSENT, ABSENT" reads_eq a_ng2 "$A" "OK ABSENT ABSENT"

    say "attract demo: the demo-level clear delivers ABSENT at the title"
    leg a_demo - - 4000 "$P" "$A"
    check "a_demo A reads ABSENT only, at least once" \
        bash -c "r=\$(grep -o '$A: read status [A-Z_]*' '$WORK/a_demo/log.txt' | awk '{print \$4}' | sort -u | tr '\n' ' '); [ \"\$r\" = 'ABSENT ' ] && echo \"reads \$(grep -c '$A: read status' '$WORK/a_demo/log.txt') ABSENT\" || { echo \"reads [\$r]\"; exit 1; }"

    say "runahead adoption: a save on the update that leaves g_Gamestate 0"
    CFG_DRV='actions = "leave:1@0"' leg a_adopt0 fairy-save - "$SAVE_FRAMES" "$P" "$A" "$DRV"
    CFG_DRV='actions = "leave:1@0"' EXTRA_ARGS="--runahead 1" leg a_adopt1 fairy-save - "$SAVE_FRAMES" "$P" "$A" "$DRV"
    check "a_adopt1 the leave save ran" logged a_adopt1 "$DRV: leave 1"
    check "a_adopt1 engaged: the save's queue adopted" logged a_adopt1 "adopted speculative ticks"
    check "a_adopt1 one snapshot at save 1" eq "$(snaps_at a_adopt1 1)" 1
    check "a_adopt1 card equals the --runahead 0 card" python3 "$CARD_PY" same "$WORK/a_adopt0/card1.mcr" "$WORK/a_adopt1/card1.mcr"
    CFG_DRV='actions = "leave:1@0"' EXTRA_ARGS="--runahead 1" EXTRA_ENV="OPENPETE_MEMCARD_FAULT_ADOPT_FLUSH=1" \
        leg a_adoptfail fairy-save - "$SAVE_FRAMES" "$P" "$A" "$DRV"
    check "a_adoptfail failed adoption flush drops the queue (logged)" logged a_adoptfail "adoption card flush failed"
    check "a_adoptfail no snapshot at save 1" eq "$(snaps_at a_adoptfail 1)" 0

    say "runahead rollback that fails after a partial restore: the queue is discarded"
    CFG_DRV='actions = "save:1@1600"' EXTRA_ARGS="--runahead 1" EXTRA_ENV="OPENPETE_STATE_FAULT_SPU_RESTORE=card" \
        leg a_partial load-title "$WORK/a_s_ab/card1.mcr" "$LOAD_FRAMES" "$P" "$A" "$DRV"
    check "a_partial fault engaged" logged a_partial "fault injected: SPU restore failed"
    check "a_partial speculative queue discarded" logged a_partial "rolled back speculative ticks"
    # The partial restore leaves the arena (the driver's counter) at its led
    # value, so the committed pass does not repeat the save: the led save
    # must leave neither a card record nor a snapshot.
    check "a_partial no snapshot at save 1" eq "$(snaps_at a_partial 1)" 0
    check "a_partial the led save never reached the card file" \
        bash -c "python3 '$CARD_PY' records '$WORK/a_partial/card1.mcr' | grep -q '^record 1 empty 1' && echo 'record 1 empty'"

    say "rewind after a driven save: the save cleared the ring, one snapshot"
    CFG_DRV='actions = "save:1@1600"' EXTRA_ARGS="--rewind 600 --rewind-hold 3600:60" \
        leg a_rewind load-title "$WORK/a_s_ab/card1.mcr" "$LOAD_FRAMES" "$P" "$A" "$DRV"
    check "a_rewind engaged: the hold rewound" bash -c "grep -iE 'rewind.*(restor|jump|back to)' '$WORK/a_rewind/log.txt' | head -1 | grep . "
    check "a_rewind one snapshot at save 1" eq "$(snaps_at a_rewind 1)" 1
    check "a_rewind no read after the load" reads_eq a_rewind "$A" "OK"

    say "hot reload of a registered mod: no read, arena kept"
    CFG_A='schema = 1' CFG_DRV='actions = "save:0@2000"' EXTRA_ENV="OPENPETE_MOD_HOT_RELOAD=1" \
        DURING_WAIT="read status OK" DURING_CMD="sleep 2; echo '# edit' >> '$WORK/a_hot/mods/$A/config.toml'" \
        leg a_hot load-title "$WORK/a_s_ab/card1.mcr" "$LOAD_FRAMES" "$P" "$A" "$DRV"
    check "a_hot reload engaged" bash -c "grep -iE '$A.*reload|reload.*$A' '$WORK/a_hot/log.txt' | grep -iv armed | head -1 | grep ."
    check "a_hot A reads only at the load" reads_eq a_hot "$A" "OK"
    check "a_hot A's state survived the reload (write value above its load value 99)" \
        bash -c "v=\$(grep -o '$A: write index 0 value [0-9]*' '$WORK/a_hot/log.txt' | tail -1 | awk '{print \$NF}'); [ -n \"\$v\" ] && [ \"\$v\" -gt 99 ] && echo \"value \$v\""

    say "savestate from save 0 loaded while on save 1: no read"
    CFG_DRV='actions = "save:1@1600,load:1@1700"' \
        EXTRA_ARGS="--state-save-on-frame 2400 --state-save-output $WORK/ss01.state --state-load-on-frame 3800 --state-load-input $WORK/ss01.state" \
        leg a_ss01 load-title "$WORK/a_s_ab/card1.mcr" "$LOAD_FRAMES" "$P" "$A" "$DRV"
    check "a_ss01 driven load of save 1 read" bash -c "grep -q '$A: read status OK index 1' '$WORK/a_ss01/log.txt' && echo 'read save 1'"
    # The state predates the driven ops, which the restored driver counter
    # runs again; the state load itself delivers nothing.
    check "a_ss01 no read between the state load and the next driven op" \
        bash -c "awk '/savestate: loaded/{on=1;next} on&&/modsave-driver: /{exit} on&&/$A: read status/{n++} END{print n+0\" reads\"; exit (n>0)}' '$WORK/a_ss01/log.txt'"

    say "savestate blob with no save-record trailer: key unknown, no read"
    CFG_DRV='actions = "save:1@2300"' EXTRA_ENV="OPENPETE_MODSAVE_FAULT_NO_TRAILER=1" \
        EXTRA_ARGS="--state-save-on-frame 3000 --state-save-output $WORK/old.state --state-load-on-frame 3100 --state-load-input $WORK/old.state" \
        leg a_oldblob load-title "$WORK/a_s_ab/card1.mcr" "$LOAD_FRAMES" "$P" "$A" "$DRV"
    check "a_oldblob no read after the load of the old blob" reads_eq a_oldblob "$A" "OK"
    CFG_DRV='actions = "save:1@2150"' EXTRA_ENV="OPENPETE_MODSAVE_FAULT_NO_TRAILER=1" \
        EXTRA_ARGS="--state-save-on-frame 3000 --state-save-output $WORK/old2.state --state-load-on-frame 3100 --state-load-input $WORK/old2.state" \
        leg a_oldblob2 load-title "$WORK/a_s_ab/card1.mcr" "$LOAD_FRAMES" "$P" "$A" "$DRV"
    check "a_oldblob2 a save after the old blob finds the key unknown" logged a_oldblob2 "written with the key unknown"

    say "one card image in both ports: the game's selection resolves the match"
    CARD2="$WORK/a_s_ab/card1.mcr" leg a_two load-title "$WORK/a_s_ab/card1.mcr" "$LOAD_FRAMES" "$P" "$A"
    check "a_two load bound to card 0 save 0 of 2 candidates" logged a_two "load bound to card 0 save 0 of 2 candidates"
    check "a_two A reads OK" reads_eq a_two "$A" "OK"

    say "handoff: key, held identities and arena carried, no read"
    EXTRA_ENV="OPENPETE_HANDOFF_AT=2600" leg a_handoff load-title "$WORK/a_s_ab/card1.mcr" "$LOAD_FRAMES" "$P" "$A"
    check "a_handoff engaged" bash -c "grep -iE 'handoff' '$WORK/a_handoff/log.txt' | grep -iE 'exec|resum' | head -1 | grep ."
    # The resumed process boots and replays the recording up to the
    # checkpoint (its title load reads again); the resume itself delivers
    # nothing.
    check "a_handoff no read after the session resumed" \
        bash -c "awk '/handoff: session resumed/{on=1;next} on&&/$A: read status/{n++} END{print n+0\" reads after resume\"; exit (n>0)}' '$WORK/a_handoff/log.txt'"
    check "a_handoff resumed" logged a_handoff "handoff: session resumed"

    say "savestate loaded with the cards swapped: the key follows its card"
    python3 "$CARD_PY" edit "$WORK/a_s_ab/card1.mcr" "$WORK/other.mcr" 0 0x58A >/dev/null
    CARD2="$WORK/other.mcr" EXTRA_ARGS="--state-save-on-frame 2600 --state-save-output $WORK/sw.state" \
        leg a_sw1 load-title "$WORK/a_s_ab/card1.mcr" 2700 "$P" "$A"
    # A file load waits for the state's park site, a gameplay yield, so it
    # is scheduled after the title load of the swapped run.
    CARD_ARGS="--card1 $WORK/a_sw1/card2.mcr --card2 $WORK/a_sw1/card1.mcr" \
        EXTRA_ARGS="--state-load-on-frame 2000 --state-load-input $WORK/sw.state" \
        leg a_sw2 load-title - 2300 "$P" "$A"
    check "a_sw2 state loaded" logged a_sw2 "savestate: loaded"
    check "a_sw2 key rebound to the other port" logged a_sw2 "savestate key rebound from card slot 0 to 1"
    check "a_sw2 no read after the state load" \
        bash -c "awk '/savestate: loaded/{on=1;next} on&&/$A: read status/{n++} END{print n+0\" reads\"; exit (n>0)}' '$WORK/a_sw2/log.txt'"

    say "snapshot eviction: five saves at one index"
    CFG_DRV='actions = "save:0@1500,save:0@1700,save:0@1900,save:0@2100,save:0@2300"' \
        leg a_evict load-title "$WORK/a_s_ab/card1.mcr" "$LOAD_FRAMES" "$P" "$A" "$DRV"
    check "a_evict the oldest snapshot evicted (logged)" logged a_evict "evicted"
    check "a_evict four snapshots at save 0" eq "$(snaps_at a_evict 0)" 4
}

# Rows no headless run can reach, each with its reason.
section_api_carve() {
    carve "deferred first entry: headless runs keep the exact enabled set (the load-all path that defers a mod's entry is interactive only)"
    carve "savestate with no game-extra section: core appends the save-record trailer to every section, so every state written now has one; an older trailer-less blob is the a_oldblob row"
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

# ── s23: Spyro 2 and 3, the asynchronous BIOS file-layer writes ───────────
# Instrument: the fixture's write and read lines, the engine's "save write
# issued at frame N, <bytes> at <ofs>, <n> records covered" and "save write
# completed at frame N" lines, the sidecar dump and the card bytes. Every
# card is compared with the no-mod run of the same inputs. Without the
# game's save descriptor save_register returns -1 (the fixture logs
# "register rc -1"), so every save-record check below fails.
F=modsave-fixture-s23
F2=modsave-fixture-s23b
RUN_ARGS_CARD=()
for a in "${PG_RUN_ARGS[@]}"; do [ "$a" = --no-card ] || RUN_ARGS_CARD+=("$a"); done

# leg23 <tag> <bk2 name|-> <card in|-> <frames> <mods...>
# As leg, for the Spyro 2 and 3 fixture: $F from mods/, $F2 a copy under a
# second id; CFG_F and CFG_F2 append to their config.toml. A card of "-"
# provisions a blank card. The watchdog allows 30 s for the CdInit stall
# a loaded machine can show at boot.
leg23() {
    local tag="$1" bk="$2" card="$3" frames="$4"; shift 4
    local d="$WORK/$tag"
    rm -rf "${d:?}"; mkdir -p "$d/mods/$F" "$d/mods/$F2"
    : > "$d/mods/enabled.txt"
    cp -r "$REPO/mods/$F/mod.toml" "$REPO/mods/$F/src" "$d/mods/$F/"
    cp -r "$REPO/mods/$F/src" "$d/mods/$F2/"
    sed "s/^id *= *\"$F\"/id      = \"$F2\"/" "$REPO/mods/$F/mod.toml" > "$d/mods/$F2/mod.toml"
    printf 'tag = "%s"\n%s\n' "$F" "${CFG_F:-}" > "$d/mods/$F/config.toml"
    printf 'tag = "%s"\n%s\n' "$F2" "${CFG_F2:-}" > "$d/mods/$F2/config.toml"
    local m
    for m in "$@"; do echo "$m" >> "$d/mods/enabled.txt"; done
    [ "$card" != "-" ] && cp "$card" "$d/card1.mcr"
    local sc="${SIDECAR:-$card.modsave}"
    [ "$card" != "-" ] && [ "$sc" != "-" ] && [ -f "$sc" ] && cp "$sc" "$d/card1.mcr.modsave"
    # MODSET=<tag> plants a card-level .modset beside the card.
    [ -n "${MODSET:-}" ] && printf '%s\n' "$MODSET" > "$d/card1.mcr.modset"
    local -a PG_CAPTURE_ENV=(OPENPETE_HANG_STALL_SECONDS=30)
    [ -n "${EXTRA_ENV:-}" ] && PG_CAPTURE_ENV+=($EXTRA_ENV)
    local -a bkarg=(--bk2 "$BK2_DIR/$bk.bk2" --bk2-stop-on-end)
    [ "$bk" = "-" ] && bkarg=()
    local -a extra=()
    [ -n "${EXTRA_ARGS:-}" ] && extra=($EXTRA_ARGS)
    local PG_CAPTURE_TIMEOUT=300
    local outs=""
    [ "$card" = "-" ] && outs="$d/card1.mcr"
    # The game's boot flags without --no-card, which would unmount --card1.
    local -a PG_RUN_ARGS=("${RUN_ARGS_CARD[@]}")
    pg_capture "$d/log.txt" "$outs" -- --mute 1 --mods-dir "$d/mods" \
        "${bkarg[@]}" --card1 "$d/card1.mcr" --no-card2 --no-log-file \
        --frame-limit "$frames" "${extra[@]}"
    echo $? > "$d/rc"
    [ -f "$d/card1.mcr" ] || echo "check_modsave: leg $tag left no card" >&2
}
issued()    { grep -c 'save write issued' "$WORK/$1/log.txt"; }
completed() { grep -c 'save write completed' "$WORK/$1/log.txt"; }
issue_frames(){ grep -o 'save write issued at frame [0-9]*' "$WORK/$1/log.txt" | awk '{print $NF}' | tr '\n' ' ' | sed 's/ $//'; }
done_frames(){ grep -o 'save write completed at frame [0-9]*' "$WORK/$1/log.txt" | awk '{print $NF}' | tr '\n' ' ' | sed 's/ $//'; }
card_same() { python3 "$CARD_PY" same "$WORK/$1/card1.mcr" "$WORK/$2/card1.mcr"; }
grecords()  { python3 "$CARD_PY" game-records "$GAME" "$WORK/$1/card1.mcr"; }
nonempty_idx(){ grecords "$1" | awk '$4 == 0 {print $2}' | tr '\n' ' ' | sed 's/ $//'; }
last_read() { grep -o "$2: read status [A-Z_]*" "$WORK/$1/log.txt" | tail -1 | awk '{print $NF}'; }
no_line()   { if has "$1" "$2"; then echo "logged '$2'"; return 1; fi; echo "no '$2' line"; }
reg_ok()    { logged "$1" "$F: mode auto game $GAME register rc 0"; }
# snap_sig <tag>: the sidecar's index, position, record hash and payloads, lineage left out
snap_sig()  { snapdump "$1" | awk '{$7=$8=""; print}'; }
# lineages <tag> <index>: the lineage of each snapshot at the index, newest first
lineages()  { snapdump "$1" | awk -v i="$2" '$2 == i {print $8}' | tr '\n' ' ' | sed 's/ $//'; }
# split_ok <tag> <k>: the k-th (1-based) write's completion frame is after its issue frame
split_ok() {
    local i c
    i="$(issue_frames "$1" | cut -d' ' -f"$2")"; c="$(done_frames "$1" | cut -d' ' -f"$2")"
    [ -n "$i" ] && [ -n "$c" ] && [ "$c" -gt "$i" ] && echo "issue frame $i, completion frame $c" ||
        { echo "issue frame '$i', completion frame '$c'"; return 1; }
}

section_s2() {
    local t AUTO="--skip-to-cutscene summer_forest_story"
    say "S2 New Game, then the story-step autosave: file creation over three empty records, a record write"
    EXTRA_ARGS="$AUTO" leg23 s2c0 s2-create-save - 3600
    EXTRA_ARGS="$AUTO" leg23 s2c1 s2-create-save - 3600 "$F"
    EXTRA_ARGS="$AUTO" leg23 s2c2 s2-create-save - 3600 "$F" "$F2"
    check "s2c0 engaged: the autosave left record 0 the only non-empty record" eq "$(nonempty_idx s2c0)" 0
    check "s2c1 save_register accepted" reg_ok s2c1
    for t in s2c1 s2c2; do check "$t card byte-identical to the no-mod card" card_same s2c0 "$t"; done
    check "s2c1 file creation: one write call covering the three records" logged s2c1 "0x1400 bytes at 0x0, 3 records covered"
    check "s2c1 autosave: one write call covering record 0" logged s2c1 "0x600 bytes at 0x200, 1 record covered"
    check "s2c1 two issues, two completions" eq "$(issued s2c1)/$(completed s2c1)" 2/2
    check "s2c1 autosave issue and completion on different ticks" split_ok s2c1 2
    check "s2c1 New Game delivers ABSENT, nothing else" reads_eq s2c1 "$F" "ABSENT"
    check "s2c1 snapshots 1/0/0 (the empty records dropped)" eq "$(snaps_at s2c1 0)/$(snaps_at s2c1 1)/$(snaps_at s2c1 2)" 1/0/0
    check "s2c1 record at save 0 holds the fixture's write" eqnz "$(newest_rec s2c1 0 "$F" | cut -d: -f4)" "$(last_write s2c1 "$F")"
    check "s2c1 the key was cleared by New Game, not unknown" no_line s2c1 "written with the key unknown"
    check "s2c1 no outside edit" no_line s2c1 "outside the file-level entry"
    check "s2c2 both fixtures' records in the snapshot" \
        bash -c "python3 '$CARD_PY' modsave '$WORK/s2c2/card1.mcr.modsave' | grep '^index 0 snap 0 ' | grep -q '$F:' && python3 '$CARD_PY' modsave '$WORK/s2c2/card1.mcr.modsave' | grep '^index 0 snap 0 ' | grep -q '$F2:' && echo 'both records'"

    say "S2 title load of the autosaved card"
    leg23 s2l0 s2-load-save "$WORK/s2c0/card1.mcr" 3000
    leg23 s2l1 s2-load-save "$WORK/s2c1/card1.mcr" 3000 "$F"
    check "s2l1 card byte-identical to the no-mod run" card_same s2l0 s2l1
    check "s2l1 card unchanged by a load" card_same s2c1 s2l1
    check "s2l1 load bound to save 0" logged s2l1 "load bound to card 0 save 0 of 1 candidate"
    check "s2l1 last read OK" eq "$(last_read s2l1 "$F")" OK
    check "s2l1 reads the payload it wrote" eqnz "$(last_read_data s2l1 "$F")" "$(last_write s2c1 "$F")"

    say "S2 New Game alone: the file created over three empty records, no record saved"
    leg23 s2n0 s2-create-save - 1500
    leg23 s2n1 s2-create-save - 1500 "$F"
    check "s2n1 card byte-identical to the no-mod card" card_same s2n0 s2n1
    check "s2n1 no snapshot (three empty records dropped)" eq "$(snaps_at s2n1 0)/$(snaps_at s2n1 1)/$(snaps_at s2n1 2)" 0/0/0

    say "S2 load File 1, then the pause save to File 2: the loaded playthrough's lineage carried"
    leg23 s2x0 s2-save-other-slot "$WORK/s2c0/card1.mcr" 3400
    leg23 s2x1 s2-save-other-slot "$WORK/s2c1/card1.mcr" 3400 "$F"
    check "s2x0 engaged: record 1 written" eq "$(nonempty_idx s2x0)" "0 1"
    check "s2x1 card byte-identical to the no-mod run" card_same s2x0 s2x1
    check "s2x1 the write covers record 1 alone" logged s2x1 "0x600 bytes at 0x800, 1 record covered"
    check "s2x1 the load read OK, the save nothing" reads_eq s2x1 "$F" "OK"
    check "s2x1 save 1's snapshot carries save 0's lineage" \
        bash -c "a=\$(python3 '$CARD_PY' modsave '$WORK/s2x1/card1.mcr.modsave' | awk '\$2==0 && \$4==0 {print \$8}'); b=\$(python3 '$CARD_PY' modsave '$WORK/s2x1/card1.mcr.modsave' | awk '\$2==1 && \$4==0 {print \$8}'); [ -n \"\$a\" ] && [ \"\$a\" = \"\$b\" ] && echo \"lineage \$a\" || { echo \"lineages '\$a' '\$b'\"; exit 1; }"
    check "s2x1 save 1 holds the fixture's write" eqnz "$(newest_rec s2x1 1 "$F" | cut -d: -f4)" "$(last_write s2x1 "$F")"

    say "S2 Horizontal changed in Options, then a pause save: the 0x80-byte header write covers no record"
    leg23 s2h0 s2-header-save "$WORK/s2c0/card1.mcr" 3600
    leg23 s2h1 s2-header-save "$WORK/s2c1/card1.mcr" 3600 "$F"
    check "s2h1 card byte-identical to the no-mod run" card_same s2h0 s2h1
    check "s2h1 the header write: one entry covering no record" logged s2h1 "0x80 bytes at 0x0, 0 records covered"
    check "s2h1 two issues, two completions (header, record)" eq "$(issued s2h1)/$(completed s2h1)" 2/2
    check "s2h1 no outside edit, no orphan completion" bash -c "! grep -qE 'outside the file-level entry|no pending entry' '$WORK/s2h1/log.txt' && echo 'none'"
    check "s2h1 two snapshots at save 0, one lineage" bash -c "set -- $(lineages s2h1 0); [ \$# = 2 ] && [ \"\$1\" = \"\$2\" ] && echo \"\$1 x2\""

    say "S2 in-level pause save (gameplay-live): three saves to File 1 of the New Game card"
    leg23 s2p0 s2-pause-save "$WORK/s2n0/card1.mcr" 4000
    leg23 s2p1 s2-pause-save "$WORK/s2n1/card1.mcr" 4000 "$F"
    check "s2p0 engaged: the pause save wrote record 0" eq "$(nonempty_idx s2p0)" 0
    check "s2p1 card byte-identical to the no-mod run" card_same s2p0 s2p1
    check "s2p1 three issues, three completions" eq "$(issued s2p1)/$(completed s2p1)" 3/3
    check "s2p1 each write covers record 0" eq "$(grep -c '0x600 bytes at 0x200, 1 record covered' "$WORK/s2p1/log.txt")" 3
    check "s2p1 first save: issue and completion on different ticks" split_ok s2p1 1
    # --skip-to-level enters through an attract-demo start, whose reset
    # clears the key: the first save mints a lineage from a cleared key.
    check "s2p1 the demo-start reset delivered ABSENT, the saves nothing" reads_eq s2p1 "$F" "ABSENT"
    check "s2p1 three snapshots at save 0" eq "$(snaps_at s2p1 0)" 3
    check "s2p1 newest record holds the last write" eqnz "$(newest_rec s2p1 0 "$F" | cut -d: -f4)" "$(last_write s2p1 "$F")"
    check "s2p1 the three pause saves share one lineage (the first mints it)" \
        bash -c "set -- $(lineages s2p1 0); [ \$# = 3 ] && [ \"\$1\" = \"\$2\" ] && [ \"\$2\" = \"\$3\" ] && echo \"\$1 x3\""

    say "S2 pause save under runahead: issue and completion on speculative ticks"
    EXTRA_ARGS="--runahead 1" leg23 s2ra s2-pause-save "$WORK/s2n1/card1.mcr" 4000 "$F"
    check "s2ra engaged: a suppressed speculative flush" logged s2ra "flush during speculative tick suppressed"
    check "s2ra card byte-identical to the no-mod run" card_same s2p0 s2ra
    # The fixture's count (Update calls while live) is not a guest value:
    # under runahead adoption it may differ, so the payloads are compared
    # with each run's own writes, the record hashes across runs.
    check "s2ra snapshots: the same record hashes as the --runahead 0 run" \
        bash -c "diff <(python3 '$CARD_PY' modsave '$WORK/s2p1/card1.mcr.modsave' | awk '{print \$2, \$4, \$6}') <(python3 '$CARD_PY' modsave '$WORK/s2ra/card1.mcr.modsave' | awk '{print \$2, \$4, \$6}') && echo 'same record hashes'"
    check "s2ra newest record holds the committed write" eqnz "$(newest_rec s2ra 0 "$F" | cut -d: -f4)" "$(last_write s2ra "$F")"
    check "s2ra one lineage across the three saves" bash -c "set -- $(lineages s2ra 0); [ \$# = 3 ] && [ \"\$1\" = \"\$3\" ] && [ \"\$1\" = \"\$2\" ] && echo \"\$1 x3\""

    local i1 c1
    i1="$(issue_frames s2p1 | cut -d' ' -f1)"; c1="$(done_frames s2p1 | cut -d' ' -f1)"
    if [ -z "$i1" ] || [ -z "$c1" ]; then
        fail "s2 savestate legs: no issue/completion frames from s2p1"
        return
    fi
    # --state-*-on-frame count VBLs; the capture lines count openpete
    # frames. The bk2 replay line names both at its first input.
    local off
    off="$(grep -o 'openpete_frame=[0-9]* bk2_frame=[0-9]*' "$WORK/s2p1/log.txt" | head -1 | awk -F'[= ]' '{print $4 - $2}')"
    [ -n "$off" ] || off=0
    local mid=$(( (i1 + c1) / 2 + off )) after=$(( c1 + 30 + off ))
    say "S2 savestate taken between issue ($i1) and completion ($c1), loaded after the completion"
    EXTRA_ARGS="--state-save-on-frame $mid --state-save-output $WORK/s2mid.state --state-load-on-frame $after --state-load-input $WORK/s2mid.state" \
        leg23 s2ss s2-pause-save "$WORK/s2n1/card1.mcr" 4000 "$F"
    check "s2ss state loaded" logged s2ss "savestate: loaded"
    check "s2ss the restored entry completes again (one more completion than issues)" \
        eq "$(completed s2ss)" "$(( $(issued s2ss) + 1 ))"
    check "s2ss no outside edit, no orphan completion" no_line s2ss "no pending entry"
    check "s2ss the re-run completion replaced its equal snapshot (snapshot count as s2p1)" eq "$(snaps_at s2ss 0)" "$(snaps_at s2p1 0)"
    EXTRA_ENV="OPENPETE_MODSAVE_FAULT_NO_TRAILER=1" \
    EXTRA_ARGS="--state-save-on-frame $mid --state-save-output $WORK/s2old.state --state-load-on-frame $after --state-load-input $WORK/s2old.state" \
        leg23 s2orph s2-pause-save "$WORK/s2n1/card1.mcr" 4000 "$F"
    check "s2orph state loaded" logged s2orph "savestate: loaded"
    check "s2orph a completion with no pending entry is logged as an outside edit" logged s2orph "completed with no pending entry"
    check "s2orph the orphan completion stores nothing (snapshot count as s2p1)" eq "$(snaps_at s2orph 0)" "$(snaps_at s2p1 0)"

    say "S2 torn completion: the mid-write state loaded over another card (the autosaved one, record 0 non-empty)"
    EXTRA_ARGS="--state-save-on-frame $mid --state-save-output $WORK/s2tear.state" \
        leg23 s2tA s2-pause-save "$WORK/s2n1/card1.mcr" $(( mid + 5 )) "$F"
    check "s2tA state written" test -f "$WORK/s2tear.state"
    # A state loads only while a port holds the image path it held: the
    # autosaved card is copied over s2tA's card path, which s2tB mounts.
    cp "$WORK/s2c0/card1.mcr" "$WORK/s2tA/card1.mcr"
    rm -f "${WORK:?}/s2tA/card1.mcr.modsave"
    EXTRA_ARGS="--state-load-on-frame 200 --state-load-input $WORK/s2tear.state --card1 $WORK/s2tA/card1.mcr" \
        leg23 s2tB s2-pause-save "$WORK/s2c0/card1.mcr" $(( c1 + off + 300 )) "$F"
    check "s2tB state loaded" logged s2tB "savestate: loaded"
    check "s2tB the completion's checksum verdict fails; nothing kept" logged s2tB "fails the game's checksum in the card image; no save record kept"
}

section_s3() {
    local t
    say "S3 New Game, then the New Game save: file creation, the summary (no record), the record"
    leg23 s3c0 s3-create-save - 6000
    leg23 s3c1 s3-create-save - 6000 "$F"
    check "s3c0 engaged: one non-empty record" bash -c "[ -n \"$(nonempty_idx s3c0)\" ] && echo 'record $(nonempty_idx s3c0)'"
    check "s3c1 save_register accepted" reg_ok s3c1
    check "s3c1 card byte-identical to the no-mod card" card_same s3c0 s3c1
    check "s3c1 file creation: one write call covering the three records" logged s3c1 "0x1D80 bytes at 0x0, 3 records covered"
    check "s3c1 summary write covers no record" logged s3c1 "0x80 bytes at 0x200, 0 records covered"
    check "s3c1 record write covers one record" logged s3c1 "0x900 bytes at 0x$(printf '%X' $(( 0x280 + 0x900 * $(nonempty_idx s3c0) ))), 1 record covered"
    check "s3c1 three issues, three completions" eq "$(issued s3c1)/$(completed s3c1)" 3/3
    check "s3c1 record write: issue and completion on different ticks" split_ok s3c1 3
    check "s3c1 no outside edit" no_line s3c1 "outside the file-level entry"
    check "s3c1 title menu entry delivers ABSENT" bash -c "r=\$(grep -o '$F: read status [A-Z_]*' '$WORK/s3c1/log.txt' | awk '{print \$4}' | sort -u | tr '\n' ' '); [ \"\$r\" = 'ABSENT ' ] && echo 'ABSENT only' || { echo \"reads [\$r]\"; exit 1; }"
    local ix; ix="$(nonempty_idx s3c0)"
    check "s3c1 one snapshot, at the saved index" eq "$(snaps_at s3c1 "$ix")" 1
    check "s3c1 record holds the fixture's write" eqnz "$(newest_rec s3c1 "$ix" "$F" | cut -d: -f4)" "$(last_write s3c1 "$F")"
    check "s3c1 the key was cleared at the title, not unknown" no_line s3c1 "written with the key unknown"

    say "S3 title load of the New Game save"
    leg23 s3l0 s3-load-save "$WORK/s3c0/card1.mcr" 4000
    leg23 s3l1 s3-load-save "$WORK/s3c1/card1.mcr" 4000 "$F"
    check "s3l1 card byte-identical to the no-mod run" card_same s3l0 s3l1
    check "s3l1 card unchanged by a load" card_same s3c1 s3l1
    check "s3l1 load bound to the saved index" logged s3l1 "load bound to card 0 save $ix of 1 candidate"
    check "s3l1 last read OK" eq "$(last_read s3l1 "$F")" OK
    check "s3l1 reads the payload it wrote" eqnz "$(last_read_data s3l1 "$F")" "$(last_write s3c1 "$F")"

    say "S3 options save on a blank card under --skip-to-level, then the Exit Level autosave"
    # The level is entered past the title, so the saved record has
    # visitedLevels[0] = 0: empty by the game's predicate, which the title
    # refuses to load. Every record write is a drop, not a snapshot.
    leg23 s3o0 s3-options-save-exit - 30000
    leg23 s3o1 s3-options-save-exit - 30000 "$F"
    check "s3o1 card byte-identical to the no-mod card" card_same s3o0 s3o1
    check "s3o1 five issues (create, summary, record; summary, record), five completions" eq "$(issued s3o1)/$(completed s3o1)" 5/5
    check "s3o0 record 0 written and empty by the game's predicate" \
        bash -c "python3 '$CARD_PY' game-records '$GAME' '$WORK/s3o0/card1.mcr' | grep -q '^record 0 empty 1 checksum ok' && echo 'record 0 empty, checksum ok'"
    check "s3o1 no snapshot: an empty record's write drops the index" eq "$(snaps_at s3o1 0)/$(snaps_at s3o1 1)/$(snaps_at s3o1 2)" 0/0/0
    check "s3o1 no fixture write for an empty record" bash -c "! grep -q '$F: write index' '$WORK/s3o1/log.txt' && echo 'no write call'"

    say "S3 card-changed abort: record 0's saveId changed between the options save and the autosave's stamp read"
    local fo fr ic3 is4
    fo="$(python3 "$CARD_PY" field-offset "$GAME" "$WORK/s3o0/card1.mcr" 0 0)"
    ic3="$(done_frames s3o1 | cut -d' ' -f3)"; is4="$(issue_frames s3o1 | cut -d' ' -f4)"
    if [ -z "$ic3" ] || [ -z "$is4" ] || [ -z "$fo" ]; then fail "s3 abort legs: no frames or offset from s3o1"; else
    fr=$(( (ic3 + is4) / 2 ))
    EXTRA_ENV="OPENPETE_MEMCARD_FAULT_EDIT=$fr:0:$fo" leg23 s3ab0 s3-options-save-exit - 30000
    EXTRA_ENV="OPENPETE_MEMCARD_FAULT_EDIT=$fr:0:$fo" leg23 s3ab1 s3-options-save-exit - 30000 "$F"
    check "s3ab1 fault engaged" logged s3ab1 "fault injected: card 0 byte"
    check "s3ab1 card byte-identical to the no-mod run with the same edit" card_same s3ab0 s3ab1
    check "s3ab1 the autosave aborted: no write after the stamp read (3 issues, 3 completions)" eq "$(issued s3ab1)/$(completed s3ab1)" 3/3
    check "s3ab1 no outside edit" no_line s3ab1 "outside the file-level entry"
    check "s3ab1 no orphan completion" no_line s3ab1 "no pending entry"
    fi

    say "S3 load slot 1 of the 117% card, then the options save to slot 3: the load's lineage carried"
    leg23 s3x0 s3-save-other-slot "$BK2_DIR/s3-load-gameover.bk2.mcr" 6000
    SIDECAR=- leg23 s3x1 s3-save-other-slot "$BK2_DIR/s3-load-gameover.bk2.mcr" 6000 "$F"
    check "s3x0 engaged: record 2 written" eq "$(nonempty_idx s3x0)" "0 2"
    check "s3x1 card byte-identical to the no-mod run" card_same s3x0 s3x1
    check "s3x1 summary (no record) then record 2" eq "$(grep -c '0x80 bytes at 0x200, 0 records covered' "$WORK/s3x1/log.txt")/$(grep -c '0x900 bytes at 0x1480, 1 record covered' "$WORK/s3x1/log.txt")" 1/1
    check "s3x1 save 2's snapshot carries the lineage the load bound" \
        bash -c "a=\$(grep -o 'load bound to card 0 save 0 of 1 candidate (hash [0-9a-f]*, lineage [0-9a-f]*' '$WORK/s3x1/log.txt' | awk '{print \$NF}'); b=\$(python3 '$CARD_PY' modsave '$WORK/s3x1/card1.mcr.modsave' | awk '\$2==2 && \$4==0 {print \$8}'); [ -n \"\$a\" ] && [ \"\$a\" = \"\$b\" ] && echo \"lineage \$a\" || { echo \"lineages '\$a' '\$b'\"; exit 1; }"
    check "s3x1 save 2 holds the fixture's write" eqnz "$(newest_rec s3x1 2 "$F" | cut -d: -f4)" "$(last_write s3x1 "$F")"

    say "S3 load of the 117% card, game over, then the portal autosave"
    leg23 s3g0 s3-load-gameover "$BK2_DIR/s3-load-gameover.bk2.mcr" 6000
    SIDECAR=- leg23 s3g1 s3-load-gameover "$BK2_DIR/s3-load-gameover.bk2.mcr" 6000 "$F"
    check "s3g1 card byte-identical to the no-mod run" card_same s3g0 s3g1
    check "s3g1 the load binds a fresh lineage (no snapshot yet)" logged s3g1 "fresh)"
    check "s3g1 the loaded save reads ABSENT (no record of the fixture)" eq "$(last_read s3g1 "$F")" ABSENT
    check "s3g1 the autosave: summary and record, two completions" eq "$(issued s3g1)/$(completed s3g1)" 2/2
    check "s3g1 one snapshot at save 0 holding the fixture's write" \
        eq "$(snaps_at s3g1 0)/$(newest_rec s3g1 0 "$F" | cut -d: -f4)" "1/$(last_write s3g1 "$F")"
    check "s3g1 the autosave carries the load's lineage, not unknown" no_line s3g1 "written with the key unknown"
}

section_s23_modset() {
    say "card-level .modset retired: neither checked nor rewritten on a game with save records"
    local src="$WORK/s2c0/card1.mcr" bk=s2-load-save fr=3000
    [ "$GAME" = spyro-3 ] && { src="$WORK/s3c0/card1.mcr"; bk=s3-load-save; fr=4000; }
    MODSET="other-mod@9.9" SIDECAR=- leg23 "${GAME}ms" "$bk" "$src" "$fr" "$F"
    check "${GAME}ms the planted .modset is untouched" eq "$(cat "$WORK/${GAME}ms/card1.mcr.modset")" "other-mod@9.9"
    check "${GAME}ms no card-level modset warning" \
        bash -c "! grep -qE 'modset MISMATCH|holds VANILLA|written under a modset|tagged with modset' '$WORK/${GAME}ms/log.txt' && echo 'no .modset line'"
    check "${GAME}ms engaged: the card was read (load bound)" logged "${GAME}ms" "load bound to card 0"
}

section_s23_carve() {
    carve "a New Game or load between a write's issue and completion: the game blocks input for the whole write, so only a mod's call can do it; the state model covers the generation guard"
}

[ "$HAVE_API" = 1 ] && say "save-record api present in $SDK_HDR" ||
    say "no save-record api in $SDK_HDR: pre-fix record; api legs SKIP"
if [ "$SECTION" = s23 ]; then
    [ "$GAME" = spyro-1 ] && { echo "check_modsave: section s23 needs --game spyro-2 or spyro-3" >&2; exit 64; }
    [ "$GAME" = spyro-2 ] && section_s2
    [ "$GAME" = spyro-3 ] && section_s3
    section_s23_modset
    section_s23_carve
fi
on record && section_record
on repair && section_repair
on driver && section_driver
on api && section_api
{ on api || [ "$SECTION" = more ]; } && section_api_more
on api && section_api_carve
on calibrate && section_calibrate

echo
[ "$GAME" = spyro-1 ] && echo "[modsave] engagement: no-mod save leg $N_SAVE stock updates, load leg $N_LOAD"
echo "[modsave] totals: $n_pass PASS, $n_fail FAIL, $n_skip SKIP, $n_carve CARVE-OUT"
if [ "$n_fail" -ne 0 ]; then echo "[modsave] verdict: FAIL"; exit 1; fi
if [ "$n_pass" -eq 0 ]; then echo "[modsave] verdict: VACUOUS (no leg ran)"; exit 1; fi
[ "$n_skip" -ne 0 ] && echo "[modsave] verdict: PASS for the legs run; $n_skip legs SKIP awaiting the save-record engine" ||
    echo "[modsave] verdict: PASS"
exit 0
