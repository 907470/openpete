#!/usr/bin/env python3
"""gen_game_structs.py — typed game structs for the mod SDK.

Emits every top-level `typedef struct` of the matching decomp's include/
(the layouts are authoritative: they build a byte-identical PSX binary),
preceded by the PSYQ types they embed (tools/psyq_types.py), rewritten
for 64-bit hosts viewing 32-bit guest RAM:

  - pointer fields and pointer arrays become `uint32_t` guest vaddrs (a
    host pointer would be 8 bytes and wreck the layout) — the original
    type is kept in a comment; dereference via api->guest()
  - PSX-width typedefs (u_char/u_short/u_int/u_long/uint/ushort, and
    `long`, which is 32-bit on MIPS) become fixed-width stdint types
  - the `#define`s that array bounds reference are emitted first

Every emitted struct is pinned by _Static_asserts on its size and on the
offset of every non-bitfield member, with the values read from
manifest/struct_layout.toml — the MIPS o32 layout probed by
tools/probe_struct_layout.py from the original headers — so the host
compile of a mod proves the rewritten layout equals the guest layout.
The toml carries a hash of the headers it was probed from; a checkout
whose hash differs is refused with the regen command, as is any
mismatch between the structs and pins emitted here and those in the
toml. The game's layout pins (tools/game_struct_config.py), each read
from its own asm or source, are asserted against the toml as a second
instrument. Everything that differs between decompilations — header
globs, PSYQ stub set, pins — comes from that config, keyed by --game.

Usage: gen_game_structs.py --game <slug> --include <decomp include dir>
                           --layout <struct_layout.toml> --out <header>
"""
import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import psyq_types as P  # noqa: E402
import game_struct_config as C  # noqa: E402

TYPEDEF_START_RE = re.compile(r"typedef\s+struct(?:\s+\w+)?\s*\{")

# `T (*name)(params);` and `T (**name[N])(params);`: the member is the
# name inside the parentheses, and the parameter list names no member.
FUNC_PTR_DECL_RE = re.compile(
    r"^[^()]*\(\s*\**\s*(\w+)\s*((?:\[[^\]]*\]\s*)*)\)\s*\(.*\)\s*$", re.S)

WIDTH_MAP = [
    (re.compile(r"\bu_char\b"), "uint8_t"),
    (re.compile(r"\bu_short\b"), "uint16_t"),
    (re.compile(r"\bu_int\b"), "uint32_t"),
    (re.compile(r"\bu_long\b"), "uint32_t"),  # MIPS long is 32-bit
    (re.compile(r"\buint\b"), "uint32_t"),
    (re.compile(r"\bushort\b"), "uint16_t"),
    (re.compile(r"\blong\b"), "int32_t"),
]

# `T *name;` and `T *name[N];` — the subscripts survive the rewrite.
PTR_FIELD_RE = re.compile(
    r"^(\s*)((?:struct\s+\w+|\w+)\s*\*+)\s*(\w+)((?:\s*\[[^\]]*\])*)\s*;(.*)$")

# `enum E name;`: the enumerations live in the decomp's headers, and MIPS
# gives an enum member int width, so the host view is a plain int32_t.
ENUM_FIELD_RE = re.compile(
    r"^(\s*)enum\s+(\w+)\s+(\w+)((?:\s*\[[^\]]*\])*)\s*;(.*)$")


def strip_comments(text: str) -> str:
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)


def headers(include_dir: Path, globs=("*.h",)):
    """The decomp headers a game's probe covers, in a stable order."""
    seen, out = set(), []
    for g in globs:
        for h in sorted(include_dir.glob(g)):
            if h not in seen:
                seen.add(h)
                out.append(h)
    return out


def enumerate_structs(include_dir: Path, globs=("*.h",)):
    """Every top-level `typedef struct [tag] { ... } Name;` in the game's
    headers, as (header path relative to include/, Name, raw body) in file
    order."""
    out = []
    for h in headers(include_dir, globs):
        text = h.read_text(encoding="utf-8")
        for m in TYPEDEF_START_RE.finditer(text):
            depth, i = 1, m.end()
            while depth and i < len(text):
                depth += text[i] == "{"
                depth -= text[i] == "}"
                i += 1
            tail = re.match(r"\s*(\w+)\s*;", text[i:])
            if tail:
                out.append((h.relative_to(include_dir).as_posix(),
                            tail.group(1), text[m.end():i - 1]))
    return out


