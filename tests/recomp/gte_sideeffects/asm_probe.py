#!/usr/bin/env python3
"""Emit the raw MIPS probe binary for scripts/check_recomp_gte.sh.

Five leaf functions, 64 bytes apart, loaded at 0x80010000. Each has the
standard prologue/epilogue so the recompiler's function analyzer accepts
it, and reads the register under test back through mfc2 so the read path
is exercised alongside the write:

  0x80010000  mtc2 $a0,$30 ; mfc2 $v0,$31             LZCS write -> LZCR
  0x80010040  mtc2 $a0,$28 ; mfc2 $v0,$9 $v1,$10 $a1,$11   IRGB write -> IR1-3
  0x80010080  lwc2 $30,0($a0) ; mfc2 $v0,$31
  0x800100C0  lwc2 $28,4($a0) ; mfc2 $v0,$9 $v1,$10 $a1,$11
  0x80010100  mtc2 $a0,$9 ; mfc2 $v0,$9               control: plain data register
"""
import struct
import sys

A0, A1, V0, V1 = 4, 5, 2, 3


def mtc2(rt, rd):
    return 0x48800000 | (rt << 16) | (rd << 11)


def mfc2(rt, rd):
    return 0x48000000 | (rt << 16) | (rd << 11)


def lwc2(rt, rs, off):
    return 0xC8000000 | (rs << 21) | (rt << 16) | (off & 0xFFFF)


NOP = 0
JR_RA = 0x03E00008
ADDIU_SP_M8 = 0x27BDFFF8
SW_RA_SP = 0xAFBF0000
LW_RA_SP = 0x8FBF0000
ADDIU_SP_8 = 0x27BD0008


def fn(body):
    words = [ADDIU_SP_M8, SW_RA_SP] + body + [LW_RA_SP, ADDIU_SP_8, JR_RA, NOP]
    assert len(words) <= 16
    return words + [NOP] * (16 - len(words))


def main():
    words = []
    words += fn([mtc2(A0, 30), NOP, mfc2(V0, 31)])
    words += fn([mtc2(A0, 28), NOP, mfc2(V0, 9), mfc2(V1, 10), mfc2(A1, 11)])
    words += fn([lwc2(30, A0, 0), NOP, mfc2(V0, 31)])
    words += fn([lwc2(28, A0, 4), NOP, mfc2(V0, 9), mfc2(V1, 10), mfc2(A1, 11)])
    words += fn([mtc2(A0, 9), NOP, mfc2(V0, 9)])
    with open(sys.argv[1], "wb") as f:
        f.write(b"".join(struct.pack("<I", w) for w in words))
    return 0


if __name__ == "__main__":
    sys.exit(main())
