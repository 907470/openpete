#!/usr/bin/env python3
"""probe_struct_layout.py — MIPS-ABI layout of every decomp game struct.

Compiles one probe TU that includes every decomp include/*.h under
mipsel-linux-gnu-gcc with the decomp's own flags, using stub PSYQ headers
generated from tools/psyq_types.py, and reads sizeof / offsetof of every
struct and member path back out of the assembler output. Writes
games/spyro-1/manifest/struct_layout.toml, which gen_game_structs.py
turns into _Static_assert pins in the SDK header, so the host compile of
a mod proves the transformed layout equals the guest layout.

Only this step needs the MIPS compiler; the toml is committed and every
other build reads it. The toml carries a hash of include/*.h plus the
stubs and flags; the generator refuses a checkout whose hash differs.

Engagement checks (refused on failure, so a wrong stub cannot pass):
  - every `unk_0xNNN` member's offset equals NNN (SpyroPhysics members
    are named relative to Spyro.m_Physics at 0xC8);
  - sizeof(DB) == 0x84 and offsetof(DRAWENV, r0) == 0x19, the g_DB store
    offsets in asm/nonmatchings/overlays/credits/func_credits_8007BFD0.s;
  - every struct-typed `extern` global with a `.space` in asm/data/*.s
    occupies count * sizeof plus 0 or 4 bytes of alignment slack, or is a
    split symbol smaller than its struct (recorded as such).

Usage: probe_struct_layout.py --decomp <spyro-1 checkout> --out <toml>
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
import psyq_types as P                # noqa: E402

CC = "mipsel-linux-gnu-gcc"
FLAGS = ["-S", "-march=mips1", "-mabi=32", "-msoft-float", "-ffreestanding"]
# A zero-valued global is emitted as `.space 4`, not `.word 0`, whatever the
# bss flags; every probe value is stored +1 so it always has a `.word`.
BIAS = 1

SPYRO_PHYSICS_BASE = 0xC8   # Spyro.m_Physics; its unk_ names are Spyro-relative

# .bss reservations larger than the header's struct by more than alignment,
# each with the reason; anything else outside {0, 4} refuses the run.
KNOWN_SLACK = {
    # PadBuffer is 0x20 in gamepad.h (u_char padding[24]); the .bss reserves
    # 0x28, annotated "0x24 + 0x4 padding": PSYQ's pad buffer is 34 bytes,
    # so the header's padding is 4 short. Reported upstream; the game only
    # touches the first 8 bytes.
    "g_PadBuffer": 8,
    "g_PadBufferSecondController": 8,
}


def write_stubs(d: Path):
    (d / "sys").mkdir(parents=True, exist_ok=True)
    (d / "sys" / "types.h").write_text(P.SYS_TYPES_H)
    for name, text in P.PSYQ_TYPES:
        guard = name.upper().replace(".", "_")
        (d / name).write_text(f"#ifndef {guard}\n#define {guard}\n"
                              f"#include <sys/types.h>\n{text}#endif\n")
    for name in P.EMPTY_STUBS:
        (d / name).write_text("")


def sym(path: str) -> str:
    """Member path -> identifier fragment (`a[0].b` -> `a_0__b`)."""
    return re.sub(r"[^\w]+", "_", path.replace(".", "__")).strip("_")


def probe_tu(include_dir: Path, structs, extra_names):
    lines = [f'#include "{h.name}"' for h in sorted(include_dir.glob("*.h"))]
    for _, name, body in structs:
        lines.append(f"unsigned long sz_{name} = sizeof({name}) + {BIAS};")
        for path in G.member_paths(body):
            lines.append(f"unsigned long off_{name}__{sym(path)} "
                         f"= __builtin_offsetof({name}, {path}) + {BIAS};")
    for name, paths in extra_names:
        lines.append(f"unsigned long sz_{name} = sizeof({name}) + {BIAS};")
        for path in paths:
            lines.append(f"unsigned long off_{name}__{sym(path)} "
                         f"= __builtin_offsetof({name}, {path}) + {BIAS};")
    return "\n".join(lines) + "\n"


def read_asm(path: Path):
    vals, cur = {}, None
    for l in path.read_text().splitlines():
        m = re.match(r"^(sz_\w+|off_\w+):", l)
        if m:
            cur = m.group(1)
            continue
        m = re.match(r"\s*\.(?:word|long|4byte)\s+(-?\d+)", l)
        if m and cur:
            vals[cur] = int(m.group(1)) - BIAS
            cur = None
    return vals


def bss_globals(decomp: Path, sizes: dict):
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
    for h in sorted((decomp / "include").glob("*.h")):
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
    ap.add_argument("--decomp", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    inc = args.decomp / "include"

    structs = G.enumerate_structs(inc)
    psyq = [(n, G.member_paths(re.search(
        rf"typedef struct {n} \{{(.*?)\}} {n};", P.psyq_text(), re.S).group(1)))
            for n in P.psyq_names()]

    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        stubs = td / "stubs"
        write_stubs(stubs)
        (td / "probe.c").write_text(probe_tu(inc, structs, psyq))
        r = subprocess.run([CC, *FLAGS, f"-I{inc}", f"-I{stubs}",
                            "-o", str(td / "probe.s"), str(td / "probe.c")],
                           capture_output=True, text=True)
        if r.returncode:
            sys.exit(f"[probe] {CC} failed:\n{r.stderr}")
        vals = read_asm(td / "probe.s")

    errors = []
    sizes = {n: vals[f"sz_{n}"] for _, n, _ in structs}
    sizes.update({n: vals[f"sz_{n}"] for n, _ in psyq})

    # unk_0xNNN names encode offsets.
    n_unk = 0
    for _, name, body in structs:
        for path in G.member_paths(body):
            m = re.fullmatch(r"(?:.*\.)?unk_0x([0-9a-fA-F]+)", path)
            if not m:
                continue
            n_unk += 1
            want = int(m.group(1), 16)
            got = vals[f"off_{name}__{sym(path)}"]
            if "." in path:
                # a member of a nested block is named relative to the block
                parent = re.sub(r"\[0\]", "", path.rsplit(".", 1)[0])
                got -= vals[f"off_{name}__{sym(parent)}"]
            if name == "SpyroPhysics":
                got += SPYRO_PHYSICS_BASE
            if got != want:
                errors.append(f"{name}.{path}: probed {got:#x}, name says {want:#x}")

    # g_DB store offsets.
    if sizes["DB"] != 0x84:
        errors.append(f"sizeof(DB) = {sizes['DB']:#x}, asm stores say 0x84")
    if vals["off_DRAWENV__r0"] != P.DRAWENV_R0_OFFSET:
        errors.append(f"offsetof(DRAWENV, r0) = {vals['off_DRAWENV__r0']:#x}, "
                      f"asm stores say {P.DRAWENV_R0_OFFSET:#x}")
    if sizes["DRAWENV"] != P.DRAWENV_SIZE or sizes["DISPENV"] != P.DISPENV_SIZE:
        errors.append("DRAWENV/DISPENV sizes differ from psyq_game_hooks.h")

    # .bss globals: count * sizeof + slack in {0, 4}, or a split symbol.
    globs = []
    for name, t, count, space in bss_globals(args.decomp, sizes):
        expect = count * sizes[t]
        slack = space - expect
        if space < expect:
            globs.append((name, t, count, space, "split"))
        elif slack in (0, 4) or KNOWN_SLACK.get(name) == slack:
            globs.append((name, t, count, space, slack))
        else:
            errors.append(f"{name}: .space {space:#x} vs {count} x "
                          f"sizeof({t}) {sizes[t]:#x} (slack {slack})")

    if errors:
        sys.exit("[probe] engagement failed:\n  " + "\n  ".join(errors))

    h = hashlib.sha256()
    for f in sorted(inc.glob("*.h")):
        h.update(f.name.encode())
        h.update(f.read_bytes())
    h.update(P.SYS_TYPES_H.encode())
    h.update(P.psyq_text().encode())
    h.update(" ".join(FLAGS).encode())

    out = ["# Auto-generated by tools/probe_struct_layout.py — do not edit.",
           "# MIPS o32 layout of every decomp game struct; regenerate with",
           "# scripts/regen-struct-layout.sh after a decomp header change.",
           "",
           f'headers_sha256 = "{h.hexdigest()}"',
           ""]
    for header, name, body in structs + [("psyq", n, None) for n, _ in psyq]:
        paths = (G.member_paths(body) if body is not None
                 else dict(psyq)[name])
        out.append("[[struct]]")
        out.append(f'name = "{name}"')
        out.append(f'header = "{header}"')
        out.append(f"size = {sizes[name]:#x}")
        out.append("fields = [")
        for pth in paths:
            off = vals["off_%s__%s" % (name, sym(pth))]
            out.append(f'  {{ name = "{pth}", offset = {off:#x} }},')
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
          f"{n_pins} pins, {n_unk} unk_0x checks, {len(globs)} .bss globals "
          f"({sum(1 for g in globs if g[4] == 'split')} split) -> {args.out}")


if __name__ == "__main__":
    main()
