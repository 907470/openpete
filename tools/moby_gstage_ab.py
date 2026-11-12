#!/usr/bin/env python3
"""Moby vertex stage readback: the GPU stage against the CPU projection.

A dump (OPENPETE_MOBY_GSTAGE_AB=1, written beside a screenshot as
<base>-gstage.bin; core/renderer/native_moby_gstage.c
openpete_gsab_dump_write) holds the records of the scene the dump drew, the
CPU projection's outputs per vertex at full precision, and two arms of the
GPU stage (core/rhi/shaders/moby_stage.glsl through moby_rb.vert) on the
same inputs: the stage, and its negative control, whose translation moves
a vertex at the record's centre depth 1/64 output px at 36 output px per
screen px and its depth by 2^-18, and each axis by at least 2^-19 of the
largest translation component. A vertex is paired by its element index, which the stage writes back.

  compare DUMP [--rows R] [--tag T]
      Prints one `row=` line with the counts compared, the stage arm's
      maxima (screen delta in output px at R rows of the internal resolution,
      R / 240 output px per screen px; default 8640, 2160 rows at 4x
      supersampling), the flag census, and the classes not compared pending
      a decision, then one `class=` line per class: the vertices in it, the
      stage arm's mismatches and maxima, and the control arm's mismatches.
      Exit 0 when the stage arm has no mismatch, 1 on a mismatch or an
      unreadable dump, 2 when nothing was compared or no vertex of the dump
      is one the presented scene carries (scene_match 0: VACUOUS).

  grade [--carve CLASS=REASON]... COMPARE_OUTPUT...
      Grades a run per class over the `compare` outputs of every row: FAIL
      on any stage mismatch; per class engaged (compared, and the control
      mismatched on it), blind (compared, the control never mismatched),
      unseen (never compared), carve-out (blind or unseen and named).
      Exit 0 PASS, 1 FAIL, 2 VACUOUS (a blind or unseen class).

A stage mismatch is, outside the divide-overflow regime, a screen or
homogeneous-position delta of 1/256 output px or more, a relative depth
delta (tvz, vz, kvz) above 2^-20, a relative rotated-offset delta above
2^-20, or a regime or saturation flag that differs away from its boundary.
The relative deltas divide by the larger of the CPU value and the near
plane (16 world view-Z; 1 for the rotated offset). A flag that differs
within 2^-16 of its boundary (2 mac3 against H_canon, |mac| against the
int16 range) is counted in the boundary census. Inside the regime the
screen pair is the GTE's capped pair: the regime flag is compared exactly
and the pair's delta is reported as a census. The screen bound holds inside
the viewport window (twice the 4:3 half-extents about the screen centre);
beyond it the screen delta divided by the distance from the centre is held
to 2^-20: the float spacing of a position grows with its magnitude, centred
or not. The cancellation class (|mac3| under 1/16 of the record's largest
translation component) is held to the same bounds; its deltas in float
spacings of that component are reported (max_cancel_ulps).
"""
import struct
import sys

import numpy as np

