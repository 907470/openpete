#!/usr/bin/env python3
"""Summarize a .opp performance trace into a compact plain-text report
(and optional JSON). CPU scopes and GPU zones are split by frame_kind
(canonical vs intermediate) with p50/p99/max and per-run totals.

Usage:
  profile-summary.py TRACE.opp [--skip-ticks N] [--label STR] [--json OUT.json]

Notes:
  * Spans bin to markers (_opp_reader.py): SCENE records under eager
    production, PRESENT records on the inline path. "present_busy" is the
    wall-time union of all CPU spans binned to a marker (the real CPU cost;
    parent/child scope overlap is de-duplicated); "presents_total" and the
    per-kind "presents" count markers.
  * "fps" and [present] are the cadence of PRESENT records, each written at a
    present to the display (includes pacing/vsync waits). [scene] is the
    cadence of SCENE records, each written when a scene is published to the
    scene ring. A format 2 trace has no SCENE: under eager its PRESENT records
    were written at scene publication, and [present] says so.
  * rex.* scopes are children of rex_total; compose/* GPU zones are children of
    compose. Per-scope share% is of the kind's total present_busy and will sum
    to >100% because of that nesting - it's a breakdown, not a partition.
  * [scheduler] reads the eager planner's per-span counters (eager.*, pt.*)
    off the canonical scene markers: cost mean and deviation, the tail bound,
    budget, level and afford distributions, the afford recomputed from the
    recorded budget and costs (a check that the record matches the rule), and
    the planned rate (scenes planned per span times spans per second), the
    cost floor's rate (1 / max(tail, present-thread render + present step)),
    the prediction (the lower of the two) and the measured present and scene
    rates. Absent without eager.
  * [sim.split] breaks the sim tick down: each fiber hook's inclusive cost
    (hook.<feature>), the guest renderer it calls (<feature>.srend), and the
    remainder - openpete's own extraction on the game fiber. The bottom
    "unattributed" row is sim_tick minus every hook, i.e. guest game logic plus
    whatever fiber-side work is still uninstrumented. A scope "<a>.defer.<b>"
    (hook <a> running work held by hook <b>) is moved from <a>'s total to <b>'s
    and shown as <b>'s part "deferred_from_<a>".
"""
import argparse
import collections
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _opp_reader import (read_opp, present_busy_us, partition_transients,  # noqa: E402
                         OppTraceError, KIND_CANONICAL, KIND_INTERMEDIATE)


def pct(sorted_vals, q):
    if not sorted_vals:
        return 0
    if len(sorted_vals) == 1:
        return sorted_vals[0]
    i = int((len(sorted_vals) - 1) * q)
    return sorted_vals[i]


def interval_union_us(spans):
    """Deprecated shim: use _opp_reader.present_busy_us. Kept for callers passing
    a raw span list."""
    class _P:
        pass
    p = _P()
    p.spans = spans
    return present_busy_us(p)


def cadence(records):
    """Interval statistics of marker records in time order: count, p50, p99
    and max of the positive gaps between consecutive t_now_us, the time span
    they cover, and the first record's time."""
    t = sorted(r.t_now_us for r in records)
    iv = sorted(t[i] - t[i - 1] for i in range(1, len(t)) if t[i] > t[i - 1])
    return {
        "records": len(t),
        "first_t_us": t[0] if t else 0,
        "interval_p50_us": pct(iv, 0.50),
        "interval_p99_us": pct(iv, 0.99),
        "interval_max_us": iv[-1] if iv else 0,
        "intervals": len(iv),
        "span_us": sum(iv),
    }


def ladder_afford(budget, ema, dev, cap=128):
    """The eager planner's afford (op_eager_ladder_afford): the largest N with
    N * ema + 2 * sqrt(N) * dev <= budget, at least 1, at most cap."""
    if ema <= 0:
        return cap
    n = 0
    while n < cap:
        m = n + 1
        if m * ema + 2.0 * math.sqrt(m) * dev > budget:
            break
        n += 1
    return max(1, n)


def plan_afford(c):
    """The afford eager_afford computes from one span's recorded counters:
    priced on the sum past warmup, on the per-scene tail before it, one per
    slot before any cost exists; at least 1."""
    if c["eager.warm"] and c["eager.cost_ema_ns"] > 0:
        a = ladder_afford(c["eager.budget_ns"], c["eager.cost_ema_ns"], c["eager.cost_dev_ns"])
    elif c["eager.tail_ns"] > 0:
        a = c["eager.budget_ns"] // c["eager.tail_ns"]
    else:
        a = c["eager.slots"]
    return max(1, a)


