#!/usr/bin/env python3
"""Fence: every reader of the frame word's 21-bit stream field is
substituted - no more, no fewer, and at the instruction it claims.

An animation frame word packs the vertex-stream address into bits 0-20.
The game decodes it in two forms:

    sll r, r, 11          ; drop the top 11 bits
    srl r, r, 11 | 10     ; ... and shift back (10 = Spyro's halved encoding)

    and r, w, m           ; m = 0x1FFFFF, built in a register (lui/ori,
    sll r, r, 1           ;   lui/addiu, ...); Spyro's halved encoding

The final shift of each decode is substituted (games/spyro-1/manifest/
render_channels.toml, channel moby_vstream_field) with a resolver call that
treats field values below the handle bound as stream handles. The
substitution is what lets mod model data live outside the first 2 MB, and it
is only correct if it covers EVERY reader: a missed site reads a handle as a
byte address into the kernel area, silently.

This fence censuses the GENERATED C - PSX.EXE_full.c and every overlay's
_full.c, each file on its own (overlays share one load address, so a PC
names a different instruction in each) - because the decomp asm has no
overlay code at all. It decodes the opcode word in each line's trailing
comment (`/* 0xPC: 0xOPCODE */`), not the C text: after substitution the
line no longer holds the shift, so a textual census would find nothing.

The mask census propagates constants through lui/ori/addiu/sll/srl in each
file's PC order (callee-saved registers survive a jal) and records every
`and` with a register holding 0x1FFFFF. Each such `and` is a reader when the
next instruction is `sll rd, rd, 1` on its result; any other `and` refuses
unless FIELD_RELOCATORS lists it, and so does a built mask no `and` consumes.

It refuses:
  - a decode the census finds that the manifest does not list, including
    any in an overlay (--channel-sites is passed to the main EXE alone);
  - a manifest row naming a PC that is not the final shift of a decode (a
    drifted or mistyped address - the recompiler's anchor check is
    opcode-agnostic for replace_call, so it would substitute an unrelated
    instruction without complaint);
  - a row whose hook does not match the decode (moby for srl 11, halved for
    srl 10, halved_field for the mask-and sll 1);
  - a manifest row whose generated line lacks the substitution - a stale
    regen, which would run the stock decode at that site;
  - a mask-and that is not a recognized decode and not a listed relocator.

Run: python3 scripts/fences/vstream_sites.py [repo root]
Exit: 0 clean, 67 the field's readers and the manifest disagree,
      64 the generated tree or manifest is missing.
"""
import glob
import os
import re
import sys

try:
    import tomllib
except ImportError:  # 3.10
    import tomli as tomllib  # type: ignore

CHANNEL = "moby_vstream_field"
MAIN = "PSX.EXE_full.c"
FIELD_MASK = 0x1FFFFF
HOOK_SRL = {11: "psx_runtime_vstream_moby", 10: "psx_runtime_vstream_halved"}
HOOK_MASK_AND = "psx_runtime_vstream_halved_field"
TRAIL = re.compile(r"/\*\s*0x([0-9A-Fa-f]{8}):\s*0x([0-9A-Fa-f]{8})\s*\*/\s*$")
# $v0-$t9 minus the callee-saved $s0-$s7, $gp, $sp, $fp: clobbered by a call.
CALLER_SAVED = set(range(1, 16)) | {24, 25, 31}

# `and`s with the field mask that write the field rather than read a stream:
# the load-time relocation that adds the file base to each frame's field
# (func_80013230 Spyro's halved frames, func_800133E0 moby frames; the
# decomp's src/loaders.c). They run on disc data before a mod can bind a
# handle into the frame.
FIELD_RELOCATORS = {
    0x80013378, 0x80013384,                 # func_80013230
    0x8001356C, 0x80013578, 0x800135C8,     # func_800133E0
}


def decode(op):
    """@brief Split a MIPS instruction word into its fields."""
    return {
        "opcode": (op >> 26) & 0x3F,
        "rs": (op >> 21) & 0x1F,
        "rt": (op >> 16) & 0x1F,
        "rd": (op >> 11) & 0x1F,
        "shamt": (op >> 6) & 0x1F,
        "funct": op & 0x3F,
        "imm": op & 0xFFFF,
        "simm": (op & 0xFFFF) - (0x10000 if op & 0x8000 else 0),
    }


