#!/usr/bin/env python3
"""gen_state_regions.py — emit a C header from the gameplay-state-regions
manifest so the runtime's hash-log path can compile in the canonical
list of addresses without a runtime TOML dependency.

Inputs:  manifest/gameplay_state_regions.toml
Outputs: <out_dir>/gameplay_state_regions.gen.h

`flatten_regions()` is the one place the manifest's per-kind fields are
reduced to the runtime table row. `--out-flat` writes the same rows as the
line-per-region text the DuckStation capture module reads at capture time
(`-psxtrace-regions`), so both sides hash from one reduction; the FNV-1a-64
of that text is emitted into the header as
OPENPETE_STATE_REGIONS_TABLE_HASH and written by the module into the trace,
and the verifier refuses a golden whose hash differs from the table it
registered.

Flat format, one header line then one line per region, fields separated by
single spaces, name last (a name carries no whitespace and is under 64
bytes so the verifier compares it whole):

    psxtrace-regions <format_version> <field_count>
    <kind> <start> <length> <aux> <elem_size> <mask_per_element> <mask> <name>

kind is DIRECT | INDIRECT_ARRAY | INDIRECT_ARRAY_END_PTR |
INDIRECT_ARRAY_COUNT; start/length/aux/elem_size are 0x-hex; mask is a
comma-joined list of 0x-hex offsets or `-` for none. cd_load_transient and
gameplay_neutral are openpete-side gate policy and are not in the file.
"""
import argparse
import sys
from pathlib import Path

try:
    import tomllib  # py 3.11+
except ImportError:
    import tomli as tomllib  # type: ignore


def coerce_int(v: object) -> int:
    return v if isinstance(v, int) else int(v, 0)  # type: ignore[arg-type]


def sanitize(s: str) -> str:
    return "".join(c if c.isalnum() else "_" for c in s)


def load_regions(manifest: str) -> list:
    with open(manifest, "rb") as f:
        data = tomllib.load(f)
    return data.get("region", [])


def flatten_regions(regions: list) -> list:
    """Reduce each [[region]] to the runtime table row.

    Row keys: kind (DIRECT / INDIRECT_ARRAY / INDIRECT_ARRAY_END_PTR /
    INDIRECT_ARRAY_COUNT), start, length, aux, elem_size, name, mask
    (tuple of offsets), mask_per_element (0/1), cd_load_transient (0/1),
    gameplay_neutral (0/1).
    """
    rows = []
    for r in regions:
        name = r["name"]
        kind = r.get("kind", "direct")
        aux = 0
        elem_size_field = 0
        if kind == "direct":
            start = coerce_int(r["start"])
            length = coerce_int(r["length"])
            kind_tag = "DIRECT"
        elif kind == "indirect_array":
            # `base_ptr` is the PSX vaddr of the base pointer; the runtime
            # dereferences it and hashes `element_size * max_elements`
            # bytes from the resulting address. `live_end_ptr_addr` clamps
            # the hashed extent to `*end_ptr - *base_ptr`; `live_count_addr`
            # clamps to `*count * element_size`. Either keeps stale heap
            # garbage in unallocated tail slots out of the hash.
            start = coerce_int(r["base_ptr"])
            elem_sz = coerce_int(r["element_size"])
            max_n = coerce_int(r["max_elements"])
            length = elem_sz * max_n
            if "live_end_ptr_addr" in r:
                aux = coerce_int(r["live_end_ptr_addr"])
                # Per-element masks need the stride at runtime.
                if "mask_per_element" in r:
                    elem_size_field = elem_sz
                kind_tag = "INDIRECT_ARRAY_END_PTR"
            elif "live_count_addr" in r:
                aux = coerce_int(r["live_count_addr"])
                elem_size_field = elem_sz
                kind_tag = "INDIRECT_ARRAY_COUNT"
            else:
                if "mask_per_element" in r:
                    elem_size_field = elem_sz
                kind_tag = "INDIRECT_ARRAY"
        else:
            raise ValueError(f"unknown region kind {kind!r} for {name}")
        if "mask_offsets" in r:
            mask = tuple(coerce_int(m) for m in r["mask_offsets"])
            per_elem = 0
        elif "mask_per_element" in r:
            mask = tuple(coerce_int(m) for m in r["mask_per_element"])
            per_elem = 1
        else:
            mask = ()
            per_elem = 0
        rows.append({
            "kind": kind_tag,
            "start": start,
            "length": length,
            "aux": aux,
            "elem_size": elem_size_field,
            "name": name,
            "mask": mask,
            "mask_per_element": per_elem,
            "cd_load_transient": 1 if r.get("cd_load_transient", False) else 0,
            "gameplay_neutral": 1 if r.get("gameplay_neutral", False) else 0,
        })
    return rows


