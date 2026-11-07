#!/usr/bin/env bash
# check_s3_areawarp.sh - the Spyro 3 Area warp pair harness.
#
# Runs the rows of games/spyro-3/corpus/areawarp.manifest headless. Each row
# boots into its start area (--skip-to-level for area 0, --skip-to-area for
# a sub-area), then fires the Area warp to the row's target sub-area through
# OPENPETE_AREAWARP_TEST at tick TRIGGER (default 1700). Every run replays a
# generated movie that presses Cross for 2 VBLs every 40 VBLs, which
# dismisses the speeches some areas open on arrival; without it those runs
# wait in game.state 1 and the warp has no route.
#
# Per row:
#   engaged  the log has an "areawarp: called" line (the warp's first
#            request reached the game). A row whose request the warp
#            ignored ("no route from this state") is NOTRUN, not judged.
#   arrived  the log has "areawarp: arrived in area K" with K = level * 16
#            + target, the warp's final entry number.
#   lit      native screenshots every 100 VBLs from SHOT_FIRST to the end;
#            the warp's start is the first load state (5 or 13) after the
#            boot's release line;
#            each is scored by its lit fraction, the share of pixels with
#            luma above 24. A shot taken while game.state (0x8006E344, read
#            per VBL by OPENPETE_STATE_WATCH) is 0 (gameplay) or 1 (speech)
#            must have a lit fraction of at least LIT_MIN (0.05); shots in a
#            load or fade state (3, 5, 13) are not judged. At least one
#            judged shot after the arrival VBL is required, or the row is
#            VACUOUS.
# An engaged row passes when it arrived, no judged shot after the warp's
# first call is black, and a judged shot after the arrival exists.
#
#   tests/harness/check_s3_areawarp.sh [--all] [--build-dir=DIR] [--filter=NAME]
#
# Without --all the default set runs (35 rows, about 18 minutes at 2 jobs on
# the reference machine); --all runs the 88 rows (about 45 minutes). Exit 0
# PASS, 1 FAIL (any row failing, a VACUOUS row, or zero engaged rows), 64
# SKIP (binary or disc missing). At most AREAWARP_JOBS (default 2) game
# processes run at once.
set -uo pipefail
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh
export OPENPETE_VIBRATION=0

BUILD="build/linux-release"; FILTER=""; ALL=0
for a in "$@"; do
    case "$a" in
        --build-dir=*) BUILD="${a#--build-dir=}";;
        --filter=*)    FILTER="${a#--filter=}";;
        --all)         ALL=1;;
    esac
done
GAME="spyro-3"
PG_BUILD_DIR="$BUILD" pg_resolve "$GAME" || exit 64
MANIFEST="$(pg_corpus_manifest "$GAME" areawarp.manifest)"
pg_manifest "$MANIFEST" pipe
OUT="${AREAWARP_OUT:-$BUILD/areawarp/$GAME}"
JOBS="${AREAWARP_JOBS:-2}"
TRIGGER="${AREAWARP_TRIGGER:-1700}"
LIMIT=3310; SHOT_FIRST=2300; LIT_MIN=0.05

if [[ ! -x "$PG_BIN" || ! -f "$PG_DISC" ]]; then
    echo "check_s3_areawarp: SKIP ($PG_BIN or $PG_DISC missing)"; exit 64
fi

mkdir -p "$OUT"
rm -rf "${OUT:?}/rows" "${OUT:?}/presses.bk2"
mkdir -p "$OUT/rows"

