#!/usr/bin/env python3
"""Static store enumeration on Spyro 3's cutscene trigger paths.

Each Spyro 3 story cutscene starts with a call WarpToLevel func_800584BC(6,
id) made by a trigger function (the title update, a moby update, GoToLevel
func_80053F50). For every row this tool lists each store the recompiled C
can make on a path from the trigger function's entry to that call site:

  1. The trigger's own blocks: every block reachable from the entry that can
     reach the block holding the call, following gotos, fallthroughs, split
     pieces (a `func_<piece>(cpu); return;` or `call_by_address(cpu, X);
     return;` transfer) and jump-table cases. Stores in the call block after
     the call are dropped.
  2. Every function those blocks call, and transitively every function the
     callees call: all stores of each callee's body (bounds from its .s under
     the decomp's asm/nonmatchings/), callees resolved in the trigger's own
     overlay first, then in PSX.EXE.

Stack stores (base register sp) are dropped. A store's address is resolved
when its base register was loaded with a constant (`lui`, the g_SonyImage
redirect, a following `addiu`) earlier in the same block; its value when the
stored register holds a constant (`addiu`/`ori` from zero, a `lui` pair, or
zero). Unresolved bases print as the register and offset. The enumeration
over-approximates (every store on any path) and does not follow indirect
calls (`call_by_address(cpu, cpu->reg)`: a jalr, or a jump table's
out-of-range default); each is listed.

  tools/cutscene_store_enum.py [--game-dir games/spyro-3] [--row ID ...]

Reads games/spyro-3/generated/ and games/spyro-3/spyro-3/asm/nonmatchings/.
"""
import argparse
import collections
import glob
import os
import re
import sys

# (ids, overlay or None for PSX.EXE, trigger entry, pc of the jal to
# WarpToLevel). One row per trigger call site; GoToLevel carries three.
ROWS = [
    ((61,), "title", 0x8007566C, 0x80077A40),
    ((62,), None, 0x80053F50, 0x80054374),
    ((64,), None, 0x80053F50, 0x800543A0),
    ((66,), None, 0x80053F50, 0x800543C8),
    ((63,), "level_10", 0x8007F0BC, 0x8007F6E4),
    ((65,), "level_20", 0x8008098C, 0x80080F94),
    ((67,), "level_30", 0x8007D3C8, 0x8007DAD4),
    ((68,), "level_37", 0x800818EC, 0x8008212C),
    ((69,), "level_47", 0x8007CF38, 0x8007E280),
    ((74,), "level_50", 0x8008B434, 0x8008C440),
    ((77,), "level_10", 0x8007EEBC, 0x8007F094),
    ((78,), "level_20", 0x8007D224, 0x8007D3FC),
    ((79,), "level_30", 0x8007E7D8, 0x8007E9B0),
    ((80,), "level_40", 0x8007BFC0, 0x8007C198),
]
WARP_TO_LEVEL = 0x800584BC
SONY_IMAGE_BASE = 0x8006FCF4
OVERLAY_BASE = 0x80074000   # overlays load above the EXE's text and data

FNA = re.compile(r'^void (\w+)\(CPUState\* cpu\)')
BL = re.compile(r'^block_([0-9A-F]{8}):;')
GOTO = re.compile(r'goto block_([0-9A-F]{8});')
PIECE = re.compile(r'\b(func_\w*?([0-9A-F]{8}))\(cpu\);')
CALLC = re.compile(r'call_by_address\(cpu, 0x([0-9A-F]{8})u\)')
CALLR = re.compile(r'call_by_address\(cpu, (cpu->\w+)\)')
PSYQ = re.compile(r'\b(psyq_\w+|psx_syscall)\(cpu')
WR = re.compile(r'write_(byte|half|word)\(cpu->(\w+)(?: \+ (-?\d+))?, (?:\(uint\d+_t\))?cpu->(\w+)\)')
PC = re.compile(r'/\* 0x([0-9A-F]{8}): 0x')
SIZE = re.compile(r'/\* Address: 0x[0-9A-F]{8}, Size: (\d+) bytes')


class Block:
    __slots__ = ("addr", "fn", "items", "succ", "dead", "depth", "lines", "size")

    def __init__(self, addr, fn):
        self.addr, self.fn, self.size = addr, fn, 0
        self.items, self.succ, self.lines = [], [], []
        self.dead, self.depth = False, 0


