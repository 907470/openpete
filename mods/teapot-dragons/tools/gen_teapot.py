#!/usr/bin/env python3
"""gen_teapot.py — tessellate the Newell teapot into an S1 animated Model
blob and write assets/teapot_s1pack.bin for the teapot-dragons mod.

The model is the densest a single S1 moby model can be:
  * the animation header's vertex count is one byte per tier (HP at +8,
    LP at +9), so at most 255 vertices;
  * face words carry 9-bit vertex indices (byte offset & 0x7FC), so 512
    would be addressable, but the count byte is the binding limit;
  * vertex literals are 11/11/12-bit (x, z in [-1024, 1023]; y even in
    [-4096, 4094]) — the teapot is scaled to fit x/z and stands on y=0
    growing toward negative y, the game's up.

Patches are subdivided greedily by surface area until the vertex budget
is spent. Vertices are welded across patch seams. Vertex order puts the
patch-corner vertices first so the LP tier (one quad per patch, count =
number of corner vertices) is a prefix of the same stream, which is how
the game reads the two tiers: same stream, tier-selected count.

Colours: one table entry per vertex, a two-sided directional shade baked
into the base colour; the far (LP) palette is the same at half intensity,
as the game's own LP colour tables are.

Faces are gouraud, untextured, double-sided (w0 bit 0), fresh-face
records (12 bytes). Pack layout (little-endian):

  header: magic 'TPOT' u32 | version u32 | payload_size u32 |
          n_reloc_words u32 | n_reloc_frames u32 | model_off u32 |
          hp_verts u32 | hp_faces u32 | lp_verts u32 | lp_faces u32
  reloc_words:  n × u32 payload offsets of absolute-vaddr words
                (linked at base 0: add the park vaddr)
  reloc_frames: n × u32 payload offsets of frame words whose 21-bit
                field holds a stream offset (exchange for a handle)
  payload
"""
import argparse
import math
import struct
from pathlib import Path

HERE = Path(__file__).resolve().parent
MOD = HERE.parent

MAGIC = 0x544F5054  # 'TPOT'
VERSION = 1

# ---------------------------------------------------------------- patches

def load_bpt(path):
    toks = path.read_text().split()
    n = int(toks[0]); i = 1
    patches = []
    for _ in range(n):
        du, dv = int(toks[i]), int(toks[i + 1]); i += 2
        assert du == 3 and dv == 3
        cp = []
        for _ in range(16):
            cp.append((float(toks[i]), float(toks[i + 1]), float(toks[i + 2])))
            i += 3
        patches.append(cp)
    return patches


def bern(t):
    s = 1.0 - t
    return (s * s * s, 3 * s * s * t, 3 * s * t * t, t * t * t)


def dbern(t):
    s = 1.0 - t
    return (-3 * s * s, 3 * s * s - 6 * s * t, 6 * s * t - 3 * t * t, 3 * t * t)


def eval_patch(cp, u, v):
    bu, bv = bern(u), bern(v)
    dbu, dbv = dbern(u), dbern(v)
    p = [0.0, 0.0, 0.0]; du = [0.0, 0.0, 0.0]; dv = [0.0, 0.0, 0.0]
    for i in range(4):
        for j in range(4):
            c = cp[i * 4 + j]
            w, wu, wv = bu[i] * bv[j], dbu[i] * bv[j], bu[i] * dbv[j]
            for k in range(3):
                p[k] += w * c[k]; du[k] += wu * c[k]; dv[k] += wv * c[k]
    return tuple(p), tuple(du), tuple(dv)


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def norm(a):
    l = math.sqrt(sum(x * x for x in a))
    return (a[0] / l, a[1] / l, a[2] / l) if l > 1e-9 else (0.0, 0.0, 1.0)


def patch_area(cp, n=8):
    a = 0.0
    for i in range(n):
        for j in range(n):
            _, du, dv = eval_patch(cp, (i + 0.5) / n, (j + 0.5) / n)
            a += math.sqrt(sum(x * x for x in cross(du, dv))) / (n * n)
    return a


# --------------------------------------------------------------- welding

class Welder:
    """Weld tessellated vertices across patch seams. Keyed on the rounded
    teapot-space position; the seam vertices of adjacent patches evaluate
    to identical control points so this is exact in practice."""
    def __init__(self):
        self.index = {}
        self.pos = []
        self.nrm = []

    def add(self, p, n):
        key = tuple(round(x, 4) for x in p)
        k = self.index.get(key)
        if k is None:
            k = len(self.pos)
            self.index[key] = k
            self.pos.append(p)
            self.nrm.append([0.0, 0.0, 0.0])
        # accumulate |normal| direction-agnostically: the Newell patches
        # are not consistently oriented, so average the unsigned normal
        # by flipping into the hemisphere of the running sum
        acc = self.nrm[k]
        if acc[0] * n[0] + acc[1] * n[1] + acc[2] * n[2] < 0:
            n = (-n[0], -n[1], -n[2])
        for i in range(3):
            acc[i] += n[i]
        return k


