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
#   format-ref  Spyro 2 formats an unformatted card in slot 1 and saves to
#         it: openpete against the fork's capture along
#         games/spyro-2/bk2/s2-format-card.bk2 (consume-keyed, the fork one
#         VBL early; games/spyro-2/oracle-card-format.conf, 500 records,
#         ending before the in-work VBLANK of record 502). The row writes the
#         unformatted card (131072 zero bytes, the recipe's CARD1_FIXTURE)
#         before the run; the capture is generated on the first run.
#         Instrument: the oracle's per-tick compare over every record, the
#         card image each side left, and the slot 1 card commands of each
#         side (openpete OPENPETE_CARD_WINDOW, the fork's <golden>.cardwin).
#         Pass: the verifier's PASS with 0 hard failures and oracle exit 0;
#         the two card images byte-identical. Engagement: a side that issued
#         no card command or wrote no sector is VACUOUS. Run by name only;
#         ctest card_format_reference.
#   format-ref-clock  format-ref along 800 records under the clock row
#         (OPENPETE_ORACLE_CLOCK=1 OPENPETE_ORACLE_GTE_STRICT=1) on its own
#         capture (games/spyro-2/oracle-card-format-clock.conf): from record
#         502 the level takes a VBLANK inside the game tick that only the
#         clock row delivers where the reference took it. Pass as
#         format-ref; engagement as format-ref plus in-work VBLANKs delivered
#         (the clock summary's vblpos delivered) and GTE ops hashed, zero
#         VACUOUS. Run by name only; ctest card_format_reference_clock.
#   full-save-ref-clock  Spyro 2's save movie on a full card (15 foreign
#         one-block saves, tests/harness/make_full_card.py over the blank
#         fixture, written before each run) along 520 records under the
#         clock row (OPENPETE_ORACLE_CLOCK=1 OPENPETE_ORACLE_GTE_STRICT=1),
#         on its own capture (games/spyro-2/oracle-card-full-save-clock.conf).
#         The game finds no free block and creates no save; past the card
#         step the level takes a VBLANK inside the game tick that only the
#         clock row delivers where the reference took it.
#         Instrument: the oracle's per-tick compare over every record, the
#         card image each side left, and the slot 1 card commands of each
#         side (openpete OPENPETE_CARD_WINDOW, the fork's <golden>.cardwin).
#         Pass: the verifier's PASS with 0 hard failures and oracle exit 0;
#         the two card images byte-identical; the 15 foreign directory
#         frames and blocks unchanged and no BASCUS-94425SP2RR frame in
#         either image; the sectors each side wrote are the same set, all
#         in block 0. Engagement: a side that issued no card command or
#         wrote no sector, no in-work VBLANK delivered, or no GTE op hashed
#         is VACUOUS. Run by name only; ctest card_full_save_reference_clock.
#   load-ref  Boot with the save present and load it: openpete against the
#         fork's capture along games/<game>/bk2/s2-load-save.bk2 or
#         s3-load-save.bk2 (games/<game>/oracle-card-load.conf), both sides
#         booting the card the save-ref capture left (the reference's
#         exported start image). The capture is generated on the first run.
#         Instrument: the oracle's per-tick compare, the card image each
#         side left, openpete's card window (B calls, C reads, R: the span
#         and hash of each finished asynchronous read) and the reference's
#         card commands (<golden>.cardwin).
#         Pass: the verifier's PASS with 0 hard failures; the card images
#         byte-identical; every open of the save returns fd 2; each read
#         step runs one lseek returning its offset and one read returning
#         0; each read's sectors are the file span's, in order, n / 128 of
#         them; the bytes each read delivers hash equal to the card image's
#         span; the reference reads the same sectors in the same calls, at
#         the same per-sector VBL deltas and at the reference VBLANKs
#         openpete's commands are keyed on. Engagement: no read of the save
#         is VACUOUS. Run by name only; ctest card_load_reference (Spyro 2)
#         and card_load_reference_s3.
#   load-ref-clock  Spyro 2's load-ref into the level, 1300 records under
#         the clock row (OPENPETE_ORACLE_CLOCK=1 OPENPETE_ORACLE_GTE_STRICT=1)
#         on its own capture (games/spyro-2/oracle-card-load-clock.conf): the
#         level takes VBLANKs inside the game tick that only the clock row
#         delivers where the reference took them. Instrument and pass as
#         load-ref. Run by name only; ctest card_load_reference_clock.
#   autosave  Spyro 2's story-step autosave on leaving Glimmer for Summer
#         Forest: games/spyro-2/bk2/s2-create-save.bk2 (New Game on the blank
#         card, which creates the save, selects it and reaches Glimmer), with
#         --skip-to-cutscene summer_forest_story taking Glimmer's exit on its
#         first live tick; the game then plays the story cutscene, runs the
#         next story step's loading loop (func_80019364, back-edge header
#         0x800194F8), whose autosave waits on libmcrd's done flag that only
#         a VBL's slot-7 callback sets, and loads Summer Forest.
#         --frame-limit 3600. A second run of the movie alone to
#         --frame-limit 1400 (the save row's) leaves the card as it stands
#         before the autosave. Instrument: the exit status and the watchdog's
#         trip address, the loop-site census line of 0x800194F8 (quanta: VBLs
#         delivered at the back-edge), the card window (B(33h) lseek and
#         B(35h) write of the autosave, the vblank step's sector writes) and
#         the two card images.
#         Pass: exit 0; quanta > 0 at 0x800194F8; a B(35h) write of 0x600
#         bytes that the bu device accepts (result 0, the asynchronous start)
#         after an lseek to 0x200 + idx * 0x600; the 12 sectors of that span
#         in the save's block written; and the span of the end image differs
#         from the pre-autosave image. Engagement: the skip's exit call logged
#         with the save selected (0x80067E20 1) and the story cutscene
#         released (k 4); without them VACUOUS. A wait that never sees a VBL
#         trips the hang watchdog (exit 2, trip 0x80070AD0, the autosave's
#         MemCardSync state). Run by name only (--row=autosave); ctest
#         card_autosave.
#
# Verdict lines:
#   [card-boot] row=boot game=spyro-3 exit=<rc> opens=<n> lseeks=<n> watchdog_vbl=<v|-> verdict=...
#   [card-boot] row=save game=spyro-2 exit=<rc> bu_calls=<n> creat=<fd|-1|-> ... verdict=...
#   [card-boot] row=save-ref game=<game> records=<n> verifier=<PASS|FAIL> hard_failed=<n> ... card_bytes_differ=<n> written=<op>/<ref> verdict=...
#   [card-boot] row=save-ref-clock game=spyro-2 records=<n> ... verdict=...
#   [card-boot] row=format-ref|format-ref-clock game=spyro-2 records=<n> verifier=<PASS|FAIL> hard_failed=<n> ... card_bytes_differ=<n> commands=<op>/<ref> written=<op>/<ref> [vblpos_delivered=<n> gte_ops=<n>] verdict=...
#   [card-boot] row=full-save-ref-clock game=spyro-2 records=<n> verifier=<PASS|FAIL> hard_failed=<n> ... card_bytes_differ=<n> commands=<op>/<ref> written=<op sectors>/<ref sectors> foreign_kept=<op>/<ref> save_frames=<op>/<ref> vblpos_delivered=<n> gte_ops=<n> verdict=...
#   [card-boot] row=load-ref|load-ref-clock game=<game> records=<n> ... fd=<fds> reads=<bytes@offset:sectors/cadence,...> ref_sectors=<n> ... verdict=...
#   [card-boot] row=autosave game=spyro-2 exit=<rc> trip=<addr|-> selected=<0|1|-> released=<0|1> quanta=<n> write=<bytes@offset:result|-> written=<n>/12 changed=<n> verdict=...
#
# Usage: tests/harness/check_card_boot.sh [--build-dir=DIR] [--row=boot|save|save-ref|save-ref-clock|format-ref|format-ref-clock|full-save-ref-clock|load-ref|load-ref-clock|autosave]
#        [--game=spyro-2|spyro-3] (the save-ref and load-ref rows' game; default spyro-2)
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

