#!/usr/bin/env bash
# check_card_boot.sh - Spyro 2 and Spyro 3 reach the memory card through the
# kernel's bu file device with a formatted card in port A and port B empty.
#
# The games link PsyQ libmcrd and reach the card through B(32h) open,
# B(33h) lseek, B(34h) read, B(35h) write and B(36h) close on the bu device,
# with _bu_init (A(70h)) loading the directory inside the call.
#
# Fixture: tests/golden/spyro-3/card-v6-blank.mcd, the card the DuckStation
# fork formats, exported before the first frame by
#   scripts/capture_psx_oracle_ds.sh --game spyro-3 \
#       --recipe games/spyro-3/oracle-card.conf
# (generated, never committed; a blank formatted card, so both games use
# it). Each row copies it to a scratch card under its own output directory
# and deletes the row's outputs first; the run writes through to that copy.
#
# Rows:
#   boot  Spyro 3, no movie, --frame-limit 3573. libmcrd retries lseek until
#         it returns the requested offset, so a model that answers lseek with
#         0 spins until the hang watchdog ends the run (exit 2).
#         Instrument: the census lines of B(32h) (opens) and B(33h)
#         (lseeks), OPENPETE_BIOS_CENSUS=2 streamed through a counter (a
#         spinning lseek writes one line per call).
#         Pass: exit 0 with opens >= 1 (engagement: the title's card probe
#         ran); opens = 0 is VACUOUS.
#   save  Spyro 2, games/spyro-2/bk2/s2-create-save.bk2 (a new game that
#         creates and writes its save), --frame-limit 1400.
#         Instrument: the card window (OPENPETE_CARD_WINDOW: every bu call
#         with its result, every card command the vblank step starts) and
#         the card image after the run.
#         Pass: exit 0; an open with O_CREAT returns an fd and no O_CREAT
#         refusal is logged; the image's directory holds
#         "BASCUS-94425SP2RR" with state 0x51, size 8192 and a valid
#         checksum; 40 distinct sectors of the file's block are written;
#         a read of 0x1400 bytes from that fd returns 0 and its 40 sectors
#         are read. Engagement: bu calls (B(32h)-B(36h)) seen; 0 is
#         VACUOUS.
#   save-ref  The save row's movie on both sides: openpete against the
#         DuckStation fork's capture along it (games/spyro-2/oracle-card-save.conf,
#         consume-keyed, the fork one VBL early). The capture is generated on
#         the first run (needs scripts/setup_duckstation.sh).
#         Instrument: the oracle's per-tick region compare over the capture's
#         records (tests/harness/check_psx_oracle.sh, every record compared),
#         the card image each side left after the last record, and the card
#         command census of each side (openpete OPENPETE_CARD_WINDOW, the
#         fork's <golden>.cardwin).
#         Pass: the verifier's PASS with 0 hard failures (a cd_load_transient
#         region may differ only while a load re-converges); the two card
#         images are byte-identical;
#         each side wrote the sectors of the save's block (Spyro 2: 40 of
#         BASCUS-94425SP2RR; Spyro 3: 59 of BASCUS-94467SPY3_1). Engagement:
#         a side that wrote none of them is VACUOUS. Run by name only
#         (--row=save-ref, --game=spyro-2 or spyro-3); ctest
#         card_save_reference (Spyro 2) and card_save_reference_s3. Spyro 3
#         runs games/spyro-3/oracle-card-save.conf along
#         games/spyro-3/bk2/s3-create-save.bk2.
#   save-ref-clock  Spyro 2's save-ref along 600 records under the clock
#         row (OPENPETE_ORACLE_CLOCK=1 OPENPETE_ORACLE_GTE_STRICT=1), on its
#         own capture (games/spyro-2/oracle-card-save-clock.conf): past the
#         save, the level it returns to takes a VBLANK inside the game tick
#         that only the clock row delivers where the reference took it.
#         Instrument and pass as save-ref, plus the clock row's own
#         engagement (an in-work VBLANK delivered, exit 3 otherwise). Run by
#         name only; ctest card_save_reference_clock.
#
# Verdict lines:
#   [card-boot] row=boot game=spyro-3 exit=<rc> opens=<n> lseeks=<n> watchdog_vbl=<v|-> verdict=...
#   [card-boot] row=save game=spyro-2 exit=<rc> bu_calls=<n> creat=<fd|-1|-> ... verdict=...
#   [card-boot] row=save-ref game=<game> records=<n> verifier=<PASS|FAIL> hard_failed=<n> ... card_bytes_differ=<n> written=<op>/<ref> verdict=...
#   [card-boot] row=save-ref-clock game=spyro-2 records=<n> ... verdict=...
#
# Usage: tests/harness/check_card_boot.sh [--build-dir=DIR] [--row=boot|save|save-ref|save-ref-clock]
#        [--game=spyro-2|spyro-3] (the save-ref row's game; default spyro-2)
# Exit 0 PASS, 1 FAIL or VACUOUS, 64 SKIP (binary, disc, movie or fixture missing).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

