#!/usr/bin/env python3
"""Per-overlay forced entries for the callbacks the game reaches by address.

The main EXE `jalr`s through fixed RAM slots (Spyro 1: 0x800758CC,
0x80075734, ...; Spyro 2: 0x800670F0, ...) that the game fills with
`lui+addiu` constants pointing into whichever overlay is resident. The
static recompiler cannot follow that data flow, so the target is a runtime
stub miss unless it is registered as a forced entry of that overlay. This
tool derives, from the bytes alone, which overlay each such constant
belongs to:

  slot filter  A constant stored into a fixed RAM word counts only if some
               `lw` of that word feeds a `jr`/`jalr` somewhere in the EXE or
               an overlay. Credits buffers and per-level data pointers are
               stored the same way and are not entries.
  R2           A store inside a case of the jump-table switch keyed on the
               game's level-id word (`LEVEL_ID_ADDR`) belongs to the overlay
               whose registered section name is `level_<id>` for that case.
               A case ends at its first unconditional `j`/`jr` plus delay
               slot; a `j` is followed once, carrying the pending constant,
               to the shared tail that stores the last one.
  R1           A store by an overlay's own bytes, outside such cases, belongs
               to that overlay (Spyro 3 levels install their callback
               themselves).
  R4           A jump-read slot inside the overlay window is a word of the
               resident overlay's own image (Spyro 3 keeps a table of
               per-level entry points in the overlay header, which the EXE
               `jalr`s through); the word at that slot in each overlay's
               blob belongs to that overlay when it lies in the window.
  R3           Any other cross-binary store into a jump-read slot is an
               error naming the site, unless a curated forced-entry row
               already names the target.
  validator    An attributed target must be word-aligned, inside the
               overlay's blob, not a zero word and not printable text;
               anything else is reported, not forced.

Constants are composed by a linear walk with register invalidation and no
branch following (`addiu rd,rs,imm` on a composed `rs` composes `rd`). A
binary that holds a jump-table switch keyed on a fixed word other than
`LEVEL_ID_ADDR` whose cases store window constants into jump-read slots is
an error: the installer idiom must be declared, never attributed to the
installer's own bytes.

Usage:
    extract_overlay_func_pointers.py <PSX.EXE> --overlay-range=LO:HI \\
        --registry=generated/overlay_registry.txt --out-dir=generated \\
        [--level-id-addr=0x...] [--curated-dir=manifest/forced_entries]

Writes generated/<overlay>/fn_pointers.txt for every registered overlay and
generated/overlay_fn_report.txt.
"""
import argparse
import struct
import sys
from collections import defaultdict
from pathlib import Path

OP_SPECIAL = 0x00
OP_REGIMM = 0x01
OP_J = 0x02
OP_JAL = 0x03
OP_ADDI = 0x08
OP_ADDIU = 0x09
OP_SLTIU = 0x0B
OP_LUI = 0x0F
OP_LW = 0x23
OP_SW = 0x2B
FN_JR = 0x08
FN_JALR = 0x09
FN_SLL = 0x00
FN_ADDU = 0x21
# Opcodes that write no GPR: branches, j, stores, swc2.
OPS_NO_WRITE = {0x04, 0x05, 0x06, 0x07, OP_REGIMM, OP_J,
                0x28, 0x29, 0x2A, 0x2B, 0x2E, 0x3A}
SP = 29
RA = 31


def s16(x):
    return x - 0x10000 if x & 0x8000 else x


class Binary:
    def __init__(self, name, base, data, is_exe, blob_end=None):
        self.name = name
        self.base = base
        self.data = data
        self.is_exe = is_exe
        self.end = base + len(data)
        self.blob_end = blob_end if blob_end is not None else self.end

    def word(self, addr):
        off = addr - self.base
        if off < 0 or off + 4 > len(self.data):
            return None
        return struct.unpack_from('<I', self.data, off)[0]

    def contains(self, addr):
        return self.base <= addr < self.blob_end


def load_exe(path):
    exe = path.read_bytes()
    if exe[:8] != b'PS-X EXE':
        raise SystemExit(f'{path}: not a PS-X EXE')
    text_addr = struct.unpack_from('<I', exe, 0x18)[0]
    text_size = struct.unpack_from('<I', exe, 0x1C)[0]
    return Binary('main_exe', text_addr, exe[0x800:0x800 + text_size], True)


def decode(w):
    return (w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31,
            (w >> 6) & 31, w & 0x3F, w & 0xFFFF)


