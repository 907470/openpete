#!/usr/bin/env python3
"""Render a .opp trace as a flamegraph SVG image (self-contained, opens in any
browser). Aggregates the nested CPU scope spans (and GPU zones) across all
presents into a call tree by time-containment, then draws proportional-width
frames. Split by frame_kind at the root so canonical vs intermediate are
comparable side by side.

Usage:
  profile-flamegraph.py TRACE.opp -o OUT.svg [--skip-ticks N] [--which cpu|gpu]
                        [--title STR]

For a PNG, convert the SVG (e.g. `rsvg-convert OUT.svg -o OUT.png`).
"""
import argparse
import colorsys
import html
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _opp_reader import (read_opp, partition_transients, OppTraceError,  # noqa: E402
                         KIND_NAME)


def build_tree_for_present(spans):
    """spans: list of (depth, name, t0, dur, aux, tid) FROM ONE THREAD.
    Reconstruct nesting by time containment (ignore the unreliable stored
    depth). Return list of root nodes, each {name, t0, dur, children:[...]}.
    Callers split by tid first: two threads' spans overlap in wall time and
    containment would read the overlap as nesting."""
    ev = sorted(spans, key=lambda s: (s[2], -(s[3])))  # by t0, longer first
    roots = []
    stack = []  # open ancestors
    for (_d, name, t0, dur, _a, _tid) in ev:
        node = {"name": name, "t0": t0, "dur": dur, "children": []}
        while stack and stack[-1]["t0"] + stack[-1]["dur"] <= t0:
            stack.pop()
        if stack and stack[-1]["t0"] + stack[-1]["dur"] >= t0 + dur:
            stack[-1]["children"].append(node)
        else:
            roots.append(node)
        stack.append(node)
    return roots


def fold_into(acc, node, prefix):
    """Accumulate self-time (dur minus children) per stack path into acc dict."""
    path = prefix + (node["name"],)
    child_dur = sum(c["dur"] for c in node["children"])
    self_dur = max(0, node["dur"] - child_dur)
    acc[path] = acc.get(path, 0) + self_dur
    for c in node["children"]:
        fold_into(acc, c, path)


def gpu_tree_for_frame(gpuz):
    """gpuz: list of (depth, name, t0_rel, dur, tid) for ONE rendered frame.
    Use stored depth for nesting."""
    roots, stack = [], []
    for (depth, name, t0, dur, _tid) in sorted(gpuz, key=lambda z: (z[2], -(z[3]))):
        node = {"name": name, "t0": t0, "dur": dur, "children": []}
        while len(stack) > depth:
            stack.pop()
        if stack:
            stack[-1]["children"].append(node)
        else:
            roots.append(node)
        stack.append(node)
    return roots


# ---- folded → nested tree for rendering ------------------------------------
def folded_to_tree(acc):
    root = {"name": "all", "value": 0, "children": {}}
    for path, val in acc.items():
        root["value"] += val
        node = root
        for part in path:
            node = node["children"].setdefault(
                part, {"name": part, "value": 0, "children": {}})
            node["value"] += val
    return root


def color_for(name):
    h = (hash(name) % 1000) / 1000.0
    # warm flamegraph palette: hue 0..0.13, high sat/val
    hue = 0.02 + 0.11 * h
    r, g, b = colorsys.hsv_to_rgb(hue, 0.65 + 0.2 * ((hash(name) >> 3) % 2),
                                  0.92 - 0.15 * ((hash(name) >> 5) % 2))
    return "#%02x%02x%02x" % (int(r * 255), int(g * 255), int(b * 255))


ROW_H = 18
PAD = 2


def layout(node, depth, x, total_w, total_val, rects, max_depth):
    """Emit rects for node and its children. x/width in px."""
    w = total_w * (node["value"] / total_val) if total_val else 0
    if w >= 0.5:
        rects.append((x, depth, w, node["name"], node["value"]))
        max_depth[0] = max(max_depth[0], depth)
    cx = x
    for _, child in sorted(node["children"].items(),
                           key=lambda kv: -kv[1]["value"]):
        cw = total_w * (child["value"] / total_val) if total_val else 0
        layout(child, depth + 1, cx, total_w, total_val, rects, max_depth)
        cx += cw
    return rects


