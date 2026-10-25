#!/usr/bin/env python3
"""clipmap.py RUN.log NKEYS - locate a scene's shot in its recorded clip and QC it.

A scene of N disc keys lasts 2N presents in the video: keys 1..N-1 at two
presents a key (2N-3 presents from key 1's on-tick present), then the last
present cloned to 2N (the switch-tick hold). Key 0 is never shown. Reads the
capture's log lines ([fcpath] path key per tick, [alpha] per present,
[cut-census] shown camera position per present, [recq]/[recpub] the
encoder's present-to-frame map) and prints one JSON object:

  f1               encoded frame of key 1's on-tick (alpha 0) present
  engage_vbl       VBL of the first path tick
  vbl_range        first and last VBL of the 2N-3 present window
  alpha_hist       alpha values over the window (0 and 0.5 expected)
  cuts             presents in the window flagged cut or hold
  key_mismatch     on-tick presents whose camera is not their key's pose
  last_dev         largest per-axis distance of the window's last present from its key
  take             presents post takes: 2N-3, or 2N-4 when last_dev > 4 (the
                   last present shows the next shot, not 1-unit rounding)
  vbls_per_tick    histogram of VBLs between path ticks
  encoded_frames   frames the encoder published

Key 1 is found as the first on-tick present showing key 2's pose, two
presents back (keys 0 and 1 may share a pose).
"""
import json
import re
import sys
from collections import Counter

RF = re.compile(r"\[fcpath\] vbl=(\d+) seg=(\d+) t=\d+ pose=\((-?\d+),(-?\d+),(-?\d+)\)")
RA = re.compile(r"\[alpha\] vbl=(\d+) key=(\d+) paired=(\d) qa=([-\d.]+) alpha=([-\d.]+) "
                r"bracketed=(\d) cut=(\d) hold=(\d)")
RC = re.compile(r"\[cut-census vbl=(\d+)\] shown=camera a=([-\d.]+) cut=\d pos=\((-?\d+),(-?\d+),(-?\d+)\)")
RQ = re.compile(r"\[recq\] pts=(\d+) vbl=(\d+)")
RP = re.compile(r"\[recpub\] n=(\d+) pts=(\d+)")
LAST_DEV_MAX = 4


def parse(path):
    """The log's path ticks, presents, shown positions and encoder maps."""
    fc, al, cen, rq, rp = [], [], {}, [], []
    with open(path, errors="replace") as f:
        for line in f:
            if m := RF.match(line):
                fc.append(tuple(map(int, m.groups())))
            elif m := RA.match(line):
                g = m.groups()
                al.append(dict(frame=int(g[0]), key=int(g[1]), alpha=float(g[4]),
                               cut=int(g[6]), hold=int(g[7])))
            elif m := RC.match(line):
                cen.setdefault(int(m.group(1)), tuple(map(int, m.groups()[2:])))
            elif m := RQ.match(line):
                rq.append((int(m.group(1)), int(m.group(2))))
            elif m := RP.match(line):
                rp.append((int(m.group(1)), int(m.group(2))))
    return fc, al, cen, rq, rp


def key1_index(fc, al, cen, out):
    """Index in `al` of key 1's on-tick present."""
    E = out["engage_vbl"]
    k2 = next(tuple(f[2:5]) for f in fc if f[1] == 2 and f[0] > E)
    for i, a in enumerate(al):
        if a["key"] >= E and abs(a["alpha"]) < 1e-6 and cen.get(a["frame"]) == k2:
            return i - 2
    # fall back: the first on-tick present at or after the commit of key 2
    k1 = next(f for f in fc if f[1] == 1 and f[0] > E)
    out["f1_fallback"] = True
    return next(i for i, a in enumerate(al) if a["key"] >= k1[0] + 2 and abs(a["alpha"]) < 1e-6)


def analyze(path, nkeys):
    """Return the QC record of one capture log for a scene of nkeys disc keys."""
    fc, al, cen, rq, rp = parse(path)
    missing = [t for t, v in (("[fcpath]", fc), ("[alpha]", al), ("[cut-census] shown=camera", cen),
                              ("[recq]", rq), ("[recpub]", rp)) if not v]
    if missing:
        raise ValueError(f"{path}: no {', '.join(missing)} lines")
    out = {}
    E = next(f for f in fc if f[1] == 0)[0]
    out["engage_vbl"] = E
    k1 = next(f for f in fc if f[1] == 1 and f[0] > E)
    want = tuple(k1[2:5])
    idx = key1_index(fc, al, cen, out)
    a1 = al[idx]
    if cen.get(a1["frame"]) != want:
        out["key1_pose_check"] = "FAIL"
    out["present_key1"] = dict(a1, pos=cen.get(a1["frame"]), want=want)
    # encoded frame: recpub n -> pts -> recq vbl; the nth present of that vbl as in [alpha]
    pts2vbl = dict(rq)
    same = [i for i, a in enumerate(al) if a["key"] == a1["key"]]
    occ = same.index(idx)
    cands = [n for n, p in rp if pts2vbl.get(p) == a1["key"]]
    out["f1"] = cands[occ] if occ < len(cands) else None
    out["f1_cands"] = cands
    n = 2 * nkeys - 3
    win = al[idx: idx + n]
    out["vbl_range"] = [win[0]["key"], win[-1]["key"]]
    out["alpha_hist"] = dict(Counter(round(a["alpha"], 4) for a in win))
    out["cuts"] = [(a["key"], a["cut"], a["hold"]) for a in win if a["cut"] or a["hold"]]
    # present j (even) shows key 1 + j/2 at alpha 0
    keypose = {}
    for v, s, x, y, z in fc:
        if v >= E and s not in keypose:
            keypose[s] = (x, y, z)
    mism, first = 0, []
    for j, a in enumerate(win):
        if j % 2:
            continue
        k = 1 + j // 2
        if cen.get(a["frame"]) != keypose.get(k):
            mism += 1
            if len(first) < 5:
                first.append((j, a["key"], k, cen.get(a["frame"]), keypose.get(k)))
    out["key_mismatch"] = mism
    out["key_mismatch_first"] = first
    last, kl = win[-1], 1 + (len(win) - 1) // 2
    shown, kp = cen.get(last["frame"]), keypose.get(kl)
    out["last_dev"] = (max(abs(p - q) for p, q in zip(shown, kp)) if shown and kp else None)
    out["take"] = n - 1 if out["last_dev"] is None or out["last_dev"] > LAST_DEV_MAX else n
    ticks = [f[0] for f in fc if f[0] >= E]
    out["vbls_per_tick"] = dict(Counter(ticks[i] - ticks[i - 1] for i in range(1, len(ticks))))
    out["encoded_frames"] = len(rp)
    if len(win) < n or out["f1"] is None or out["f1"] + out["take"] > len(rp):
        out["short_clip"] = True
    return out


def main():
    """Print the QC record of RUN.log."""
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    try:
        print(json.dumps(analyze(sys.argv[1], int(sys.argv[2]))))
    except ValueError as e:
        sys.exit(str(e))


if __name__ == "__main__":
    main()