def is_uncond_jump(w):
    op, rs, rt, rd, sh, fn, imm = decode(w)
    return op == OP_J or (op == OP_SPECIAL and fn in (FN_JR, FN_JALR))


def is_call_delay_slot(b, addr):
    w = b.word(addr - 4)
    if w is None:
        return False
    op, rs, rt, rd, sh, fn, imm = decode(w)
    return op == OP_JAL or (op == OP_SPECIAL and fn == FN_JALR)


class Walker:
    """Linear register-state walk over a range of one binary.

    Emits stores of composed constants and the set of fixed words whose
    loaded value reaches a jr/jalr."""

    def __init__(self, b, lo, hi):
        self.b = b
        self.lo, self.hi = lo, hi
        self.stores = []       # (site, dest, value); dest = int | ('reg', base, off)
        self.jump_read = set()
        self.jr_sites = []     # (site, rs, loaded_from or None, index_key)
        self.reset()

    def reset(self):
        self.high = {}
        self.full = {}
        self.loaded = {}       # reg -> fixed word it was loaded from
        self.lw_src = {}       # reg -> (base_composed_or_None, off)

    def kill(self, reg):
        self.high.pop(reg, None)
        self.full.pop(reg, None)
        self.loaded.pop(reg, None)
        self.lw_src.pop(reg, None)

    def step(self, addr, w):
        op, rs, rt, rd, sh, fn, imm = decode(w)
        if op == OP_LUI:
            self.kill(rt)
            self.high[rt] = imm << 16
        elif op in (OP_ADDIU, OP_ADDI):
            if rs in self.high:
                a = (self.high[rs] + s16(imm)) & 0xFFFFFFFF
                self.kill(rt)
                self.high[rt] = a
                if self.lo <= a < self.hi:
                    self.full[rt] = a
            elif rs in self.full:
                a = (self.full[rs] + s16(imm)) & 0xFFFFFFFF
                self.kill(rt)
                if self.lo <= a < self.hi:
                    self.full[rt] = a
            else:
                self.kill(rt)
        elif op == OP_SW:
            if rt in self.full:
                if rs in self.high:
                    dest = (self.high[rs] + s16(imm)) & 0xFFFFFFFF
                else:
                    dest = ('reg', rs, s16(imm))
                self.stores.append((addr, dest, self.full[rt]))
        elif op == OP_LW:
            base = (self.high[rs] + s16(imm)) & 0xFFFFFFFF if rs in self.high else None
            self.kill(rt)
            if base is not None:
                self.loaded[rt] = base
            self.lw_src[rt] = (base, s16(imm))
        elif op == OP_SPECIAL:
            if fn in (FN_JR, FN_JALR):
                if rs in self.loaded:
                    self.jump_read.add(self.loaded[rs])
                self.jr_sites.append((addr, rs, self.loaded.get(rs)))
                if fn == FN_JALR:
                    self.kill(rd if rd else RA)
            else:
                if rd:
                    self.kill(rd)
        elif op == OP_JAL:
            self.kill(RA)
        elif op not in OPS_NO_WRITE:
            self.kill(rt)

    def run(self, start=None, end=None):
        start = self.b.base if start is None else start
        end = self.b.end if end is None else end
        for addr in range(start, end - 3, 4):
            w = self.b.word(addr)
            if w is None:
                break
            self.step(addr, w)