def is_sll(f, n):
    """@brief True when @p f is `sll rd, rt, n` with rd == rt."""
    return f["opcode"] == 0 and f["funct"] == 0 and f["shamt"] == n and f["rd"] == f["rt"]


def is_srl(f):
    """@brief True when @p f is `srl rd, rt, 10|11` with rd == rt."""
    return f["opcode"] == 0 and f["funct"] == 2 and f["shamt"] in (10, 11) and f["rd"] == f["rt"]


def scan_generated(root):
    """@brief Return {file basename: {pc: (opcode word, line)}} over every generated _full.c."""
    gen = os.path.join(root, "games", "spyro-1", "generated")
    files = sorted(glob.glob(os.path.join(gen, "*_full.c"))
                   + glob.glob(os.path.join(gen, "*", "*_full.c")))
    # _main_exe_pass1/ is the stub-discovery pass's intermediate copy of the
    # main EXE; it is not linked.
    files = [p for p in files if os.path.basename(os.path.dirname(p)) != "_main_exe_pass1"]
    tables = {}
    for path in files:
        table = {}
        with open(path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                m = TRAIL.search(line)
                if m:
                    table.setdefault(int(m.group(1), 16),
                                     (int(m.group(2), 16), line.rstrip("\n")))
        tables[os.path.basename(path)] = table
    return files, tables


def const_result(f, consts):
    """@brief Return (dest register, constant or None) for an instruction, or (None, None)."""
    op, fn = f["opcode"], f["funct"]
    src = consts.get(f["rs"], 0 if f["rs"] == 0 else None)
    if op == 0x0F:                                   # lui
        return f["rt"], (f["imm"] << 16) & 0xFFFFFFFF
    if op == 0x0D:                                   # ori
        return f["rt"], None if src is None else src | f["imm"]
    if op == 0x09:                                   # addiu
        return f["rt"], None if src is None else (src + f["simm"]) & 0xFFFFFFFF
    if op == 0 and fn in (0, 2):                     # sll, srl
        v = consts.get(f["rt"], 0 if f["rt"] == 0 else None)
        if v is None:
            return f["rd"], None
        return f["rd"], (v << f["shamt"]) & 0xFFFFFFFF if fn == 0 else v >> f["shamt"]
    if op == 0 and fn not in (0x08, 0x09, 0x0C, 0x0D, 0x18, 0x19, 0x1A, 0x1B, 0x11, 0x13):
        return f["rd"], None                          # other R-type writes rd
    if op == 0 and fn == 0x09:                       # jalr
        return f["rd"], None
    if op in (0x08, 0x0A, 0x0B, 0x0C, 0x0E, 0x20, 0x21, 0x23, 0x24, 0x25, 0x22, 0x26):
        return f["rt"], None
    return None, None


def mask_census(table):
    """@brief Return (mask-and pcs, built-mask pcs no `and` consumed) for one file."""
    consts, built, ands, orphans = {}, {}, [], []
    for pc in sorted(table):
        f = decode(table[pc][0])
        if f["opcode"] == 0 and f["funct"] == 0x24:       # and
            for r in (f["rs"], f["rt"]):
                if consts.get(r) == FIELD_MASK:
                    ands.append(pc)
                    built.pop(r, None)
                    break
        clobbered = set()
        if f["opcode"] == 0x03 or (f["opcode"] == 0 and f["funct"] == 0x09):  # jal, jalr
            clobbered |= CALLER_SAVED
        dst, val = const_result(f, consts)
        if dst:
            clobbered.add(dst)
        for r in clobbered:
            consts.pop(r, None)
            if r in built and not (r == dst and val == FIELD_MASK):
                orphans.append(built.pop(r))   # mask lost before any `and` read it
        if dst and val is not None:
            consts[dst] = val
            if val == FIELD_MASK:
                built.setdefault(dst, pc)
    return ands, sorted(set(orphans) | set(built.values()))


def census_file(name, table):
    """@brief Return ({pc: hook} decodes, [problem lines]) for one generated file."""
    sites, bad = {}, []
    for pc, (op, _line) in table.items():
        f = decode(op)
        nxt = table.get(pc + 4)
        if is_sll(f, 11) and nxt:
            g = decode(nxt[0])
            if is_srl(g) and g["rt"] == f["rd"]:
                sites[pc + 4] = HOOK_SRL[g["shamt"]]
    ands, orphans = mask_census(table)
    for pc in ands:
        f = decode(table[pc][0])
        nxt = table.get(pc + 4)
        g = decode(nxt[0]) if nxt else None
        if g and g["opcode"] == 0 and g["funct"] == 0 and g["shamt"] == 1 and g["rt"] == f["rd"]:
            sites[pc + 4] = HOOK_MASK_AND
        elif not (name == MAIN and pc in FIELD_RELOCATORS):
            bad.append(f"  {name} 0x{pc:08X}: `and` with the 0x1FFFFF field mask is neither "
                       f"an `and ; sll 1` decode nor a listed relocator - classify it")
    for pc in orphans:
        bad.append(f"  {name} 0x{pc:08X}: builds the 0x1FFFFF field mask and no `and` in "
                   f"PC order consumes it - classify the reader")
    return sites, bad


def check_row(pc, row, table, census_hook):
    """@brief Return the problem lines for one manifest row against the main EXE's generated C."""
    ent = table.get(pc)
    if not ent:
        return [f"  0x{pc:08X}: manifest row names a PC absent from the generated C"]
    op, line = ent
    if census_hook is None:
        return [f"  0x{pc:08X}: opcode 0x{op:08X} is not the final shift of a field decode "
                f"(manifest row drifted; the substitution would land on an unrelated instruction)"]
    bad = []
    if row.get("hook") != census_hook:
        bad.append(f"  0x{pc:08X}: the decode here needs hook {census_hook}, "
                   f"manifest says {row.get('hook')}")
    if row.get("operand") != "rt" or row.get("dest", "rd") != "rd":
        bad.append(f"  0x{pc:08X}: a shift's source is rt and its destination rd "
                   f"(row has operand={row.get('operand')!r} dest={row.get('dest')!r})")
    if "channel replace_call" not in line or f"{census_hook}(" not in line:
        bad.append(f"  0x{pc:08X}: generated line carries no substitution - stale regen "
                   f"(run OPENPETE_REGEN_SCOPE=main_exe scripts/regen-recomp.sh)")
    return bad


def main():
    """@brief Census the generated C against the manifest; return the exit code."""
    root = sys.argv[1] if len(sys.argv) > 1 else os.getcwd()
    manifest = os.path.join(root, "games", "spyro-1", "manifest", "render_channels.toml")
    if not os.path.exists(manifest):
        print(f"vstream_sites: no manifest at {manifest}", file=sys.stderr)
        return 64
    with open(manifest, "rb") as fh:
        data = tomllib.load(fh)
    rows = {}
    for ch in data.get("channel", []):
        if ch.get("name") != CHANNEL:
            continue
        for s in ch.get("substitution_sites") or []:
            if isinstance(s, dict):
                rows[int(s["addr"])] = s
    if not rows:
        print(f"vstream_sites: channel {CHANNEL!r} has no rows", file=sys.stderr)
        return 67

    files, tables = scan_generated(root)
    if MAIN not in tables:
        print("vstream_sites: no generated C found (run scripts/regen-recomp.sh)",
              file=sys.stderr)
        return 64

    bad, main_sites, found = [], {}, 0
    for name, table in sorted(tables.items()):
        sites, problems = census_file(name, table)
        bad += problems
        found += len(sites)
        if name == MAIN:
            main_sites = sites
            continue
        for pc in sorted(sites):
            bad.append(f"  {name} 0x{pc:08X}: field decode in an overlay; the manifest's "
                       f"substitutions reach the main EXE only")
    for pc in sorted(set(main_sites) - set(rows)):
        bad.append(f"  0x{pc:08X}: field decode found in generated C but not in the manifest")
    for pc in sorted(rows):
        bad += check_row(pc, rows[pc], tables[MAIN], main_sites.get(pc))

    if bad:
        print("vstream_sites: the frame-word field's readers and the manifest disagree:",
              file=sys.stderr)
        print("\n".join(bad), file=sys.stderr)
        return 67
    print(f"fence (vstream_sites): clean - {len(rows)} sites substituted, "
          f"{found} found across {len(files)} generated files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
