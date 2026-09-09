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

Some functions in the PSYQ text are not library members at all: __main is
emitted by the compiler, so no .LIB carries it and no signature match can name
it. --reference names those from another decomp's psyq.s that already carries
a human-recovered name, matched by the same masked-body comparison.

Usage:
    gen_psyq_names.py <asm/psyq.s> <psyq-sdk-dir-or-zip> <out.toml>
                      [--reference <other/asm/psyq.s> ...]
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
    if op == 0x0f:              # lui: the high half of a relocated address
        return w & 0xFFFF0000
    return w


def mask_seq(ws):
    """Mask the fields a relocation rewrites, and only those.

    A 16-bit immediate is link-dependent only when it is the low half of a
    relocated address: the instruction after a `lui` into the same register, or
    a $gp-relative access. Every other immediate is a struct field offset or a
    stack displacement, identical however the code is linked, and masking it
    discards the field that tells otherwise-identical functions apart -- PSYQ's
    callback trampolines differ from each other in nothing else.
    """
    out = []
    tainted = set()          # registers currently holding a lui'd address half
    for w in ws:
        op = w >> 26
        rs = (w >> 21) & 0x1F
        rt = (w >> 16) & 0x1F
        m = mask(w)
        # $gp-relative access, or the %lo half of an address a lui put in rs.
        # The pair is not always adjacent -- the compiler schedules other
        # instructions between them -- so the taint lasts until rs is written
        # by something other than that lui.
        if op in IMM16 and (rs == 28 or rs in tainted):
            m = w & 0xFFFF0000
        out.append(m)
        if op == 0x0f:
            tainted.add(rt)
        elif op == 0x09 and rs in tainted:
            # addiu completes lui/addiu: rt now holds a finished address, so a
            # later load through it is still relocation-dependent.
            tainted.add(rt)
        elif op in IMM16:
            # Any other I-type writing rt leaves a loaded or computed value
            # there, not an address half. A load through it afterwards carries
            # a struct offset, which must stay in the comparison.
            tainted.discard(rt)
        elif op == 0:
            rd = (w >> 11) & 0x1F
            tainted.discard(rd)
    return tuple(out)


def mseq(ws):
    """Permissive: mask every 16-bit immediate.

    A library member is unrelocated, so an address half reads as 0 there and as
    a real value in the linked game. Masking all of them matches across that
    difference reliably; it also discards struct offsets, which makes some
    short functions indistinguishable. Those are separated by sseq below rather
    than by tightening this one, which loses genuine matches.
    """
    out = []
    for w in ws:
        op = w >> 26
        out.append(w & 0xFFFF0000 if op in IMM16 else mask(w))
    return tuple(out)


def sseq(ws):
    """Strict: mask only what a relocation can actually rewrite.

    Keeps struct offsets and stack displacements, which is what tells PSYQ's
    callback trampolines apart -- they differ in nothing but the field offset
    they load through.
    """
    return mask_seq(ws)


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
                # Index with the same masking the lookup uses; per-word
                # masking is not equivalent once the mask is context-sensitive.
                for i in range(n - NGRAM + 1):
                    index[mseq(ws[i:i + NGRAM])].append((k, i))
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
                hits.append((lib, member, xd.get(i * 4),
                             sseq(lws[i:i + len(ws)])))
        if not hits:
            unmatched.append((label, vaddr, 'no library member matches the body'))
            continue
        named_hits = [h for h in hits if h[2]]
        names = {h[2] for h in named_hits}
        if len(names) > 1:
            # Several symbols share this body under the permissive mask.
            # Re-compare keeping the fields a relocation cannot rewrite.
            strict = sseq(ws)
            refined = [h for h in named_hits
                       if h[3] is not None and h[3] == strict]
            if len({h[2] for h in refined}) == 1:
                names = {refined[0][2]}
                hits = refined
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


def name_from_reference(funcs, named, ref_path):
    """Name still-unnamed functions from a reference psyq.s's real labels.

    Some functions in the PSYQ text are not library members: __main is emitted
    by the compiler, so no .LIB carries it and no signature match can reach it.
    Another decomp that already carries the human-recovered label supplies it,
    matched by the same masked-body comparison used against the libraries.
    """
    ref = parse_psyq_s(ref_path)
    index, strict_index = {}, {}
    for label, (_va, ws) in ref.items():
        if label.startswith(('func_', 'D_', 'jtbl_')) or len(ws) < NGRAM:
            continue
        index.setdefault(mseq(ws), []).append(label)
        strict_index.setdefault(sseq(ws), []).append(label)
    added = 0
    for label, (_va, ws) in funcs.items():
        if label in named or len(ws) < NGRAM:
            continue
        hits = index.get(mseq(ws))
        if hits and len(set(hits)) > 1:
            # Separate them on the fields a relocation cannot rewrite.
            strict = strict_index.get(sseq(ws))
            if strict and len(set(strict)) == 1:
                hits = strict
        if hits and len(set(hits)) == 1:
            named[label] = (hits[0], 'reference', ref_path)
            added += 1
    return added


def main():
    argv = sys.argv[1:]
    refs = []
    while '--reference' in argv:
        i = argv.index('--reference')
        refs.append(argv[i + 1])
        del argv[i:i + 2]
    if len(argv) < 2:
        sys.exit(__doc__)
    psyq_s, sdk = argv[0], argv[1]
    out = argv[2] if len(argv) > 2 else '--report'

    funcs = parse_psyq_s(psyq_s)
    if not funcs:
        sys.exit('error: no functions parsed from %s -- check the listing format'
                 % psyq_s)
    store, index = build_sections(sdk)
    if not store:
        sys.exit('error: no library sections read from %s' % sdk)
    named, static, unmatched = locate(funcs, store, index)

    for ref in refs:
        n = name_from_reference(funcs, named, ref)
        sys.stderr.write("  %d name(s) from reference %s\n" % (n, ref))
    # A reference name claimed by more than one function identifies none of
    # them, exactly as for a library match: short stubs differing only in a
    # masked immediate are indistinguishable by body.
    contested = {n for n, c in Counter(v[0] for v in named.values()).items() if c > 1}
    for label in [l for l, v in named.items() if v[0] in contested]:
        nm = named.pop(label)[0]
        unmatched.append((label, funcs[label][0],
                          'symbol %s is claimed by several functions' % nm))

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