def walk_members(body: str, prefix: str = ""):
    """Every member of a struct body as (dotted path, bitfield width or
    None), recursing into anonymous nested struct/union members (an array
    of nested blocks is walked through element 0)."""
    body = strip_comments(body)
    out, depth, cur, nested = [], 0, "", None
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
        if not decl:
            continue
        fm = FUNC_PTR_DECL_RE.match(decl)
        if fm:
            out.append((f"{prefix}{fm.group(1)}", None))
            continue
        bm = re.search(r"(\w+)\s*:\s*(\d+)$", decl)
        if bm:
            out.append((f"{prefix}{bm.group(1)}", int(bm.group(2))))
            continue
        # Declarator names are the words followed by `,` or the end, after
        # optional subscripts; a type word never sits in that position.
        for n, subs in re.findall(r"(\w+)\s*((?:\[[^\]]*\]\s*)*)(?:,|$)", decl):
            full = f"{prefix}{n}"
            out.append((full, None))
            if is_nested:
                elem = full + "[0]" * subs.count("[")
                out.extend(walk_members(nested, elem + "."))
    return out


def member_paths(body: str, prefix: str = ""):
    """Dotted member paths usable in offsetof: every member except
    bitfields (offsetof on a bitfield is a constraint violation); a
    bitfield's enclosing member is still listed."""
    return [p for p, w in walk_members(body, prefix) if w is None]


def bitfield_paths(body: str, prefix: str = ""):
    """(dotted path, width) of every bitfield member."""
    return [(p, w) for p, w in walk_members(body, prefix) if w is not None]


# `T (*name)(params);` on one source line, with an optional trailing
# comment: the whole declarator becomes one guest pointer.
FUNC_PTR_LINE_RE = re.compile(
    r"^(\s*)([^();]*\(\s*\**\s*)(\w+)((?:\s*\[[^\]]*\])*)(\s*\)\s*\([^;]*\))\s*;(.*)$")


