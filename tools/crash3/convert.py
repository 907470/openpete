#!/usr/bin/env python3
"""convert.py - Crash 3 demo models -> a Spyro 1 animated-moby-model pack.

Reads the demo NSFs off the disc (nsf.py), poses every model on one
animation frame (tgeo.py), and encodes each as a complete Spyro 1
`Model` blob (one animation, one frame, all-literal vertex stream) into
`crash3_s1pack.bin`, with the textures baked into mod-texture atlases
(`textures/own/<hexid>.png`).

Per model:
  1. CrashEdit viewer space (x, y-up, z) -> S1 moby space (-x, -y, z),
     scaled so the model stands `--height` world units tall on y = 0;
     x/z clamped to the literal field (±1023). The x flip is the
     handedness difference between the two spaces: without it the
     checkpoint crate's "C" reads mirrored in Spyro (the S3 animated-
     model port hit the same mirror). `--yaw` then rotates about the
     vertical axis: a moby draws along its own local axes, and a Crash
     model faces +x where the statue's pedestal expects -y.
  2. Unreferenced vertices dropped; if still over `--budget` (255, the
     header's one-byte tier count), shortest-edge collapse until it fits.
  3. The same collapse run further gives the LP tier (≤ `--lp-faces`
     faces); its surviving vertices are moved to the FRONT of the stream
     so the LP tier is a prefix, which is how the game selects tiers.
  4. Faces oriented for a single-sided target (BFS across manifold
     edges, per-component outward side by signed volume); Crash's
     no-cull faces and open-sheet boundary faces get the S1 double-sided
     flag (w0 bit 0).
  5. Textured faces reference an atlas tile; the CLUT u16 carries the
     atlas id (tpage mode 3), Crash blend 0/1 sets the face's ABE bit
     (w1 bit 0) with ABR 0/1, blend 2 (subtract) has no native mod-
     texture pipe and is emitted opaque with a warning.
  6. Layout: model hdr (0x38 + 4 × anim_slots) | anim hdr (0x24 + 8 ×
     frame_slots) | colours | far colours (halved: the HP distance-fade
     target) | HP faceblock | LP faceblock | vertex stream. Every anim
     slot points at the one anim header and every frame slot holds the
     same frame word, so a mod can raise numAnims / nframes to whatever
     the stock model it replaces declares and any (anim, frame) a live
     moby sits on resolves to this pose.

Pack (little-endian), `CR3P` v1:
  header  : magic u32 | version u32 | n_models u32 | table_off u32 |
            payload_off u32 | payload_size u32 | anim_slots u32 |
            frame_slots u32
  table   : n_models × 52 B {eid u32, name[8], blob_off u32, blob_size u32,
            hp_verts u16, lp_verts u16, hp_faces u16, lp_faces u16,
            src_verts u16, pose_frame u16, pose_anim u32,
            reloc_words_off u32, n_reloc_words u32,
            reloc_frames_off u32, n_reloc_frames u32}
  relocs  : u32 offsets RELATIVE TO THE MODEL BLOB - absolute-vaddr words
            (add the park vaddr) and frame words whose 21-bit field is a
            stream offset (exchange for a stream handle)
  payload : model blobs, 16-aligned, linked at base 0
"""
import argparse
import math
import struct
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from nsf import Library, load_demo, eid_to_name          # noqa: E402
from tgeo import decode_pose, default_pose, decode_tile  # noqa: E402

PACK_MAGIC = 0x50335243      # 'CR3P'
PACK_VERSION = 1
ATLAS_ID_BASE = 0xC300
ATLAS_SIZE = 256             # UV bytes: coordinates must stay ≤ 255
TPAGE_MODE3 = 3 << 7
MODEL_HDR = 0x38
TABLE_ROW = 52               # struct.calcsize('<I8sIIHHHHHHIIIII')
ANIM_HDR = 0x24


# ------------------------------------------------------------ packing

def _s32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def decode_vert(w):
    return (_s32((w << 10) & 0xFFFFFFFF) >> 21,
            _s32((w << 20) & 0xFFFFFFFF) >> 19,
            _s32(w) >> 21)


