#!/usr/bin/env python3
"""CPU re-emulation of the TERRAIN_FP32 terrain position chain, and the
analytic ulp census, over an OPENPETE_TERRAIN_FP32_EMU dump.

The dump (games/common/renderer/terrain.c tn_fp32emu_dump) holds one `C`
line per terrain build (feed key, camera in world/16, the matrix rows, H,
ofx, ofy, near plane) and, after it, `S` / `V` lines for every sector slot
decoded since the previous build: the slot's vertices as world x4 integers.

Builds are stamped with the feed key, a VBL count. The camera is resampled
linearly between consecutive builds at most 4 VBLs apart at --rate presents
per second (default 720), the VBL period 37609/2250000 s; each present runs, for every vertex of the
resident sectors that is visible at that present's camera (view-Z above
the near plane and --zmin, default 16, the view-Z below which the cache
claims a sector to the near path, and the projection inside the 512x240
GTE screen plus --margin PSX px):

  exact  the camera-relative operand, mac_of and proj_of in float64 on the
         exact operands (the error of float64 here is below 1e-12 px).
  fp32   the TERRAIN_FP32 chain in float32 in its operation order, with no
         contraction: the camera split as tc_globals_f32 splits it (int32
         floor of cam x4 and the float32 fraction), cam_rel, mac_of with the
         /4096 as a multiply by its exact reciprocal, proj_of with a
         correctly rounded divide.

Outputs, in window pixels (PSX px x --width / 512):
  err_max     max over presents and vertices of |fp32 - exact| per axis,
              plus the ANV divide term (2.5 ulp of |m*H/z|, the OpFDiv
              bound, added analytically since the host divide is correctly
              rounded); err_max_host the same without that term
  held        vertex-presents where the fp32 position equals the previous
              present's while the exact one moved by more than 1/256 px
  presents, vertex_presents, moved (exact deltas above zero): engagement;
              the verdict is refused at zero
  census      per level segment: max over visible vertices and builds of
              ulp(|v|) * H / z in window px (|v| the operand's Euclidean
              norm), and the vertex and feed key attaining it, against the
              1/256 px snap quantum

A level segment starts at a build that decodes --level-burst or more
sectors at once; slots decoded before it are dropped.

Usage: terrain_fp32_emu.py DUMP [--width W] [--rate R] [--margin M]
           [--keys LO:HI] [--level-burst N] [--zmin Z]
"""
import sys

import numpy as np

VBL_S = 37609.0 / 2250000.0
F = np.float32


def parse(path):
    builds = []   # (key, cam[3], M[3][3], H, ofx, ofy, nearz, slots dict copy-on-write)
    slots = {}
    cur = None
    pending = []
    for line in open(path):
        t = line.split()
        if not t:
            continue
        if t[0] == "C":
            if cur is not None:
                builds.append((cur, pending))
            cur = (int(t[1]), [float(x) for x in t[2:5]],
                   np.array([int(x) for x in t[5:14]], dtype=np.int64).reshape(3, 3),
                   float(t[14]), float(t[15]), float(t[16]), float(t[17]))
            pending = []
        elif t[0] == "S":
            pending.append([int(t[1]), []])
        elif t[0] == "V" and pending:
            pending[-1][1].append((int(t[1]), int(t[2]), int(t[3])))
    if cur is not None:
        builds.append((cur, pending))
    return builds


def exact_chain(w, cam, M, H, ofx, ofy):
    c4 = np.array(cam) * 4.0
    v = np.stack([(c4[1] - w[:, 1]) * 0.25, (c4[2] - w[:, 2]) * 0.25,
                  (w[:, 0] - c4[0]) * 0.25], axis=1)
    m = v @ M.T.astype(np.float64) / 4096.0
    z = np.maximum(m[:, 2], 1e-9)
    return v, m, np.stack([ofx + m[:, 0] * H / z, ofy + m[:, 1] * H / z], axis=1)


def f32_chain(w, cam, M, H, ofx, ofy):
    ci, cf = [], []
    for k in range(3):
        c4 = cam[k] * 4.0
        fl = np.floor(c4)
        fr = F(c4 - fl)
        if fr >= F(1.0):
            fl += 1.0
            fr = F(0.0)
        ci.append(int(fl))
        cf.append(fr)
    w = w.astype(np.int64)
    vx = (F(ci[1] - w[:, 1]).astype(F) + cf[1]).astype(F)
    vy = (F(ci[2] - w[:, 2]).astype(F) + cf[2]).astype(F)
    vz = ((w[:, 0] - ci[0]).astype(F) - cf[0]).astype(F)
    vx, vy, vz = vx * F(0.25), vy * F(0.25), vz * F(0.25)
    Mf = M.astype(F)
    rcp = F(1.0) / F(4096.0)
    m = []
    for r in range(3):
        a = (Mf[r, 0] * vx).astype(F)
        a = (a + (Mf[r, 1] * vy).astype(F)).astype(F)
        a = (a + (Mf[r, 2] * vz).astype(F)).astype(F)
        m.append((a * rcp).astype(F))
    z = np.where(m[2] > F(1e-9), m[2], F(1e-9)).astype(F)
    Hf, ox, oy = F(H), F(ofx), F(ofy)
    qx = ((m[0] * Hf).astype(F) / z).astype(F)
    qy = ((m[1] * Hf).astype(F) / z).astype(F)
    return np.stack([(ox + qx).astype(F), (oy + qy).astype(F)], axis=1), qx, qy


