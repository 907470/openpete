#!/usr/bin/env bash
# check_bugreport_custom_mode.sh - a custom level's slot/replace toggle, the
# shared slot's claim and a first-run memory card replay from a bug-report
# bundle (Spyro 1).
#
# Fixture: scripts/lib/custom_level_fixture.sh, a declarative mod "Warp
# Fixture" in replace mode over donor 12 (Dark Hollow's own data entry).
# OPENPETE_CUSTOM_MODE=<tick>:slot:Warp Fixture queues the toggle the Mods
# menu's mode checkbox queues, OPENPETE_CUSTOM_WARP=<tick>:Warp Fixture the
# request its Warp button queues, each at that registry tick. Every run is
# headless under --skip-to-level artisans.
#
# Leg mode (the toggle, and the card): the toggle at registry tick MODE_AT
# and the warp at WARP_AT fall after the bundle's anchor (the session's
# second, VBL 1796); the press is at PRESS. The recording runs without
# --no-card, on the sandbox's fresh card directory, so each port holds a
# card that has no file by the anchor. Replays: mode (as written) and
# mode-old (warps.txt without its custom_mode lines, negative control).
#
# Leg claim (the slot claim at the anchor): the toggle at CLAIM_MODE_AT and
# the warp at CLAIM_WARP_AT fall before the anchor, so the anchor is taken
# in the slot level with the claim made; a second warp into the slot at
# CLAIM_REWARP_AT falls after it, where the registry's mode and claim and
# the level ops' alias donor decide the warp's writes. --no-card. Replays:
# claim (as written) and claim-bare (anchor.state without its registry
# section, the state a build without the section writes: negative
# control; the warp then takes the manifest's replace mode).
#
# Carve-out: the press-state compare's spu section (PsyCross voice state)
# differs by 2 or 3 bytes near offset 0x1E64 on some Spyro 1 bundles while
# every hash row matches, also on a recording with no mod, no card and no
# toggle; a replay with that difference alone reads SPU-ONLY and counts as
# reproduced. Any other section, or any row, still fails.
#
# Instrument: warps.txt; the recording's `custom level "Warp Fixture"
# entering (id 65` line; each replay's verdict line (`bug-report-replay:
# IDENTICAL | DIVERGED tick=T ...`), its `warp custom_mode to 0 queued at
# frame F` line, its `warps:` summary line, and its `card N: the anchor
# holds a card with no image` line.
#
# Pass criterion: mode's warps.txt holds one custom_mode line to 0 (row 0,
# slot) and one custom_warp line to 0 after the anchor, the toggle first,
# and the recording enters the slot; mode IDENTICAL (or SPU-ONLY) with the toggle taken at
# its recorded frame (2 queued, 0 past their frame, 0 not reached) and a
# fresh card provisioned for port 1; mode-old DIVERGED at or after the
# toggle's frame. claim's warps.txt holds custom_mode:0 and custom_warp:0 at
# or before the anchor and one custom_warp:0 after it; claim IDENTICAL (or SPU-ONLY),
# entering the slot (id 65) at the second warp (1 queued, 0 past their
# frame, 0 not reached); claim-bare DIVERGED at or after that warp's frame.
#
# Verdict line:
#   [bugreport-custom-mode] mode-frame=<F> mode=<verdict> mode-old=<verdict>@<tick>
#     claim=<verdict> claim-bare=<verdict>@<tick> verdict=PASS|FAIL
#
# Usage: tests/harness/check_bugreport_custom_mode.sh [--build-dir=DIR]
# Exit 0 PASS, 1 FAIL, 64 SKIP (binary or disc missing).
# xvfb-guard: no-launch (every run is headless)
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh
# shellcheck source=scripts/lib/custom_level_fixture.sh
source scripts/lib/custom_level_fixture.sh

for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
    esac
done

GAME=spyro-1
pg_resolve "$GAME"
[[ -x "$PG_BIN" && -f "$PG_DISC" ]] ||
    { echo "bugreport-custom-mode: SKIP (binary or disc missing)"; exit 64; }

OUT="${PG_BUILD_DIR:-build/linux-release}/bugreport-custom-mode"
rm -rf "${OUT:?}"
mkdir -p "$OUT"
ABS_OUT="$(cd "$OUT" && pwd)"
PG_CAPTURE_TIMEOUT=600
MODE_AT=1350
WARP_AT=1400
PRESS=5200
CLAIM_MODE_AT=100
CLAIM_WARP_AT=150
CLAIM_REWARP_AT=1400
MODS="$ABS_OUT/mods"

fail=()
custom_level_fixture "$MODS" "$(dirname "$PG_DISC")/WAD.WAD" ||
    { echo "bugreport-custom-mode: cannot read level 12's data entry"; echo "[bugreport-custom-mode] verdict=FAIL"; exit 1; }

