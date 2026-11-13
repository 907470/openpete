#!/usr/bin/env bash
# check_bugreport_replay.sh - record bug-report bundles headless on Spyro 2
# and replay them with --bug-report-replay on the same build.
#
# Runs: two recordings under OPENPETE_BUGREPORT_AT (the press at a feed key),
# each bundle written under a scratch XDG_DATA_HOME:
#   skip    --skip-to-level summer_forest, press at VBL 1500 (the skip
#           induces near VBL 1260 and the level is playable near VBL 1360,
#           both after the session's first anchor)
#   plain   no launch flag, press at VBL 900 (the title)
#   audio   as plain, on the audio feed's synthetic device
#           (OPENPETE_AUDIO_FEED_SIM=0.0,1, the virtual clock) with the host
#           slowed to 0.96x from paced VBL 200 for 600 VBLs
#           (OPENPETE_PACE_INJECT_US=16536,1,200,600), so the feed primes,
#           the host slips and the time stretch engages before the press
#   knobs   as skip, under --renderer vk --aspect 16:9 and --no-config (no
#           change is saved), press at VBL 4400, with knob changes from the
#           harness schedule OPENPETE_BUGREPORT_KNOB_AT: terrain_visibility
#           = game and hide_player = on at feed key 1400 (before the
#           bundle's anchor, the session's second) and dither = on at 3000
#           (after it); a native screenshot at feed key 4390
#   display as plain, under the display and output-modifier flags a
#           tester's argv can hold: --renderer vk --fullscreen 1 --window
#           2560x1440 --vsync 0 --gpu NoSuchGpu --ssaa 2 --background 1
#           --input-lock 1 (kept by the replay) and --screenshot-native-only
#           --screenshot-overlays 1 --record-fps-overlay 1 --no-record-audio
#           (dropped with the outputs they modify); a native screenshot at
#           feed key 890
# and thirteen replays:
#   skip         the skip bundle as written
#   skip-late    the skip bundle with its manifest's anchor seq set to 2:
#                the replay drops the launch flags (negative control)
#   plain        the plain bundle
#   mods         the plain bundle with three save-record rows holding no
#                identity added to press.state's game-extra trailer, the
#                shape a recording with installed, disabled mods writes
#                (a headless recording loads no disabled mod); the replay's
#                empty mod set writes none
#   mods-held    as mods, one added row holding an identity (negative
#                control)
#   env          the skip bundle with OPENPETE_TERRAIN_TSC_AUDIT=1 named in
#                OPENPETE_BUGREPORT_REPLAY_ENV, rendering one native
#                screenshot so the terrain audit runs
#   env-refused  OPENPETE_BUGREPORT_REPLAY_ENV naming OPENPETE_PLAYER_POSE
#   knobs        the knobs bundle, a native screenshot at feed key 4390
#   knobs-off    as knobs under OPENPETE_BUGREPORT_REPLAY_KNOBS=0 (negative
#                control: the live values are not applied)
#   knobs-old    the knobs bundle without knobs.txt and its manifest entry,
#                the shape a bundle written before the member has, a native
#                screenshot at feed key 4390
#   vk-noshot    the knobs bundle (argv: --renderer vk --screenshot-native-only)
#                with no screenshot request
#   display      the display bundle with no screenshot request
#   native-only  the plain bundle with the developer's own
#                --screenshot-native-only and no screenshot request (the
#                replay passes the developer's flags unfiltered)
#
# Every recording runs under --mute 1.
#
# Instrument: the replay's verdict line (`bug-report-replay: IDENTICAL |
# DIVERGED | REFUSED ...`), its `launch flags kept|dropped` line, the level
# skip's `[skip-to-level] induced` line, and the audit's `[tsc-audit] total`
# line; the replay's `argv:` line; `tools/bugreport.py audio` over each recording's audio-feed.txt
# (exit 3 when the feed played data and holds no event); `tools/bugreport.py
# info` over the knobs bundle; the replay's `knob` and `knobs:` lines; the
# decoded pixels of the screenshots at feed key 4390.
#
# Pass criterion: skip IDENTICAL with the launch flags kept and the skip
# induced in the replay; mods IDENTICAL; mods-held DIVERGED on
# game-extra:modsave; skip-late DIVERGED with the launch flags dropped and
# no induction; plain IDENTICAL with no launch line; env IDENTICAL with an
# `env kept OPENPETE_TERRAIN_TSC_AUDIT` line and a `[tsc-audit] total` line
# whose hits are nonzero; env-refused exit 64 naming OPENPETE_PLAYER_POSE;
# every recording's audio-feed.txt parses (exit 0, not VACUOUS) and states
# mute=1; the audio recording's names device=synthetic, has played data,
# and holds a nonzero event count with a prime, a slip and a stretch;
# `info` of the knobs bundle names each of the three knobs as changed live;
# knobs IDENTICAL with terrain_visibility and hide_player applied at the
# anchor load and dither at its tick, and its screenshot equal to the
# recording's (zero knobs applied reads VACUOUS); knobs-off IDENTICAL with
# zero applied and a screenshot that differs from the recording's (equal
# means the case cannot fail); knobs-old IDENTICAL with the `no knobs.txt`
# line and a screenshot equal to knobs-off's, and `info` naming the member
# absent; vk-noshot, display and native-only exit 0 IDENTICAL with
# rows > 0 (the replay ran and compared that many tick hashes; zero rows
# reads VACUOUS), and the argv line of vk-noshot and display keeps every
# display flag and holds none of the dropped modifiers.
#
# Verdict line:
#   [bugreport-replay] skip=.. skip-late=.. plain=.. mods=.. mods-held=.. env=.. env-refused=.. audio=<events>
#                      knobs=<applied>|VACUOUS knobs-off=<verdict>/<capture> knobs-old=..
#                      vk-noshot=<verdict>/<rows> display=<verdict>/<rows> native-only=<verdict>/<rows> verdict=PASS|FAIL
#
# Usage: tests/harness/check_bugreport_replay.sh [--build-dir=DIR]
# Exit 0 PASS, 1 FAIL, 64 SKIP (binary or disc missing).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
    esac
