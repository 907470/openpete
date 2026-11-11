#!/usr/bin/env python3
"""Shared-vertex position readback: pair vertices by key, count disagreements.

A readback dump (OPENPETE_TERRAIN_READBACK=1, written beside a screenshot as
<base>-readback.bin; native_terrain_cache.c openpete_tercache_readback_write)
holds, per element of every drawn arena buffer of the retained terrain, two
texels of four 32-bit words: the bits of the vertex's final gl_Position and
its readback key (terrain.vert rb_*). The key names the operation tree the
vertex's camera-space position was built by (leaf: one integer world corner,
or a previous -> current pair; node: a mean of two positions; cut: an edge
clipped at the near plane), so two vertices with one key apply the same
operations to the same operands. A killed vertex writes neither texel.

  pairs DUMP [--detail N]
      Groups the written elements by key and counts, per family pair, the
      unordered pairs of elements sharing a key (compared), the pairs
      whose four gl_Position words differ (mismatch), and of each the pairs
      whose two elements were drawn by different sectors (xsec,
      xsec_mismatch; the sector slot rides bits 8-16 of the key's fourth
      word). A family is the arena
      class an element was drawn from: plain, anim, near, or cell (the near
      and anim cell-record arenas). A cut key (a near-plane cut vertex) is
      counted under cut/cut whatever its arenas. Prints a header line
      (elements, written, written per family, keys, shared keys, torn:
      elements with a key but no position) and one line per family pair, then up to N mismatching
      groups with each member's arena, element and position.
      Exit 0; 1 when the dump is unreadable.

  same DUMP_A DUMP_B
      Compares the multisets of (family, key, position) of two dumps of one
      pose. Prints same=1 or same=0 and the element counts. Exit 0 when the
      same, 1 otherwise.

  grade [--game G] [--carve PAIR=REASON]... LEG...
      Grades a run per family pair. Each LEG is POSE:LOD:ARM:PAIRS_FILE, a
      `pairs` output of the f32 or ctl arm. Per pair, summed over the legs:
      the poses that compared it, the f32 compared, xsec and mismatch
      counts, the ctl compared and mismatch counts, and the poses whose ctl
      leg mismatched on it. A pair's verdict:
        FAIL     an f32 mismatch;
        engaged  compared and the control mismatched on it;
        blind    compared, the control never mismatched: a zero f32 count
                 there is not shown able to fail;
        unseen   a named pair no leg compared;
        carve-out  blind or unseen and named by --carve with its reason.
      Prints one line per pair (the named pairs, then any other compared)
      and a summary line, verdict FAIL (any f32 mismatch), VACUOUS (a
      blind or unseen pair) or PASS. Exit 0 PASS, 1 FAIL, 2 VACUOUS.
"""
import struct
import sys

import numpy as np

FAMILIES = ("plain", "anim", "near", "cell")
# The named family pairs, in report order, then the rest.
NAMED = ["plain/plain", "plain/anim", "plain/near", "anim/near", "cell/near",
         "cell/anim", "cell/cell", "cut/cut"]
OTHER = ["anim/anim", "near/near", "plain/cell"]
KIND_CUT = 3


def family_of(arena):
    """Return the family index of an arena (openpete_terrain_cache.h order)."""
    if arena < 5:
        return 0
    if arena < 10:
        return 1
    if arena < 15:
        return 2
    return 3


def load(path):
    """Return (entries, family per element, arena per element, pos, key)."""
    data = open(path, "rb").read()
    if len(data) < 24:
        raise ValueError(f"{path}: short file")
    magic, ver, _w, _h, n, _row = struct.unpack_from("<6I", data, 0)
    if magic != 0x42524354 or ver != 1:
        raise ValueError(f"{path}: not a readback dump")
    ents = np.frombuffer(data, "<u4", count=5 * n, offset=24).reshape(n, 5)
    tex = np.frombuffer(data, "<u4", offset=24 + 20 * n).reshape(-1, 2, 4)
    elems = int(ents[:, 3].sum()) if n else 0
    if tex.shape[0] != elems:
        raise ValueError(f"{path}: {tex.shape[0]} elements, entries name {elems}")
    fam = np.empty(elems, np.int8)
    arena = np.empty(elems, np.int16)
    for a, _b, base, count, _per in ents:
        fam[base:base + count] = family_of(int(a))
        arena[base:base + count] = a
    return ents, fam, arena, tex[:, 0, :], tex[:, 1, :]


def canonical(f, g):
    """Return the report name of a family pair, cell first as NAMED spells it."""
    names = {FAMILIES[f], FAMILIES[g]}
    if "cell" in names and len(names) == 2:
        other = (names - {"cell"}).pop()
        return "plain/cell" if other == "plain" else f"cell/{other}"
    if len(names) == 1:
        n = names.pop()
        return f"{n}/{n}"
    a, b = sorted((f, g))
    return f"{FAMILIES[a]}/{FAMILIES[b]}"