ROWS=(boot save)
SAVE_REF_GAME=spyro-2
for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
        --game=*) SAVE_REF_GAME="${a#--game=}";;
        --row=*) ROWS=("${a#--row=}");;
        *) echo "card-boot: unknown argument $a" >&2; exit 64;;
    esac
done

FIXTURE="tests/golden/spyro-3/card-v6-blank.mcd"
[[ "$(stat -L -c %s "$FIXTURE" 2>/dev/null || echo 0)" == 131072 ]] ||
    { echo "card-boot: SKIP (no card fixture $FIXTURE; scripts/capture_psx_oracle_ds.sh --game spyro-3 --recipe games/spyro-3/oracle-card.conf writes it)"; exit 64; }

# row_boot: the Spyro 3 title's card probe. Prints the verdict line; returns 0, 1 or 64.
row_boot() {
    local GAME=spyro-3 LIMIT=3573
    pg_resolve "$GAME"
    [[ -x "$PG_BIN" && -f "$PG_DISC" ]] ||
        { echo "[card-boot] row=boot SKIP (binary or disc missing)"; return 64; }
    local OUT="${PG_BUILD_DIR:-build/linux-release}/card-boot/$GAME"
    mkdir -p "$OUT"
    local CARD="$OUT/card1.mcr" LOG="$OUT/boot.log" COUNTS="$OUT/counts.txt"
    rm -f "${OUT:?}/card1.mcr" "${OUT:?}/card1.mcr.modset" "${OUT:?}/boot.log" "${OUT:?}/counts.txt"
    cp "$FIXTURE" "$CARD"
    # The census lines are counted, not stored; every other line is the log.
    local rc
    rc="$(
        set -o pipefail
        env OPENPETE_VIBRATION=0 OPENPETE_BIOS_CENSUS=2 timeout -k 10 900 \
            "$PG_BIN" --game "$GAME" --headless --no-config --disc "$PG_DISC" --disc-cache 0 \
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
        { echo "[card-boot] row=boot game=$GAME verdict=FAIL: the run left no log (exit $rc)"; return 1; }
    local opens lseeks wvbl head
    read -r opens lseeks < "$COUNTS"
    wvbl="$(sed -n 's/.*openpete_vbl=\([0-9]*\).*/\1/p' "$LOG" | head -n 1)"
    head="[card-boot] row=boot game=$GAME exit=$rc opens=$opens lseeks=$lseeks watchdog_vbl=${wvbl:--}"
    if [[ "$rc" != 0 ]]; then
        echo "$head verdict=FAIL"
        grep -iE 'watchdog|hang' "$LOG" | head -n 3 | sed 's/^/[card-boot]   /'
        return 1
    fi
    if (( opens == 0 )); then
        echo "$head verdict=VACUOUS: the title's card probe opened no file"
        return 1
    fi
    echo "$head verdict=PASS"
    return 0
}