def scheduler(markers, present_cadence, scene_cadence):
    """The eager scheduler's per-span counters (eager_prof_plan), read off the
    canonical scene markers that carry them: the cost estimate, the tail and
    the budget, the level and afford distributions, the afford recomputed
    from the recorded budget and costs against the recorded one, and the
    predicted rate (the lower of the planned rate, scenes planned per span
    times spans per second, and the cost floor's rate) against the measured
    present and scene rates. None when no marker
    carries the counters (eager production did not run)."""
    plans = [p for p in markers if "eager.afford" in p.counts]
    if not plans:
        return None
    keys = ("eager.cost_ema_ns", "eager.cost_dev_ns", "eager.tail_ns", "eager.budget_ns",
            "eager.afford", "eager.slots", "eager.planned", "eager.level", "eager.std_level",
            "eager.period_ns", "eager.grid_period_ns", "eager.free", "eager.lattice",
            "eager.warm", "eager.vbl_ema_ns", "eager.tick_late_ns", "eager.lag_ns",
            "pt.render_ema_ns", "pt.present_ema_ns")
    rows = [{k: p.counts.get(k, 0) for k in keys} for p in plans]

    def q(k, f=0.5, scale=1e-3):
        return round(pct(sorted(r[k] for r in rows), f) * scale, 1)

    def dist(k):
        h = collections.Counter(r[k] for r in rows)
        return {str(v): round(100.0 * n / len(rows), 1) for v, n in sorted(h.items())}

    t = sorted(p.t_now_us for p in plans)
    span_s = (t[-1] - t[0]) / 1e6 if len(t) > 1 else 0.0
    spans_per_s = (len(t) - 1) / span_s if span_s > 0 else 0.0
    planned = sum(r["eager.planned"] for r in rows) / len(rows)
    warm = [r for r in rows if r["eager.warm"]]
    match = sum(1 for r in rows if plan_afford(r) == r["eager.afford"])
    pt_cost = sorted(r["pt.render_ema_ns"] + r["pt.present_ema_ns"] for r in rows)
    floor_ns = max(pct(sorted(r["eager.tail_ns"] for r in rows), 0.5), pct(pt_cost, 0.5))
    return {
        "spans": len(rows),
        "warm_spans": len(warm),
        "free_pct": round(100.0 * sum(r["eager.free"] for r in rows) / len(rows), 1),
        "lattice_pct": round(100.0 * sum(r["eager.lattice"] for r in rows) / len(rows), 1),
        "cost_ema_p50_us": q("eager.cost_ema_ns"), "cost_ema_p99_us": q("eager.cost_ema_ns", 0.99),
        "cost_dev_p50_us": q("eager.cost_dev_ns"), "cost_dev_p99_us": q("eager.cost_dev_ns", 0.99),
        "tail_p50_us": q("eager.tail_ns"), "tail_p99_us": q("eager.tail_ns", 0.99),
        "budget_p50_us": q("eager.budget_ns"), "budget_p01_us": q("eager.budget_ns", 0.01),
        "period_p50_us": q("eager.period_ns"), "grid_period_p50_us": q("eager.grid_period_ns"),
        "lag_p50_us": q("eager.lag_ns"), "vbl_ema_p50_us": q("eager.vbl_ema_ns"),
        "pt_render_p50_us": q("pt.render_ema_ns"), "pt_present_p50_us": q("pt.present_ema_ns"),
        "slots_mean": round(sum(r["eager.slots"] for r in rows) / len(rows), 2),
        "planned_mean": round(planned, 2),
        "level_dist": dist("eager.level"),
        "std_level_dist": dist("eager.std_level"),
        "afford_dist": dist("eager.afford"),
        "afford_recomputed_match_pct": round(100.0 * match / len(rows), 1),
        "spans_per_s": round(spans_per_s, 2),
        "predicted_scenes_per_s": round(planned * spans_per_s, 1),
        "cost_floor_per_s": round(1e9 / floor_ns, 1) if floor_ns > 0 else 0.0,
        # A span plans at least its canonical scene, so the plan alone cannot
        # show a present thread slower than one present per tick; the
        # prediction is the lower of the plan's rate and the cost floor's.
        "predicted_presents_per_s": round(min(planned * spans_per_s, 1e9 / floor_ns), 1)
        if floor_ns > 0 else round(planned * spans_per_s, 1),
        "measured_presents_per_s": round(present_cadence["intervals"] * 1e6 / present_cadence["span_us"], 1)
        if present_cadence.get("span_us") else 0.0,
        "measured_scenes_per_s": round(scene_cadence["intervals"] * 1e6 / scene_cadence["span_us"], 1)
        if scene_cadence.get("span_us") else 0.0,
    }