def strip_comments(line):
    return re.sub(r'/\*.*?\*/', '', line).strip()


def parse(path):
    """Blocks of one generated C file, keyed by guest address."""
    blocks = collections.OrderedDict()
    cur, fn, size = None, None, 0
    with open(path) as f:
        for ln, line in enumerate(f, 1):
            m = FNA.match(line)
            if m:
                fn, cur, size = m.group(1), None, 0
                continue
            m = SIZE.search(line)
            if m and cur is None:
                size = int(m.group(1))
                continue
            m = BL.match(line)
            if m:
                cur = Block(int(m.group(1), 16), fn)
                cur.size, size = size, 0
                blocks[cur.addr] = cur
                continue
            if cur is None:
                continue
            if line.startswith('}'):
                cur = None
                continue
            s = strip_comments(line)
            if not s or cur.dead or 'psx_override_dispatch' in s:
                continue
            scan_line(cur, line, s, ln)
    order = list(blocks)
    for a, b in zip(order, order[1:]):
        if not blocks[a].dead and blocks[a].fn == blocks[b].fn:
            blocks[a].succ.append(b)
    return blocks


def scan_line(cur, line, s, ln):
    cur.lines.append((ln, s))
    pcm = PC.search(line)
    pc = int(pcm.group(1), 16) if pcm else None
    for g in GOTO.findall(s):
        cur.succ.append(int(g, 16))
    transfer = re.search(r'\breturn;', s) is not None
    if '/* jal */' in line:
        m = PIECE.search(s)
        if m:
            cur.items.append(('call', int(m.group(2), 16), ln, pc))
    elif 'call_by_address' not in s:
        for m in PIECE.finditer(s):
            cur.succ.append(int(m.group(2), 16))
    m = CALLC.search(s)
    if m:
        if transfer:
            cur.succ.append(int(m.group(1), 16))   # split-piece transfer
        else:
            cur.items.append(('call', int(m.group(1), 16), ln, pc))
    elif CALLR.search(s):
        cur.items.append(('icall', CALLR.search(s).group(1), ln, pc))
    m = PSYQ.search(s)
    if m:
        cur.items.append(('lib', m.group(1), ln, pc))
    for w in WR.finditer(s):
        cur.items.append(('store', w.groups(), ln, pc))
    opens, closes = s.count('{'), s.count('}')
    if s.startswith('if (cpu->') and opens == closes:
        return
    depth = cur.depth
    nd = depth + opens - closes
    if depth == 0 and (s.startswith('goto ') or transfer):
        cur.dead = True
    if depth > 0 and nd == 0 and s == '}':
        cur.dead = True    # if/else or switch: every arm transfers
    cur.depth = nd


def reach(blocks, starts, lo, hi, rev=False):
    g = collections.defaultdict(list)
    for a, b in blocks.items():
        for s in b.succ:
            if lo <= s < hi and s in blocks:
                (g[s] if rev else g[a]).append(s if not rev else a)
    seen, st = set(starts), list(starts)
    while st:
        for y in g[st.pop()]:
            if y not in seen and lo <= y < hi:
                seen.add(y)
                st.append(y)
    return seen


CALLER_SAVED = {'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2',
                't3', 't4', 't5', 't6', 't7', 't8', 't9', 'ra'}
UNKNOWN = object()


class Scope:
    """The blocks a resolution may walk back through (a path or a body), with
    their predecessors inside it."""

    def __init__(self, blocks, addrs):
        self.blocks = blocks
        self.preds = collections.defaultdict(list)
        for a in addrs:
            for t in blocks[a].succ:
                if t in addrs:
                    self.preds[t].append(a)