# row_save: Spyro 2 creates, writes and reads back its save. Prints the verdict line; returns 0, 1 or 64.
row_save() {
    local GAME=spyro-2 LIMIT=1400 MOVIE=games/spyro-2/bk2/s2-create-save.bk2
    pg_resolve "$GAME"
    [[ -x "$PG_BIN" && -f "$PG_DISC" && -f "$MOVIE" ]] ||
        { echo "[card-boot] row=save SKIP (binary, disc or movie missing)"; return 64; }
    local OUT="${PG_BUILD_DIR:-build/linux-release}/card-boot/$GAME"
    mkdir -p "$OUT"
    local CARD="$OUT/card1.mcr" LOG="$OUT/save.log" CW="$OUT/card-window.txt"
    rm -f "${OUT:?}/card1.mcr" "${OUT:?}/card1.mcr.modset" "${OUT:?}/save.log" "${OUT:?}/card-window.txt"
    cp "$FIXTURE" "$CARD"
    op_own_run env OPENPETE_VIBRATION=0 OPENPETE_CARD_WINDOW="$CW" timeout -k 10 900 \
        "$PG_BIN" --game "$GAME" --headless --no-config --disc "$PG_DISC" --disc-cache 0 \
        --no-log-file --mute 1 --vibration 0 --bk2 "$MOVIE" \
        --card1 "$CARD" --no-card2 --frame-limit "$LIMIT" </dev/null > "$LOG" 2>&1
    local rc=$?
    [[ -f "$LOG" && -f "$CW" && -f "$CARD" ]] ||
        { echo "[card-boot] row=save game=$GAME verdict=FAIL: the run left no log, card window or card (exit $rc)"; return 1; }
    local refused=0
    grep -q 'O_CREAT.*refused' "$LOG" && refused=1
    python3 - "$CW" "$CARD" "$rc" "$refused" <<'PY'
import struct, sys
cw, card, rc, refused = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
rows = [l.split() for l in open(cw) if l[:1] in 'BC']
bu = [r for r in rows if r[0] == 'B' and r[3] in ('32', '33', '34', '35', '36')]
creat = [r for r in bu if r[3] == '32' and int(r[5], 16) & 0x200]
fd = int(creat[-1][7], 16) if creat else None
fd_s = '-' if fd is None else ('-1' if fd == 0xFFFFFFFF else str(fd))
ok_fd = fd is not None and fd < 16
write = [r for r in bu if ok_fd and r[3] == '35' and int(r[4], 16) == fd and r[6] == '00001400' and r[7] == '00000000']
read = [r for r in bu if ok_fd and r[3] == '34' and int(r[4], 16) == fd and r[6] == '00001400' and r[7] == '00000000']
img = open(card, 'rb').read()
entry, first = None, None
for i in range(1, 16):
    f = img[i * 128:(i + 1) * 128]
    if f[10:30].split(b'\0')[0] == b'BASCUS-94425SP2RR':
        x = 0
        for c in f[:127]: x ^= c
        st, size, nxt = struct.unpack_from('<IIH', f)
        entry = (st, size, x == f[127])
        first = i - 1
        break
lo, hi = (64 + 64 * first, 64 + 64 * first + 40) if first is not None else (0, 0)
cmds = [r for r in rows if r[0] == 'C' and r[3] == '0']
wsec = {int(r[5]) for r in cmds if r[4] == '4' and lo <= int(r[5]) < hi}
rsec = {int(r[5]) for r in cmds if r[4] == '2' and lo <= int(r[5]) < hi}
ent = '-' if entry is None else '0x%X/%d/%s' % (entry[0], entry[1], 'ok' if entry[2] else 'bad')
head = ('[card-boot] row=save game=spyro-2 exit=%d bu_calls=%d creat=%s refused=%d entry=%s '
        'written=%d write_calls=%d read_sectors=%d read_calls=%d'
        % (rc, len(bu), fd_s, refused, ent, len(wsec), len(write), len(rsec), len(read)))
if not bu:
    print(head + ' verdict=VACUOUS: the run made no bu file call'); sys.exit(1)
ok = (rc == 0 and ok_fd and not refused and entry == (0x51, 8192, True)
      and len(wsec) == 40 and write and len(rsec) == 40 and read)
print(head + (' verdict=PASS' if ok else ' verdict=FAIL'))
sys.exit(0 if ok else 1)
PY
}

# row_save_ref ROW GAME RECIPE CLOCK: a save movie against the fork's capture along it. Prints the verdict line; returns 0, 1 or 64.
row_save_ref() {
    local ROW="$1" GAME="$2" RECIPE="$3" CLOCK="$4" SAVE_NAME SAVE_SECTORS
    case "$GAME" in
        spyro-2) SAVE_NAME=BASCUS-94425SP2RR; SAVE_SECTORS=40 ;;
        spyro-3) SAVE_NAME=BASCUS-94467SPY3_1; SAVE_SECTORS=59 ;;
        *) echo "[card-boot] row=$ROW game=$GAME: no save reference for this game" >&2; return 1 ;;
    esac
    pg_resolve "$GAME"
    [[ -x "$PG_BIN" && -f "$PG_DISC" ]] ||
        { echo "[card-boot] row=$ROW SKIP (binary or disc missing)"; return 64; }
    local GOLDEN CARD1_END
    GOLDEN="$(bash -c 'source "$1" >/dev/null 2>&1; echo "$GOLDEN"' _ "$RECIPE")"
    CARD1_END="$(bash -c 'source "$1" >/dev/null 2>&1; echo "$CARD1_END"' _ "$RECIPE")"
    local OUT="${PG_BUILD_DIR:-build/linux-release}/card-boot/$GAME"
    mkdir -p "$OUT"
    local LOG="$OUT/$ROW.log" CW="$OUT/$ROW-card-window.txt" CARD="$OUT/$ROW-card1.mcr"
    rm -f "${OUT:?}/$ROW.log" "${OUT:?}/$ROW-card-window.txt" "${OUT:?}/$ROW-card1.mcr" "${OUT:?}/$ROW.out"
    local clock_env=()
    [[ "$CLOCK" == 1 ]] && clock_env=(OPENPETE_ORACLE_CLOCK=1 OPENPETE_ORACLE_GTE_STRICT=1)
    # Every record is compared (keep going), so the run reaches the capture's
    # end and the card it leaves is the whole save.
    env OPENPETE_BIN="$PG_BIN" OPENPETE_CARD_WINDOW="$CW" OPENPETE_ORACLE_CARD_OUT="$CARD" \
        OPENPETE_ORACLE_LOG="$LOG" PSXTRACE_KEEP_GOING=1 "${clock_env[@]}" \
        tests/harness/check_psx_oracle.sh --game "$GAME" --recipe "$RECIPE" > "$OUT/$ROW.out" 2>&1
    local rc=$?
    (( rc == 64 )) && { echo "[card-boot] row=$ROW SKIP (no capture and DuckStation not built; see $OUT/$ROW.out)"; return 64; }
    [[ -f "$LOG" && -f "$CW" && -f "$CARD" && -f "$CARD1_END" && -f "$GOLDEN.cardwin" ]] ||
        { echo "[card-boot] row=$ROW game=$GAME verdict=FAIL: missing log, card window or card (oracle exit $rc; $OUT/$ROW.out)"; return 1; }
    python3 - "$LOG" "$CW" "$CARD" "$CARD1_END" "$GOLDEN.cardwin" "$rc" "$ROW" "$GAME" "$SAVE_NAME" "$SAVE_SECTORS" <<'PY'
