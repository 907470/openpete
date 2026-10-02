#!/usr/bin/env python3
"""fps-summary.py - concise present-FPS summary from a --frame-time-log.

Complements measure-pacing.py (which is tuned for 30/60 Hz gameplay pacing).
For a uniform high render-fps target this reports the metrics that matter for a
perf A/B: average FPS, the 1%/0.1%-low FPS (worst sustained, = high frametime
percentiles), and the frametime tail. One or more logs may be given; with two,
the second is reported as a delta vs the first (baseline).

Usage: fps-summary.py LOG [LOG2 ...]
"""
import sys, struct

HDR = 32        # magic,version,period,created,record_size,reserved
REC = 32        # frame u32, slip u32, op_now_ns i64, deadline_ns i64, slack_ns i64


def load_deltas_ms(path):
    with open(path, "rb") as f:
        data = f.read()
    body = data[HDR:]
    n = len(body) // REC
    ts = [struct.unpack_from("<q", body, i * REC + 8)[0] for i in range(n)]  # op_now_ns
    ts.sort()  # presents are recorded in order, but be safe
    deltas = [(ts[i] - ts[i - 1]) / 1e6 for i in range(1, n)]  # ms
    wall_s = (ts[-1] - ts[0]) / 1e9 if n > 1 else 0.0
    return ts, deltas, wall_s


def pct(sorted_xs, p):
    if not sorted_xs:
        return float("nan")
    i = min(len(sorted_xs) - 1, max(0, int(round(p * (len(sorted_xs) - 1)))))
    return sorted_xs[i]


def summarize(path):
    ts, deltas, wall_s = load_deltas_ms(path)
    n_pres = len(ts)
    if n_pres < 2:
        return None
    avg_fps = (n_pres - 1) / wall_s if wall_s > 0 else float("nan")
    sd = sorted(deltas)
    # 1%-low / 0.1%-low FPS = reciprocal of the high frametime percentiles
    ft_p50, ft_p99, ft_p999, ft_max = pct(sd, .5), pct(sd, .99), pct(sd, .999), sd[-1]
    return {
        "path": path, "presents": n_pres, "wall_s": wall_s, "avg_fps": avg_fps,
        "low1_fps": 1000.0 / ft_p99 if ft_p99 else float("nan"),
        "low01_fps": 1000.0 / ft_p999 if ft_p999 else float("nan"),
        "ft_p50": ft_p50, "ft_p99": ft_p99, "ft_p999": ft_p999, "ft_max": ft_max,
    }


def fmt(s):
    return (f"  presents={s['presents']}  wall={s['wall_s']:.1f}s\n"
            f"  avg={s['avg_fps']:.1f} fps   1%-low={s['low1_fps']:.1f}   0.1%-low={s['low01_fps']:.1f}\n"
            f"  frametime ms: p50={s['ft_p50']:.3f}  p99={s['ft_p99']:.3f}  "
            f"p99.9={s['ft_p999']:.3f}  max={s['ft_max']:.3f}")


def main(argv):
    if len(argv) < 2:
        print(__doc__); return 2
    summaries = []
    for p in argv[1:]:
        s = summarize(p)
        if s is None:
            print(f"{p}: too few records"); continue
        print(f"\n{p}:"); print(fmt(s)); summaries.append(s)
    if len(summaries) >= 2:
        b, o = summaries[0], summaries[-1]
        d = lambda a, c: f"{c - a:+.1f} ({(c - a) / a * 100:+.1f}%)" if a else "n/a"
        print("\n=== delta (last vs first) ===")
        print(f"  avg fps:   {b['avg_fps']:.1f} -> {o['avg_fps']:.1f}   {d(b['avg_fps'], o['avg_fps'])}")
        print(f"  1%-low:    {b['low1_fps']:.1f} -> {o['low1_fps']:.1f}   {d(b['low1_fps'], o['low1_fps'])}")
        print(f"  ft p99 ms: {b['ft_p99']:.3f} -> {o['ft_p99']:.3f}   {d(b['ft_p99'], o['ft_p99'])}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
