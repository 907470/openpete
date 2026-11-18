#!/usr/bin/env bash
# check_terrain_claims.sh - the retained-terrain claim census: every corpus
# pose draws every visible sector from the retained cache, none claimed back
# to the CPU walk.
#
# Class: the retained terrain cache's coverage (the cache is never degraded by
# a feature; a sector the cache cannot carry is claimed to the CPU walk and
# counted by reason). A claim on a corpus pose is a sector class the cache
# does not yet carry, or an arena that refused a grant.
#
# Rows: games/<game>/corpus/terrain-claims.manifest, the freecam corpus row
# form `<level>[:<area>] | <pose> | [start VBL] | [launch]`. Each row runs one
# leg per LOD tier (--lod original and --lod extended; --lod picks one). Leg id
# `<pg_pose_slug>__<lod>`.
#
# Instrument: one headless run per leg, 4:3, native-only, the retained cache
# forced on under software vk (OPENPETE_TERRAIN_CACHE_FORCE=1), a freecam pose
# captured at the row's VBL (default the freecam corpus's canonical VBL).
# OPENPETE_TERRAIN_GPU_CENSUS=1 prints one `[tgpu-cen f=<VBL>] sub=<n>
# secs=<n> claims=<n> near=.. arena=..` line per walk: the sectors the walk
# retained (secs), the sectors it claimed (claims), and the claims by class.
# The leg's value is the last line with sub=0 (a canonical walk, not a
# sub-tick re-extract) at or before the capture VBL. OPENPETE_TERRAIN_GPU_DBG=1
# adds the `tercache:` exit line, whose grow count is printed beside the row.
#
# Pass, per leg: exit 0, no crash report, claims 0, every claim class 0 (near
# portal anim scroll fill arena col slot tp nearb prim pcov near_pa near_ca;
# nodraw counts sectors drawn by neither path and is not a claim).
# Engagement, per leg: the capture exists, a census line exists, and the walk
# saw a sector (secs + claims > 0); otherwise the leg is VACUOUS. A leg whose
# every sector is claimed (secs 0, claims > 0) engaged and FAILs. A leg
# whose game refused its pose at option parse FAILs.
#
# --calibrate: the negative control. The selected legs run with
# OPENPETE_TERRAIN_CANIMNEAR=0, which sends every colour-anim sector whose
# sphere crosses the near threshold back to the CPU walk (class near_ca). The
# census must read claims > 0 on at least one leg; a selection that reads 0
# everywhere means the instrument is blind and FAILs. Select rows that show
# colour-anim near sectors (the `canimnear=` field of the census line), for
# example `--game spyro-3 --filter lost_fleet:1`.
#
# Reduced sample: --filter SUBSTR keeps rows whose level field or tile key
# contains SUBSTR (pg_row_selected); --stride N[:K] then keeps the K-th of
# every N selected rows (K default 0). The full corpus is 43 rows, 86 legs.
#
# Concurrency: the legs of every selected game run CORPUS_JOBS at a time
# (scripts/corpus-jobs.sh; CORPUS_JOBS=1 runs them serially). A leg writes
# only its own <leg>.log, <leg>-vk.png and <leg>.rc, and boots in its own
# sandbox root (pg_capture), so no file is shared between legs. results.tsv
# is assembled from the <leg>.rc files after every leg has exited, in
# manifest order, and is identical at every job count; a leg that left no
# .rc fails the run under its name.
#
# Usage:
#   tests/harness/check_terrain_claims.sh [--game SLUG]... [--build-dir=DIR]
#       [--out DIR] [--filter SUBSTR] [--stride N[:K]]
#       [--lod original|extended|both] [--calibrate]
# --game may repeat; default every game with a manifest (spyro-1 spyro-2
# spyro-3). Output: <out>/<game>/<leg>.log and <leg>-vk.png, and
# <out>/results.tsv (one row per leg); default <out> is
# <build-dir>/terrain-claims[-calibrate].
# Exit 0 PASS, 1 FAIL, 2 VACUOUS, 64 SKIP (no binary, disc or Vulkan loader)
# or bad usage.
set -u