import re, sys
log, cw, card, ref_card, ref_cw, rc, row, game, save_name, save_sectors = sys.argv[1:11]
save_sectors = int(save_sectors)
text = open(log, errors='replace').read()
m = re.findall(r'psxtrace: summary: records=(\d+) frames_compared=\d+ hash_mismatches=(\d+)', text)
records, mism = (int(m[-1][0]), int(m[-1][1])) if m else (0, -1)
div = sorted({int(x) for x in re.findall(r'DIVERGED[^\n]*?at frame=(\d+)', text)})
v = re.findall(r'psxtrace: verdict: (PASS|FAIL) \(hard_failed=(\d+)', text)
verdict, hard = (v[-1][0], int(v[-1][1])) if v else ('none', -1)
a, b = open(card, 'rb').read(), open(ref_card, 'rb').read()
diff = [i for i in range(min(len(a), len(b))) if a[i] != b[i]] + list(range(min(len(a), len(b)), max(len(a), len(b))))
# The save's block: the directory frame naming it gives the block index.
first = None
for i in range(1, 16):
    if b[i * 128 + 10:i * 128 + 30].split(b'\0')[0] == save_name.encode():
        first = i - 1
        break
lo, hi = (64 + 64 * first, 64 + 64 * first + 64) if first is not None else (0, 0)
opw = {int(r.split()[5]) for r in open(cw) if r[:2] == 'C ' and r.split()[3] == '0' and r.split()[4] == '4'}
refw = {int(r.split()[5]) for r in open(ref_cw) if r[:2] == 'C ' and r.split()[3] == '0' and r.split()[4] == '57'}
opn, refn = len({s for s in opw if lo <= s < hi}), len({s for s in refw if lo <= s < hi})
secs = sorted({i // 128 for i in diff})
head = ('[card-boot] row=%s game=%s records=%d verifier=%s hard_failed=%d hash_mismatches=%d '
        'first_differing=%s card_bytes_differ=%d card_sectors_differ=%s written=%d/%d oracle_exit=%s'
        % (row, game, records, verdict, hard, mism, div[0] if div else '-', len(diff),
           ','.join(map(str, secs[:12])) or '-', opn, refn, rc))
if opn == 0 or refn == 0:
    print(head + ' verdict=VACUOUS: a side wrote none of the save block'); sys.exit(1)
ok = (records > 0 and verdict == 'PASS' and hard == 0 and not diff
      and opn == save_sectors and refn == save_sectors and rc == '0')
print(head + (' verdict=PASS' if ok else ' verdict=FAIL'))
sys.exit(0 if ok else 1)
PY
}

# Exit 1 on any failing row, 64 when every row skipped, else 0.
failed=0; ran=0
for row in "${ROWS[@]}"; do
    case "$row" in
        boot) row_boot; r=$? ;;
        save) row_save; r=$? ;;
        save-ref) row_save_ref save-ref "$SAVE_REF_GAME" "games/$SAVE_REF_GAME/oracle-card-save.conf" 0; r=$? ;;
        save-ref-clock) row_save_ref save-ref-clock spyro-2 games/spyro-2/oracle-card-save-clock.conf 1; r=$? ;;
        *) echo "card-boot: unknown row $row" >&2; exit 64 ;;
    esac
    (( r == 1 )) && failed=1
    (( r != 64 )) && ran=1
done
(( failed )) && exit 1
(( ran )) || exit 64
exit 0
