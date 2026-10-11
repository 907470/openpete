#!/usr/bin/env python3
"""bench-ab.py - per-scope noise calibration and arm A/B over bench-corpus
rollups.

The metric is every rollup scope's p99_max_us (sim_tick among them), keyed
by (group, kind, scope); an arm's value per scope is the median over its
rollups. A value exceeds the threshold when it exceeds both units,
--threshold-pct (default 10) and --threshold-us (default 100), the test
bench-corpus.py's compare_baseline applies.

  calibrate --a R... --b R...
      Two equal halves of static rollups. Each scope's noise N is
      |median(A) - median(B)|, in us and as % of median(A). Resolved when no
      scope's N exceeds the threshold.
  ab --static R... --shared R...
      Equal counts per arm. Each scope's delta is median(shared) -
      median(static), signed (positive a regression), in us and as % of
      median(static). Pass when no scope's delta exceeds the threshold.

Every figure is printed, not only the scopes beyond the threshold. Refused
(exit 64): a rollup without the recorded identity (schema < 2), a
functional rollup, rollups whose platform tags, games, movie sets or
commits differ, a rollup whose recorded arm is missing or differs from its
role (every calibration rollup static; each A/B side its own arm), or
halves of unequal size. A scope present in only some rollups is reported
and left out of the verdict.

Usage:
  tests/harness/bench-ab.py calibrate --a r1.json r3.json r5.json \\
      --b r2.json r4.json r6.json [--json OUT] [--threshold-pct P] [--threshold-us U]
  tests/harness/bench-ab.py ab --static s1.json s2.json s3.json \\
      --shared h1.json h2.json h3.json [--json OUT] [...]

Exit: 0 resolved / pass, 1 A/B fail (some delta beyond the threshold),
2 calibration unresolved (some noise beyond the threshold), 64 refused.
"""
import argparse
import json
import statistics
import sys

EXIT_FAIL = 1
EXIT_UNRESOLVED = 2
EXIT_REFUSED = 64
ROLLUP_SCHEMA = 2


class Refused(Exception):
    """A rollup set that cannot be compared; the message names why."""


def load(path):
    """One rollup, refused when it predates the recorded identity."""
    with open(path) as f:
        r = json.load(f)
    if r.get("schema", 1) < ROLLUP_SCHEMA:
        raise Refused("%s: rollup predates the recorded game/arm/commit "
                      "(schema %s)" % (path, r.get("schema", 1)))
    if r.get("mode") != "timed":
        raise Refused("%s: mode %r, not a timed run" % (path, r.get("mode")))
    r["_path"] = path
    return r


def check_set(groups):
    """Refuse a rollup set whose identity is not one build at one commit on
    one platform, or whose arm differs from its role. `groups` maps a role
    name to (expected arm, [rollups])."""
    every = [r for _, rs in groups.values() for r in rs]
    first = every[0]
    for key, get in (("platform tag", lambda r: r["platform"]["tag"]),
                     ("game", lambda r: r["game"]),
                     ("movie set", lambda r: r["movie_set"]),
                     ("commit", lambda r: r["build"]["commit"])):
        for r in every[1:]:
            if get(r) != get(first):
                raise Refused("%s differs: %s has %r, %s has %r"
                              % (key, first["_path"], get(first), r["_path"], get(r)))
    for role, (arm, rs) in groups.items():
        for r in rs:
            got = r["build"].get("arm")
            if not got:
                raise Refused("%s: no recorded arm" % r["_path"])
            if got != arm:
                raise Refused("%s: arm %s in the %s role, which takes %s"
                              % (r["_path"], got, role, arm))
    sizes = {role: len(rs) for role, (_, rs) in groups.items()}
    if len(set(sizes.values())) != 1:
        raise Refused("unequal sizes: %s" % sizes)


def medians(rollups):
    """{scope key: median p99_max_us} over the scopes every rollup carries,
    and the keys some rollups lack."""
    per = [{(s["group"], s["kind"], s["scope"]): s["p99_max_us"]
            for s in r["scopes"]} for r in rollups]
    common = set.intersection(*(set(p) for p in per))
    partial = set.union(*(set(p) for p in per)) - common
    return {k: statistics.median(p[k] for p in per) for k in common}, partial


def exceeds(d_us, base, thr_pct, thr_us):
    """Whether |d_us| exceeds both threshold units; base 0 counts as
    exceeding the percent unit."""
    pct = 100.0 * abs(d_us) / base if base else float("inf")
    return abs(d_us) > thr_us and pct > thr_pct


