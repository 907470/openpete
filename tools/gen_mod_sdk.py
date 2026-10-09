#!/usr/bin/env python3
"""gen_mod_sdk.py - generate one game's mod SDK symbol surface.

Emits, from the game's manifests:

  <out>/openpete_sdk_symbols.h   #define OP_FNADDR_<name> 0x...u, and
                                 #define OP_OVLID_<name> <id>u for every
                                 name whose rows lie in one overlay
  <out>/openpete_sdk_wrappers.h  natural-signature wrappers per prototype
  <out>/openpete_game.h          OPENPETE_GAME_ID and OPENPETE_GAME_SPYRO{1,2,3}
  <out>/sdk_symbols_table.c      the (name, overlay)-sorted runtime table
                                 compiled into the engine (api->resolve /
                                 override_name)

symbols.toml is the decomp-derived manifest (tools/build_manifest.py):
[[function]] blocks with addr/name/kind/source_file, parsed with a line
regex on that uniform shape. A row's overlay is the directory under
`src/overlays/` or `asm/overlays/` its source_file names, else the file
stem of a file directly under one of them; the overlay id is that name's
id in the game's generated spyro_overlay_table.c (0 = the EXE).

Rows emitted into the runtime table and the header:
  - every manifest row (a duplicate manifest name is refused);
  - `func_<ADDR>` for every row not so named, legacy = 1 (the decomp
    promotion contract: a renamed function keeps its raw name);
  - `func_<overlay>_<ADDR>` for every overlay row not so named,
    legacy = 0, so every overlay function has a name carrying its address
    and overlay;
an alias whose name equals a manifest row is not emitted (the manifest
row wins). The table holds one row per (name, overlay); the header emits
each `OP_FNADDR_` name once. Names that are not C identifiers stay in the
table and are left out of the header.
"""

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import psyq_types as P  # noqa: E402

FUNC_RE = re.compile(r"^\[\[function\]\]\s*$")
ADDR_RE = re.compile(r"^addr\s*=\s*(0x[0-9A-Fa-f]+)\s*$")
NAME_RE = re.compile(r'^name\s*=\s*"([^"]+)"\s*$')
SRC_RE = re.compile(r'^source_file\s*=\s*"([^"]+)"\s*$')
IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
OVL_DIR_RE = re.compile(r"^(?:src|asm)/overlays/([^/]+)/")
OVL_FILE_RE = re.compile(r"^(?:src|asm)/overlays/([^/]+)\.\w+$")
OVL_TABLE_RE = re.compile(r'\{\s*(\d+)\s*,\s*"([^"]+)"')


def parse_symbols(path: Path):
    """symbols.toml -> [(name, addr, source_file or "")] in file order."""
    syms = []
    cur = None
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line.startswith("[["):
            if cur and cur.get("addr") is not None and cur.get("name"):
                syms.append((cur["name"], cur["addr"], cur.get("src", "")))
            cur = {} if FUNC_RE.match(line) else None
            continue
        if cur is None:
            continue
        m = ADDR_RE.match(line)
        if m:
            cur["addr"] = int(m.group(1), 16)
            continue
        m = NAME_RE.match(line)
        if m:
            cur["name"] = m.group(1)
            continue
        m = SRC_RE.match(line)
        if m:
            cur["src"] = m.group(1)
    if cur and cur.get("addr") is not None and cur.get("name"):
        syms.append((cur["name"], cur["addr"], cur.get("src", "")))
    return syms


def overlay_name(source_file: str):
    """The overlay a row's source_file names, or None for an EXE row."""
    m = OVL_DIR_RE.match(source_file) or OVL_FILE_RE.match(source_file)
    return m.group(1) if m else None


def parse_overlay_table(path: Path):
    """spyro_overlay_table.c -> {overlay name: overlay id}."""
    return {name: int(i) for i, name in
            OVL_TABLE_RE.findall(path.read_text(encoding="utf-8"))}


def raw_alias(addr: int) -> str:
    return f"func_{addr:08X}"