# The press movie: Cross held for VBLs 0-1 of every 40, LIMIT rows. Its
# header names Spyro 3's default level-load delay (66 VBLs), which replay
# requires to equal the run's; the periodic presses do not depend on it.
python3 - "$OUT/presses.bk2" "$LIMIT" <<'EOF'
import sys, zipfile
path, n = sys.argv[1], int(sys.argv[2])
key = ("LogKey:#Disc Select|Open|Close|Reset|#P1 LStick X|P1 LStick Y|P1 RStick X|"
       "P1 RStick Y|P1 Up|P1 Down|P1 Left|P1 Right|P1 Select|P1 Start|P1 Square|"
       "P1 Triangle|P1 Circle|P1 Cross|P1 L1|P1 R1|P1 L2|P1 R2|P1 L3|P1 R3|P1 MODE|")
idle, cross = "." * 17, "." * 9 + "X" + "." * 7
rows = [f"|    1,...|  128,  128,  128,  128,{cross if i % 40 < 2 else idle}|" for i in range(n)]
header = ("MovieVersion BizHawk v2.0\nAuthor openpete\nemuVersion openpete-bk2-record\n"
          "Platform PSX\nCore Octoshock\nrerecordCount 0\nLoadDelay 66\n"
          "OpenpeteFeedKey vbl\n")
with zipfile.ZipFile(path, "w") as z:
    z.writestr("Header.txt", header)
    z.writestr("Input Log.txt", "[Input]\n" + key + "\n" + "\n".join(rows) + "\n")
EOF
[[ -f "$OUT/presses.bk2" ]] || { echo "check_s3_areawarp: FAIL (press movie not written)"; exit 1; }

