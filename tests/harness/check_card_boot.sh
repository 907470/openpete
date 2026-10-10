#!/usr/bin/env bash
# check_card_boot.sh - Spyro 3 boots past its title-screen card probe with a
# formatted card in port A and port B empty.
#
# The title links PsyQ libmcrd and reaches the card through the kernel's
# bu file device: B(32h) open, B(33h) lseek, B(34h) read, B(35h) write,
# B(36h) close. libmcrd retries lseek until it returns the requested offset,
# so a BIOS model that answers lseek with 0 spins until the hang watchdog
# ends the run (exit 2).
#
# Fixture: tests/golden/spyro-3/card-v6-blank.mcd, the card the DuckStation
# fork formats, exported before the first frame by
#   scripts/capture_psx_oracle_ds.sh --game spyro-3 \
#       --recipe games/spyro-3/oracle-card.conf
# (generated, never committed). The run writes through to its card file,
# so the row copies the fixture to a scratch path it deletes first.
#
# Run: headless, --card1 <scratch copy> --no-card2, --frame-limit 3000,
# OPENPETE_BIOS_CENSUS=2 streamed through a counter (the log is not kept:
# a spinning lseek writes one census line per call).
#
# Instrument: the census lines of B(32h) (opens) and B(33h) (lseeks); the
# watchdog's line names the VBL it counted from.
#
# Pass criterion: the run reaches the frame limit (exit 0) with opens >= 1
# (the engagement count: the title's card probe ran). FAIL: any other
# exit, or opens = 0 (VACUOUS: the probe never opened a file).
#
# Verdict line:
#   [card-boot] game=spyro-3 exit=<rc> opens=<n> lseeks=<n> watchdog_vbl=<v|->
#               verdict=PASS|FAIL|VACUOUS
#
# Usage: tests/harness/check_card_boot.sh [--build-dir=DIR]
# Exit 0 PASS, 1 FAIL or VACUOUS, 64 SKIP (binary, disc or fixture missing).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
        *) echo "card-boot: unknown argument $a" >&2; exit 64;;
    esac
done

GAME=spyro-3
pg_resolve "$GAME"
FIXTURE="tests/golden/$GAME/card-v6-blank.mcd"
LIMIT=3000

[[ -x "$PG_BIN" && -f "$PG_DISC" ]] ||
    { echo "card-boot: SKIP (binary or disc missing)"; exit 64; }
[[ "$(stat -c %s "$FIXTURE" 2>/dev/null || echo 0)" == 131072 ]] ||
    { echo "card-boot: SKIP (no card fixture $FIXTURE; scripts/capture_psx_oracle_ds.sh --game $GAME --recipe games/$GAME/oracle-card.conf writes it)"; exit 64; }

OUT="${PG_BUILD_DIR:-build/linux-release}/card-boot/$GAME"
mkdir -p "$OUT"
CARD="$OUT/card1.mcr"
LOG="$OUT/boot.log"
COUNTS="$OUT/counts.txt"
rm -f "${OUT:?}/card1.mcr" "${OUT:?}/card1.mcr.modset" "${OUT:?}/boot.log" "${OUT:?}/counts.txt"
cp "$FIXTURE" "$CARD"

# The census lines are counted, not stored; every other line is the log.
rc="$(
    set -o pipefail
    env OPENPETE_VIBRATION=0 OPENPETE_BIOS_CENSUS=2 timeout 900 \
        "$PG_BIN" --headless --no-config --disc "$PG_DISC" --disc-cache 0 \
        --no-log-file --mute 1 --vibration 0 \
        --card1 "$CARD" --no-card2 --frame-limit "$LIMIT" </dev/null 2>&1 |
    awk -v logf="$LOG" -v counts="$COUNTS" '
        /0xB0\(0x32\)/ { opens++; next }
        /0xB0\(0x33\)/ { lseeks++; next }
        /\] bios: 0x[ABC]0\(/ { next }
        { print > logf }
        END { printf "%d %d\n", opens, lseeks > counts }'
    echo "${PIPESTATUS[0]}"
)"
[[ -f "$COUNTS" && -f "$LOG" ]] ||
    { echo "[card-boot] game=$GAME verdict=FAIL: the run left no log (exit $rc)"; exit 1; }
read -r opens lseeks < "$COUNTS"
wvbl="$(sed -n 's/.*openpete_vbl=\([0-9]*\).*/\1/p' "$LOG" | head -n 1)"
head="[card-boot] game=$GAME exit=$rc opens=$opens lseeks=$lseeks watchdog_vbl=${wvbl:--}"
if [[ "$rc" != 0 ]]; then
    echo "$head verdict=FAIL"
    grep -iE 'watchdog|hang' "$LOG" | head -n 3 | sed 's/^/[card-boot]   /'
    exit 1
fi
if (( opens == 0 )); then
    echo "$head verdict=VACUOUS: the title's card probe opened no file"
    exit 1
fi
echo "$head verdict=PASS"
exit 0
