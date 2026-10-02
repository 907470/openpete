#!/usr/bin/env python3
"""derive_state_regions.py - print the link-map-derived direct rows of a
game's gameplay_state_regions.toml.

The rows are the main image's .data/.sdata/.sbss/.bss extents (psx.map
`main_<SECTION>_START/END`) minus the `[[psyq_internal]]` extents and every
`[[expect]]` range of games/<game>/manifest/ram_diff_expect.toml, with the
named rows given on the command line carved out as rows of their own, the
rest coalesced into one row per remaining extent. Each coalesced row lists
its symbol count and its role-named (`g_`/lower-case) symbols as a comment;
`D_<addr>` names are counted, not listed.

    tools/derive_state_regions.py spyro-3 \\
        --named psyq_rand_seed=0x80073F3C:4:gameplay \\
        --named levelIndex=0x8006C58C:4:gameplay \\
        --overlay 0x800742D0:0x2D800

`--overlay START:LEN` adds the overlay image union as `cd_load_transient`
rows cut at the `[[expect]]` ranges inside it. A named row's flags
(`cd_load_transient`, `constant_ok`, `gameplay_neutral`, `host_precision`, masks,
descriptions) are added when the manifest is assembled.

Prints TOML `[[region]]` rows to stdout; the manifest is assembled by hand
from this output plus the indirect (pool) rows, which need the decomp's
struct facts. `tools/check_regions_containment.py OLD.flat NEW.flat` then
proves the assembled table hashes every byte the table it replaces hashed.

    tools/derive_state_regions.py --check games/spyro-1/manifest/gameplay_state_regions.toml

re-derives the direct rows from the manifest's own `[derivation]` table
(`named`, `overlay`, `min_row`: the arguments above) against the current
expect file and fails (exit 2) when the manifest's direct rows (every row
with a start/length whose class is not psyq_mirror) differ in extent from
the derivation, or when the derivation drops a fragment. An expect row
deleted by a fix therefore forces the manifest to be re-derived in the
same change (ctest `state_regions_derivation_<game>`).
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

try:
    import tomllib
except ImportError:
    import tomli as tomllib  # type: ignore

ROOT = Path(__file__).resolve().parents[1]
SECTIONS = ("DATA", "SDATA", "SBSS", "BSS")


def read_map(path: Path):
    bounds = {}
    symbols = []  # (addr, name)
    sym_re = re.compile(r"^\s+0x([0-9a-f]{16})\s+([A-Za-z_][A-Za-z0-9_.]*)\s*$")
    bound_re = re.compile(r"^\s+0x([0-9a-f]{16})\s+main_(\w+)_(START|END) = \.")
    with open(path) as fh:
        for line in fh:
            m = bound_re.match(line)
            if m:
                bounds[(m.group(2), m.group(3))] = int(m.group(1), 16)
                continue
            m = sym_re.match(line)
            if m:
                symbols.append((int(m.group(1), 16), m.group(2)))
    return bounds, symbols


def subtract(ranges, holes):
    out = []
    for lo, hi in ranges:
        cur = [(lo, hi)]
        for hlo, hhi in holes:
            nxt = []
            for a, b in cur:
                if hhi <= a or hlo >= b:
                    nxt.append((a, b))
                else:
                    if hlo > a:
                        nxt.append((a, hlo))
                    if hhi < b:
                        nxt.append((hhi, b))
            cur = nxt
        out.extend(cur)
    return sorted(out)


def check_manifest(path: Path) -> int:
    """Compare a manifest's direct rows with the derivation its [derivation]
    table describes; exit 2 on any difference or dropped fragment."""
    with open(path, "rb") as fh:
        man = tomllib.load(fh)
    d = man.get("derivation")
    if not d:
        print(f"[derivation] {path}: no [derivation] table", file=sys.stderr)
        return 2
    game = path.resolve().parents[1].name
    argv = [game] + [f"--named={n}" for n in d.get("named", [])]
    if d.get("overlay"):
        argv.append(f"--overlay={d['overlay']}")
    argv += ["--min-row", str(d.get("min_row", 4))]
    import io, contextlib
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = main(argv)
    if rc != 0:
        print(buf.getvalue(), file=sys.stderr)
        return 2
    derived = set()
    dropped = 0
    cur = {}
    for line in buf.getvalue().splitlines():
        if line.startswith("# dropped fragment"):
            dropped += 1
        m = re.match(r"(start|length)\s*=\s*(0x[0-9A-Fa-f]+)", line)
        if m:
            cur[m.group(1)] = int(m.group(2), 16)
            if "start" in cur and "length" in cur:
                derived.add((cur["start"], cur["length"]))
                cur = {}
    manifest = {(int(r["start"]), int(r["length"])) for r in man.get("region", [])
                if "start" in r and r.get("kind", "direct") == "direct"
                and r.get("class") != "psyq_mirror"}
    missing = sorted(derived - manifest)
    extra = sorted(manifest - derived)
    for s0, n in missing:
        print(f"[derivation] derived row 0x{s0:08X}+0x{n:X} not in manifest")
    for s0, n in extra:
        print(f"[derivation] manifest row 0x{s0:08X}+0x{n:X} not derived")
    if dropped:
        print(f"[derivation] derivation dropped {dropped} fragment(s) below min_row")
    print(f"[derivation] {game}: {len(derived)} derived direct rows, {len(manifest)} manifest direct rows, "
          f"{len(missing) + len(extra)} differ, {dropped} dropped")
    if missing or extra or dropped:
        print("[derivation] FAIL: re-derive the manifest against the current expect file")
        return 2
    print("[derivation] PASS")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("game", nargs="?")
    ap.add_argument("--named", action="append", default=[],
                    help="NAME=ADDR:LEN[:class] carved out of the extents as its own row")
    ap.add_argument("--min-row", type=int, default=4,
                    help="drop coalesced fragments shorter than this many bytes (default 4)")
    ap.add_argument("--overlay", metavar="START:LEN",
                    help="overlay image union (OVERLAY_LOAD_ADDR, largest image); emitted as "
                         "cd_load_transient rows cut at the [[expect]] ranges inside it")
    ap.add_argument("--check", metavar="MANIFEST",
                    help="re-derive from the manifest's [derivation] table and compare")
    a = ap.parse_args(argv)
    if a.check:
        return check_manifest(Path(a.check))
    if not a.game:
        ap.error("game required")

    gdir = ROOT / "games" / a.game
    map_path = gdir / a.game / "build" / "psx.map"
    expect_path = gdir / "manifest" / "ram_diff_expect.toml"
    bounds, symbols = read_map(map_path)
    with open(expect_path, "rb") as fh:
        expect = tomllib.load(fh)

    extents = []
    for sec in SECTIONS:
        lo, hi = bounds.get((sec, "START")), bounds.get((sec, "END"))
        if lo is None or hi is None or hi <= lo:
            print(f"# main_{sec}: absent", file=sys.stderr)
            continue
        extents.append((lo, hi))
        print(f"# main_{sec}: 0x{lo:08X}-0x{hi:08X} ({hi - lo} bytes)")

    holes = []
    for row in expect.get("psyq_internal", []):
        s, n = int(row["start"]), int(row["length"])
        holes.append((s, s + n))
        print(f"# minus psyq_internal 0x{s:08X}-0x{s + n:08X} ({row.get('source', '')})")
    for row in expect.get("expect", []):
        s, e = int(row["start"]), int(row["end"])
        holes.append((s, e))
        print(f"# minus expect {row['class']} 0x{s:08X}-0x{e:08X} ({row.get('name', '')})")
    named = []
    for spec in a.named:
        name, rest = spec.split("=", 1)
        parts = rest.split(":")
        addr, ln = int(parts[0], 0), int(parts[1], 0)
        cls = parts[2] if len(parts) > 2 else "gameplay"
        named.append((addr, ln, name, cls))
        holes.append((addr, addr + ln))

    rows = subtract(extents, holes)
    rows = [(lo, hi) for lo, hi in rows if hi - lo >= a.min_row]
    dropped = [(lo, hi) for lo, hi in subtract(extents, holes) if hi - lo < a.min_row]
    for lo, hi in dropped:
        print(f"# dropped fragment 0x{lo:08X}-0x{hi:08X} ({hi - lo} bytes)")

    total = 0
    print()
    for addr, ln, name, cls in sorted(named):
        print("[[region]]")
        print(f'name   = "{name}"')
        print(f"start  = 0x{addr:08X}")
        print(f"length = 0x{ln:X}")
        print(f'class  = "{cls}"')
        print()
        total += ln
    for i, (lo, hi) in enumerate(rows):
        syms = [(s, n) for s, n in symbols if lo <= s < hi]
        role = [n for _, n in syms if not n.startswith("D_")]
        sec = next((sec for sec in SECTIONS
                    if bounds.get((sec, "START"), 1) <= lo < bounds.get((sec, "END"), 0)), "?")
        print(f"# .{sec.lower()} extent {i}: {len(syms)} symbols" +
              (f"; named: {', '.join(role[:40])}" + (" ..." if len(role) > 40 else "") if role else ""))
        print("[[region]]")
        print(f'name   = "main_{sec.lower()}_{lo:08x}"')
        print(f"start  = 0x{lo:08X}")
        print(f"length = 0x{hi - lo:X}")
        print('class  = "gameplay"')
        print()
        total += hi - lo
    n_rows = len(rows) + len(named)
    if a.overlay:
        o_lo, o_len = (int(x, 0) for x in a.overlay.split(":"))
        o_hi = o_lo + o_len
        holes_in = [(s, e) for s, e in holes if s < o_hi and e > o_lo]
        pieces = subtract([(o_lo, o_hi)], holes_in)
        print(f"# overlay image union 0x{o_lo:08X}-0x{o_hi:08X}, cut at {len(holes_in)} expect range(s) "
              f"into {len(pieces)} row(s); every piece is a CdRead destination")
        for i, (lo, hi) in enumerate(pieces):
            print("[[region]]")
            print(f'name   = "overlay_image_{i}"')
            print(f"start  = 0x{lo:08X}")
            print(f"length = 0x{hi - lo:X}")
            print('class  = "gameplay"')
            print("cd_load_transient = true")
            print()
            total += hi - lo
            n_rows += 1
    print(f"# {n_rows} direct rows, {total} bytes", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