# Unattended launcher: every run this script starts is silenced.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

cd "$(dirname "$0")/../.."
# shellcheck source=scripts/corpus-jobs.sh
source scripts/corpus-jobs.sh
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

BUILD_DIR=build/linux-release
OUT=""
FILTER=""
STRIDE=1
STRIDE_K=0
LODS="original extended"
CAL=0
GAMES=()
usage() { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 64; }
prev=""
for a in "$@"; do
    case "$prev" in
        --build-dir) BUILD_DIR="$a"; prev=""; continue;;
        --out)       OUT="$a";       prev=""; continue;;
        --filter)    FILTER="$a";    prev=""; continue;;
        --stride)    a="--stride=$a";;
        --lod)       a="--lod=$a";;
        --game)      GAMES+=("$a");  prev=""; continue;;
    esac
    case "$a" in
        --build-dir=*) BUILD_DIR="${a#*=}";;
        --out=*)       OUT="${a#*=}";;
        --filter=*)    FILTER="${a#*=}";;
        --game=*)      GAMES+=("${a#*=}");;
        --stride=*)
            s="${a#*=}"
            [[ "$s" =~ ^([1-9][0-9]*)(:([0-9]+))?$ ]] ||
                { echo "terrain-claims: --stride expects N or N:K, got '$s'" >&2; exit 64; }
            STRIDE="${BASH_REMATCH[1]}"; STRIDE_K="${BASH_REMATCH[3]:-0}"
            (( STRIDE_K < STRIDE )) ||
                { echo "terrain-claims: --stride N:K needs K < N, got '$s'" >&2; exit 64; };;
        --lod=*)
            case "${a#*=}" in
                original|extended) LODS="${a#*=}";;
                both)              LODS="original extended";;
                *) echo "terrain-claims: --lod expects original, extended or both" >&2; exit 64;;
            esac;;
        --calibrate)   CAL=1;;
        --build-dir|--out|--filter|--stride|--lod|--game) ;;
        -h|--help)     usage;;
        *) echo "terrain-claims: unknown argument $a" >&2; exit 64;;
    esac
    prev="$a"
done
[[ -z "$prev" || "$prev" != --* || "$prev" == --calibrate || "$prev" == *=* ]] ||
    { echo "terrain-claims: $prev needs a value" >&2; exit 64; }
