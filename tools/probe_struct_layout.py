#!/usr/bin/env python3
"""probe_struct_layout.py - MIPS-ABI layout of every decomp game struct.

Compiles one probe TU that includes every header of a game's decomp
include/ under mipsel-linux-gnu-gcc with the decomp's own flags, using
stub PSYQ headers generated from tools/psyq_types.py, and reads sizeof /
offsetof of every struct and member path back out of the assembler
output. Writes games/<game>/manifest/struct_layout.toml, which
gen_game_structs.py turns into _Static_assert pins in the typed structs
header, so a host compile against that header proves the transformed
layout equals the guest layout.

One probe serves every game; what differs between decompilations (header
globs, PSYQ stub set, unknown-member naming convention, layout pins) is
tools/game_struct_config.py, keyed by --game.

Only this step needs the MIPS compiler; the toml is committed and every
other build reads it. The toml carries a hash of the headers plus the
stubs and flags; the generator refuses a checkout whose hash differs.

Engagement checks (refused on failure, so a wrong stub cannot pass):
  - every member the game names after its own offset re-derives that
    offset, under the config's naming convention (a struct listed as
    using the same spelling for a sequence number instead must have a
    member that proves it, or the run refuses);
  - each member whose trailing comment starts with `0xNN` sits at that
    offset, and members commented with an absolute address keep their
    address spacing (config `comment_offsets`);
  - the config's pins, each read from the game's own asm or source;
  - every struct-typed `extern` global with a `.space` in asm/data/*.s
    occupies count * sizeof plus 0 or 4 bytes of alignment slack, or is a
    split symbol smaller than its struct (recorded as such).

Usage: probe_struct_layout.py --game <slug> --decomp <checkout> --out <toml>
"""
import argparse
import hashlib
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_game_structs as G          # noqa: E402
import game_struct_config as C        # noqa: E402
import psyq_types as P                # noqa: E402

CC = "mipsel-linux-gnu-gcc"
FLAGS = ["-S", "-march=mips1", "-mabi=32", "-msoft-float", "-ffreestanding"]
# A zero-valued global is emitted as `.space 4`, not `.word 0`, whatever the
# bss flags; every probe value is stored +1 so it always has a `.word`.
BIAS = 1

# A member's own source line carries its offset (`0xNN`) or its absolute
# address (eight hex digits) as the first word of a trailing comment.
COMMENT_OFFSET_RE = re.compile(r"^\s*(?:0x(?P<off>[0-9a-fA-F]+)"
                               r"|(?P<addr>[0-9a-fA-F]{8}))\b")


def write_stubs(d: Path, stubs, empty):
    (d / "sys").mkdir(parents=True, exist_ok=True)
    (d / "sys" / "types.h").write_text(P.SYS_TYPES_H)
    for name, text in P.psyq_types(stubs):
        guard = name.upper().replace(".", "_")
        (d / name).write_text(f"#ifndef {guard}\n#define {guard}\n"
                              f"#include <sys/types.h>\n{text}#endif\n")
    for name in empty:
        (d / name).write_text("")


def sym(path: str) -> str:
    """Member path -> identifier fragment (`a[0].b` -> `a_0__b`)."""
    return re.sub(r"[^\w]+", "_", path.replace(".", "__")).strip("_")


def probe_tu(include_dir: Path, structs, extra_names, globs):
    lines = [f'#include "{h.relative_to(include_dir).as_posix()}"'
             for h in G.headers(include_dir, globs)]
    keep = []
    for _, name, body in structs:
        lines.append(f"unsigned long sz_{name} = sizeof({name}) + {BIAS};")
        for path in G.member_paths(body):
            lines.append(f"unsigned long off_{name}__{sym(path)} "
                         f"= __builtin_offsetof({name}, {path}) + {BIAS};")
        # A bitfield has no offsetof; an object with only that field set
        # to all ones is emitted as data, and its bytes give the bit range.
        for path, width in G.bitfield_paths(body):
            v = f"bf_{name}__{sym(path)}"
            lines.append(f"static const {name} {v} = {{ .{path} = "
                         f"{(1 << width) - 1:#x}u }};")
            keep.append(v)
    if keep:
        lines.append("const void *bf_keep[] = { " + ", ".join(f"&{v}" for v in keep) + " };")
    for name, paths in extra_names:
        lines.append(f"unsigned long sz_{name} = sizeof({name}) + {BIAS};")
        for path in paths:
            lines.append(f"unsigned long off_{name}__{sym(path)} "
                         f"= __builtin_offsetof({name}, {path}) + {BIAS};")
    return "\n".join(lines) + "\n"