done

GAME=spyro-2
pg_resolve "$GAME"
[[ -x "$PG_BIN" && -f "$PG_DISC" ]] ||
    { echo "bugreport-replay: SKIP (binary or disc missing)"; exit 64; }

OUT="${PG_BUILD_DIR:-build/linux-release}/bugreport-replay"
rm -rf "${OUT:?}"
mkdir -p "$OUT"
PG_CAPTURE_TIMEOUT=600

# record TAG AT [args...]: one headless run pressing at feed key AT; prints the bundle path.
# REC_ENV holds extra environment for the recording.
REC_ENV=()
record() {
    local tag="$1" at="$2"; shift 2
    PG_CAPTURE_ENV=("XDG_DATA_HOME=$OUT/xdg-$tag" "OPENPETE_BUGREPORT_AT=$at" "OPENPETE_BUGREPORT_NOTE=0"
                    "${REC_ENV[@]}")
    pg_capture "$OUT/rec-$tag.log" "" -- --game "$GAME" --mute 1 --frame-limit $((at + 20)) "$@" </dev/null ||
        return 1
    local z
    z="$(find "$OUT/xdg-$tag" -name 'bug-*.zip' | head -n 1)"
    [[ -n "$z" ]] && echo "$z"
}

# replay TAG BUNDLE [args...]: one replay; sets RC and LOG.
replay() {
    local tag="$1" bundle="$2"; shift 2
    LOG="$OUT/replay-$tag.log"
    PG_CAPTURE_ENV=("OPENPETE_BUGREPORT_REPLAY_DIR=$OUT/scratch-$tag" "${REPLAY_ENV[@]}")
    pg_capture "$LOG" "" -- --game "$GAME" --mute 1 --bug-report-replay "$bundle" "$@" </dev/null
    RC=$?
}

# verdict LOG: the verdict word of a replay log, or "none".
verdict() {
    local v
    v="$(grep -oE 'bug-report-replay: (IDENTICAL|DIVERGED|REFUSED|VACUOUS)' "$1" | tail -n 1)"
    v="${v#bug-report-replay: }"
    echo "${v:-none}"
}

