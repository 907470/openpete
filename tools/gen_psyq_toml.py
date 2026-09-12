#!/usr/bin/env python3
"""Generate manifest/psyq.toml from a GNU ld linker map.

Identifies the PSYQ .text section in the map (a single object file containing
the PSYQ runtime, e.g. build/asm/psyq.o for the Spyro 1 decomp), enumerates
every symbol that lives in that range, and emits a TOML mapping name -> addr.

The resulting manifest is consumed by psxrecomp via --external-symbols=...:
JAL targets that fall in the range are emitted as extern declarations + a
thunk; their MIPS bodies are NOT translated. PsyCross provides the C
implementations.

A decomp that splats the PSYQ runtime without symbols names every function
func_<addr>. Since the recompiler emits psyq_<name> and the thunk layer
overrides by name, such a function reaches no thunk. Pass --names with the
output of tools/gen_psyq_names.py to substitute the recovered names.

Only the symbols listed here are treated as external by the recompiler; a PSYQ
function left out of the manifest has its body translated like any other code.
That is the right treatment for PSYQ's internal helpers, which are ordinary
logic over PSX RAM: stubbing them aborts on a call that would simply have
worked.

It is the wrong treatment for anything that touches the hardware registers at
0x1F80xxxx. PsyCross owns those devices host-side, so openpete does not model
them; translated, such a function polls a status bit that never changes and the
game waits forever. Those must stay external so a thunk answers for them -- or,
until one exists, so the stub says which.

PSYQ's device drivers reach the hardware through the BIOS jump tables with an
indirect call, which no static scan follows and for which the recompiler has no
code at all, so scanning for a register access does not find them. They are
identified by name instead: the libcd, libspu, libpad, libgpu and libetc
internals all carry a family prefix.

--thunked-only keeps a symbol external when the thunk layer implements it,
--hardware-from keeps the ones that name a hardware register directly, and the
device-driver families below are kept external unconditionally.

Usage:
    gen_psyq_toml.py <psx.map> <psyq-object> <out.toml> [--names <names.toml>]
                     [--thunked-only <thunks.cpp> ...]

Example:
    gen_psyq_toml.py games/spyro-1/spyro-1/build/psx.map \\
                     build/asm/psyq.o \\
                     games/spyro-1/manifest/psyq.toml
"""
import re
import sys
from pathlib import Path


def parse(map_path: Path, psyq_obj: str):
    text = map_path.read_text()
    # Header line: " .text          0x000000008005b8e0    0x10300 build/asm/psyq.o"
    header_re = re.compile(
        r"^\s*\.text\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+"
        + re.escape(psyq_obj) + r"\s*$",
        re.MULTILINE,
    )
    m = header_re.search(text)
    if not m:
        sys.exit(f"error: could not find .text section for {psyq_obj} in {map_path}")
    base = int(m.group(1), 16)
    size = int(m.group(2), 16)
    end = base + size

    # Symbols immediately follow the section header. Each is a line of the
    # form `                0x<addr>                <name>`. Stop at the next
    # ld section marker (a line starting with " .", e.g. " .text" or
    # " .data") or LOAD/end.
    sym_re = re.compile(r"^\s{8,}0x([0-9a-fA-F]+)\s+(\S+)\s*$")
    syms = []
    saw_first = False
    # Start scanning at the line after the section header.
    lines = text[m.end():].splitlines()
    for line in lines:
        if not line.strip():
            if saw_first:
                break
            continue
        # Next ld section marker terminates the symbol list.
        if line.lstrip().startswith(".") and "0x" in line:
            break
        sm = sym_re.match(line)
        if not sm:
            # Anything else (e.g. ` build/...(.text)`) — stop.
            break
        addr = int(sm.group(1), 16)
        name = sm.group(2)
        if addr < base or addr >= end:
            break
        saw_first = True
        # Skip local jumptable labels.
        if name.startswith(".L"):
            continue
        # The decomp's `nonmatching` macro emits a marker symbol beside the
        # real one at the same address. It marks "not yet byte-matching"; it is
        # not a function, and emitting it would give two entries per function.
        if name.endswith(".NON_MATCHING"):
            continue
        syms.append((addr, name))

    # Several symbols can still share an address. A generated placeholder next
    # to a real name is the same function under two spellings, so keep the
    # real one; two real names at one address are aliases and the first wins.
    best = {}
    order = []
    for addr, name in syms:
        placeholder = name.startswith(("func_", "D_"))
        if addr not in best:
            best[addr] = name
            order.append(addr)
        elif best[addr].startswith(("func_", "D_")) and not placeholder:
            best[addr] = name
    return base, end, [(a, best[a]) for a in order]


def emit_toml(base: int, end: int, syms, out: Path, source: str):
    lines = [
        f"# Auto-generated by tools/gen_psyq_toml.py from {source}",
        "# Symbols whose JAL targets resolve into this range are stubbed by",
        "# the recompiler (extern decl only; PsyCross supplies the C body).",
        "",
        f"range_start = 0x{base:08x}",
        f"range_end   = 0x{end:08x}",
        "",
    ]
    for addr, name in syms:
        lines.append("[[symbols]]")
        lines.append(f'name = "{name}"')
        lines.append(f"addr = 0x{addr:08x}")
        lines.append("")
    out.write_text("\n".join(lines) + "\n")


# PSYQ's device-driver internals. Translating one runs library code that
# expects a BIOS behind it and a device openpete does not model, and the symptom
# is a poll that never completes rather than a crash.
DEVICE_PREFIXES = ("CD_", "_spu_", "_pad", "_SpuInit", "_GPU_", "startIntr",
                   "_patch_gte", "_dirFail", "Spu", "Pad", "Cd")


