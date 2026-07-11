#!/usr/bin/env python3
"""extract_s3_gems.py — carve the Spyro 3 (SCUS-94467) gem assets out of
the user's own Year of the Dragon disc image.

The spyro3-gems mod replaces Spyro 1's gems with Spyro 3's: model (all 5
denomination anims), collect-chime SFX, and the rising-pitch ladder.  Gem
data is Activision/Insomniac content, so — same legal posture as every
openpete asset tool — the mod never SHIPS it: this tool derives it locally
from a disc the user supplies:

    mods/spyro3-gems/tools/extract_s3_gems.py \
        --disc "roms/SCUS-94467/Spyro ... .img" \
        --out  mods/spyro3-gems/assets/s3

Format facts (spyro-3 decomp + SpyroEdit, disc-verified 2026-07-11):

  Level model bank (gem = moby class 1, present in every level):
  - WAD.WAD wadHeader: LevelWads lvl[40] at +776 (each {lvl{off,size},
    ovl{off,size}}); all LevelWadHeader offsets are relative to the
    level's lvl Data entry start (proven by the dragonModels loader,
    spyro-3 src/mobyutil.c:620).
  - LevelWadHeader (0x220 bytes at lvl start): vramSpu@0, scene@8,
    AreaHeader area[4]@0x10, dragonModels[6]@0x60, modelOffsets[64]@0xA0
    (u32, lvl-relative; loader computes ram = base + off - area[0].env.off,
    func_8002B31C), modelClasses[64]@0x1A0 (u16).  Runtime table
    D_8006EE2C[mobyClass] = model pointer (func_8002AF9C).
  - Model word0 >= 0 -> ANIMATED (numAnims); word0 < 0 -> SIMPLE model,
    word0 = -numStates (func_8002AF9C negates it at load).  The gem is a
    SIMPLE model with 5 states = denominations red/green/blue/gold/pink
    (anim ids 0-4 select states, mobyutil D_80066988 identity table).
  - SimpleModelHeader (disc form): s32 -numStates@0, u32@4, two
    base-rebased ptr words @8/@C, sharedDataBase offset @0x10
    (model-relative), state offsets[numStates]@0x14 (model-relative).
  - SimpleModelStateHeader: u8 numHpColours, numHpVerts, unk1, unk2;
    u32 allUnks@4; u16 hpFaceOff@8, hpColourOff@A, lpFaceOff@C,
    lpColourOff@E — ON DISC relative to the model's sharedDataBase
    target (the loader rebases them to state-relative by adding
    (sharedBase - state)); packed verts (u32 each) at state+0x10.
  - Simple vertex packing: 10/10/12 — x = -bitss(w,22,10),
    y = -bitss(w,12,10), z = -bitss(w,0,12)/2, world scale
    4 * MOBYPOSSCALE (SpyroEdit UpdateMoby, non-animated path).
  - Faces: u32[0] = face-data byte length; records follow: 2 words
    untextured / 5 words textured (word1 bit31).  Simple models use
    7-BIT vert ids: tri iff (w0&0x7F)==((w0>>7)&0x7F); quad
    v=[w0>>7, w0, w0>>14, w0>>21]&0x7F, tri v=[w0>>21, w0>>14, w0>>7];
    colour ids 7-bit in w1 at bits [10],[3],[17],[24] (quad) /
    [24],[17],[10] (tri) (SpyroEdit ConvertFaces, non-animated path).
  - (Animated models keep the 11/11/10 + block/adjust codec with the
    S1-identical 5x5x5 delta LUT, retained below for completeness;
    verified against S1 ROM data D_8006D614.)

  Chime SFX:
  - vramSpu block = VRAM texture part (first 0x80000 bytes) + SPU-ADPCM
    part at +0x80000 (constant hardcoded in the loading overlay).  The
    SPU part is uploaded wholesale to SPU addr 0x1010 in 64 KiB chunks
    (loading overlay func_loading_8007494C: disc read at
    lvlBase + vramSpu.offset + 0x80000, SpuSetTransferStartAddr
    (k<<16)|0x1010; disc-verified — valid ADPCM frames span exactly
    vramSpu.off+0x80000 .. vramSpu.off+vramSpu.size).
  - SoundTable (0x100 bytes) + u32 count + SoundDefinition[count]
    (0x14 bytes: s32 m_Addr + 8 u16/u8 params) live in the level layout
    section chain; m_Addr values are pre-fixup offsets into the vramSpu
    payload (+0x1010 applied at load, src/loaders.c func_8002C9F4).
    Located here by signature scan with strict validation.
  - gemChime = SoundTable byte 0 (spyro-3 include/spu.h) = the
    gem-BOUNCE sound.  The actual collect gling is def 1 (SoundTable
    byte 1, the slot spu.h names gemCollect), 11025 Hz — identified by
    ear from a full-bank dump; the asm-traced byte 0x1A ("changeVolume",
    sparkle-arrival handler) is the menu blip, a red herring.

Outputs (all in --out, which is gitignored — NEVER commit extracted data):
    gem_model.bin        raw model blob
    gem_anim<N>.json     decoded frame-0 geometry per anim 0..4
    gem_chime.vag_body.bin  raw SPU-ADPCM frames (no VAG header)
    gem_chime.json       SoundDefinition fields + raw hex
    pitch_ladder.md      rising-pitch mechanics (constants from asm)
"""

