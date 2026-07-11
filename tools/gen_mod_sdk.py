#!/usr/bin/env python3
"""gen_mod_sdk.py — generate the mod SDK symbol surface from symbols.toml.

P1 of notes/plans/openpete-modding-framework.md ("SDK generation at
recomp time"). v0 scope: the function symbol table only. Emits:

  <out>/openpete_sdk_symbols.h   #define OP_FNADDR_<name> 0x...u
                                 (mod-facing; staged into <exe>/sdk/)
  <out>/sdk_symbols_table.c      sorted name->addr table compiled into
                                 the engine (api->resolve / override_name)

symbols.toml is the decomp-derived manifest (tools/build_manifest.py,
2,414 functions at time of writing): [[function]] blocks with
addr/name/kind. Parsed with a line regex on that uniform shape — no
external TOML dependency for the build.

Names are emitted as-is into the runtime table (any string resolves) and
sanitized/deduped for the #define header (first occurrence wins; invalid
C identifiers are skipped from the header but stay resolvable by name).
"""

import argparse
import re
import sys
from pathlib import Path

FUNC_RE = re.compile(r"^\[\[function\]\]\s*$")
ADDR_RE = re.compile(r"^addr\s*=\s*(0x[0-9A-Fa-f]+)\s*$")
NAME_RE = re.compile(r'^name\s*=\s*"([^"]+)"\s*$')
IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def parse_symbols(path: Path):
    syms = []
    addr = name = None
    in_func = False
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if FUNC_RE.match(line):
            if in_func and addr is not None and name:
                syms.append((name, addr))
            in_func, addr, name = True, None, None
            continue
        if line.startswith("[[") :  # some other table kind
            if in_func and addr is not None and name:
                syms.append((name, addr))
            in_func, addr, name = False, None, None
            continue
        if not in_func:
            continue
        m = ADDR_RE.match(line)
        if m:
            addr = int(m.group(1), 16)
            continue
        m = NAME_RE.match(line)
        if m:
            name = m.group(1)
    if in_func and addr is not None and name:
        syms.append((name, addr))
    return syms


GLOBAL_RE = re.compile(r"^\[\[global\]\]\s*$")


def parse_globals(path: Path):
    """[[global]] blocks share the addr/name line shape of functions."""
    globs = []
    addr = name = None
    in_glob = False
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if GLOBAL_RE.match(line):
            if in_glob and addr is not None and name:
                globs.append((name, addr))
            in_glob, addr, name = True, None, None
            continue
        if line.startswith("[["):
            if in_glob and addr is not None and name:
                globs.append((name, addr))
            in_glob, addr, name = False, None, None
            continue
        if not in_glob:
            continue
        m = ADDR_RE.match(line)
        if m:
            addr = int(m.group(1), 16)
            continue
        m = NAME_RE.match(line)
        if m:
            name = m.group(1)
    if in_glob and addr is not None and name:
        globs.append((name, addr))
    return globs


PROTO_RE = re.compile(r"^\[\[proto\]\]\s*$")
PARAMS_RE = re.compile(r'\{\s*type\s*=\s*"([^"]+)"\s*,\s*name\s*=\s*"([^"]+)"\s*\}')

SCALAR_C = {  # prototype-DB scalar pointer bases → C types
    "char": "char", "u_char": "uint8_t", "unsigned char": "uint8_t",
    "s8": "int8_t", "u8": "uint8_t", "short": "int16_t",
    "u_short": "uint16_t", "s16": "int16_t", "u16": "uint16_t",
    "int": "int32_t", "u_int": "uint32_t", "s32": "int32_t",
    "u32": "uint32_t", "long": "int32_t", "u_long": "uint32_t",
    "void": "void",
}


def parse_protos(path: Path):
    """prototypes.toml → {name: (ret, [(kind, pname)...])}."""
    protos = {}
    cur = None
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if PROTO_RE.match(line):
            cur = {}
            continue
        if cur is None:
            continue
        m = NAME_RE.match(line)
        if m and "name" not in cur:
            cur["name"] = m.group(1)
            continue
        m = re.match(r'^ret\s*=\s*"([^"]+)"\s*$', line)
        if m:
            cur["ret"] = m.group(1)
            continue
        if line.startswith("params"):
            cur["params"] = PARAMS_RE.findall(line)
            continue
        if line.startswith("header") and "name" in cur and "ret" in cur:
            protos[cur["name"]] = (cur["ret"], cur.get("params", []))
            cur = None
    return protos


