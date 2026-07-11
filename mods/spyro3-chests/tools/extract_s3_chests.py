#!/usr/bin/env python3
"""Extract Spyro 3 container models + break SFX + textures for the
spyro3-chests mod.

Pulls from the S3 disc (level 10, Sunrise Spring Home — every asset we
need is in its bank):
  - basket   (class 200, SIMPLE, state 0)          -> S1 wooden chest
  - vase     (class 201, ANIMATED, anim 0 frame 0) -> S1 metal chest
  - fragments (classes 306/307/308, SIMPLE 2-state) -> S1 chest fragments
  - break SFX: SoundDefinition 0x39 (basket) + 0x3A (vase), carved from
    the level SPU bank as raw SPU-ADPCM bodies
  - textures: every 4bpp CLUT tile referenced by the models' textured
    faces, decoded from the level VRAM block to RGBA + kept raw for the
    converter's 15bpp VRAM re-encode

Format ground truth (see notes/plans/openpete-mod-spyro3-chests.md):
  - SIMPLE model internal ptrs are model-relative; state hp/lp u16
    offsets are relative to sharedDataBase (model+0x10).
  - ANIMATED: anims[]@+0x3C are model-relative; anim-internal ptrs
    (+0xC verts, +0x10 faces, +0x14 colours, +0x18 lpFaces, +0x20 data)
    are relative to the model's DEFAULT faces block (value @model+0x38).
    NULL anim faces ptr => the default block (rebases to it naturally).
    Single-frame models alias verts==data: the vertex array doubles as
    the frame's block stream (all long blocks).
  - Textured face record (5 words): word2 = p1U|p1V<<8|paletteX(6b,
    *16 halfwords)<<16|paletteY(10b)<<22; word3 = p2U|p2V<<8|
    imageBlockX(4b,*64 halfwords)<<16|imageBlockY(1b,*256)<<20;
    word4 = p3U|p3V<<8|p4U<<16|p4V<<24. U/V are 4bpp texel units within
    the block. (SpyroEdit UpdateObjTexMap.)
  - The level vramSpu block's first 0x80000 bytes are the RIGHT HALF of
    VRAM: 512x512 halfwords, VRAM x in [512,1024). Verified below by
    requiring every referenced tile/CLUT to land inside it.

Usage:
  python3 extract_s3_chests.py [--disc PATH] [--out DIR] [--level N]
"""
import argparse
import json
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent.parent          # openpete/
sys.path.insert(0, str(REPO_ROOT / "mods" / "spyro3-gems" / "tools"))
sys.path.insert(0, str(REPO_ROOT / "tools"))

import extract_s3_gems as g                    # noqa: E402  (decoders, WAD plumbing)
import extract_s3_player as esp                # noqa: E402  (Disc, iso_find)

BASKET_CLASS = 200      # SIMPLE 1-state  -> S1 wooden chest (194)
VASE_CLASS = 201        # ANIMATED 1x1    -> S1 metal chest (195)
FRAG_CLASSES = (306, 307, 308)   # SIMPLE 2-state debris models
BASKET_SOUND_DEF = 0x39  # model header byte +4 (fPlayMobySound slot 0)
VASE_SOUND_DEF = 0x3A

VRAM_HALF_W = 512       # right-half snapshot: 512x512 halfwords
VRAM_X_BASE = 512


def u8(b, o): return b[o]
def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def u32(b, o): return struct.unpack_from("<I", b, o)[0]
def s32(b, o): return struct.unpack_from("<i", b, o)[0]


def model_blob(lvl, hdr, cls):
    slots = [i for i, c in enumerate(hdr.model_classes) if c == cls]
    if not slots:
        raise SystemExit(f"class {cls} not in this level's model bank")
    off = hdr.model_offsets[slots[0]]
    bounds = sorted(o for o in hdr.model_offsets + [hdr.scene_off] if o > off)
    end = bounds[0] if bounds else len(lvl)
    return off, lvl[off:end]