FLAT_FORMAT_VERSION = 1
FLAT_FIELD_COUNT = 8
# The verifier stores a header name in a 64-byte buffer and compares whole.
NAME_MAX_BYTES = 63


def check_names(rows: list) -> None:
    seen = set()
    for row in rows:
        name = row["name"]
        if any(c.isspace() for c in name):
            raise ValueError(f"region name {name!r} contains whitespace")
        if len(name.encode()) > NAME_MAX_BYTES:
            raise ValueError(f"region name {name!r} is longer than {NAME_MAX_BYTES} bytes")
        if name in seen:
            raise ValueError(f"duplicate region name {name!r}")
        seen.add(name)


def flat_text(rows: list) -> str:
    lines = [f"psxtrace-regions {FLAT_FORMAT_VERSION} {FLAT_FIELD_COUNT}"]
    for row in rows:
        mask = ",".join(f"0x{m:X}" for m in row["mask"]) or "-"
        lines.append(
            f"{row['kind']} 0x{row['start']:08X} 0x{row['length']:X} 0x{row['aux']:08X} "
            f"0x{row['elem_size']:X} {row['mask_per_element']} {mask} {row['name']}")
    return "\n".join(lines) + "\n"


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--out",
                    help="C header with the table and OPENPETE_STATE_REGIONS_TABLE_HASH")
    ap.add_argument("--out-flat",
                    help="line-per-region text the DuckStation capture module reads "
                         "(-psxtrace-regions)")
    ap.add_argument("--out-lua",
                    help="optional: emit a Lua-table mirror for the "
                         "PCSX-Redux oracle dumper (scripts/pcsx-redux-trace.lua)")
    args = ap.parse_args()
    if not (args.out or args.out_flat or args.out_lua):
        ap.error("one of --out, --out-flat, --out-lua is required")

    regions = load_regions(args.manifest)
    if not regions:
        print(f"error: no [[region]] entries in {args.manifest}", file=sys.stderr)
        return 1
    try:
        rows = flatten_regions(regions)
        check_names(rows)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    flat = flat_text(rows)
    table_hash = fnv1a64(flat.encode())

    if args.out_flat:
        out_flat = Path(args.out_flat)
        out_flat.parent.mkdir(parents=True, exist_ok=True)
        out_flat.write_text(flat)
        print(f"[gen_state_regions] wrote {out_flat} ({len(rows)} regions, flat, "
              f"hash 0x{table_hash:016X})")
    if not args.out:
        return 0

    lines: list[str] = []
    lines.append("/* Auto-generated by tools/gen_state_regions.py — do not edit. */")
    lines.append(f"/* Source: {args.manifest} */")
    lines.append("")
    lines.append("#pragma once")
    lines.append("/* The OpenpeteStateRegion struct lives in openpete_psx.h so this")
    lines.append(" * header is purely the data table. Per-game TUs (e.g.")
    lines.append(" * games/spyro-1/spyro_state_regions.c) include both and re-export.")
    lines.append(" */")
    lines.append("#include \"openpete_psx.h\"")
    lines.append("")
    lines.append(f"#define OPENPETE_STATE_REGION_COUNT {len(rows)}")
    lines.append("/* FNV-1a-64 of the flat table text (--out-flat) the capture module")
    lines.append(" * hashes into the trace header; the verifier refuses a golden taken")
    lines.append(" * from a different table. */")
    lines.append(f"#define OPENPETE_STATE_REGIONS_TABLE_HASH 0x{table_hash:016X}ull")
    lines.append("")

    # Per-region static mask arrays keep the table struct dense.
    for row in rows:
        if not row["mask"]:
            continue
        sym = f"openpete_mask_{sanitize(row['name'])}"
        ms = ", ".join(f"0x{m:X}" for m in row["mask"])
        lines.append(f"static const uint16_t {sym}[] = {{ {ms} }};")
    lines.append("")

    lines.append("static const OpenpeteStateRegion openpete_state_regions_table[OPENPETE_STATE_REGION_COUNT] = {")
    for row in rows:
        if row["mask"]:
            mask_sym = f"openpete_mask_{sanitize(row['name'])}"
        else:
            mask_sym = "NULL"
        # cd_load_transient: the region diverges for the duration of a
        # synchronous CdRead and re-converges; the gate tolerates a healing
        # divergence (see openpete_psx.h). gameplay_neutral: audited
        # render-side region whose divergence is logged, never hard-failed;
        # the proof accompanies it in the manifest.
        lines.append(
            f"    {{ OPENPETE_REGION_{row['kind']}, 0x{row['start']:08X}u, 0x{row['length']:X}u, "
            f"0x{row['aux']:08X}u, 0x{row['elem_size']:X}u, \"{row['name']}\", "
            f"{mask_sym}, {len(row['mask'])}, {row['mask_per_element']}, "
            f"{row['cd_load_transient']}, {row['gameplay_neutral']} }},"
        )
    lines.append("};")
    lines.append("")

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines))
    print(f"[gen_state_regions] wrote {out} ({len(rows)} regions, hash 0x{table_hash:016X})")

    if args.out_lua:
        # Kind values mirror the OPENPETE_REGION_* enum, hardcoded so the
        # PCSX-Redux Lua dumper (LuaJIT, no C toolchain) needs no header.
        # elem_size is emitted for COUNT regions only: the dumper has no
        # mask support, so the per-element stride is unused elsewhere.
        lua_kind = {"DIRECT": 0, "INDIRECT_ARRAY": 1,
                    "INDIRECT_ARRAY_END_PTR": 2, "INDIRECT_ARRAY_COUNT": 3}
        lua_lines: list[str] = []
        lua_lines.append("-- Auto-generated by tools/gen_state_regions.py — do not edit.")
        lua_lines.append(f"-- Source: {args.manifest}")
        lua_lines.append("--")
        lua_lines.append("-- Mirror of openpete_state_regions_table for the PCSX-Redux")
        lua_lines.append("-- oracle dumper. Kind values match the OPENPETE_REGION_* enum:")
        lua_lines.append("--   0=DIRECT 1=INDIRECT_ARRAY 2=INDIRECT_ARRAY_END_PTR")
        lua_lines.append("--   3=INDIRECT_ARRAY_COUNT")
        lua_lines.append("return {")
        for row in rows:
            es = row["elem_size"] if row["kind"] == "INDIRECT_ARRAY_COUNT" else 0
            lua_lines.append(
                f"    {{ kind={lua_kind[row['kind']]}, start=0x{row['start']:08X}, "
                f"length=0x{row['length']:X}, aux=0x{row['aux']:08X}, "
                f"elem_size=0x{es:X}, "
                f"name={row['name']!r} }},"
            )
        lua_lines.append("}")
        out_lua = Path(args.out_lua)
        out_lua.parent.mkdir(parents=True, exist_ok=True)
        out_lua.write_text("\n".join(lua_lines) + "\n")
        print(f"[gen_state_regions] wrote {out_lua} ({len(rows)} regions, lua)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