def is_device_internal(name: str) -> bool:
    return name.startswith(DEVICE_PREFIXES)


def hardware_touching(psyq_s: Path):
    """-> {label} for every PSYQ function that reaches a 0x1F80xxxx register.

    A hardware access is a `lui` of the I/O segment (0x1F80 uncached, 0x9F80 /
    0xBF80 through the other kseg mappings). The set is closed over calls: a
    function whose callee touches hardware is itself unsafe to translate.
    """
    text = psyq_s.read_text(errors="replace")
    label_re = re.compile(r"^glabel\s+(\S+)", re.M)
    insn_re = re.compile(
        r"^\s*/\* [0-9A-Fa-f]+ [0-9A-Fa-f]{8} [0-9A-Fa-f]{8} \*/\s+(\S+)\s*(.*)$")

    bodies, calls, cur = {}, {}, None
    for line in text.splitlines():
        m = label_re.match(line)
        if m:
            cur = m.group(1)
            bodies[cur] = False
            calls[cur] = set()
            continue
        if cur is None:
            continue
        i = insn_re.match(line)
        if not i:
            continue
        op, rest = i.group(1), i.group(2)
        if op == "lui" and re.search(r"0x(1F80|9F80|BF80)", rest, re.I):
            bodies[cur] = True
        elif op in ("jal", "j"):
            t = rest.strip().split()[0] if rest.strip() else ""
            if t:
                calls[cur].add(t)

    hw = {k for k, v in bodies.items() if v}
    changed = True
    while changed:
        changed = False
        for fn, callees in calls.items():
            if fn not in hw and (callees & hw):
                hw.add(fn)
                changed = True
    return hw


def read_names(path: Path):
    """-> {addr: name} from a gen_psyq_names.py manifest."""
    names, addr = {}, None
    for line in path.read_text().splitlines():
        line = line.strip()
        if line.startswith("addr"):
            addr = int(line.split("=", 1)[1].strip(), 16)
        elif line.startswith("name") and addr is not None:
            names[addr] = line.split("=", 1)[1].strip().strip('"')
            addr = None
    return names


def main():
    argv = sys.argv[1:]
    hw_source = None
    if "--hardware-from" in argv:
        i = argv.index("--hardware-from")
        hw_source = Path(argv[i + 1])
        del argv[i:i + 2]
    thunk_sources = []
    while "--thunked-only" in argv:
        i = argv.index("--thunked-only")
        thunk_sources.append(Path(argv[i + 1]))
        del argv[i:i + 2]
    entry_exe = None
    if "--exe" in argv:
        # The header's entry is the crt0, which the runtime dispatches; it is
        # never a JAL target and must be translated, never stubbed.
        i = argv.index("--exe")
        entry_exe = Path(argv[i + 1])
        del argv[i:i + 2]
    translate = set()
    while "--translate" in argv:
        # A symbol the crt0 reaches before main (InitHeap) that has no host
        # body and must be translated.
        i = argv.index("--translate")
        translate.add(argv[i + 1])
        del argv[i:i + 2]
    names_path = None
    if "--names" in argv:
        i = argv.index("--names")
        names_path = Path(argv[i + 1])
        del argv[i:i + 2]
    if len(argv) != 3:
        sys.exit(__doc__)
    map_path = Path(argv[0])
    psyq_obj = argv[1]
    out_path = Path(argv[2])
    base, end, syms = parse(map_path, psyq_obj)

    renamed = 0
    if names_path is not None:
        recovered = read_names(names_path)
        out = []
        for addr, name in syms:
            new = recovered.get(addr)
            # Only a generated placeholder is replaced: a name the decomp
            # carries is human-recovered and outranks a signature match.
            if new and name.startswith(("func_", "D_")):
                out.append((addr, new))
                renamed += 1
            else:
                out.append((addr, name))
        syms = out

    if entry_exe is not None:
        import struct
        entry = struct.unpack_from("<I", entry_exe.read_bytes(), 0x10)[0]
        syms = [(a, n) for a, n in syms if a != entry]
    if translate:
        syms = [(a, n) for a, n in syms if n not in translate]

    hardware = set()
    if hw_source is not None:
        hardware = hardware_touching(hw_source)

    kept = None
    if thunk_sources:
        implemented = set()
        for src in thunk_sources:
            implemented |= set(re.findall(
                r"\bpsyq_(\w+)\s*\(\s*CPUState\s*\*\s*cpu\s*\)\s*\{",
                src.read_text()))
        if not implemented:
            sys.exit("error: found no psyq_ thunk definitions in %s"
                     % ", ".join(str(p) for p in thunk_sources))
        before = len(syms)
        syms = [(a, n) for a, n in syms
                if n in implemented or n in hardware or is_device_internal(n)]
        kept = (len(syms), before, len(implemented), len(hardware))

    out_path.parent.mkdir(parents=True, exist_ok=True)
    emit_toml(base, end, syms, out_path, source=str(map_path))
    print(
        f"Wrote {len(syms)} PSYQ symbols "
        f"(range 0x{base:08x}..0x{end:08x}) to {out_path}"
        + (f"; {renamed} named from {names_path.name}" if names_path else "")
        + (f"; kept {kept[0]} of {kept[1]} external "
           f"({kept[2]} thunked, {kept[3]} touch hardware), rest translated"
           if kept else "")
    )


if __name__ == "__main__":
    main()