def face_records(blob, face_off, animated):
    """decode_faces + raw record words (we need word2..4 for textures)."""
    faces = g.decode_faces(blob[face_off:face_off + u32(blob, face_off) + 8],
                           animated=animated)
    # re-walk to capture the tile words per textured face
    idx = 1
    for f in faces:
        base = face_off + idx * 4
        if f.get("textured"):
            w2, w3, w4 = struct.unpack_from("<3I", blob, base + 8)
            f["uv"] = decode_tile_words(w2, w3, w4)
            idx += 5
        else:
            idx += 2
    return faces


def decode_tile_words(w2, w3, w4):
    return {
        "u": [w2 & 0xFF, w3 & 0xFF, (w4 >> 0) & 0xFF, (w4 >> 16) & 0xFF],
        "v": [(w2 >> 8) & 0xFF, (w3 >> 8) & 0xFF, (w4 >> 8) & 0xFF,
              (w4 >> 24) & 0xFF],
        "palette_x": ((w2 >> 16) & 0x3F) * 0x10,   # halfword column
        "palette_y": (w2 >> 22) & 0x3FF,
        "block_x": ((w3 >> 16) & 0xF) * 0x40,      # halfword column
        "block_y": ((w3 >> 20) & 1) * 0x100,
        "raw": [f"{w2:08x}", f"{w3:08x}", f"{w4:08x}"],
    }


def decode_simple_state(blob, state_idx):
    shared_base = u32(blob, 0x10)
    st = u32(blob, 0x14 + 4 * state_idx)
    n_col, n_vert = u8(blob, st), u8(blob, st + 1)
    hp_face = shared_base + u16(blob, st + 8)
    hp_colour = shared_base + u16(blob, st + 10)
    verts = []
    for i in range(n_vert):
        w = u32(blob, st + 0x10 + 4 * i)
        # SpyroEdit UpdateMoby non-animated (true coords; z halved here)
        verts.append((-g.bitss(w, 22, 10), -g.bitss(w, 12, 10),
                      -g.bitss(w, 0, 12) / 2.0))
    faces = face_records(blob, hp_face, animated=False)
    colours = [u32(blob, hp_colour + 4 * i) for i in range(n_col)]
    return dict(verts=verts, faces=faces, colours=[f"{c:08x}" for c in colours],
                world_scale=4.0)


def decode_animated_frame(blob, anim_idx=0, frame_idx=0):
    base_faces = u32(blob, 0x38)
    ah = u32(blob, 0x3C + 4 * anim_idx)
    num_frames, num_colours, vert_scale = (u8(blob, ah), u8(blob, ah + 1),
                                           u8(blob, ah + 2))
    num_verts = u8(blob, ah + 4)
    verts_p = base_faces + u32(blob, ah + 0x0C)
    faces_p = base_faces + u32(blob, ah + 0x10)
    colours_p = base_faces + u32(blob, ah + 0x14)
    data_p = base_faces + u32(blob, ah + 0x20)
    fr = ah + 0x24 + 8 * frame_idx
    block_off, num_blocks, start_flags = (u16(blob, fr), u8(blob, fr + 2),
                                          u8(blob, fr + 3))
    raw_verts = [u32(blob, verts_p + 4 * i) for i in range(num_verts)]
    blocks_at = data_p + 2 * block_off
    adjusts_at = blocks_at + 2 * (num_blocks + ((start_flags & 1) << 8))
    verts = g.decode_animated_vertices(
        raw_verts, blob[blocks_at:blocks_at + 8192],
        blob[adjusts_at:adjusts_at + 8192], (start_flags & 2) != 0, num_verts)
    faces = face_records(blob, faces_p, animated=True)
    colours = [u32(blob, colours_p + 4 * i) for i in range(num_colours)]
    return dict(verts=verts, faces=faces, colours=[f"{c:08x}" for c in colours],
                vert_scale=vert_scale, num_frames=num_frames,
                # SpyroEdit renders animated at MOBYPOSSCALE*(1<<vertScale);
                # relative to the simple path's x4 that is (1<<vs)/4. The S1
                # converter fits to the stock chest envelope anyway, so this
                # only labels the JSON.
                world_scale=float(1 << vert_scale))


# ---------------------------------------------------------------------------
# textures
# ---------------------------------------------------------------------------