def pack_vert(vx, vy, vz):
    """Literal stream word: VZ s11 @21, VX s11 @11, VY s12 @0 (×2). Bits
    21 and 11 are shared between fields, so VZ's LSB must equal VX's sign
    and VX's LSB must equal VY's sign; ±1 nudges satisfy that. Bit 0 is
    the stream flag (0 = the next vertex is a literal)."""
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


def face_words(i, c, double_sided, textured, abe):
    """Fresh face: w0 = 0x80000000 | i0<<20 | i1<<11 | i2<<2 | tex<<1 |
    dbl, w1 = c0<<17 | c1<<8 | c2>>1 | abe, w2 = i3 | c3<<9; the i/c
    fields are byte offsets (index × 4)."""
    i0b, i1b, i2b, i3b = [ix * 4 for ix in i]
    c0b, c1b, c2b, c3b = [ci * 4 for ci in c]
    assert max(i0b, i1b, i2b, i3b) <= 0x7FC and max(c0b, c1b, c2b, c3b) <= 0x7FC
    w0 = 0x80000000 | (i0b << 20) | (i1b << 11) | (i2b << 2)
    if textured:
        w0 |= 2
    if double_sided:
        w0 |= 1
    w1 = (c0b << 17) | (c1b << 8) | (c2b >> 1) | (1 if abe else 0)
    w2 = i3b | (c3b << 9)
    return w0, w1, w2


# ----------------------------------------------------------- decimation

def collapse(verts, tris, target_verts=None, target_tris=None):
    """Shortest-edge collapse. verts: list of (x,y,z); tris: list of dicts
    with v[3] (and tex/rgb per corner, carried along). Returns (alive
    vertex indices in original numbering, tris with original indices).
    Collapsing an edge (a, b) moves b onto a; the survivor keeps a's
    position; corners referencing b now reference a but keep their own
    colour/UV, so textures do not smear across a seam."""
    tris = [dict(t) for t in tris]
    remap = list(range(len(verts)))

    def find(i):
        while remap[i] != i:
            remap[i] = remap[remap[i]]
            i = remap[i]
        return i

    def live_tris():
        out = []
        for t in tris:
            v = [find(i) for i in t["v"]]
            if len(set(v)) == 3:
                out.append(t)
        return out

    def n_alive():
        return len({find(i) for t in live_tris() for i in t["v"]})

    while True:
        cur = live_tris()
        na = n_alive()
        if (target_verts is None or na <= target_verts) and \
           (target_tris is None or len(cur) <= target_tris):
            break
        # tile/colour agreement per vertex: collapsing across a texture
        # boundary is penalised so seams survive longest
        tile_of = {}
        for t in cur:
            key = id(t["tex"]["rec"]) if t["tex"] else None
            for i in t["v"]:
                tile_of.setdefault(find(i), set()).add(key)
        best = None
        for t in cur:
            v = [find(i) for i in t["v"]]
            for k in range(3):
                a, b = v[k], v[(k + 1) % 3]
                if a == b:
                    continue
                d = math.dist(verts[a], verts[b])
                if tile_of[a] != tile_of[b]:
                    d *= 4.0
                if best is None or d < best[0]:
                    best = (d, a, b)
        if best is None:
            break
        _, a, b = best
        remap[b] = a
    cur = live_tris()
    for t in cur:
        t["v"] = [find(i) for i in t["v"]]
    alive = sorted({i for t in cur for i in t["v"]})
    return alive, cur


# ---------------------------------------------------------- orientation

