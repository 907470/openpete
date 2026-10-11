#!/usr/bin/env python3
"""Census of loops in psxrecomp output, by what a loop body can and cannot do.

A back-edge is a `goto block_T` inside a block at address B with T <= B; the
loop body is every emitted block in [T, B]. The classes:

  call-free            the body reaches no function, thunk, syscall or
                       register-indirect call, so nothing outside the guest
                       can run while it spins;
  store-free           the body writes no guest RAM (a void channel
                       replace_call replaces a store and counts as one);
  with a load          the body reads guest RAM, so it can be a poll;
  pure spin            no register carries a value from one iteration to the
                       next except through a load (a bare poll: which
                       iteration an interrupt lands on is unobservable);
  counted poll         some register is updated from its own previous value
                       (a timeout, a table index: interrupt placement is
                       observable);
  VSync reach          for a loop with calls, whether psyq_VSync is in the
                       body, reachable through direct callees, unknown
                       (a register-indirect call somewhere on the path), or
                       unreachable.

A constant `call_by_address(cpu, 0xADDRu)` is a direct call: to the PSYQ
symbol at that address when the game's psyq.toml names it, else to the
generated function at that address.

Usage:
  tools/loop_census.py GAME [--list CLASS] [--range LO HI] FILE...
  GAME is spyro-1 | spyro-2 | spyro-3 (selects games/GAME/manifest/psyq.toml).
  --list CLASS prints the members of one class: pure, counted, novsync,
  callpoll (call-containing, store-free, back-edge condition on v0),
  emit (the emission criterion: store-free, every call to a pure leaf, and a
  load in the body or a leaf; a pure leaf is a generated function with no
  store, no call in any form and no back-edge; insns adds each called leaf's
  count per call; rows carry the back-edge source block and the leaves).
  --range LO HI restricts to functions whose address is in [LO, HI).
"""
import argparse, collections, re, sys

FUNC  = re.compile(r'\nvoid (\w+?)_([0-9A-Fa-f]{8})\(CPUState\* cpu\)\n\{')
BLK   = re.compile(r'^block_([0-9A-Fa-f]{8}):', re.M)
GOTO  = re.compile(r'goto block_([0-9A-Fa-f]{8})')
CALLF = re.compile(r'\b(func_\w+)\(cpu\)')
CALLP = re.compile(r'\b(psyq_\w+)\(cpu\)')
CALLC = re.compile(r'call_by_address\(cpu, 0x([0-9A-Fa-f]{8})u\)')
CALLR = re.compile(r'call_by_address\(cpu, cpu->|psx_syscall\(')
STORE = re.compile(r'cpu->write_|cpu->sw[lr]\(|channel replace_call \(void\)')
LOAD  = re.compile(r'cpu->read_|cpu->lw[lr]\(')
INSN  = re.compile(r'/\* 0x[0-9A-Fa-f]{8}: 0x[0-9A-Fa-f]{8} \*/')
ASSIGN= re.compile(r'\s*cpu->(\w+) = (.*);')
COND  = re.compile(r'int _bc_\w+ = \(([^;]*)\);')


def load_psyq(game):
    names = {}
    try:
        text = open(f'games/{game}/manifest/psyq.toml').read()
    except OSError:
        return names
    for m in re.finditer(r'name = "(\w+)"\naddr = (0x[0-9a-fA-F]+)', text):
        names[int(m.group(2), 16)] = m.group(1)
    return names


