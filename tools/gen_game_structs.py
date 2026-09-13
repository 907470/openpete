#!/usr/bin/env python3
"""gen_game_structs.py — typed game structs for the mod SDK (the
"generated headers over guest RAM" half of the P1 SDK-generation item).

Extracts a CURATED set of struct typedefs from the matching decomp's
headers (the layouts are authoritative: they build a byte-identical
PSX binary) and rewrites them for 64-bit hosts viewing 32-bit guest
RAM:

  - pointer fields become `uint32_t` guest vaddrs (a host pointer would
    be 8 bytes and wreck the layout) — the original type is kept in a
    comment; dereference via api->guest()
  - PSX-width typedefs (u_char/u_short/u_int/u_long — long is 32-bit
    on MIPS) become fixed-width stdint types

Every emitted struct is pinned by _Static_asserts on its total size
and on empirically verified field offsets (cross-checked against the
port's native extractors), so any layout drift — decomp edits, host
packing surprises — explodes at MOD COMPILE TIME, never at runtime.

Usage: gen_game_structs.py --include <decomp include dir> --out <header>
"""
import argparse
import re
import sys
from pathlib import Path

# (header file, typedef name) in emission order (leaf types first).
CURATED = [
    ("vector.h", "Vector3D"),
    ("vector.h", "Vector3D8"),
    ("matrix.h", "SHORTMATRIX"),
    ("moby.h",   "AnimationState"),
    ("moby.h",   "Moby"),
]

# Compile-time pins: (type, sizeof) and (type, field, offset).
# Offsets are the port's ground truth (include/moby.h cross-checked
# against asm/42CC4.s and features/moby_native.c — recon 2026-07-11).
SIZES = {
    "Vector3D": 12,
    "Vector3D8": 3,
    "SHORTMATRIX": 20,
    "AnimationState": 7,
    "Moby": 0x58,
}
OFFSETS = [
    ("Moby", "m_Props", 0x00),
    ("Moby", "m_Position", 0x0C),
    ("Moby", "m_DamageFlags", 0x18),
    ("Moby", "m_RotationMatrix", 0x20),
    ("Moby", "m_CollisionRegion", 0x34),
    ("Moby", "m_Class", 0x36),
    ("Moby", "m_AnimationState", 0x3C),
    ("Moby", "m_Rotation", 0x44),
    ("Moby", "m_State", 0x48),
    ("Moby", "m_SectorIndex", 0x4A),
    ("Moby", "m_RenderRadius", 0x50),
    ("Moby", "m_WasDrawn", 0x51),
    ("Moby", "m_UpdateDistance", 0x52),
]

TYPEDEF_START_RE = re.compile(r"typedef\s+struct(?:\s+\w+)?\s*\{")

WIDTH_MAP = [
    (re.compile(r"\bu_char\b"), "uint8_t"),
    (re.compile(r"\bu_short\b"), "uint16_t"),
    (re.compile(r"\bu_int\b"), "uint32_t"),
    (re.compile(r"\bu_long\b"), "uint32_t"),  # MIPS long is 32-bit
]

PTR_FIELD_RE = re.compile(
    r"^(\s*)((?:struct\s+\w+|\w+)\s*\*+)\s*(\w+)\s*;(.*)$")


def strip_comments(text: str) -> str:
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)


def enumerate_structs(include_dir: Path):
    """Every top-level `typedef struct [tag] { ... } Name;` in include/*.h,
    as (header basename, Name, raw body) in file order."""
    out = []
    for h in sorted(include_dir.glob("*.h")):
        text = h.read_text(encoding="utf-8")
        for m in TYPEDEF_START_RE.finditer(text):
            depth, i = 1, m.end()
            while depth and i < len(text):
                depth += text[i] == "{"
                depth -= text[i] == "}"
                i += 1
            tail = re.match(r"\s*(\w+)\s*;", text[i:])
            if tail:
                out.append((h.name, tail.group(1), text[m.end():i - 1]))
    return out


def member_paths(body: str, prefix: str = ""):
    """Dotted member paths of a struct body usable in offsetof: every
    declarator at depth 0, recursing into anonymous nested struct/union
    members. Bitfield members are omitted (offsetof on a bitfield is a
    constraint violation); their enclosing member is still listed."""
    body = strip_comments(body)
    paths, depth, cur, nested = [], 0, "", None
    for ch in body:
        if depth > 0:
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    cur = "\0"   # the declarator that follows names a block
                    continue
            nested += ch
            continue
        if ch == "{":
            depth, nested = 1, ""
            continue
        if ch != ";":
            cur += ch
            continue
        decl, cur = cur.strip(), ""
        is_nested = decl.startswith("\0")
        decl = decl.lstrip("\0").strip()
        if not decl or re.search(r":\s*\d+$", decl):
            continue
        # Declarator names are the words followed by `,` or the end, after
        # optional subscripts; a type word never sits in that position.
        names = re.findall(r"(\w+)\s*((?:\[[^\]]*\]\s*)*)(?:,|$)", decl)
        for n, subs in names:
            full = f"{prefix}{n}"
            paths.append(full)
            if is_nested:
                # an array of nested blocks is walked through element 0
                elem = full + "[0]" * subs.count("[")
                paths.extend(member_paths(nested, elem + "."))
    return paths