def transform_body(body: str) -> str:
    out = []
    for line in body.splitlines():
        fm = FUNC_PTR_LINE_RE.match(line)
        if fm:
            indent, head, fname, subs, tail, rest = fm.groups()
            sig = re.sub(r"\s+", " ", (head + "*" + subs + tail).strip())
            line = (f"{indent}uint32_t {fname}{subs.strip()}; "
                    f"/**< Guest pointer: `{sig}`. */{rest}")
            out.append(line)
            continue
        em = ENUM_FIELD_RE.match(line)
        if em:
            indent, ename, fname, subs, rest = em.groups()
            line = (f"{indent}int32_t {fname}{subs.strip()}; "
                    f"/**< Guest enum: `enum {ename}`. */{rest}")
            out.append(line)
            continue
        pm = PTR_FIELD_RE.match(line)
        if pm:
            indent, ptype, fname, subs, rest = pm.groups()
            line = (f"{indent}uint32_t {fname}{subs.strip()}; "
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


def read_layout(path: Path):
    """struct_layout.toml -> (headers_sha256, {name: (size, [(path, off)])})."""
    text = path.read_text(encoding="utf-8")
    sha = re.search(r'^headers_sha256 = "([0-9a-f]{64})"', text, re.M)
    structs = {}
    for chunk in text.split("[[struct]]")[1:]:
        chunk = chunk.split("[[global]]")[0]
        name = re.search(r'^name = "(\w+)"', chunk, re.M).group(1)
        size = int(re.search(r"^size = (0x[0-9a-fA-F]+)", chunk, re.M).group(1), 16)
        fields = [(f, int(o, 16)) for f, o in re.findall(
            r'\{ name = "([^"]+)", offset = (0x[0-9a-fA-F]+) \}', chunk)]
        structs[name] = (size, fields)   # bitfield rows (bit =) carry no pin
    return (sha.group(1) if sha else None), structs


def headers_hash(include_dir: Path, globs=("*.h",), stubs=None) -> str:
    """The identity of everything the probe compiled: the game's headers,
    the PSYQ stubs it saw, and the compiler flags."""
    import hashlib
    import probe_struct_layout as PR
    h = hashlib.sha256()
    for f in headers(include_dir, globs):
        h.update(f.relative_to(include_dir).as_posix().encode())
        h.update(f.read_bytes())
    h.update(P.SYS_TYPES_H.encode())
    h.update(P.psyq_text(stubs).encode())
    h.update(" ".join(PR.FLAGS).encode())
    return h.hexdigest()


def topo_sort(structs, known: set):
    """Emission order: a struct after every struct it embeds by value.
    Pointer fields carry no edge (they are uint32_t after the rewrite)."""
    names = {n for _, n, _ in structs}
    bodies = {n: strip_comments(b) for _, n, b in structs}
    deps = {}
    for n, b in bodies.items():
        refs = set()
        for line in b.splitlines():
            if "*" in line.split("//")[0] and PTR_FIELD_RE.match(line):
                continue
            refs.update(w for w in re.findall(r"\b[A-Za-z_]\w*\b", line)
                        if w in names and w != n)
        deps[n] = refs
    order, done = [], set(known)
    pending = [n for _, n, _ in structs]
    while pending:
        step = [n for n in pending if deps[n] <= done | set()]
        step = [n for n in pending if all(d in done for d in deps[n])]
        if not step:
            raise SystemExit(f"[game-structs] ERROR: type cycle or unknown "
                             f"type among {sorted(pending)}")
        for n in step:
            order.append(n)
            done.add(n)
            pending.remove(n)
    return order


def bound_macros(include_dir: Path, bodies, globs=("*.h",)):
    """`#define NAME value` lines for every macro an array bound names."""
    used = set()
    for b in bodies:
        for sub in re.findall(r"\[([^\]]*)\]", strip_comments(b)):
            used.update(re.findall(r"\b[A-Z_][A-Z0-9_]+\b", sub))
    defs = {}
    for h in headers(include_dir, globs):
        for m in re.finditer(r"^#define (\w+)\s+(.+?)\s*(?:/[*/].*)?$",
                             h.read_text(encoding="utf-8"), re.M):
            defs[m.group(1)] = m.group(2)
    out, seen, queue = [], set(), sorted(used)
    while queue:
        name = queue.pop(0)
        if name in seen or name not in defs:
            continue
        seen.add(name)
        # a macro's value may name further macros; emit those first
        inner = [w for w in re.findall(r"\b[A-Z_][A-Z0-9_]+\b", defs[name])
                 if w in defs and w not in seen]
        if inner:
            queue = inner + [name] + queue
            seen.discard(name)
            continue
        out.append(f"#ifndef {name}\n#define {name} {defs[name]}\n#endif")
    missing = sorted(u for u in used if u not in defs)
    if missing:
        raise SystemExit(f"[game-structs] ERROR: array bound macros not "
                         f"found in include/: {missing}")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default="spyro-1",
                    help="game slug, keying tools/game_struct_config.py")
    ap.add_argument("--include", type=Path, required=True)
    ap.add_argument("--layout", type=Path, required=True,
                    help="manifest/struct_layout.toml from "
                         "scripts/regen-struct-layout.sh")
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()

    cfg = C.config(args.game)
    globs = tuple(cfg["header_globs"])
    stubs = cfg["psyq_stubs"]

    sha, layout = read_layout(args.layout)
    want = headers_hash(args.include, globs, stubs)
    if sha != want:
        raise SystemExit(
            f"[game-structs] ERROR: {args.layout} was probed from different "
            f"headers (toml {str(sha)[:12]}, checkout {want[:12]}); run "
            f"GAME={args.game} bash scripts/regen-struct-layout.sh "
            f"(needs mipsel-linux-gnu-gcc) and commit the result")

    structs = enumerate_structs(args.include, globs)
    psyq_names = P.psyq_names(stubs)
    order = topo_sort(structs, set(psyq_names))
    bodies = {n: b for _, n, b in structs}

    # The SDK-facing spelling of a PSYQ type carries the PSX_ prefix, in
    # its own definition and in every game-struct field that embeds it:
    # <windows.h> defines RECT, and a mod including both headers would
    # otherwise see two definitions of the name.
    blocks = []
    for _, text in P.psyq_types(stubs):
        blocks.append(transform_body(P.psx_prefixed(text, psyq_names)) + "\n")
    for name in order:
        body = transform_body(P.psx_prefixed(bodies[name], psyq_names))
        # Named tag matching the typedef: COMPLETES the opaque forward
        # declarations the natural-signature wrappers emit (their
        # `typedef struct Moby Moby;` and ours legally coexist in C11),
        # so wrapper args of type Moby* become field-addressable.
        blocks.append(f"typedef struct {name} {{\n{body}\n}} {name};\n")

    emitted = psyq_names + order
    if set(emitted) != set(layout):
        raise SystemExit(f"[game-structs] ERROR: emitted structs differ from "
                         f"the toml: only here {sorted(set(emitted) - set(layout))}, "
                         f"only in toml {sorted(set(layout) - set(emitted))}")
    asserts = []
    for name in emitted:
        emit_name = P.psx_prefixed(name, psyq_names)
        size, fields = layout[name]
        paths = member_paths(bodies[name]) if name in bodies else \
            member_paths(re.search(rf"typedef struct {name} \{{(.*?)\}} {name};",
                                   P.psyq_text(stubs), re.S).group(1))
        if paths != [f for f, _ in fields]:
            raise SystemExit(f"[game-structs] ERROR: {name} member paths differ "
                             f"from the toml; regenerate struct_layout.toml")
        asserts.append(f'_Static_assert(sizeof({emit_name}) == {size:#x}, '
                       f'"{emit_name} size drifted");')
        for f, off in fields:
            asserts.append(f'_Static_assert(offsetof({emit_name}, {f}) == '
                           f'{off:#x}, "{emit_name}.{f} offset drifted");')

    # The game's own layout facts are a second instrument on the probe.
    for pin in cfg["pins"]:
        st = pin["struct"]
        if st not in layout:
            raise SystemExit(f"[game-structs] ERROR: pin names {st}, which "
                             f"the toml does not carry")
        if "size" in pin and layout[st][0] != pin["size"]:
            raise SystemExit(f"[game-structs] ERROR: probed sizeof({st}) "
                             f"{layout[st][0]:#x} != {pin['size']:#x} from "
                             f"{pin['why']}")
        if "field" in pin:
            got = dict(layout[st][1]).get(pin["field"])
            if got != pin["offset"]:
                raise SystemExit(f"[game-structs] ERROR: probed {st}."
                                 f"{pin['field']} {got} != {pin['offset']:#x} "
                                 f"from {pin['why']}")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(
        "/**\n"
        " * @file openpete_game_structs.h\n"
        " * @brief Typed views of game structs in guest RAM.\n"
        " *\n"
        " * Generated by the engine build; do not edit.\n"
        " *\n"
        " * Layouts come from the decompilation, which builds a byte-identical\n"
        " * PSX binary; the PSYQ types the game embeds come first, under a PSX_\n"
        " * prefix (PSX_MATRIX, PSX_RECT, PSX_DRAWENV, ...) so a translation\n"
        " * unit may also include <windows.h>, which defines RECT. Pointer\n"
        " * fields are uint32_t guest vaddrs; resolve them with `api->guest()`.\n"
        " * The `_Static_assert`s at the end pin every size and member offset\n"
        " * to the MIPS layout, so a layout change fails the mod's compile.\n"
        " */\n"
        "#ifndef OPENPETE_GAME_STRUCTS_H\n"
        "#define OPENPETE_GAME_STRUCTS_H\n\n"
        "#include <stddef.h>\n"
        "#include <stdint.h>\n\n"
        + "\n".join(bound_macros(args.include, bodies.values(), globs)) + "\n\n"
        + "\n".join(blocks) + "\n"
        + "\n".join(asserts) + "\n\n"
        "#endif /* OPENPETE_GAME_STRUCTS_H */\n",
        encoding="utf-8")
    print(f"[game-structs] wrote {args.out} "
          f"({len(order)} structs + {len(psyq_names)} PSYQ types, "
          f"{len(asserts)} pins)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
