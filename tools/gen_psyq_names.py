#!/usr/bin/env python3
"""Recover PSYQ function names for a decomp's asm/psyq.s from a PSYQ SDK.

A decomp that splats the PSYQ runtime without symbols names every library
function func_<addr>. openpete's PSYQ path is name-keyed end to end -- psyq.toml
carries names, psxrecomp emits psyq_<name>, and the thunk layer overrides by
name -- so an unnamed library function reaches no thunk and aborts at its first
call.

Each library member's LNK object exports symbols with a section and an offset,
which anchors a name to an exact code position. A decomp function is named when
its whole body matches the library code at such a position. Immediates that a
relocation rewrites are masked out, since the same library code linked at a
different base differs only in those fields; branch displacements are
PC-relative and are compared exactly, because masking them would let distinct
short functions collide.

Three outcomes are reported separately rather than as one hit rate:

  named      starts at an exported symbol -- these are what thunks key on
  static     verifies inside a member but at no exported offset. A
             library-internal function; it needs no name because nothing links
             to it by one, and it is recompiled like ordinary code
  unmatched  no member verifies, with the reason given

Usage:
    gen_psyq_names.py <asm/psyq.s> <psyq-sdk-dir-or-zip> <out.toml>
    gen_psyq_names.py <asm/psyq.s> <psyq-sdk> --report
"""
import re
import struct
import sys
from collections import defaultdict, Counter

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from psyq_lib import load_all, parse_lnk

# The listing column holds the instruction's bytes in memory order and must be
# byte-swapped to recover the word. Spyro 1's psyq.s is unindented where Spyro
# 2's and 3's are indented, so the pattern must not anchor at the line start:
# anchoring it at one silently matches nothing in the other and reports a
# confident zero.
INSN = re.compile(r'^\s*/\* [0-9A-Fa-f]+ ([0-9A-Fa-f]{8}) ([0-9A-Fa-f]{8}) \*/')
GLAB = re.compile(r'^glabel\s+(\S+)')

# Opcodes whose 16-bit immediate can carry half of a relocated address.
IMM16 = {0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
         0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
         0x28, 0x29, 0x2a, 0x2b, 0x2e, 0x31, 0x32, 0x35, 0x36, 0x39, 0x3a,
         0x3d, 0x3e}

NGRAM = 8


def mask(w):
    op = w >> 26
    if op in (0x02, 0x03):      # j/jal: absolute target, fixed at link time
        return w & 0xFC000000
    if op in IMM16:
        return w & 0xFFFF0000
    return w


def mseq(ws):
    return tuple(mask(w) for w in ws)


def parse_psyq_s(path):
    """-> {label: (vaddr, [words])}"""
    out, cur = {}, None
    for line in open(path, encoding='utf-8', errors='replace'):
        g = GLAB.match(line)
        if g:
            cur = g.group(1)
            out[cur] = [None, []]
            continue
        m = INSN.match(line)
        if m and cur:
            if out[cur][0] is None:
                out[cur][0] = int(m.group(1), 16)
            out[cur][1].append(
                struct.unpack('<I', struct.pack('>I', int(m.group(2), 16)))[0])
    return {k: (v[0], v[1]) for k, v in out.items() if v[1]}


def build_sections(sdk):
    """-> (store, index). store[k] = (lib, member, section, words, {byte_off: name})"""
    store, index = [], defaultdict(list)
    for lib, members in sorted(load_all(sdk).items()):
        for member, _syms, blob in members:
            if not blob:
                continue
            try:
                secs, xdefs, _ = parse_lnk(blob)
            except Exception:
                continue
            for si, b in secs.items():
                n = len(b) // 4
                if n < NGRAM:
                    continue
                ws = list(struct.unpack('<%dI' % n, bytes(b[:n * 4])))
                k = len(store)
                store.append((lib, member, si, ws,
                              {o: nm for nm, s, o in xdefs if s == si}))
                mw = [mask(w) for w in ws]
                for i in range(n - NGRAM + 1):
                    index[tuple(mw[i:i + NGRAM])].append((k, i))
    return store, index


