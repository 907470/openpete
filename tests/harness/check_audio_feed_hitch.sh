#!/usr/bin/env bash
# check_audio_feed_hitch.sh - an isolated host stall the host catches up
# leaves the audio feed's time stretch alone; a second lag, a slip, and a
# host slower than real time engage it.
#
# Run: one headless Spyro 1 attract run per leg, rule and producer-model
# seed (1-3) on the feed's synthetic device at drift 0 on the virtual clock
# (OPENPETE_AUDIO_FEED_SIM=0.0,SEED), host stalls injected through
# OPENPETE_PACE_INJECT_US=US,EVERY,FROM,COUNT as virtual-clock advances
# from paced VBL 1800 (about 30 s in, the feed in its steady state), to
# paced VBL 2700:
#   i55 i70 i80  one stall of 55, 70, 80 ms: late 3.3-4.8 sim periods,
#                under the slip clamp (a tick more than 5 periods late)
#   slip100      one 100 ms stall: the clamp discards 5 periods
#   pair         two 55 ms stalls 6 VBLs (100 ms) apart
#   slow80       20019 us on each of 120 VBLs: 2 s at 0.8x
#   slow96       16536 us on each of 120 VBLs: 2 s at 0.96x
# Rules: `new` (the shipped second-lag rule) and `single`
# (OPENPETE_AUDIO_FEED_STRETCH_RULE=single: every lag engages, the rule the
# second-lag rule replaced, with the same peak-lag sampling).
#
# Instrument: the feed timeline (OPENPETE_AUDIO_FEED_LOG, a sample every
# device period by OPENPETE_AUDIO_FEED_LOG_EVERY=1024): its `lag`,
# `stretch` (value = trigger, PSYX_SPUFEED_ST_BY_*) and `UNDERRUN` events
# after the first lag; the device depth's least value per sample (`low=`)
# in device time: the median over 3 to 1 s before that lag (steady), the
# least from 0.5 s before it to 2 s after (the stall's dip), and the
# median over 0.5 to 2 s after it (recovered); the exit lines `feed: pushed=` (underruns) and
# `feed: stretch:` (lags, tempo_lo/tempo_hi: the content rate's extremes
# over the run while data plays). Engagement delay: VBL tag of the stretch
# event less the tag of the first lag event.
# Engagement: every run records at least one lag event (the stall reached
# the feed); a run without one is VACUOUS.
#
# Pass, per seed: under `new`, i55/i70/i80 record 0 stretch events, 0
# underruns and the content rate within 0.5% (tempo_lo >= 0.995, tempo_hi
# <= 1.005), and slip100, pair, slow80 and slow96 record a stretch event;
# under `single` every leg records one (the A/B engages).
#
# Usage: tests/harness/check_audio_feed_hitch.sh [--build-dir=DIR] [--seeds=1,2,3] [--legs=a,b]
# Exit 0 PASS, 2 FAIL, 3 VACUOUS, 64 SKIP (binary or disc missing).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

SEEDS=1,2,3
LEGS=i55,i70,i80,slip100,pair,slow80,slow96
for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
        --seeds=*) SEEDS="${a#--seeds=}";;
        --legs=*) LEGS="${a#--legs=}";;
        *) echo "check_audio_feed_hitch: unknown argument $a" >&2; exit 64;;
    esac
done

declare -A INJECT=([i55]=55000,1,1800,1 [i70]=70000,1,1800,1 [i80]=80000,1,1800,1
                   [slip100]=100000,1,1800,1 [pair]=55000,6,1800,2
                   [slow80]=20019,1,1800,120 [slow96]=16536,1,1800,120)
LIMIT=2700
ROOT="${PG_BUILD_DIR:-build/linux-release}/audio-feed-hitch"
game=spyro-1
pg_resolve "$game"
if [[ ! -x "$PG_BIN" || ! -f "$PG_DISC" ]]; then
    echo "[audio-feed-hitch] SKIP (binary or disc missing)"
    exit 64
fi

# run_one <leg> <rule> <seed>: one capture into $ROOT/<leg>-<rule>-<seed>.
run_one() {
    local leg="$1" rule="$2" seed="$3"
    local out="$ROOT/$leg-$rule-$seed"
    mkdir -p "$out"
    rm -f "${out:?}/run.log" "${out:?}/feed.log"
    local -a env=("OPENPETE_AUDIO_FEED_SIM=0.0,$seed" "OPENPETE_AUDIO_FEED_LOG=$out/feed.log"
                  "OPENPETE_AUDIO_FEED_LOG_EVERY=1024" "OPENPETE_PACE_INJECT_US=${INJECT[$leg]}")
    [[ "$rule" == single ]] && env+=("OPENPETE_AUDIO_FEED_STRETCH_RULE=single")
    PG_CAPTURE_ENV=("${env[@]}")
    PG_CAPTURE_TIMEOUT=600
    pg_capture "$out/run.log" "$out/feed.log" -- --no-card --mute 0 --frame-limit "$LIMIT" </dev/null
}

IFS=',' read -ra seed_list <<<"$SEEDS"
IFS=',' read -ra leg_list <<<"$LEGS"
jobs_run=()
for leg in "${leg_list[@]}"; do
    [[ -n "${INJECT[$leg]:-}" ]] || { echo "[audio-feed-hitch] unknown leg $leg" >&2; exit 64; }
    for seed in "${seed_list[@]}"; do
        for rule in new single; do jobs_run+=("$leg $rule $seed"); done
    done
