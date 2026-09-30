#!/usr/bin/env python3
"""check_channel_registry.py — reconcile games/spyro-1/manifest/render_channels.toml
against the address-keyed substitution table in
third_party/psxrecomp/recompiler/src/code_generator.cpp.

Three failure modes:
  1. Registry references an address that the recompiler does not
     substitute — substitution went missing (likely a bad refactor).
  2. Recompiler substitutes an address no channel claims — orphan
     coupling site, must be filed under a channel.
  3. Two channels claim the same address.

On success, prints a one-line dashboard:
   channels: N total / G gated / O open / V verified

Wire into CTest as `channel_registry_audit`. Cheap (no PSX boot).
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

try:
    import tomllib  # py 3.11+
except ImportError:  # pragma: no cover
    import tomli as tomllib  # type: ignore

ADDR_RE = re.compile(r"addr\s*==\s*0x([0-9A-Fa-f]+)u?")


def parse_recompiler_sites(cpp_path: Path) -> set[int]:
    text = cpp_path.read_text()
    return {int(m.group(1), 16) for m in ADDR_RE.finditer(text)}


def parse_registry(toml_path: Path) -> list[dict]:
    with open(toml_path, "rb") as f:
        data = tomllib.load(f)
    return data.get("channel", [])


def coerce_addr(v) -> int:
    return v if isinstance(v, int) else int(v, 0)


def parse_state_region_names(toml_path: Path) -> set[str]:
    with open(toml_path, "rb") as f:
        data = tomllib.load(f)
    return {r["name"] for r in data.get("region", [])}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--registry", default="games/spyro-1/manifest/render_channels.toml")
    ap.add_argument(
        "--recompiler",
        default="third_party/psxrecomp/recompiler/src/code_generator.cpp",
    )
    ap.add_argument(
        "--state-regions",
        default="games/spyro-1/manifest/gameplay_state_regions.toml",
    )
    args = ap.parse_args()

    registry_path = Path(args.registry)
    cpp_path = Path(args.recompiler)
    state_regions_path = Path(args.state_regions)
    if not registry_path.is_file():
        print(f"error: registry not found: {registry_path}", file=sys.stderr)
        return 2
    if not cpp_path.is_file():
        print(f"error: recompiler source not found: {cpp_path}", file=sys.stderr)
        return 2

    channels = parse_registry(registry_path)
    recompiler_sites = parse_recompiler_sites(cpp_path)
    state_region_names = (parse_state_region_names(state_regions_path)
                          if state_regions_path.is_file() else set())

    # registry → address → list of channel names (catches duplicates)
    registry_addrs: dict[int, list[str]] = {}
    # Sites expressed as inline tables with a `kind` are GENERATED into the
    # recompiler from this manifest (tools/gen_channel_sites.py + the
    # --channel-sites loader) rather than hardcoded as `addr == 0xNNN` arms in
    # code_generator.cpp. They are therefore EXEMPT from the missing-in-cpp
    # check below: the manifest is their source of truth, not the .cpp.
    generated_addrs: set[int] = set()
    counts = {"open": 0, "gated": 0, "verified": 0, "audited_clean": 0}
    valid_status = set(counts.keys())

    errors: list[str] = []

    for ch in channels:
        name = ch.get("name", "<unnamed>")
        status = ch.get("status")
        if status not in valid_status:
            errors.append(f"channel {name!r}: unknown status {status!r} "
                          f"(expected one of {sorted(valid_status)})")
            continue
        counts[status] += 1
        sites = ch.get("substitution_sites") or []
        for s in sites:
            if isinstance(s, dict):
                if "addr" not in s:
                    errors.append(f"channel {name!r}: inline-table site "
                                  f"missing `addr`: {s}")
                    continue
                addr = coerce_addr(s["addr"])
                if "kind" in s:
                    generated_addrs.add(addr)
            else:
                addr = coerce_addr(s)
            registry_addrs.setdefault(addr, []).append(name)
        # gated channels with no sites are usually a manifest mistake.
        # Exceptions:
        #  * gated_via = "<other_channel>" — closure is a side effect of
        #    another channel's substitutions.
        #  * verdict_fn set — gating happens openpete-side (e.g. a C-side
        #    canonical-mask AND in the env-renderer wrapper) rather than
        #    via a recompiler substitution; sites are intentionally empty.
        gated_via = ch.get("gated_via")
        verdict_fn = ch.get("verdict_fn")
        if (status in ("gated", "verified") and not sites
                and not gated_via and not verdict_fn):
            errors.append(
                f"channel {name!r}: status={status!r} but substitution_sites "
                f"is empty, no gated_via, and no verdict_fn"
            )
        if gated_via:
            target = next((c for c in channels if c.get("name") == gated_via),
                          None)
            if not target:
                errors.append(
                    f"channel {name!r}: gated_via references "
                    f"unknown channel {gated_via!r}"
                )
            elif target.get("status") not in ("gated", "verified"):
                errors.append(
                    f"channel {name!r}: gated_via={gated_via!r} but that "
                    f"channel has status={target.get('status')!r} (must be "
                    "gated or verified)"
                )
        # state_regions cross-check: every region named here must exist
        # in gameplay_state_regions.toml so the harness can map a
        # divergent region back to this channel.
        if state_region_names:
            for sr in ch.get("state_regions") or []:
                if sr not in state_region_names:
                    errors.append(
                        f"channel {name!r}: state_regions references "
                        f"{sr!r} but no such region in {state_regions_path}"
                    )

    # 1. Missing: registry → recompiler. Generated sites are exempt — they
    #    are emitted from the manifest, not hardcoded in the .cpp.
    missing = sorted(a for a in registry_addrs
                     if a not in recompiler_sites and a not in generated_addrs)
    for a in missing:
        owners = ", ".join(registry_addrs[a])
        errors.append(
            f"missing substitution: 0x{a:08X} listed by [{owners}] but no "
            f"`addr == 0x{a:08X}` clause found in {cpp_path}"
        )

    # 2. Orphan: recompiler → registry.
    orphan = sorted(a for a in recompiler_sites if a not in registry_addrs)
    for a in orphan:
        errors.append(
            f"orphan substitution: 0x{a:08X} substituted in {cpp_path} but "
            f"no channel in {registry_path} claims it"
        )

    # 3. Duplicate ownership.
    for a, owners in sorted(registry_addrs.items()):
        if len(owners) > 1:
            errors.append(
                f"duplicate ownership: 0x{a:08X} claimed by multiple "
                f"channels: {owners}"
            )

    if errors:
        for e in errors:
            print(f"FAIL {e}", file=sys.stderr)
        return 1

    total = sum(counts.values())
    print(
        f"channels: {total} total / {counts['gated']} gated / "
        f"{counts['open']} open / {counts['verified']} verified / "
        f"{counts['audited_clean']} audited-clean  "
        f"({len(recompiler_sites)} substitution sites reconciled)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