NAMES=(); LEVELS=(); FROMS=(); TOS=()
while IFS='|' read -r name level from to set; do
    name="${name//[[:space:]]/}"
    [[ -z "$name" || "$name" == \#* ]] && continue
    set="${set//[[:space:]]/}"
    [[ -n "$FILTER" && "$name" != "$FILTER" ]] && continue
    [[ -z "$FILTER" && $ALL == 0 && "$set" != default ]] && continue
    NAMES+=("$name"); LEVELS+=("${level//[[:space:]]/}")
    FROMS+=("${from//[[:space:]]/}"); TOS+=("${to//[[:space:]]/}")
done < "$MANIFEST"
if (( ${#NAMES[@]} == 0 )); then
    echo "check_s3_areawarp: FAIL (no rows in $MANIFEST${FILTER:+ matching $FILTER})"; exit 1
fi

# run NAME LEVEL FROM TO: one session; log, exit code and shots under rows/NAME.
run() {
    local name="$1" level="$2" from="$3" to="$4" d="$OUT/rows/$1" shots="" v
    local -a boot
    mkdir -p "$d"
    if [[ "$from" == 0 ]]; then boot=(--skip-to-level "$level"); else boot=(--skip-to-area "$level:$from"); fi
    for ((v = SHOT_FIRST; v < LIMIT; v += 100)); do shots+="${shots:+,}$v"; done
    env OPENPETE_AREAWARP_TEST="$TRIGGER:$level:$to" OPENPETE_STATE_WATCH="gs=8006E344" \
        timeout -k 10 900 "$PG_BIN" --headless --no-config --disc "$PG_DISC" \
        "${PG_RUN_ARGS[@]}" --no-card --mute 1 --input-lock --vibration 0 \
        --renderer vk --bk2 "$OUT/presses.bk2" "${boot[@]}" \
        --screenshot-native-only --screenshot-on-frame "$shots" \
        --screenshot-output "$d/shot-{n}.png" \
        --frame-limit "$LIMIT" >"$d/log.txt" 2>&1
    echo $? >"$d/rc"
}

running=0
for i in "${!NAMES[@]}"; do
    run "${NAMES[$i]}" "${LEVELS[$i]}" "${FROMS[$i]}" "${TOS[$i]}" &
    running=$((running + 1))
    if (( running >= JOBS )); then wait -n; running=$((running - 1)); fi
done
wait

# classify: one line per row, "<VERDICT> <name>: <detail>".
CLASSIFY="$(cat <<'EOF'
import glob, os, re, sys
from PIL import Image
out, lit_min, rows = sys.argv[1], float(sys.argv[2]), sys.argv[3:]
LEVELS = {"sunny_villa": 11, "cloud_spires": 12, "molten_crater": 13, "seashell_shore": 14,
          "icy_peak": 21, "enchanted_towers": 22, "spooky_swamp": 23, "bamboo_terrace": 24,
          "frozen_altars": 31, "lost_fleet": 32, "fireworks_factory": 33, "charmed_ridge": 34,
          "crystal_islands": 41, "desert_ruins": 42, "haunted_tomb": 43, "dino_mines": 44,
          "super_bonus_round": 50}
def lit(path):
    im = Image.open(path).convert("L")
    h = im.histogram()
    return sum(h[25:]) / float(sum(h))
for row in rows:
    name, level, to = row.split(":")
    d = os.path.join(out, "rows", name)
    try:
        log = open(os.path.join(d, "log.txt"), errors="replace").read()
        rc = open(os.path.join(d, "rc")).read().strip()
    except OSError:
        print(f"FAIL {name}: no output"); continue
    rel = [int(v) for v in re.findall(r"(?:presents resume at vbl=|released: [^\n]*key=)(\d+)", log)]
    boot = max(rel) if rel else 0
    if "areawarp: called" not in log:
        why = "ignored, no route" if "no route from this state" in log else "no call"
        print(f"NOTRUN {name}: {why}"); continue
    key = LEVELS[level] * 16 + int(to)
    m = re.search(rf"areawarp: arrived in area {key} at vbl=(\d+)", log)
    states = [(int(v), int(g)) for v, g in re.findall(r"\[state\] vbl=(\d+) frame=\d+ gs=(-?\d+)", log)]
    leaves = [v for v, g in states if v > boot and g in (5, 13)]
    first_call = leaves[0] if leaves else 0
    def state_at(v):
        g = None
        for sv, sg in states:
            if sv <= v: g = sg
            else: break
        return g
    black, after = [], 0
    for p in sorted(glob.glob(os.path.join(d, "shot-*.png"))):
        v = int(re.search(r"shot-(\d+)", p).group(1))
        if v < first_call or state_at(v) not in (0, 1): continue
        f = lit(p)
        if f < lit_min: black.append(f"{v}:{f:.3f}")
        if m and v >= int(m.group(1)): after += 1
    if rc != "0":
        print(f"FAIL {name}: exit {rc}"); continue
    if not m:
        last = states[-1] if states else None
        print(f"FAIL {name}: no arrival in area {key} (last state vbl,gs {last}); black {black or 'none'}"); continue
    if black:
        print(f"FAIL {name}: black judged shots {black}"); continue
    if after == 0:
        print(f"VACUOUS {name}: no judged shot after the arrival at vbl {m.group(1)}"); continue
    print(f"PASS {name}: arrived vbl {m.group(1)}, {after} lit judged shot(s) after")
EOF
)"
ROWSPEC=()
for i in "${!NAMES[@]}"; do ROWSPEC+=("${NAMES[$i]}:${LEVELS[$i]}:${TOS[$i]}"); done
RESULT="$(python3 -c "$CLASSIFY" "$OUT" "$LIT_MIN" "${ROWSPEC[@]}")"
echo "$RESULT"
engaged="$(grep -vc '^NOTRUN' <<<"$RESULT")"
failed="$(grep -Ec '^(FAIL|VACUOUS)' <<<"$RESULT")"
notrun="$(grep -c '^NOTRUN' <<<"$RESULT")"
echo "check_s3_areawarp: rows ${#NAMES[@]}, engaged $engaged, not run $notrun, failed $failed"
if (( engaged == 0 )); then
    echo "check_s3_areawarp: FAIL (zero engaged rows: no warp reached the game)"; exit 1
fi
(( failed == 0 )) || { echo "check_s3_areawarp: FAIL"; exit 1; }
echo "check_s3_areawarp: PASS"
