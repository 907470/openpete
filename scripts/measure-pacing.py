#!/usr/bin/env python3
"""
measure-pacing.py — analyze a binary frame-time log produced by openpete's
`--frame-time-log PATH` option (Phase 4.1/4.3 of openpete-frame-pacing.md).

Binary format (little-endian, see core/include/openpete_frame_time_log.h):
  header (32 B): u32 magic='OFTL', u32 version=1, i64 target_period_ns,
                 i64 created_unix_ns, u32 record_size=32, u32 reserved
  record (32 B): u32 frame, u32 slip_count,
                 i64 op_now_ns, i64 deadline_ns, i64 slack_ns

Usage:
    scripts/measure-pacing.py [--histogram] [--plot] LOG_PATH

By default prints a summary: target rate, achieved mean, p50/p99/max
delta between successive presents, slip count, and phase-drift slope
(linear fit of `op_now_ns - deadline_ns` vs frame number).

With --histogram prints a coarse text histogram of inter-present deltas
relative to the target period.

With --plot writes `<log>.deltas.png` and `<log>.phase.png` next to the
log file (silently skipped if matplotlib is unavailable).

Exits non-zero if the log is malformed or empty.
"""
import argparse
import struct
import sys
from pathlib import Path

OFTL_MAGIC = 0x4C54464F  # 'OFTL'
OFTL_VERSION = 1
OFTL_HEADER_SIZE = 32
OFTL_RECORD_SIZE = 32