done
# Two game processes at a time.
rcs=()
for ((i = 0; i < ${#jobs_run[@]}; i += 2)); do
    read -r l1 r1 s1 <<<"${jobs_run[i]}"
    run_one "$l1" "$r1" "$s1" & p1=$!
    p2=""
    if (( i + 1 < ${#jobs_run[@]} )); then
        read -r l2 r2 s2 <<<"${jobs_run[i + 1]}"
        run_one "$l2" "$r2" "$s2" & p2=$!
    fi
    wait "$p1"; rcs+=("$?")
    [[ -n "$p2" ]] && { wait "$p2"; rcs+=("$?"); }
done

python3 -I - "$ROOT" "${#jobs_run[@]}" "${jobs_run[@]}" -- "${rcs[@]}" <<'EOF'
import re, sys
root, n = sys.argv[1], int(sys.argv[2])
jobs = sys.argv[3:3 + n]
rcs = sys.argv[3 + n + 1:]
KV = re.compile(r"(\w+)=(\S+)")
CATCH_UP = ("i55", "i70", "i80")
fails, vacuous, rows = [], [], {}
for job, rc in zip(jobs, rcs):
    leg, rule, seed = job.split()
    d = f"{root}/{leg}-{rule}-{seed}"
    tag = f"{leg}/{rule}/seed{seed}"
    if rc != "0":
        fails.append(f"{tag}: run exited {rc}"); continue
    ev = []
    for line in open(f"{d}/feed.log", errors="replace"):
        p = line.split()
        if len(p) < 3 or line.startswith("#"):
            continue
        kv = dict(KV.findall(line))
        ev.append((p[1], int(kv["tag"]), int(kv.get("low", "-1")), float(kv.get("value", "0")),
                   int(kv["consumed"])))
    run = open(f"{d}/run.log", errors="replace").read()
    st = dict(KV.findall(re.findall(r"feed: stretch: .*", run)[-1]))
    fd = dict(KV.findall(re.findall(r"feed: pushed=.*", run)[-1]))
    lags = [e for e in ev if e[0] == "lag"]
    if not lags:
        vacuous.append(f"{tag}: no lag event"); continue
    t0, ms0 = lags[0][1], lags[0][4]
    after = [e for e in ev if e[4] >= ms0]
    stretch = [e for e in after if e[0] == "stretch"]
    under = [e for e in after if e[0] == "UNDERRUN"]
    def lows(a, b):
        return sorted(e[2] for e in ev if e[0] == "tick" and e[2] >= 0 and ms0 + a * 44100 <= e[4] <= ms0 + b * 44100)
    steady, window, post = lows(-3, -1), lows(-0.5, 2), lows(0.5, 2)
    row = dict(lag_tag=t0, lag=lags[0][3], lags=len(lags),
               stretch_delay=(stretch[0][1] - t0) if stretch else None,
               trigger=int(stretch[0][3]) if stretch else 0, underruns=len(under),
               depth_steady=steady[len(steady) // 2] if steady else None,
               depth_min=window[0] if window else None, depth_post=post[len(post) // 2] if post else None,
               depth_min_run=int(fd["depth_min"]), tempo_lo=float(st["tempo_lo"]), tempo_hi=float(st["tempo_hi"]))
    rows[(leg, rule, seed)] = row
    print(f"[audio-feed-hitch] {tag} lag={row['lag']:.2f}@{t0} lags={row['lags']} "
          f"stretch={'none' if not stretch else 'delay %d VBL by %d' % (row['stretch_delay'], row['trigger'])} "
          f"underruns={row['underruns']} depth steady={row['depth_steady']} dip_min={row['depth_min']} "
          f"recovered={row['depth_post']} "
          f"run_min={row['depth_min_run']} tempo={row['tempo_lo']:.5f}..{row['tempo_hi']:.5f}")
    if rule == "new" and leg in CATCH_UP:
        if stretch: fails.append(f"{tag}: stretch engaged")
        if under: fails.append(f"{tag}: {len(under)} underrun(s)")
        if row["tempo_lo"] < 0.995 or row["tempo_hi"] > 1.005:
            fails.append(f"{tag}: tempo {row['tempo_lo']:.5f}..{row['tempo_hi']:.5f} outside 0.5%")
    elif not stretch:
        fails.append(f"{tag}: no stretch engaged")
# Spread over seeds, per leg and rule.
for leg in dict.fromkeys(j.split()[0] for j in jobs):
    for rule in ("new", "single"):
        rs = [r for (l, ru, s), r in rows.items() if l == leg and ru == rule]
        if not rs: continue
        def span(k):
            v = [r[k] for r in rs if r[k] is not None]
            return f"{min(v)}..{max(v)}" if v else "none"
        print(f"[audio-feed-hitch] spread {leg}/{rule} n={len(rs)} stretch_delay_vbl={span('stretch_delay')} "
              f"underruns={span('underruns')} depth_dip_min={span('depth_min')} "
              f"tempo_lo={min(r['tempo_lo'] for r in rs):.5f} tempo_hi={max(r['tempo_hi'] for r in rs):.5f}")
if fails:
    print("[audio-feed-hitch] verdict=FAIL: " + "; ".join(fails)); sys.exit(2)
if vacuous:
    print("[audio-feed-hitch] verdict=VACUOUS: " + "; ".join(vacuous)); sys.exit(3)
print("[audio-feed-hitch] verdict=PASS"); sys.exit(0)
EOF
