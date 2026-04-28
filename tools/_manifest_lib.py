"""Shared loaders for the manifest pipeline.

build_manifest.py emits symbols.toml + globals.toml from psx.map.
The generators (gen_defsyms, gen_dispatch, …) load those plus the
hand-curated native.toml to decide what code/links to emit.
"""
from __future__ import annotations

import sys
import tomli
from pathlib import Path


def load_toml(path: Path) -> dict:
    with open(path, "rb") as f:
        return tomli.load(f)


def load_manifest(manifest_dir: Path) -> tuple[dict, dict, dict]:
    """Returns (native_config, symbols_index, globals_index)."""
    native = load_toml(manifest_dir / "native.toml")
    syms = load_toml(manifest_dir / "symbols.toml")
    globs = load_toml(manifest_dir / "globals.toml")
    sym_by_name = {f["name"]: f for f in syms.get("function", [])}
    sym_by_addr = {f["addr"]: f for f in syms.get("function", [])}
    glob_by_name = {g["name"]: g for g in globs.get("global", [])}
    return native, {"by_name": sym_by_name, "by_addr": sym_by_addr}, glob_by_name


def fail(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)
