#!/usr/bin/env python3
"""check_module_imports.py - refuse a shared recomp module whose imports do
not each resolve to exactly one definition in the merged image.

The merged image of a shared-linkage game is the executable, the module it
dlopens (RTLD_NOW | RTLD_LOCAL), and the shared libraries both depend on.
The module links with --unresolved-symbols=ignore-all, so the linker accepts
any import; this check is the build-time refusal that replaces the
static-link error. It reads the dynamic symbol tables (ELF) and fails when:

  * an import of the module (an undefined, non-weak dynamic symbol) has no
    definition in the executable or in any library the module or the
    executable needs (the dlopen would fail, or a lazily bound call would
    abort at first use);
  * an import has a definition in more than one of those objects (which one
    binds depends on load order; an executable definition preempting a
    library's is the hazard this rules out);
  * a symbol the module defines is also defined by the executable, outside
    the declared host->module forwarders (--forwarder, --forwarder-header)
    and the linker's reserved names. The module binds its own functions internally
    (-Bsymbolic-functions), but its data references would bind to the
    executable's copy.

PE needs no such check: the module links against an import library naming
the executable, so an import missing from the executable's .def export
table fails the module link, and every import names one image.

Exit 0 clean, 1 refused (each violation on stderr), 2 usage or tool error.
"""
import argparse
import os
import re
import subprocess
import sys

# Linker-defined names every ELF image carries.
RESERVED = {"_init", "_fini", "_edata", "_end", "__bss_start",
            "_DYNAMIC", "_GLOBAL_OFFSET_TABLE_", "__dso_handle"}


def dynsyms(path):
    """Return (defined, undefined_strong) dynamic symbol names of an ELF."""
    out = subprocess.run(["nm", "-D", path], check=True,
                         capture_output=True, text=True).stdout
    defined, undefined = set(), set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            _, kind, name = parts
        elif len(parts) == 2:
            kind, name = parts
        else:
            continue
        name = name.split("@", 1)[0]
        if kind == "U":
            undefined.add(name)
        elif kind in ("w", "v"):
            continue
        else:
            defined.add(name)
    return defined, undefined


def needed_libs(path):
    """Resolve the shared libraries an ELF needs, transitively, via ldd."""
    out = subprocess.run(["ldd", path], capture_output=True, text=True).stdout
    libs = []
    for line in out.splitlines():
        m = re.search(r"=>\s+(\S+)\s+\(", line)
        if m and os.path.exists(m.group(1)):
            libs.append(m.group(1))
    return libs


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--module", required=True)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--forwarder", action="append", default=[],
                    help="a host->module forwarder the executable defines")
    ap.add_argument("--forwarder-header", action="append", default=[],
                    help="a module_imports.h whose X(name) entries are "
                         "forwarders (repeatable: the one executable "
                         "defines every linked game's)")
    ap.add_argument("--stamp", help="written on success")
    a = ap.parse_args()

    for p in (a.module, a.exe):
        if not os.path.exists(p):
            print(f"check_module_imports: {p} missing", file=sys.stderr)
            return 2
    if a.stamp and os.path.exists(a.stamp):
        os.remove(a.stamp)

    mod_def, mod_undef = dynsyms(a.module)
    exe_def, _ = dynsyms(a.exe)
    providers = [(os.path.basename(a.exe), exe_def)]
    for lib in sorted(set(needed_libs(a.module)) | set(needed_libs(a.exe))):
        providers.append((os.path.basename(lib), dynsyms(lib)[0]))

    errors = []
    for sym in sorted(mod_undef - mod_def):
        where = [name for name, defs in providers if sym in defs]
        if not where:
            errors.append(f"import {sym}: no definition in the executable "
                          "or its libraries")
        elif len(where) > 1:
            errors.append(f"import {sym}: {len(where)} definitions "
                          f"({', '.join(where)})")
    forwarders = set(a.forwarder)
    for header in a.forwarder_header:
        with open(header) as f:
            forwarders |= set(re.findall(r"\bX\((\w+)\)", f.read()))
    for sym in sorted((mod_def & exe_def) - forwarders - RESERVED):
        errors.append(f"symbol {sym}: defined by both the module and the "
                      "executable, and not a declared forwarder")

    tag = os.path.basename(a.module)
    if errors:
        print(f"check_module_imports: REFUSED {tag} "
              f"({len(errors)} violation(s)):", file=sys.stderr)
        for e in errors:
            print(f"  {e}", file=sys.stderr)
        return 1
    n = len(mod_undef - mod_def)
    print(f"check_module_imports: {tag}: {n} imports, each with one "
          f"definition; {len(mod_def & exe_def & forwarders)} forwarders")
    if a.stamp:
        with open(a.stamp, "w") as f:
            f.write(f"{n}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