fail=()
skip_zip="$(record skip 1500 --skip-to-level summer_forest)" || fail+=("skip recording failed (see $OUT/rec-skip.log)")
plain_zip="$(record plain 900)" || fail+=("plain recording failed (see $OUT/rec-plain.log)")
REC_ENV=("OPENPETE_AUDIO_FEED_SIM=0.0,1" "OPENPETE_PACE_INJECT_US=16536,1,200,600")
audio_zip="$(record audio 900)" || fail+=("audio recording failed (see $OUT/rec-audio.log)")
REC_ENV=("OPENPETE_BUGREPORT_KNOB_AT=1400:terrain_visibility=game,1400:hide_player=on,3000:dither=on")
knobs_zip="$(record knobs 4400 --skip-to-level summer_forest --renderer vk --aspect 16:9 \
    --screenshot-on-frame 4390 --screenshot-native-only --screenshot-output "$OUT/knobs-rec__{n}.png")" ||
    fail+=("knobs recording failed (see $OUT/rec-knobs.log)")
REC_ENV=()
DISPLAY_KEPT=(--renderer vk --fullscreen 1 --window 2560x1440 --vsync 0 --gpu NoSuchGpu --ssaa 2 --background 1
              --input-lock 1)
DISPLAY_DROPPED=(--screenshot-native-only --screenshot-overlays --record-fps-overlay --no-record-audio)
display_zip="$(record display 900 "${DISPLAY_KEPT[@]}" --screenshot-native-only --screenshot-overlays 1 \
    --record-fps-overlay 1 --no-record-audio --screenshot-on-frame 890 --screenshot-output "$OUT/display-rec__{n}.png")" ||
    fail+=("display recording failed (see $OUT/rec-display.log)")
