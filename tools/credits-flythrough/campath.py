#!/usr/bin/env python3
"""campath.py - measure and smooth --freecam-path camera paths (Spyro 2/3 credits).

  campath.py measure  [--H 341 --rows 240 --out-h 2160] PATH...
  campath.py smooth   [--tol-pos 1 --tol-ang 0.15 --order 3] IN OUT
  campath.py logmetric [--H ...] RUNLOG [--from-key A --to-key B]

Path format: "ticks=1;" then one pose per tick "x,y,z,yaw,pitch,roll".
Integer keys come from the game: position on a 4-unit lattice (key << 2),
angles in 4096-per-turn units. The view is Rz(roll)·Rx(pitch)·Ry(yaw)
(Spyro 2/3 builder func_8004F6C4), sky matrix unscaled, world matrix with
row 1 scaled by 0x140/512.

Presentation model (record-cfr 60): one present on each tick (alpha 0, the
key) and one at its midpoint (alpha 0.5). Stock: the renderer lerps the two
ticks' Q12 int16 matrices componentwise and rounds. Fractional: the matrix
of the pose interpolated at the present's path parameter, rounded to Q12.

Jitter instrument: for presents k, w_k is the rotation vector of
R_{k+1}·R_k^T; the angular jerk |w_{k+1} - w_k| times the focal length in
output pixels is the whole-frame jerk in px at the image centre. Position:
|p_{k+1} - 2 p_k + p_{k-1}| in world units, and in px for a point at a
reference depth. Presents touching a cut are excluded.
"""
import argparse
import math
import re
import sys

import numpy as np
from scipy.sparse import diags
from scipy.spatial.transform import Rotation

TURN = 4096.0
K = 2 * math.pi / TURN


def load(path):
    """Return (header, array n x 6) of a path file."""
    segs = [s.strip() for s in re.split(r"[;\n]", open(path).read()) if s.strip()]
    hdr = [s for s in segs if s.startswith("ticks=")]
    rows = [[float(v) for v in s.split(",")] for s in segs
            if not s.startswith("ticks=") and s != "cut"]
    w = np.zeros((len(rows), 6))
    for i, r in enumerate(rows):
        w[i, :len(r)] = r
    return (hdr[0] if hdr else "ticks=1"), w


def unwrap_angles(w):
    """Unwrap yaw, pitch and roll columns to continuous angle units."""
    y = w.copy()
    for c in (3, 4, 5):
        y[:, c] = np.unwrap(y[:, c] * K) / K
    return y


def rot(axis, units):
    """Game-form rotation matrix of one axis (freecam_game.c rot_game)."""
    r = units * K
    c, s = math.cos(r), math.sin(r)
    if axis == 0:
        return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])
    if axis == 1:
        return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
    return np.array([[c, s, 0], [-s, c, 0], [0, 0, 1]])


def view(yaw, pitch, roll):
    """Rz(roll)·Rx(pitch)·Ry(yaw), unit scale."""
    return rot(2, roll) @ rot(0, pitch) @ rot(1, yaw)


def q12(m):
    """Round a unit matrix to the game's Q12 int16 form, kept as float /4096."""
    return np.round(m * 4096.0) / 4096.0


def find_cuts(y, pos_thr=64.0, ang_thr=8.0):
    """Indices i where key i -> i+1 is a cut: a velocity jump larger than either threshold on both sides."""
    v = np.diff(y, axis=0)
    a = np.diff(v, axis=0)
    pa = np.linalg.norm(a[:, :3], axis=1)
    aa = np.abs(a[:, 3:]).max(axis=1)
    bad = (pa > pos_thr) | (aa > ang_thr)
    cuts = set()
    # a[j] = v[j+1] - v[j]: a lone outlier step v[i] shows as bad[i-1] and bad[i]
    for j in range(len(bad) - 1):
        if bad[j] and bad[j + 1]:
            cuts.add(j + 1)
    return sorted(cuts)


def segments(n, cuts):
    """Split 0..n-1 into runs at cuts (a cut i separates key i from key i+1)."""
    out, s = [], 0
    for c in cuts:
        out.append((s, c + 1))
        s = c + 1
    out.append((s, n))
    return [(a, b) for a, b in out if b > a]