HDR = struct.Struct("<12I")
REC = np.dtype([("q", "<f4", 32), ("mid", "<u4"), ("first", "<u4"), ("n", "<u4")])
VERT = np.dtype([("inp", "<f4", 6), ("ref", "<f8", 9), ("dz", "<f4", 2), ("fl", "u1", 4)])
F_GPF, F_ANIM, F_LERP, F_MIRROR, F_LIFT, F_SHELL, F_ENV = 1, 2, 4, 8, 16, 32, 64
F_SHADED, F_BILLBOARD, F_SIMPLE, F_SCREEN = 128, 256, 512, 1024
SCREEN_BOUND = 1.0 / 256.0
DEPTH_BOUND = 2.0 ** -20
FLAG_EDGE = 2.0 ** -16
NEAR_W = 16.0
# The viewport window in screen px from the screen centre: twice the 4:3
# frame's half-extents (256 x 120), which holds the widest Hor+ view.
WIN_X, WIN_Y = 512.0, 240.0
# Classes in report order: (name, description).
CLASSES = [
    ("all", "every compared vertex"),
    ("unr", "the GTE divide-overflow regime"),
    ("near", "tvz below the near plane (near-cut corners)"),
    ("sat", "the GTE saturation flag"),
    ("lifted", "a record the near-floor lift moved"),
    ("gpf", "a record with the GPF translation rescale"),
    ("animated", "two keyframes or two poses blend"),
    ("lerp", "the sub-tick previous-to-current pose lerp"),
    ("mirrored", "a rotation with negative determinant"),
    ("shell", "the sphere-shell pull"),
    ("cancel", "projected view-Z under 1/16 of the translation (cancellation)"),
    ("shaded", "a shaded-pass record (static model, W factor 2 or 64 / draw-scale byte)"),
    ("billboard", "a shaded-pass billboard (screen-position offset, canonical view)"),
    ("drawscale", "a shaded-pass record with a draw-scale byte (W factor 64 / byte)"),
    ("simple", "a second-kind record (full-world rotation and translation, W factor 1)"),
    ("simplescreen", "a second-kind record placed in screen space (its own origin, canonical view)"),
]


def load(path):
    """Return (header tuple, records, vertices, texels[2, n, 3, 4]) of a dump."""
    data = open(path, "rb").read()
    if len(data) < HDR.size:
        raise ValueError(f"{path}: short file")
    h = HDR.unpack_from(data, 0)
    if h[0] != 0x4253474D or h[1] != 1:
        raise ValueError(f"{path}: not a gstage dump")
    nrec, nvert = h[2], h[3]
    off = HDR.size
    if nvert == 0:
        return h, np.zeros(0, REC), np.zeros(0, VERT), np.zeros((2, 0, 3, 4), np.uint32)
    rec = np.frombuffer(data, REC, nrec, off)
    off += rec.nbytes
    vert = np.frombuffer(data, VERT, nvert, off)
    off += vert.nbytes
    tex = np.frombuffer(data, "<u4", 2 * 3 * nvert * 4, off).reshape(2, nvert, 3, 4)
    return h, rec, vert, tex


def f32(words):
    """Reinterpret uint32 words as float32, widened to float64."""
    return words.astype(np.uint32).view(np.float32).astype(np.float64)


def per_vertex_record(rec, nvert):
    """Return each vertex's record index."""
    ri = np.zeros(nvert, np.int64)
    for r in range(len(rec)):
        ri[rec["first"][r]:rec["first"][r] + rec["n"][r]] = r
    return ri