if (( ${#fail[@]} )); then
    printf 'bugreport-replay: %s\n' "${fail[@]}"
    echo "[bugreport-replay] verdict=FAIL"
    exit 1
fi

# audio_check TAG BUNDLE [KIND...]: the bundle's audio-feed.txt reads (not
# VACUOUS) and states mute=1; with KINDs, it names the synthetic device,
# played data and holds each KIND. Sets HELD to the held event count.
audio_check() {
    local tag="$1" bundle="$2"; shift 2
    local out="$OUT/audio-$tag.txt" rc
    python3 -I tools/bugreport.py audio "$bundle" >"$out" 2>&1
    rc=$?
    HELD=0
    (( rc == 0 )) || { fail+=("audio $tag: bugreport.py audio exit $rc (see $out)"); return; }
    grep -q '^audio-feed.txt v1: mute=1 ' "$out" || fail+=("audio $tag: no mute=1 in the header")
    HELD="$(grep -oE 'events [0-9]+ held' "$out" | grep -oE '[0-9]+')"
    HELD="${HELD:-0}"
    if (( $# )); then
        grep -q ' device=synthetic ' "$out" || fail+=("audio $tag: device is not synthetic")
        grep -qE 'data_out=[1-9]' "$out" || fail+=("audio $tag: the feed played no data")
        (( HELD > 0 )) || fail+=("audio $tag: VACUOUS: no event held")
        local k
        for k in "$@"; do
            grep -qE "^ +-?[0-9.]+ +[0-9]+ +[0-9]+ $k " "$out" || fail+=("audio $tag: no $k event")
        done
    fi
}
audio_check skip "$skip_zip"
audio_check plain "$plain_zip"
audio_check audio "$audio_zip" prime slip stretch
n_audio="$HELD"

REPLAY_ENV=()
replay skip "$skip_zip"
v_skip="$(verdict "$LOG")"
[[ "$v_skip" == IDENTICAL ]] || fail+=("skip: $v_skip, want IDENTICAL")
grep -q 'launch flags kept: the anchor is the session.s first (seq 1): --skip-to-level summer_forest' "$LOG" ||
    fail+=("skip: no 'launch flags kept' line for seq 1")
grep -q '\[skip-to-level\] induced' "$LOG" || fail+=("skip: the replay induced no level skip")

# The negative control: the same bundle with its anchor marked a later one.
late="$OUT/skip-late"
mkdir -p "$late"
python3 -I - "$skip_zip" "$late" <<'EOF'
import json, sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
z.extractall(sys.argv[2])
p = sys.argv[2] + "/manifest.json"
m = json.load(open(p))
m["anchor"]["seq"] = 2
json.dump(m, open(p, "w"))
EOF
replay skip-late "$late"
v_late="$(verdict "$LOG")"
[[ "$v_late" == DIVERGED ]] || fail+=("skip-late: $v_late, want DIVERGED")
grep -q 'launch flags dropped' "$LOG" || fail+=("skip-late: no 'launch flags dropped' line")
grep -q '\[skip-to-level\] induced' "$LOG" && fail+=("skip-late: the replay induced a level skip")

replay plain "$plain_zip"
v_plain="$(verdict "$LOG")"
[[ "$v_plain" == IDENTICAL ]] || fail+=("plain: $v_plain, want IDENTICAL")
grep -q 'launch flags' "$LOG" && fail+=("plain: a launch line for a bundle without launch flags")

# rows BUNDLE DIR HELD: unpack BUNDLE into DIR and add three save-record rows
# to press.state's game-extra trailer, the first holding identity HELD.
rows() {
    python3 -I - "$@" <<'PY'
import hashlib, struct, sys, zipfile
zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])
p = sys.argv[2] + "/press.state"
d = bytearray(open(p, "rb").read())
old = hashlib.sha256(d).hexdigest()
ge, modset, arena = struct.unpack_from("<3I", d, 76)
start = len(d) - modset - arena - ge
end = start + ge
plen, pmag = struct.unpack_from("<2I", d, end - 8)
if pmag == 0x31504D4D:
    end -= 8 + plen
blen, bmag = struct.unpack_from("<2I", d, end - 8)
assert bmag == 0x314B534D, "no save-record trailer"
blk = end - 8 - blen
assert struct.unpack_from("<I", d, blk + blen - 4)[0] == 0, "the recording has mod rows"
add = b"".join(f"absent-mod-{i}".encode().ljust(64, b"\0")
               + struct.pack("<I", int(sys.argv[3]) if i == 0 else 0) + bytes(20)
               for i in range(3))
struct.pack_into("<I", d, blk + blen - 4, 3)
d[end - 8:end - 4] = struct.pack("<I", blen + len(add))
d[blk + blen:blk + blen] = add
struct.pack_into("<I", d, 76, ge + len(add))
open(p, "wb").write(d)
m = sys.argv[2] + "/manifest.json"
t = open(m).read()
assert old in t, "manifest names no press.state digest"
open(m, "w").write(t.replace(old, hashlib.sha256(d).hexdigest()))
PY
}

rows "$plain_zip" "$OUT/mods" 0 || fail+=("mods: fixture failed")
replay mods "$OUT/mods"
v_mods="$(verdict "$LOG")"
[[ "$v_mods" == IDENTICAL ]] || fail+=("mods: $v_mods, want IDENTICAL")

rows "$plain_zip" "$OUT/mods-held" 1 || fail+=("mods-held: fixture failed")
replay mods-held "$OUT/mods-held"
v_held="$(verdict "$LOG")"
[[ "$v_held" == DIVERGED ]] || fail+=("mods-held: $v_held, want DIVERGED")
grep -q 'sections differing: game-extra:modsave' "$LOG" || fail+=("mods-held: no game-extra:modsave difference")

REPLAY_ENV=("OPENPETE_TERRAIN_TSC_AUDIT=1" "OPENPETE_BUGREPORT_REPLAY_ENV=OPENPETE_TERRAIN_TSC_AUDIT")
replay env "$skip_zip" --renderer vk --screenshot-on-frame 900 --screenshot-native-only \
    --screenshot-output "$OUT/env__{n}.png"
v_env="$(verdict "$LOG")"
[[ "$v_env" == IDENTICAL ]] || fail+=("env: $v_env, want IDENTICAL")
grep -q 'env kept OPENPETE_TERRAIN_TSC_AUDIT=1' "$LOG" || fail+=("env: no 'env kept' line")
grep -qE '\[tsc-audit\] total hits=[1-9]' "$LOG" || fail+=("env: no '[tsc-audit] total' line with hits")

REPLAY_ENV=("OPENPETE_PLAYER_POSE=0,0,0" "OPENPETE_BUGREPORT_REPLAY_ENV=OPENPETE_PLAYER_POSE")
replay env-refused "$skip_zip"
v_ref="$(verdict "$LOG")"
(( RC == 64 )) || fail+=("env-refused: exit $RC, want 64")
grep -q 'REFUSED OPENPETE_BUGREPORT_REPLAY_ENV: OPENPETE_PLAYER_POSE' "$LOG" ||
    fail+=("env-refused: no refusal naming OPENPETE_PLAYER_POSE")

# png_cmp A B: "equal", or "differ <bytes>" over the decoded image data, or "missing".
png_cmp() {
    python3 -I - "$1" "$2" <<'PY'
import struct, sys, zlib
def pixels(p):
    try:
        d = open(p, "rb").read()
    except OSError:
        return None
    i, idat, head = 8, b"", b""
    while i + 8 <= len(d):
        n = struct.unpack(">I", d[i:i + 4])[0]
        kind, body = d[i + 4:i + 8], d[i + 8:i + 8 + n]
        if kind == b"IHDR":
            head = body
        elif kind == b"IDAT":
            idat += body
        i += 12 + n
    return head, zlib.decompress(idat)
a, b = pixels(sys.argv[1]), pixels(sys.argv[2])
if a is None or b is None:
    print("missing")
elif a == b:
    print("equal")
else:
    print("differ", sum(x != y for x, y in zip(a[1], b[1])) + abs(len(a[1]) - len(b[1])))
PY
}

python3 -I tools/bugreport.py info "$knobs_zip" >"$OUT/info-knobs.txt" 2>&1 || fail+=("knobs: info exit $?")
for k in "terrain_visibility = game" "hide_player = on" "dither = on"; do
    grep -q "^knob changed live: $k (changed live at " "$OUT/info-knobs.txt" ||
        fail+=("knobs: info names no live change '$k'")
done

REPLAY_ENV=()
replay knobs "$knobs_zip" --screenshot-on-frame 4390 --screenshot-native-only \
    --screenshot-output "$OUT/knobs-on__{n}.png"
v_knobs="$(verdict "$LOG")"
[[ "$v_knobs" == IDENTICAL ]] || fail+=("knobs: $v_knobs, want IDENTICAL")
for k in "terrain_visibility = game applied at the anchor load" "hide_player = on applied at the anchor load" \
         "dither = on applied at frame"; do
    grep -q "bug-report-replay: knob $k" "$LOG" || fail+=("knobs: no '$k' line")
done
n_knobs="$(grep -oE 'bug-report-replay: knobs: [0-9]+ changed after boot, [0-9]+ applied' "$LOG" |
           grep -oE '[0-9]+ applied' | grep -oE '[0-9]+')"
if (( ${n_knobs:-0} == 0 )); then
    fail+=("knobs: VACUOUS: the bundle holds live changes and the replay applied none")
    n_knobs=VACUOUS
fi
c_on="$(png_cmp "$OUT/knobs-rec__4390-vk.png" "$OUT/knobs-on__4390-vk.png")"
[[ "$c_on" == equal ]] || fail+=("knobs: the replay's screenshot is $c_on against the recording's")

REPLAY_ENV=("OPENPETE_BUGREPORT_REPLAY_KNOBS=0")
replay knobs-off "$knobs_zip" --screenshot-on-frame 4390 --screenshot-native-only \
    --screenshot-output "$OUT/knobs-off__{n}.png"
v_koff="$(verdict "$LOG")"
[[ "$v_koff" == IDENTICAL ]] || fail+=("knobs-off: $v_koff, want IDENTICAL")
grep -q 'bug-report-replay: knobs: 3 changed after boot, 0 applied' "$LOG" || fail+=("knobs-off: knobs applied")
c_off="$(png_cmp "$OUT/knobs-rec__4390-vk.png" "$OUT/knobs-off__4390-vk.png")"
[[ "$c_off" == differ* ]] ||
    fail+=("knobs-off: the screenshot is $c_off against the recording's: the case cannot fail")

# The knobs bundle as a bundle written before the member: no knobs.txt, no manifest entry.
python3 -I - "$knobs_zip" "$OUT/knobs-old.zip" <<'PY'
import json, sys, zipfile
src = zipfile.ZipFile(sys.argv[1])
m = json.loads(src.read("manifest.json"))
m["members"] = [x for x in m["members"] if x["name"] != "knobs.txt"]
with zipfile.ZipFile(sys.argv[2], "w", zipfile.ZIP_DEFLATED) as out:
    for n in src.namelist():
        if n == "manifest.json":
            out.writestr(n, json.dumps(m))
        elif n != "knobs.txt":
            out.writestr(n, src.read(n))
PY
REPLAY_ENV=()
replay knobs-old "$OUT/knobs-old.zip" --screenshot-on-frame 4390 --screenshot-native-only \
    --screenshot-output "$OUT/knobs-old__{n}.png"
v_kold="$(verdict "$LOG")"
c_old="$(png_cmp "$OUT/knobs-off__4390-vk.png" "$OUT/knobs-old__4390-vk.png")"
[[ "$c_old" == equal ]] || fail+=("knobs-old: the screenshot is $c_old against knobs-off's")
[[ "$v_kold" == IDENTICAL ]] || fail+=("knobs-old: $v_kold, want IDENTICAL")
grep -q 'bug-report-replay: knobs: the bundle has no knobs.txt' "$LOG" || fail+=("knobs-old: no 'no knobs.txt' line")
python3 -I tools/bugreport.py info "$OUT/knobs-old.zip" 2>&1 | grep -q '^knobs: no knobs.txt' ||
    fail+=("knobs-old: info does not name knobs.txt absent")

# noshot TAG: exit 0 and IDENTICAL with rows > 0 for the last replay; sets NOSHOT to <verdict>/<rows>.
noshot() {
    local tag="$1" v rows
    v="$(verdict "$LOG")"
    rows="$(grep -oE 'bug-report-replay: IDENTICAL rows=[0-9]+' "$LOG" | grep -oE '[0-9]+$')"
    (( RC == 0 )) || fail+=("$tag: exit $RC, want 0 (see $LOG)")
    [[ "$v" == IDENTICAL ]] || fail+=("$tag: $v, want IDENTICAL")
    if (( ${rows:-0} == 0 )); then
        fail+=("$tag: VACUOUS: no tick hash compared")
        rows=VACUOUS
    fi
    NOSHOT="$v/$rows"
}
# argv_check TAG: the last replay's argv line keeps DISPLAY_KEPT's pairs and holds no DISPLAY_DROPPED flag.
argv_check() {
    local tag="$1" line k
    line="$(grep -m1 '^bug-report-replay: argv:' "$LOG")"
    for ((k = 0; k < ${#DISPLAY_KEPT[@]}; k += 2)); do
        [[ "$line" == *" ${DISPLAY_KEPT[k]} ${DISPLAY_KEPT[k + 1]} "* ]] ||
            fail+=("$tag: argv lost ${DISPLAY_KEPT[k]} ${DISPLAY_KEPT[k + 1]}")
    done
    for k in "${DISPLAY_DROPPED[@]}"; do
        [[ "$line" == *" $k "* || "$line" == *" $k" ]] && fail+=("$tag: argv kept $k")
    done
}

REPLAY_ENV=()
replay vk-noshot "$knobs_zip"
noshot vk-noshot
r_vk="$NOSHOT"
grep -q '^bug-report-replay: argv:.* --renderer vk ' "$LOG" || fail+=("vk-noshot: argv lost --renderer vk")
grep -q '^bug-report-replay: argv:.* --screenshot-native-only' "$LOG" && fail+=("vk-noshot: argv kept --screenshot-native-only")

replay display "$display_zip"
noshot display
r_disp="$NOSHOT"
argv_check display

replay native-only "$plain_zip" --screenshot-native-only
noshot native-only
r_nat="$NOSHOT"

res=PASS
if (( ${#fail[@]} )); then
    printf 'bugreport-replay: %s\n' "${fail[@]}"
    res=FAIL
fi
echo "[bugreport-replay] skip=$v_skip skip-late=$v_late plain=$v_plain mods=$v_mods mods-held=$v_held env=$v_env env-refused=$v_ref audio=$n_audio knobs=$n_knobs knobs-off=$v_koff/${c_off// /:} knobs-old=$v_kold vk-noshot=$r_vk display=$r_disp native-only=$r_nat verdict=$res"
[[ "$res" == PASS ]]
