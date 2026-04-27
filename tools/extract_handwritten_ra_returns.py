#!/usr/bin/env python3
"""Forced-entry harvester for handwritten `jr $reg` returns.

A few of Spyro's handwritten asm functions save `$ra` into a non-`$ra`
register and later return via `jr <that reg>` instead of the usual
`jr $ra`. Examples:

  * particle_alloc.s `func_80053570`     — saves to `$a3`
  * 42CC4.s          `func_800530C0`     — saves to `$t7`

When the recompiler translates such a function, the `jr $reg` falls into
the global dispatch table (the static analyzer can't prove which PC the
register holds). At runtime the register holds the caller's return PC
(`callsite + 8`) — but psxrecomp only registers function *entries* as
dispatch cases, so the return PC stub-misses unless we forced-entry it.

This is a generalization of the per-overlay `--forced-entry=...` flags
in scripts/regen-recomp.sh: rather than hand-listing every Stone Hill
or Artisans Homeworld particle-alloc return site, scan the handwritten
asm for the pattern, find every `jal <fn>` callsite across all .s
files, and emit `<callsite>+8` into the per-binary forced-entry file.

Usage:
    extract_handwritten_ra_returns.py <asm-root> <out-dir>

`<out-dir>` is the same directory `extract_asm_dispatch_targets.py`
writes to (manifest/asm_dispatch_targets/) — this script *appends* to
the existing per-binary files, so run that script first.
"""
import argparse
import re
from collections import defaultdict
from pathlib import Path

GLABEL_RE = re.compile(r'^glabel\s+(\S+)\s*$')
ADDR_INSTR_RE = re.compile(
    r'^/\*\s+\w+\s+([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{8})\s+\*/\s*(.*)'
)
SAVE_RA_RE = re.compile(r'^addi\s+\$(\w+),\s*\$ra,\s*0x0\b')
JR_REG_RE = re.compile(r'^jr\s+\$(\w+)\b')
JAL_RE = re.compile(r'^jal\s+(\S+)')


def binary_for(asm_root: Path, path: Path) -> str:
    parts = path.relative_to(asm_root).parts
    if 'overlays' in parts:
        i = parts.index('overlays')
        if i + 1 < len(parts):
            return parts[i + 1]
    return 'main_exe'


def find_ra_save_functions(asm_root: Path) -> dict[str, int]:
    """Return {func_name: entry_addr} for handwritten functions that save
    `$ra` into a non-`$ra` register and later `jr <that reg>` in the same
    function body. Scans every .s file (the pattern shows up only in
    handwritten ones, but the check is cheap)."""
    out: dict[str, int] = {}
    for path in asm_root.rglob('*.s'):
        lines = path.read_text().splitlines()
        # Pre-pass: split into function blocks keyed by glabel.
        blocks: list[tuple[str, int, int]] = []  # (name, start, end)
        cur_name = None
        cur_start = 0
        for i, line in enumerate(lines):
            m = GLABEL_RE.match(line.strip())
            if m:
                if cur_name is not None:
                    blocks.append((cur_name, cur_start, i))
                cur_name = m.group(1)
                cur_start = i
        if cur_name is not None:
            blocks.append((cur_name, cur_start, len(lines)))

        for name, start, end in blocks:
            entry_addr = None
            saved_regs: set[str] = set()
            jr_regs: set[str] = set()
            for j in range(start, end):
                am = ADDR_INSTR_RE.match(lines[j])
                if not am:
                    continue
                addr = int(am.group(1), 16)
                rest = am.group(3).lstrip()
                if entry_addr is None:
                    entry_addr = addr
                m = SAVE_RA_RE.match(rest)
                if m and m.group(1) != 'ra':
                    saved_regs.add(m.group(1))
                m = JR_REG_RE.match(rest)
                if m and m.group(1) != 'ra':
                    jr_regs.add(m.group(1))
            if entry_addr is not None and (saved_regs & jr_regs):
                out[name] = entry_addr
    return out


def find_callsites(
    asm_root: Path, fn_names: set[str]
) -> dict[str, set[int]]:
    """For every `jal <fn>` line whose target is in fn_names, record
    `callsite + 8` (the PC the JAL returns to) keyed by the .s file's
    owning binary."""
    out: dict[str, set[int]] = defaultdict(set)
    for path in asm_root.rglob('*.s'):
        binary = binary_for(asm_root, path)
        for line in path.read_text().splitlines():
            am = ADDR_INSTR_RE.match(line)
            if not am:
                continue
            jal_addr = int(am.group(1), 16)
            rest = am.group(3).lstrip()
            jm = JAL_RE.match(rest)
            if not jm:
                continue
            target = jm.group(1).rstrip(',')
            if target in fn_names:
                out[binary].add(jal_addr + 8)
    return out


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        'asm_root',
        type=Path,
        nargs='?',
        default=Path('games/spyro-1/spyro-1/asm'),
    )
    p.add_argument('out_dir', type=Path)
    args = p.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    ra_funcs = find_ra_save_functions(args.asm_root)
    if not ra_funcs:
        print('no handwritten jr-reg-return functions found')
        return 0
    print(f'detected {len(ra_funcs)} handwritten jr-reg-return functions:')
    for name, addr in sorted(ra_funcs.items(), key=lambda kv: kv[1]):
        print(f'  {name} @ 0x{addr:08X}')

    by_binary = find_callsites(args.asm_root, set(ra_funcs))
    total = 0
    for binary, addrs in sorted(by_binary.items()):
        path = args.out_dir / f'{binary}.txt'
        existing = set()
        header = (
            f'# {binary}.txt — auto-generated by\n'
            '# tools/extract_asm_dispatch_targets.py and\n'
            '# tools/extract_handwritten_ra_returns.py. Do not hand-edit.\n'
        )
        if path.exists():
            for ln in path.read_text().splitlines():
                ln = ln.strip()
                if ln.startswith('0x'):
                    existing.add(int(ln, 16))
        merged = sorted(existing | addrs)
        with path.open('w') as f:
            f.write(header)
            for a in merged:
                f.write(f'0x{a:08X}\n')
        added = len(merged) - len(existing)
        print(f'{binary}: +{added} ra-return entries '
              f'({len(addrs)} found, total {len(merged)})')
        total += added
    print(f'total new entries: {total}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