def render_svg(root, title, subtitle):
    width = 1200
    total_val = root["value"] or 1
    rects = []
    max_depth = [0]
    layout(root, 0, 0.0, float(width), float(total_val), rects, max_depth)
    top = 54
    height = top + (max_depth[0] + 1) * ROW_H + 12

    def us(v):
        return "%.1f ms" % (v / 1000.0) if v >= 1000 else "%d us" % v

    parts = []
    parts.append(
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
        'font-family="Verdana,Helvetica,sans-serif" font-size="12">' % (width, height))
    parts.append('<rect width="100%" height="100%" fill="#1e1e1e"/>')
    parts.append('<text x="%d" y="24" fill="#eee" font-size="17" '
                 'text-anchor="middle">%s</text>' % (width // 2, html.escape(title)))
    parts.append('<text x="%d" y="42" fill="#aaa" font-size="11" '
                 'text-anchor="middle">%s</text>' % (width // 2, html.escape(subtitle)))
    for (x, depth, w, name, val) in rects:
        y = top + (max_depth[0] - depth) * ROW_H  # root at bottom
        fill = "#444" if name == "all" else color_for(name)
        parts.append('<rect x="%.2f" y="%d" width="%.2f" height="%d" fill="%s" '
                     'stroke="#1e1e1e" stroke-width="0.5"><title>%s (%s, %.1f%%)</title></rect>'
                     % (x + PAD / 2, y, max(0.5, w - PAD), ROW_H - 1, fill,
                        html.escape(name), us(val), 100.0 * val / total_val))
        if w > 42:
            label = name
            maxc = int((w - 6) / 6.2)
            if len(label) > maxc:
                label = label[:max(1, maxc - 1)] + "…"
            parts.append('<text x="%.2f" y="%d" fill="#111">%s</text>'
                         % (x + 3, y + 13, html.escape(label)))
    parts.append("</svg>")
    return "\n".join(parts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--skip-ticks", type=int, default=0)
    ap.add_argument("--transient-ms", type=int, default=40,
                    help="exclude presents whose CPU busy exceeds N ms as load "
                         "transients; 0 disables")
    ap.add_argument("--which", choices=["cpu", "gpu", "func"], default="cpu")
    ap.add_argument("--title", default=None)
    args = ap.parse_args()

    try:
        tr = read_opp(args.trace)
    except OppTraceError as e:
        sys.stderr.write("profile-flamegraph: %s\n" % e)
        return 2
    presents, funcs = tr.presents, tr.funcs
    presents = [p for p in presents if p.canon_tick >= args.skip_ticks]
    presents, transients = partition_transients(presents, args.transient_ms * 1000)
    if transients:
        print("excluded %d load-transient present(s) >%dms" %
              (len(transients), args.transient_ms), file=sys.stderr)

    acc = {}
    if args.which == "func":
        # Flat self-time profile of the recompiled game logic (FUNC_TRACE). No call
        # stacks are recorded, so this is a one-level flame: width = self µs.
        for fn in funcs:
            if fn["self_us"] > 0:
                acc[("sim_tick (game logic)", fn["name"])] = fn["self_us"]
    elif args.which == "gpu":
        # GPU zones group per rendered frame (RHI frame serial), not per
        # present: under eager production a present index is not a frame.
        for zones in tr.gpu_frames.values():
            for node in gpu_tree_for_frame(zones):
                fold_into(acc, node, ("frame",))
    else:
        # One tree per (present, producer thread): containment across threads
        # would read concurrent spans as nesting.
        for p in presents:
            kind = KIND_NAME.get(p.kind, "?")
            by_tid = {}
            for sp in p.spans:
                by_tid.setdefault(sp[5], []).append(sp)
            for tid, sub in sorted(by_tid.items()):
                for node in build_tree_for_present(sub):
                    fold_into(acc, node, (kind, "tid %d" % tid))
    root = folded_to_tree(acc)

    total_ms = root["value"] / 1000.0
    title = args.title or ("%s flamegraph — %s" %
                           (args.which.upper(), os.path.basename(args.trace)))
    if args.which == "func":
        sub = ("%.0f ms total game-logic self-time across the run; width = per-"
               "function self-time (recompiled EXE, -finstrument-functions)" % total_ms)
    elif args.which == "gpu":
        sub = ("%d rendered frame(s), %.0f ms total GPU time (self-time by zone, "
               "grouped by RHI frame serial)" % (len(tr.gpu_frames), total_ms))
    else:
        sub = ("%d presents, %.0f ms total CPU time (self-time by scope; root "
               "split canonical/intermediate, then producer thread)"
               % (len(presents), total_ms))
    svg = render_svg(root, title, sub)
    with open(args.out, "w") as f:
        f.write(svg)
    print("wrote %s (%.0f ms total)" % (args.out, total_ms), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
