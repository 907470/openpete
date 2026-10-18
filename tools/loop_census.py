#!/usr/bin/env python3
"""Census of loops in psxrecomp output, by what a loop body can and cannot do.

Each function's emitted C is read back as a region graph: one region per
`block_X:` label, an edge per `goto block_T` to a label of the function, and
an edge into the next region when a region does not end in a goto or a
return. A loop site is a goto from a region reachable from the entry to a
label T at or below the region's own label, where T is reachable from the
entry and reaches the source region. The body is T plus every region that T
reaches and that reaches the source without passing through T. The classes:

  call-free            the body reaches no function, thunk, syscall or
                       register-indirect call, so nothing outside the guest
                       can run while it spins;
  store-free           no body instruction is a store (sb sh swl sw swr swc2);
  with a load          some body instruction is a load to a register other
                       than $zero (lb lh lwl lw lbu lhu lwr, lwc2);
  emission sites       the recompiler's psx_loop_poll criterion: no store, no
                       syscall, register-indirect or thunk call, every call
                       to a pure leaf (a generated function of the same unit,
                       or of the main EXE, with no store, no call in any form
                       and no back-edge), and a load in the body or a called
                       leaf; insns counts body instructions plus each called
                       leaf's per call;
  pure / counted       an emission site is counted when a register live into
                       the header over the body (read on a path from the
                       header before any body definition, with the edges back
                       into the header removed) is defined in the body, pure
                       otherwise; a region holding a call defines at, v0, v1,
                       a0-a3 and t0-t9;
  VSync reach          for a body with calls, whether psyq_VSync is in the
                       body, reachable through direct callees, unknown
                       (a register-indirect call somewhere on the path), or
                       unreachable;
  call-carrying spins  the recompiler's kind 3 and 4 sites: a back-edge that
                       is no emission site and no device-status poll (kind 2
                       in the unit's <unit>_loop_sites.tsv), whose body stores nothing, makes
                       no PSYQ, syscall or register call itself, carries no
                       register, and calls only generated functions whose
                       call closure is quiet (every call resolves, a kernel
                       call excepted: a syscall or a `jr $t2` with t2 set to
                       0xA0, 0xB0 or 0xC0), reaches no psyq_VSync and no
                       device-status thunk (CdSync, CdReady,
                       SpuIsTransferCompleted, MemCardSync), and does not
                       hold the function of the loop (recursion); the
                       callees' closures make at least one kernel call. A main
                       EXE call to a target outside it is deferred (kind 4)
                       and resolved here against every overlay unit given
                       that defines it.

A constant `call_by_address(cpu, 0xADDRu)` is a direct call: to the PSYQ
symbol at that address when the game's psyq.toml names it, else to the
generated function at that address in the same unit or the main EXE.

Usage:
  tools/loop_census.py GAME [--list CLASS] [--range LO HI] FILE...
  GAME is spyro-1 | spyro-2 | spyro-3 (selects games/GAME/manifest/psyq.toml).
  FILE is a <unit>_full.c; the file named PSX.EXE_full.c among them supplies
  the main EXE's pure leaves to the others. The whole game is
  games/GAME/generated/PSX.EXE_full.c games/GAME/generated/*/*.bin_full.c
  (the _main_exe_pass1 copy of PSX.EXE_full.c is not one of its units).
  --list CLASS prints the members of one class: pure, counted (the emission
  sites split by carry), novsync, callpoll (call-containing, store-free,
  back-edge condition on v0), emit (every emission site with its source
  region, arm, carry verdict and called leaves, the columns of the
  recompiler's <unit>_loop_sites.tsv), callspin (every call-carrying spin
  with its kind, callees and, for a deferred one, each overlay's verdict).
  --range LO HI restricts to functions whose address is in [LO, HI).
"""
import argparse, collections, os, re

FUNC   = re.compile(r'^void (\w+?)_([0-9A-Fa-f]{8})\(CPUState\* cpu\)\n\{\n(.*?)^\}\n', re.M | re.S)
LBL    = re.compile(r'^block_([0-9A-F]{8}):;', re.M)
GOTO   = re.compile(r'goto block_([0-9A-F]{8});\s*/\* (taken|not taken|j|continue after call|'
                    r'continue after jalr) \*/|case \w+: goto block_([0-9A-F]{8});')