def read_asm(path: Path):
    """{sz_/off_ symbol: value} and {bf_ symbol: bytes of the object}."""
    vals, images, cur, img = {}, {}, None, None
    for l in path.read_text().splitlines():
        m = re.match(r"^(sz_\w+|off_\w+|bf_\w+):", l)
        if m:
            cur = m.group(1)
            img = [] if cur.startswith("bf_") else None
            if img is not None:
                images[cur] = img
            continue
        if img is not None:
            m = re.match(r"\s*\.(byte|half|short|word|long|space)\s+(-?\d+)", l)
            if not m:
                if re.match(r"\s*\.(globl|type|size|align|section)\b", l) and img:
                    img = None
                continue
            kind, n = m.group(1), int(m.group(2))
            if kind == "space":
                img.extend([0] * n)
            else:
                width = {"byte": 1, "half": 2, "short": 2, "word": 4, "long": 4}[kind]
                img.extend((n & ((1 << (8 * width)) - 1)).to_bytes(width, "little"))
            continue
        m = re.match(r"\s*\.(?:word|long|4byte)\s+(-?\d+)", l)
        if m and cur:
            vals[cur] = int(m.group(1)) - BIAS
            cur = None
    return vals, images


def comment_annotations(body: str):
    """{member name: ("off"|"addr", value)} from each single-declarator
    source line whose trailing comment starts with the member's offset or
    its absolute address. The decomp writes these by hand next to the
    member, so they are independent of the layout the compiler produces."""
    out = {}
    for line in body.splitlines():
        code, _, comment = line.partition("//")
        if not comment or ";" not in code:
            continue
        decl = code.split(";")[0].strip()
        fm = G.FUNC_PTR_DECL_RE.match(decl)
        if fm:
            names = [fm.group(1)]
        else:
            names = [n for n, _ in re.findall(
                r"(\w+)\s*((?:\[[^\]]*\]\s*)*)(?:,|$)", decl)]
        if len(names) != 1:
            continue
        m = COMMENT_OFFSET_RE.match(comment)
        if not m:
            continue
        out[names[0]] = (("off", int(m.group("off"), 16)) if m.group("off")
                         else ("addr", int(m.group("addr"), 16)))
    return out


