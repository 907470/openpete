#!/usr/bin/env python3
"""gen_channel_signatures.py — signature anchors for the determinism-critical
render channels (hook-anchor plan step 2 / Tier-3 prep).

A channel substitution site is normally anchored by its cached retail address.
For a *non-retail* binary (a romhack, or a custom-compiled-from-source build)
those addresses shift, so the address alone can't locate the hook site. A
SIGNATURE locates it by content instead: a window of K consecutive
instructions starting at the site, with the register fields wildcarded, that
is UNIQUE in the binary. Scanning a different binary for that pattern
relocates the site.

This tool calibrates signatures from the reference (retail SCUS-94228) binary
for every channel marked `determinism_critical = true`, growing the window
until each signature is unique, and verifies:
  1. each signature resolves to EXACTLY ONE address, and it is the cached addr
     (self-consistency + distinctiveness on the reference binary), and
  2. a synthetic-relocation self-test: the matcher finds a site's window when
     it is spliced into a buffer at an arbitrary offset (proves relocation
     works without needing a real romhack).

Outputs `<out>` (a signatures table) and exits non-zero if any site cannot be
made unique or fails to resolve — so a stale manifest / shifted reference is
caught.

Scope is deliberately minimal (per the plan's Open question §3): single-window
signatures over determinism-critical sites, calibrated + verified on retail.
Deploying them to actually relocate a romhack is Tier-3 (future).
"""
import argparse
import bisect
import struct
import sys
from pathlib import Path

try:
    import tomllib
except ImportError:
    import tomli as tomllib  # type: ignore

MAX_WINDOW = 8       # local-window growth limit
MAX_FUNC_WINDOW = 28  # function-entry-window growth limit (scoped fallback)


def load_exe(path: Path):
    data = path.read_bytes()
    if data[:8] != b"PS-X EXE":
        raise SystemExit(f"{path}: not a PS-X EXE")
    load_addr = struct.unpack_from("<I", data, 0x18)[0]
    file_size = struct.unpack_from("<I", data, 0x1C)[0]
    text = data[0x800:0x800 + file_size]
    return load_addr, text


def words(text: bytes):
    return [struct.unpack_from("<I", text, i)[0] for i in range(0, len(text) & ~3, 4)]


def sig_mask(instr: int) -> int:
    """Wildcard the register fields; keep the opcode + funct/shamt (R-type) or
    the opcode + immediate (I-type) so the pattern is register-allocation
    independent but still distinctive."""
    op = (instr >> 26) & 0x3F
    if op == 0x00:          # SPECIAL (R-type): keep op + shamt + funct
        return 0xFC0007FF
    if op in (0x02, 0x03):  # J / JAL: target varies; keep opcode only
        return 0xFC000000
    return 0xFC00FFFF       # I-type: keep opcode + immediate, wildcard rs/rt


def masked_window(prog, idx, k):
    """Return [(value, mask), ...] for k instructions starting at word index idx."""
    out = []
    for j in range(k):
        instr = prog[idx + j]
        m = sig_mask(instr)
        out.append((instr & m, m))
    return out


def scan(prog, sig):
    """Return list of word-indices where the masked window `sig` matches."""
    k = len(sig)
    hits = []
    for i in range(0, len(prog) - k + 1):
        ok = True
        for j, (val, m) in enumerate(sig):
            if (prog[i + j] & m) != val:
                ok = False
                break
        if ok:
            hits.append(i)
    return hits


def load_functions(path: Path):
    """Sorted list of function-start vaddrs from discovered_functions.log."""
    if not path.is_file():
        return []
    out = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if line.startswith("0x"):
            out.append(int(line.split()[0], 16))
    out.sort()
    return out