def table(rows_in, signed, thr_pct, thr_us):
    """Rows of (key, a, b, d_us, d_pct, beyond) sorted by group then -|d|."""
    rows = []
    for k, (a, b) in rows_in.items():
        d = (b - a) if signed else abs(b - a)
        pct = 100.0 * d / a if a else (0.0 if d == 0 else float("inf"))
        rows.append((k, a, b, d, pct, exceeds(d, a, thr_pct, thr_us)))
    rows.sort(key=lambda r: (r[0][0], -abs(r[3])))
    return rows


def render(rows, label_a, label_b, what):
    out = ["%-12s %-12s %-22s %10s %10s %10s %9s  %s"
           % ("group", "kind", "scope", label_a, label_b, what + "_us",
              what + "_%", "beyond")]
    for (g, k, s), a, b, d, pct, beyond in rows:
        out.append("%-12s %-12s %-22s %10.0f %10.0f %+10.0f %+9.1f  %s"
                   % (g, k, s[:22], a, b, d, pct, "BEYOND" if beyond else ""))
    return "\n".join(out)


def run(args):
    if args.cmd == "calibrate":
        a = [load(p) for p in args.a]
        b = [load(p) for p in args.b]
        check_set({"calibration A": ("static", a), "calibration B": ("static", b)})
        ma, pa = medians(a)
        mb, pb = medians(b)
        keys = set(ma) & set(mb)
        rows = table({k: (ma[k], mb[k]) for k in keys}, False, args.threshold_pct, args.threshold_us)
        label, what = ("med_A", "med_B"), "noise"
        partial = pa | pb | (set(ma) ^ set(mb))
        k = len(a)
    else:
        st = [load(p) for p in args.static]
        sh = [load(p) for p in args.shared]
        check_set({"static": ("static", st), "shared": ("shared", sh)})
        ms, ps = medians(st)
        mh, ph = medians(sh)
        keys = set(ms) & set(mh)
        rows = table({k: (ms[k], mh[k]) for k in keys}, True, args.threshold_pct, args.threshold_us)
        label, what = ("static", "shared"), "delta"
        partial = ps | ph | (set(ms) ^ set(mh))
        k = len(st)
    beyond = [r for r in rows if r[5]]
    if args.cmd == "calibrate":
        verdict = "UNRESOLVED" if beyond else "RESOLVED"
        rc = EXIT_UNRESOLVED if beyond else 0
    else:
        verdict = "FAIL" if beyond else "PASS"
        rc = EXIT_FAIL if beyond else 0
    first = (args.a if args.cmd == "calibrate" else args.static)[0]
    r0 = load(first)
    print("# bench-ab %s  game=%s  platform=%s  commit=%s  k=%d  threshold=%.0f%% and %dus"
          % (args.cmd, r0["game"], r0["platform"]["tag"],
             (r0["build"]["commit"] or "?")[:12], k, args.threshold_pct,
             args.threshold_us))
    print(render(rows, label[0], label[1], what))
    if partial:
        print("# scopes absent from some rollups (not judged): %s"
              % ", ".join("/".join(p) for p in sorted(partial)))
    print("# verdict: %s (%d of %d scopes beyond)" % (verdict, len(beyond), len(rows)))
    if args.json:
        with open(args.json, "w") as f:
            json.dump({"cmd": args.cmd, "verdict": verdict, "k": k,
                       "game": r0["game"], "platform": r0["platform"]["tag"],
                       "commit": r0["build"]["commit"],
                       "threshold_pct": args.threshold_pct,
                       "threshold_us": args.threshold_us,
                       "rows": [{"group": g, "kind": kd, "scope": s,
                                 label[0]: a, label[1]: b, what + "_us": d,
                                 what + "_pct": (None if pct == float("inf") else round(pct, 2)),
                                 "beyond": bd}
                                for (g, kd, s), a, b, d, pct, bd in rows],
                       "partial": ["/".join(p) for p in sorted(partial)]},
                      f, indent=2)
    return rc


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("calibrate", "ab"):
        p = sub.add_parser(name)
        if name == "calibrate":
            p.add_argument("--a", nargs="+", required=True)
            p.add_argument("--b", nargs="+", required=True)
        else:
            p.add_argument("--static", nargs="+", required=True)
            p.add_argument("--shared", nargs="+", required=True)
        p.add_argument("--threshold-pct", type=float, default=10.0)
        p.add_argument("--threshold-us", type=int, default=100)
        p.add_argument("--json", default=None)
    args = ap.parse_args(argv)
    try:
        return run(args)
    except Refused as e:
        print("[bench-ab] REFUSED: %s" % e, file=sys.stderr)
        return EXIT_REFUSED


if __name__ == "__main__":
    sys.exit(main())