INSN   = re.compile(r'/\* 0x[0-9A-F]{8}: 0x([0-9A-F]{8}) \*/')
LCALL  = re.compile(r'(?<![A-Za-z0-9_])func_\w*?([0-9A-F]{8})\(cpu\)|'
                    r'call_by_address\(cpu, 0x([0-9A-Fa-f]{8})u\)')
PSYQ   = re.compile(r'\b(psyq_\w+)\(cpu')
OPAQUE = re.compile(r'call_by_address\(cpu, cpu->|psx_syscall|call_by_address\(cpu, [a-z]')
JTDEF  = re.compile(r'default: call_by_address\(cpu, cpu->\w+\); return;')
KVEC   = re.compile(r'cpu->t2 = (160|176|192);')
KFN    = re.compile(r'cpu->t1 = \d+;')
TOK    = re.compile(r'(?<![A-Za-z0-9_])cpu->([A-Za-z0-9_]+)')
DEF    = re.compile(r'\s*cpu->(\w+) = (.*)$')
COND   = re.compile(r'int _bc_\w+ = \(([^;]*)\);')
ARM    = {'taken': 'taken', 'not taken': 'not-taken', 'j': 'j',
          'continue after call': 'continue', 'continue after jalr': 'continue'}
REGS   = {'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5',
          't6', 't7', 's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0',
          'k1', 'gp', 'sp', 'fp', 'ra', 'hi', 'lo'}
CALLDEF = {'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5',
           't6', 't7', 't8', 't9'}
STORE_OPS = {0x28, 0x29, 0x2A, 0x2B, 0x2E, 0x3A}


def load_psyq(game):
    names = {}
    try:
        text = open(f'games/{game}/manifest/psyq.toml').read()
    except OSError:
        return names
    for m in re.finditer(r'name = "(\w+)"\naddr = (0x[0-9a-fA-F]+)', text):
        names[int(m.group(2), 16)] = m.group(1)
    return names


def strip(text):
    return re.sub(r'/\*.*?\*/', '', text, flags=re.S)


def is_load(w):
    op = w >> 26
    return (0x20 <= op <= 0x26 and (w >> 16) & 31 != 0) or op == 0x32


def regions(body, psyq):
    """Split a function body at its labels into regions with their facts."""
    ms = list(LBL.finditer(body))
    out = []
    for i, m in enumerate(ms):
        end = ms[i + 1].start() if i + 1 < len(ms) else len(body)
        raw = body[m.end():end]
        st = strip(raw)
        lines = [l.strip() for l in st.split('\n') if l.strip() and l.strip() != ';']
        last = lines[-1] if lines else ''
        falls = not (last.startswith('goto') or last.startswith('return')
                     or last == '}' or last.endswith('return;'))
        gotos = [(int(g.group(1), 16), ARM[g.group(2)]) if g.group(1)
                 else (int(g.group(3), 16), 'case') for g in GOTO.finditer(raw)]
        words = [int(w, 16) for w in INSN.findall(raw)]
        calls, ps = [], set(PSYQ.findall(st))
        for a, b in LCALL.findall(st):
            addr = int(a or b, 16)
            if b and addr in psyq:
                ps.add('psyq_' + psyq[addr])
            else:
                calls.append(addr)
        rest = JTDEF.sub('', st)
        kread = 'cpu->t2 = 176;' in st and 'cpu->t1 = 92;' in st
        if kread:
            # B(5Ch) _card_status reads kernel RAM: a load, as the recompiler counts it.
            st = st.replace('call_by_address(cpu, cpu->t2)', 'kernel_read(cpu)')
        if KVEC.search(rest) and KFN.search(rest):
            rest = rest.replace('call_by_address(cpu, cpu->t2)', '')
        rest = rest.replace('psx_syscall', '')
        kcall = kread or 'psx_syscall' in st or (KVEC.search(st) is not None and KFN.search(st) is not None
                                                  and 'call_by_address(cpu, cpu->t2)' in st)
        out.append(dict(label=int(m.group(1), 16), raw=raw, text=st, gotos=gotos,
                        falls=falls, calls=calls, psyq=ps, kernel=kcall,
                        opaque=bool(OPAQUE.search(st)),
                        unknown=bool(OPAQUE.search(rest)),
                        store=any(w >> 26 in STORE_OPS for w in words),
                        load=kread or any(is_load(w) for w in words), insns=len(words)))
    return out