def tessellate(patches, budget, lp_first=True):
    """Greedy subdivision by area under a welded-vertex budget."""
    areas = [patch_area(cp) for cp in patches]
    subdiv = [1] * len(patches)

    def count(subdiv):
        w = Welder()
        for cp, n in zip(patches, subdiv):
            for i in range(n + 1):
                for j in range(n + 1):
                    p, du, dv = eval_patch(cp, i / n, j / n)
                    w.add(p, norm(cross(du, dv)))
        return len(w.pos)

    while True:
        # candidate: the patch with the largest area per current cell
        best = max(range(len(patches)),
                   key=lambda k: areas[k] / (subdiv[k] * subdiv[k]))
        trial = list(subdiv); trial[best] += 1
        if count(trial) > budget:
            # try every other patch once before giving up
            grew = False
            for k in sorted(range(len(patches)),
                            key=lambda k: -areas[k] / (subdiv[k] * subdiv[k])):
                t2 = list(subdiv); t2[k] += 1
                if count(t2) <= budget:
                    subdiv = t2; grew = True; break
            if not grew:
                break
        else:
            subdiv = trial

    # final build: corner vertices first (the LP prefix), then the rest
    w = Welder()
    corner_of = []
    for cp in patches:
        ids = []
        for (u, v) in ((0, 0), (1, 0), (0, 1), (1, 1)):
            p, du, dv = eval_patch(cp, u, v)
            ids.append(w.add(p, norm(cross(du, dv))))
        corner_of.append(ids)
    lp_count = len(w.pos)
    hp_faces = []
    for cp, n in zip(patches, subdiv):
        grid = []
        for i in range(n + 1):
            row = []
            for j in range(n + 1):
                p, du, dv = eval_patch(cp, i / n, j / n)
                row.append(w.add(p, norm(cross(du, dv))))
            grid.append(row)
        for i in range(n):
            for j in range(n):
                a, b = grid[i][j], grid[i + 1][j]
                c, d = grid[i][j + 1], grid[i + 1][j + 1]
                if len({a, b, c, d}) < 3:
                    continue           # collapsed cell (patch poles)
                hp_faces.append((a, b, c, d))
    lp_faces = []
    for ids in corner_of:
        a, b, c, d = ids
        if len({a, b, c, d}) >= 3:
            lp_faces.append((a, b, c, d))
    return w, subdiv, lp_count, hp_faces, lp_faces


# ---------------------------------------------------------------- packing

def pack_vert(vx, vy, vz):
    """One literal stream word (bit-exact mirror of the renderer's decode):
    VZ = s11 @21..31, VX = s11 @11..21, VY = s12 @0..11 (*2). Bits 21 and
    11 are shared between fields, so VZ's LSB must equal VX's sign and
    VX's LSB must equal VY's sign; ±1 nudges satisfy that. Bit 0 is the
    stream flag (0 = next vertex is a literal)."""
    vyf = int(round(vy / 2.0)) & ~1
    vyf = max(-2048, min(2046, vyf))
    sy = 1 if vyf < 0 else 0
    if (vx & 1) != sy:
        vx += -1 if (vx > 0 or vx <= -1023) else 1
    sx = 1 if vx < 0 else 0
    if (vz & 1) != sx:
        vz += -1 if (vz > 0 or vz <= -1023) else 1
    assert -1024 <= vx <= 1023 and -1024 <= vz <= 1023, (vx, vz)
    w = ((vz & 0x7FF) << 21) | ((vx & 0x7FF) << 11) | (vyf & 0xFFF)
    assert decode_vert(w) == (vx, vyf * 2, vz)
    return w


def _s32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def decode_vert(w):
    return (_s32((w << 10) & 0xFFFFFFFF) >> 21,
            _s32((w << 20) & 0xFFFFFFFF) >> 19,
            _s32(w) >> 21)