def c_type(kind):
    """prototype kind → (c_type_str, opaque_struct_or_None)."""
    if kind == "void":
        return "void", None
    if kind == "int":
        return "int32_t", None
    assert kind.startswith("ptr:")
    base = kind[4:]
    if base in SCALAR_C:
        return f"{SCALAR_C[base]}*", None
    return f"{base}*", base


def emit_wrappers(out_path: Path, protos, name_to_addr):
    """Emit openpete_sdk_wrappers.h: per-function natural-signature
    override/register/base/call wrappers over the cpu-context ABI."""
    opaques = set()
    fns = []  # (name, addr, ret, params[(kind, pname)])
    for name in sorted(protos):
        if name not in name_to_addr:
            continue
        ret, params = protos[name]
        for kind, _ in params:
            _, op = c_type(kind)
            if op:
                opaques.add(op)
        fns.append((name, name_to_addr[name], ret, params))

    L = []
    L.append("/* Generated by tools/gen_mod_sdk.py from "
             "manifest/prototypes.toml + symbols.toml — do not edit.\n"
             " *\n"
             " * Natural-signature marshalling wrappers over the "
             "cpu-context ABI.\n"
             " * For a prototyped function Foo, this header provides:\n"
             " *   OP_OVERRIDE_IMPL_Foo(my_fn) { ... }   define a natural-\n"
             " *       signature override body (cpu is also in scope);\n"
             " *   OP_REGISTER_OVERRIDE_Foo(api, self, my_fn)   register it\n"
             " *       (also binds the SDK api slot);\n"
             " *   op_base_Foo(cpu, ...)   fall through the chain with\n"
             " *       (possibly modified) natural args;\n"
             " *   op_call_Foo(cpu, ...)   call the game function (its own\n"
             " *       override chain included); arg regs saved/restored.\n"
             " * Struct types are OPAQUE here (v0): pointers marshal\n"
             " * guest<->host; field access goes through api->guest +\n"
             " * OP_GADDR_* until typed struct headers land.\n"
             " * Only valid on the sim thread inside overrides/entry.\n"
             " */\n"
             "#ifndef OPENPETE_SDK_WRAPPERS_H\n"
             "#define OPENPETE_SDK_WRAPPERS_H\n\n"
             "#include <stdint.h>\n"
             "#include <openpete_mod_api.h>\n"
             "#include <psx_runtime.h> /* full CPUState — wrappers read "
             "registers */\n\n")

    for t in sorted(opaques):
        L.append(f"typedef struct {t} {t};\n")

    L.append("""
/* SDK api slot: bound automatically by every OP_REGISTER_OVERRIDE_*
 * invocation (per translation unit). */
static const openpete_mod_api_t* op__api_slot;
static inline void op_sdk_bind(const openpete_mod_api_t* api) {
    op__api_slot = api;
}
static inline void* op__g(uint32_t va) {
    return va ? op__api_slot->guest(va) : (void*)0;
}
static inline uint32_t op__ga(const void* p) {
    return p ? op__api_slot->guest_addr(p) : 0u;
}
static inline void op__save(CPUState* c, uint32_t* s) {
    s[0]=c->a0; s[1]=c->a1; s[2]=c->a2; s[3]=c->a3;
    s[4]=c->ra; s[5]=c->v0; s[6]=c->v1;
}
static inline void op__restore(CPUState* c, const uint32_t* s) {
    c->a0=s[0]; c->a1=s[1]; c->a2=s[2]; c->a3=s[3];
    c->ra=s[4]; c->v0=s[5]; c->v1=s[6];
}
""")

    areg = ["a0", "a1", "a2", "a3"]
    for name, addr, ret, params in fns:
        rtype, _ = c_type(ret)
        sig_parts = []
        unpack = []   # trampoline: cpu regs -> natural args
        pack = []     # base/call: natural args -> cpu regs
        for i, (kind, pname) in enumerate(params):
            ct, op = c_type(kind)
            sig_parts.append(f"{ct} {pname}")
            if kind == "int":
                unpack.append(f"(int32_t)cpu->{areg[i]}")
                pack.append(f"cpu->{areg[i]} = (uint32_t){pname};")
            else:
                unpack.append(f"({ct})op__g(cpu->{areg[i]})")
                pack.append(f"cpu->{areg[i]} = op__ga({pname});")
        sig = ", ".join(["CPUState* cpu"] + sig_parts)
        args = ", ".join(["cpu"] + unpack)
        pack_s = " ".join(pack)

        L.append(f"/* {rtype} {name}({', '.join(sig_parts) or 'void'})"
                 f"  [0x{addr:08X}] */\n")
        # override impl + trampoline
        if ret == "void":
            tramp_body = f"IMPL({args});"
        else:
            tramp_body = f"cpu->v0 = (uint32_t)IMPL({args});"
        L.append(
            f"#define OP_OVERRIDE_IMPL_{name}(IMPL) \\\n"
            f"    static {rtype} IMPL({sig}); \\\n"
            f"    static void IMPL##__op_tramp(CPUState* cpu) "
            f"{{ {tramp_body} }} \\\n"
            f"    static {rtype} IMPL({sig})\n")
        L.append(
            f"#define OP_REGISTER_OVERRIDE_{name}(api, self, IMPL) \\\n"
            f"    (op_sdk_bind(api), \\\n"
            f"     (api)->override_addr((self), 0x{addr:08X}u, "
            f"IMPL##__op_tramp))\n")
        # base
        # pointer returns are opaque guest vaddrs: cast through intptr_t
        # so Win64 compiles clean (value identical — the sign-extension
        # matches the 64-bit-Linux behaviour)
        ret_grab = "" if ret == "void" else \
            f"    {rtype} rv__ = ({rtype})(intptr_t)(int32_t)cpu->v0;\n"
        ret_stmt = "" if ret == "void" else "    return rv__;\n"
        L.append(
            f"static inline {rtype} op_base_{name}({sig}) {{\n"
            + (f"    {pack_s}\n" if pack_s else "")
            + "    op__api_slot->base(cpu);\n"
            + ret_grab + ret_stmt + "}\n")
        # call (arg regs saved/restored)
        L.append(
            f"static inline {rtype} op_call_{name}({sig}) {{\n"
            "    uint32_t s__[7];\n"
            "    op__save(cpu, s__);\n"
            + (f"    {pack_s}\n" if pack_s else "")
            + f"    op__api_slot->call_addr(cpu, 0x{addr:08X}u);\n"
            + ret_grab
            + "    op__restore(cpu, s__);\n"
            + ret_stmt + "}\n")
        # legacy raw-name aliases (promotion contract: a wrapper keeps
        # existing under every historical name after a rename)
        raw = f"func_{addr:08X}"
        if raw != name:
            L.append(
                f"#define OP_OVERRIDE_IMPL_{raw} OP_OVERRIDE_IMPL_{name}\n"
                f"#define OP_REGISTER_OVERRIDE_{raw} "
                f"OP_REGISTER_OVERRIDE_{name}\n"
                f"#define op_base_{raw} op_base_{name}\n"
                f"#define op_call_{raw} op_call_{name}\n")
        L.append("\n")

    L.append("#endif /* OPENPETE_SDK_WRAPPERS_H */\n")
    out_path.write_text("".join(L), encoding="utf-8")
    return len(fns), len(opaques)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--symbols", required=True, type=Path)
    ap.add_argument("--globals", type=Path, default=None)
    ap.add_argument("--protos", type=Path, default=None)
    ap.add_argument("--out-dir", required=True, type=Path)
    args = ap.parse_args()

    syms = parse_symbols(args.symbols)
    if not syms:
        print(f"gen_mod_sdk: no [[function]] entries in {args.symbols}",
              file=sys.stderr)
        return 1

    # Dedupe by name, first occurrence wins (stable across regens).
    seen = {}
    for name, addr in syms:
        if name not in seen:
            seen[name] = addr
    table = sorted(seen.items(), key=lambda kv: kv[0])  # bytewise strcmp

    args.out_dir.mkdir(parents=True, exist_ok=True)

    globs = []
    if args.globals and args.globals.exists():
        seen_g = {}
        for name, addr in parse_globals(args.globals):
            if name not in seen_g:
                seen_g[name] = addr
        globs = sorted(seen_g.items(), key=lambda kv: kv[0])

    # Decomp promotion contract: every renamed function keeps its
    # historical raw name as a legacy alias — old names resolve forever
    # (with a loader warning), so a match/rename never breaks a mod.
    aliases = []
    for name, addr in table:
        raw = f"func_{addr:08X}"
        if name != raw:
            aliases.append((raw, addr))
    full = sorted([(n, a, 0) for n, a in table]
                  + [(n, a, 1) for n, a in aliases],
                  key=lambda t: t[0])

    hdr = args.out_dir / "openpete_sdk_symbols.h"
    with hdr.open("w", encoding="utf-8") as f:
        f.write("/* Generated by tools/gen_mod_sdk.py from "
                "manifest/symbols.toml + globals.toml — do not edit.\n"
                " * OP_FNADDR_*: guest function vaddrs (api->override_addr;\n"
                " * prefer api->override_name for named symbols — identical\n"
                " * data, and names survive re-derivation).\n"
                " * OP_GADDR_*:  guest data vaddrs (api->guest / typed\n"
                " * accessors in openpete_mod_api.h).\n"
                " * Legacy raw aliases (func_XXXXXXXX of renamed functions)\n"
                " * are emitted too — decomp promotion never breaks a mod.\n"
                " */\n"
                "#ifndef OPENPETE_SDK_SYMBOLS_H\n"
                "#define OPENPETE_SDK_SYMBOLS_H\n\n")
        for name, addr in table:
            if IDENT_RE.match(name):
                f.write(f"#define OP_FNADDR_{name} 0x{addr:08X}u\n")
        f.write("\n/* Legacy raw-name aliases (promotion contract). */\n")
        for name, addr in aliases:
            f.write(f"#define OP_FNADDR_{name} 0x{addr:08X}u\n")
        if globs:
            f.write("\n/* Data globals (manifest/globals.toml). */\n")
            for name, addr in globs:
                if IDENT_RE.match(name):
                    f.write(f"#define OP_GADDR_{name} 0x{addr:08X}u\n")
        f.write("\n#endif /* OPENPETE_SDK_SYMBOLS_H */\n")

    tbl = args.out_dir / "sdk_symbols_table.c"
    with tbl.open("w", encoding="utf-8") as f:
        f.write("/* Generated by tools/gen_mod_sdk.py from "
                "manifest/symbols.toml — do not edit.\n"
                " * Sorted by strcmp(name) for the loader's binary search.\n"
                " * legacy=1 rows are historical raw aliases of renamed\n"
                " * functions (decomp promotion contract). */\n"
                "#include \"openpete_mods.h\"\n\n"
                "const openpete_sdk_symbol_t openpete_sdk_symbols[] = {\n")
        for name, addr, legacy in full:
            f.write(f'    {{ "{name}", 0x{addr:08X}u, {legacy} }},\n')
        f.write("};\n"
                f"const size_t openpete_sdk_symbol_count = {len(full)};\n")

    n_wrapped = 0
    if args.protos and args.protos.exists():
        protos = parse_protos(args.protos)
        name_to_addr = dict(table)
        n_wrapped, n_opaque = emit_wrappers(
            args.out_dir / "openpete_sdk_wrappers.h", protos, name_to_addr)

    print(f"gen_mod_sdk: {len(table)} functions + {len(globs)} globals + "
          f"{n_wrapped} natural wrappers -> {hdr.name}, {tbl.name}, "
          f"openpete_sdk_wrappers.h")
    return 0


if __name__ == "__main__":
    sys.exit(main())
