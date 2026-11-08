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
# and five replays:
#   skip         the skip bundle as written
#   skip-late    the skip bundle with its manifest's anchor seq set to 2:
#                the replay drops the launch flags (negative control)
#   plain        the plain bundle
#   env          the skip bundle with OPENPETE_TERRAIN_TSC_AUDIT=1 named in
#                OPENPETE_BUGREPORT_REPLAY_ENV, rendering one native
#                screenshot so the terrain audit runs
#   env-refused  OPENPETE_BUGREPORT_REPLAY_ENV naming OPENPETE_PLAYER_POSE
#
# Instrument: the replay's verdict line (`bug-report-replay: IDENTICAL |
# DIVERGED | REFUSED ...`), its `launch flags kept|dropped` line, the level
# skip's `[skip-to-level] induced` line, and the audit's `[tsc-audit] total`
# line.
#
# Pass criterion: skip IDENTICAL with the launch flags kept and the skip
# induced in the replay; skip-late DIVERGED with the launch flags dropped and
# no induction; plain IDENTICAL with no launch line; env IDENTICAL with an
# `env kept OPENPETE_TERRAIN_TSC_AUDIT` line and a `[tsc-audit] total` line
# whose hits are nonzero; env-refused exit 64 naming OPENPETE_PLAYER_POSE.
#
# Verdict line:
#   [bugreport-replay] skip=.. skip-late=.. plain=.. env=.. env-refused=.. verdict=PASS|FAIL
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
record() {
    local tag="$1" at="$2"; shift 2
    PG_CAPTURE_ENV=("XDG_DATA_HOME=$OUT/xdg-$tag" "OPENPETE_BUGREPORT_AT=$at" "OPENPETE_BUGREPORT_NOTE=0")
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
if (( ${#fail[@]} )); then
    printf 'bugreport-replay: %s\n' "${fail[@]}"
    echo "[bugreport-replay] verdict=FAIL"
    exit 1
fi

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
echo "[bugreport-replay] skip=$v_skip skip-late=$v_late plain=$v_plain env=$v_env env-refused=$v_ref verdict=$res"
[[ "$res" == PASS ]]