def main(argv):
    if not argv:
        print(__doc__)
        return 64
    path = argv[0]
    opt = {"--width": 1280.0, "--rate": 720.0, "--margin": 64.0, "--level-burst": 40,
           "--zmin": 16.0}
    keys = None
    i = 1
    while i < len(argv):
        if argv[i] == "--keys":
            lo, hi = argv[i + 1].split(":")
            keys = (int(lo), int(hi))
        else:
            opt[argv[i]] = type(opt[argv[i]])(argv[i + 1])
        i += 2
    scale = opt["--width"] / 512.0
    builds = parse(path)
    slots = {}
    seg = 0
    census = {}
    err_max = err_host = 0.0
    err_at = None
    held = presents = vpres = moved = 0
    prev = None   # (ids, exact, f32) of the previous present
    acc = 0.0     # present phase carried across builds
    for bi in range(len(builds)):
        (key, cam, M, H, ofx, ofy, nearz), dec = builds[bi]
        if len(dec) >= opt["--level-burst"]:
            seg += 1
            slots = {}
            prev = None
        for sl, vs in dec:
            slots[sl] = np.array(vs, dtype=np.int64).reshape(-1, 3)
        if not slots or bi + 1 >= len(builds):
            continue
        W = np.unique(np.concatenate(list(slots.values())), axis=0)
        ids = W[:, 0] * (1 << 40) + W[:, 1] * (1 << 20) + W[:, 2]
        v, m, se = exact_chain(W, cam, M, H, ofx, ofy)
        zmin = max(nearz, opt["--zmin"])
        vis = (m[:, 2] > zmin) & (np.abs(se[:, 0] - ofx) <= 256 + opt["--margin"]) & \
              (np.abs(se[:, 1] - ofy) <= 120 + opt["--margin"])
        if vis.any():
            nv = np.linalg.norm(v[vis], axis=1).astype(F)
            u = np.spacing(nv).astype(np.float64) * H / m[vis, 2] * scale
            j = int(np.argmax(u))
            c = census.get(seg)
            if c is None or u[j] > c[0]:
                census[seg] = (float(u[j]), tuple(int(x) for x in W[vis][j]), key, int(vis.sum()))
        if keys and not (keys[0] <= key <= keys[1]):
            prev = None
            continue
        (nk, ncam, *_), _d = builds[bi + 1]
        if not 1 <= nk - key <= 4:
            prev = None
            continue
        per = (nk - key) * VBL_S * opt["--rate"]
        Wv, idv = W[vis], ids[vis]
        while acc < 1.0:
            a = acc
            pc = [cam[k] + (ncam[k] - cam[k]) * a for k in range(3)]
            _, me, pe = exact_chain(Wv, pc, M, H, ofx, ofy)
            pf, qx, qy = f32_chain(Wv, pc, M, H, ofx, ofy)
            # Visibility at the present's own camera: in front of the
            # retained regime's z floor and on screen.
            pv = (me[:, 2] > zmin) & (np.abs(pe[:, 0] - ofx) <= 256 + opt["--margin"]) & \
                 (np.abs(pe[:, 1] - ofy) <= 120 + opt["--margin"])
            pe, pf, qx, qy, idp, Wp = pe[pv], pf[pv], qx[pv], qy[pv], idv[pv], Wv[pv]
            d = np.abs(pf.astype(np.float64) - pe) * scale
            anv = 2.5 * np.stack([np.spacing(np.abs(qx)), np.spacing(np.abs(qy))],
                                 axis=1).astype(np.float64) * scale
            if d.size:
                k = np.unravel_index(int(np.argmax(d + anv)), d.shape)
                if d[k] + anv[k] > err_max:
                    err_max, err_at = float(d[k] + anv[k]), (key, a, tuple(int(x) for x in Wp[k[0]]))
                err_host = max(err_host, float(d.max()))
            if prev is not None:
                pid, pe0, pf0 = prev
                common, ia, ib = np.intersect1d(pid, idp, return_indices=True)
                de = np.abs(pe[ib] - pe0[ia]) * scale
                same = np.all(pf[ib] == pf0[ia], axis=1)
                mv = np.any(de > 0.0, axis=1)
                moved += int(mv.sum())
                held += int((same & np.any(de > 1.0 / 256.0, axis=1)).sum())
            prev = (idp, pe, pf)
            presents += 1
            vpres += len(idp)
            acc += 1.0 / per
        acc -= 1.0
    print("rate=%g width=%g presents=%d vertex_presents=%d moved=%d err_max=%.3e "
          "err_max_host=%.3e held=%d snap=%.3e"
          % (opt["--rate"], opt["--width"], presents, vpres, moved, err_max, err_host, held,
             1.0 / 256.0))
    if err_at:
        print("err_max_at key=%d alpha=%.4f w_x4=%s" % err_at)
    for s, (u, w, k, n) in sorted(census.items()):
        print("census seg=%d max=%.3e window_px at w_x4=%s key=%d visible=%d ratio_to_snap=%.4f"
              % (s, u, w, k, n, u * 256.0))
    verdict = "VACUOUS" if presents == 0 or vpres == 0 or moved == 0 else "DONE"
    print("verdict=%s" % verdict)
    return 0 if verdict == "DONE" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