def vram_halfword(vram, x, y):
    """x = absolute VRAM halfword column, y = row; right-half snapshot."""
    if not (VRAM_X_BASE <= x < VRAM_X_BASE + VRAM_HALF_W and 0 <= y < 512):
        raise ValueError(f"VRAM ref ({x},{y}) outside right-half snapshot")
    return struct.unpack_from("<H", vram,
                              (y * VRAM_HALF_W + (x - VRAM_X_BASE)) * 2)[0]


def decode_tile(vram, uv):
    """Decode the 4bpp tile covering this face's UV span -> dict with
    RGBA rows + the raw 16-entry CLUT. Tile bounds are the face's UV
    bounding box snapped out to 4-texel columns."""
    us, vs = uv["u"], uv["v"]
    u0, u1 = min(us) & ~3, (max(us) | 3)
    v0, v1 = min(vs), max(vs)
    clut = [vram_halfword(vram, uv["palette_x"] + i, uv["palette_y"])
            for i in range(16)]
    rows = []
    for v in range(v0, v1 + 1):
        row = []
        for u in range(u0, u1 + 1):
            hw = vram_halfword(vram, uv["block_x"] + (u >> 2),
                               uv["block_y"] + v)
            nib = (hw >> ((u & 3) * 4)) & 0xF
            row.append(clut[nib])
        rows.append(row)
    return dict(u0=u0, v0=v0, w=u1 - u0 + 1, h=v1 - v0 + 1,
                clut=[f"{c:04x}" for c in clut],
                texels_5551=[[f"{t:04x}" for t in row] for row in rows])


def tile_key(uv):
    return (uv["palette_x"], uv["palette_y"], uv["block_x"], uv["block_y"])


def collect_textures(vram, models):
    """One merged tile per (CLUT, block) pair, spanning all faces that
    reference it. Faces get a tile id + their UVs kept block-absolute."""
    spans = {}
    for m in models.values():
        for f in m["faces"]:
            if "uv" not in f:
                continue
            k = tile_key(f["uv"])
            us, vs = f["uv"]["u"], f["uv"]["v"]
            n = min(us), min(vs), max(us), max(vs)
            if k in spans:
                o = spans[k]
                spans[k] = (min(o[0], n[0]), min(o[1], n[1]),
                            max(o[2], n[2]), max(o[3], n[3]))
            else:
                spans[k] = n
    tiles = {}
    for i, (k, (u0, v0, u1, v1)) in enumerate(sorted(spans.items())):
        uv = dict(palette_x=k[0], palette_y=k[1], block_x=k[2], block_y=k[3],
                  u=[u0, u1], v=[v0, v1])
        tiles[k] = dict(id=i, **decode_tile(vram, uv))
        tiles[k]["palette_x"], tiles[k]["palette_y"] = k[0], k[1]
        tiles[k]["block_x"], tiles[k]["block_y"] = k[2], k[3]
    for m in models.values():
        for f in m["faces"]:
            if "uv" in f:
                f["tile"] = tiles[tile_key(f["uv"])]["id"]
    return [tiles[k] for k in sorted(tiles)]