def find_signature(prog, hook_idx, func_idx):
    """Return (sig, hook_delta) such that scanning `prog` for the masked window
    `sig` yields a unique base index B, and the hook is at B + hook_delta. Two
    tiers: a LOCAL window near the hook (small), else a FUNCTION-SCOPED window
    at the containing function's entry (distinct prologues disambiguate
    near-duplicate parallel render paths). Returns (None, None) if neither is
    unique within the growth limits."""
    # Tier 1 — local window near the hook (prefer smallest; backward first).
    for total_k in range(1, MAX_WINDOW + 1):
        for pre in range(total_k - 1, -1, -1):
            start = hook_idx - pre
            if start < 0 or start + total_k > len(prog):
                continue
            sig = masked_window(prog, start, total_k)
            if scan(prog, sig) == [start]:
                return sig, pre
    # Tier 2 — function-scoped: window at the function entry + hook offset.
    if func_idx is not None:
        for total_k in range(1, MAX_FUNC_WINDOW + 1):
            if func_idx + total_k > len(prog):
                break
            sig = masked_window(prog, func_idx, total_k)
            if scan(prog, sig) == [func_idx]:
                return sig, (hook_idx - func_idx)
    return None, None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", default="manifest/render_channels.toml")
    ap.add_argument("--exe", default="games/spyro-1/spyro-1/build/PSX.EXE")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    exe_path = Path(args.exe)
    funcs_path = Path("games/spyro-1/generated/discovered_functions.log")
    if not exe_path.is_file() or not funcs_path.is_file():
        # Regen artifacts absent (e.g. a fresh checkout / CI without the disc):
        # nothing to calibrate against — skip cleanly rather than fail.
        print("[gen_channel_signatures] SKIP — reference binary or "
              "discovered_functions.log not present "
              f"({exe_path} / {funcs_path})")
        return 0

    load_addr, text = load_exe(exe_path)
    prog = words(text)
    end_addr = load_addr + len(text)
    funcs = load_functions(funcs_path)

    def containing_func_idx(addr):
        if not funcs:
            return None
        i = bisect.bisect_right(funcs, addr) - 1
        if i < 0:
            return None
        return (funcs[i] - load_addr) // 4

    with open(args.manifest, "rb") as f:
        data = tomllib.load(f)

    # Collect determinism-critical site addresses.
    sites = []  # (channel, addr)
    for ch in data.get("channel", []):
        if not ch.get("determinism_critical"):
            continue
        name = ch.get("name", "<unnamed>")
        for s in ch.get("substitution_sites") or []:
            addr = s["addr"] if isinstance(s, dict) else (
                s if isinstance(s, int) else int(s, 0))
            sites.append((name, addr))

    if not sites:
        print("error: no channels marked determinism_critical = true",
              file=sys.stderr)
        return 1

    lines = [
        "# Auto-generated by tools/gen_channel_signatures.py — do not edit.",
        f"# Reference binary: {args.exe} (load 0x{load_addr:08X})",
        "# Format: <hook-addr-hex> klen anchor_off <val:mask ...>",
        "#   the hook sits at window-word `anchor_off`; resolve = match_base + anchor_off*4",
    ]
    failures = 0
    made = 0
    for name, addr in sorted(sites, key=lambda t: t[1]):
        if addr < load_addr or addr >= end_addr:
            print(f"FAIL {name} 0x{addr:08X}: outside reference text", file=sys.stderr)
            failures += 1
            continue
        idx = (addr - load_addr) // 4
        sig, hook_delta = find_signature(prog, idx, containing_func_idx(addr))
        if sig is None:
            print(f"FAIL {name} 0x{addr:08X}: not uniquely anchorable "
                  f"(local <= {MAX_WINDOW}, func-scoped <= {MAX_FUNC_WINDOW})",
                  file=sys.stderr)
            failures += 1
            continue
        # Self-check the resolution on the reference binary.
        hits = scan(prog, sig)
        if len(hits) != 1 or load_addr + (hits[0] + hook_delta) * 4 != addr:
            print(f"FAIL {name} 0x{addr:08X}: signature does not resolve to "
                  f"itself", file=sys.stderr)
            failures += 1
            continue
        parts = " ".join(f"{v:08X}:{m:08X}" for v, m in sig)
        # Format: <hook-addr> <klen> <hook_delta_words> <words...>
        lines.append(f"0x{addr:08X} {len(sig)} {hook_delta} {parts}")
        made += 1

    # Synthetic-relocation self-test: splice the first resolved site's window
    # into a buffer at an arbitrary offset and confirm the matcher finds it
    # there (proves relocation works without a real romhack).
    selftest = "skipped (no sites)"
    if made:
        # rebuild the first emitted signature (new format: addr klen off words)
        first = next(l for l in lines if l.startswith("0x"))
        toks = first.split()
        k = int(toks[1])
        anchor_off = int(toks[2])
        sig = []
        for t in toks[3:3 + k]:
            v, m = t.split(":")
            sig.append((int(v, 16), int(m, 16)))
        # original window words (unmasked) — the window STARTS anchor_off
        # instructions before the hook addr.
        site_addr = int(toks[0], 16)
        start_idx = (site_addr - load_addr) // 4 - anchor_off
        window = prog[start_idx:start_idx + k]
        # synthetic buffer: distinct padding + window + padding, placed at an
        # arbitrary offset; the matcher must relocate the window's start, so
        # the resolved hook = found_start + anchor_off.
        pad = [0x3C080000] * 20  # lui $t0, 0  (distinct from the site windows)
        win_start = 13
        buf = pad[:win_start] + list(window) + pad
        hits = scan(buf, sig)
        resolved_hook = (hits[0] + anchor_off) if len(hits) == 1 else None
        if hits == [win_start] and resolved_hook == win_start + anchor_off:
            selftest = (f"PASS (window relocated to word {win_start}, "
                        f"hook at +{anchor_off})")
        else:
            selftest = f"FAIL (hits={hits}, expected [{win_start}])"
            failures += 1

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n")
    print(f"[gen_channel_signatures] {made} signatures, {failures} failures; "
          f"relocation self-test: {selftest}")
    print(f"[gen_channel_signatures] wrote {out}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