def carried_register(text):
    """A register assigned in the body and read before its first assignment."""
    defined = set()
    for line in text.splitlines():
        m = ASSIGN.match(line)
        if not m:
            continue
        r, rhs = m.group(1), m.group(2)
        if r not in defined and r not in ('zero', 'lo', 'hi') \
                and re.search(r'\bcpu->%s\b' % r, rhs):
            return True
        defined.add(r)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('game')
    ap.add_argument('files', nargs='+')
    ap.add_argument('--list', choices=['pure', 'counted', 'novsync', 'callpoll', 'emit'])
    ap.add_argument('--range', nargs=2)
    a = ap.parse_args()
    lo, hi = (int(a.range[0], 16), int(a.range[1], 16)) if a.range else (0, 1 << 32)
    psyq = load_psyq(a.game)

    bodies, byaddr, loops, looping = {}, {}, [], set()
    for path in a.files:
        src = open(path).read()
        fs = [(m.start(), m.group(1) + '_' + m.group(2), int(m.group(2), 16))
              for m in FUNC.finditer(src)]
        for i, (pos, fname, faddr) in enumerate(fs):
            end = fs[i + 1][0] if i + 1 < len(fs) else len(src)
            body = src[pos:end]
            bodies[fname] = body
            byaddr.setdefault(faddr, fname)
            blks = [(int(m.group(1), 16), m.start()) for m in BLK.finditer(body)]
            bounds = {}
            for j, (addr, st) in enumerate(blks):
                en = blks[j + 1][1] if j + 1 < len(blks) else len(body)
                bounds[addr] = (st, en)
            order = [addr for addr, _ in blks]
            if any(int(g.group(1), 16) <= sa for sa in order
                   for g in GOTO.finditer(body[bounds[sa][0]:bounds[sa][1]])):
                looping.add(fname)
            if not (lo <= faddr < hi):
                continue
            for sa in order:
                st, en = bounds[sa]
                for g in GOTO.finditer(body[st:en]):
                    t = int(g.group(1), 16)
                    if t > sa:
                        continue
                    span = [x for x in order if t <= x <= sa]
                    text = ''.join(body[bounds[x][0]:bounds[x][1]] for x in span)
                    loops.append((path.split('/')[-1], fname, t, sa, text, body[st:en]))

    def pure_leaf(f):
        b = bodies.get(f)
        return (b is not None and f not in looping and not STORE.search(b)
                and not CALLF.search(b) and not CALLP.search(b)
                and not CALLC.search(b) and not CALLR.search(b))

    def emit_row(text):
        """(insns, leaves) when the body meets the emission criterion, else None."""
        if STORE.search(text) or CALLP.search(text) or CALLR.search(text):
            return None
        calls = CALLF.findall(text)
        for h in CALLC.findall(text):
            addr = int(h, 16)
            if addr in psyq or addr not in byaddr:
                return None
            calls.append(byaddr[addr])
        if not all(pure_leaf(c) for c in calls):
            return None
        if not (LOAD.search(text) or any(LOAD.search(bodies[c]) for c in calls)):
            return None
        n = len(INSN.findall(text)) + sum(len(INSN.findall(bodies[c])) for c in calls)
        return n, calls

    def calls_of(text):
        fs = set(CALLF.findall(text))
        ps = set(CALLP.findall(text))
        ind = bool(CALLR.search(text))
        for h in CALLC.findall(text):
            addr = int(h, 16)
            if addr in psyq:
                ps.add('psyq_' + psyq[addr])
            elif addr in byaddr:
                fs.add(byaddr[addr])
            else:
                ind = True
        return fs, ps, ind

    callees = {f: calls_of(b) for f, b in bodies.items()}
    memo = {}

    def reach(f, stack=()):
        if f in memo:
            return memo[f]
        if f in stack:
            return (False, False)
        if f not in callees:
            memo[f] = (False, True)
            return memo[f]
        fs, ps, ind = callees[f]
        vs, unk = 'psyq_VSync' in ps, ind
        for c in fs:
            v, u = reach(c, stack + (f,))
            vs, unk = vs or v, unk or u
        memo[f] = (vs, unk)
        return memo[f]

    cnt = collections.Counter()
    listed = []
    thunks = collections.Counter()
    for src, fname, t, sa, text, tail in loops:
        n = len(INSN.findall(text))
        if a.list == 'emit':
            row = emit_row(text)
            if row:
                cnt['emission sites (pure-leaf calls)'] += 1
                listed.append((src, fname, t, row[0], 'src=0x%08X %s' % (sa, ','.join(row[1]) or '-')))
        fs, ps, ind = calls_of(text)
        cnt['back-edges'] += 1
        if fs or ps or ind:
            cnt['with a call'] += 1
            if 'psyq_VSync' in ps:
                cnt['  VSync in body'] += 1
                continue
            vs, unk = False, ind
            for c in fs:
                v, u = reach(c)
                vs, unk = vs or v, unk or u
            if vs:
                cnt['  VSync via callees'] += 1
            elif unk:
                cnt['  unknown (indirect call)'] += 1
            else:
                cnt['  no path to VSync'] += 1
                for p in ps:
                    thunks[p] += 1
                if a.list == 'novsync':
                    listed.append((src, fname, t, n, ','.join(sorted(fs | ps))))
            if a.list == 'callpoll' and not STORE.search(text):
                m = COND.search(tail)
                if m and 'cpu->v0' in m.group(1) and n <= 20:
                    listed.append((src, fname, t, n, ','.join(sorted(fs | ps))))
            continue
        cnt['call-free'] += 1
        if STORE.search(text):
            continue
        cnt['call-free, store-free'] += 1
        if not LOAD.search(text):
            continue
        cnt['  with a load (emission sites)'] += 1
        kind = 'counted' if carried_register(text) else 'pure'
        cnt['    ' + kind] += 1
        if a.list == kind:
            listed.append((src, fname, t, n, ''))

    print(a.game)
    for k in ('back-edges', 'with a call', '  VSync in body', '  VSync via callees',
              '  unknown (indirect call)', '  no path to VSync', 'call-free',
              'call-free, store-free', '  with a load (emission sites)',
              '    pure', '    counted', 'emission sites (pure-leaf calls)'):
        print('  %-34s %d' % (k, cnt[k]))
    print('  thunks called from loops with no path to VSync:',
          ', '.join('%s x%d' % kv for kv in thunks.most_common()))
    for src, fname, t, n, extra in listed:
        print('  %-30s %-42s hdr=0x%08X insns=%-3d %s' % (src, fname, t, n, extra))


if __name__ == '__main__':
    main()
