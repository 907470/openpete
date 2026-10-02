#!/usr/bin/env python3
"""nsf.py - read the Crash Bandicoot 3 demo's NSF archives straight off
the Spyro 1 disc image and expose the entries a model converter needs.

The Spyro 1 disc (SCUS-94228) ships a Crash 3 demo under S0/: CRASH.EXE
plus two NSF/NSD pairs. This module never needs the files staged on disk:
it walks the ISO9660 directory (raw 2352-byte or cooked 2048-byte
sectors), pulls S0/<name>.NSF and parses it.

NSF layout (Crash 2/3 engine; format reference: cbhacks/CrashEdit):
  * 64 KiB chunks. u16 magic 0x1234 (0x1235 = LZ-compressed; the demo's
    chunks are all raw and this reader refuses 0x1235), u16 type at +2:
    0 = entry chunk, 1 = texture page, 3 = sound chunk.
  * Entry chunk: u32 entry count at +8, entry offsets from +16. Entry:
    u32 magic 0x100FFFF, eID, type, item count, then count+1 item offsets
    (entry-relative) - item k spans [off[k], off[k+1]).
    Types: 1 = SVTX (animation frames), 2 = TGEO (model), 7 = zone,
    11 = GOOL (object code + frame groups), 12 = sound.
  * Texture page chunk: EID at +4; the whole 64 KiB is raw VRAM bytes,
    read as 512 bytes per row x 128 rows (256 16-bit texels per row).
  * eID: 5 characters x 6 bits over "0-9a-zA-Z_!", low bit set.

GOOL entries matter here only for their frame groups (item 5): a
VertexGroup3 record {u16 1, u16 frames, u32 lerp, u32 number, u32 model
eID, u32 anim eID} is the ONLY link from an SVTX animation to the TGEO
model it poses - Crash 3 frames carry no model eID of their own.
"""
import struct
from pathlib import Path

SEC_DATA = 2048
SEC_RAW = 2352
SEC_RAW_SKEW = 24

CHUNK_LEN = 65536
CHUNK_MAGIC = 0x1234
CHUNK_MAGIC_LZ = 0x1235
CHUNK_ENTRY = 0
CHUNK_TEXTURE = 1
CHUNK_SOUND = 3
ENTRY_MAGIC = 0x100FFFF

ET_SVTX = 1
ET_TGEO = 2
ET_GOOL = 11

ENAME_CHARS = "0123456789abcdefghijklmnopqrstuvwxyz" \
              "ABCDEFGHIJKLMNOPQRSTUVWXYZ_!"

DEMO_DIR = "S0"
DEMO_NSFS = ("S000000D.NSF", "S000003C.NSF")


def eid_to_name(eid):
    out = []
    e = eid >> 1
    for _ in range(5):
        out.append(ENAME_CHARS[e & 0x3F])
        e >>= 6
    return "".join(reversed(out))


def name_to_eid(name):
    if len(name) != 5:
        raise ValueError(f"eName must be 5 chars: {name!r}")
    eid = 1
    for i, ch in enumerate(name):
        eid |= ENAME_CHARS.index(ch) << (25 - 6 * i)
    return eid


# ---------------------------------------------------------------- disc