def present_source(tr):
    """Name the event a trace's PRESENT records stamp.

    "present-thread": the trace has SCENE records and no PRESENT shares a
    thread with them (eager production throughout). "inline": no SCENE
    (every present on the sim worker). "mixed": both kinds of thread (eager
    toggled during the run). "format-2": the format carries no SCENE, and
    under eager its PRESENT records were written at scene publication."""
    if tr.version < 3:
        return "format-2"
    if not tr.scenes:
        return "inline"
    if tr.present_tids.isdisjoint(tr.scene_tids):
        return "present-thread"
    return "mixed"


def summarize(path, skip_ticks=0, label=None, transient_ms=40):
    tr = read_opp(path)
    funcs = tr.funcs
    presents = [p for p in tr.markers if p.canon_tick >= skip_ticks]
    presents, transients = partition_transients(presents, transient_ms * 1000)
    transient_ids = set(id(p) for p in transients)

    kinds = {KIND_CANONICAL: "canonical", KIND_INTERMEDIATE: "intermediate"}
    out = {
        "trace": os.path.basename(path),
        "format": tr.version,
        "label": label or "",
        "presents_total": len(presents),
        "transients_excluded": len(transients),
        "transients_total_ms": round(sum(present_busy_us(p) for p in transients) / 1000.0, 1),
        "transient_ms_threshold": transient_ms,
        "kinds": {},
        "threads": tr.threads,
        "cpu_scopes": [],
        "gpu_zones": [],
        "gpu_frames": {},
        "census": [],
        "fps": {},
        "funcs": [],
    }

    # ---- per-function aggregate (present only in FUNC_TRACE builds) --------
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

    # ---- per-marker busy + present cadence --------------------------------
    busy_by_kind = {k: [] for k in kinds}
    for p in presents:
        busy_by_kind.setdefault(p.kind, []).append(present_busy_us(p))

    for k, kname in kinds.items():
        b = sorted(busy_by_kind[k])
        out["kinds"][kname] = {
            "presents": len(b),
            "busy_p50_us": pct(b, 0.50),
            "busy_p99_us": pct(b, 0.99),
            "busy_max_us": max(b) if b else 0,
        }

    # fps from the cadence of PRESENT records (all kinds interleaved, time
    # order). A PRESENT that is a marker and a load transient is dropped, as
    # its marker is; a present-thread PRESENT carries no spans and never is.
    shown = [p for p in tr.presents
             if p.canon_tick >= skip_ticks and id(p) not in transient_ids]
    all_t = sorted(p.t_now_us for p in shown)
    out["present"] = dict(cadence(shown), source=present_source(tr),
                          threads=sorted(tr.present_tids),
                          present_thread_records=sum(
                              1 for p in shown
                              if tr.scenes and p.tid not in tr.scene_tids))
    out["scene"] = dict(cadence([p for p in tr.scenes if p.canon_tick >= skip_ticks]),
                        threads=sorted(tr.scene_tids))
    out["scheduler"] = scheduler([p for p in presents if p.kind == KIND_CANONICAL],
                                 out["present"], out["scene"])
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
            for (name, _tid), us in d.items():
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
        for (_depth, name, _t0, dur, _aux, tid) in p.spans:
            key = (name, tid)
            d[key] = d.get(key, 0) + dur
        return d

    out["cpu_scopes"] = agg(cpu_pick)

    # ---- per-thread scope counts -------------------------------------------
    # One row per (scope, thread): a scope that moves between the sim worker and
    # the present thread when a default flips shows here as two rows.
    per_thread = {}
    for p in presents:
        for (_d, name, _t0, dur, _a, tid) in p.spans:
            e = per_thread.setdefault((tid, name), [0, 0])
            e[0] += 1
            e[1] += dur
    out["thread_scopes"] = [
        {"tid": tid, "scope": name, "n": n, "sum_ms": round(us / 1000.0, 1)}
        for (tid, name), (n, us) in sorted(per_thread.items(),
                                           key=lambda kv: (kv[0][0], -kv[1][1]))]

    # ---- GPU zones, grouped by RHI frame serial ----------------------------
    # A GPU zone belongs to the frame that submitted it, not to a present: under
    # eager production scenes are published and superseded without rendering, so
    # a present index is not a frame. Zone stats are therefore per rendered
    # frame, independent of the present binning above.
    frames = tr.gpu_frames
    zrows, ztot = {}, {}
    zones_per_frame = []
    for serial, zones in frames.items():
        zones_per_frame.append(len(zones))
        per = {}
        for (_d, name, _t0r, dur, _tid) in zones:
            per[name] = per.get(name, 0) + dur
        for name, us in per.items():
            zrows.setdefault(name, []).append(us)
            ztot[name] = ztot.get(name, 0) + us
    zrows_out = []
    frame_total_us = sum(ztot.values())
    for name, vals in zrows.items():
        sv = sorted(vals)
        zrows_out.append({
            "scope": name, "kind": "frame",
            "p50_us": pct(sv, 0.50), "p99_us": pct(sv, 0.99), "max_us": max(sv),
            "sum_ms": round(ztot[name] / 1000.0, 1), "n": len(vals),
            "share_pct": round(100.0 * ztot[name] / frame_total_us, 1)
                         if frame_total_us else 0.0,
        })
    zrows_out.sort(key=lambda r: -r["sum_ms"])
    out["gpu_zones"] = zrows_out
    zpf = sorted(zones_per_frame)
    out["gpu_frames"] = {
        "frames": len(frames),
        "zones_total": sum(zones_per_frame),
        "zones_per_frame_p50": pct(zpf, 0.50),
        "zones_per_frame_max": max(zpf) if zpf else 0,
        "zones_per_frame_min": min(zpf) if zpf else 0,
    }

    # ---- sim-tick split (fiber hooks) ---------------------------------------
    # Canonical markers only: the hooks run on the game fiber, which only the
    # canonical scene or present yields to. Sums are per-canonical-tick averages so the
    # rows compare directly against sim_tick's p50.
    canon = [p for p in presents if p.kind == KIND_CANONICAL]
    tot = {}
    for p in canon:
        for (_d, name, _t0, dur, _a, _tid) in p.spans:
            tot[name] = tot.get(name, 0) + dur
    nc = len(canon) or 1

    def per_tick(name):
        return tot.get(name, 0) / nc

    hooks = sorted(n for n in tot if n.startswith("hook."))
    # "<a>.defer.<b>" times work hook <a> runs on behalf of hook <b> (a held
    # build flushed at the end of another hook): moved from <a>'s total to <b>'s.
    moved = {}
    for n in tot:
        parts = n.split(".")
        if len(parts) == 3 and parts[1] == "defer":
            moved[parts[0]] = moved.get(parts[0], 0.0) - per_tick(n)
            moved[parts[2]] = moved.get(parts[2], 0.0) + per_tick(n)
    rows = []
    accounted = 0.0
    for h in hooks:
        feat = h[len("hook."):]
        prefix = feat[:-len("_native")] if feat.endswith("_native") else feat
        subs = {n: tot[n] / nc for n in tot
                if n.startswith(prefix + ".") and not n.startswith(prefix + ".defer.")}
        for n in tot:
            p = n.split(".")
            if len(p) == 3 and p[1] == "defer" and p[2] == prefix:
                subs[prefix + ".deferred_from_" + p[0]] = per_tick(n)
        total = per_tick(h) + moved.pop(prefix, 0.0)
        accounted += total
        rows.append({
            "hook": feat,
            "total_us": round(total, 1),
            "srend_us": round(subs.get(prefix + ".srend", 0.0), 1),
            # Everything the hook spent outside the guest renderer call: capture,
            # native walk, scene feed, and any unscoped remainder.
            "openpete_us": round(total - subs.get(prefix + ".srend", 0.0), 1),
            "parts": {k.split(".", 1)[1]: round(v, 1) for k, v in sorted(subs.items())},
        })
    # Fiber-side openpete work that is not a feature hook (PSYQ thunk scopes).
    for n in sorted(tot):
        if n.startswith("psyq."):
            accounted += per_tick(n)
            rows.append({"hook": n, "total_us": round(per_tick(n), 1),
                         "srend_us": 0.0, "openpete_us": round(per_tick(n), 1),
                         "parts": {}})
    if any(abs(v) >= 0.05 for v in moved.values()):
        sys.stderr.write("profile-summary: defer scope with no matching hook: %r\n" % moved)
    rows.sort(key=lambda r: -r["total_us"])
    sim = per_tick("sim_tick")
    out["sim_split"] = {
        "sim_tick_us_per_tick": round(sim, 1),
        "rows": rows,
        "unattributed_us": round(sim - accounted, 1),
        "canonical_presents": nc,
    }

    # ---- census -----------------------------------------------------------
    cen = {}
    for p in presents:
        for name, val in p.counts.items():
            if name.startswith(("eager.", "pt.")):
                continue   # the scheduler's counters: [scheduler]
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
    hdr = "# openpete-profile v%d  trace=%s" % (s.get("format", 3), s["trace"])
    if s["label"]:
        hdr += "  label=%s" % s["label"]
    L.append(hdr)
    L.append("# markers=%d canonical=%d intermediate=%d"
             % (s["presents_total"], ncanon, ninter))
    if s.get("transients_excluded"):
        L.append("# excluded %d load-transient present(s) >%dms (%.0f ms total - level "
                 "loads / CD stalls)" % (s["transients_excluded"],
                 s["transient_ms_threshold"], s["transients_total_ms"]))
    ck = s["kinds"].get("canonical", {})
    ik = s["kinds"].get("intermediate", {})
    L.append("# present_busy_us p50/p99/max: canonical %d/%d/%d  intermediate %d/%d/%d"
             % (ck.get("busy_p50_us", 0), ck.get("busy_p99_us", 0), ck.get("busy_max_us", 0),
                ik.get("busy_p50_us", 0), ik.get("busy_p99_us", 0), ik.get("busy_max_us", 0)))
    f = s.get("fps", {})
    L.append("# fps avg %s  1%%low %s  frame_p99_us %s"
             % (f.get("avg", "?"), f.get("1pct_low", "?"), f.get("frame_p99_us", "?")))
    pr, sc = s.get("present", {}), s.get("scene", {})
    note = {"present-thread": "written on the present thread at each present",
            "inline": "written on the sim worker at each inline present",
            "mixed": "on the present thread and the sim worker (eager inactive for part of the run)",
            "format-2": "format 2 trace: under eager these were written at scene "
                        "publication, so this is publication cadence"}
    L.append("[present] records=%d (%d on the present thread)  interval_us "
             "p50/p99/max %d/%d/%d  - %s"
             % (pr.get("records", 0), pr.get("present_thread_records", 0),
                pr.get("interval_p50_us", 0), pr.get("interval_p99_us", 0),
                pr.get("interval_max_us", 0), note.get(pr.get("source"), "?")))
    if sc.get("records"):
        L.append("[scene]   records=%d  interval_us p50/p99/max %d/%d/%d  - "
                 "scene publication on the sim worker"
                 % (sc["records"], sc["interval_p50_us"], sc["interval_p99_us"],
                    sc["interval_max_us"]))
    sch = s.get("scheduler")
    if sch:
        def dist(d):
            return " ".join("%s:%.0f%%" % kv for kv in d.items())
        L.append("[scheduler] spans=%d (warm %d, free %.0f%%, lattice %.0f%%)  cost_ema p50/p99 %.1f/%.1f us  "
                 "cost_dev %.1f/%.1f us  tail %.1f/%.1f us  budget p50 %.1f p1 %.1f us  pt render/present "
                 "p50 %.1f/%.1f us  period p50 %.1f us (grid %.1f)  slots %.2f planned %.2f per span  "
                 "level %s  std %s  afford %s  afford recomputed %.1f%%  planned %.1f/s (%.2f spans/s), "
                 "cost floor %.1f/s, predicted %.1f/s, measured presents %.1f/s scenes %.1f/s"
                 % (sch["spans"], sch["warm_spans"], sch["free_pct"], sch["lattice_pct"],
                    sch["cost_ema_p50_us"], sch["cost_ema_p99_us"], sch["cost_dev_p50_us"],
                    sch["cost_dev_p99_us"], sch["tail_p50_us"], sch["tail_p99_us"],
                    sch["budget_p50_us"], sch["budget_p01_us"], sch["pt_render_p50_us"],
                    sch["pt_present_p50_us"], sch["period_p50_us"], sch["grid_period_p50_us"],
                    sch["slots_mean"], sch["planned_mean"], dist(sch["level_dist"]),
                    dist(sch["std_level_dist"]), dist(sch["afford_dist"]),
                    sch["afford_recomputed_match_pct"], sch["predicted_scenes_per_s"],
                    sch["spans_per_s"], sch["cost_floor_per_s"], sch["predicted_presents_per_s"],
                    sch["measured_presents_per_s"],
                    sch["measured_scenes_per_s"]))
    L.append("# producer threads: %s" % ", ".join(str(t) for t in s.get("threads", [])))
    gf = s.get("gpu_frames", {})
    if gf.get("frames"):
        L.append("# gpu frames=%d  zones=%d  zones/frame min/p50/max %d/%d/%d"
                 % (gf["frames"], gf["zones_total"], gf["zones_per_frame_min"],
                    gf["zones_per_frame_p50"], gf["zones_per_frame_max"]))
    else:
        L.append("# gpu frames=0 - no GPU zone records in this trace "
                 "(OPENPETE_GPU_TIMERS unset, or a headless run, which has no "
                 "render thread and no timer pool)")

    def table(title, rows):
        if not rows:
            return
        L.append("[%s]%s" % (title, " " * max(1, 20 - len(title))) + "kind          p50_us  p99_us  max_us  sum_ms  share%")
        for r in rows:
            L.append("%-22s%-12s%8d%8d%8d%9.1f%8.1f"
                     % (r["scope"][:22], r["kind"], r["p50_us"], r["p99_us"],
                        r["max_us"], r["sum_ms"], r["share_pct"]))

    ss = s.get("sim_split")
    if ss and ss["rows"]:
        L.append("[sim.split]           per canonical tick, us   "
                 "(hook = inclusive; srend = the guest renderer it calls)")
        L.append("%-22s%10s%10s%10s  %s" % ("scope", "total", "srend", "openpete", "parts"))
        for r in ss["rows"]:
            parts = " ".join("%s=%.0f" % (k, v) for k, v in r["parts"].items()
                             if k != "srend" and v >= 1.0)
            L.append("%-22s%10.1f%10.1f%10.1f  %s"
                     % (r["hook"][:22], r["total_us"], r["srend_us"],
                        r["openpete_us"], parts))
        L.append("%-22s%10.1f%10s%10s  %s"
                 % ("unattributed", ss["unattributed_us"], "", "",
                    "guest game logic + uninstrumented fiber work"))
        L.append("%-22s%10.1f" % ("= sim_tick", ss["sim_tick_us_per_tick"]))

    table("cpu.scope", s["cpu_scopes"])
    if s["gpu_zones"]:
        table("gpu.zone", s["gpu_zones"])
    else:
        L.append("[gpu.zone]            none recorded")
    ts = s.get("thread_scopes") or []
    if ts:
        L.append("[thread.scope]        tid          n        sum_ms")
        for r in ts:
            L.append("%-22s%-10d%-10d%.1f" % (r["scope"][:22], r["tid"], r["n"],
                                              r["sum_ms"]))
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
                    help="drop markers and PRESENT records with canon_tick < N "
                         "(warm-up exclusion)")
    ap.add_argument("--transient-ms", type=int, default=40,
                    help="exclude markers whose CPU busy exceeds N ms as load "
                         "transients (level loads / CD stalls); 0 disables")
    ap.add_argument("--label", default=None)
    ap.add_argument("--json", default=None, help="also write JSON summary here")
    args = ap.parse_args()

    try:
        s = summarize(args.trace, skip_ticks=args.skip_ticks, label=args.label,
                      transient_ms=args.transient_ms)
    except OppTraceError as e:
        sys.stderr.write("profile-summary: %s\n" % e)
        return 2
    print(render_text(s))
    if args.json:
        with open(args.json, "w") as f:
            json.dump(s, f, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
