#!/usr/bin/env python3
"""check_channel_registry.py - reconcile games/spyro-1/manifest/render_channels.toml
against the substitutions the recompiler emitted.

A site with a `kind` is generated: tools/gen_channel_sites.py passes it to
psxrecomp (--channel-sites) and the recompiler tags each emitted
substitution in the generated C with `/* channel <kind>[: <knob>] */`
followed by the instruction's `/* 0xADDR: 0xINSN */` comment. Those sites
are reconciled against games/<game>/generated/PSX.EXE_full.c. A bare
address is a hand-written arm and is reconciled against the
`addr == 0xNNN` clauses of
third_party/psxrecomp/recompiler/src/code_generator.cpp.

Failure modes:
  1. Registry references an address that the recompiler does not
     substitute (missing emission, or missing hand-written arm).
  2. The generated C or code_generator.cpp substitutes an address no
     channel claims - orphan coupling site, must be filed under a channel.
  3. A generated site is emitted with another kind, symbol, or void-ness
     than its registry row names.
  4. Two channels claim the same address.
  5. The registry lists sites but none was reconciled (vacuous run).

On success, prints a one-line dashboard:
   channels: N total / G gated / O open / V verified ... (S sites reconciled)

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
MARKER_RE = re.compile(
    r"/\* channel (append_call|replace_call(?: \(void\))?|"
    r"offset_add|const_load)(?:: (\w+))? \*/\s*/\* 0x([0-9A-Fa-f]{8}): ")


def parse_recompiler_sites(cpp_path: Path) -> set[int]:
    """Return the addresses of the hand-written `addr == 0x...` arms in code_generator.cpp."""
    text = cpp_path.read_text()
    return {int(m.group(1), 16) for m in ADDR_RE.finditer(text)}


def call_symbol(prefix: str) -> str | None:
    """Return the callee of the last `f(...);` statement ending `prefix`.

    Matches the closing parenthesis back to its opening one, so arguments
    such as `(cpu->sp + 81)` and a preceding statement on the same line
    (an append_call follows the verbatim instruction) are skipped.
    """
    t = prefix.rstrip()
    if not t.endswith(");"):
        return None
    depth = 0
    for i in range(len(t) - 2, -1, -1):
        c = t[i]
        if c == ")":
            depth += 1
        elif c == "(":
            depth -= 1
            if depth == 0:
                m = re.search(r"(\w+)\s*$", t[:i])
                return m.group(1) if m else None
    return None


def parse_emitted_sites(c_path: Path) -> dict[int, set[tuple[str, str, bool]]]:
    """Map each tagged address in the generated C to its emitted (kind, symbol, void) set.

    A block the recompiler emits twice yields one entry per distinct
    emission; a site whose symbol cannot be read maps to symbol "?".
    """
    out: dict[int, set[tuple[str, str, bool]]] = {}
    with open(c_path, errors="replace") as f:
        for line in f:
            if "/* channel " not in line:
                continue
            m = MARKER_RE.search(line)
            if not m:
                continue
            tag, knob, addr_hex = m.group(1), m.group(2), m.group(3)
            void = tag.endswith("(void)")
            kind = tag.split(" ")[0]
            sym = knob if knob else call_symbol(line[:m.start()])
            out.setdefault(int(addr_hex, 16), set()).add((kind, sym or "?", void))
    return out


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
    ap.add_argument(
        "--generated",
        default=None,
        help="generated main-EXE C, the only binary --channel-sites reaches "
             "(default: <registry>/../../generated/PSX.EXE_full.c)",
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

    gen_path = (Path(args.generated) if args.generated else
                registry_path.parent.parent / "generated" / "PSX.EXE_full.c")
    if not gen_path.is_file():
        print(f"error: generated C not found: {gen_path} "
              "(run scripts/regen-recomp.sh)", file=sys.stderr)
        return 2

    channels = parse_registry(registry_path)
    recompiler_sites = parse_recompiler_sites(cpp_path)
    emitted = parse_emitted_sites(gen_path)
    state_region_names = (parse_state_region_names(state_regions_path)
                          if state_regions_path.is_file() else set())

    # registry -> address -> list of channel names (catches duplicates)
    registry_addrs: dict[int, list[str]] = {}
    # Sites expressed as inline tables with a `kind` are GENERATED into the
    # recompiler from this manifest (tools/gen_channel_sites.py + the
    # --channel-sites loader) rather than hardcoded as `addr == 0xNNN` arms in
    # code_generator.cpp. They are therefore EXEMPT from the missing-in-cpp
    # check below: the manifest is their source of truth, not the .cpp.
    generated_addrs: set[int] = set()
    # address -> (kind, symbol, void) the registry row expects to be emitted
    expected: dict[int, tuple[str, str, bool]] = {}
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
                    kind = s["kind"]
                    sym = s.get("hook") if kind in ("append_call",
                                                    "replace_call") \
                        else s.get("knob")
                    void = kind == "replace_call" and s.get("dest") == ""
                    expected[addr] = (kind, str(sym), void)
            else:
                addr = coerce_addr(s)
            registry_addrs.setdefault(addr, []).append(name)
        # gated channels with no sites are usually a manifest mistake.
        # Exceptions:
        #  * gated_via = "<other_channel>" - closure is a side effect of
        #    another channel's substitutions.
        #  * verdict_fn set - gating happens openpete-side (e.g. a C-side
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

    # 1. Missing: registry -> recompiler. A hand-written site needs its
    #    `addr ==` arm in the .cpp; a generated site needs its tagged
    #    emission in the generated C.
    missing = sorted(a for a in registry_addrs
                     if a not in recompiler_sites and a not in generated_addrs)
    for a in missing:
        owners = ", ".join(registry_addrs[a])
        errors.append(
            f"missing substitution: 0x{a:08X} listed by [{owners}] but no "
            f"`addr == 0x{a:08X}` clause found in {cpp_path}"
        )
    reconciled = len([a for a in registry_addrs
                      if a in recompiler_sites and a not in generated_addrs])
    for a in sorted(expected):
        owners = ", ".join(registry_addrs[a])
        kind, sym, void = expected[a]
        got = emitted.get(a)
        if not got:
            errors.append(
                f"missing emission: 0x{a:08X} {kind} {sym} listed by "
                f"[{owners}] but no `/* channel {kind}` tag at that address "
                f"in {gen_path} (stale regen, or the instruction no longer "
                "matches the site's shape)")
            continue
        if got != {(kind, sym, void)}:
            shown = ", ".join(f"{k} {s}{' (void)' if v else ''}"
                              for k, s, v in sorted(got))
            errors.append(
                f"emission mismatch: 0x{a:08X} [{owners}] expects {kind} "
                f"{sym}{' (void)' if void else ''}, {gen_path} emits {shown}")
            continue
        reconciled += 1

    # 2. Orphan: recompiler -> registry.
    orphan = sorted(a for a in recompiler_sites if a not in registry_addrs)
    for a in orphan:
        errors.append(
            f"orphan substitution: 0x{a:08X} substituted in {cpp_path} but "
            f"no channel in {registry_path} claims it"
        )
    for a in sorted(a for a in emitted if a not in generated_addrs):
        shown = ", ".join(f"{k} {s}" for k, s, _ in sorted(emitted[a]))
        errors.append(
            f"orphan emission: 0x{a:08X} ({shown}) tagged in {gen_path} but "
            f"no generated site in {registry_path} claims it")

    # 5. Engagement: a registry with sites must reconcile at least one.
    if registry_addrs and reconciled == 0:
        errors.append(
            f"vacuous: {len(registry_addrs)} registry sites, 0 reconciled")

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
        f"({reconciled}/{len(registry_addrs)} substitution sites reconciled: "
        f"{len(generated_addrs)} generated against {gen_path}, "
        f"{len(recompiler_sites)} hand-written arms in {cpp_path.name})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