(( ${#GAMES[@]} )) || GAMES=(spyro-1 spyro-2 spyro-3)
[[ -n "$OUT" ]] || OUT="$BUILD_DIR/terrain-claims$( (( CAL )) && echo -calibrate)"

ldconfig -p 2>/dev/null | grep -q 'libvulkan\.so\.1' ||
    { echo "terrain-claims: SKIP (no Vulkan loader)"; exit 64; }
# shellcheck source=scripts/lib/swrender.sh
source scripts/lib/swrender.sh
sw_render_env

mkdir -p "$OUT"
RESULTS="$OUT/results.tsv"
rm -f "${RESULTS:?}"

trim() { sed 's/^[[:space:]]*//; s/[[:space:]]*$//' <<<"$1"; }

# run_leg ID STEM VBL LEVEL -- <run args...>: one leg, run in the background
# by cj_run. Its exit status goes to STEM.rc, the only channel from the job
# back to the harness; the caller deletes STEM.rc before queueing the leg, so
# a job killed before it writes leaves no stale status behind.
run_leg() {
    local id="$1" stem="$2" vbl="$3" lvl="$4"; shift 5
    # pg_capture deletes the log and the capture before the run.
    pg_capture "$stem.log" "$stem-vk.png" -- "$@" </dev/null
    local rc=$?
    echo "$rc" > "$stem.rc" || return 1
    echo "  [$PG_GAME $id] level=$lvl vbl=$vbl rc=$rc"
}

# The legs in launch order, one `game<TAB>id<TAB>vbl<TAB>stem` entry each;
# results.tsv is written from this list, not in completion order.
ORDER=()
for game in "${GAMES[@]}"; do
    PG_BUILD_DIR="$BUILD_DIR"
    pg_resolve "$game" || exit 64
    [[ -x "$PG_BIN"  ]] || { echo "terrain-claims: SKIP (no binary at $PG_BIN)"; exit 64; }
    [[ -f "$PG_DISC" ]] || { echo "terrain-claims: SKIP (no disc at $PG_DISC)"; exit 64; }
    pg_cap_settings
    MAN="$(pg_corpus_manifest "$PG_GAME" terrain-claims.manifest)"
    pg_manifest "$MAN" pipe
    GOUT="$OUT/$PG_GAME"
    mkdir -p "$GOUT"

    # The manifest is read into arrays before any run: a child that inherits
    # the loop's stdin would eat the rows it has not reached yet.
    KEYS=(); LEVELS=(); POSES=(); STARTS=(); LAUNCH=()
    n=0
    while IFS='|' read -r f1 f2 f3 f4; do
        f1="$(trim "$f1")"
        [[ -z "$f1" || "${f1:0:1}" == "#" ]] && continue
        f2="$(trim "${f2:-}")"; f3="$(trim "${f3:-}")"; f4="$(trim "${f4:-}")"
        [[ -n "$f2" ]] || { echo "terrain-claims: FAIL (row '$f1' has no pose, $MAN)"; exit 1; }
        key="$(pg_pose_slug "$f1" "$f2")"
        pg_row_selected "$FILTER" "$f1" "$key" || continue
        (( n++ % STRIDE == STRIDE_K )) || continue
        KEYS+=("$key"); LEVELS+=("$f1"); POSES+=("$f2")
        STARTS+=("${f3:-$PG_FREECAM_START}"); LAUNCH+=("$f4")
    done < "$MAN"

    for i in "${!KEYS[@]}"; do
        lvl="${LEVELS[$i]}" vbl="${STARTS[$i]}"
        if [[ "$lvl" == *:* ]]; then boot=(--skip-to-area "$lvl"); else boot=(--skip-to-level "$lvl"); fi
        pg_split_launch "${LAUNCH[$i]}"
        for lod in $LODS; do
            id="${KEYS[$i]}__$lod"
            stem="$GOUT/$id"
            PG_CAPTURE_ENV=(OPENPETE_TERRAIN_CACHE_FORCE=1 OPENPETE_TERRAIN_GPU_CENSUS=1
                            OPENPETE_TERRAIN_GPU_DBG=1 "${ROW_ENV[@]}")
            (( CAL )) && PG_CAPTURE_ENV+=(OPENPETE_TERRAIN_CANIMNEAR=0)
            PG_CAPTURE_TIMEOUT=600
            rm -f "${stem:?}.rc"
            ORDER+=("$PG_GAME"$'\t'"$id"$'\t'"$vbl"$'\t'"$stem")
            # cj_run forks the job with this iteration's PG_* and row arrays.
            cj_run run_leg "$id" "$stem" "$vbl" "$lvl" -- \
                --renderer vk --mute 1 --aspect 4:3 --no-card --lod "$lod" \
                "${boot[@]}" --freecam-pose "${POSES[$i]}" "${ROW_ARGS[@]}" \
                --screenshot-native-only --screenshot-on-frame "$vbl" \
                --screenshot-output "$stem.png" \
                --frame-limit $((vbl + 10)) --max-runtime-seconds 580
        done
    done
done
cj_wait || echo "terrain-claims: a leg job failed to record its exit status"

legs=0 lost=()
for e in "${ORDER[@]}"; do
    IFS=$'\t' read -r game id vbl stem <<<"$e"
    rc="$(cat "$stem.rc" 2>/dev/null)"
    if ! [[ "$rc" =~ ^[0-9]+$ ]]; then lost+=("$game/$id"); continue; fi
    printf '%s\t%s\t%s\t%s\t%s\n' "$game" "$id" "$vbl" "$rc" "$stem" >> "$RESULTS"
    legs=$((legs + 1))
done
if (( ${#lost[@]} )); then
    echo "terrain-claims: FAIL (no exit status recorded: ${lost[*]})"
    exit 1
fi

if (( legs == 0 )); then
    echo "terrain-claims: VACUOUS (no leg selected: filter='$FILTER' stride=$STRIDE:$STRIDE_K)"
    exit 2
fi

python3 - "$RESULTS" "$CAL" <<'TC_PY'
import os, re, sys

results, cal = sys.argv[1], sys.argv[2] == "1"
CLASSES = ("near portal anim scroll fill arena col slot tp nearb prim pcov "
           "near_pa near_ca").split()
CEN = re.compile(r"\[tgpu-cen f=(\d+)\] sub=(\d+) (.*)")
GROW = re.compile(r"^tercache:.*?\bgrow=(\d+)")

fail = vac = 0
total_claims = 0
print("leg\tsecs\tclaims\tarena\tclasses\tgrow\tverdict")
for line in open(results):
    game, lid, vbl, rc, stem = line.rstrip("\n").split("\t")
    vbl, rc = int(vbl), int(rc)
    log, png = stem + ".log", stem + "-vk.png"
    text = open(log, errors="replace").read() if os.path.isfile(log) else ""
    last, grow = None, "-"
    for l in text.splitlines():
        m = CEN.match(l)
        if m and int(m.group(1)) <= vbl and int(m.group(2)) == 0:
            last = dict(kv.split("=", 1) for kv in m.group(3).split())
        g = GROW.match(l)
        if g:
            grow = g.group(1)
    crash = "=== openpete crash" in text
    refused = rc != 0 and "openpete: --freecam-pose: " in text
    name = f"{game}/{lid}"
    if refused:
        verdict = f"FAIL(pose refused, rc={rc})"
        fail += 1
        print(f"{name}\t-\t-\t-\t-\t{grow}\t{verdict}")
        continue
    if crash or (rc != 0 and os.path.isfile(png)):
        verdict = f"FAIL(rc={rc} crash={int(crash)})"
        fail += 1
        print(f"{name}\t-\t-\t-\t-\t{grow}\t{verdict}")
        continue
    seen = int(last.get("secs", 0)) + int(last.get("claims", 0)) if last else 0
    if last is None or not os.path.isfile(png) or seen == 0:
        why = ("no capture" if not os.path.isfile(png) else
               "no census line" if last is None else "secs=0 claims=0")
        vac += 1
        print(f"{name}\t{(last or {}).get('secs', '-')}\t-\t-\t-\t{grow}\tVACUOUS({why}, rc={rc})")
        continue
    nz = {c: int(last.get(c, 0)) for c in CLASSES if int(last.get(c, 0))}
    claims, arena = int(last["claims"]), int(last.get("arena", 0))
    total_claims += claims
    bad = claims != 0 or arena != 0 or bool(nz)
    verdict = "PASS"
    if not cal and bad:
        verdict = "FAIL"
        fail += 1
    elif cal:
        verdict = "claims" if bad else "none"
    cls = ",".join(f"{k}={v}" for k, v in nz.items()) or "-"
    print(f"{name}\t{last['secs']}\t{claims}\t{arena}\t{cls}\t{grow}\t{verdict}")

if cal:
    if fail:
        print("terrain-claims: FAIL (calibration leg crashed or exited nonzero)"); sys.exit(1)
    if vac:
        print("terrain-claims: VACUOUS (calibration leg did not engage)"); sys.exit(2)
    if total_claims == 0:
        print("terrain-claims: FAIL (calibration: OPENPETE_TERRAIN_CANIMNEAR=0 read 0 claims; "
              "the census is blind or the selection holds no colour-anim near sector)")
        sys.exit(1)
    print(f"terrain-claims: PASS (calibration: {total_claims} claims under OPENPETE_TERRAIN_CANIMNEAR=0)")
    sys.exit(0)
if fail:
    print("terrain-claims: FAIL"); sys.exit(1)
if vac:
    print("terrain-claims: VACUOUS"); sys.exit(2)
print("terrain-claims: PASS")
TC_PY