def present_poses(y, frac):
    """Per-present (pose, segment id): keys at alpha 0 and linear midpoints at alpha 0.5.

    frac False models the stock engine (matrix lerp of rounded key matrices),
    True a fractional path: the engine's Catmull-Rom pose at the present,
    matrices rounded to Q12."""
    mats, pos, seg = [], [], []
    keym = [q12(view(*k[3:6])) for k in y]
    for i in range(len(y) - 1):
        for al in (0.0, 0.5):
            p = y[i, :3] + al * (y[i + 1, :3] - y[i, :3])
            if frac:
                # the engine's curve: uniform Catmull-Rom, ends extended linearly
                p1, p2 = y[i], y[i + 1]
                p0 = y[i - 1] if i > 0 else 2 * p1 - p2
                p3 = y[i + 2] if i + 2 < len(y) else 2 * p2 - p1
                t = al
                v = 0.5 * (2 * p1 + (p2 - p0) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t
                           + (3 * p1 - p0 - 3 * p2 + p3) * t ** 3)
                p = v[:3]
                m = q12(view(*v[3:]))
            else:
                m = q12(keym[i] + al * (keym[i + 1] - keym[i]))
            mats.append(m)
            pos.append(p)
            seg.append(i)
    return np.array(mats), np.array(pos), np.array(seg)


def jerk_metrics(mats, pos, seg_of_present, cut_keys):
    """Angular jerk (rad) and position second difference (units) per interior present, cut-adjacent ones dropped."""
    R = Rotation.from_matrix(mats)
    w = (R[1:] * R[:-1].inv()).as_rotvec()
    jw = np.linalg.norm(np.diff(w, axis=0), axis=1)        # centred at present k+1
    jp = np.linalg.norm(pos[2:] - 2 * pos[1:-1] + pos[:-2], axis=1)
    keep = np.ones(len(jw), bool)
    cutset = set(cut_keys)
    for k in range(len(jw)):
        if any(int(seg_of_present[k + d]) in cutset for d in (0, 1, 2)):
            keep[k] = False
    return jw[keep], jp[keep]


def focal_px(H, rows, out_h, vfov=None):
    """Focal length in output pixels from the vertical FOV, else tan(vfov/2) = (rows/2) / (0.625 H)."""
    t = math.tan(math.radians(vfov) / 2) if vfov else (rows / 2.0) / (0.625 * H)
    return (out_h / 2.0) / t, 2 * math.degrees(math.atan(t))


def stats(x):
    """RMS, p99 and max."""
    if len(x) == 0:
        return (0.0, 0.0, 0.0)
    return (float(np.sqrt(np.mean(x ** 2))), float(np.percentile(x, 99)), float(x.max()))


def cmd_measure(a):
    """The measure subcommand: modelled per-present jerk of each path file."""
    f, vfov = focal_px(a.H, a.rows, a.out_h, a.vfov)
    print(f"focal {f:.1f} px at {a.out_h} rows (vfov {vfov:.2f} deg, H {a.H}, rows {a.rows}); "
          f"1 angle unit = {K * f:.2f} px; position px at depth {a.depth:.0f}")
    print("scene                       model  ang-jerk px rms/p99/max     pos d2 units rms/p99/max   "
          "(px@depth)  quant-resid ang rms")
    for p in a.paths:
        _, w = load(p)
        y = unwrap_angles(w)
        cuts = find_cuts(y)
        # cut-free quantisation residual: key minus a heavy local fit
        res = []
        for s0, s1 in segments(len(y), cuts):
            if s1 - s0 < 9:
                continue
            for c in (3, 4, 5):
                seg = y[s0:s1, c]
                t = np.arange(len(seg))
                fit = np.zeros_like(seg)
                for i in range(len(seg)):
                    lo, hi = max(0, i - 6), min(len(seg), i + 7)
                    co = np.polyfit(t[lo:hi], seg[lo:hi], 2)
                    fit[i] = np.polyval(co, i)
                res.append(seg - fit)
        qr = float(np.sqrt(np.mean(np.concatenate(res) ** 2))) if res else 0.0
        frac_input = bool(np.any(np.abs(w - np.round(w)) > 1e-9))
        models = [("frac", True)] if frac_input else [("stock", False)]
        for name, fr in models:
            m, ps, sg = present_poses(y, fr)
            jw, jp = jerk_metrics(m, ps, sg, cuts)
            r = stats(jw * f)
            q = stats(jp)
            print(f"{p[-40:]:40s} {name:5s} {r[0]:6.2f} {r[1]:6.2f} {r[2]:6.2f}   "
                  f"{q[0]:7.2f} {q[1]:7.2f} {q[2]:7.2f}   ({q[1] * f / a.depth:5.2f} p99)  {qr:.3f}"
                  f"  cuts={cuts}")