# rewrite_bundle SRC DST MODE: a copy of the bundle with one member changed
# and its manifest entry updated. MODE nomode drops warps.txt's custom_mode
# lines; MODE bare drops anchor.state's registry section (header field at
# byte 120 zeroed, the section's trailing bytes cut).
rewrite_bundle() {
    python3 -I - "$1" "$2" "$3" <<'PY'
import hashlib, json, struct, sys, zipfile
src = zipfile.ZipFile(sys.argv[1])
mode = sys.argv[3]
name = "warps.txt" if mode == "nomode" else "anchor.state"
d = src.read(name)
if mode == "nomode":
    d = "".join(l for l in d.decode().splitlines(True) if not l.startswith("warp\t") or l.split("\t")[4] != "custom_mode").encode()
else:
    ver, = struct.unpack_from("<I", d, 4)
    reg, = struct.unpack_from("<I", d, 120)
    if ver < 7 or reg == 0:
        sys.exit("anchor.state holds no registry section")
    d = d[:120] + struct.pack("<I", 0) + d[124:len(d) - reg]
m = json.loads(src.read("manifest.json"))
for x in m["members"]:
    if x["name"] == name:
        x["size"] = len(d)
        x["sha256"] = hashlib.sha256(d).hexdigest()
with zipfile.ZipFile(sys.argv[2], "w", zipfile.ZIP_DEFLATED) as out:
    for n in src.namelist():
        out.writestr(n, json.dumps(m) if n == "manifest.json" else d if n == name else src.read(n))
PY
}

# warp_rows BUNDLE: "<anchor> <frame>:<id>:<target> ..." over warps.txt's lines.
warp_rows() {
    python3 -I - "$1" <<'PY'
import json, sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
anchor = json.loads(z.read("manifest.json"))["anchor"]["frame"]
rows = [l.split("\t") for l in z.read("warps.txt").decode().splitlines() if l.startswith("warp\t")]
print(anchor, *("%s:%s:%s" % (r[1], r[4], r[5]) for r in rows))
PY
}

# record TAG MODE_TICK WARP_TICK ARGS...: one recording with a press at PRESS; sets ZIP.
record() {
    local tag="$1" mt="$2" wt="$3"
    shift 3
    PG_CAPTURE_ENV=("OPENPETE_SANDBOX=$ABS_OUT/xdg-$tag" "OPENPETE_BUGREPORT_AT=$PRESS" "OPENPETE_BUGREPORT_NOTE=0"
                    "OPENPETE_CUSTOM_MODE=$mt:slot:Warp Fixture" "OPENPETE_CUSTOM_WARP=$wt:Warp Fixture")
    pg_capture "$OUT/rec-$tag.log" "" -- --game "$GAME" --mute 1 --mods-dir "$MODS" \
        --frame-limit $((PRESS + 20)) --skip-to-level artisans "$@" </dev/null ||
        fail+=("$tag: recording failed (see $OUT/rec-$tag.log)")
    ZIP="$(find "$OUT/xdg-$tag" -name 'bug-*.zip' | head -n 1)"
    [[ -n "$ZIP" ]] || fail+=("$tag: the recording wrote no bundle (see $OUT/rec-$tag.log)")
    grep -q 'custom level "Warp Fixture" entering (id 65' "$OUT/rec-$tag.log" ||
        fail+=("$tag: the recording did not enter the shared slot")
}

# replay TAG BUNDLE: one replay; sets LOG, V (verdict word) and T (divergent tick).
# A DIVERGED replay whose every hash row matched and whose press-state
# compare differs in the spu section alone reads SPU-ONLY (see the carve-out
# above).
replay() {
    LOG="$OUT/replay-$1.log"
    PG_CAPTURE_ENV=("OPENPETE_BUGREPORT_REPLAY_DIR=$OUT/scratch-$1")
    pg_capture "$LOG" "" -- --game "$GAME" --mute 1 --bug-report-replay "$2" </dev/null
    V="$(grep -oE '^bug-report-replay: (IDENTICAL|DIVERGED|REFUSED|VACUOUS|REPRODUCED)' "$LOG" | tail -n 1)"
    V="${V#bug-report-replay: }"
    V="${V:-none}"
    T="$(grep -oE '^bug-report-replay: DIVERGED tick=[0-9]+' "$LOG" | tail -n 1 | grep -oE '[0-9]+$')"
    if [[ "$V" == DIVERGED ]] && grep -qE '^bug-report-replay: DIVERGED tick=0 region=none ' "$LOG" &&
       grep -qE '^bug-report-replay: sections differing: spu\+0x[0-9A-F]+ \([0-9]+ byte\(s\)\)$' "$LOG"; then
        V=SPU-ONLY
    fi
}

# same V: 0 when a replay reproduced the recording (IDENTICAL, or SPU-ONLY).
same() { [[ "$1" == IDENTICAL || "$1" == SPU-ONLY ]]; }