def orient(tris, verts):
    """Per-face reverse flags for a single-sided target: BFS-propagate a
    consistent orientation across manifold edges, then pick each
    component's outward side by signed volume (centroid vote for open
    sheets). Faces on boundary edges are reported so they can draw
    double-sided. verts are S1-space positions."""
    loops = [list(t["v"]) for t in tris]
    edge_users = {}
    for fi, lp in enumerate(loops):
        for k in range(3):
            a, b = lp[k], lp[(k + 1) % 3]
            edge_users.setdefault(frozenset((a, b)), []).append((fi, (a, b)))
    adj = [[] for _ in tris]
    for users in edge_users.values():
        if len(users) != 2:
            continue
        (f1, d1), (f2, d2) = users
        same = d1 == d2
        adj[f1].append((f2, same))
        adj[f2].append((f1, same))
    mc = [sum(v[k] for v in verts) / float(len(verts)) for k in range(3)]
    rev = [False] * len(tris)
    seen = [False] * len(tris)
    conflicts = ncomp = 0
    for seed in range(len(tris)):
        if seen[seed]:
            continue
        ncomp += 1
        comp = [seed]
        seen[seed] = True
        qi = 0
        while qi < len(comp):
            f = comp[qi]; qi += 1
            for g, same in adj[f]:
                want = rev[f] ^ same
                if not seen[g]:
                    seen[g] = True; rev[g] = want; comp.append(g)
                elif rev[g] != want:
                    conflicts += 1
        svol = avol = vote = 0.0
        for f in comp:
            lp = loops[f][::-1] if rev[f] else loops[f]
            A, B, C = verts[lp[0]], verts[lp[1]], verts[lp[2]]
            vol = (A[0] * (B[1] * C[2] - B[2] * C[1])
                   - A[1] * (B[0] * C[2] - B[2] * C[0])
                   + A[2] * (B[0] * C[1] - B[1] * C[0]))
            svol += vol; avol += abs(vol)
            ab = [B[k] - A[k] for k in range(3)]
            ac = [C[k] - A[k] for k in range(3)]
            n = [ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2],
                 ab[0] * ac[1] - ab[1] * ac[0]]
            fc = [(A[k] + B[k] + C[k]) / 3.0 - mc[k] for k in range(3)]
            vote += sum(n[k] * fc[k] for k in range(3))
        closed = avol and abs(svol) / avol >= 0.10
        if (svol if closed else vote) < 0:
            for f in comp:
                rev[f] = not rev[f]
    boundary = [False] * len(tris)
    for users in edge_users.values():
        if len(users) == 1:
            boundary[users[0][0]] = True
    return rev, boundary, dict(components=ncomp, conflicts=conflicts,
                               reversed=sum(rev), boundary=sum(boundary))


# ---------------------------------------------------------------- atlas

class Atlas:
    """Shelf-packed 256×256 5551 tiles, one mod-texture id per sheet."""

    def __init__(self, lib):
        self.lib = lib
        self.tiles = {}       # key -> dict(px=5551 array, sheet, x, y, w, h)
        self.sheets = []      # list of (h, w) uint16 arrays
        self.cursor = []      # per sheet: [x, y, shelf_h]

    def tile(self, tex):
        rec, page = tex["rec"], tex["page"]
        key = rec.key(page)
        t = self.tiles.get(key)
        if t is None:
            px, (x0, y0) = decode_tile(self.lib.pages[page], rec)
            h, w = px.shape
            if w > ATLAS_SIZE - 1 or h > ATLAS_SIZE - 1:
                raise SystemExit(f"tile {w}x{h} exceeds one atlas")
            t = dict(px=px, x0=x0, y0=y0, w=w, h=h, sheet=None, x=0, y=0,
                     blend=rec.blend)
            self.tiles[key] = t
        return t

    def pack(self):
        order = sorted(self.tiles.values(), key=lambda t: (-t["h"], -t["w"]))
        for t in order:
            placed = False
            for si, cur in enumerate(self.cursor):
                x, y, sh = cur
                if x + t["w"] > ATLAS_SIZE - 1:
                    x, y, sh = 0, y + sh, 0
                if y + t["h"] <= ATLAS_SIZE - 1:
                    self._place(si, t, x, y)
                    self.cursor[si] = [x + t["w"], y, max(sh, t["h"])]
                    placed = True
                    break
            if not placed:
                self.sheets.append(np.zeros((ATLAS_SIZE, ATLAS_SIZE), dtype=np.uint16))
                self.cursor.append([0, 0, 0])
                si = len(self.sheets) - 1
                self._place(si, t, 0, 0)
                self.cursor[si] = [t["w"], 0, t["h"]]

    def _place(self, si, t, x, y):
        self.sheets[si][y:y + t["h"], x:x + t["w"]] = t["px"]
        t["sheet"], t["x"], t["y"] = si, x, y

    def write_pngs(self, out_dir):
        from PIL import Image
        out_dir.mkdir(parents=True, exist_ok=True)
        for si, sh in enumerate(self.sheets):
            rgba = np.zeros((ATLAS_SIZE, ATLAS_SIZE, 4), dtype=np.uint8)
            rgba[..., 0] = (sh & 0x1F) * 255 // 31
            rgba[..., 1] = ((sh >> 5) & 0x1F) * 255 // 31
            rgba[..., 2] = ((sh >> 10) & 0x1F) * 255 // 31
            rgba[..., 3] = np.where(sh != 0, 255, 0)   # 0x0000 = transparent
            Image.fromarray(rgba, "RGBA").save(out_dir / f"{ATLAS_ID_BASE + si:04x}.png")


