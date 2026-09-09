"""PSYQ .LIB / .OBJ (LNK) container reader.

A .LIB is a flat sequence of entries: 8-char member name, three dwords (the
third is the stride from the ENTRY start to the next entry, not from the data
start), a length-prefixed table of the member's exported symbol names, then the
LNK object itself.

A LNK object is a stream of tagged records. Only the ones that place bytes or
name an offset matter here; the rest must still be skipped exactly, because a
single mis-sized skip desynchronises everything after it. Unknown tags raise
rather than being stepped over, so a gap in this table shows up as a parse
failure on a named member instead of as silently missing symbols.
"""
import struct, re, zipfile
from collections import defaultdict


class Desync(Exception):
    pass


def _u8(d, p):  return d[p], p + 1
def _u16(d, p): return struct.unpack_from('<H', d, p)[0], p + 2
def _u32(d, p): return struct.unpack_from('<I', d, p)[0], p + 4


def _pstr(d, p):
    n = d[p]
    return d[p + 1:p + 1 + n].decode('ascii', 'replace'), p + 1 + n


def _expr(d, p):
    """Relocation expression tree. Returns the position after it."""
    op, p = _u8(d, p)
    if op == 0x00:            # literal
        _, p = _u32(d, p)
    elif op in (0x02, 0x04, 0x0c, 0x16):   # symbol / section base / start / end
        _, p = _u16(d, p)
    elif op in (0x2c, 0x2e, 0x32):         # add / sub / div
        p = _expr(d, p)
        p = _expr(d, p)
    else:
        raise Desync("expr op 0x%02x at %d" % (op, p - 1))
    return p


def parse_lnk(d):
    """-> (sections, xdefs, locals)
    sections: {index: bytearray}; xdefs/locals: [(name, section, offset)]."""
    if d[:4] != b'LNK\x02':
        raise Desync("not LNK")
    p = 4
    sections = defaultdict(bytearray)
    cur = None
    xdefs, locs = [], []
    n = len(d)
    while p < n:
        tag, p = _u8(d, p)
        if tag == 0x00:                       # end
            break
        elif tag == 0x02:                     # code bytes into current section
            ln, p = _u16(d, p)
            sections[cur] += d[p:p + ln]
            p += ln
        elif tag == 0x06:                     # switch section
            cur, p = _u16(d, p)
        elif tag == 0x08:                     # uninitialised space
            ln, p = _u32(d, p)
            sections[cur] += b'\x00' * ln
        elif tag == 0x0a:                     # relocation
            _, p = _u8(d, p)                  # patch kind
            _, p = _u16(d, p)                 # offset in section
            p = _expr(d, p)
        elif tag == 0x0c:                     # exported symbol
            _, p = _u16(d, p)                 # symbol index
            sec, p = _u16(d, p)
            off, p = _u32(d, p)
            nm, p = _pstr(d, p)
            xdefs.append((nm, sec, off))
        elif tag == 0x0e:                     # imported symbol
            _, p = _u16(d, p)
            _, p = _pstr(d, p)
        elif tag == 0x10:                     # section definition
            idx, p = _u16(d, p)
            _, p = _u16(d, p)                 # group
            _, p = _u8(d, p)                  # alignment
            _, p = _pstr(d, p)
            sections.setdefault(idx, bytearray())
        elif tag == 0x12:                     # local symbol
            sec, p = _u16(d, p)
            off, p = _u32(d, p)
            nm, p = _pstr(d, p)
            locs.append((nm, sec, off))
        elif tag == 0x1c:                     # source file name
            _, p = _u16(d, p)
            _, p = _pstr(d, p)
        elif tag == 0x2e:                     # processor type
            _, p = _u8(d, p)
        elif tag == 0x30:                     # uninitialised symbol
            _, p = _u16(d, p)
            _, p = _u16(d, p)
            _, p = _u32(d, p)
            _, p = _pstr(d, p)
        elif tag == 0x32:                     # SLD line-number base
            _, p = _u32(d, p)
        elif tag in (0x34, 0x36):             # SLD line increments
            pass
        elif tag == 0x38:
            _, p = _u32(d, p)
        elif tag in (0x3a, 0x3c, 0x3e, 0x40, 0x42, 0x44, 0x46, 0x48):
            raise Desync("unhandled debug tag 0x%02x at %d" % (tag, p - 1))
        else:
            raise Desync("tag 0x%02x at %d" % (tag, p - 1))
    return sections, xdefs, locs


def read_lib(data):
    """-> [(member_name, exported_syms, lnk_bytes)]"""
    if data[:4] != b'LIB\x01':
        return []
    out = []
    off = 4
    n = len(data)
    while off + 20 <= n:
        name = data[off:off + 8].rstrip(b' \x00').decode('ascii', 'replace')
        _, _, size = struct.unpack_from('<III', data, off + 8)
        if size <= 20 or off + size > n:
            break
        body = data[off + 20:off + size]
        k = body.find(b'LNK\x02')
        syms = []
        if k > 0:
            p = 0
            while p < k:
                L = body[p]
                if L == 0 or p + 1 + L > k:
                    break
                s = body[p + 1:p + 1 + L]
                if re.fullmatch(rb'[A-Za-z_][A-Za-z0-9_]*', s):
                    syms.append(s.decode())
                p += 1 + L
        out.append((name, syms, bytes(body[k:]) if k >= 0 else b''))
        # No alignment padding between entries: an entry may begin at an odd
        # offset, and rounding up desynchronises every entry after it.
        off += size
    return out


def load_all(path):
    """Read every .LIB in a directory tree or a .zip. -> {libname: members}

    The PSYQ SDK is Sony proprietary and is not distributed with this project,
    so the location is always supplied by the developer.
    """
    import os
    libs = {}
    if os.path.isdir(path):
        for root, _, files in os.walk(path):
            for fn in files:
                if fn.lower().endswith('.lib'):
                    with open(os.path.join(root, fn), 'rb') as fh:
                        libs[fn.upper()] = read_lib(fh.read())
    else:
        z = zipfile.ZipFile(path)
        for entry in z.namelist():
            if entry.lower().endswith('.lib'):
                libs[entry.split('/')[-1].upper()] = read_lib(z.read(entry))
    return libs