def arm_deltas(ref, dz, fl, inst, tex, scale):
    """Return a dict of per-vertex deltas and flag results of one arm."""
    g0, g1, g2 = tex[:, 0, :], tex[:, 1, :], tex[:, 2, :]
    gsx, gsy, gtvz = f32(g0[:, 0]), f32(g0[:, 1]), f32(g0[:, 2])
    gunr, gsat = (g0[:, 3] & 1).astype(bool), ((g0[:, 3] >> 1) & 1).astype(bool)
    gvz, gkvz, ghx, ghy = f32(g1[:, 0]), f32(g1[:, 1]), f32(g1[:, 2]), f32(g1[:, 3])
    ge = np.stack([f32(g2[:, 0]), f32(g2[:, 1]), f32(g2[:, 2])], 1)
    csx, csy, mac3, ctvz = ref[:, 0], ref[:, 1], ref[:, 2], ref[:, 3]
    chx, chy, ce = ref[:, 4], ref[:, 5], ref[:, 6:9]
    cvz, ckvz = dz[:, 0].astype(np.float64), dz[:, 1].astype(np.float64)
    cunr, csat = fl[:, 0].astype(bool), fl[:, 1].astype(bool)
    d = {}
    d["screen"] = np.maximum(np.abs(gsx - csx), np.abs(gsy - csy)) * scale
    den = np.maximum(np.abs(ctvz), NEAR_W)
    d["homog"] = np.maximum(np.abs(ghx - chx), np.abs(ghy - chy)) / den * scale

    def rel(g, c, floor):
        return np.abs(g - c) / np.maximum(np.abs(c), floor)
    d["tvz"] = rel(gtvz, ctvz, NEAR_W)
    d["vz"] = rel(gvz, cvz, NEAR_W)
    d["kvz"] = rel(gkvz, ckvz, NEAR_W)
    d["depth"] = np.maximum(np.maximum(d["tvz"], d["vz"]), d["kvz"])
    emag = np.maximum(np.max(np.abs(ce), 1), 1.0)
    d["e"] = np.max(np.abs(ge - ce), 1) / emag
    ofx, ofy, H, hc = (inst[:, 12 + k].astype(np.float64) for k in range(4))
    unit = inst[:, 17].astype(np.float64)
    edge_unr = (np.abs(2.0 * mac3 - hc) <= FLAG_EDGE * np.maximum(hc, 1.0)) | \
               (np.abs(mac3) <= FLAG_EDGE)
    # mac1 and mac2 from the homogeneous position: h = (of mac3 + mac H) unit.
    mac1 = (chx / unit - ofx * mac3) / H
    mac2 = (chy / unit - ofy * mac3) / H
    mac_edge = np.zeros(len(mac3), bool)
    for m in (mac1, mac2):
        mac_edge |= (np.abs(np.abs(m) - 32767.5) <= FLAG_EDGE * 32768.0)
    d["unr_mis"] = gunr != cunr
    d["sat_mis"] = gsat != csat
    d["flag_boundary"] = (d["unr_mis"] | d["sat_mis"]) & (edge_unr | mac_edge)
    d["flag_mis"] = (d["unr_mis"] | d["sat_mis"]) & ~d["flag_boundary"]
    both_unr = cunr & gunr
    d["unr_pair_mis"] = both_unr & ((gsx != np.float32(csx).astype(np.float64)) |
                                    (gsy != np.float32(csy).astype(np.float64)))
    d["ident_ok"] = g2[:, 3] == np.arange(len(g2), dtype=np.uint32)
    # Off the viewport window the screen position's own float spacing grows
    # with its magnitude; there the delta is graded relative to the distance
    # from the screen centre, the precision a clip-space position keeps.
    off = np.maximum(np.abs(csx - ofx) / WIN_X, np.abs(csy - ofy) / WIN_Y)
    d["inwin"] = ~cunr & (off <= 1.0)
    d["offwin"] = ~cunr & (off > 1.0)
    rad = np.maximum(np.abs(csx - ofx), np.abs(csy - ofy))
    d["off_rel"] = np.where(d["offwin"], d["screen"] / scale / np.maximum(rad, 1.0), 0.0)
    # Cancellation: a vertex whose projected view-Z is small against the
    # record's translation (|mac3| < max|TR| / 16). The stage's compensated
    # sum holds it to the strict bounds; the band is a class, and its deltas
    # in float spacings of the translation are reported as a census.
    trmag = np.max(np.abs(inst[:, [3, 7, 11]].astype(np.float64)), 1)
    d["cancel"] = np.abs(mac3) < trmag / 16.0
    spacing = np.spacing(trmag.astype(np.float32)).astype(np.float64) * unit
    ddepth = np.maximum(np.maximum(np.abs(gtvz - ctvz), np.abs(gvz - cvz)), np.abs(gkvz - ckvz))
    dh = np.maximum(np.abs(ghx - chx), np.abs(ghy - chy)) / H
    d["cancel_ulps"] = np.where(d["cancel"], np.maximum(ddepth, dh) / spacing, 0.0)
    d["mis"] = (d["inwin"] & ((d["screen"] >= SCREEN_BOUND) | (d["homog"] >= SCREEN_BOUND))) | \
               (d["offwin"] & (d["off_rel"] > DEPTH_BOUND)) | \
               (d["depth"] > DEPTH_BOUND) | (d["e"] > DEPTH_BOUND) | d["flag_mis"] | ~d["ident_ok"]
    d["unr"] = cunr
    return d