# ---- leg mode: the toggle after the anchor, on a first-run card ----
m_frame=none v_mode=none v_mold=none t_mold=none
record mode "$MODE_AT" "$WARP_AT"
if [[ -n "$ZIP" ]]; then
    read -r anchor r1 r2 rest <<<"$(warp_rows "$ZIP")"
    if [[ -z "${rest:-}" && "${r1:-}" == *:custom_mode:0 && "${r2:-}" == *:custom_warp:0 &&
          ${r1%%:*} -gt $anchor && ${r2%%:*} -ge ${r1%%:*} ]]; then
        m_frame="${r1%%:*}"
    else
        fail+=("mode: warps.txt holds '${r1:-} ${r2:-} ${rest:-}' after anchor ${anchor:-?}, want one custom_mode:0 then one custom_warp:0 after it")
    fi
    replay mode "$ZIP"
    v_mode="$V"
    same "$V" || fail+=("mode: $V, want IDENTICAL (see $LOG)")
    grep -q "^bug-report-replay: warp custom_mode to 0 queued at frame $m_frame (drained at frame $m_frame " "$LOG" ||
        fail+=("mode: the toggle was not taken at its recorded frame $m_frame")
    grep -q '^bug-report-replay: warps: 2 queued (0 past their frame), 0 not reached' "$LOG" ||
        fail+=("mode: no 'warps: 2 queued (0 past their frame), 0 not reached' line")
    grep -q '^bug-report-replay: card 1: the anchor holds a card with no image' "$LOG" ||
        fail+=("mode: VACUOUS for the card: port 1 held no card without an image (see $LOG)")

    rewrite_bundle "$ZIP" "$OUT/mode-old.zip" nomode || fail+=("mode-old: cannot rewrite the bundle")
    replay mode-old "$OUT/mode-old.zip"
    v_mold="$V" t_mold="${T:-none}"
    [[ "$V" == DIVERGED ]] || fail+=("mode-old: $V, want DIVERGED (see $LOG)")
    if [[ ! "$m_frame" =~ ^[0-9]+$ ]] || (( ${T:-0} < m_frame )); then
        fail+=("mode-old: diverged at tick ${T:-none}, before the toggle's frame $m_frame")
    fi
fi

# ---- leg claim: the slot claimed before the anchor ----
v_claim=none v_bare=none t_bare=none
record claim "$CLAIM_MODE_AT" "$CLAIM_WARP_AT,$CLAIM_REWARP_AT" --no-card
if [[ -n "$ZIP" ]]; then
    c_frame=none
    read -r anchor r1 r2 r3 rest <<<"$(warp_rows "$ZIP")"
    if [[ -z "${rest:-}" && "${r1:-}" == *:custom_mode:0 && "${r2:-}" == *:custom_warp:0 &&
          "${r3:-}" == *:custom_warp:0 && ${r2%%:*} -le $anchor && ${r3%%:*} -gt $anchor ]]; then
        c_frame="${r3%%:*}"
    else
        fail+=("claim: warps.txt holds '${r1:-} ${r2:-} ${r3:-} ${rest:-}' around anchor ${anchor:-?}, want custom_mode:0 and custom_warp:0 at or before it, one custom_warp:0 after it")
    fi
    replay claim "$ZIP"
    v_claim="$V"
    same "$V" || fail+=("claim: $V, want IDENTICAL (see $LOG)")
    grep -q 'custom level "Warp Fixture" entering (id 65' "$LOG" ||
        fail+=("claim: the replay's warp did not enter the shared slot")
    grep -q '^bug-report-replay: warps: 1 queued (0 past their frame), 0 not reached' "$LOG" ||
        fail+=("claim: no 'warps: 1 queued (0 past their frame), 0 not reached' line")

    rewrite_bundle "$ZIP" "$OUT/claim-bare.zip" bare || fail+=("claim-bare: cannot rewrite the bundle")
    replay claim-bare "$OUT/claim-bare.zip"
    v_bare="$V" t_bare="${T:-none}"
    [[ "$V" == DIVERGED ]] || fail+=("claim-bare: $V, want DIVERGED (see $LOG)")
    if [[ ! "$c_frame" =~ ^[0-9]+$ ]] || (( ${T:-0} < c_frame )); then
        fail+=("claim-bare: diverged at tick ${T:-none}, before the second warp's frame $c_frame")
    fi
fi

res=PASS
if (( ${#fail[@]} )); then
    printf 'bugreport-custom-mode: %s\n' "${fail[@]}"
    res=FAIL
fi
echo "[bugreport-custom-mode] mode-frame=$m_frame mode=$v_mode mode-old=$v_mold@$t_mold" \
     "claim=$v_claim claim-bare=$v_bare@$t_bare verdict=$res"
[[ "$res" == PASS ]]
