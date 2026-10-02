#!/usr/bin/env python3
"""Fence: every reader of the moby frame word's 21-bit stream field is
substituted - no more, no fewer, and at the instruction it claims.

An animation frame word packs the vertex-stream address into bits 0-20.
Every reader in the game decodes it with one idiom:

    sll r, r, 11        ; drop the top 11 bits
    srl r, r, 11 | 10   ; ... and shift back (10 = Spyro's halved encoding)

The srl of each pair is substituted (games/spyro-1/manifest/render_channels.toml, channel
moby_vstream_field) with a resolver call that treats field values below the
handle bound as stream handles. The substitution is what lets mod model data
live outside the first 2 MB, and it is only correct if it covers EVERY
reader: a missed site reads a handle as a byte address into the kernel area
and walks garbage vertices, silently.

This fence censuses the GENERATED C - PSX.EXE_full.c and every overlay's
_full.c - because the decomp asm has no overlay code at all. It decodes the
opcode word in each line's trailing comment (`/* 0xPC: 0xOPCODE */`), not
the C text: after substitution the line no longer contains `>> 11`, so a
textual census would find nothing and flag every manifest row. It refuses:

  - a site the census finds that the manifest does not list;
  - a manifest row naming a PC that is not an srl-by-11|10 following an
    sll-by-11 on the same register (a drifted or mistyped address - the
    recompiler's anchor check is opcode-agnostic for replace_call, so it
    would substitute an unrelated instruction without complaint);
  - a row whose hook does not match the shift (moby for 11, halved for 10);
  - a manifest row whose generated line lacks the substitution - a stale
    regen, which would run the stock decode at that site.

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
HOOK_FOR_SHAMT = {11: "psx_runtime_vstream_moby", 10: "psx_runtime_vstream_halved"}
TRAIL = re.compile(r"/\*\s*0x([0-9A-Fa-f]{8}):\s*0x([0-9A-Fa-f]{8})\s*\*/\s*$")


def decode(op):
    return {
        "opcode": (op >> 26) & 0x3F,
        "rs": (op >> 21) & 0x1F,
        "rt": (op >> 16) & 0x1F,
        "rd": (op >> 11) & 0x1F,
        "shamt": (op >> 6) & 0x1F,
        "funct": op & 0x3F,
    }


def is_sll11(f):
    return f["opcode"] == 0 and f["funct"] == 0 and f["shamt"] == 11 and f["rd"] == f["rt"]


def is_srl(f):
    return f["opcode"] == 0 and f["funct"] == 2 and f["shamt"] in (10, 11) and f["rd"] == f["rt"]


def scan_generated(root):
    """pc -> (opcode word, line text) over every generated _full.c."""
    gen = os.path.join(root, "games", "spyro-1", "generated")
    files = sorted(glob.glob(os.path.join(gen, "*_full.c"))
                   + glob.glob(os.path.join(gen, "*", "*_full.c")))
    table = {}
    for path in files:
        with open(path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                m = TRAIL.search(line)
                if not m:
                    continue
                pc = int(m.group(1), 16)
                table[pc] = (int(m.group(2), 16), line.rstrip("\n"), path)
    return files, table


def main():
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

    files, table = scan_generated(root)
    if not files:
        print("vstream_sites: no generated C found (run scripts/regen-recomp.sh)",
              file=sys.stderr)
        return 64

    # Census: the srl of every sll11/srl pair on one register.
    census = {}
    for pc, (op, _line, path) in table.items():
        f = decode(op)
        if not is_sll11(f):
            continue
        nxt = table.get(pc + 4)
        if not nxt:
            continue
        g = decode(nxt[0])
        if is_srl(g) and g["rt"] == f["rd"]:
            census[pc + 4] = g["shamt"]

    bad = []
    for pc in sorted(set(census) - set(rows)):
        bad.append(f"  0x{pc:08X}: field decode found in generated C but not in the manifest "
                   f"({os.path.relpath(table[pc][2], root)})")
    for pc in sorted(rows):
        row = rows[pc]
        ent = table.get(pc)
        if not ent:
            bad.append(f"  0x{pc:08X}: manifest row names a PC absent from the generated C")
            continue
        op, line, path = ent
        f = decode(op)
        prev = table.get(pc - 4)
        if not (is_srl(f) and prev and is_sll11(decode(prev[0]))
                and decode(prev[0])["rd"] == f["rt"]):
            bad.append(f"  0x{pc:08X}: opcode 0x{op:08X} is not the idiom's srl "
                       f"(manifest row drifted; the substitution would land on an unrelated instruction)")
            continue
        want = HOOK_FOR_SHAMT[f["shamt"]]
        if row.get("hook") != want:
            bad.append(f"  0x{pc:08X}: srl by {f['shamt']} needs hook {want}, manifest says {row.get('hook')}")
        if row.get("operand") != "rt" or row.get("dest", "rd") != "rd":
            bad.append(f"  0x{pc:08X}: a shift's source is rt and its destination rd "
                       f"(row has operand={row.get('operand')!r} dest={row.get('dest')!r})")
        if "channel replace_call" not in line or want not in line:
            bad.append(f"  0x{pc:08X}: generated line carries no substitution - stale regen "
                       f"(run OPENPETE_REGEN_SCOPE=main_exe scripts/regen-recomp.sh)")

    if bad:
        print("vstream_sites: the frame-word field's readers and the manifest disagree:",
              file=sys.stderr)
        print("\n".join(bad), file=sys.stderr)
        return 67
    print(f"fence (vstream_sites): clean - {len(rows)} sites substituted, "
          f"{len(census)} found across {len(files)} generated files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