def graph(regs):
    idx = {r['label']: i for i, r in enumerate(regs)}
    succ = [[idx[g] for g, _ in r['gotos'] if g in idx]
            + ([i + 1] if r['falls'] and i + 1 < len(regs) else [])
            for i, r in enumerate(regs)]
    pred = [[] for _ in regs]
    for i, ss in enumerate(succ):
        for s in ss:
            pred[s].append(i)
    return idx, succ, pred


def reach_from(succ, x):
    seen, work = {x}, [x]
    while work:
        for s in succ[work.pop()]:
            if s not in seen:
                seen.add(s)
                work.append(s)
    return seen


def loop_body(pred, h, i, from_h):
    """The header plus every region from_h holds that reaches i without passing h."""
    body, work = {h, i}, ([i] if i != h else [])
    while work:
        for p in pred[work.pop()]:
            if p in from_h and p not in body:
                body.add(p)
                work.append(p)
    return body


def use_def(r):
    use, d = set(), set()
    for stmt in re.split(r'[;{}\n]', r['text']):
        m = DEF.match(stmt)
        for x in TOK.findall(m.group(2) if m else stmt):
            if x in REGS and x not in d:
                use.add(x)
        if m and m.group(1) in REGS:
            d.add(m.group(1))
    if r['calls']:
        d |= CALLDEF
    return use, d


def carried(regs, succ, body, h):
    """Registers live into the header over the body and defined in it."""
    ud = {i: use_def(regs[i]) for i in body}
    live = {i: set() for i in body}
    changed = True
    while changed:
        changed = False
        for i in body:
            out = set()
            for s in succ[i]:
                if s in body and s != h:
                    out |= live[s]
            new = ud[i][0] | (out - ud[i][1])
            if new != live[i]:
                live[i] = new
                changed = True
    defs = set().union(*[ud[i][1] for i in body])
    return sorted(live[h] & defs)


class Unit:
    """One generated file: its functions' regions and its pure leaves."""

    def __init__(self, path, psyq):
        self.path = path
        self.name = os.path.basename(path)
        self.funcs = {}
        for m in FUNC.finditer(open(path).read()):
            name = m.group(1) + '_' + m.group(2)
            regs = regions(m.group(3), psyq)
            self.funcs[name] = (int(m.group(2), 16), regs, 'STUB:' in m.group(3))
        self.byaddr = {}
        for name, (addr, _, _) in self.funcs.items():
            self.byaddr.setdefault(addr, name)
        self.leaves = {}
        for name, (addr, regs, stub) in self.funcs.items():
            if stub or any(r['store'] or r['opaque'] or r['calls'] or r['psyq'] for r in regs):
                continue
            if any(g <= r['label'] for r in regs for g, _ in r['gotos']):
                continue
            self.leaves[addr] = (sum(r['insns'] for r in regs), any(r['load'] for r in regs))