def bss_globals(decomp: Path, sizes: dict, globs):
    """(name, type, count, space) for every `extern T name[N];` whose T is
    a probed struct and whose symbol carries a `.space` in asm/data."""
    space = {}
    for s in sorted((decomp / "asm" / "data").glob("*.s")):
        ls = s.read_text(errors="replace").splitlines()
        for i, l in enumerate(ls):
            m = re.match(r"^dlabel (\w+)", l)
            if m and i + 1 < len(ls):
                m2 = re.search(r"\.space (0x[0-9a-fA-F]+|\d+)", ls[i + 1])
                if m2:
                    space[m.group(1)] = int(m2.group(1), 0)
    out = []
    for h in G.headers(decomp / "include", globs):
        for m in re.finditer(r"^extern\s+(\w+)\s+(\w+)((?:\[[^\]]*\])*)\s*;",
                             h.read_text(), re.M):
            t, name, subs = m.groups()
            if t not in sizes or name not in space:
                continue
            count = 1
            for n in re.findall(r"\[([^\]]*)\]", subs):
                count *= int(n, 0)
            out.append((name, t, count, space[name]))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default="spyro-1",
                    help="game slug, keying tools/game_struct_config.py")
    ap.add_argument("--decomp", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    inc = args.decomp / "include"
    cfg = C.config(args.game)
    header_globs = tuple(cfg["header_globs"])
    stub_set = cfg["psyq_stubs"]

    structs = G.enumerate_structs(inc, header_globs)
    psyq = [(n, G.member_paths(re.search(
        rf"typedef struct {n} \{{(.*?)\}} {n};",
        P.psyq_text(stub_set), re.S).group(1)))
            for n in P.psyq_names(stub_set)]

    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        stubs = td / "stubs"
        write_stubs(stubs, stub_set, cfg["empty_stubs"])
        (td / "probe.c").write_text(
            probe_tu(inc, structs, psyq, header_globs))
        r = subprocess.run([CC, *FLAGS, f"-I{inc}", f"-I{stubs}",
                            "-o", str(td / "probe.s"), str(td / "probe.c")],
                           capture_output=True, text=True)
        if r.returncode:
            sys.exit(f"[probe] {CC} failed:\n{r.stderr}")
        vals, images = read_asm(td / "probe.s")

    errors = []
    sizes = {n: vals[f"sz_{n}"] for _, n, _ in structs}
    bits = {}   # (struct, path) -> (first bit, width), bit 0 = struct start
    for _, name, body in structs:
        for path, width in G.bitfield_paths(body):
            img = images[f"bf_{name}__{sym(path)}"]
            img = img + [0] * (sizes[name] - len(img))
            n = int.from_bytes(bytes(img), "little")
            set_bits = [i for i in range(8 * sizes[name]) if n >> i & 1]
            if len(set_bits) != width or set_bits != list(range(set_bits[0], set_bits[0] + width)):
                errors.append(f"{name}.{path}: bitfield image {set_bits} is not "
                              f"{width} contiguous bits")
                continue
            bits[(name, path)] = (set_bits[0], width)
    sizes.update({n: vals[f"sz_{n}"] for n, _ in psyq})

    # Members the game names after their own offset.
    unk_re = re.compile(cfg["unk_offset_re"])
    skip = cfg["unk_offset_skip"]
    n_unk, skip_used = 0, set()
    for _, name, body in structs:
        for path in G.member_paths(body):
            m = unk_re.fullmatch(path)
            if not m:
                continue
            want = int(m.group(1), 16)
            got = vals[f"off_{name}__{sym(path)}"]
            if "." in path:
                # a member of a nested block is named relative to the block
                parent = re.sub(r"\[0\]", "", path.rsplit(".", 1)[0])
                got -= vals[f"off_{name}__{sym(parent)}"]
            got += cfg["unk_offset_bases"].get(name, 0)
            if name in skip:
                if got != want:
                    skip_used.add(name)
                continue
            n_unk += 1
            if got != want:
                errors.append(f"{name}.{path}: probed {got:#x}, name says {want:#x}")
    for name in sorted(set(skip) - skip_used):
        errors.append(f"{name} is listed as naming unknowns by sequence "
                      f"({skip[name]}), but every such member sits at the "
                      f"offset its name gives; drop the entry")

    # Members the game annotates with their offset or their address.
    n_comment = 0
    if cfg["comment_offsets"]:
        for _, name, body in structs:
            ann = comment_annotations(body)
            addrs = []
            for member, (kind, value) in ann.items():
                key = f"off_{name}__{sym(member)}"
                if key not in vals:
                    continue
                if kind == "off":
                    n_comment += 1
                    if vals[key] != value:
                        errors.append(f"{name}.{member}: probed "
                                      f"{vals[key]:#x}, comment says {value:#x}")
                else:
                    addrs.append((member, value, vals[key]))
            # An addressed member spacing must match its offset spacing;
            # the struct's own base address is not in the header.
            if len(addrs) > 1:
                base = addrs[0]
                for member, addr, off in addrs[1:]:
                    n_comment += 1
                    if addr - base[1] != off - base[2]:
                        errors.append(
                            f"{name}.{member}: at {off:#x}, {off - base[2]:#x} "
                            f"past {base[0]}, but its address is "
                            f"{addr - base[1]:#x} past {base[1]:#x}")

    # Layout facts read from the game's own asm or source.
    for pin in cfg["pins"]:
        st = pin["struct"]
        if "size" in pin and sizes.get(st) != pin["size"]:
            errors.append(f"sizeof({st}) = {sizes.get(st)}, {pin['why']} "
                          f"says {pin['size']:#x}")
        if "field" in pin:
            got = vals.get(f"off_{st}__{sym(pin['field'])}")
            if got != pin["offset"]:
                errors.append(f"offsetof({st}, {pin['field']}) = {got}, "
                              f"{pin['why']} says {pin['offset']:#x}")

    # .bss globals: count * sizeof + slack in {0, 4}, or a split symbol.
    known_slack = cfg["known_slack"]
    globs = []
    for name, t, count, space in (bss_globals(args.decomp, sizes, header_globs)
                                  if cfg["bss_globals"] else []):
        expect = count * sizes[t]
        slack = space - expect
        if space < expect:
            globs.append((name, t, count, space, "split"))
        elif slack in (0, 4) or known_slack.get(name) == slack:
            globs.append((name, t, count, space, slack))
        else:
            errors.append(f"{name}: .space {space:#x} vs {count} x "
                          f"sizeof({t}) {sizes[t]:#x} (slack {slack})")

    if errors:
        sys.exit("[probe] engagement failed:\n  " + "\n  ".join(errors))

    h = hashlib.sha256()
    for f in G.headers(inc, header_globs):
        h.update(f.relative_to(inc).as_posix().encode())
        h.update(f.read_bytes())
    h.update(P.SYS_TYPES_H.encode())
    h.update(P.psyq_text(stub_set).encode())
    h.update(" ".join(FLAGS).encode())

    out = ["# Auto-generated by tools/probe_struct_layout.py - do not edit.",
           "# MIPS o32 layout of every decomp game struct; regenerate with",
           "# scripts/regen-struct-layout.sh after a decomp header change.",
           "",
           f'headers_sha256 = "{h.hexdigest()}"',
           ""]
    for header, name, body in structs + [("psyq", n, None) for n, _ in psyq]:
        members = (G.walk_members(body) if body is not None
                   else [(p, None) for p in dict(psyq)[name]])
        out.append("[[struct]]")
        out.append(f'name = "{name}"')
        out.append(f'header = "{header}"')
        out.append(f"size = {sizes[name]:#x}")
        out.append("fields = [")
        for pth, width in members:
            if width is None:
                off = vals["off_%s__%s" % (name, sym(pth))]
                out.append(f'  {{ name = "{pth}", offset = {off:#x} }},')
            else:
                bit, w = bits[(name, pth)]
                out.append(f'  {{ name = "{pth}", bit = {bit}, width = {w} }},')
        out.append("]")
        out.append("")
    for name, t, count, space, slack in globs:
        out.append("[[global]]")
        out.append(f'name = "{name}"')
        out.append(f'type = "{t}"')
        out.append(f"count = {count}")
        out.append(f"space = {space:#x}")
        out.append(f'slack = "{slack}"' if slack == "split" else f"slack = {slack}")
        out.append("")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("\n".join(out))
    n_pins = sum(1 + len(G.member_paths(b)) for _, _, b in structs) + \
        sum(1 + len(p) for _, p in psyq)
    print(f"[probe] {len(structs)} structs + {len(psyq)} PSYQ types, "
          f"{n_pins} pins, {len(bits)} bitfields, {n_unk} named-offset "
          f"checks, {n_comment} commented-offset checks, "
          f"{len(cfg['pins'])} asm pins, {len(globs)} .bss globals "
          f"({sum(1 for g in globs if g[4] == 'split')} split) -> {args.out}")


if __name__ == "__main__":
    main()