# row_format_ref ROW RECIPE CLOCK: the Spyro 2 format movie against the fork's capture along it. Prints the verdict line; returns 0, 1 or 64.
row_format_ref() {
    local ROW="$1" RECIPE="$2" CLOCK="$3" GAME=spyro-2
    pg_resolve "$GAME"
    [[ -x "$PG_BIN" && -f "$PG_DISC" ]] ||
        { echo "[card-boot] row=$ROW SKIP (binary or disc missing)"; return 64; }
    local GOLDEN CARD1_FIXTURE CARD1_END
    GOLDEN="$(bash -c 'source "$1" >/dev/null 2>&1; echo "$GOLDEN"' _ "$RECIPE")"
    CARD1_FIXTURE="$(bash -c 'source "$1" >/dev/null 2>&1; echo "$CARD1_FIXTURE"' _ "$RECIPE")"
    CARD1_END="$(bash -c 'source "$1" >/dev/null 2>&1; echo "$CARD1_END"' _ "$RECIPE")"
    [[ -n "$CARD1_FIXTURE" ]] ||
        { echo "[card-boot] row=$ROW game=$GAME verdict=FAIL: $RECIPE sets no CARD1_FIXTURE"; return 1; }
    # The unformatted card both sides boot, written fresh each run.
    rm -f "${CARD1_FIXTURE:?}"
    mkdir -p "$(dirname "$CARD1_FIXTURE")"
    head -c 131072 /dev/zero > "$CARD1_FIXTURE"
    local OUT="${PG_BUILD_DIR:-build/linux-release}/card-boot/$GAME"
    mkdir -p "$OUT"
    local LOG="$OUT/$ROW.log" CW="$OUT/$ROW-card-window.txt" CARD="$OUT/$ROW-card1.mcr"
    rm -f "${OUT:?}/$ROW.log" "${OUT:?}/$ROW-card-window.txt" "${OUT:?}/$ROW-card1.mcr" "${OUT:?}/$ROW.out"
    local clock_env=()
    [[ "$CLOCK" == 1 ]] && clock_env=(OPENPETE_ORACLE_CLOCK=1 OPENPETE_ORACLE_GTE_STRICT=1)
    env OPENPETE_BIN="$PG_BIN" OPENPETE_CARD_WINDOW="$CW" OPENPETE_ORACLE_CARD_OUT="$CARD" \
        OPENPETE_ORACLE_LOG="$LOG" PSXTRACE_KEEP_GOING=1 "${clock_env[@]}" \
        tests/harness/check_psx_oracle.sh --game "$GAME" --recipe "$RECIPE" > "$OUT/$ROW.out" 2>&1
    local rc=$?
    (( rc == 64 )) && { echo "[card-boot] row=$ROW SKIP (no capture and DuckStation not built; see $OUT/$ROW.out)"; return 64; }
    [[ -f "$LOG" && -f "$CW" && -f "$CARD" && -f "$CARD1_END" && -f "$GOLDEN.cardwin" ]] ||
        { echo "[card-boot] row=$ROW game=$GAME verdict=FAIL: missing log, card window or card (oracle exit $rc; $OUT/$ROW.out)"; return 1; }
    python3 - "$LOG" "$CW" "$CARD" "$CARD1_END" "$GOLDEN.cardwin" "$rc" "$ROW" "$GAME" "$CLOCK" <<'PY'
import re, sys
log, cw, card, ref_card, ref_cw, rc, row, game, clock = sys.argv[1:10]
text = open(log, errors='replace').read()
m = re.findall(r'psxtrace: summary: records=(\d+) frames_compared=\d+ hash_mismatches=(\d+)', text)
records, mism = (int(m[-1][0]), int(m[-1][1])) if m else (0, -1)
div = sorted({int(x) for x in re.findall(r'DIVERGED[^\n]*?at frame=(\d+)', text)})
v = re.findall(r'psxtrace: verdict: (PASS|FAIL) \(hard_failed=(\d+)', text)
verdict, hard = (v[-1][0], int(v[-1][1])) if v else ('none', -1)
a, b = open(card, 'rb').read(), open(ref_card, 'rb').read()
diff = [i for i in range(min(len(a), len(b))) if a[i] != b[i]] + list(range(min(len(a), len(b)), max(len(a), len(b))))
secs = sorted({i // 128 for i in diff})
# Slot 1 card commands: openpete's C rows carry port 0 and command 2 (read)
# or 4 (write); the fork's carry port 0 and the command byte (0x57 write).
op = [r.split() for r in open(cw) if r[:2] == 'C ' and r.split()[3] == '0']
ref = [r.split() for r in open(ref_cw) if r[:2] == 'C ' and r.split()[3] == '0']
opw, refw = {int(r[5]) for r in op if r[4] == '4'}, {int(r[5]) for r in ref if r[4] == '57'}
head = ('[card-boot] row=%s game=%s records=%d verifier=%s hard_failed=%d hash_mismatches=%d '
        'first_differing=%s card_bytes_differ=%d card_sectors_differ=%s commands=%d/%d written=%d/%d'
        % (row, game, records, verdict, hard, mism, div[0] if div else '-', len(diff),
           ','.join(map(str, secs[:12])) or '-', len(op), len(ref), len(opw), len(refw)))
vacuous = []
if not op or not ref:
    vacuous.append('a side issued no card command')
if not opw or not refw:
    vacuous.append('a side wrote no sector')
if clock == '1':
    c = re.findall(r'psxtrace: clock: vblpos delivered=(\d+)', text)
    g = re.findall(r'psxtrace: gte: records=\d+ mismatches=\d+ ops_hashed=(\d+)', text)
    deliv, ops = (int(c[-1]) if c else 0), (int(g[-1]) if g else 0)
    head += ' vblpos_delivered=%d gte_ops=%d' % (deliv, ops)
    if deliv == 0:
        vacuous.append('no in-work VBLANK delivered')
    if ops == 0:
        vacuous.append('no GTE op hashed')
head += ' oracle_exit=%s' % rc
if vacuous:
    print(head + ' verdict=VACUOUS: ' + '; '.join(vacuous)); sys.exit(1)
ok = records > 0 and verdict == 'PASS' and hard == 0 and not diff and rc == '0'
print(head + (' verdict=PASS' if ok else ' verdict=FAIL'))
sys.exit(0 if ok else 1)
PY
}

# row_full_save_ref ROW RECIPE: the Spyro 2 save movie on a full card against the fork's capture along it, under the clock row. Prints the verdict line; returns 0, 1 or 64.
row_full_save_ref() {
    local ROW="$1" RECIPE="$2" GAME=spyro-2
    pg_resolve "$GAME"
    [[ -x "$PG_BIN" && -f "$PG_DISC" ]] ||
        { echo "[card-boot] row=$ROW SKIP (binary or disc missing)"; return 64; }
    local GOLDEN CARD1_FIXTURE CARD1_END
    GOLDEN="$(bash -c 'source "$1" >/dev/null 2>&1; echo "$GOLDEN"' _ "$RECIPE")"
    CARD1_FIXTURE="$(bash -c 'source "$1" >/dev/null 2>&1; echo "$CARD1_FIXTURE"' _ "$RECIPE")"
    CARD1_END="$(bash -c 'source "$1" >/dev/null 2>&1; echo "$CARD1_END"' _ "$RECIPE")"
    [[ -n "$CARD1_FIXTURE" ]] ||
        { echo "[card-boot] row=$ROW game=$GAME verdict=FAIL: $RECIPE sets no CARD1_FIXTURE"; return 1; }
    # The full card both sides boot, regenerated from the blank fixture each run.
    rm -f "${CARD1_FIXTURE:?}"
    mkdir -p "$(dirname "$CARD1_FIXTURE")"
    python3 tests/harness/make_full_card.py "$FIXTURE" "$CARD1_FIXTURE" 15 ||
        { echo "[card-boot] row=$ROW game=$GAME verdict=FAIL: make_full_card.py could not write $CARD1_FIXTURE"; return 1; }
    local OUT="${PG_BUILD_DIR:-build/linux-release}/card-boot/$GAME"
    mkdir -p "$OUT"
    local LOG="$OUT/$ROW.log" CW="$OUT/$ROW-card-window.txt" CARD="$OUT/$ROW-card1.mcr"
    rm -f "${OUT:?}/$ROW.log" "${OUT:?}/$ROW-card-window.txt" "${OUT:?}/$ROW-card1.mcr" "${OUT:?}/$ROW.out"
    env OPENPETE_BIN="$PG_BIN" OPENPETE_CARD_WINDOW="$CW" OPENPETE_ORACLE_CARD_OUT="$CARD" \
        OPENPETE_ORACLE_LOG="$LOG" PSXTRACE_KEEP_GOING=1 \
        OPENPETE_ORACLE_CLOCK=1 OPENPETE_ORACLE_GTE_STRICT=1 \
        tests/harness/check_psx_oracle.sh --game "$GAME" --recipe "$RECIPE" > "$OUT/$ROW.out" 2>&1
    local rc=$?
    (( rc == 64 )) && { echo "[card-boot] row=$ROW SKIP (no capture and DuckStation not built; see $OUT/$ROW.out)"; return 64; }
    [[ -f "$LOG" && -f "$CW" && -f "$CARD" && -f "$CARD1_END" && -f "$GOLDEN.cardwin" ]] ||
        { echo "[card-boot] row=$ROW game=$GAME verdict=FAIL: missing log, card window or card (oracle exit $rc; $OUT/$ROW.out)"; return 1; }
    python3 - "$LOG" "$CW" "$CARD" "$CARD1_END" "$GOLDEN.cardwin" "$CARD1_FIXTURE" "$rc" "$ROW" "$GAME" <<'PY'
import re, sys
log, cw, card, ref_card, ref_cw, fixture, rc, row, game = sys.argv[1:10]
text = open(log, errors='replace').read()
m = re.findall(r'psxtrace: summary: records=(\d+) frames_compared=\d+ hash_mismatches=(\d+)', text)
records, mism = (int(m[-1][0]), int(m[-1][1])) if m else (0, -1)
div = sorted({int(x) for x in re.findall(r'DIVERGED[^\n]*?at frame=(\d+)', text)})
v = re.findall(r'psxtrace: verdict: (PASS|FAIL) \(hard_failed=(\d+)', text)
verdict, hard = (v[-1][0], int(v[-1][1])) if v else ('none', -1)
a, b, fx = open(card, 'rb').read(), open(ref_card, 'rb').read(), open(fixture, 'rb').read()
diff = [i for i in range(min(len(a), len(b))) if a[i] != b[i]] + list(range(min(len(a), len(b)), max(len(a), len(b))))
secs = sorted({i // 128 for i in diff})
# Directory frames 1..15 and blocks 1..15 of the fixture are the foreign
# saves; an image keeps one when both its frame and its block are unchanged.
def kept(img):
    return sum(1 for n in range(1, 16)
               if img[n * 128:(n + 1) * 128] == fx[n * 128:(n + 1) * 128]
               and img[n * 8192:(n + 1) * 8192] == fx[n * 8192:(n + 1) * 8192])
def save_frames(img):
    return sum(1 for n in range(1, 16)
               if img[n * 128 + 10:n * 128 + 30].split(b'\0')[0] == b'BASCUS-94425SP2RR')
# Slot 1 card commands: openpete's C rows carry port 0 and command 2 (read)
# or 4 (write); the fork's carry port 0 and the command byte (0x57 write).
op = [r.split() for r in open(cw) if r[:2] == 'C ' and r.split()[3] == '0']
ref = [r.split() for r in open(ref_cw) if r[:2] == 'C ' and r.split()[3] == '0']
opw, refw = {int(r[5]) for r in op if r[4] == '4'}, {int(r[5]) for r in ref if r[4] == '57'}
c = re.findall(r'psxtrace: clock: vblpos delivered=(\d+)', text)
g = re.findall(r'psxtrace: gte: records=\d+ mismatches=\d+ ops_hashed=(\d+)', text)
deliv, ops = (int(c[-1]) if c else 0), (int(g[-1]) if g else 0)
ka, kb, sa, sb = kept(a), kept(b), save_frames(a), save_frames(b)
head = ('[card-boot] row=%s game=%s records=%d verifier=%s hard_failed=%d hash_mismatches=%d '
        'first_differing=%s card_bytes_differ=%d card_sectors_differ=%s commands=%d/%d '
        'written=%s/%s foreign_kept=%d/%d save_frames=%d/%d vblpos_delivered=%d gte_ops=%d oracle_exit=%s'
        % (row, game, records, verdict, hard, mism, div[0] if div else '-', len(diff),
           ','.join(map(str, secs[:12])) or '-', len(op), len(ref),
           ','.join(map(str, sorted(opw))) or '-', ','.join(map(str, sorted(refw))) or '-',
           ka, kb, sa, sb, deliv, ops, rc))
vacuous = []
if not op or not ref:
    vacuous.append('a side issued no card command')
if not opw or not refw:
    vacuous.append('a side wrote no sector')
if deliv == 0:
    vacuous.append('no in-work VBLANK delivered')
if ops == 0:
    vacuous.append('no GTE op hashed')
if vacuous:
    print(head + ' verdict=VACUOUS: ' + '; '.join(vacuous)); sys.exit(1)
ok = (records > 0 and verdict == 'PASS' and hard == 0 and not diff and rc == '0'
      and ka == 15 and kb == 15 and sa == 0 and sb == 0
      and opw == refw and all(s < 64 for s in opw))
print(head + (' verdict=PASS' if ok else ' verdict=FAIL'))
sys.exit(0 if ok else 1)
PY
}

# row_load_ref ROW GAME RECIPE CLOCK: the load movie against the fork's capture along it. Prints the verdict line; returns 0, 1 or 64.
row_load_ref() {
    local ROW="$1" GAME="$2" RECIPE="$3" CLOCK="$4" SAVE_NAME
    case "$GAME" in
        spyro-2) SAVE_NAME=BASCUS-94425SP2RR ;;
        spyro-3) SAVE_NAME=BASCUS-94467SPY3_1 ;;
        *) echo "[card-boot] row=$ROW game=$GAME: no load reference for this game" >&2; return 1 ;;
    esac
    pg_resolve "$GAME"
    [[ -x "$PG_BIN" && -f "$PG_DISC" ]] ||
        { echo "[card-boot] row=$ROW SKIP (binary or disc missing)"; return 64; }
    local GOLDEN CARD1_FIXTURE CARD1_END
    GOLDEN="$(bash -c 'source "$1" >/dev/null 2>&1; echo "$GOLDEN"' _ "$RECIPE")"
    CARD1_FIXTURE="$(bash -c 'source "$1" >/dev/null 2>&1; echo "$CARD1_FIXTURE"' _ "$RECIPE")"
    CARD1_END="$(bash -c 'source "$1" >/dev/null 2>&1; echo "$CARD1_END"' _ "$RECIPE")"
    local OUT="${PG_BUILD_DIR:-build/linux-release}/card-boot/$GAME"
    mkdir -p "$OUT"
    local LOG="$OUT/$ROW.log" CW="$OUT/$ROW-card-window.txt" CARD="$OUT/$ROW-card1.mcr"
    rm -f "${OUT:?}/$ROW.log" "${OUT:?}/$ROW-card-window.txt" "${OUT:?}/$ROW-card1.mcr" "${OUT:?}/$ROW.out"
    local clock_env=()
    [[ "$CLOCK" == 1 ]] && clock_env=(OPENPETE_ORACLE_CLOCK=1 OPENPETE_ORACLE_GTE_STRICT=1)
    env OPENPETE_BIN="$PG_BIN" OPENPETE_CARD_WINDOW="$CW" OPENPETE_ORACLE_CARD_OUT="$CARD" \
        OPENPETE_ORACLE_LOG="$LOG" PSXTRACE_KEEP_GOING=1 "${clock_env[@]}" \
        tests/harness/check_psx_oracle.sh --game "$GAME" --recipe "$RECIPE" > "$OUT/$ROW.out" 2>&1
    local rc=$?
    (( rc == 64 )) && { echo "[card-boot] row=$ROW SKIP (no capture and DuckStation not built; see $OUT/$ROW.out)"; return 64; }
    [[ -f "$LOG" && -f "$CW" && -f "$CARD" && -f "$CARD1_FIXTURE" && -f "$CARD1_END" && -f "$GOLDEN.cardwin" ]] ||
        { echo "[card-boot] row=$ROW game=$GAME verdict=FAIL: missing log, card window or card (oracle exit $rc; $OUT/$ROW.out)"; return 1; }
    python3 - "$LOG" "$CW" "$CARD" "$CARD1_FIXTURE" "$CARD1_END" "$GOLDEN.cardwin" "$rc" "$GAME" "$SAVE_NAME" "$ROW" <<'PY'
import re, sys
log, cw, card, fixture, ref_card, ref_cw, rc, game, save_name, row = sys.argv[1:11]
text = open(log, errors='replace').read()
m = re.findall(r'psxtrace: summary: records=(\d+) frames_compared=\d+ hash_mismatches=(\d+)', text)
records, mism = (int(m[-1][0]), int(m[-1][1])) if m else (0, -1)
v = re.findall(r'psxtrace: verdict: (PASS|FAIL) \(hard_failed=(\d+)', text)
verdict, hard = (v[-1][0], int(v[-1][1])) if v else ('none', -1)
img, a, b = open(fixture, 'rb').read(), open(card, 'rb').read(), open(ref_card, 'rb').read()
card_diff = sum(x != y for x, y in zip(a, b)) + abs(len(a) - len(b))
# The save's block chain from the directory: frame i names block i - 1,
# whose data is sectors 64 * i ..; the frame's u16 at 8 links the next block.
chain, i = [], None
for f in range(1, 16):
    if img[f * 128 + 10:f * 128 + 30].split(b'\0')[0] == save_name.encode():
        i = f - 1
        break
while i is not None and i < 15 and len(chain) < 15:
    chain.append(i)
    nxt = int.from_bytes(img[(i + 1) * 128 + 8:(i + 1) * 128 + 10], 'little')
    i = None if nxt == 0xFFFF else nxt
def sector(off):
    return 64 * (chain[off // 8192] + 1) + (off % 8192) // 128
def fnv(data):
    h = 0xCBF29CE484222325
    for c in data:
        h = ((h ^ c) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h
rows = [l.split() for l in open(cw) if l[:1] in 'BCR']
save_secs = {64 * (k + 1) + s for k in chain for s in range(64)}
# Per file: open, then (lseek, read) per read step, then close.
opens, fds, calls, bad = 0, set(), [], []
fd, off, cur = None, 0, None
for r in rows:
    if r[0] == 'B' and r[3] == '32':
        opens += 1
        fd = int(r[7], 16)
        fds.add(fd)
        cur = None
    elif r[0] == 'B' and r[3] == '33' and fd is not None and int(r[4], 16) == fd:
        if cur is not None and cur.get('lseeks', 0) and 'read' not in cur:
            bad.append('lseek twice before a read at VBL ' + r[1])
        if int(r[7], 16) != int(r[5], 16) or int(r[6], 16) != 0:
            bad.append('lseek at VBL %s returned 0x%s for 0x%s' % (r[1], r[7], r[5]))
        off = int(r[7], 16)
        cur = {'lseeks': 1, 'off': off}
    elif r[0] == 'B' and r[3] == '34' and fd is not None and int(r[4], 16) == fd:
        if cur is None or 'read' in cur:
            bad.append('read without its own lseek at VBL ' + r[1])
            cur = {'off': off}
        n = int(r[6], 16)
        if int(r[7], 16) != 0:
            bad.append('read at VBL %s returned 0x%s' % (r[1], r[7]))
        cur.update(read=n, buf=int(r[5], 16), cmds=[], vbl=int(r[1]))
        calls.append(cur)
    elif r[0] == 'C' and r[4] == '2' and int(r[5]) in save_secs and calls and 'done' not in calls[-1]:
        calls[-1]['cmds'].append((int(r[1]), int(r[5]), int(r[6])))
    elif r[0] == 'R' and calls and 'done' not in calls[-1]:
        calls[-1]['done'] = (int(r[4], 16), int(r[5]), int(r[6], 16))
# The reference's reads of the save's sectors, split by openpete's call lengths.
ref = [(int(r.split()[1]), int(r.split()[5])) for r in open(ref_cw)
       if r[:2] == 'C ' and r.split()[4] == '52' and int(r.split()[5]) in save_secs]
pos, summ = 0, []
for c in calls:
    want = [sector(c['off'] + 128 * k) for k in range(c['read'] // 128)]
    got = [s for _, s, _ in c['cmds']]
    span = b''.join(img[s * 128:(s + 1) * 128] for s in want)
    if got != want:
        bad.append('read at VBL %d: sectors %s, file span %s' % (c['vbl'], got[:4], want[:4]))
    if 'done' not in c:
        bad.append('read at VBL %d never finished' % c['vbl'])
    elif c['done'][1] != c['read'] or c['done'][2] != fnv(span):
        bad.append('read at VBL %d delivered %d bytes hash %016x, card span hash %016x'
                   % (c['vbl'], c['done'][1], c['done'][2], fnv(span)))
    r_rows = ref[pos:pos + len(got)]
    pos += len(got)
    op_d = [y[0] - x[0] for x, y in zip(c['cmds'], c['cmds'][1:])]
    ref_d = [y[0] - x[0] for x, y in zip(r_rows, r_rows[1:])]
    if [s for _, s in r_rows] != got:
        bad.append('read at VBL %d: reference sectors differ' % c['vbl'])
    if op_d != ref_d or [k for _, _, k in c['cmds']] != [v for v, _ in r_rows]:
        bad.append('read at VBL %d: cadence %s, reference %s' % (c['vbl'], sorted(set(op_d)), sorted(set(ref_d))))
    cad = '/'.join(map(str, sorted(set(op_d)))) or '-'
    summ.append('0x%x@0x%x:%ds/%s' % (c['read'], c['off'], len(got), cad))
if pos != len(ref):
    bad.append('the reference read %d save sectors, openpete %d' % (len(ref), pos))
head = ('[card-boot] row=%s game=%s records=%d verifier=%s hard_failed=%d hash_mismatches=%d '
        'card_bytes_differ=%d opens=%d fd=%s reads=%s ref_sectors=%d oracle_exit=%s'
        % (row, game, records, verdict, hard, mism, card_diff, opens, ','.join(map(str, sorted(fds))) or '-',
           ','.join(summ) or '-', len(ref), rc))
if not calls:
    print(head + ' verdict=VACUOUS: no asynchronous read of the save'); sys.exit(1)
ok = (records > 0 and verdict == 'PASS' and hard == 0 and card_diff == 0 and rc == '0'
      and fds == {2} and not bad)
print(head + (' verdict=PASS' if ok else ' verdict=FAIL'))
for x in bad[:8]:
    print('[card-boot]   ' + x)
sys.exit(0 if ok else 1)
PY
}

# row_autosave: Spyro 2's story-step autosave on leaving Glimmer. Prints the verdict line; returns 0, 1 or 64.
row_autosave() {
    local GAME=spyro-2 LIMIT=3600 PRE_LIMIT=1400 MOVIE=games/spyro-2/bk2/s2-create-save.bk2
    pg_resolve "$GAME"
    [[ -x "$PG_BIN" && -f "$PG_DISC" && -f "$MOVIE" ]] ||
        { echo "[card-boot] row=autosave SKIP (binary, disc or movie missing)"; return 64; }
    local OUT="${PG_BUILD_DIR:-build/linux-release}/card-boot/$GAME"
    mkdir -p "$OUT"
    local CARD="$OUT/autosave-card1.mcr" PRE="$OUT/autosave-pre-card1.mcr"
    local LOG="$OUT/autosave.log" PRE_LOG="$OUT/autosave-pre.log" CW="$OUT/autosave-card-window.txt"
    rm -f "${OUT:?}/autosave-card1.mcr" "${OUT:?}/autosave-card1.mcr.modset" \
        "${OUT:?}/autosave-pre-card1.mcr" "${OUT:?}/autosave-pre-card1.mcr.modset" \
        "${OUT:?}/autosave.log" "${OUT:?}/autosave-pre.log" "${OUT:?}/autosave-card-window.txt"
    cp "$FIXTURE" "$PRE"
    cp "$FIXTURE" "$CARD"
    local common=(--game "$GAME" --headless --no-config --disc "$PG_DISC" --disc-cache 0
                  --no-log-file --mute 1 --vibration 0 --bk2 "$MOVIE" --no-card2)
    op_own_run env OPENPETE_VIBRATION=0 timeout -k 10 900 \
        "$PG_BIN" "${common[@]}" --card1 "$PRE" --frame-limit "$PRE_LIMIT" </dev/null > "$PRE_LOG" 2>&1
    local pre_rc=$?
    op_own_run env OPENPETE_VIBRATION=0 OPENPETE_CARD_WINDOW="$CW" timeout -k 10 900 \
        "$PG_BIN" "${common[@]}" --card1 "$CARD" --skip-to-cutscene summer_forest_story \
        --frame-limit "$LIMIT" </dev/null > "$LOG" 2>&1
    local rc=$?
    [[ "$pre_rc" == 0 && -f "$PRE" ]] ||
        { echo "[card-boot] row=autosave game=$GAME verdict=FAIL: the pre-autosave run exited $pre_rc ($PRE_LOG)"; return 1; }
    [[ -f "$LOG" && -f "$CW" && -f "$CARD" ]] ||
        { echo "[card-boot] row=autosave game=$GAME verdict=FAIL: the run left no log, card window or card (exit $rc)"; return 1; }
    python3 - "$LOG" "$CW" "$PRE" "$CARD" "$rc" <<'PY'
import re, sys
log, cw, pre, card, rc = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5])
text = open(log, errors='replace').read()
trip = re.findall(r'hang detected:.*?trip addr=(0x[0-9A-Fa-f]+)', text)
sel = re.findall(r'summer_forest_story: save selected 0x80067E20 = (\d+)', text)
called = 'summer_forest_story: completion stores, func_80019928(0)' in text
released = re.search(r'level playable .* cutscene=summer_forest_story k=4 D_800681C8=6', text) is not None
q = re.findall(r'loop site 0x800194F8: \d+ polls, \d+ runs, (\d+) quanta', text)
quanta = int(q[-1]) if q else 0
rows = [l.split() for l in open(cw) if l[:1] in 'BC']
a, b = open(pre, 'rb').read(), open(card, 'rb').read()
first = None
for i in range(1, 16):
    if b[i * 128 + 10:i * 128 + 30].split(b'\0')[0] == b'BASCUS-94425SP2RR':
        first = i - 1
        break
# The autosave's write: B(35h) of 0x600 bytes, the offset from the lseek
# on the same fd before it.
write, off, seek = None, None, {}
for k, r in enumerate(rows):
    if r[0] == 'B' and r[3] == '33':
        seek[r[4]] = int(r[7], 16)
    elif r[0] == 'B' and r[3] == '35' and r[6] == '00000600':
        write, off = (k, r), seek.get(r[4])
written = changed = 0
if write and off is not None and first is not None:
    lo = 64 * (first + 1) + off // 128
    span = set(range(lo, lo + 12))
    wsec = {int(r[5]) for r in rows[write[0]:] if r[0] == 'C' and r[3] == '0' and r[4] == '4'}
    written = len(span & wsec)
    changed = sum(a[s * 128:(s + 1) * 128] != b[s * 128:(s + 1) * 128] for s in span)
wr = '-' if not write else '0x600@0x%x:0x%s' % (off if off is not None else -1, write[1][7])
head = ('[card-boot] row=autosave game=spyro-2 exit=%d trip=%s selected=%s released=%d quanta=%d '
        'write=%s written=%d/12 changed=%d'
        % (rc, trip[-1] if trip else '-', sel[-1] if sel else '-', released, quanta, wr, written, changed))
if not called or sel[-1:] != ['1'] or not released:
    print(head + ' verdict=VACUOUS: the exit call with a save selected or the story cutscene did not run'); sys.exit(1)
ok = (rc == 0 and quanta > 0 and write is not None and write[1][7] == '00000000'
      and off is not None and off >= 0x200 and (off - 0x200) % 0x600 == 0
      and written == 12 and changed > 0)
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
        format-ref) row_format_ref format-ref games/spyro-2/oracle-card-format.conf 0; r=$? ;;
        format-ref-clock) row_format_ref format-ref-clock games/spyro-2/oracle-card-format-clock.conf 1; r=$? ;;
        full-save-ref-clock) row_full_save_ref full-save-ref-clock games/spyro-2/oracle-card-full-save-clock.conf; r=$? ;;
        load-ref) row_load_ref load-ref "$SAVE_REF_GAME" "games/$SAVE_REF_GAME/oracle-card-load.conf" 0; r=$? ;;
        load-ref-clock) row_load_ref load-ref-clock spyro-2 games/spyro-2/oracle-card-load-clock.conf 1; r=$? ;;
        autosave) row_autosave; r=$? ;;
        *) echo "card-boot: unknown row $row" >&2; exit 64 ;;
    esac
    (( r == 1 )) && failed=1
    (( r != 64 )) && ran=1
done
(( failed )) && exit 1
(( ran )) || exit 64
exit 0