def face_words(i, c, double_sided):
    """Fresh gouraud face: w0 = 0x80000000 | i0<<20 | i1<<11 | i2<<2 (byte
    offsets = index*4), w1 = c0<<17 | c1<<8 | c2>>1, w2 = i3 | c3<<9."""
    i0b, i1b, i2b, i3b = [ix * 4 for ix in i]
    c0b, c1b, c2b, c3b = [ci * 4 for ci in c]
    assert max(i0b, i1b, i2b, i3b) <= 0x7FC and max(c0b, c1b, c2b, c3b) <= 0x7FC
    w0 = 0x80000000 | (i0b << 20) | (i1b << 11) | (i2b << 2)
    if double_sided:
        w0 |= 1
    w1 = (c0b << 17) | (c1b << 8) | (c2b >> 1)
    w2 = i3b | (c3b << 9)
    return w0, w1, w2


def build(args):
    patches = load_bpt(HERE / "teapot.bpt")
    w, subdiv, lp_count, hp_faces, lp_faces = tessellate(patches, args.budget)
    nverts = len(w.pos)
    assert nverts <= 255, nverts

    # teapot space: z up, [x,y] the ground plane. World: y up is NEGATIVE,
    # base on y = 0; centre x/z on the bbox.
    xs = [p[0] for p in w.pos]; ys = [p[1] for p in w.pos]; zs = [p[2] for p in w.pos]
    cx, cy = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
    zmin = min(zs)
    half_w = max(max(xs) - cx, cx - min(xs), max(ys) - cy, cy - min(ys))
    height = max(zs) - zmin
    scale = min(args.height / height, args.halfwidth / half_w)
    # yaw about the vertical axis: the statue faces along the moby's local
    # axes and an unrotated teapot points its spout at the player
    ca, sa = math.cos(math.radians(args.yaw)), math.sin(math.radians(args.yaw))
    verts = []
    for p in w.pos:
        gx, gz = (p[0] - cx) * scale, (p[1] - cy) * scale
        vx = int(round(gx * ca - gz * sa))
        vz = int(round(gx * sa + gz * ca))
        vy = -int(round((p[2] - zmin) * scale))
        verts.append((vx, vy, vz))

    # baked two-sided directional shade, one colour per vertex
    light = norm((-0.4, -0.5, 0.75))
    base = (args.r, args.g, args.b)
    cols = []
    for n in w.nrm:
        nn = norm(n)
        d = abs(nn[0] * light[0] + nn[1] * light[1] + nn[2] * light[2])
        k = 0.35 + 0.65 * d
        r, g, b = [min(255, int(round(c * k))) for c in base]
        cols.append(r | (g << 8) | (b << 16))
    far = [(((c >> 16) & 0xFF) // 2) << 16 | (((c >> 8) & 0xFF) // 2) << 8 | ((c & 0xFF) // 2)
           for c in cols]

    # strip order: game quad (A,B,C,D) draws tris ABC + BDC (shared B-C);
    # our grid cell (a,b,c,d) = (i,j),(i+1,j),(i,j+1),(i+1,j+1): A=a B=b C=c D=d
    def frecs(faces):
        out = []
        for (a, b, c, d) in faces:
            if a == b or a == c:
                i = [b, d, c, c]        # degenerate at a: single tri
            elif d == b or d == c:
                i = [a, b, c, c]
            else:
                i = [a, b, c, d]
            out.append((i, list(i)))    # colour index == vertex index
        return out
    hp = frecs(hp_faces); lp = frecs(lp_faces)
    # stress knob: repeat the HP face block (same vertices) to probe the
    # face-side limits — the game's polygon buffer and the walks' caps
    hp = hp * max(1, args.dup_faces)

    # layout: model hdr 0x3C | anim hdr 0x2C (1 frame) | cols | far |
    #         hp faceblock | lp faceblock | vertex stream
    ncol = nverts
    off_anim = 0x3C
    off_cols = off_anim + 0x2C
    off_far = off_cols + 4 * ncol
    off_hpf = off_far + 4 * ncol
    off_lpf = off_hpf + 4 + 12 * len(hp)
    off_verts = off_lpf + 4 + 12 * len(lp)
    total = off_verts + 4 * nverts
    blob = bytearray(total)
    reloc_words, reloc_frames = [], []

    struct.pack_into("<i", blob, 0, 1)                    # numAnims
    # m_Sounds[16], m_CollisionModels[8]: copied from the stock model at
    # install time (both are per-level)
    struct.pack_into("<I", blob, 0x34, 0)                 # m_Data
    struct.pack_into("<I", blob, 0x38, off_anim); reloc_words.append(0x38)

    struct.pack_into("<hH", blob, off_anim, 1, ncol)      # frames, colours
    blob[off_anim + 4] = 0                                # not a Spyro anim
    blob[off_anim + 5] = args.scale                       # m_Scale (stock 0)
    blob[off_anim + 6] = 0                                # short shift (all literal)
    blob[off_anim + 7] = args.radius
    blob[off_anim + 8] = nverts                           # HP count
    blob[off_anim + 9] = lp_count                         # LP count (stream prefix)
    blob[off_anim + 0xB] = args.depth
    blob[off_anim + 0xC] = 0
    struct.pack_into("<I", blob, off_anim + 0x10, 0)
    struct.pack_into("<I", blob, off_anim + 0x14, off_hpf)
    struct.pack_into("<I", blob, off_anim + 0x18, off_cols)
    struct.pack_into("<I", blob, off_anim + 0x1C, off_lpf)
    struct.pack_into("<I", blob, off_anim + 0x20, off_far)
    reloc_words += [off_anim + 0x14, off_anim + 0x18, off_anim + 0x1C, off_anim + 0x20]
    f0 = off_anim + 0x24
    struct.pack_into("<I", blob, f0, (off_verts & 0x1FFFFF) | (0xFF << 24))
    reloc_frames.append(f0)
    struct.pack_into("<H", blob, f0 + 4, 0)
    blob[f0 + 6] = args.shadow
    blob[f0 + 7] = nverts                                 # short offset: all literal

    for k, c in enumerate(cols):
        struct.pack_into("<I", blob, off_cols + 4 * k, c)
    for k, c in enumerate(far):
        struct.pack_into("<I", blob, off_far + 4 * k, c)
    for off, recs in ((off_hpf, hp), (off_lpf, lp)):
        struct.pack_into("<I", blob, off, 12 * len(recs))
        fp = off + 4
        for i, c in recs:
            struct.pack_into("<3I", blob, fp, *face_words(i, c, not args.single_sided))
            fp += 12
    for k, (vx, vy, vz) in enumerate(verts):
        struct.pack_into("<I", blob, off_verts + 4 * k, pack_vert(vx, vy, vz))

    hdr = struct.pack("<10I", MAGIC, VERSION, total, len(reloc_words), len(reloc_frames),
                      0, nverts, len(hp), lp_count, len(lp))
    out = hdr + b"".join(struct.pack("<I", o) for o in reloc_words) \
              + b"".join(struct.pack("<I", o) for o in reloc_frames) + bytes(blob)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(out)

    ext = [(min(v[i] for v in verts), max(v[i] for v in verts)) for i in range(3)]
    print(f"teapot: {nverts} verts (LP prefix {lp_count}), {len(hp)} HP faces, "
          f"{len(lp)} LP faces, subdiv {min(subdiv)}..{max(subdiv)} "
          f"(cells {sum(n * n for n in subdiv)}), scale {scale:.2f} u/teapot-unit, "
          f"ext x{ext[0]} y{ext[1]} z{ext[2]}, payload {total} B -> {args.out}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--budget", type=int, default=255,
                    help="vertex budget (the format's count byte caps it at 255)")
    ap.add_argument("--height", type=float, default=1400.0,
                    help="teapot height in world units (stock statue ~1640)")
    ap.add_argument("--halfwidth", type=float, default=1000.0,
                    help="max |x|,|z| in world units (literal field limit 1023)")
    ap.add_argument("--yaw", type=float, default=90.0,
                    help="rotation about the vertical axis, degrees (spout sideways at 90)")
    ap.add_argument("--scale", type=int, default=0, help="AnimationHeader m_Scale (stock 0)")
    ap.add_argument("--radius", type=int, default=28, help="AnimationHeader m_Radius (stock 28)")
    ap.add_argument("--depth", type=int, default=2, help="AnimationHeader m_DepthScale (stock 2)")
    ap.add_argument("--shadow", type=int, default=68, help="frame shadow byte (stock 68)")
    ap.add_argument("--r", type=int, default=210); ap.add_argument("--g", type=int, default=225)
    ap.add_argument("--b", type=int, default=255)
    ap.add_argument("--dup-faces", type=int, default=1,
                    help="STRESS: emit the HP face block N times over the same vertices")
    ap.add_argument("--single-sided", action="store_true",
                    help="omit the double-sided face flag (winding is NOT made consistent)")
    ap.add_argument("--out", type=Path, default=MOD / "assets" / "teapot_s1pack.bin")
    args = ap.parse_args()
    # A stress pack never lands on the shipped asset path: the mod loads
    # whatever sits at assets/teapot_s1pack.bin, and a face count past the
    # polygon buffer hangs the game.
    if args.dup_faces > 1 and args.out == MOD / "assets" / "teapot_s1pack.bin":
        args.out = MOD / "assets" / f"teapot_s1pack.stress{args.dup_faces}.bin"
    build(args)


if __name__ == "__main__":
    main()