def ovl_alias(ovl: str, addr: int) -> str:
    return f"func_{ovl}_{addr:08X}"


def build_rows(syms, ovl_ids):
    """Manifest rows plus the generated aliases, as
    (name, addr, legacy, overlay id, overlay name) sorted by name then
    overlay id. Refuses a duplicate manifest name and an overlay absent
    from the overlay table."""
    manifest, seen, dups, missing = [], set(), set(), set()
    for name, addr, src in syms:
        ovl = overlay_name(src)
        if ovl is not None and ovl not in ovl_ids:
            missing.add(ovl)
            continue
        if name in seen:
            dups.add(name)
        seen.add(name)
        manifest.append((name, addr, 0, ovl_ids[ovl] if ovl else 0, ovl))
    if dups or missing:
        sys.exit(f"gen_mod_sdk: duplicate manifest names {sorted(dups)[:10]}; "
                 f"overlays absent from the overlay table {sorted(missing)}")
    rows = {(n, o): (n, a, l, o, on) for n, a, l, o, on in manifest}
    for name, addr, _, oid, ovl in manifest:
        for alias, legacy in ((raw_alias(addr), 1),
                              (ovl_alias(ovl, addr) if ovl else None, 0)):
            if alias and alias != name and alias not in seen:
                rows.setdefault((alias, oid), (alias, addr, legacy, oid, ovl))
    return manifest, sorted(rows.values(), key=lambda r: (r[0], r[3]))


def name_overlays(rows):
    """{name: set of overlay ids its rows lie in}."""
    out = {}
    for name, _, _, oid, _ in rows:
        out.setdefault(name, set()).add(oid)
    return out


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

SCALAR_C = {  # prototype-DB scalar pointer bases -> C types
    "char": "char", "u_char": "uint8_t", "unsigned char": "uint8_t",
    "s8": "int8_t", "u8": "uint8_t", "short": "int16_t",
    "u_short": "uint16_t", "s16": "int16_t", "u16": "uint16_t",
    "int": "int32_t", "u_int": "uint32_t", "s32": "int32_t",
    "u32": "uint32_t", "long": "int32_t", "u_long": "uint32_t",
    "void": "void",
}


def parse_protos(path: Path):
    """prototypes.toml -> {name: (ret, [(kind, pname)...])}."""
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
    """prototype kind -> (c_type_str, opaque_struct_or_None)."""
    if kind == "void":
        return "void", None
    if kind == "int":
        return "int32_t", None
    assert kind.startswith("ptr:")
    base = kind[4:]
    if base in SCALAR_C:
        return f"{SCALAR_C[base]}*", None
    # A PSYQ type is spelled with the PSX_ prefix the game-structs header
    # gives it, so the forward declaration below completes that definition
    # and neither name collides with <windows.h>'s RECT.
    base = P.psx_prefixed(base)
    return f"{base}*", base


def wrapper_aliases(fns, manifest_names, overlays_of):
    """{primary name: [alias, ...]}: the `func_<ADDR>` and
    `func_<overlay>_<ADDR>` spellings each prototyped function's four
    wrapper families are forwarded under. Left out: an alias equal to a
    manifest row, a raw alias whose rows lie in more than one overlay
    (ambiguous), and an alias two prototyped functions would both claim."""
    claims = {}
    for name, addr, _, _, ovl in fns:
        cands = [raw_alias(addr)]
        if ovl:
            cands.append(ovl_alias(ovl, addr))
        for a in cands:
            if a == name or a in manifest_names:
                continue
            if a == raw_alias(addr) and len(overlays_of.get(a, ())) > 1:
                continue
            claims.setdefault(a, []).append(name)
    out = {}
    for a in sorted(claims):
        if len(claims[a]) == 1:
            out.setdefault(claims[a][0], []).append(a)
    return out


