#!/usr/bin/env python3
"""entry.py - level-entry present cadence of one leg per pace log.

    python3 scripts/pacing/entry.py [--json OUT] LEG.oppl...

Presents are PRESENT records not flagged not-live and not REPEAT (a repeat
shows no new image). Entry t0 is anchored on the load, never on a present
interval:

  * an entry the pace log flags (a skip-to-level load, a Spyro 2 / 3 portal):
    the first live VBL after the last not-live VBL run longer than 0.2 s;
  * an entry it does not flag (a Spyro 1 portal keeps presenting through the
    transition, so the only not-live run is the boot's, ending within 3 s of
    the first present): the exec start of the longest VBL that starts 3 s or
    more after that run, which is the guest tick that loads the level.
    A present-interval anchor (the first interval over 12 ms, or the largest
    one) misfires once steady play drops presents: those misses reach 28 ms,
    the portal stall's own intervals 26-31 ms.

Columns: steady rate (new images per second over [t0 + 6 s, end)); images
lost in [t0, t0 + 1 s), [t0, t0 + 5 s) and [t0 + 1 s, t0 + 5 s) against the
steady rate, and in the last 4 s of the log (the baseline for isolated
misses); intervals over 1.5 steady periods in [t0 + 1 s, t0 + 5 s) and in the
last 4 s; the largest interval of the first 5 s with its end relative to t0,
and the three largest.
"""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pacelog                                                  # noqa: E402

NOT_LIVE_RUN_NS = int(0.2e9)
BOOT_SLACK_NS = int(3e9)
STEADY_FROM_NS = int(6e9)


def new_images(log):
    """Sorted timestamps of live, non-repeat presents."""
    return sorted(r.t_ns for r in log.records
                  if r.type == pacelog.PRESENT and not (r.flags & pacelog.F_NOT_LIVE)
                  and (r.flags & 3) != pacelog.REPEAT)


def last_not_live_end(log):
    """The first live VBL after the last not-live VBL run longer than
    NOT_LIVE_RUN_NS, or None."""
    best, run_a, run_live = None, None, None
    for r in log.of(pacelog.VBL):
        live = not (r.flags & pacelog.F_NOT_LIVE)
        if live != run_live:
            if live and run_live is False and r.t_ns - run_a > NOT_LIVE_RUN_NS:
                best = r.t_ns
            run_a, run_live = r.t_ns, live
    return best


def load_tick(log, after_ns):
    """Exec start of the longest-running live VBL starting at or after
    `after_ns`, or None."""
    vbls = [r for r in log.of(pacelog.VBL)
            if not (r.flags & pacelog.F_NOT_LIVE) and r.exec_start_ns >= after_ns
            and r.exec_end_ns > r.exec_start_ns]
    if not vbls:
        return None
    return max(vbls, key=lambda r: r.exec_end_ns - r.exec_start_ns).exec_start_ns


def entry_t0(log, pres):
    """(t0_ns, how): the entry anchor of a leg (see the module text)."""
    t0 = last_not_live_end(log)
    if t0 is not None and t0 >= pres[0] + BOOT_SLACK_NS:
        return t0, "not-live run end"
    after = (t0 if t0 is not None else pres[0]) + BOOT_SLACK_NS
    lt = load_tick(log, after)
    if lt is None:
        raise ValueError("no live VBL 3 s after boot to anchor the entry on")
    return lt, "load tick"


def leg(path):
    """The entry metrics of one pace log, as a dict."""
    log = pacelog.read(path)
    pres = new_images(log)
    if len(pres) < 2:
        raise ValueError("fewer than two live presents")
    t0, how = entry_t0(log, pres)
    end = pres[-1]
    st = [t for t in pres if t >= t0 + STEADY_FROM_NS]
    if len(st) < 2:
        raise ValueError("log ends before t0 + 6 s")
    rate = (len(st) - 1) / ((st[-1] - st[0]) / 1e9)
    per_ms = 1e3 / rate

    def n_in(a, b):
        return sum(1 for t in pres if t0 + a * 1e9 <= t < t0 + b * 1e9)

    def ivs(a, b):
        p = [t for t in pres if a <= t < b]
        return [((y - t0) / 1e9, (y - x) / 1e6) for x, y in zip(p, p[1:])]

    tail_a = end - 4e9
    iv5 = ivs(t0, t0 + 5e9)
    top = sorted(iv5, key=lambda x: -x[1])[:3]
    return {"leg": Path(path).name.replace(".oppl", ""), "t0_how": how,
            "t0_s": (t0 - pres[0]) / 1e9, "live_s": (end - t0) / 1e9, "rate": rate,
            "lost1": rate - n_in(0, 1), "lost5": rate * 5 - n_in(0, 5),
            "lost1_5": rate * 4 - n_in(1, 5),
            "lost_tail4": rate * 4 - sum(1 for t in pres if t >= tail_a),
            "long1_5": sum(1 for t, d in iv5 if t >= 1 and d > 1.5 * per_ms),
            "long_tail4": sum(1 for _, d in ivs(tail_a, end + 1) if d > 1.5 * per_ms),
            "max5": top[0] if top else (0.0, 0.0), "top3": top}


HEAD = "%-26s %6s %6s %6s %6s %7s %7s %6s %6s %13s  %s"
ROW = "%-26s %6.1f %6.1f %6.0f %6.0f %7.0f %7.0f %6d %6d %6.1f@%5.3f  %s"


def main(argv):
    out = None
    if argv[:1] == ["--json"]:
        out, argv = argv[1], argv[2:]
    if not argv:
        print(__doc__.strip().split("\n\n")[1])
        return 64
    print(HEAD % ("leg", "live_s", "rate", "lost1", "lost5", "lost1-5", "tail4", "lng1-5",
                  "lngT4", "max5(ms@s)", "top3"))
    rows, rc = [], 0
    for p in argv:
        try:
            d = leg(p)
        except (ValueError, SystemExit) as e:
            print("%-26s ERROR %s" % (Path(p).name, e))
            rc = 1
            continue
        rows.append(d)
        print(ROW % (d["leg"][:26], d["live_s"], d["rate"], d["lost1"], d["lost5"],
                     d["lost1_5"], d["lost_tail4"], d["long1_5"], d["long_tail4"],
                     d["max5"][1], d["max5"][0],
                     " ".join("%.1f@%.3f" % (m, t) for t, m in d["top3"])))
    if out:
        with open(out, "w") as f:
            json.dump(rows, f)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