def walk_case(b, start, lo, hi, follow=True):
    """Straight-line walk of one switch case: to the first unconditional
    `j`/`jr` plus delay slot; a `j` is followed once to the tail's `jr`.
    Returns (stores, covered_sites, fell_through_to)."""
    wk = Walker(b, lo, hi)
    covered = []
    addr = start
    followed = False
    for _ in range(4096):
        w = b.word(addr)
        if w is None:
            break
        wk.step(addr, w)
        covered.append(addr)
        if is_uncond_jump(w):
            op = w >> 26
            d = b.word(addr + 4)
            if d is not None:
                wk.step(addr + 4, d)
                covered.append(addr + 4)
            if op == OP_J and follow and not followed:
                target = (addr & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                addr = target
                followed = True
                continue
            break
        addr += 4
    return wk.stores, covered


def find_switches(b, lo, hi):
    """Jump-table switches whose key register is loaded from a fixed word.

    Pattern (psxrecomp's P1/P2): lw key,W ; [addiu key,key,-K] ; sltiu
    bound,key,N ; sll t,key,2 ; lui T[,addiu T] ; addu T,T,t ; lw t,off(T) ;
    jr t. The table base is lui + addiu + the lw offset. Returns
    [(jr_site, key_word, K, N, table_base)]."""
    out = []
    n = len(b.data)
    for off in range(0, n - 3, 4):
        w = struct.unpack_from('<I', b.data, off)[0]
        op, rs, rt, rd, sh, fn, imm = decode(w)
        if not (op == OP_SPECIAL and fn == FN_JR and rs != RA):
            continue
        site = b.base + off
        jr_rs = rs
        lw_base = None
        lw_off = 0
        addu_srcs = None
        table_reg = None
        lui_val = None
        addiu_val = 0
        key_reg = None
        K = 0
        N = None
        key_word = None
        # Spyro 2's loading overlay zeroes eight slots between the key load
        # and the bound check, so the window is generous.
        for back in range(1, 128):
            a = site - back * 4
            x = b.word(a)
            if x is None:
                break
            xop, xrs, xrt, xrd, xsh, xfn, ximm = decode(x)
            if lw_base is None:
                if xop == OP_LW and xrt == jr_rs:
                    lw_base, lw_off = xrs, s16(ximm)
                continue
            if addu_srcs is None:
                if xop == OP_SPECIAL and xfn == FN_ADDU and xrd == lw_base:
                    addu_srcs = (xrs, xrt)
                continue
            if key_reg is None and xop == OP_SPECIAL and xfn == FN_SLL and xsh == 2 and xrd in addu_srcs:
                key_reg = xrt
                table_reg = addu_srcs[0] if addu_srcs[1] == xrd else addu_srcs[1]
                continue
            if lui_val is None and xop == OP_ADDIU and xrt == xrs and xrt in addu_srcs and xrt != key_reg:
                addiu_val = s16(ximm)
                continue
            if lui_val is None and xop == OP_LUI and xrt in addu_srcs and xrt != key_reg:
                lui_val = ximm << 16
                table_reg = xrt
                continue
            if key_reg is None:
                continue
            if N is None and xop == OP_SLTIU and xrs == key_reg:
                N = ximm
                continue
            if xop == OP_ADDIU and xrt == key_reg and xrs == key_reg:
                K -= s16(ximm)
                continue
            if xop == OP_LW and xrt == key_reg:
                base = None
                for b2 in range(1, 8):
                    y = b.word(a - b2 * 4)
                    if y is None:
                        break
                    yop, yrs, yrt, yrd, ysh, yfn, yimm = decode(y)
                    if yop == OP_LUI and yrt == xrs:
                        base = yimm << 16
                        break
                if base is not None:
                    key_word = (base + s16(ximm)) & 0xFFFFFFFF
                break
        if key_word is None or lui_val is None or N is None:
            continue
        table = (lui_val + addiu_val + lw_off) & 0xFFFFFFFF
        out.append((site, key_word, K, N, table))
    return out


def printable_run(b, addr, words=2):
    for i in range(words):
        w = b.word(addr + i * 4)
        if w is None:
            return False
        for byte in struct.pack('<I', w):
            if byte != 0 and not (0x20 <= byte < 0x7F):
                return False
    return True


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('exe', type=Path)
    p.add_argument('--overlay-range', required=True)
    p.add_argument('--registry', required=True, type=Path,
                   help='overlay_registry.txt: "<id> <name> <blob path>" per line')
    p.add_argument('--out-dir', required=True, type=Path)
    p.add_argument('--level-id-addr', default=None)
    p.add_argument('--curated-dir', type=Path, default=None)
    args = p.parse_args()
    lo_s, hi_s = args.overlay_range.split(':')
    lo, hi = int(lo_s, 0), int(hi_s, 0)
    level_id_addr = int(args.level_id_addr, 0) if args.level_id_addr else None

    overlays = {}
    for line in args.registry.read_text().splitlines():
        f = line.split()
        if len(f) != 3:
            continue
        name, blob = f[1], Path(f[2])
        overlays[name] = Binary(name, lo, blob.read_bytes(), False)
    binaries = [load_exe(args.exe)] + list(overlays.values())
    by_level = {n: n for n in overlays if n.startswith('level_')}

    curated = defaultdict(set)
    if args.curated_dir and args.curated_dir.is_dir():
        for f in args.curated_dir.glob('*.txt'):
            for line in f.read_text().splitlines():
                s = line.strip()
                if s.startswith('0x'):
                    curated[f.stem].add(int(s, 16))
    curated_targets = set().union(*curated.values()) if curated else set()

    # Pass 1: every binary, linear walk: stores and jump-read words. A
    # slot-fed site is a jr/jalr whose register was loaded straight from a
    # fixed word; a site fed through a table index or a struct field is not
    # one, and its targets are the recompiler's own discovery.
    walks = {}
    jump_read = set()
    slot_sites = set()
    for b in binaries:
        wk = Walker(b, lo, hi)
        wk.run()
        walks[b.name] = wk
        jump_read |= wk.jump_read
        slot_sites |= {site for site, rs, src in wk.jr_sites if src is not None}

    report = []
    attributed = defaultdict(set)       # overlay -> {target}
    attribution_source = {}             # (overlay, target) -> rule
    errors = []
    covered_sites = defaultdict(set)    # binary -> case sites
    dead_cases = []
    switch_holders = set()

    # R2: level-keyed switches.
    for b in binaries:
        for (site, key_word, K, N, table) in find_switches(b, lo, hi):
            cases = []
            for i in range(N):
                t = b.word(table + i * 4)
                if t is None:
                    break
                cases.append(t)
            case_stores = []
            for i, c in enumerate(cases):
                if not b.contains(c) and not (b.base <= c < b.end):
                    continue
                stores, covered = walk_case(b, c, lo, hi)
                case_stores.append((i + K, c, stores, covered))
            if key_word != level_id_addr:
                # A switch keyed on another fixed word: only an error if its
                # cases install window constants into jump-read slots.
                bad = [(lvl, st) for lvl, c, stores, cov in case_stores
                       for st in stores if isinstance(st[1], int) and st[1] in jump_read]
                if bad:
                    errors.append(f'{b.name}: jump-table switch at 0x{site:08X} keyed on '
                                  f'0x{key_word:08X} (not LEVEL_ID_ADDR) installs '
                                  f'{len(bad)} window constants into jump-read slots')
                continue
            switch_holders.add(b.name)
            report.append(f'switch {b.name} jr 0x{site:08X} key 0x{key_word:08X} '
                          f'K={K} N={N} table 0x{table:08X}')
            for lvl, c, stores, covered in case_stores:
                covered_sites[b.name].update(covered)
                kept = [st for st in stores
                        if (isinstance(st[1], int) and st[1] in jump_read)
                        or (isinstance(st[1], tuple) and st[1][1] != SP)]
                name = by_level.get(f'level_{lvl}')
                if name is None:
                    if kept:
                        dead_cases.append((lvl, len(kept)))
                    continue
                for site_, dest, val in kept:
                    attributed[name].add(val)
                    attribution_source[(name, val)] = 'R2'

    if level_id_addr is not None and not switch_holders:
        errors.append(f'LEVEL_ID_ADDR=0x{level_id_addr:08X} set but no jump-table switch '
                      f'keyed on it was found')

    # R1 / R3: stores outside switch cases.
    slot_dropped = []
    sp_relative = []
    for b in binaries:
        wk = walks[b.name]
        for site, dest, val in wk.stores:
            if site in covered_sites[b.name]:
                continue
            if isinstance(dest, tuple):
                if dest[1] == SP:
                    sp_relative.append((b.name, site, val))
                    continue
                fixed = False
            else:
                fixed = True
                if dest not in jump_read:
                    slot_dropped.append((b.name, site, dest, val))
                    continue
            if b.is_exe or (fixed and b.name in switch_holders):
                if val in curated_targets:
                    continue
                errors.append(f'R3: {b.name} stores window constant 0x{val:08X} into '
                              f'{"0x%08X" % dest if fixed else "a struct field"} at '
                              f'0x{site:08X} outside any level-keyed case')
                continue
            attributed[b.name].add(val)
            attribution_source.setdefault((b.name, val), 'R1')

    # R4: slots the loader fills, as words of each overlay's own image.
    for slot in sorted(jump_read):
        if not (lo <= slot < hi):
            continue
        for name, b in overlays.items():
            val = b.word(slot)
            if val is None or not (lo <= val < hi):
                continue
            attributed[name].add(val)
            attribution_source.setdefault((name, val), 'R4')

    # Validator.
    validator_dropped = []
    final = defaultdict(set)
    for name, targets in attributed.items():
        b = overlays[name]
        for t in sorted(targets):
            why = None
            if t & 3:
                why = 'unaligned'
            elif not b.contains(t):
                why = 'outside blob'
            elif b.word(t) == 0:
                why = 'zero word'
            elif printable_run(b, t):
                why = 'text'
            elif is_call_delay_slot(b, t):
                errors.append(f'{name}: attributed entry 0x{t:08X} is a call delay slot')
                continue
            if why:
                validator_dropped.append((name, t, why, attribution_source[(name, t)]))
            else:
                final[name].add(t)

    # Calibration (not part of the rule): does the target look like code
    # entered from outside in that overlay?
    def entry_like(b, t):
        w = b.word(t)
        if w is None:
            return False
        op, rs, rt, rd, sh, fn, imm = decode(w)
        if op == OP_ADDIU and rt == SP and rs == SP and s16(imm) < 0:
            return True
        prev = b.word(t - 4)
        if prev is not None:
            pop, prs, prt, prd, psh, pfn, pimm = decode(prev)
            if pop == OP_SPECIAL and pfn == FN_JR and prs == RA:
                return True
            prev2 = b.word(t - 8)
            if prev2 is not None and prev == 0 and is_uncond_jump(prev2):
                return True
        # A JAL target inside the same overlay.
        enc = OP_JAL << 26 | ((t & 0x0FFFFFFF) >> 2)
        for off in range(0, len(b.data) - 3, 4):
            if struct.unpack_from('<I', b.data, off)[0] == enc:
                return True
        return False

    args.out_dir.mkdir(parents=True, exist_ok=True)
    total = 0
    cal_hit = cal_all = 0
    for name in overlays:
        d = args.out_dir / name
        d.mkdir(parents=True, exist_ok=True)
        targets = sorted(final.get(name, set()))
        total += len(targets)
        with (d / 'fn_pointers.txt').open('w') as f:
            f.write(f'# fn_pointers.txt — auto-generated by\n'
                    f'# tools/extract_overlay_func_pointers.py: callback entries the game\n'
                    f'# installs by address while {name} is resident.\n')
            for t in targets:
                f.write(f'0x{t:08X}\n')
        hits = sum(1 for t in targets if entry_like(overlays[name], t))
        cal_hit += hits
        cal_all += len(targets)
        rules = defaultdict(int)
        for t in targets:
            rules[attribution_source[(name, t)]] += 1
        report.append(f'{name}: {len(targets)} entries ' +
                      ' '.join(f'{k}={v}' for k, v in sorted(rules.items())) +
                      f' calibration {hits}/{len(targets)}')

    with (args.out_dir / 'overlay_fn_report.txt').open('w') as f:
        f.write(f'# overlay_fn_report.txt — auto-generated by tools/extract_overlay_func_pointers.py\n')
        f.write(f'window 0x{lo:08X}..0x{hi:08X}; level-id word '
                f'{"0x%08X" % level_id_addr if level_id_addr is not None else "unset"}\n')
        f.write(f'jump-read slots ({len(jump_read)}): ' +
                ' '.join(f'0x{w:08X}' for w in sorted(jump_read)) + '\n')
        f.write(f'slot-fed sites ({len(slot_sites)}): ' +
                ' '.join(f'0x{w:08X}' for w in sorted(slot_sites)) + '\n')
        for line in report:
            f.write(line + '\n')
        f.write(f'total entries {total}; calibration {cal_hit}/{cal_all}\n')
        f.write(f'dead cases ({len(dead_cases)}): ' +
                ' '.join(f'{lvl}:{n}' for lvl, n in dead_cases) + '\n')
        f.write(f'slot-filter drops ({len(slot_dropped)}):\n')
        for bn, site, dest, val in slot_dropped:
            f.write(f'  {bn} 0x{site:08X}: 0x{val:08X} -> 0x{dest:08X}\n')
        f.write(f'sp-relative constants ({len(sp_relative)}):\n')
        for bn, site, val in sp_relative:
            f.write(f'  {bn} 0x{site:08X}: 0x{val:08X}\n')
        f.write(f'validator drops ({len(validator_dropped)}):\n')
        for name, t, why, rule in validator_dropped:
            f.write(f'  {name} 0x{t:08X} ({rule}): {why}\n')
        f.write(f'errors ({len(errors)}):\n')
        for e in errors:
            f.write(f'  {e}\n')

    print(f'overlay fn-pointers: {total} entries across {len(overlays)} overlays; '
          f'calibration {cal_hit}/{cal_all}; {len(slot_dropped)} slot drops; '
          f'{len(validator_dropped)} validator drops; {len(dead_cases)} dead cases; '
          f'{len(errors)} errors -> {args.out_dir / "overlay_fn_report.txt"}')
    for e in errors:
        print(f'  ERROR: {e}', file=sys.stderr)
    return 1 if errors else 0


if __name__ == '__main__':
    raise SystemExit(main())