def smooth_channel(yc, bound, order, eps=1e-4, pin=1e7, max_iter=4000):
    """The smoothest curve (least squared order-th difference) inside |z - y| <= bound.

    Active-set solve of min |D z|^2 + eps |z - y|^2 subject to the tube: each
    round pins the local peaks of violation to the bound they cross (weight
    pin), and releases a pinned key whose pin pulls it outward (the curve,
    unpinned there, would sit inside the tube)."""
    from scipy.sparse.linalg import spsolve
    n = len(yc)
    if n <= order + 1:
        return yc.copy()
    coeffs = [(-1) ** (order - k) * math.comb(order, k) for k in range(order + 1)]
    D = diags(coeffs, list(range(order + 1)), shape=(n - order, n))
    DtD = (D.T @ D).tocsc()
    lo, hi = yc - bound, yc + bound
    side = np.zeros(n)            # +1 pinned at hi, -1 at lo, 0 free
    z = yc.copy()
    for _ in range(max_iter):
        w = np.where(side != 0, pin, eps)
        t = np.where(side > 0, hi, np.where(side < 0, lo, yc))
        z = spsolve((DtD + diags(w)).tocsc(), w * t)
        viol = np.where(z > hi + 1e-9, z - hi, np.where(z < lo - 1e-9, lo - z, 0.0))
        viol[side != 0] = 0.0
        changed = False
        if viol.max() > 1e-6:
            # local peaks of the violation within +-3 keys
            for i in np.nonzero(viol > 1e-6)[0]:
                a, b = max(0, i - 3), min(n, i + 4)
                if viol[i] >= viol[a:b].max():
                    side[i] = 1 if z[i] > hi[i] else -1
                    changed = True
        else:
            # release pins whose force points into the tube
            g = DtD @ z           # gradient of the smoothness term at z
            rel = ((side > 0) & (g > 1e-9)) | ((side < 0) & (g < -1e-9))
            if rel.any():
                # release the strongest one per window to avoid oscillation
                gi = np.abs(g) * rel
                for i in np.nonzero(rel)[0]:
                    a, b = max(0, i - 3), min(n, i + 4)
                    if gi[i] >= gi[a:b].max():
                        side[i] = 0
                        changed = True
        if not changed:
            break
    return np.clip(z, lo, hi)


def wrap(a):
    """Wrap an angle to [-2048, 2048)."""
    return ((a + 2048.0) % 4096.0) - 2048.0


def smooth_file(inp, out, q_pos=4.0, q_ang=1.0, tol_pos=1.0, tol_ang=0.15, order=3):
    """Write the smoothed path of `inp` to `out`; return a one-line summary.

    Each channel of each cut-free run becomes the smoothest curve (least
    squared order-th difference) inside +-(q/2 + tol) of the keys; a key pair
    find_cuts reports is written as a `cut` entry, and the wrap from the last
    key to the first is a cut unless it continues the motion."""
    hdr, w = load(inp)
    y = unwrap_angles(w)
    cuts = find_cuts(y)
    z = y.copy()
    bnd = [q_pos / 2 + tol_pos] * 3 + [q_ang / 2 + tol_ang] * 3
    for s0, s1 in segments(len(y), cuts):
        for c in range(6):
            z[s0:s1, c] = smooth_channel(y[s0:s1, c], bnd[c], order)
    dev = z - y
    dpos = np.linalg.norm(dev[:, :3], axis=1).max()
    dang = np.abs(dev[:, 3:]).max(axis=0)
    with open(out, "w") as fo:
        parts = [hdr]
        cutset = set(cuts)
        for i, r in enumerate(z):
            parts.append(",".join([f"{r[0]:.3f}", f"{r[1]:.3f}", f"{r[2]:.3f}",
                                   f"{wrap(r[3]):.4f}", f"{wrap(r[4]):.4f}", f"{wrap(r[5]):.4f}"]))
            if i in cutset:
                parts.append("cut")
        # the loop back to the first key: a cut unless it continues the motion
        wrap_step = y[0] - y[-1]
        wrap_step[3:] = (wrap_step[3:] + 2048) % 4096 - 2048
        v_end = y[-1] - y[-2]
        if np.linalg.norm(wrap_step[:3] - v_end[:3]) > 64 or np.abs(wrap_step[3:] - v_end[3:]).max() > 8:
            parts.append("cut")
            wrap_cut = True
        else:
            wrap_cut = False
        fo.write(";".join(parts) + "\n")
    return (f"{inp} -> {out}: {len(y)} keys, cuts {cuts}, wrap cut {wrap_cut}, max dev pos {dpos:.2f} units "
            f"(per axis {np.abs(dev[:, :3]).max(axis=0).round(2).tolist()}), "
            f"angles yaw/pitch/roll {dang.round(3).tolist()} units = "
            f"{(dang * 360 / 4096).round(4).tolist()} deg")