def locate(funcs, store, index):
    named, static, unmatched = {}, {}, []
    for label, (vaddr, ws) in funcs.items():
        if len(ws) < NGRAM:
            unmatched.append((label, vaddr, 'below the %d-instruction floor' % NGRAM))
            continue
        full = mseq(ws)
        hits = []
        for k, i in index.get(mseq(ws[:NGRAM]), ()):
            lib, member, _si, lws, xd = store[k]
            if i + len(ws) <= len(lws) and mseq(lws[i:i + len(ws)]) == full:
                hits.append((lib, member, xd.get(i * 4)))
        if not hits:
            unmatched.append((label, vaddr, 'no library member matches the body'))
            continue
        names = {h[2] for h in hits if h[2]}
        if len(names) == 1:
            named[label] = (names.pop(), hits[0][0], hits[0][1])
        elif len(names) > 1:
            unmatched.append((label, vaddr, 'matches several symbols: '
                              + ','.join(sorted(names))))
        else:
            static[label] = (hits[0][0], hits[0][1])

    # A symbol claimed by more than one function identifies none of them: short
    # functions differing only in a masked immediate are indistinguishable by
    # this method. Demote every claimant rather than picking one.
    contested = {n for n, c in Counter(v[0] for v in named.values()).items() if c > 1}
    for label in [l for l, v in named.items() if v[0] in contested]:
        nm = named.pop(label)[0]
        unmatched.append((label, funcs[label][0],
                          'symbol %s is claimed by several functions' % nm))
    return named, static, unmatched


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    psyq_s, sdk = sys.argv[1], sys.argv[2]
    out = sys.argv[3] if len(sys.argv) > 3 else '--report'

    funcs = parse_psyq_s(psyq_s)
    if not funcs:
        sys.exit('error: no functions parsed from %s -- check the listing format'
                 % psyq_s)
    store, index = build_sections(sdk)
    if not store:
        sys.exit('error: no library sections read from %s' % sdk)
    named, static, unmatched = locate(funcs, store, index)

    # Where the decomp already carries a hand-recovered name, the matcher must
    # agree with it. A disagreement means the matcher is wrong, not the human.
    known = {k for k in funcs if not k.startswith(('func_', 'D_', 'jtbl_', '_'))}
    agree = sorted(k for k in known & set(named) if named[k][0] == k)
    differ = sorted((k, named[k][0]) for k in known & set(named) if named[k][0] != k)

    sys.stderr.write(
        "%s: %d functions; named %d, library-internal %d, unmatched %d\n"
        % (psyq_s, len(funcs), len(named), len(static), len(unmatched)))
    sys.stderr.write("  calibration against %d pre-existing names: %d agree, %d differ\n"
                     % (len(known), len(agree), len(differ)))
    for k, got in differ:
        sys.stderr.write("    DISAGREES: decomp says %s, matcher says %s\n" % (k, got))
    if differ:
        sys.exit('error: matcher disagrees with a hand-recovered name; '
                 'do not trust its output until this is explained')
    if not named:
        sys.exit('error: named zero functions -- vacuous, not a clean result')

    if out == '--report':
        for lib, c in sorted(Counter(v[1] for v in named.values()).items()):
            sys.stderr.write("  %-14s %d\n" % (lib, c))
        for label, (nm, lib, member) in sorted(named.items(),
                                               key=lambda x: funcs[x[0]][0]):
            print("0x%08x  %-28s %s::%s" % (funcs[label][0], nm, lib, member))
        return

    with open(out, 'w', encoding='utf-8') as fh:
        fh.write("# Auto-generated by tools/gen_psyq_names.py from %s\n"
                 % psyq_s.split('/')[-1])
        fh.write("# PSYQ library function names recovered by signature match.\n"
                 "# Addresses absent here are library-internal or unmatched and\n"
                 "# keep their generated func_<addr> name.\n\n")
        for label, (nm, lib, member) in sorted(named.items(),
                                               key=lambda x: funcs[x[0]][0]):
            fh.write("[[names]]\naddr = 0x%08x\nname = \"%s\"\nfrom = \"%s::%s\"\n\n"
                     % (funcs[label][0], nm, lib, member))
    sys.stderr.write("wrote %s (%d names)\n" % (out, len(named)))


if __name__ == '__main__':
    main()