def emit_wrappers(out_path: Path, protos, manifest, rows, game_guard):
    """Emit openpete_sdk_wrappers.h: per-function natural-signature
    override/register/base/call wrappers over the cpu-context ABI, plus
    object-like forwards of all four families under each alias spelling
    (wrapper_aliases). Bindings use the row's address."""
    by_name = {n: (a, oid, ovl) for n, a, _, oid, ovl in manifest}
    opaques = set()
    fns = []  # (name, addr, ret, params[(kind, pname)], overlay name)
    for name in sorted(protos):
        if name not in by_name:
            continue
        ret, params = protos[name]
        for kind, _ in params:
            _, op = c_type(kind)
            if op:
                opaques.add(op)
        addr, _, ovl = by_name[name]
        fns.append((name, addr, ret, params, ovl))
    aliases = wrapper_aliases(fns, set(by_name), name_overlays(rows))

    L = []
    L.append("/**\n"
             " * @file openpete_sdk_wrappers.h\n"
             " * @brief Natural-signature wrappers over the CPUState override ABI.\n"
             " *\n"
             " * Generated by the engine build; do not edit.\n"
             " *\n"
             " * For each prototyped game function Foo:\n"
             " * - `OP_OVERRIDE_IMPL_Foo(my_fn) { ... }` defines an override body\n"
             " *   with natural arguments; `cpu` is also in scope.\n"
             " * - `OP_REGISTER_OVERRIDE_Foo(api, self, my_fn)` registers it and\n"
             " *   binds the api slot below.\n"
             " * - `op_base_Foo(cpu, ...)` runs the next override or the original\n"
             " *   with the given arguments.\n"
             " * - `op_call_Foo(cpu, ...)` calls the game function through its own\n"
             " *   override chain; argument registers are saved and restored.\n"
             " *\n"
             " * Struct pointers marshal between guest vaddrs and host pointers;\n"
             " * include openpete_game_structs.h for field access. Valid only on\n"
             " * the sim thread, inside entry or an override.\n"
             " */\n"
             "#ifndef OPENPETE_SDK_WRAPPERS_H\n"
             "#define OPENPETE_SDK_WRAPPERS_H\n\n"
             + game_guard("openpete_sdk_wrappers.h") +
             "#include <stdint.h>\n"
             "#include <openpete_mod_api.h>\n"
             "#include <psx_runtime.h> /* CPUState */\n\n")

    for t in sorted(opaques):
        L.append(f"typedef struct {t} {t};\n")

    L.append("""
/** @brief The api pointer used by the wrappers, bound by every `OP_REGISTER_OVERRIDE_*` invocation (one slot per translation unit). */
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
    for name, addr, ret, params, ovl in fns:
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

        where = f"0x{addr:08X} in overlay `{ovl}`" if ovl else f"0x{addr:08X}"
        ovl_note = ("\n * `op_call_` and `op_base_` reach the address in whichever "
                    "overlay is\n * resident: while another overlay is loaded "
                    "they run that overlay's\n * function at the address."
                    if ovl else "")
        L.append(f"/** @name {name}\n"
                 f" * `{rtype} {name}({', '.join(sig_parts) or 'void'})` at {where}.{ovl_note}\n"
                 f" * @{{ */\n")
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
        # so Win64 compiles clean (value identical - the sign-extension
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
            + f"    op__api_slot->call(cpu, 0x{addr:08X}u);\n"
            + ret_grab
            + "    op__restore(cpu, s__);\n"
            + ret_stmt + "}\n")
        # Alias spellings (promotion contract: a wrapper keeps existing
        # under every historical name after a rename; an overlay function
        # is also reachable as func_<overlay>_<ADDR>).
        for alias in aliases.get(name, []):
            L.append(
                f"#define OP_OVERRIDE_IMPL_{alias} OP_OVERRIDE_IMPL_{name}\n"
                f"#define OP_REGISTER_OVERRIDE_{alias} "
                f"OP_REGISTER_OVERRIDE_{name}\n"
                f"#define op_base_{alias} op_base_{name}\n"
                f"#define op_call_{alias} op_call_{name}\n")
        L.append("/** @} */\n\n")

    L.append("#endif /* OPENPETE_SDK_WRAPPERS_H */\n")
    out_path.write_text("".join(L), encoding="utf-8")
    return len(fns), sum(len(v) for v in aliases.values())


GAMES = ("spyro-1", "spyro-2", "spyro-3")


def game_guard(header: str) -> str:
    """The refusal every per-game header opens with: an object built with
    OPENPETE_MOD_ANY_GAME claims every game, so it reads no per-game
    header."""
    return ("#ifdef OPENPETE_MOD_ANY_GAME\n"
            f"#error \"{header} is per-game; an OPENPETE_MOD_ANY_GAME object "
            "reads no per-game header\"\n"
            "#endif\n\n")


def emit_game_header(out_path: Path, game: str):
    """openpete_game.h: the compile-time identity of the SDK's game."""
    L = ["/**\n"
         " * @file openpete_game.h\n"
         " * @brief The game this per-game SDK directory serves.\n"
         " *\n"
         " * Generated by the engine build; do not edit.\n"
         " *\n"
         " * openpete_mod_api.h includes it when it is on the include path, and\n"
         " * #OPENPETE_MOD_ABI_DEFINE then records #OPENPETE_GAME_ID in the\n"
         " * object. Each of `OPENPETE_GAME_SPYRO1`, `_SPYRO2` and `_SPYRO3` is\n"
         " * defined, the running SDK's game as 1 and the others as 0, so a\n"
         " * cross-game mod branches with `#if` and closes the chain with\n"
         " * `#else #error`.\n"
         " */\n"
         "#ifndef OPENPETE_GAME_H\n"
         "#define OPENPETE_GAME_H\n\n",
         game_guard("openpete_game.h"),
         f"/** @brief The game id, as `[mod] game` spells it. */\n"
         f"#define OPENPETE_GAME_ID \"{game}\"\n"]
    for g in GAMES:
        macro = "OPENPETE_GAME_" + g.replace("-", "").upper()
        L.append(f"/** @brief 1 when this SDK serves {g}, else 0. */\n"
                 f"#define {macro} {1 if g == game else 0}\n")
    L.append("\n#endif /* OPENPETE_GAME_H */\n")
    out_path.write_text("".join(L), encoding="utf-8")


def emit_symbols_header(path: Path, manifest, rows, globs):
    """openpete_sdk_symbols.h: one `OP_FNADDR_` per name, `OP_OVLID_`
    beside each name whose rows all lie in one overlay."""
    overlays_of = name_overlays(rows)
    manifest_names = {r[0] for r in manifest}
    addr_of = {r[0]: r[1] for r in rows}
    aliases = sorted({r[0] for r in rows if r[0] not in manifest_names})

    def emit(f, name):
        f.write(f"#define OP_FNADDR_{name} 0x{addr_of[name]:08X}u\n")
        oids = overlays_of[name]
        if len(oids) == 1 and next(iter(oids)) != 0:
            f.write(f"#define OP_OVLID_{name} {next(iter(oids))}u\n")

    with path.open("w", encoding="utf-8") as f:
        f.write("/**\n"
                " * @file openpete_sdk_symbols.h\n"
                " * @brief Guest function and data addresses.\n"
                " *\n"
                " * Generated by the engine build; do not edit.\n"
                " *\n"
                " * - `OP_FNADDR_*`: guest function vaddrs, for `api->override_addr`.\n"
                " *   Prefer `api->override_name` with the same name: names survive\n"
                " *   re-derivation, addresses may not.\n"
                " * - `OP_OVLID_*`: the overlay id of a function that lies in one\n"
                " *   overlay (0 is the EXE, which carries none). Every overlay\n"
                " *   function also has the name `func_<overlay>_<ADDR>`.\n"
                " * - `OP_GADDR_*`: guest data vaddrs, for `api->guest` and the typed\n"
                " *   accessors in openpete_mod_api.h.\n"
                " *\n"
                " * A renamed function keeps its old func_XXXXXXXX name as an alias,\n"
                " * so a rename never breaks a mod.\n"
                " */\n"
                "#ifndef OPENPETE_SDK_SYMBOLS_H\n"
                "#define OPENPETE_SDK_SYMBOLS_H\n\n"
                + game_guard("openpete_sdk_symbols.h"))
        for name, *_ in manifest:
            if IDENT_RE.match(name):
                emit(f, name)
        f.write("\n/** @name Aliases of renamed and overlay functions\n * @{ */\n")
        for name in aliases:
            emit(f, name)
        f.write("/** @} */\n")
        if globs:
            f.write("\n/** @name Data globals\n * @{ */\n")
            for name, addr in globs:
                if IDENT_RE.match(name):
                    f.write(f"#define OP_GADDR_{name} 0x{addr:08X}u\n")
            f.write("/** @} */\n")
        f.write("\n#endif /* OPENPETE_SDK_SYMBOLS_H */\n")


def emit_table(path: Path, rows):
    """sdk_symbols_table.c: every row, sorted by name then overlay id."""
    with path.open("w", encoding="utf-8") as f:
        f.write("/* Generated by tools/gen_mod_sdk.py from "
                "manifest/symbols.toml - do not edit.\n"
                " * Sorted by strcmp(name), then overlay id, for the loader's\n"
                " * binary search. legacy=1 rows are historical raw aliases of\n"
                " * renamed functions (decomp promotion contract); overlay 0 is\n"
                " * the EXE. */\n"
                "#include \"openpete_mods.h\"\n\n"
                "const openpete_sdk_symbol_t openpete_sdk_symbols[] = {\n")
        for name, addr, legacy, oid, _ in rows:
            f.write(f'    {{ "{name}", 0x{addr:08X}u, {legacy}, {oid}u }},\n')
        f.write("};\n"
                f"const size_t openpete_sdk_symbol_count = {len(rows)};\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", required=True, choices=GAMES)
    ap.add_argument("--symbols", required=True, type=Path)
    ap.add_argument("--overlay-table", required=True, type=Path,
                    help="the game's generated spyro_overlay_table.c")
    ap.add_argument("--globals", type=Path, default=None)
    ap.add_argument("--protos", type=Path, default=None)
    ap.add_argument("--out-dir", required=True, type=Path)
    args = ap.parse_args()

    syms = parse_symbols(args.symbols)
    if not syms:
        print(f"gen_mod_sdk: no [[function]] entries in {args.symbols}",
              file=sys.stderr)
        return 1
    manifest, rows = build_rows(syms, parse_overlay_table(args.overlay_table))
    manifest.sort(key=lambda r: r[0])  # bytewise strcmp

    globs = []
    if args.globals and args.globals.exists():
        seen_g = {}
        for name, addr in parse_globals(args.globals):
            if name not in seen_g:
                seen_g[name] = addr
        globs = sorted(seen_g.items(), key=lambda kv: kv[0])

    args.out_dir.mkdir(parents=True, exist_ok=True)
    emit_symbols_header(args.out_dir / "openpete_sdk_symbols.h", manifest,
                        rows, globs)
    emit_table(args.out_dir / "sdk_symbols_table.c", rows)
    emit_game_header(args.out_dir / "openpete_game.h", args.game)
    protos = parse_protos(args.protos) \
        if args.protos and args.protos.exists() else {}
    n_wrapped, n_fwd = emit_wrappers(
        args.out_dir / "openpete_sdk_wrappers.h", protos, manifest, rows,
        game_guard)

    n_ovl = sum(1 for r in rows if r[2] == 0 and r[4] and
                r[0] == ovl_alias(r[4], r[1]))
    print(f"gen_mod_sdk: {args.game}: {len(manifest)} functions, "
          f"{len(rows)} table rows ({sum(r[2] for r in rows)} legacy, "
          f"{n_ovl} func_<overlay>_<addr> names), {len(globs)} globals, "
          f"{n_wrapped} natural wrappers, {n_fwd} wrapper aliases")
    return 0


if __name__ == "__main__":
    sys.exit(main())