def extract(header_text: str, name: str) -> str:
    """Brace-matched typedef body (handles nested unions/structs)."""
    for m in TYPEDEF_START_RE.finditer(header_text):
        depth, i = 1, m.end()
        while depth and i < len(header_text):
            if header_text[i] == "{":
                depth += 1
            elif header_text[i] == "}":
                depth -= 1
            i += 1
        tail = re.match(r"\s*(\w+)\s*;", header_text[i:])
        if tail and tail.group(1) == name:
            return header_text[m.end():i - 1]
    raise SystemExit(f"[game-structs] ERROR: typedef {name} not found")


def transform_body(body: str) -> str:
    out = []
    for line in body.splitlines():
        pm = PTR_FIELD_RE.match(line)
        if pm:
            indent, ptype, fname, rest = pm.groups()
            line = (f"{indent}uint32_t {fname}; "
                    f"/**< Guest pointer: `{ptype.strip()}`. */{rest}")
        for rx, repl in WIDTH_MAP:
            line = rx.sub(repl, line)
        tm = re.match(r"^(.*;\s*)/\*(?!\*)\s*(.*?)\s*\*/(.*)$", line)
        if tm:
            line = f"{tm.group(1)}/**< {tm.group(2).rstrip('.')}. */{tm.group(3)}"
        out.append(line)
    out = fold_doc_comments(out)
    # squeeze runs of blank lines
    text = "\n".join(out)
    return re.sub(r"\n{3,}", "\n\n", text).strip("\n")


DOC_LINE_RE = re.compile(r"^(\s*)///\s?(?:@brief\s+)?(.*)$")


def fold_doc_comments(lines):
    """A run of `///` field-doc lines -> one `/* brief */` comment
    carrying the run's first line (the @brief); continuation lines are
    the decompilation's working notes and are dropped."""
    out, run, indent = [], [], ""
    def flush():
        if not run:
            return
        body = [l for l in run if l.strip()]
        if body:
            brief = re.split(r"\.\s", body[0], maxsplit=1)[0].rstrip(".")
            out.append(f"{indent}/** @brief {brief}. */")
        run.clear()
    for line in lines:
        m = DOC_LINE_RE.match(line)
        if m:
            indent = m.group(1)
            run.append(m.group(2).rstrip())
        else:
            flush()
            out.append(line)
    flush()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--include", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()

    blocks = []
    for header, name in CURATED:
        text = (args.include / header).read_text(encoding="utf-8")
        body = transform_body(extract(text, name))
        # Named tag matching the typedef: COMPLETES the opaque forward
        # declarations the natural-signature wrappers emit (their
        # `typedef struct Moby Moby;` and ours legally coexist in C11),
        # so wrapper args of type Moby* become field-addressable.
        blocks.append(f"typedef struct {name} {{\n{body}\n}} {name};\n")

    asserts = []
    for t, sz in SIZES.items():
        asserts.append(f'_Static_assert(sizeof({t}) == {sz:#x}, '
                       f'"{t} size drifted");')
    for t, f, off in OFFSETS:
        asserts.append(f'_Static_assert(offsetof({t}, {f}) == {off:#x}, '
                       f'"{t}.{f} offset drifted");')

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(
        "/**\n"
        " * @file openpete_game_structs.h\n"
        " * @brief Typed views of game structs in guest RAM.\n"
        " *\n"
        " * Generated by the engine build; do not edit.\n"
        " *\n"
        " * Layouts come from the decompilation, which builds a byte-identical\n"
        " * PSX binary. Pointer fields are uint32_t guest vaddrs; resolve them\n"
        " * with `api->guest()`. The `_Static_assert`s at the end pin every\n"
        " * size and offset, so a layout change fails the mod's compile.\n"
        " */\n"
        "#ifndef OPENPETE_GAME_STRUCTS_H\n"
        "#define OPENPETE_GAME_STRUCTS_H\n\n"
        "#include <stddef.h>\n"
        "#include <stdint.h>\n\n"
        + "\n".join(blocks) + "\n"
        + "\n".join(asserts) + "\n\n"
        "#endif /* OPENPETE_GAME_STRUCTS_H */\n",
        encoding="utf-8")
    print(f"[game-structs] wrote {args.out} "
          f"({len(CURATED)} structs, {len(asserts)} pins)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