def class_masks(vert, rec, ri):
    """Return {class name: boolean mask over vertices}."""
    flags = rec["q"][:, 25].view(np.uint32)[ri] if len(rec) else np.zeros(0, np.uint32)
    ref = vert["ref"]
    m = {
        "all": np.ones(len(vert), bool),
        "unr": vert["fl"][:, 0].astype(bool),
        "near": ref[:, 3] < NEAR_W,
        "sat": vert["fl"][:, 1].astype(bool),
        "lifted": (flags & F_LIFT) != 0,
        "gpf": (flags & F_GPF) != 0,
        "animated": (flags & F_ANIM) != 0,
        "lerp": (flags & F_LERP) != 0,
        "mirrored": (flags & F_MIRROR) != 0,
        "shell": (flags & F_SHELL) != 0,
        "cancel": np.abs(ref[:, 2]) < np.max(np.abs(rec["q"][:, [3, 7, 11]].astype(np.float64)), 1)[ri] / 16.0
        if len(rec) else np.zeros(0, bool),
        "shaded": (flags & F_SHADED) != 0,
        "billboard": (flags & F_BILLBOARD) != 0,
        "drawscale": ((flags & F_SHADED) != 0) & ((flags & F_BILLBOARD) == 0) &
        (rec["q"][:, 17][ri] != np.float32(2.0)),
        "simple": (flags & F_SIMPLE) != 0,
        "simplescreen": (flags & F_SCREEN) != 0,
    }
    return m, flags


def fmt(x):
    """Format a float for a report field."""
    return f"{x:.3g}"


def cmd_compare(path, rows, tag):
    """Compare one dump and print its row and class lines."""
    try:
        h, rec, vert, tex = load(path)
    except (OSError, ValueError) as e:
        print(f"row={tag} verdict=FAIL error={e}")
        return 1
    scale = rows / 240.0
    nvert = len(vert)
    match, total, negctl, skipped, known = h[4], h[5], h[6], h[8], h[10]
    if nvert == 0:
        print(f"row={tag} records=0 compared=0 scene_match={match}/{total} verdict=VACUOUS")
        return 2
    ri = per_vertex_record(rec, nvert)
    inst = rec["q"][ri]
    masks, flags = class_masks(vert, rec, ri)
    arms = [arm_deltas(vert["ref"], vert["dz"], vert["fl"], inst, tex[a], scale) for a in (0, 1)]
    s, c = arms
    out = s["inwin"]
    env_v = int(((flags & F_ENV) != 0).sum())
    nmis = int(s["mis"].sum())
    # A dump none of whose vertices the presented scene carries compared a
    # projection, but not the present's: unpaired.
    verdict = "FAIL" if nmis else ("VACUOUS" if match == 0 else "PASS")
    p99 = float(np.percentile(s["screen"][out], 99)) if out.any() else 0.0
    print(f"row={tag} records={len(rec)} compared={nvert} scene_match={match}/{total} "
          f"scene_known={known} skipped={skipped} negctl={negctl} rows={rows} "
          f"max_screen_px={fmt(s['screen'][out].max() if out.any() else 0.0)} p99_screen_px={fmt(p99)} "
          f"max_homog_px={fmt(s['homog'][out].max() if out.any() else 0.0)} "
          f"max_rel_tvz={fmt(s['tvz'].max())} max_rel_vz={fmt(s['vz'].max())} "
          f"max_rel_kvz={fmt(s['kvz'].max())} max_rel_e={fmt(s['e'].max())} "
          f"flag_mismatch={int(s['flag_mis'].sum())} flag_boundary={int(s['flag_boundary'].sum())} "
          f"offwin={int(s['offwin'].sum())} max_offwin_rel={fmt(s['off_rel'].max())} "
          f"unr_pair_census={int(s['unr_pair_mis'].sum())} "
          f"cancel={int(s['cancel'].sum())} max_cancel_ulps={fmt(s['cancel_ulps'].max())} "
          f"max_rel_depth_cancel={fmt(s['depth'][s['cancel']].max() if s['cancel'].any() else 0.0)} "
          f"unr_screen_census_px={fmt(s['screen'][s['unr']].max() if s['unr'].any() else 0.0)} "
          f"ident_bad={int((~s['ident_ok']).sum())} mismatches={nmis} "
          f"ctl_mismatches={int(c['mis'].sum())} "
          f"pending_V313_paint_key_vertices={nvert} pending_V314_env_uv_vertices={env_v} "
          f"verdict={verdict}")
    for name, _desc in CLASSES:
        m = masks[name]
        n = int(m.sum())
        mo = m & out
        smax = s["screen"][mo].max() if mo.any() else 0.0
        print(f"class={name} n={n} mismatches={int((s['mis'] & m).sum())} "
              f"max_screen_px={fmt(smax)} max_rel_depth={fmt(s['depth'][m].max() if n else 0.0)} "
              f"ctl_mismatches={int((c['mis'] & m).sum())}")
    if nmis:
        idx = np.nonzero(s["mis"])[0][:8]
        for i in idx:
            r = ri[i]
            print(f"  mismatch v={i} rec={r} mid={rec['mid'][r]} flags={int(flags[i])} "
                  f"screen={fmt(s['screen'][i])} homog={fmt(s['homog'][i])} depth={fmt(s['depth'][i])} "
                  f"e={fmt(s['e'][i])} unr_cpu={int(s['unr'][i])} flag_mis={int(s['flag_mis'][i])} "
                  f"tvz={fmt(vert['ref'][i, 3])}")
    return 1 if nmis else (2 if match == 0 else 0)