class Disc:
    """Sector reader over a .bin/.iso; raw (2352) or cooked (2048)."""

    def __init__(self, path):
        path = Path(path)
        if path.suffix.lower() == ".cue":
            path = resolve_cue(path)
        self.f = open(path, "rb")
        probe = self.f.read(12)
        self.raw = probe[:2] == b"\x00\xff" and probe[11:12] == b"\x00"

    def sector(self, n):
        off = n * SEC_RAW + SEC_RAW_SKEW if self.raw else n * SEC_DATA
        self.f.seek(off)
        return self.f.read(SEC_DATA)

    def span(self, lba, size):
        out = bytearray()
        n = lba
        while len(out) < size:
            out += self.sector(n)
            n += 1
        return bytes(out[:size])

    def _listdir(self, lba, size):
        data = self.span(lba, size)
        pos = 0
        ents = {}
        while pos < len(data):
            rl = data[pos]
            if rl == 0:                       # pad to the next sector
                pos = (pos // SEC_DATA + 1) * SEC_DATA
                continue
            nl = data[pos + 32]
            name = data[pos + 33:pos + 33 + nl].decode("ascii", "replace")
            name = name.split(";")[0].upper()
            ents[name] = (struct.unpack_from("<I", data, pos + 2)[0],
                          struct.unpack_from("<I", data, pos + 10)[0])
            pos += rl
        return ents

    def read_file(self, path):
        """ISO9660 lookup of 'DIR/NAME' (any depth) -> bytes."""
        pvd = self.sector(16)
        if pvd[1:6] != b"CD001":
            raise SystemExit("no ISO9660 PVD at sector 16")
        lba = struct.unpack_from("<I", pvd, 158)[0]
        size = struct.unpack_from("<I", pvd, 166)[0]
        ents = self._listdir(lba, size)
        parts = path.upper().split("/")
        for p in parts[:-1]:
            if p not in ents:
                raise FileNotFoundError(f"{p} not in ISO directory")
            lba, size = ents[p]
            ents = self._listdir(lba, size)
        if parts[-1] not in ents:
            raise FileNotFoundError(f"{path} not on disc")
        lba, size = ents[parts[-1]]
        return self.span(lba, size)


def resolve_cue(cue):
    for line in cue.read_text().splitlines():
        line = line.strip()
        if line.upper().startswith("FILE"):
            name = line.split('"')[1] if '"' in line else line.split()[1]
            return cue.parent / name
    raise SystemExit(f"no FILE line in {cue}")


# ----------------------------------------------------------------- nsf

class Entry:
    __slots__ = ("eid", "type", "items", "chunk")

    def __init__(self, eid, etype, items, chunk):
        self.eid, self.type, self.items, self.chunk = eid, etype, items, chunk

    @property
    def name(self):
        return eid_to_name(self.eid)


class VertexGroup3:
    """GOOL frame group type 1 (Crash 3): one animation posed on one model."""
    __slots__ = ("frames", "lerp", "number", "model_eid", "anim_eid")

    def __init__(self, frames, lerp, number, model_eid, anim_eid):
        self.frames, self.lerp, self.number = frames, lerp, number
        self.model_eid, self.anim_eid = model_eid, anim_eid


class NSF:
    def __init__(self, data, label=""):
        self.label = label
        self.entries = {}       # eid -> Entry (first occurrence wins)
        self.pages = {}         # eid -> 64 KiB bytes
        self.dupes = 0
        self.n_chunks = len(data) // CHUNK_LEN
        for ci in range(self.n_chunks):
            ch = data[ci * CHUNK_LEN:(ci + 1) * CHUNK_LEN]
            magic, ctype = struct.unpack_from("<HH", ch, 0)
            if magic == CHUNK_MAGIC_LZ:
                raise SystemExit(f"{label} chunk {ci}: LZ chunks unsupported")
            if magic != CHUNK_MAGIC:
                continue
            if ctype == CHUNK_TEXTURE:
                eid = struct.unpack_from("<I", ch, 4)[0]
                self.pages.setdefault(eid, ch)
                continue
            if ctype == CHUNK_SOUND:
                pass  # sound entries parse like any other entry chunk
            n = struct.unpack_from("<I", ch, 8)[0]
            for i in range(n):
                eo = struct.unpack_from("<I", ch, 16 + 4 * i)[0]
                magic, eid, etype, nitems = struct.unpack_from("<4I", ch, eo)
                if magic != ENTRY_MAGIC:
                    continue
                offs = [struct.unpack_from("<I", ch, eo + 16 + 4 * k)[0]
                        for k in range(nitems + 1)]
                items = [ch[eo + offs[k]:eo + offs[k + 1]]
                         for k in range(nitems)]
                if eid in self.entries:
                    self.dupes += 1
                    continue
                self.entries[eid] = Entry(eid, etype, items, ci)

    def of_type(self, etype):
        return [e for e in self.entries.values() if e.type == etype]

    def get(self, key):
        eid = key if isinstance(key, int) else name_to_eid(key)
        return self.entries.get(eid)

    def vertex_groups(self):
        """Every VertexGroup3 across the GOOL entries, in entry order."""
        out = []
        for g in self.of_type(ET_GOOL):
            if len(g.items) < 6:
                continue
            out += parse_frame_groups(g.items[5])
        return out


def parse_frame_groups(data):
    """Walk GOOL item 5 (Crash 3 = GOOL version 3 groups) and return the
    VertexGroup3 records; the other group kinds are skipped by size."""
    groups = []
    i = 0
    n = len(data)
    while i + 2 < n:
        t = struct.unpack_from("<h", data, i)[0]
        if t == 1:
            if i + 20 > n:
                break
            _, frames, lerp, num, model, anim = struct.unpack_from("<hhIIII", data, i)
            groups.append(VertexGroup3(frames, lerp != 0, num, model, anim))
            i += 20
        elif t == 2:      # sprite group: hdr 8 + 16/frame
            fc = struct.unpack_from("<h", data, i + 2)[0]
            i += 8 + 16 * fc
        elif t == 3:      # font group: hdr 8 + 20/frame
            fc = struct.unpack_from("<h", data, i + 2)[0]
            i += 8 + 20 * fc
        elif t == 4:      # text group: hdr 12 + NUL-terminated strings
            fc = struct.unpack_from("<h", data, i + 2)[0]
            i += 12
            for _ in range(fc):
                end = data.find(b"\0", i)
                i = n if end < 0 else end + 1
        elif t == 5:      # image group: hdr 12 + 24/part/frame
            fc = struct.unpack_from("<h", data, i + 2)[0]
            parts = struct.unpack_from("<I", data, i + 8)[0]
            i += 12 + 24 * parts * fc
        else:
            i += 4        # unknown group kind: CrashEdit's loop guard
    return groups


def load_demo(disc_path, names=DEMO_NSFS):
    """Read every demo NSF off the disc -> [NSF]."""
    disc = Disc(disc_path)
    return [NSF(disc.read_file(f"{DEMO_DIR}/{nm}"), nm) for nm in names]


class Library:
    """All demo NSFs merged: models/anims/pages by eid, anim->model map."""

    def __init__(self, nsfs):
        self.nsfs = nsfs
        self.models = {}
        self.anims = {}
        self.pages = {}
        self.groups = []
        for nsf in nsfs:
            for e in nsf.of_type(ET_TGEO):
                self.models.setdefault(e.eid, e)
            for e in nsf.of_type(ET_SVTX):
                self.anims.setdefault(e.eid, e)
            for eid, pg in nsf.pages.items():
                self.pages.setdefault(eid, pg)
            self.groups += nsf.vertex_groups()

    def anims_for(self, model_eid):
        """SVTX entries posing this model, GOOL order, present ones only."""
        seen, out = set(), []
        for g in self.groups:
            if g.model_eid == model_eid and g.anim_eid in self.anims \
                    and g.anim_eid not in seen:
                seen.add(g.anim_eid)
                out.append(self.anims[g.anim_eid])
        return out

    def model(self, key):
        eid = key if isinstance(key, int) else name_to_eid(key)
        return self.models.get(eid)

    def anim(self, key):
        eid = key if isinstance(key, int) else name_to_eid(key)
        return self.anims.get(eid)


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(description="census of the Crash 3 demo NSFs")
    ap.add_argument("--disc", required=True, help=".cue/.bin of SCUS-94228")
    args = ap.parse_args()
    lib = Library(load_demo(args.disc))
    for nsf in lib.nsfs:
        print(f"{nsf.label}: {nsf.n_chunks} chunks, {len(nsf.entries)} entries, "
              f"{len(nsf.pages)} texture pages, {nsf.dupes} duplicate entries")
    print(f"models {len(lib.models)}, anims {len(lib.anims)}, pages "
          f"{len(lib.pages)} ({' '.join(eid_to_name(e) for e in lib.pages)}), "
          f"vertex groups {len(lib.groups)}")
    for eid, m in sorted(lib.models.items(), key=lambda kv: eid_to_name(kv[0])):
        info = m.items[0]
        nv = struct.unpack_from("<I", info, 0x38)[0]
        npoly = struct.unpack_from("<I", info, 0x44)[0]
        ntex = struct.unpack_from("<I", info, 0x34)[0]
        anims = ", ".join(f"{a.name}x{len(a.items)}" for a in lib.anims_for(eid))
        print(f"  {m.name}: {nv:4d} verts {npoly:4d} polys {ntex:4d} texrecs "
              f"items={len(m.items)} anims=[{anims}]")