def read_log(path: Path):
    data = path.read_bytes()
    if len(data) < OFTL_HEADER_SIZE:
        raise SystemExit(f"{path}: too small to contain header ({len(data)} bytes)")
    magic, version, period_ns, created_ns, rec_size, _reserved = struct.unpack_from(
        "<IIqqII", data, 0
    )
    if magic != OFTL_MAGIC:
        raise SystemExit(f"{path}: bad magic 0x{magic:08x} (expected OFTL)")
    if version != OFTL_VERSION:
        raise SystemExit(f"{path}: unsupported version {version}")
    if rec_size != OFTL_RECORD_SIZE:
        raise SystemExit(f"{path}: unsupported record size {rec_size}")

    body = data[OFTL_HEADER_SIZE:]
    if len(body) % OFTL_RECORD_SIZE != 0:
        # Trailing partial record can happen if the run SIGKILLed mid-write;
        # truncate to whole records rather than refusing to read.
        body = body[: (len(body) // OFTL_RECORD_SIZE) * OFTL_RECORD_SIZE]
    n = len(body) // OFTL_RECORD_SIZE
    if n == 0:
        raise SystemExit(f"{path}: no records")

    frames = [0] * n
    slips = [0] * n
    now = [0] * n
    deadline = [0] * n
    slack = [0] * n
    for i in range(n):
        off = i * OFTL_RECORD_SIZE
        f, s, t, d, k = struct.unpack_from("<IIqqq", body, off)
        frames[i] = f
        slips[i] = s
        now[i] = t
        deadline[i] = d
        slack[i] = k

    return {
        "path": path,
        "target_period_ns": period_ns,
        "created_unix_ns": created_ns,
        "frames": frames,
        "slips": slips,
        "now": now,
        "deadline": deadline,
        "slack": slack,
    }


def classify_bimodal(deltas_ns):
    """Bucket inter-present deltas into "fast"/"slow" modes using a
    midpoint threshold derived from the data, not a hardcoded constant
    — the game's swap interval can be N=1 (60 Hz flyby) or N=2 (30 Hz
    gameplay); the actual period depends on the host's measured VBL.

    Strategy:
      1. Take the median delta. If the IQR (p75 - p25) is small relative
         to the median (< 25 %), declare unimodal — no flybys present.
      2. Otherwise pick the threshold at the geometric mean of the
         two natural anchors: the p25 (fast mode) and p75 (slow mode).
         Anything below threshold → fast; above → slow.
      3. Report counts and per-mode rate + p99 + drift slope.

    Returns a dict: {'unimodal': bool, 'threshold_ns': int or None,
                      'fast': stats|None, 'slow': stats|None}
    """
    n = len(deltas_ns)
    if n < 32:
        return {"unimodal": True, "threshold_ns": None,
                "fast": None, "slow": None, "reason": "too few records"}
    s = sorted(deltas_ns)
    p25 = s[n // 4]
    p50 = s[n // 2]
    p75 = s[(3 * n) // 4]
    iqr = p75 - p25
    if p50 > 0 and iqr / p50 < 0.25:
        return {"unimodal": True, "threshold_ns": None,
                "fast": None, "slow": None,
                "reason": f"IQR/median={iqr/p50:.3f} < 0.25"}
    # Threshold at geometric mean of p25 and p75.
    if p25 <= 0 or p75 <= 0:
        return {"unimodal": True, "threshold_ns": None,
                "fast": None, "slow": None,
                "reason": "non-positive percentile"}
    thresh = int((p25 * p75) ** 0.5)
    fast = [d for d in deltas_ns if d <= thresh]
    slow = [d for d in deltas_ns if d > thresh]
    return {
        "unimodal": False,
        "threshold_ns": thresh,
        "fast": _mode_stats(fast),
        "slow": _mode_stats(slow),
    }


def _mode_stats(xs):
    if not xs:
        return None
    s = sorted(xs)
    mean = sum(xs) / len(xs)
    return {
        "count": len(xs),
        "mean_ns": mean,
        "p50_ns": s[len(s) // 2],
        "p99_ns": s[int(0.99 * (len(s) - 1))],
        "min_ns": s[0],
        "max_ns": s[-1],
        "rate_hz": 1e9 / mean if mean > 0 else float("nan"),
    }


def percentile(sorted_xs, p):
    if not sorted_xs:
        return float("nan")
    if p <= 0:
        return sorted_xs[0]
    if p >= 1:
        return sorted_xs[-1]
    idx = int(p * (len(sorted_xs) - 1))
    return sorted_xs[idx]


def linear_slope(ys):
    # Slope of `ys` against x = 0..n-1. Returns ns/frame.
    n = len(ys)
    if n < 2:
        return 0.0
    sum_x = (n - 1) * n / 2.0
    sum_y = float(sum(ys))
    sum_xx = (n - 1) * n * (2 * n - 1) / 6.0
    sum_xy = 0.0
    for i, y in enumerate(ys):
        sum_xy += i * y
    denom = n * sum_xx - sum_x * sum_x
    if denom == 0:
        return 0.0
    return (n * sum_xy - sum_x * sum_y) / denom


def summarize(log, args):
    n = len(log["frames"])
    period_ns = log["target_period_ns"]
    target_hz = 1e9 / period_ns if period_ns > 0 else float("nan")

    # Inter-present deltas in ns (n-1 of them).
    deltas = [log["now"][i + 1] - log["now"][i] for i in range(n - 1)]
    deltas_sorted = sorted(deltas)
    total_ns = log["now"][-1] - log["now"][0]
    mean_delta = total_ns / max(n - 1, 1)
    achieved_hz = 1e9 / mean_delta if mean_delta > 0 else float("nan")

    p50 = percentile(deltas_sorted, 0.50)
    p99 = percentile(deltas_sorted, 0.99)
    pmax = deltas_sorted[-1]
    pmin = deltas_sorted[0]

    # Phase-drift: how `now - deadline` evolves. The slope (ns/frame)
    # tells us cumulative drift per frame; the absolute end value tells
    # us how far off we are after N frames.
    err = [log["now"][i] - log["deadline"][i] for i in range(n)]
    slope = linear_slope(err)
    drift_total = err[-1] - err[0]
    total_slips = sum(log["slips"])

    # Classify before printing so we can qualify mean-rate / phase-drift
    # stats — both average across two distributions when the run mixes
    # 30 Hz gameplay with 60 Hz flybys (Spyro switches via VSync(N)).
    bm = classify_bimodal(deltas)

    print(f"log:                {log['path']}")
    print(f"records:            {n}")
    print(f"target rate:        {target_hz:>10.4f} Hz "
          f"({period_ns/1e6:.4f} ms / frame)")
    if bm["unimodal"]:
        print(f"achieved rate:      {achieved_hz:>10.4f} Hz "
              f"({mean_delta/1e6:.4f} ms / frame)")
        print(f"rate error:         "
              f"{(achieved_hz - target_hz)/target_hz*100:+.3f} %")
    else:
        print(f"achieved rate:      [mixed — see bimodal breakdown below]")
    print(f"delta p50:          {p50/1e6:>10.4f} ms "
          f"({(p50-period_ns)/1e6:+.4f} from target)")
    print(f"delta p99:          {p99/1e6:>10.4f} ms "
          f"({(p99-period_ns)/1e6:+.4f} from target)")
    print(f"delta max:          {pmax/1e6:>10.4f} ms")
    print(f"delta min:          {pmin/1e6:>10.4f} ms")
    if bm["unimodal"]:
        print(f"phase drift total:  {drift_total/1e6:>10.4f} ms "
              f"over {n} frames")
        print(f"phase drift slope:  {slope/1e3:>10.4f} us/frame")
    else:
        print(f"phase drift:        [not meaningful under mixed rates]")
    print(f"slip events:        {total_slips}")

    print()
    if bm["unimodal"]:
        print(f"bimodal check:      unimodal ({bm.get('reason', '')})")
    else:
        thresh_ms = bm["threshold_ns"] / 1e6
        print(f"bimodal check:      threshold {thresh_ms:.3f} ms "
              f"(below = fast / above = slow)")
        for name in ("fast", "slow"):
            m = bm[name]
            if m is None:
                continue
            print(f"  {name:>4}: n={m['count']:>6}  "
                  f"rate={m['rate_hz']:>7.3f} Hz  "
                  f"p50={m['p50_ns']/1e6:>7.3f} ms  "
                  f"p99={m['p99_ns']/1e6:>7.3f} ms  "
                  f"min={m['min_ns']/1e6:>6.3f}  max={m['max_ns']/1e6:>7.3f}")

    if args.histogram:
        print()
        # Trim the top 1 % so a single outlier (cd-read stall, level
        # load) doesn't squash bucket resolution. The slow-mode max is
        # already reported in the bimodal block above.
        ds = sorted(deltas)
        cap = ds[int(0.99 * (len(ds) - 1))]
        trimmed = [d for d in deltas if d <= cap]
        print_histogram(trimmed, period_ns)


def print_histogram(deltas, period_ns):
    """Coarse 30-bucket text histogram of (delta - target_period) in ms."""
    if not deltas:
        return
    errs_ms = sorted((d - period_ns) / 1e6 for d in deltas)
    lo, hi = errs_ms[0], errs_ms[-1]
    if hi - lo < 1e-6:
        print(f"all deltas within rounding of target; spread={hi-lo:.6f} ms")
        return
    buckets = 30
    width = (hi - lo) / buckets
    counts = [0] * buckets
    for e in errs_ms:
        b = min(int((e - lo) / width), buckets - 1)
        counts[b] += 1
    cmax = max(counts)
    bar_width = 40
    print(f"histogram of (delta - target) [ms], n={len(errs_ms)}:")
    for i, c in enumerate(counts):
        edge_lo = lo + i * width
        edge_hi = edge_lo + width
        bar = "#" * (c * bar_width // cmax) if cmax else ""
        print(f"  [{edge_lo:+8.4f} .. {edge_hi:+8.4f}]  {c:6d}  {bar}")


def maybe_plot(log, args):
    if not args.plot:
        return
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("note: matplotlib unavailable; --plot skipped", file=sys.stderr)
        return
    n = len(log["frames"])
    period_ns = log["target_period_ns"]
    deltas_ms = [(log["now"][i + 1] - log["now"][i]) / 1e6 for i in range(n - 1)]
    err_ms = [(log["now"][i] - log["deadline"][i]) / 1e6 for i in range(n)]

    base = log["path"]
    fig, ax = plt.subplots(figsize=(10, 4))
    ax.hist(deltas_ms, bins=60)
    ax.axvline(period_ns / 1e6, color="red", linestyle="--",
               label=f"target ({period_ns/1e6:.3f} ms)")
    ax.set_xlabel("inter-present delta (ms)")
    ax.set_ylabel("count")
    ax.set_title(f"frame-time histogram — {base.name}")
    ax.legend()
    out = base.with_suffix(base.suffix + ".deltas.png")
    fig.tight_layout()
    fig.savefig(out)
    plt.close(fig)
    print(f"wrote {out}")

    fig, ax = plt.subplots(figsize=(10, 4))
    ax.plot(err_ms)
    ax.axhline(0, color="black", linewidth=0.5)
    ax.set_xlabel("frame")
    ax.set_ylabel("now − deadline (ms)")
    ax.set_title(f"phase drift — {base.name}")
    out = base.with_suffix(base.suffix + ".phase.png")
    fig.tight_layout()
    fig.savefig(out)
    plt.close(fig)
    print(f"wrote {out}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", type=Path, help="binary .frametime log from --frame-time-log")
    ap.add_argument("--histogram", action="store_true",
                    help="print text histogram of inter-present deltas")
    ap.add_argument("--plot", action="store_true",
                    help="write PNG histogram + phase-drift plots (needs matplotlib)")
    args = ap.parse_args()

    log = read_log(args.log)
    summarize(log, args)
    maybe_plot(log, args)


if __name__ == "__main__":
    main()
