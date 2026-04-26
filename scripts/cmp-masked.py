#!/usr/bin/env python3
"""Masked RAM diff between openpete and PCSX-Redux dumps.

Reads each side's 'CSv1' CPU dump (528 bytes; produced by openpete's
--halt-cpu-dump-output and pcsx-redux-ram-dump.lua's PCSXR_DUMP_CPU)
to discover sp at the halt instant, then zeros the stack region below
sp on both RAM dumps before bytewise comparison. Stack memory below
sp is dead state (pre-call sub-frames, BIOS scratch, etc.); diffing
it just measures "did both sides walk the exact same call chain on
the way here," which is rarely what the diff loop wants to verify.

Usage:
    cmp-masked.py op.ram oracle.ram op.cpu oracle.cpu

Prints sp + total + per-region byte counts. Exits 0 always (caller
reads counts from stdout).
"""
import struct, sys

if len(sys.argv) != 5:
    print(__doc__, file=sys.stderr)
    sys.exit(2)

op_ram, or_ram, op_cpu, or_cpu = sys.argv[1:5]


def load_sp(p: str) -> int:
    with open(p, 'rb') as f:
        b = f.read()
    if b[:4] != b'CSv1':
        raise SystemExit(f'{p}: not a CSv1 dump')
    # CSv1 layout: magic(4) + 32 GPRs(128); gpr[29] is sp.
    return struct.unpack_from('<I', b, 4 + 29 * 4)[0]


sp_op = load_sp(op_cpu)
sp_or = load_sp(or_cpu)
if sp_op != sp_or:
    print(f'!! sp mismatch op=0x{sp_op:08X} oracle=0x{sp_or:08X}')
sp = sp_op

with open(op_ram, 'rb') as f:
    op = bytearray(f.read())
with open(or_ram, 'rb') as f:
    ora = bytearray(f.read())

# Stack region: 0x801F0000..0x80200000. Zero everything below sp.
sp_phys = sp & 0x1FFFFF
for i in range(0x1F0000, sp_phys):
    op[i] = 0
    ora[i] = 0

counts = {'kernel': 0, 'EXE': 0, 'post-EXE': 0, 'heap': 0, 'stack': 0}
total = 0
for i, (a, b) in enumerate(zip(op, ora)):
    if a == b:
        continue
    total += 1
    if i < 0x010000:
        r = 'kernel'
    elif i < 0x076000:
        r = 'EXE'
    elif i < 0x080000:
        r = 'post-EXE'
    elif i < 0x1F0000:
        r = 'heap'
    else:
        r = 'stack'
    counts[r] += 1

bits = '  '.join(f'{k}={v}' for k, v in counts.items() if v)
print(f'sp=0x{sp:08X}  total={total}  {bits}')
