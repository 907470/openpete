#!/usr/bin/env python3
"""gen_mod_sites.py — collect enabled mods' [[site]] blocks into the baked
instruction-site set (modding framework PoC rung 14, the site-hook tier).

Reads <mods-dir>/enabled.txt (order = priority, same single truth as code
mods) and each enabled mod's mod.toml [[site]] blocks:

    [[site]]
    name = "sony-fade-in"     # handler key within the mod's namespace
    addr = 0x800128EC         # guest vaddr of ONE anchored instruction
    kind = "replace"          # "replace" (guarded instead-of) | "append"

Outputs (always written, empty set = empty outputs):
  --out-sites     flat lines `mod_replace <addr> <idx> <modid>:<name>` in
                  the --channel-sites grammar (regen-recomp.sh appends
                  this to generated/channel_sites.txt; the recompiler's
                  guarded ModReplace/ModAppend emission does the rest)
  --out-registry  C table mapping (mod_id, name) -> baked idx, compiled
                  into the exe and resolved by api->site_handler()

Baked index assignment is deterministic: enabled.txt order, then
declaration order within each mod.toml. The set's CONTENT is the regen
cache key — scripts/regen-mod-sites.sh only re-runs the scoped recomp
when the emitted sites file differs from the installed one.

Python 3.10-compatible: hand-parses the same restricted TOML subset the
other mod tools use (tomllib arrives in 3.11).
"""
import argparse
import re
import sys
from pathlib import Path

MODSITE_MAX = 256  # keep in sync with OPENPETE_MODSITE_MAX (openpete_mods.h)

NAME_RE = re.compile(r"^[a-z0-9][a-z0-9-]*$")


def strip_comment(line: str) -> str:
    out, in_str = [], False
    for ch in line:
        if ch == '"':
            in_str = not in_str
        elif ch == "#" and not in_str:
            break
        out.append(ch)
    return "".join(out).strip()


def parse_sites(mod_toml: Path):
    """[[site]] dicts in declaration order (restricted-TOML hand parse)."""
    sites, cur = [], None
    section = None
    for lineno, raw in enumerate(mod_toml.read_text(encoding="utf-8")
                                 .splitlines(), 1):
        line = strip_comment(raw)
        if not line:
            continue
        if line.startswith("[["):
            section = line.strip("[]").strip()
            cur = {"_line": lineno} if section == "site" else None
            if cur is not None:
                sites.append(cur)
            continue
        if line.startswith("["):
            section = line.strip("[]").strip()
            cur = None
            continue
        if cur is None or "=" not in line:
            continue
        key, val = [x.strip() for x in line.split("=", 1)]
        cur[key] = val.strip('"')
    return sites


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mods-dir", type=Path, default=Path("mods"))
    ap.add_argument("--out-sites", type=Path, required=True)
    ap.add_argument("--out-registry", type=Path, required=True)
    args = ap.parse_args()

    enabled_txt = args.mods_dir / "enabled.txt"
    enabled = []
    if enabled_txt.is_file():
        enabled = [l.strip() for l in enabled_txt.read_text().splitlines()
                   if l.strip() and not l.strip().startswith("#")]

    collected = []  # (mod_id, name, addr, kind)
    errors = []
    seen_addr = {}
    for mod_id in enabled:
        mod_toml = args.mods_dir / mod_id / "mod.toml"
        if not mod_toml.is_file():
            continue
        seen_names = set()
        for s in parse_sites(mod_toml):
            where = f"{mod_toml}:{s['_line']}"
            name = s.get("name", "")
            kind = s.get("kind", "replace")
            try:
                addr = int(s.get("addr", ""), 0)
            except ValueError:
                errors.append(f"{where}: bad or missing addr")
                continue
            if not NAME_RE.match(name):
                errors.append(f"{where}: bad site name {name!r} "
                              "(kebab-case, [a-z0-9-])")
            if name in seen_names:
                errors.append(f"{where}: duplicate site name {name!r}")
            seen_names.add(name)
            if kind not in ("replace", "append"):
                errors.append(f"{where}: kind must be replace|append")
            if addr & 3 or not (0x80000000 <= addr < 0x80800000):
                errors.append(f"{where}: addr 0x{addr:08X} not a word-"
                              "aligned guest text address")
            if addr in seen_addr:
                errors.append(f"{where}: addr 0x{addr:08X} already claimed "
                              f"by {seen_addr[addr]}")
            seen_addr[addr] = f"{mod_id}:{name}"
            collected.append((mod_id, name, addr, kind))

    if len(collected) > MODSITE_MAX:
        errors.append(f"{len(collected)} sites exceed OPENPETE_MODSITE_MAX "
                      f"({MODSITE_MAX})")
    if errors:
        for e in errors:
            print(f"[mod-sites] ERROR: {e}", file=sys.stderr)
        return 1

    with args.out_sites.open("w") as f:
        f.write("# AUTO-GENERATED by tools/gen_mod_sites.py — baked mod\n"
                "# instruction sites (appended to channel_sites.txt by\n"
                "# regen-recomp.sh). Do not edit; edit mod.toml [[site]]\n"
                "# blocks and re-run scripts/regen-mod-sites.sh.\n")
        for idx, (mod_id, name, addr, kind) in enumerate(collected):
            k = "mod_replace" if kind == "replace" else "mod_append"
            f.write(f"{k} 0x{addr:08X} {idx} {mod_id}:{name}\n")

    with args.out_registry.open("w") as f:
        f.write("/* AUTO-GENERATED by tools/gen_mod_sites.py — baked\n"
                " * instruction-site registry (modding rung 14). One row per\n"
                " * guarded substitution in the recompiled C; the loader\n"
                " * resolves api->site_handler(name) attachments through it.\n"
                " * Regenerate via scripts/regen-mod-sites.sh. */\n"
                "#include \"openpete_mods.h\"\n\n"
                "const openpete_modsite_desc_t openpete_modsite_registry[] "
                "= {\n")
        for idx, (mod_id, name, addr, kind) in enumerate(collected):
            f.write(f'    {{ "{mod_id}", "{name}", 0x{addr:08X}u, {idx} }},\n')
        if not collected:
            f.write("    { 0, 0, 0u, 0 }, /* empty registry placeholder */\n")
        f.write("};\n"
                f"const int openpete_modsite_registry_count = "
                f"{len(collected)};\n")

    print(f"[mod-sites] collected {len(collected)} site(s) from "
          f"{len(enabled)} enabled mod(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