def tiles_preview_png(tiles, path):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return
    if not tiles:
        return
    cols = min(4, len(tiles))
    rows = (len(tiles) + cols - 1) // cols
    fig, axes = plt.subplots(rows, cols, figsize=(3 * cols, 3 * rows),
                             squeeze=False)
    for i, t in enumerate(tiles):
        ax = axes[i // cols][i % cols]
        img = []
        for row in t["texels_5551"]:
            img.append([[(hw & 0x1F) / 31.0, ((hw >> 5) & 0x1F) / 31.0,
                         ((hw >> 10) & 0x1F) / 31.0] for hw in
                        (int(h, 16) for h in row)])
        ax.imshow(img, interpolation="nearest")
        ax.set_title(f"tile {t['id']} {t['w']}x{t['h']}", fontsize=8)
        ax.axis("off")
    for j in range(len(tiles), rows * cols):
        axes[j // cols][j % cols].axis("off")
    fig.tight_layout()
    fig.savefig(path, dpi=100)
    plt.close(fig)
    print(f"  wrote {path.name}")


# ---------------------------------------------------------------------------
# sounds
# ---------------------------------------------------------------------------

def extract_break_sounds(lvl, hdr, out_dir):
    """Carve SoundDefinition bodies 0x39/0x3A (basket/vase break) from the
    level SPU bank, using the gems extractor's disc-verified helpers."""
    spu_base = hdr.vram_spu_off + g.SPU_PART_OFF
    spu_size = hdr.vram_spu_size - g.SPU_PART_OFF
    spu = lvl[spu_base:spu_base + spu_size]
    table_off, count, addrs = g.find_sound_block(lvl, spu_size)
    if table_off is None:
        raise SystemExit("sound block not found in level data")
    defs_off = table_off + 0x104

    meta = {}
    for name, idx in (("basket_break", BASKET_SOUND_DEF),
                      ("vase_break", VASE_SOUND_DEF)):
        if idx >= count:
            print(f"  WARNING: def {idx:#x} out of range ({count}) — skipped")
            continue
        raw_def = lvl[defs_off + idx * g.SOUND_DEF_SIZE:
                      defs_off + (idx + 1) * g.SOUND_DEF_SIZE]
        addr = addrs[idx]
        nxt = [a for a in sorted(addrs) if a > addr]
        bound = (nxt[0] if nxt else spu_size)
        end_pos, bad = g.adpcm_trim(spu, addr, bound)
        body = spu[addr:end_pos]
        (out_dir / f"{name}.vag_body.bin").write_bytes(body)
        meta[name] = dict(def_index=idx, spu_addr=addr, bytes=len(body),
                          bad_frames=bad, raw_def=raw_def.hex())
        print(f"  {name}: def {idx:#x}, {len(body)} bytes ADPCM "
              f"({bad} suspect frames)")
    return meta


# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    default_disc = next((REPO_ROOT.parent / "roms" / "SCUS-94467").glob("*.img"),
                        None)
    ap.add_argument("--disc", type=Path, default=default_disc)
    ap.add_argument("--out", type=Path, default=HERE.parent / "assets" / "s3")
    ap.add_argument("--level", type=int, default=10)
    args = ap.parse_args()
    if not args.disc or not args.disc.exists():
        raise SystemExit("S3 disc image not found — pass --disc")
    args.out.mkdir(parents=True, exist_ok=True)

    disc = esp.Disc(str(args.disc))
    wad_lba, _ = esp.iso_find(disc, "WAD.WAD")
    lvl = g.read_level(disc, wad_lba, args.level)
    hdr = g.LevelWadHeader(lvl)
    vram = lvl[hdr.vram_spu_off:hdr.vram_spu_off + g.SPU_PART_OFF]

    models = {}

    off, blob = model_blob(lvl, hdr, BASKET_CLASS)
    assert s32(blob, 0) < 0, "basket expected SIMPLE"
    models["basket"] = decode_simple_state(blob, 0)
    models["basket"]["s3_class"] = BASKET_CLASS
    models["basket"]["sound_byte"] = u8(blob, 4)

    off, blob = model_blob(lvl, hdr, VASE_CLASS)
    assert s32(blob, 0) >= 0, "vase expected ANIMATED"
    models["vase"] = decode_animated_frame(blob)
    models["vase"]["s3_class"] = VASE_CLASS
    models["vase"]["sound_byte"] = u8(blob, 4)

    for cls in FRAG_CLASSES:
        off, blob = model_blob(lvl, hdr, cls)
        n_states = -s32(blob, 0)
        for st in range(n_states):
            m = decode_simple_state(blob, st)
            m["s3_class"] = cls
            models[f"frag{cls}_s{st}"] = m

    for name, m in models.items():
        ntex = sum(1 for f in m["faces"] if "uv" in f)
        print(f"  {name}: {len(m['verts'])}v {len(m['faces'])}f "
              f"({ntex} textured), {len(m['colours'])} colours")

    tiles = collect_textures(vram, models)
    print(f"  {len(tiles)} unique texture tiles")
    tiles_preview_png(tiles, args.out / "chest_tiles_preview.png")

    sounds = extract_break_sounds(lvl, hdr, args.out)

    out = dict(level=args.level, models=models, tiles=tiles, sounds=sounds)
    (args.out / "chest_models.json").write_text(json.dumps(out, indent=1))
    print(f"  wrote chest_models.json "
          f"({(args.out / 'chest_models.json').stat().st_size} bytes)")


if __name__ == "__main__":
    main()
