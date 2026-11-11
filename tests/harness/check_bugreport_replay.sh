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
# and seven replays:
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
#
# Every recording runs under --mute 1.
#
# Instrument: the replay's verdict line (`bug-report-replay: IDENTICAL |
# DIVERGED | REFUSED ...`), its `launch flags kept|dropped` line, the level
# skip's `[skip-to-level] induced` line, and the audit's `[tsc-audit] total`
# line; `tools/bugreport.py audio` over each recording's audio-feed.txt
# (exit 3 when the feed played data and holds no event).
#
# Pass criterion: skip IDENTICAL with the launch flags kept and the skip
# induced in the replay; mods IDENTICAL; mods-held DIVERGED on
# game-extra:modsave; skip-late DIVERGED with the launch flags dropped and
# no induction; plain IDENTICAL with no launch line; env IDENTICAL with an
# `env kept OPENPETE_TERRAIN_TSC_AUDIT` line and a `[tsc-audit] total` line
# whose hits are nonzero; env-refused exit 64 naming OPENPETE_PLAYER_POSE;
# every recording's audio-feed.txt parses (exit 0, not VACUOUS) and states
# mute=1; the audio recording's names device=synthetic, has played data,
# and holds a nonzero event count with a prime, a slip and a stretch.
#
# Verdict line:
#   [bugreport-replay] skip=.. skip-late=.. plain=.. mods=.. mods-held=.. env=.. env-refused=.. audio=<events> verdict=PASS|FAIL
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
REC_ENV=()
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

res=PASS
if (( ${#fail[@]} )); then
    printf 'bugreport-replay: %s\n' "${fail[@]}"
    res=FAIL
fi
echo "[bugreport-replay] skip=$v_skip skip-late=$v_late plain=$v_plain mods=$v_mods mods-held=$v_held env=$v_env env-refused=$v_ref audio=$n_audio verdict=$res"
[[ "$res" == PASS ]]