def cmd_grade(carve, files):
    """Grade per class over compare outputs."""
    tot = {n: [0, 0, 0] for n, _ in CLASSES}   # n, mismatches, ctl mismatches
    rows = fails = vac = 0
    for f in files:
        for line in open(f):
            w = dict(kv.split("=", 1) for kv in line.split() if "=" in kv)
            if line.startswith("row="):
                rows += 1
                fails += w.get("verdict") == "FAIL"
                vac += w.get("verdict") == "VACUOUS"
            elif line.startswith("class=") and w["class"] in tot:
                t = tot[w["class"]]
                t[0] += int(w["n"]); t[1] += int(w["mismatches"]); t[2] += int(w["ctl_mismatches"])
    blind = 0
    for name, desc in CLASSES:
        n, mis, ctl = tot[name]
        if mis:
            v = "FAIL"
        elif n and ctl:
            v = "engaged"
        else:
            v = "blind" if n else "unseen"
            if name in carve:
                v = f"carve-out ({carve[name]})"
            else:
                blind += 1
        print(f"[gstage] class={name} n={n} mismatches={mis} ctl_mismatches={ctl} verdict={v}  # {desc}")
    any_mis = any(t[1] for t in tot.values()) or fails
    verdict = "FAIL" if any_mis else ("VACUOUS" if blind or vac or rows == 0 else "PASS")
    print(f"[gstage] rows={rows} failed_rows={fails} vacuous_rows={vac} classes={len(CLASSES)} "
          f"unengaged={blind} verdict={verdict}")
    return 1 if verdict == "FAIL" else (2 if verdict == "VACUOUS" else 0)


def main(argv):
    """Dispatch the subcommands."""
    if len(argv) < 2 or argv[1] not in ("compare", "grade"):
        print(__doc__)
        return 1
    if argv[1] == "compare":
        rows, tag, path = 8640, "", None
        it = iter(argv[2:])
        for a in it:
            if a == "--rows":
                rows = int(next(it))
            elif a == "--tag":
                tag = next(it)
            else:
                path = a
        return cmd_compare(path, rows, tag or path)
    carve, files = {}, []
    it = iter(argv[2:])
    for a in it:
        if a == "--carve":
            k, _, r = next(it).partition("=")
            carve[k] = r
        else:
            files.append(a)
    return cmd_grade(carve, files)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