def cmd_smooth(a):
    """The smooth subcommand: smooth_file on the parsed arguments."""
    print(smooth_file(a.inp, a.out, a.q_pos, a.q_ang, a.tol_pos, a.tol_ang, a.order))


def cmd_logmetric(a):
    """Jitter from the engine's per-present camera log ([campose] lines, OPENPETE_CAM_DBG)."""
    f, _ = focal_px(a.H, a.rows, a.out_h, a.vfov)
    rx = re.compile(r"\[campose\] vbl=(\d+) key=(\d+) alpha=([-0-9.]+) cut=(\d) path=(\d) "
                    r"M=\[([-0-9 ]+)\] pos=\(([-0-9., ]+)\)(?: vfov=([0-9.]+) hfov=([0-9.]+))?")
    mats, pos, keys, cut, path, alphas, fov = [], [], [], [], [], [], None
    for line in open(a.log, errors="replace"):
        m = rx.search(line)
        if not m:
            continue
        k = int(m.group(2))
        if k < a.from_key or k > a.to_key:
            continue
        M = np.array([float(v) for v in m.group(6).split()]).reshape(3, 3) / 4096.0
        M /= np.linalg.norm(M, axis=1, keepdims=True)   # rows carry the game's scale
        mats.append(M)
        pos.append([float(v) for v in m.group(7).split(",")])
        keys.append(k)
        cut.append(int(m.group(4)))
        path.append(int(m.group(5)))
        alphas.append(round(float(m.group(3)), 3))
        if m.group(8):
            fov = (float(m.group(8)), float(m.group(9)))
    mats, pos = np.array(mats), np.array(pos)
    # a present next to a cut (or a jump the cut test missed) is not motion
    seg = np.cumsum(np.array(cut))
    jw, jp = jerk_metrics(mats, pos, seg, [])
    ok = np.ones(len(jw), bool)
    for k in range(len(jw)):
        if seg[k] != seg[k + 2] or any(cut[k + d] for d in (0, 1, 2)):
            ok[k] = False
    ok &= jp < 500
    r, q = stats(jw[ok] * f), stats(jp[ok])
    al = {}
    for x in alphas:
        al[x] = al.get(x, 0) + 1
    print(f"{a.log}: {len(mats)} presents, keys {min(keys)}-{max(keys)}, path pose {sum(path)}, "
          f"cuts {sum(cut)}, fov v/h {fov}, alpha {dict(sorted(al.items())[:6])}")
    print(f"  ang-jerk px rms/p99/max {r[0]:.2f} {r[1]:.2f} {r[2]:.2f}; "
          f"pos d2 units rms/p99/max {q[0]:.2f} {q[1]:.2f} {q[2]:.2f} (n={int(ok.sum())})")


def main():
    """Parse the subcommand and run it."""
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("measure", "logmetric"):
        s = sub.add_parser(name)
        s.add_argument("--H", type=float, default=341.0)
        s.add_argument("--rows", type=float, default=240.0)
        s.add_argument("--out-h", type=float, default=2160.0)
        s.add_argument("--depth", type=float, default=2000.0)
        s.add_argument("--vfov", type=float, default=53.747,
                       help="vertical FOV in degrees (the [campose] line's vfov; S3 credits 53.747)")
    sub.choices["measure"].add_argument("paths", nargs="+")
    lm = sub.choices["logmetric"]
    lm.add_argument("log")
    lm.add_argument("--from-key", type=int, default=0)
    lm.add_argument("--to-key", type=int, default=1 << 30)
    s = sub.add_parser("smooth")
    s.add_argument("inp")
    s.add_argument("out")
    s.add_argument("--q-pos", type=float, default=4.0, help="position lattice step")
    s.add_argument("--q-ang", type=float, default=1.0, help="angle quantum")
    s.add_argument("--tol-pos", type=float, default=1.0)
    s.add_argument("--tol-ang", type=float, default=0.15)
    s.add_argument("--order", type=int, default=3)
    a = ap.parse_args()
    return {"measure": cmd_measure, "smooth": cmd_smooth, "logmetric": cmd_logmetric}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