def reg_const(scope, addr, ln, reg, depth=0):
    """The constant `reg` holds before line `ln` of block `addr`, or None.
    With no definition in the block, every predecessor in the scope must
    agree."""
    adds, val = 0, UNKNOWN
    for _, s in reversed([x for x in scope.blocks[addr].lines if x[0] < ln]):
        m = re.match(r'cpu->%s = cpu->%s \+ (-?\d+);' % (reg, reg), s)
        if m:
            adds += int(m.group(1))
            continue
        m = re.match(r'cpu->%s = cpu->%s \| 0x([0-9A-Fa-f]+);' % (reg, reg), s)
        if m:
            adds |= int(m.group(1), 16)   # an ori after a lui: disjoint bits
            continue
        m = re.match(r'cpu->%s = 0x([0-9A-F]+) << 16;' % reg, s)
        if m:
            val = int(m.group(1), 16) << 16
            break
        m = re.match(r'cpu->%s = \(uint32_t\)\(psx_runtime_g_sony_image_base \+ 0x([0-9A-F]+)u\) - \(uint32_t\)(\d+);' % reg, s)
        if m:
            val = SONY_IMAGE_BASE + int(m.group(1), 16) - int(m.group(2))
            break
        m = re.match(r'cpu->%s = (-?\d+);' % reg, s)
        if m:
            val = int(m.group(1))
            break
        if re.match(r'cpu->%s = cpu->zero;' % reg, s):
            val = 0
            break
        if re.match(r'cpu->%s = ' % reg, s):
            return None
        if reg in CALLER_SAVED and ('call_by_address' in s or PIECE.search(s)) \
                and 'return;' not in s:
            return None
    if val is UNKNOWN:
        preds = scope.preds.get(addr, [])
        if not preds or depth > 8:
            return None
        vals = {reg_const(scope, p, 1 << 30, reg, depth + 1) for p in preds}
        if len(vals) != 1 or None in vals:
            return None
        val = vals.pop()
    return (val + adds) & 0xFFFFFFFF


def fmt_value(c):
    return ('%d' % c if c < 0x10000 else '0x%08X' % c) if c is not None else None


def describe_store(scope, addr, item):
    (width, base, off, src), ln, pc = item[1], item[2], item[3]
    off = int(off or 0)
    if base == 'sp':
        return None
    b = reg_const(scope, addr, ln, base) if base != 'zero' else 0
    where = ('0x%08X' % ((b + off) & 0xFFFFFFFF)) if b is not None else \
        ('*(%s%+d)' % (base, off))
    v = '0' if src == 'zero' else (fmt_value(reg_const(scope, addr, ln, src)) or src)
    return '%-5s %-14s = %-10s  pc 0x%08X L%d' % (width, where, v, pc or 0, ln)


class Game:
    def __init__(self, game_dir):
        self.gen = os.path.join(game_dir, 'generated')
        self.asm_root = os.path.join(game_dir, 'spyro-3', 'asm', 'nonmatchings')
        self.files = {}
        self.bounds = {}
        for f in glob.glob(os.path.join(self.asm_root, '**', 'func_*.s'), recursive=True):
            rel = os.path.relpath(f, self.asm_root).split(os.sep)
            ovl = rel[1] if rel[0] == 'overlays' else None
            m = re.search(r'([0-9A-F]{8})\.s$', f)
            if not m:
                continue
            with open(f) as fh:
                pcs = re.findall(r'^/\* [0-9A-F]+ ([0-9A-F]{8}) [0-9A-F]{8} \*/', fh.read(), re.M)
            if pcs:
                self.bounds[(ovl, int(m.group(1), 16))] = int(pcs[-1], 16) + 4
        self.map_bounds(os.path.join(game_dir, 'spyro-3', 'build', 'psx.map'))

    def map_bounds(self, path):
        """PSX.EXE functions with no .s (decompiled ones, the libraries): from
        the link map, each text symbol ending at the next symbol's address."""
        if not os.path.exists(path):
            return
        addrs = set()
        sym = re.compile(r'^\s+0x00000000(8[0-9a-f]{7})\s+(\w+)(\.NON_MATCHING)?$')
        with open(path) as fh:
            for line in fh:
                m = sym.match(line)
                if m and not m.group(2).startswith(('D_', 'jtbl_', 'jpt_')):
                    addrs.add(int(m.group(1), 16))
        order = sorted(a for a in addrs if 0x80012000 <= a < OVERLAY_BASE)
        for a, b in zip(order, order[1:]):
            self.bounds.setdefault((None, a), b)

    def blocks(self, ovl):
        if ovl not in self.files:
            name = 'PSX.EXE_full.c' if ovl is None else '%s/%s.bin_full.c' % (ovl, ovl)
            self.files[ovl] = parse(os.path.join(self.gen, name))
        return self.files[ovl]

    def locate(self, ovl, addr):
        """(overlay, end) of the function at addr: the trigger's overlay for
        an address above the EXE, else PSX.EXE. A function with no .s ends
        where its generated body's header says."""
        o = ovl if addr >= OVERLAY_BASE else None
        if (o, addr) in self.bounds:
            return o, self.bounds[(o, addr)]
        blk = self.blocks(o).get(addr)
        if blk is not None and blk.size:
            return o, addr + blk.size
        return o, None