def pair_counts(gid, fam, ngroups):
    """Return {family pair: unordered pairs within groups} for group ids gid."""
    n = np.zeros((ngroups, 4), np.int64)
    np.add.at(n, (gid, fam), 1)
    out = {}
    for f in range(4):
        for g in range(f, 4):
            c = int((n[:, f] * (n[:, f] - 1) // 2).sum()) if f == g else \
                int((n[:, f] * n[:, g]).sum())
            if c:
                name = canonical(f, g)
                out[name] = out.get(name, 0) + c
    return out


def rows_view(a):
    """Return a 1-D void view of a 2-D contiguous uint32 array, one item per row."""
    a = np.ascontiguousarray(a)
    return a.view(np.dtype((np.void, a.dtype.itemsize * a.shape[1]))).ravel()


def pairs_by(cols, f, cut):
    """Return {family pair: pairs} over the groups of rows equal in cols, cut keys as cut/cut."""
    _, gid = np.unique(rows_view(cols), return_inverse=True)
    gid = gid.ravel()
    ng = int(gid.max()) + 1 if gid.size else 0
    out = pair_counts(gid[~cut], f[~cut], ng)
    out["cut/cut"] = pair_counts(gid[cut], np.zeros_like(f[cut]), ng).get("plain/plain", 0)
    return out


def cmd_pairs(path, detail):
    """Print the per family pair census of one dump."""
    _ents, fam, arena, pos, key = load(path)
    written = key[:, 3] != 0
    torn = int((written & ~pos.any(axis=1)).sum())
    idx = np.nonzero(written)[0]
    kk = key[idx].copy()
    slot = (kk[:, 3] >> 8).astype(np.uint32)[:, None]
    kk[:, 3] &= 0xFF
    p = pos[idx]
    f = fam[idx]
    _, gid, gcount = np.unique(rows_view(kk), return_inverse=True, return_counts=True)
    gid = gid.ravel()
    shared = gcount[gid] > 1
    idx, kk, p, f, gid, slot = (idx[shared], kk[shared], p[shared], f[shared], gid[shared],
                                slot[shared])
    k = kk
    cut = kk[:, 3] == KIND_CUT
    _, gid = np.unique(gid, return_inverse=True)
    gid = gid.ravel()
    _, sid = np.unique(rows_view(np.concatenate([kk, p], axis=1)), return_inverse=True)
    sid = sid.ravel()
    total = pairs_by(kk, f, cut)
    match = pairs_by(np.concatenate([kk, p], axis=1), f, cut)
    s_total = pairs_by(np.concatenate([kk, slot], axis=1), f, cut)
    s_match = pairs_by(np.concatenate([kk, p, slot], axis=1), f, cut)
    wf = " ".join(f"written_{FAMILIES[i]}={int((fam[written] == i).sum())}" for i in range(4))
    print(f"elements={fam.size} written={int(written.sum())} {wf} "
          f"keys={int(gcount.size)} shared_keys={int((gcount > 1).sum())} torn={torn}")
    for name in NAMED + [n for n in OTHER if total.get(n, 0)]:
        c = total.get(name, 0)
        m = c - match.get(name, 0)
        sc = s_total.get(name, 0)
        sm = sc - s_match.get(name, 0)
        print(f"pair={name} compared={c} mismatch={m} xsec={c - sc} xsec_mismatch={m - sm}")
    if detail <= 0:
        return 0
    # Groups with more than one distinct position.
    order = np.argsort(gid, kind="stable")
    shown = 0
    start = 0
    while start < order.size and shown < detail:
        g = gid[order[start]]
        end = start
        while end < order.size and gid[order[end]] == g:
            end += 1
        members = order[start:end]
        if np.unique(sid[members]).size > 1:
            shown += 1
            kk = k[members[0]]
            print(f"mismatch key={kk[0]:08x}{kk[1]:08x}{kk[2]:08x} kind={kk[3]}")
            for m in members:
                xyzw = p[m].view("<f4")
                print(f"  arena={int(arena[idx[m]])} elem={int(idx[m])} "
                      f"pos=({xyzw[0]!r}, {xyzw[1]!r}, {xyzw[2]!r}, {xyzw[3]!r}) "
                      f"bits={p[m][0]:08x} {p[m][1]:08x} {p[m][2]:08x} {p[m][3]:08x}")
        start = end
    return 0


def cmd_same(a, b):
    """Print whether two dumps hold the same (family, key, position) multiset."""
    rows = []
    for path in (a, b):
        _e, fam, _ar, pos, key = load(path)
        w = key[:, 3] != 0
        r = np.concatenate([fam[w].astype(np.uint32)[:, None], key[w], pos[w]], axis=1)
        rows.append(np.sort(rows_view(r)))
    same = rows[0].size == rows[1].size and bool((rows[0] == rows[1]).all())
    print(f"same={int(same)} written_a={rows[0].size} written_b={rows[1].size}")
    return 0 if same else 1


def read_pairs(path):
    """Return {family pair: (compared, mismatch, xsec, xsec_mismatch)} from a `pairs` output."""
    out = {}
    for ln in open(path):
        if not ln.startswith("pair="):
            continue
        kv = dict(t.split("=", 1) for t in ln.split())
        out[kv["pair"]] = tuple(int(kv[k]) for k in ("compared", "mismatch", "xsec",
                                                     "xsec_mismatch"))
    return out


def cmd_grade(game, carve, legs):
    """Print the per family pair verdict table of a run and return the exit code."""
    keys = ("poses", "f32_c", "f32_x", "f32_m", "ctl_c", "ctl_m", "ctl_xm", "ctl_poses")
    acc = {}
    seen_pose = {}
    for leg in legs:
        pose, lod, arm, path = leg.split(":", 3)
        for name, (c, m, x, xm) in read_pairs(path).items():
            a = acc.setdefault(name, dict.fromkeys(keys, 0))
            if arm == "f32":
                a["f32_c"] += c; a["f32_x"] += x; a["f32_m"] += m
                if c and pose not in seen_pose.setdefault((name, "f32"), set()):
                    seen_pose[(name, "f32")].add(pose); a["poses"] += 1
            else:
                a["ctl_c"] += c; a["ctl_m"] += m; a["ctl_xm"] += xm
                if m and pose not in seen_pose.setdefault((name, "ctl"), set()):
                    seen_pose[(name, "ctl")].add(pose); a["ctl_poses"] += 1
    names = NAMED + sorted(n for n in acc if n not in NAMED and acc[n]["f32_c"])
    fail = vac = 0
    bad = []
    for name in names:
        a = acc.get(name, dict.fromkeys(keys, 0))
        if a["f32_m"]:
            v = "FAIL"
        elif a["f32_c"] and a["ctl_m"]:
            v = "engaged"
        else:
            v = "blind" if a["f32_c"] else "unseen"
            if name in carve:
                v = "carve-out"
        fail += v == "FAIL"
        if v in ("blind", "unseen"):
            vac += 1
            bad.append(f"{name}:{v}")
        why = f" reason=\"{carve[name]}\"" if v == "carve-out" else ""
        print(f"[readback] game={game} pair={name} poses={a['poses']} "
              f"f32_compared={a['f32_c']} f32_xsec={a['f32_x']} f32_mismatch={a['f32_m']} "
              f"ctl_compared={a['ctl_c']} ctl_mismatch={a['ctl_m']} "
              f"ctl_xsec_mismatch={a['ctl_xm']} ctl_poses={a['ctl_poses']} verdict={v}{why}")
    f32_m = sum(a["f32_m"] for a in acc.values())
    ctl_m = sum(a["ctl_m"] for a in acc.values())
    verdict = "FAIL" if fail else ("VACUOUS" if vac else "PASS")
    print(f"[readback] game={game} pairs={len(names)} f32_mismatch={f32_m} ctl_mismatch={ctl_m} "
          f"not_engaged={','.join(bad) or 'none'} pair_verdict={verdict}")
    return {"PASS": 0, "FAIL": 1, "VACUOUS": 2}[verdict]


def main(argv):
    """Dispatch the subcommand."""
    if argv and argv[0] == "grade":
        game, carve, legs = "?", {}, []
        it = iter(argv[1:])
        for a in it:
            if a == "--game":
                game = next(it)
            elif a == "--carve":
                p, _, why = next(it).partition("=")
                carve[p] = why
            else:
                legs.append(a)
        try:
            return cmd_grade(game, carve, legs)
        except (OSError, ValueError, KeyError) as e:
            print(f"terrain_readback: {e}", file=sys.stderr)
            return 1
    if len(argv) >= 2 and argv[0] == "pairs":
        detail = 0
        if "--detail" in argv:
            detail = int(argv[argv.index("--detail") + 1])
        try:
            return cmd_pairs(argv[1], detail)
        except (OSError, ValueError) as e:
            print(f"terrain_readback: {e}", file=sys.stderr)
            return 1
    if len(argv) == 3 and argv[0] == "same":
        try:
            return cmd_same(argv[1], argv[2])
        except (OSError, ValueError) as e:
            print(f"terrain_readback: {e}", file=sys.stderr)
            return 1
    print(__doc__, file=sys.stderr)
    return 64


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
