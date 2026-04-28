#!/usr/bin/env python3
"""Generate the psx_override_dispatch_game switch.

Reads native.toml's [[dispatch]] entries and emits a C file that
implements the override. Replaces the hand-rolled if/else chain in
games/spyro-1/game_extras_impl.c.

Each dispatch entry's `args` list says how to unpack a0..a3 (and stack
slots) into the C call. Supported tokens:

    "u32"            cpu->aN
    "u8"             (uint8_t)(cpu->aN & 0xFF)
    "ptr"            (void*)openpete_psx_to_host_ptr(cpu->aN)
    "stack_u32:OFF"  cpu->read_word(cpu->sp + OFF)

`psyq_main = true` runs psyq___main(cpu) before the body so a global
ctor / crt0 fires (used for main()).

If a [[dispatch]] entry omits `args`, the generator pulls the C
signature from spyro-1/include/ (via parse_headers.py) and derives
the marshalling tokens automatically. Override by listing `args =
[...]` explicitly — needed when the header isn't visible (e.g. main()
is declared inside src/main.c, not in include/) or when the heuristic
type-mapping is wrong.

Usage:
    gen_dispatch.py <manifest_dir> <out_c_path> [<include_dir>]
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _manifest_lib import load_manifest, fail
from parse_headers import load_signatures, Signature


REG_FOR_INDEX = ["a0", "a1", "a2", "a3"]


def _render_extern(sig: Signature, c_name: str) -> str:
    """Render `RetType c_name(params)` from a parsed Signature.

    The matching-decomp tree often uses PSYQ types we don't pull in
    here (u_char, CdlLOC, …). We rewrite a small set to portable
    stand-ins so the forward decl compiles against just <stdint.h>;
    the actual definition compiled inside spyro-1/src/*.c uses the
    real types via -include psyq_compat.h."""
    type_aliases = {
        "u_char": "uint8_t", "u_short": "uint16_t",
        "u_int": "uint32_t", "u_long": "uint32_t",
    }
    def rewrite(t: str) -> str:
        for k, v in type_aliases.items():
            t = t.replace(k, v)
        # Pointer to a non-portable type → void* (signature-compatible
        # at the ABI level — host pointers, all 8 bytes wide).
        if "*" in t and any(c.isalpha() for c in t.replace("*", "").strip()):
            kept = t.split("*")[0].strip()
            if kept and kept not in {"void", "char", "uint8_t", "uint16_t", "uint32_t",
                                      "int", "long", "short", "unsigned", "signed",
                                      "float", "double"}:
                return "void*"
        return t

    ret = rewrite(sig.return_type)
    if not sig.params:
        return f"{ret} {c_name}(void)"
    rendered = ", ".join(rewrite(ty) + (f" {n}" if n else "")
                         for ty, n in sig.params)
    return f"{ret} {c_name}({rendered})"


def render_arg(idx: int, token: str) -> tuple[str, str | None]:
    """Returns (param_decl, marshalling_expr_for_body).

    For simple registers the param is just inline at the call site, so we
    return the call expression and no separate decl. For `ptr` we want a
    typed local so the call site reads cleanly.
    """
    if token == "u32":
        if idx < 4:
            return (f"(uint32_t)cpu->{REG_FOR_INDEX[idx]}", None)
        fail(f"u32 in stack slot — use stack_u32:OFFSET (idx={idx})")
    if token == "u8":
        if idx < 4:
            return (f"(uint8_t)(cpu->{REG_FOR_INDEX[idx]} & 0xFFu)", None)
        fail(f"u8 in stack slot not yet supported (idx={idx})")
    if token == "ptr":
        if idx < 4:
            return (f"openpete_psx_to_host_ptr(cpu->{REG_FOR_INDEX[idx]})", None)
        fail(f"ptr in stack slot not yet supported (idx={idx})")
    if token.startswith("stack_u32:"):
        offset = int(token.split(":", 1)[1])
        return (f"(uint32_t)cpu->read_word(cpu->sp + {offset})", None)
    fail(f"unknown arg token {token!r}")
    return ("", None)  # unreachable


def emit_case(d: dict) -> list[str]:
    addr = d["addr"]
    name = d["name"]
    c_name = d["c_name"]
    psyq_main = d.get("psyq_main", False)
    args = d.get("args", [])

    arg_exprs = [render_arg(i, tok)[0] for i, tok in enumerate(args)]
    call = f"{c_name}({', '.join(arg_exprs)})"

    lines = [
        f"        case 0x{addr:08X}u: {{  /* {name} */",
        "            spyro_native_set_cpu(cpu);",
    ]
    if psyq_main:
        lines.append("            spyro_native_run_psyq_main();")
    lines.append(f"            {call};")
    lines.append("            return 1;")
    lines.append("        }")
    return lines