def emission(regs, body, leaves, foreign):
    """(insns, leaves) when the body meets the emission criterion, else None."""
    loads, insns, lv = False, 0, []
    for i in sorted(body):
        r = regs[i]
        if r['store'] or r['opaque'] or r['psyq']:
            return None
        loads |= r['load']
        insns += r['insns']
        for c in r['calls']:
            leaf = leaves.get(c) or foreign.get(c)
            if not leaf:
                return None
            loads |= leaf[1]
            insns += leaf[0]
            lv.append(c)
    return (insns, lv) if loads else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('game')
    ap.add_argument('files', nargs='+')
    ap.add_argument('--list', choices=['pure', 'counted', 'novsync', 'callpoll', 'emit', 'callspin'])
    ap.add_argument('--range', nargs=2)
    a = ap.parse_args()
    lo, hi = (int(a.range[0], 16), int(a.range[1], 16)) if a.range else (0, 1 << 32)
    psyq = load_psyq(a.game)
    units = [Unit(p, psyq) for p in a.files]
    main_unit = next((u for u in units if u.name == 'PSX.EXE_full.c'), None)

    def resolve(u, addr):
        if addr in u.byaddr:
            return u, u.byaddr[addr]
        if main_unit is not None and addr in main_unit.byaddr:
            return main_unit, main_unit.byaddr[addr]
        return None

    # VSync reach per function: (VSync reachable, unknown path), solved as a
    # fixpoint over the direct-call graph of every unit.
    edges, reach = {}, {}
    for u in units:
        for f, (_, regs, _) in u.funcs.items():
            vs, unk, out = False, False, []
            for r in regs:
                vs = vs or 'psyq_VSync' in r['psyq']
                unk = unk or r['opaque']
                for c in r['calls']:
                    hit = resolve(u, c)
                    if hit is None:
                        unk = True
                    else:
                        out.append((hit[0].path, hit[1]))
            reach[(u.path, f)] = (vs, unk)
            edges[(u.path, f)] = out
    changed = True
    while changed:
        changed = False
        for key, out in edges.items():
            vs, unk = reach[key]
            for c in out:
                v, k = reach[c]
                vs, unk = vs or v, unk or k
            if (vs, unk) != reach[key]:
                reach[key] = (vs, unk)
                changed = True

    # Quiet closure per function: every call resolves, no register call
    # other than a kernel call or a jump table's default, no psyq_VSync.
    quiet = {}
    status = {'psyq_CdSync', 'psyq_CdReady', 'psyq_SpuIsTransferCompleted', 'psyq_MemCardSync'}
    for u in units:
        for f, (_, regs, stub) in u.funcs.items():
            q = not stub and not any(r['unknown'] or 'psyq_VSync' in r['psyq'] or r['psyq'] & status
                                     for r in regs)
            q = q and all(resolve(u, c) is not None for r in regs for c in r['calls'])
            quiet[(u.path, f)] = q
    changed = True
    while changed:
        changed = False
        for key, out in edges.items():
            if quiet[key] and not all(quiet[c] for c in out):
                quiet[key] = False
                changed = True
    # Kernel reach per function: the closure makes a kernel call.
    kreach = {(u.path, f): any(r['kernel'] for r in regs)
              for u in units for f, (_, regs, _) in u.funcs.items()}
    changed = True
    while changed:
        changed = False
        for key, out in edges.items():
            if not kreach[key] and any(kreach[c] for c in out):
                kreach[key] = True
                changed = True
    overlays = [u for u in units if u is not main_unit]

    def device_polls(u):
        """Headers of the unit's kind-2 sites in the recompiler's site list beside it."""
        p = u.path.replace('_full.c', '_loop_sites.tsv')
        if not os.path.exists(p):
            return set()
        return {int(l.split('\t')[1], 16) for l in open(p)
                if not l.startswith('#') and l.rstrip('\n').split('\t')[-1] == '2'}

    def fname_of(u, regs):
        return next(f for f, (_, rr, _) in u.funcs.items() if rr is regs)

    def holds(hit, key):
        seen, work = {(hit[0].path, hit[1])}, [(hit[0].path, hit[1])]
        while work:
            k = work.pop()
            if k == key:
                return True
            for c in edges[k]:
                if c not in seen:
                    seen.add(c)
                    work.append(c)
        return False

    def callspin(u, regs, succ, body, h):
        """(kind, description) when the body is a call-carrying spin, else None."""
        deferred, names, kernel = None, [], False
        for i in body:
            r = regs[i]
            if r['store'] or r['opaque'] or r['psyq']:
                return None
            for c in r['calls']:
                hit = resolve(u, c)
                if hit is None:
                    if u is not main_unit or (deferred is not None and deferred != c):
                        return None
                    deferred = c
                    continue
                if not quiet[(hit[0].path, hit[1])] or holds(hit, (u.path, fname_of(u, regs))):
                    return None
                names.append(hit[1])
                kernel = kernel or kreach[(hit[0].path, hit[1])]
        if deferred is None and not names:
            return None
        if deferred is None and not kernel:
            return None
        if carried(regs, succ, body, h):
            return None
        if deferred is None:
            return 3, ','.join(names)
        verdicts = []
        for o in overlays:
            if deferred in o.byaddr:
                k = (o.path, o.byaddr[deferred])
                verdicts.append('%s:%s' % (o.name.split('.')[0],
                                           'quiet' if quiet[k] and kreach[k] else 'refused'))
        return 4, '0x%08X [%s] %s' % (deferred, ' '.join(verdicts) or 'no overlay defines it',
                                      ','.join(names))

    cnt = collections.Counter()
    listed = []
    thunks = collections.Counter()
    for u in units:
        u.device_polls = device_polls(u)
        foreign = main_unit.leaves if main_unit is not None and u is not main_unit else {}
        for fname, (faddr, regs, stub) in u.funcs.items():
            if stub or not regs or not (lo <= faddr < hi):
                continue
            idx, succ, pred = graph(regs)
            live = reach_from(succ, 0)
            for i, r in enumerate(regs):
                if i not in live:
                    continue
                for t, arm in r['gotos']:
                    if t > r['label'] or t not in idx or idx[t] not in live:
                        continue
                    h = idx[t]
                    from_h = reach_from(succ, h)
                    if i not in from_h:
                        continue
                    body = loop_body(pred, h, i, from_h)
                    br = [regs[j] for j in sorted(body)]
                    n = sum(x['insns'] for x in br)
                    cnt['back-edges'] += 1
                    em = emission(regs, body, u.leaves, foreign)
                    if em:
                        pure = not carried(regs, succ, body, h)
                        cnt['emission sites (pure-leaf calls)'] += 1
                        cnt['  pure' if pure else '  counted'] += 1
                        row = (u.name, fname, t, em[0])
                        if a.list == 'emit':
                            listed.append(row + ('src=0x%08X %s pure=%d %s' % (
                                r['label'], arm, pure,
                                ','.join('0x%08X' % c for c in em[1]) or '-'),))
                        elif a.list == ('pure' if pure else 'counted'):
                            listed.append(row + ('src=0x%08X %s' % (r['label'], arm),))
                    if not em and t not in u.device_polls:
                        cs = callspin(u, regs, succ, body, h)
                        if cs:
                            cnt['call-carrying spins'] += 1
                            cnt['  kind %d' % cs[0]] += 1
                            if a.list == 'callspin':
                                listed.append((u.name, fname, t, n, 'src=0x%08X %s kind=%d %s' % (
                                    r['label'], arm, cs[0], cs[1])))
                    calls = [c for x in br for c in x['calls']]
                    ps = set().union(*[x['psyq'] for x in br])
                    ind = any(x['opaque'] for x in br)
                    store = any(x['store'] for x in br)
                    if calls or ps or ind:
                        cnt['with a call'] += 1
                        names = set(ps)
                        if 'psyq_VSync' in ps:
                            cnt['  VSync in body'] += 1
                            continue
                        vs, unk = False, ind
                        for c in calls:
                            hit = resolve(u, c)
                            if hit is None:
                                unk = True
                                names.add('0x%08X' % c)
                                continue
                            names.add(hit[1])
                            v, k = reach[(hit[0].path, hit[1])]
                            vs, unk = vs or v, unk or k
                        if vs:
                            cnt['  VSync via callees'] += 1
                        elif unk:
                            cnt['  unknown (indirect call)'] += 1
                        else:
                            cnt['  no path to VSync'] += 1
                            for p in ps:
                                thunks[p] += 1
                            if a.list == 'novsync':
                                listed.append((u.name, fname, t, n, ','.join(sorted(names))))
                        if a.list == 'callpoll' and not store:
                            m = COND.search(r['raw'])
                            if m and 'cpu->v0' in m.group(1) and n <= 20:
                                listed.append((u.name, fname, t, n, ','.join(sorted(names))))
                        continue
                    cnt['call-free'] += 1
                    if store:
                        continue
                    cnt['call-free, store-free'] += 1
                    if any(x['load'] for x in br):
                        cnt['  with a load'] += 1

    print(a.game)
    for k in ('back-edges', 'with a call', '  VSync in body', '  VSync via callees',
              '  unknown (indirect call)', '  no path to VSync', 'call-free',
              'call-free, store-free', '  with a load',
              'emission sites (pure-leaf calls)', '  pure', '  counted',
              'call-carrying spins', '  kind 3', '  kind 4'):
        print('  %-34s %d' % (k, cnt[k]))
    print('  thunks called from loops with no path to VSync:',
          ', '.join('%s x%d' % kv for kv in thunks.most_common()))
    for src, fname, t, n, extra in listed:
        print('  %-30s %-42s hdr=0x%08X insns=%-3d %s' % (src, fname, t, n, extra))


if __name__ == '__main__':
    main()