import argparse
import json
import struct
import sys
from pathlib import Path

# Reuse the disc-verified plumbing (Disc, iso_find, read_span) from the
# player-model extractor.
REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "tools"))
import extract_s3_player as esp  # noqa: E402

GEM_CLASS = 1
NUM_GEM_ANIMS = 5  # denominations red/green/blue/gold/pink = anim ids 0-4

# ---------------------------------------------------------------------------
# Vertex-adjust delta LUT.  Entries 0..124 form a 5x5x5 signed-delta grid
# packed 11/11/10: dx,dy in {-12,-6,0,6,12}, dz in {-6,-3,0,3,6} (z doubled
# at decode).  Generated form verified byte-identical against Spyro 1's ROM
# copy (D_8006D614) — S1 and S3 share the table.  Adjust bytes b index
# LUT[b>>1]; b>>1 > 124 never occurs in valid data (warned if seen).
# ---------------------------------------------------------------------------


def _pack_delta(dx: int, dy: int, dz: int) -> int:
    return ((dx & 0x7FF) << 21) | ((dy & 0x7FF) << 10) | (dz & 0x3FF)


VERT_ADJUST_LUT = [
    _pack_delta(6 * (i % 5 - 2), 6 * (i // 5 % 5 - 2), 3 * (i // 25 - 2))
    for i in range(125)
]
assert VERT_ADJUST_LUT[0] == 0xFE9FD3FA
assert VERT_ADJUST_LUT[9] == 0x019FEBFA
assert VERT_ADJUST_LUT[62] == 0x00000000
assert VERT_ADJUST_LUT[124] == 0x01803006


def bitss(v: int, low: int, count: int) -> int:
    """Signed bitfield extract (mirrors SpyroEdit's bitss)."""
    x = (v >> low) & ((1 << count) - 1)
    return x - (1 << count) if x & (1 << (count - 1)) else x


# ---------------------------------------------------------------------------
# WAD header / level table
# ---------------------------------------------------------------------------

# WadHeader layout (spyro-3 include/common.h:36-77): 6 DataHeaders,
# CutsceneWads cutscene[21] (24 B each), loadOvl, loadImg[6],
# cutsceneImg[20], loadCycloramas, then LevelWads lvl[40] (16 B each).
LVL_TABLE_OFF = 6 * 8 + 21 * 24 + 8 + 6 * 8 + 20 * 8 + 8
assert LVL_TABLE_OFF == 776


def read_level(disc, wad_lba: int, level: int):
    off, size, ovl_off, ovl_size = struct.unpack(
        "<4i", disc.read_span(wad_lba, LVL_TABLE_OFF + level * 16, 16))
    print(f"lvl[{level}]: offset {off:#x}, size {size:#x} "
          f"(ovl {ovl_off:#x}/{ovl_size:#x})")
    if not (0 < off and 0x1000 < size < 0x400000):
        raise SystemExit(f"implausible lvl[{level}] entry")
    return disc.read_span(wad_lba, off, size)


class LevelWadHeader:
    def __init__(self, lvl: bytes):
        self.vram_spu_off, self.vram_spu_size = struct.unpack_from("<2i", lvl, 0)
        self.scene_off, self.scene_size = struct.unpack_from("<2i", lvl, 8)
        self.areas = [struct.unpack_from("<4i", lvl, 0x10 + i * 16)
                      for i in range(4)]  # (env_off, env_size, lay_off, lay_size)
        self.model_offsets = list(struct.unpack_from("<64I", lvl, 0xA0))
        self.model_classes = list(struct.unpack_from("<64H", lvl, 0x1A0))


# ---------------------------------------------------------------------------
# Gem model decode
# ---------------------------------------------------------------------------

def decode_animated_vertices(verts, blocks_bytes, adjusts_bytes,
                             start_on_block: bool, num_verts: int):
    """Integer mirror of SpyroEdit ConvertAnimatedVertices (non-Spyro path).
    Returns list of (x, y, z) with z already doubled — raw model units
    (world = raw * MOBYPOSSCALE * (1 << vertScale))."""
    out = []
    cur_x = cur_y = cur_z = 0
    cur_block = 0
    next_block = bool(start_on_block)
    block_vert = False
    adjust_vert = 0

    def block_hw(i: int) -> int:
        return struct.unpack_from("<H", blocks_bytes, i * 2)[0]

    for i in range(num_verts):
        used_block = False
        w = verts[i]
        x, y, z = bitss(w, 21, 11), bitss(w, 10, 11), bitss(w, 0, 10)

        if next_block:
            hw = block_hw(cur_block)
            block_vert = (hw & 1) != 0
            next_block = block_vert
            if hw & 0x2:  # long block: absolute position
                pos = hw | (block_hw(cur_block + 1) << 16)
                cur_x = bitss(pos, 21, 11)
                cur_y = -bitss(pos, 11, 11)
                cur_z = -bitss(pos, 1, 11)
                x = y = z = 0
                cur_block += 2
            else:  # short block: delta
                cur_x += bitss(hw, 10, 6) * 2
                cur_y += bitss(hw, 5, 6) * 2
                cur_z += bitss(hw, 1, 5) * 2
                cur_block += 1
            used_block = True

        if not used_block and not block_vert:
            adj = adjusts_bytes[adjust_vert]
            if (adj >> 1) >= len(VERT_ADJUST_LUT):
                print(f"  WARNING: adjust byte {adj:#x} indexes past the "
                      "125-entry LUT — decode suspect")
                delta = 0
            else:
                delta = VERT_ADJUST_LUT[adj >> 1]
            adjusted = (delta + w) & 0xFFFFFFFF
            if adj & 1:
                next_block = True
            x = bitss(adjusted, 21, 11)
            y = bitss(adjusted, 10, 11)
            z = bitss(adjusted, 0, 10)
            adjust_vert += 1

        cur_x += x
        cur_y += y
        cur_z += z
        out.append((cur_x, cur_y, cur_z * 2))
    return out


def decode_faces(face_bytes: bytes, animated: bool):
    """SpyroEdit ConvertFaces: 8-bit vert ids (animated) / 7-bit (simple)."""
    (total_len,) = struct.unpack_from("<I", face_bytes, 0)
    max_index = total_len // 4 + 1
    faces = []
    index = 1
    while index < max_index:
        w0, w1 = struct.unpack_from("<2I", face_bytes, index * 4)
        if animated:
            is_tri = (w0 & 0xFF) == ((w0 >> 8) & 0xFF)
            if is_tri:
                v = [w0 >> 24 & 0xFF, w0 >> 16 & 0xFF, w0 >> 8 & 0xFF]
            else:
                v = [w0 >> 8 & 0xFF, w0 & 0xFF, w0 >> 16 & 0xFF,
                     w0 >> 24 & 0xFF]
        else:
            is_tri = (w0 & 0x7F) == ((w0 >> 7) & 0x7F)
            if is_tri:
                v = [w0 >> 21 & 0x7F, w0 >> 14 & 0x7F, w0 >> 7 & 0x7F]
            else:
                v = [w0 >> 7 & 0x7F, w0 & 0x7F, w0 >> 14 & 0x7F,
                     w0 >> 21 & 0x7F]
        if is_tri:
            c = [w1 >> 24 & 0x7F, w1 >> 17 & 0x7F, w1 >> 10 & 0x7F]
        else:
            c = [w1 >> 10 & 0x7F, w1 >> 3 & 0x7F, w1 >> 17 & 0x7F,
                 w1 >> 24 & 0x7F]
        face = {"v": v, "c": c, "tri": is_tri,
                "colouredByMoby": bool(w1 & 1),
                "raw": [f"{w0:08x}", f"{w1:08x}"]}
        if w1 & 0x80000000:  # textured record: 3 extra uv words
            extra = struct.unpack_from("<3I", face_bytes, (index + 2) * 4)
            face["textured"] = True
            face["uv_raw"] = [f"{w:08x}" for w in extra]
            index += 5
        else:
            index += 2
        faces.append(face)
    return faces


def extract_gem_model(lvl: bytes, hdr: LevelWadHeader, out_dir: Path):
    slots = [i for i, c in enumerate(hdr.model_classes) if c == GEM_CLASS]
    if not slots:
        raise SystemExit("moby class 1 (gem) not in this level's model bank")
    slot = slots[0]
    model_off = hdr.model_offsets[slot]
    print(f"gem model: bank slot {slot}, class {hdr.model_classes[slot]}, "
          f"lvl offset {model_off:#x}")
    if not (0x220 <= model_off < len(lvl)):
        raise SystemExit(f"gem model offset {model_off:#x} out of lvl bounds")

    # Blob bound: next-higher model offset (or scene start) caps the blob.
    boundaries = sorted(o for o in hdr.model_offsets + [hdr.scene_off]
                        if o > model_off)
    bound = (boundaries[0] if boundaries else len(lvl)) - model_off

    word0 = struct.unpack_from("<i", lvl, model_off)[0]
    if word0 >= 0:
        raise SystemExit(f"gem model word0 = {word0:#x}: expected a SIMPLE "
                         "model (negative -numStates) — format drift?")
    num_states = -word0
    if num_states != NUM_GEM_ANIMS:
        print(f"  WARNING: expected {NUM_GEM_ANIMS} gem states, "
              f"found {num_states}")

    shared_base = struct.unpack_from("<I", lvl, model_off + 0x10)[0]
    state_offs = struct.unpack_from(f"<{num_states}I", lvl, model_off + 0x14)
    size = bound
    model = lvl[model_off:model_off + size]
    print(f"  SIMPLE model: {num_states} states, sharedDataBase "
          f"+{shared_base:#x}, states at {[hex(o) for o in state_offs]}, "
          f"next-boundary blob {size:#x}")
    print("  header hexdump:")
    for row in range(4):
        row_bytes = model[row * 16:row * 16 + 16]
        hexs = " ".join(f"{b:02x}" for b in row_bytes)
        print(f"    lvl+{model_off + row * 16:06x}: {hexs}")

    (out_dir / "gem_model.bin").write_bytes(model)
    print(f"  wrote gem_model.bin ({len(model)} bytes)")

    sbase = model_off + shared_base  # disc-form u16 offsets are vs this
    stats = []
    for a in range(num_states):
        soff = state_offs[a]
        if not (0 < soff < size):
            print(f"  state {a}: offset {soff:#x} out of blob — skipped")
            continue
        base = model_off + soff
        num_colours, num_verts, unk1, unk2 = struct.unpack_from(
            "<4B", lvl, base)
        all_unks = struct.unpack_from("<I", lvl, base + 4)[0]
        hp_face, hp_colour, lp_face, lp_colour = struct.unpack_from(
            "<4H", lvl, base + 8)

        raw_verts = struct.unpack_from(f"<{num_verts}I", lvl, base + 0x10)
        # SpyroEdit UpdateMoby non-animated: x=-bitss(w,22,10),
        # y=-bitss(w,12,10), z=-bitss(w,0,12)/2, scale 4*MOBYPOSSCALE.
        # Stored here as integers with z NOT halved (see scale_note).
        xyz = [(-bitss(w, 22, 10), -bitss(w, 12, 10), -bitss(w, 0, 12))
               for w in raw_verts]

        faces = decode_faces(lvl[sbase + hp_face:sbase + hp_face + 0x8000],
                             animated=False)
        colours = struct.unpack_from(f"<{num_colours}I", lvl,
                                     sbase + hp_colour)

        bad_v = sum(1 for f_ in faces for i in f_["v"] if i >= num_verts)
        oob_c = sum(1 for f_ in faces for i in f_["c"] if i >= num_colours)
        xs, ys, zs = ([p[k] for p in xyz] for k in range(3))
        span = (f"x[{min(xs)},{max(xs)}] y[{min(ys)},{max(ys)}] "
                f"z[{min(zs)},{max(zs)}]") if xyz else "-"
        ntri = sum(1 for f_ in faces if f_["tri"])
        # Gem states carry a single 0x7F7F7F colour and set the
        # colouredByMoby bit on every face; the 7-bit "colour" fields on
        # tri records exceed the 1-entry table — they are per-vertex
        # shading/ramp values, tinted at runtime by the moby colour
        # (D_80066998 palette).  Raw words are kept in the JSON.
        print(f"  state {a}: {num_verts} verts, {len(faces)} faces "
              f"({ntri} tri / {len(faces) - ntri} quad), "
              f"{num_colours} colours, {span}"
              + (f"  BAD-IDX v:{bad_v}" if bad_v else "")
              + (f"  (c-fields>{num_colours - 1}: {oob_c}, runtime-tinted)"
                 if oob_c else ""))

        anim_json = {
            "anim": a,
            "model_kind": "simple",
            "numFrames": 1,
            "numVerts": num_verts,
            "numColours": num_colours,
            "scale_note": "world = raw * 4 * MOBYPOSSCALE for x/y; "
                          "z additionally halved (SpyroEdit non-animated "
                          "path); verts stored sign-corrected, z UNhalved",
            "header_bytes": {"unk1": unk1, "unk2": unk2,
                             "allUnks": f"{all_unks:08x}"},
            "offsets": {"state_model_relative": soff,
                        "sharedDataBase_model_relative": shared_base,
                        "hpFaceOff": hp_face, "hpColourOff": hp_colour,
                        "lpFaceOff": lp_face, "lpColourOff": lp_colour,
                        "disc_form_note": "hp/lp offsets are relative to "
                                          "sharedDataBase on disc; loader "
                                          "rebases them state-relative"},
            "raw_header_words": [
                f"{w:08x}" for w in struct.unpack_from("<4I", lvl, base)],
            "colour_semantics_note":
                "single colour 0x7F7F7F; every face sets w1 bit0 "
                "(colouredByMoby) - runtime tint = gem palette "
                "D_80066998[colour.r * 4 words]; tri-record 7-bit fields "
                "are shading/ramp values, not colour-table indices",
            "frame0": {
                "verts": [list(p) for p in xyz],
                "verts_raw": [f"{w:08x}" for w in raw_verts],
                "faces": faces,
                "colours": [f"0x{c & 0xFFFFFF:06X}" for c in colours],
                "colours_raw": [f"{c:08x}" for c in colours],
            },
        }
        path = out_dir / f"gem_anim{a}.json"
        path.write_text(json.dumps(anim_json, indent=1))
        stats.append((a, num_verts, len(faces), num_colours))
    return slot, stats


# ---------------------------------------------------------------------------
# Chime SFX
# ---------------------------------------------------------------------------

SOUND_DEF_SIZE = 0x14


def find_sound_block(lvl: bytes, spu_size: int):
    """Signature-scan the level data for SoundTable(0x100) + u32 count +
    SoundDefinition[count]."""
    for t in range(0x220, len(lvl) - 0x200, 4):
        count = struct.unpack_from("<I", lvl, t + 0x100)[0]
        if not (8 <= count <= 0xFF):
            continue
        if t + 0x104 + count * SOUND_DEF_SIZE > len(lvl):
            continue
        table = lvl[t:t + 0x100]
        # Table bytes are def indices; 0xFF marks an absent sound.
        if any(b >= count and b != 0xFF for b in table):
            continue
        if len(set(b for b in table if b != 0xFF)) < 8:
            continue  # not a real table
        addrs = []
        ok = True
        for i in range(count):
            m_addr = struct.unpack_from("<i", lvl,
                                        t + 0x104 + i * SOUND_DEF_SIZE)[0]
            if m_addr < 0 or m_addr >= spu_size or m_addr % 16:
                ok = False
                break
            addrs.append(m_addr)
        if not ok or 0 not in addrs:
            continue
        return t, count, addrs
    return None, None, None


def adpcm_trim(payload: bytes, start: int, end: int):
    """Walk 16-byte SPU-ADPCM frames from start; stop after a frame with the
    end bit (flags byte frame[1] bit 0) or at end.  Also validate headers."""
    pos = start
    bad = 0
    while pos + 16 <= end:
        shift = payload[pos] & 0xF
        filt = (payload[pos] >> 4) & 0x7
        if shift > 12 or filt > 4:
            bad += 1
        flags = payload[pos + 1]
        pos += 16
        if flags & 1:  # end frame (0x*1 / 0x*3)
            break
    return pos, bad


SPU_PART_OFF = 0x80000  # VRAM texture part precedes the SPU-ADPCM part


def extract_gem_chime(lvl: bytes, hdr: LevelWadHeader, out_dir: Path):
    if hdr.vram_spu_size <= SPU_PART_OFF:
        raise SystemExit("vramSpu block smaller than the VRAM part — "
                         "unexpected layout")
    spu_base = hdr.vram_spu_off + SPU_PART_OFF
    spu_size = hdr.vram_spu_size - SPU_PART_OFF
    spu = lvl[spu_base:spu_base + spu_size]
    print(f"vramSpu: lvl offset {hdr.vram_spu_off:#x}, "
          f"size {hdr.vram_spu_size:#x}; SPU-ADPCM part at "
          f"+{SPU_PART_OFF:#x} ({spu_size:#x} bytes -> SPU 0x1010)")
    # Sanity: the SPU part must look like 16-byte ADPCM frames.
    probe_bad = sum(1 for f in range(min(256, spu_size // 16))
                    if spu[f * 16 + 1] > 7 or (spu[f * 16] & 0xF) > 12
                    or (spu[f * 16] >> 4) > 4)
    if probe_bad > 16:
        raise SystemExit(f"SPU part fails ADPCM probe ({probe_bad}/256 bad "
                         "frames) — layout assumption broken")

    t, count, addrs = find_sound_block(lvl, spu_size)
    if t is None:
        raise SystemExit("SoundTable+SoundDefinition block not found")
    print(f"sound block at lvl+{t:#x}: {count} definitions")

    def def_at(idx: int):
        off = t + 0x104 + idx * SOUND_DEF_SIZE
        raw = lvl[off:off + SOUND_DEF_SIZE]
        m_addr, u4, u6, u8, uA, uC, uE = struct.unpack_from("<i6H", raw, 0)
        u10, u11, u12, u13 = struct.unpack_from("<4b", raw, 0x10)
        return {"index": idx, "m_Addr": m_addr,
                "m_Addr_postfixup": m_addr + 0x1010,
                "unk4": u4, "unk6": u6, "unk8": u8, "unkA": uA,
                "unkC": uC, "unkE": uE,
                "unk10": u10, "unk11": u11, "unk12": u12, "unk13": u13,
                "raw_hex": raw.hex()}

    chime_id = lvl[t + 0]        # SoundTable.gemChime
    arrival_id = lvl[t + 0x1A]   # byte 0x1A: id played on sparkle arrival
    if chime_id == 0xFF:
        raise SystemExit("gemChime absent (0xFF) in this level's SoundTable")
    chime = def_at(chime_id)
    arrival = def_at(arrival_id) if arrival_id != 0xFF else None
    if arrival is None:
        print("  note: SoundTable[0x1A] absent in this level; "
              "arrival chime falls back to gemChime for reporting")
        arrival_id, arrival = chime_id, chime
    print(f"gemChime: SoundTable[0] = def {chime_id}, "
          f"m_Addr {chime['m_Addr']:#x} (SPU {chime['m_Addr_postfixup']:#x})")
    print(f"arrival-chime (SoundTable[0x1A]) = def {arrival_id}, "
          f"m_Addr {arrival['m_Addr']:#x}"
          + ("  [same sample]" if arrival["m_Addr"] == chime["m_Addr"] else ""))

    def carve(d):
        start = d["m_Addr"]
        higher = [a for a in addrs if a > start]
        end_bound = min(higher) if higher else spu_size
        end, bad = adpcm_trim(spu, start, end_bound)
        return spu[start:end], end_bound, bad

    body, bound, bad = carve(chime)
    print(f"gemChime sample: {len(body)} bytes "
          f"(next-def bound {bound - chime['m_Addr']}, "
          f"{bad} suspicious ADPCM headers)")
    if not (512 <= len(body) <= 65536):
        print("  WARNING: sample size outside the expected few-KB range")
    (out_dir / "gem_chime.vag_body.bin").write_bytes(body)

    extra = {}
    if arrival["m_Addr"] != chime["m_Addr"]:
        abody, abound, abad = carve(arrival)
        (out_dir / "gem_chime_arrival.vag_body.bin").write_bytes(abody)
        extra["arrival_sample_bytes"] = len(abody)
        print(f"arrival sample differs: wrote gem_chime_arrival.vag_body.bin "
              f"({len(abody)} bytes, {abad} suspicious headers)")

    # The ACTUAL collect gling — bank def 12, canonical rate 11025 Hz
    # (SPU pitch 0x400). Identified by ear from a full-bank WAV dump
    # (2026-07-11): the asm-traced candidates were red herrings —
    # SoundTable[0x1A] is the menu blip and gemChime[0] the gem-bounce.
    COLLECT_DEF = 1
    collect = def_at(COLLECT_DEF)
    cbody, cbound, cbad = carve(collect)
    (out_dir / "gem_collect.vag_body.bin").write_bytes(cbody)
    extra["collect_def"] = collect
    extra["collect_sample_bytes"] = len(cbody)
    print(f"collect gling (def {COLLECT_DEF}): wrote gem_collect.vag_body.bin "
          f"({len(cbody)} bytes, {cbad} suspicious headers)")

    # First-frame header sanity report
    frames_ok = []
    for f in range(min(4, len(body) // 16)):
        b0 = body[f * 16]
        frames_ok.append({"shift": b0 & 0xF, "filter": (b0 >> 4) & 0x7,
                          "flags": body[f * 16 + 1]})

    meta = {
        "source": "SCUS-94467 lvl[0] (Sunrise Spring)",
        "sound_block_lvl_offset": t,
        "num_definitions": count,
        "sound_table_first_0x28_bytes": list(lvl[t:t + 0x28]),
        "gemChime_table_index": 0,
        "gemChime_def": chime,
        "arrival_chime_table_index": 0x1A,
        "arrival_chime_def": arrival,
        "sample_bytes": len(body),
        "sample_is_raw_spu_adpcm_no_vag_header": True,
        "first_frames": frames_ok,
        "notes": "m_Addr is pre-fixup offset into the vramSpu SPU part "
                 "(vramSpu.offset + 0x80000; the first 0x80000 bytes are "
                 "VRAM texture data); game adds 0x1010 (SPU upload base). "
                 "Sample end = next m_Addr bound, trimmed at ADPCM "
                 "end-flag frame (flags byte bit 0).",
        **extra,
    }
    (out_dir / "gem_chime.json").write_text(json.dumps(meta, indent=1))
    return chime, arrival, len(body)


# ---------------------------------------------------------------------------
# Pitch ladder documentation
# ---------------------------------------------------------------------------

PITCH_LADDER_MD = """\
# Spyro 3 gem-chime rising-pitch ladder (SCUS-94467)

Constants recovered from the spyro-3 decomp asm, 2026-07-11.

## Who plays what

Gem pickup does NOT play the chime directly.  The chain is:

1. **Collect** — `func_80036E60` (mobyutil, also inlined in each level
   overlay's gem handler, e.g. `func_level_50_800759B4`):
   - `value = D_80066980[moby+0x49]` with `D_80066980 = {1, 2, 5, 10, 25}`
     (denomination index 0-4 = red/green/blue/gold/pink).
   - Totals: `D_8006C71C` (g_GemTotal) and the per-level slot
     `D_80071A10[level]` both `+= value`.
   - Ring buffer: `D_8006E3B0[D_8006C6F8 & 0x1F] = moby+0x49` then
     `D_8006C6F8++` (running collected-gem counter, **zeroed on level load**
     in `func_8002CA50`; the ring index wraps `& 0x1F`).
   - Spawns the flying digit/sparkle mobys via `func_80036708`
     (moby class 0x104, one per decimal digit of the value; per-digit
     offsets from tables `D_800658A0`/`D_80065920 >> 4`; the shared
     per-burst state block gets `+0x4 = 0x60` (lifetime) and
     `+0x14 = 0x80` stepped `-0x10` per digit).
   - Marks the gem collected for saves via `func_8003BA00`
     (bit `1 << (mobyIndex + areaOffset)` in `unsavedData.collection`).

2. **Chime** — when a sparkle ARRIVES (per-level-overlay handler, e.g.
   `func_level_20_800827DC` state 3 / `func_level_50_80086F98`, identical
   code in every level overlay):
   ```
   handle = PlaySound(SoundTable[0x1A], sparkleMoby, 0);
   if (handle >= 0)
       func_8003C0B0(handle, sparkleMoby->0x49 << 8);
   ```
   - The sound id is **SoundTable byte 0x1A** (the slot spu.h names
     `changeVolume` — the pause-menu volume blip reuses the same chime),
     NOT byte 0 (`gemChime`).  Both ids are extracted; on the shipped
     Sunrise Spring table compare `gem_chime.json` to see whether they
     resolve to the same sample.
   - `sparkleMoby->0x49` here is the sparkle's **sequence number n**: at
     spawn each sparkle latches `n = (*counter)++` from a shared counter
     (`[mobyTag+0x14][0]`), so consecutive arrivals carry n, n+1, n+2, ...

3. **Pitch override** — `func_8003C0B0(handle, k)` (src/spu.c:30-33,
   MATCHING):
   ```
   g_ActiveSounds[handle].unk18 =
       (k * ((def.unkC + def.unkE) / 2)) >> 12;
   g_ActiveSounds[handle].unk2 |= 8;
   ```
   With `k = n << 8` the SPU voice pitch becomes

   **pitch(n) = n * centre / 16**, `centre = (unkC + unkE) / 2`

   i.e. the ladder rises **linearly by centre/16 (6.25 % of the sound's
   centre SPU pitch) per consecutive chime**.  `unkC`/`unkE` are the
   definition's pitch min/max (PlaySound randomises inside them when no
   override is set).

4. **Consumption** — flag 8 is one-shot: the voice updater
   (`func_8003C428`) copies `unk18` straight into the SPU voice pitch
   register (clamped to 0x3FFF elsewhere in that path) and clears the
   flag (`unk2 &= 0xF7`).

## Reset behaviour

- The pitch sequence is per **sparkle burst chain**: the shared counter
  lives in the sparkle group's tag block; when the chain dies (lifetime
  byte `+0x4 = 0x60` ticks out with no new gem feeding it) the next gem
  starts a fresh counter — that is the audible "ladder reset after a
  pause".
- `D_8006C6F8` (the &0x1F ring) never resets mid-level; it is the
  running collected counter (save/bookkeeping), not the pitch source.

## Remaining uncertainty

- The shared counter's initial value (and thus the first chime's n) is
  set up across `func_80036708` / the overlay handler's spawn path; the
  asm shows the latch-and-increment clearly but the initialisation site
  is tangled with the tag-block reuse.  If the S1-side mod needs the
  exact starting pitch, capture `func_8003C0B0`'s `k` at runtime in an
  emulator (breakpoint 0x8003C0B0) — every subsequent step is exactly
  `+0x100` in `k` (= centre/16 in SPU pitch).
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--disc", type=Path,
                    default=next(iter(sorted(
                        (REPO_ROOT.parent / "roms/SCUS-94467").glob("*.img"))),
                        None),
                    help=".img/.bin/.iso (CloneCD raw or cooked)")
    ap.add_argument("--out", type=Path,
                    default=REPO_ROOT / "mods/spyro3-gems/assets/s3")
    ap.add_argument("--level", type=int, default=0,
                    help="wadHeader lvl[] index (0 = Sunrise Spring)")
    args = ap.parse_args()
    if args.disc is None or not args.disc.exists():
        raise SystemExit("disc image not found — pass --disc")

    out_dir = args.out
    out_dir.mkdir(parents=True, exist_ok=True)

    disc = esp.Disc(args.disc)
    wad_lba, wad_size = esp.iso_find(disc, "WAD.WAD")
    print(f"WAD.WAD at LBA {wad_lba}, {wad_size} bytes")

    for level in (args.level, args.level + 1):
        try:
            lvl = read_level(disc, wad_lba, level)
            hdr = LevelWadHeader(lvl)
            slot, stats = extract_gem_model(lvl, hdr, out_dir)
            chime, arrival, nbytes = extract_gem_chime(lvl, hdr, out_dir)
            break
        except SystemExit as e:
            print(f"lvl[{level}] parse failed: {e}")
            if level != args.level:
                raise
    else:
        raise SystemExit("no level parsed")

    footer = (
        "\n## Extracted values (this disc, lvl[0] Sunrise Spring)\n\n"
        f"- gemChime (SoundTable[0]) = def {chime['index']}: "
        f"m_Addr {chime['m_Addr']:#x}, unk6 {chime['unk6']:#x}, "
        f"unkA {chime['unkA']:#x}, unkC {chime['unkC']:#x}, "
        f"unkE {chime['unkE']:#x} -> centre {(chime['unkC'] + chime['unkE']) // 2:#x}, "
        f"ladder step centre/16 = "
        f"{(chime['unkC'] + chime['unkE']) // 32:#x} SPU pitch units\n"
        f"- arrival chime (SoundTable[0x1A]) = def {arrival['index']}: "
        f"m_Addr {arrival['m_Addr']:#x}, unk6 {arrival['unk6']:#x}, "
        f"unkA {arrival['unkA']:#x}, unkC {arrival['unkC']:#x}, "
        f"unkE {arrival['unkE']:#x} -> centre "
        f"{(arrival['unkC'] + arrival['unkE']) // 2:#x}, ladder step "
        f"{(arrival['unkC'] + arrival['unkE']) // 32:#x} SPU pitch units\n"
        f"- chime sample: {nbytes} bytes of SPU-ADPCM "
        "(gem_chime.vag_body.bin)\n"
    )
    (out_dir / "pitch_ladder.md").write_text(PITCH_LADDER_MD + footer)
    print("wrote pitch_ladder.md")
    print("done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