def list_items(blocks, addrs, stop=None):
    """Stores, calls and unresolved calls of `addrs` in address order;
    `stop` = (block, line) drops that block's items from the line on. A call
    line carries its constant arguments."""
    scope = Scope(blocks, set(addrs))
    out, callees = [], []
    for a in sorted(addrs):
        for it in blocks[a].items:
            if stop and a == stop[0] and it[2] >= stop[1]:
                continue
            if it[0] == 'store':
                d = describe_store(scope, a, it)
                if d:
                    out.append('  store ' + d)
            elif it[0] == 'call':
                argv = ['%s=%s' % (r, fmt_value(reg_const(scope, a, it[2], r)))
                        for r in ('a0', 'a1') if reg_const(scope, a, it[2], r) is not None]
                out.append('  call  0x%08X  L%d %s' % (it[1], it[2], ' '.join(argv)))
                callees.append(it[1])
            else:
                out.append('  %-5s %s  L%d' % (it[0], it[1], it[2]))
    return out, list(dict.fromkeys(callees))


def enumerate_callees(game, ovl, callees, printed):
    queue = list(callees)
    while queue:
        fn = queue.pop(0)
        if fn in printed or fn == WARP_TO_LEVEL:
            continue
        printed.add(fn)
        o, hi = game.locate(ovl, fn)
        if hi is None:
            print('callee 0x%08X: no .s (library or unsplit code): not enumerated' % fn)
            continue
        blocks = game.blocks(o)
        if fn not in blocks:
            print('callee 0x%08X: no generated body in %s: not enumerated' % (fn, o or 'PSX.EXE'))
            continue
        body = reach(blocks, [fn], fn, hi)
        items, sub = list_items(blocks, body)
        stores = [i for i in items if i.startswith('  store')]
        print('callee 0x%08X [%s 0x%08X..0x%08X]: %d stores%s' % (
            fn, o or 'PSX.EXE', fn, hi, len(stores),
            ('; calls ' + ' '.join('0x%08X' % c for c in sub)) if sub else ''))
        for i in items:
            if not i.startswith('  call'):
                print('  ' + i)
        queue.extend(sub)


def run_row(game, row):
    ids, ovl, entry, call = row
    key = (ovl, entry)
    if key not in game.bounds:
        sys.exit('no .s bounds for 0x%08X in %s' % (entry, ovl or 'PSX.EXE'))
    hi = game.bounds[key]
    blocks = game.blocks(ovl)
    target = 'cpu->ra = 0x%08X;' % (call + 8)
    cb = callline = None
    for a, b in blocks.items():
        if entry <= a < hi:
            for ln, s in b.lines:
                if s.startswith(target):
                    cb, callline = a, ln
                    break
        if cb:
            break
    if cb is None:
        sys.exit('row %s: no call at 0x%08X' % (ids, call))
    on = reach(blocks, [entry], entry, hi) & reach(blocks, [cb], entry, hi, rev=True)
    args = {r: reg_const(Scope(blocks, on), cb, callline, r) for r in ('a0', 'a1')}
    print('== row %s: %s entry 0x%08X, call 0x%08X (L%d) WarpToLevel(%s, %s); '
          '%d blocks on the path' % (
              '/'.join(map(str, ids)), ovl or 'PSX.EXE', entry, call, callline,
              args['a0'], args['a1'], len(on)))
    items, callees = list_items(blocks, on, stop=(cb, callline))
    for i in items:
        print(i)
    enumerate_callees(game, ovl, callees, set())
    print()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--game-dir', default='games/spyro-3')
    ap.add_argument('--row', type=int, action='append', default=[])
    a = ap.parse_args()
    game = Game(a.game_dir)
    for row in ROWS:
        if not a.row or set(row[0]) & set(a.row):
            run_row(game, row)


if __name__ == '__main__':
    main()
