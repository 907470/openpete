#!/usr/bin/env python3
"""Spyro 1 memory-card reader for tests/harness/check_modsave.sh.

The card holds one file, BASCUS-94228SPYRO, whose directory frame names
its first block; the file is a 0x200-byte header (title, clut, icon)
followed by three 0x600-byte SaveFile records. Per record: the body
0x000..0x58C is summed into m_Checksum at 0x58C, and the pad 0x590..0x600
is outside the checksum. A record is empty when m_LevelsVisited[0]
(record byte 0x40) is zero, the title's predicate.

Commands (each prints one line per fact and exits 0, or 1 on a failed
assertion, 2 on an unreadable card):
  records CARD                  index, empty, checksum, pad magic, pad hash
  same-outside-pads A B         byte-identical except the three pads
  same-records A B [--pads]     the three record bodies (and pads) identical;
                                the file header (icon) is not compared
  same A B                      byte-identical cards
  pad-magic CARD I MAGIC        record I's pad starts with MAGIC ("zero" for
                                an all-zero pad)
  flip CARD OUT I OFS           copy CARD to OUT with record I byte OFS
                                inverted (the checksum is left stale)
"""
import hashlib
import sys

FILE_NAME = b"BASCUS-94228SPYRO"
BLOCK = 0x2000
REC0 = 0x200
REC = 0x600
CKS = 0x58C
PAD = 0x590


def file_offset(card):
    """Card offset of the save file's first block, from the directory frames."""
    for b in range(1, 16):
        d = card[0x80 * b:0x80 * b + 0x80]
        if d[0] == 0x51 and d[10:10 + len(FILE_NAME)] == FILE_NAME:
            return BLOCK * b
    return None


def load(path):
    with open(path, "rb") as f:
        card = f.read()
    off = file_offset(card)
    if off is None:
        print(f"{path}: no {FILE_NAME.decode()} file")
        sys.exit(2)
    return card, off


def record(card, off, i):
    a = off + REC0 + i * REC
    return card[a:a + REC]


def pad_spans(off):
    return [(off + REC0 + i * REC + PAD, off + REC0 + (i + 1) * REC) for i in range(3)]


def cmd_records(path):
    card, off = load(path)
    for i in range(3):
        r = record(card, off, i)
        cks_ok = sum(r[:CKS]) == int.from_bytes(r[CKS:CKS + 4], "little")
        pad = r[PAD:]
        magic = "zero" if not any(pad) else pad[:4].decode("latin-1")
        print(f"record {i} empty {int(r[0x40] == 0)} checksum {'ok' if cks_ok else 'bad'} "
              f"pad {magic} padhash {hashlib.sha256(pad).hexdigest()[:16]} "
              f"bodyhash {hashlib.sha256(r[:PAD]).hexdigest()[:16]}")
    return 0


def cmd_same_outside_pads(a, b):
    ca, oa = load(a)
    cb, ob = load(b)
    if len(ca) != len(cb) or oa != ob:
        print(f"layout differs: {len(ca)}@{oa:#x} vs {len(cb)}@{ob:#x}")
        return 1
    spans = pad_spans(oa)
    diff = [i for i in range(len(ca)) if ca[i] != cb[i]
            and not any(s <= i < e for s, e in spans)]
    print(f"bytes differing outside the pads: {len(diff)}"
          + (f" (first {diff[0]:#x})" if diff else ""))
    return 1 if diff else 0


def cmd_same_records(a, b, pads):
    ca, oa = load(a)
    cb, ob = load(b)
    end = REC if pads else PAD
    bad = [i for i in range(3) if record(ca, oa, i)[:end] != record(cb, ob, i)[:end]]
    print(f"records differing ({'with' if pads else 'without'} pads): {bad if bad else 'none'}")
    return 1 if bad else 0


def cmd_same(a, b):
    with open(a, "rb") as f:
        ca = f.read()
    with open(b, "rb") as f:
        cb = f.read()
    n = sum(1 for x, y in zip(ca, cb) if x != y) + abs(len(ca) - len(cb))
    print(f"bytes differing: {n}")
    return 1 if n else 0


def cmd_pad_magic(path, i, magic):
    card, off = load(path)
    pad = record(card, off, int(i))[PAD:]
    got = "zero" if not any(pad) else pad[:4].decode("latin-1")
    print(f"record {i} pad {got} (want {magic})")
    return 0 if got == magic else 1


def cmd_flip(path, out, i, ofs):
    card, off = load(path)
    a = off + REC0 + int(i) * REC + int(ofs, 0)
    b = bytearray(card)
    b[a] ^= 0xFF
    with open(out, "wb") as f:
        f.write(b)
    print(f"flipped card byte {a:#x} (record {i} +{int(ofs, 0):#x})")
    return 0


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 64
    cmd, args = argv[1], argv[2:]
    if cmd == "records" and len(args) == 1:
        return cmd_records(*args)
    if cmd == "same-outside-pads" and len(args) == 2:
        return cmd_same_outside_pads(*args)
    if cmd == "same-records" and len(args) in (2, 3):
        return cmd_same_records(args[0], args[1], len(args) == 3 and args[2] == "--pads")
    if cmd == "same" and len(args) == 2:
        return cmd_same(*args)
    if cmd == "pad-magic" and len(args) == 3:
        return cmd_pad_magic(*args)
    if cmd == "flip" and len(args) == 4:
        return cmd_flip(*args)
    print(__doc__)
    return 64


if __name__ == "__main__":
    sys.exit(main(sys.argv))