# -------------------------------------------------------------- encoder

class Encoded:
    pass


def encode_model(mesh, atlas, args, report):
    name = mesh.name
    # --- 1. S1 space: (-x, -y, z), stand on y = 0, fit height ------------
    used = sorted({i for t in mesh.tris for i in t["v"]})
    src = [mesh.verts[i] for i in used]
    ymin = min(p[1] for p in src); ymax = max(p[1] for p in src)
    xs = [p[0] for p in src]; zs = [p[2] for p in src]
    cx = (min(xs) + max(xs)) / 2; cz = (min(zs) + max(zs)) / 2
    height = max(ymax - ymin, 1e-6)
    half = max(max(xs) - cx, cx - min(xs), max(zs) - cz, cz - min(zs), 1e-6)
    k = min(args.height / height, 1000.0 / half, 4090.0 / height)
    index_of = {old: new for new, old in enumerate(used)}
    ca, sa = math.cos(math.radians(args.yaw)), math.sin(math.radians(args.yaw))
    verts = []
    for p in src:
        gx, gz = -(p[0] - cx) * k, (p[2] - cz) * k
        verts.append((gx * ca - gz * sa, -(p[1] - ymin) * k, gx * sa + gz * ca))
    tris = [dict(v=[index_of[i] for i in t["v"]], rgb=t["rgb"], tex=t["tex"],
                 nocull=t["nocull"]) for t in mesh.tris]
    src_verts = len(verts)

    # --- 2. HP decimation to the tier count byte ------------------------
    alive, hp_tris = collapse(verts, tris, target_verts=args.budget)
    # --- 3. LP tier: continue collapsing, survivors become the prefix ---
    lp_alive, lp_tris = collapse(verts, hp_tris, target_tris=args.lp_faces)
    lp_set = set(lp_alive)
    order = [i for i in alive if i in lp_set] + [i for i in alive if i not in lp_set]
    new_of = {old: new for new, old in enumerate(order)}
    hp_faces = [dict(t, v=[new_of[i] for i in t["v"]]) for t in hp_tris]
    lp_faces = [dict(t, v=[new_of[i] for i in t["v"]]) for t in lp_tris]
    sverts = [verts[i] for i in order]
    n_lp = len(lp_alive)
    assert len(sverts) <= args.budget

    # --- 4. orientation -------------------------------------------------
    def orient_set(faces):
        rev, boundary, rep = orient(faces, sverts)
        for t, r, b in zip(faces, rev, boundary):
            if r:
                t["v"] = [t["v"][0], t["v"][2], t["v"][1]]
                t["rgb"] = [t["rgb"][0], t["rgb"][2], t["rgb"][1]]
                if t["tex"]:
                    xy = t["tex"]["xy"]
                    t["tex"] = dict(t["tex"], xy=[xy[0], xy[2], xy[1]])
            t["dbl"] = args.double_sided or t["nocull"] or b
        return rep
    rep_hp = orient_set(hp_faces)
    orient_set(lp_faces)

    # --- 5. colour table --------------------------------------------------
    cols = []
    col_index = {}

    def cidx(rgb):
        rgb = tuple(int(c) for c in rgb)
        j = col_index.get(rgb)
        if j is None:
            j = len(cols); col_index[rgb] = j; cols.append(rgb)
        return j
    for t in hp_faces + lp_faces:
        t["c"] = [cidx(c) for c in t["rgb"]]
    assert len(cols) <= 511, len(cols)

    # --- 6. records -------------------------------------------------------
    n_blend_warn = 0

    def records(faces):
        out = []
        for t in faces:
            i = [t["v"][0], t["v"][1], t["v"][2], t["v"][2]]
            c = [t["c"][0], t["c"][1], t["c"][2], t["c"][2]]
            tex_words = None
            abe = False
            if t["tex"]:
                tile = atlas.tile(t["tex"])
                uv = [(tile["x"] + (x - tile["x0"]), tile["y"] + (y - tile["y0"]))
                      for (x, y) in t["tex"]["xy"]]
                uv.append(uv[2])
                for u, v in uv:
                    assert 0 <= u <= 255 and 0 <= v <= 255, (name, u, v)
                blend = tile["blend"]
                abr = 0
                if blend in (0, 1):
                    abe, abr = True, blend
                elif blend == 2:
                    nonlocal n_blend_warn
                    n_blend_warn += 1
                clut = ATLAS_ID_BASE + tile["sheet"]
                tpage = TPAGE_MODE3 | (abr << 5)
                tex_words = ((uv[0][0] | uv[0][1] << 8 | clut << 16),
                             (uv[1][0] | uv[1][1] << 8 | tpage << 16),
                             (uv[2][0] | uv[2][1] << 8 | uv[3][0] << 16 | uv[3][1] << 24))
            w = face_words(i, c, t["dbl"], tex_words is not None, abe)
            out.append((w, tex_words))
        return out
    hp_recs = records(hp_faces)
    lp_recs = records(lp_faces)

    # --- 7. layout ----------------------------------------------------------
    nverts = len(sverts)
    ncol = len(cols)
    off_anim = MODEL_HDR + 4 * args.anim_slots
    off_cols = off_anim + ANIM_HDR + 8 * args.frame_slots
    off_far = off_cols + 4 * ncol
    off_hpf = off_far + 4 * ncol
    hp_bytes = sum(0x18 if tw else 0x0C for _, tw in hp_recs)
    off_lpf = off_hpf + 4 + hp_bytes
    lp_bytes = sum(0x18 if tw else 0x0C for _, tw in lp_recs)
    off_verts = off_lpf + 4 + lp_bytes
    total = (off_verts + 4 * nverts + 15) & ~15
    blob = bytearray(total)
    reloc_words, reloc_frames = [], []

    struct.pack_into("<i", blob, 0, 1)                       # numAnims
    # m_Sounds[16] @4, m_CollisionModels[8] @0x14: per-level, copied from
    # the stock model at install time. m_Data @0x34 = 0.
    for s in range(args.anim_slots):
        struct.pack_into("<I", blob, MODEL_HDR + 4 * s, off_anim)
        reloc_words.append(MODEL_HDR + 4 * s)

    ext = [(min(v[k] for v in sverts), max(v[k] for v in sverts)) for k in range(3)]
    radius = min(255, int(math.ceil(max(abs(e) for pair in ext for e in pair) / 64.0)) + 1)
    struct.pack_into("<hH", blob, off_anim, 1, ncol)         # frames, colours
    blob[off_anim + 4] = 0
    blob[off_anim + 5] = 0                                   # m_Scale
    blob[off_anim + 6] = 0                                   # short shift (all literal)
    blob[off_anim + 7] = radius
    blob[off_anim + 8] = nverts
    blob[off_anim + 9] = n_lp
    blob[off_anim + 0xB] = args.depth
    blob[off_anim + 0xC] = 0
    struct.pack_into("<I", blob, off_anim + 0x10, 0)
    struct.pack_into("<4I", blob, off_anim + 0x14, off_hpf, off_cols, off_lpf, off_far)
    reloc_words += [off_anim + 0x14, off_anim + 0x18, off_anim + 0x1C, off_anim + 0x20]
    for f in range(args.frame_slots):
        f0 = off_anim + ANIM_HDR + 8 * f
        struct.pack_into("<I", blob, f0, (off_verts & 0x1FFFFF) | (0xFF << 24))
        reloc_frames.append(f0)
        struct.pack_into("<H", blob, f0 + 4, 0)
        blob[f0 + 6] = args.shadow
        blob[f0 + 7] = nverts                                # short offset: all literal

    for j, (r, g, b) in enumerate(cols):
        struct.pack_into("<I", blob, off_cols + 4 * j, r | g << 8 | b << 16)
        struct.pack_into("<I", blob, off_far + 4 * j, (r // 2) | (g // 2) << 8 | (b // 2) << 16)
    for off, recs, nbytes in ((off_hpf, hp_recs, hp_bytes), (off_lpf, lp_recs, lp_bytes)):
        struct.pack_into("<I", blob, off, nbytes)
        fp = off + 4
        for (w0, w1, w2), tw in recs:
            struct.pack_into("<3I", blob, fp, w0, w1, w2)
            fp += 12
            if tw:
                struct.pack_into("<3I", blob, fp, *tw)
                fp += 12
    for j, (vx, vy, vz) in enumerate(sverts):
        struct.pack_into("<I", blob, off_verts + 4 * j,
                         pack_vert(int(round(vx)), int(round(vy)), int(round(vz))))

    e = Encoded()
    e.name, e.eid, e.blob = name, mesh.model.entry.eid, bytes(blob)
    e.reloc_words, e.reloc_frames = reloc_words, reloc_frames
    e.hp_verts, e.lp_verts = nverts, n_lp
    e.hp_faces, e.lp_faces = len(hp_recs), len(lp_recs)
    e.src_verts = src_verts
    e.pose = mesh.pose
    e.pose_anim, e.pose_frame = mesh.pose_anim_eid, mesh.pose_frame
    ntex = sum(1 for _, tw in hp_recs if tw)
    report.append(
        f"{name:6s} @ {mesh.pose:9s}: {src_verts:3d}->{nverts:3d} verts (LP {n_lp:3d}), "
        f"{len(hp_recs):3d} HP / {len(lp_recs):3d} LP faces ({ntex} tex), "
        f"{ncol:3d} cols, k={k:7.2f}, ext x{ext[0][0]:.0f}..{ext[0][1]:.0f} "
        f"y{ext[1][0]:.0f}..{ext[1][1]:.0f} z{ext[2][0]:.0f}..{ext[2][1]:.0f}, "
        f"orient {rep_hp['components']}c/{rep_hp['conflicts']}x/{rep_hp['reversed']}r/"
        f"{rep_hp['boundary']}b, {total} B"
        + (f", {n_blend_warn} subtract-blend faces emitted opaque" if n_blend_warn else ""))
    return e


def write_pack(encoded, args, out):
    table_off = 32
    reloc_off = table_off + TABLE_ROW * len(encoded)
    relocs = bytearray()
    payload = bytearray()
    rows = []
    for e in encoded:
        while len(payload) & 15:
            payload += b"\0"
        blob_off = len(payload)
        payload += e.blob
        rw_off = reloc_off + len(relocs)
        relocs += struct.pack(f"<{len(e.reloc_words)}I", *e.reloc_words)
        rf_off = reloc_off + len(relocs)
        relocs += struct.pack(f"<{len(e.reloc_frames)}I", *e.reloc_frames)
        rows.append((e, blob_off, rw_off, rf_off))
    payload_off = reloc_off + len(relocs)
    payload_off = (payload_off + 15) & ~15
    hdr = struct.pack("<8I", PACK_MAGIC, PACK_VERSION, len(encoded), table_off,
                      payload_off, len(payload), args.anim_slots, args.frame_slots)
    table = b""
    for e, blob_off, rw_off, rf_off in rows:
        table += struct.pack("<I8sIIHHHHHHIIIII", e.eid, e.name.encode().ljust(8, b"\0"),
                             blob_off, len(e.blob), e.hp_verts, e.lp_verts,
                             e.hp_faces, e.lp_faces, e.src_verts, e.pose_frame,
                             e.pose_anim, rw_off, len(e.reloc_words),
                             rf_off, len(e.reloc_frames))
    body = hdr + table + bytes(relocs)
    body += b"\0" * (payload_off - len(body))
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(body + bytes(payload))
    return len(body) + len(payload)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--disc", required=True, help="SCUS-94228 .cue/.bin")
    ap.add_argument("--out", type=Path, default=HERE.parents[1] / "mods" / "crash3-models" / "assets",
                    help="mod assets dir (pack + textures/own/)")
    ap.add_argument("--models", default="all", help="comma-separated eNames, or all")
    ap.add_argument("--pose", action="append", default=[],
                    help="MODEL=ANIM[:FRAME] pose override (repeatable)")
    ap.add_argument("--height", type=float, default=900.0,
                    help="model height in world units (Spyro ≈ 480)")
    ap.add_argument("--yaw", type=float, default=270.0,
                    help="rotation about the vertical axis, degrees (270 turns the "
                         "Crash models to face the statue pedestal's front)")
    ap.add_argument("--budget", type=int, default=255, help="HP vertex ceiling")
    ap.add_argument("--lp-faces", type=int, default=200, help="LP tier face target")
    ap.add_argument("--anim-slots", type=int, default=16)
    ap.add_argument("--frame-slots", type=int, default=64)
    ap.add_argument("--depth", type=int, default=2, help="AnimationHeader m_DepthScale")
    ap.add_argument("--shadow", type=int, default=68, help="frame shadow byte")
    ap.add_argument("--double-sided", action="store_true",
                    help="flag every face double-sided (skips orientation)")
    ap.add_argument("--skip-over-ceiling", action="store_true",
                    help="skip models that would lose more than 40%% of their vertices")
    args = ap.parse_args()

    lib = Library(load_demo(args.disc))
    poses = {}
    for p in args.pose:
        m, a = p.split("=")
        an, _, fr = a.partition(":")
        poses[m] = (an, int(fr or 0))
    names = sorted(m.name for m in lib.models.values()) if args.models == "all" \
        else args.models.split(",")

    atlas = Atlas(lib)
    meshes = []
    for nm in names:
        me = lib.model(nm)
        if not me:
            print(f"{nm}: no such model", file=sys.stderr); continue
        if nm in poses:
            a = lib.anim(poses[nm][0]); f = poses[nm][1]
        else:
            a, f = default_pose(lib, me)
        if a is None:
            print(f"{nm}: no animation poses it - skipped", file=sys.stderr); continue
        mesh = decode_pose(lib, me, a, f)
        if not mesh.tris:
            print(f"{nm}: no triangles - skipped", file=sys.stderr); continue
        mesh.pose_anim_eid, mesh.pose_frame = a.eid, f
        nused = mesh.stats["used_verts"]
        if args.skip_over_ceiling and nused > args.budget and \
                (nused - args.budget) / nused > 0.40:
            print(f"{nm}: {nused} verts, over-ceiling - skipped", file=sys.stderr); continue
        for t in mesh.tris:
            if t["tex"]:
                atlas.tile(t["tex"])           # register every tile first
        meshes.append(mesh)
    atlas.pack()

    report = []
    encoded = [encode_model(m, atlas, args, report) for m in meshes]
    print("\n".join(report))
    size = write_pack(encoded, args, args.out / "crash3_s1pack.bin")
    atlas.write_pngs(args.out / "textures" / "own")
    print(f"pack: {len(encoded)} models, {size} B -> {args.out / 'crash3_s1pack.bin'}; "
          f"{len(atlas.tiles)} tiles in {len(atlas.sheets)} atlas sheet(s) "
          f"(ids {ATLAS_ID_BASE:04x}..{ATLAS_ID_BASE + len(atlas.sheets) - 1:04x})")


if __name__ == "__main__":
    main()
