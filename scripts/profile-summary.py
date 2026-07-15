#!/usr/bin/env python3
"""Summarize a .opp performance trace into a compact, plain-text report
(and optional JSON). CPU scopes and GPU zones are split by frame_kind
(canonical vs intermediate) with p50/p99/max and per-run totals.

Usage:
  profile-summary.py TRACE.opp [--skip-ticks N] [--label STR] [--json OUT.json]

Notes:
  * "present_busy" is the wall-time union of all CPU spans in a present (the
    real CPU cost; parent/child scope overlap is de-duplicated). "fps" is the
    present cadence (includes pacing/vsync waits).
  * rex.* scopes are children of rex_total; compose/* GPU zones are children of
    compose. Per-scope share% is of the kind's total present_busy and will sum
    to >100% because of that nesting — it's a breakdown, not a partition.
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _opp_reader import read_opp, KIND_CANONICAL, KIND_INTERMEDIATE  # noqa: E402


def pct(sorted_vals, q):
    if not sorted_vals:
        return 0
    if len(sorted_vals) == 1:
        return sorted_vals[0]
    i = int((len(sorted_vals) - 1) * q)
    return sorted_vals[i]


def interval_union_us(spans):
    """Total wall-time covered by span [t0, t0+dur) intervals (overlap-safe)."""
    if not spans:
        return 0
    iv = sorted((t0, t0 + dur) for (_d, _n, t0, dur, _a) in spans)
    total = 0
    cur_s, cur_e = iv[0]
    for s, e in iv[1:]:
        if s > cur_e:
            total += cur_e - cur_s
            cur_s, cur_e = s, e
        elif e > cur_e:
            cur_e = e
    total += cur_e - cur_s
    return total


def summarize(path, skip_ticks=0, label=None):
    names, presents, funcs = read_opp(path)
    presents = [p for p in presents if p.canon_tick >= skip_ticks]

    kinds = {KIND_CANONICAL: "canonical", KIND_INTERMEDIATE: "intermediate"}
    out = {
        "trace": os.path.basename(path),
        "label": label or "",
        "presents_total": len(presents),
        "kinds": {},
        "cpu_scopes": [],
        "gpu_zones": [],
        "census": [],
        "fps": {},
        "funcs": [],
    }

    # ---- per-function aggregate (Tier B, present only in FUNC_TRACE builds) --
    ncanon_ticks = len(set(p.canon_tick for p in presents if p.kind == KIND_CANONICAL)) or 1
    for fn in sorted(funcs, key=lambda r: -r["self_us"])[:40]:
        out["funcs"].append({
            "addr": "0x%08x" % fn["addr"],
            "name": fn["name"],
            "calls": fn["calls"],
            "self_us": fn["self_us"],
            "total_us": fn["total_us"],
            "self_us_per_tick": round(fn["self_us"] / ncanon_ticks, 1),
        })

    # ---- per-present busy + present cadence -------------------------------
    all_t = [p.t_now_us for p in presents]
    busy_by_kind = {k: [] for k in kinds}
    for p in presents:
        busy_by_kind[p.kind].append(interval_union_us(p.spans))

    for k, kname in kinds.items():
        b = sorted(busy_by_kind[k])
        out["kinds"][kname] = {
            "presents": len(b),
            "busy_p50_us": pct(b, 0.50),
            "busy_p99_us": pct(b, 0.99),
            "busy_max_us": max(b) if b else 0,
        }

    # fps from present cadence (all kinds interleaved, time order)
    all_t.sort()
    intervals = [all_t[i] - all_t[i - 1] for i in range(1, len(all_t)) if all_t[i] > all_t[i - 1]]
    if intervals:
        total_s = sum(intervals) / 1e6
        avg_fps = len(intervals) / total_s if total_s > 0 else 0
        si = sorted(intervals, reverse=True)  # worst (largest) first
        p99_int = pct(sorted(intervals), 0.99)
        low1 = 1e6 / pct(si, 0.01) if pct(si, 0.01) else 0  # 1% slowest frames
        out["fps"] = {
            "avg": round(avg_fps, 1),
            "1pct_low": round(1e6 / (sum(si[:max(1, len(si)//100)]) / max(1, len(si)//100)), 1)
            if si else 0,
            "frame_p99_us": p99_int,
        }

    # ---- per-scope aggregation (CPU) --------------------------------------
    def agg(pick):
        # pick(present) -> dict name->summed_us for this present
        rows = {}  # (kind, name) -> list of per-present sums
        totals = {}  # (kind,name) -> total us
        for p in presents:
            d = pick(p)
            for name, us in d.items():
                rows.setdefault((p.kind, name), []).append(us)
                totals[(p.kind, name)] = totals.get((p.kind, name), 0) + us
        result = []
        for (k, name), vals in rows.items():
            sv = sorted(vals)
            result.append({
                "scope": name,
                "kind": kinds[k],
                "p50_us": pct(sv, 0.50),
                "p99_us": pct(sv, 0.99),
                "max_us": max(sv),
                "sum_ms": round(totals[(k, name)] / 1000.0, 1),
                "n": len(vals),
                "_kraw": k,
            })
        # share% of the kind's total busy
        busy_sum = {k: sum(busy_by_kind[k]) for k in kinds}
        for r in result:
            bs = busy_sum.get(r["_kraw"], 0)
            r["share_pct"] = round(100.0 * (r["sum_ms"] * 1000.0) / bs, 1) if bs else 0.0
            del r["_kraw"]
        result.sort(key=lambda r: (-r["sum_ms"]))
        return result

    def cpu_pick(p):
        d = {}
        for (_depth, name, _t0, dur, _aux) in p.spans:
            d[name] = d.get(name, 0) + dur
        return d

    def gpu_pick(p):
        d = {}
        for (_depth, name, _t0rel, dur) in p.gpuz:
            d[name] = d.get(name, 0) + dur
        return d

    out["cpu_scopes"] = agg(cpu_pick)
    out["gpu_zones"] = agg(gpu_pick)

    # ---- census -----------------------------------------------------------
    cen = {}
    for p in presents:
        for name, val in p.counts.items():
            cen.setdefault(name, []).append(val)
    for name, vals in cen.items():
        sv = sorted(vals)
        out["census"].append({
            "name": name,
            "avg": round(sum(vals) / len(vals), 1),
            "p99": pct(sv, 0.99),
            "max": max(sv),
        })

    return out


def render_text(s):
    L = []
    ncanon = s["kinds"].get("canonical", {}).get("presents", 0)
    ninter = s["kinds"].get("intermediate", {}).get("presents", 0)
    hdr = "# openpete-profile v1  trace=%s" % s["trace"]
    if s["label"]:
        hdr += "  label=%s" % s["label"]
    L.append(hdr)
    L.append("# presents=%d canonical=%d intermediate=%d"
             % (s["presents_total"], ncanon, ninter))
    ck = s["kinds"].get("canonical", {})
    ik = s["kinds"].get("intermediate", {})
    L.append("# present_busy_us p50/p99/max: canonical %d/%d/%d  intermediate %d/%d/%d"
             % (ck.get("busy_p50_us", 0), ck.get("busy_p99_us", 0), ck.get("busy_max_us", 0),
                ik.get("busy_p50_us", 0), ik.get("busy_p99_us", 0), ik.get("busy_max_us", 0)))
    f = s.get("fps", {})
    L.append("# fps avg %s  1%%low %s  frame_p99_us %s"
             % (f.get("avg", "?"), f.get("1pct_low", "?"), f.get("frame_p99_us", "?")))

    def table(title, rows):
        if not rows:
            return
        L.append("[%s]%s" % (title, " " * max(1, 20 - len(title))) + "kind          p50_us  p99_us  max_us  sum_ms  share%")
        for r in rows:
            L.append("%-22s%-12s%8d%8d%8d%9.1f%8.1f"
                     % (r["scope"][:22], r["kind"], r["p50_us"], r["p99_us"],
                        r["max_us"], r["sum_ms"], r["share_pct"]))

    table("cpu.scope", s["cpu_scopes"])
    table("gpu.zone", s["gpu_zones"])
    if s["census"]:
        L.append("[census]              avg          p99          max")
        for c in s["census"]:
            L.append("%-22s%-13s%-13s%s" % (c["name"][:22], c["avg"], c["p99"], c["max"]))
    if s["funcs"]:
        L.append("[func.top_self]       addr         calls       self_us  self_us/tick")
        for fn in s["funcs"]:
            L.append("%-22s%-13s%-12d%-10d%s"
                     % (fn["name"][:22], fn["addr"], fn["calls"],
                        fn["self_us"], fn["self_us_per_tick"]))
    return "\n".join(L)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--skip-ticks", type=int, default=0,
                    help="drop presents with canon_tick < N (warm-up exclusion)")
    ap.add_argument("--label", default=None)
    ap.add_argument("--json", default=None, help="also write JSON summary here")
    args = ap.parse_args()

    s = summarize(args.trace, skip_ticks=args.skip_ticks, label=args.label)
    print(render_text(s))
    if args.json:
        with open(args.json, "w") as f:
            json.dump(s, f, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