def main(argv: list[str]) -> int:
    if len(argv) not in (3, 4):
        print("usage: gen_dispatch.py <manifest_dir> <out_c> [<include_dir>]",
              file=sys.stderr)
        return 2
    manifest_dir = Path(argv[1])
    out_path = Path(argv[2])
    include_dir = Path(argv[3]) if len(argv) == 4 else None

    native, sym_idx, _glob = load_manifest(manifest_dir)
    dispatch = native.get("dispatch", [])
    if not dispatch:
        fail("native.toml has no [[dispatch]] entries")

    sigs: dict[str, Signature] = {}
    if include_dir is not None:
        sigs = load_signatures(include_dir)

    # Sanity-check addresses against the parsed symbols manifest, and
    # auto-fill `args` from the parsed header signature if missing.
    by_addr = sym_idx["by_addr"]
    for d in dispatch:
        sym = by_addr.get(d["addr"])
        if sym is None:
            fail(f"[[dispatch]] {d['name']} @ 0x{d['addr']:08X} not in symbols.toml")
        if sym["name"] != d["name"]:
            fail(f"[[dispatch]] addr 0x{d['addr']:08X} maps to {sym['name']!r}, "
                 f"not {d['name']!r}")
        if "args" not in d:
            sig = sigs.get(d["name"])
            if sig is None:
                fail(f"[[dispatch]] {d['name']}: no `args` and no header sig "
                     f"(declare in spyro-1/include/ or set args = [...] in native.toml)")
            d["args"] = sig.args_tokens()

    # Forward decls for each dispatch target, derived from headers
    # where possible. Spyro's headers aren't on this TU's include path
    # (the matching-decomp tree pulls in PSYQ/libgte/etc which collide
    # with PsyCross), so we rebuild a minimal extern signature here.
    fwd_lines = [
        "/* Glue from games/spyro-1/decomp/native_thunks.c. */",
        "extern void spyro_native_set_cpu(CPUState* cpu);",
        "extern void spyro_native_run_psyq_main(void);",
        "",
        "/* Forward decls — auto-derived from spyro-1/include/, except where",
        " * the manifest carries an explicit `args = [...]` override. */",
    ]
    for d in dispatch:
        sig = sigs.get(d["name"])
        if sig is not None:
            fwd_lines.append(f"extern {_render_extern(sig, d['c_name'])};")
        else:
            # No header: emit a void-no-arg fallback. Works for main()
            # and other manifest-only entries.
            fwd_lines.append(f"extern void {d['c_name']}(void);")

    header = (
        "/* Auto-generated by tools/gen_dispatch.py — do not edit. */\n"
        "/* Implements psx_override_dispatch_game's native-entry switch. */\n"
        "\n"
        "#include \"openpete_psx.h\"\n"
        "\n"
        "#include <stdint.h>\n"
        "\n"
        + "\n".join(fwd_lines) + "\n"
        "\n"
        "int spyro_native_dispatch(CPUState* cpu, uint32_t addr) {\n"
        "    switch (addr) {\n"
    )

    body: list[str] = []
    for d in dispatch:
        body.extend(emit_case(d))

    footer = (
        "        default:\n"
        "            return 0;\n"
        "    }\n"
        "}\n"
    )

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(header + "\n".join(body) + "\n" + footer)
    print(f"emitted {len(dispatch)} dispatch cases → {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
