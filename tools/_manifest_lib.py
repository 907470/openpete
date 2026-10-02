"""Shared loaders for the manifest pipeline.

build_manifest.py emits symbols.toml + globals.toml from psx.map.
The generators (gen_defsyms, gen_dispatch, ...) load those plus the
hand-curated native.toml to decide what code/links to emit.

When the rewriter has run (Pass 6 discovery), per-TU ``*.discovered.toml``
files under the rewritten dir carry auto-discovered entries that the
generators merge in on top of native.toml. Hand entries override auto
on name collision (a hand entry's `c_name`, `args`, `psyq_main`, etc.
are preserved verbatim; auto only contributes new names).
"""
from __future__ import annotations

import sys
import tomli
from pathlib import Path


def load_toml(path: Path) -> dict:
    with open(path, "rb") as f:
        return tomli.load(f)


def merge_discovered(native: dict, rewritten_dir: Path | None) -> None:
    """Fold every ``*.discovered.toml`` under ``rewritten_dir`` into
    ``native`` in-place. Hand entries win - auto entries with the same
    name (or psyq_name) are skipped. New auto entries get appended with
    a sensible default c_name (``openpete_<name>`` for PSYQ wrappers;
    same name for outbound_recomp; same name for dispatch)."""
    if rewritten_dir is None:
        return
    rewritten_dir = Path(rewritten_dir)
    if not rewritten_dir.exists():
        return

    hand_dispatch = {e["name"] for e in native.get("dispatch", [])}
    hand_recomp = {e["name"] for e in native.get("outbound_recomp", [])}
    hand_psyq = {e["psyq_name"] for e in native.get("outbound_psyq", [])}

    # `outbound_psyq_skip` / `outbound_recomp_skip` name PSYQ entry points
    # whose host-side thunk is already hand-written (e.g.
    # openpete_CdReadCallback in psyq_thunks.cpp bridges native -> host cb,
    # not native -> recomp). Auto-discovery sees the call site and would
    # emit a colliding thunk; the skip list suppresses that.
    skip_psyq = set(native.get("outbound_psyq_skip", []))
    skip_recomp = set(native.get("outbound_recomp_skip", []))

    seen_dispatch: set[str] = set()
    seen_recomp: set[str] = set()
    seen_psyq: set[str] = set()

    for path in sorted(rewritten_dir.glob("*.discovered.toml")):
        with open(path, "rb") as fh:
            d = tomli.load(fh)
        for e in d.get("discovered_dispatch", []):
            n = e["name"]
            if n in hand_dispatch or n in seen_dispatch:
                continue
            seen_dispatch.add(n)
            # `func_<addr>` native bodies collide at link with PSX.EXE_full.c's
            # recompiled body of the same name; gen_renames.py class 3 rewrites
            # the native source to `spyro_native_<name>`, so the dispatch
            # generator needs to call the renamed symbol - otherwise the
            # generated `extern void func_<addr>(...)` resolves to the recomp
            # body (with `(CPUState*)` shape) and the call crashes.
            c_name = f"spyro_native_{n}" if n.startswith("func_") else n
            entry = {
                "name": n,
                "addr": e["addr"],
                "c_name": c_name,
            }
            if "args" in e:
                entry["args"] = e["args"]
            native.setdefault("dispatch", []).append(entry)
        for e in d.get("discovered_outbound_recomp", []):
            n = e["name"]
            if n in hand_recomp or n in seen_recomp or n in skip_recomp:
                continue
            seen_recomp.add(n)
            # Every auto-discovered recomp callee gets routed through an
            # `openpete_<name>` thunk and a `#define <name> openpete_<name>`
            # redirect in native_renames.h. Two collision classes this
            # avoids:
            #   1. `func_<addr>` names clashing with PSX.EXE_full.c's
            #      recompiled body of the same name.
            #   2. names that overlap host symbols (libc / PsyCross /
            #      runtime) - e.g. `rand`, `memcpy`, `strcpy`. Without
            #      the prefix, the auto-thunk shadows the host symbol
            #      under --allow-multiple-definition and *every* host
            #      caller of that name routes into the recomp dispatcher
            #      (rand crash at frame 1793, fixed by this default).
            # Hand-curated entries are unaffected - the `n in hand_recomp`
            # guard above means a hand entry without `c_name` keeps its
            # bare-name binding (today's PascalCase Initialize / etc.).
            entry = {"name": n, "c_name": f"openpete_{n}"}
            native.setdefault("outbound_recomp", []).append(entry)
        # Auto-emitting PSYQ thunks is a no-op until Pass 0 (source-level
        # renames driven by tweaks.toml) lands. Source-level `LoadImage(...)`
        # link-resolves to PsyCross's host symbol directly today; an
        # `openpete_LoadImage` thunk would be dead code. Keep the hand list
        # authoritative for outbound_psyq for now - discovery still
        # populates the `*.discovered.toml` so the diagnostic diff surfaces
        # mismatches, but the merge step doesn't fold psyq into the
        # generators' input.
        _ = d.get("discovered_outbound_psyq", [])


def load_manifest(manifest_dir: Path,
                  rewritten_dir: Path | None = None
                  ) -> tuple[dict, dict, dict]:
    """Returns (native_config, symbols_index, globals_index).

    If ``rewritten_dir`` is provided, every ``*.discovered.toml`` under it
    is merged into the returned native_config (hand entries override).
    """
    native = load_toml(manifest_dir / "native.toml")
    syms = load_toml(manifest_dir / "symbols.toml")
    globs = load_toml(manifest_dir / "globals.toml")
    merge_discovered(native, rewritten_dir)
    sym_by_name = {f["name"]: f for f in syms.get("function", [])}
    sym_by_addr = {f["addr"]: f for f in syms.get("function", [])}
    glob_by_name = {g["name"]: g for g in globs.get("global", [])}
    return native, {"by_name": sym_by_name, "by_addr": sym_by_addr}, glob_by_name


def fail(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)
