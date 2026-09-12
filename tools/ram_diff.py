#!/usr/bin/env python3
"""Whole-RAM diff between a DuckStation dump and an openpete dump taken at the
same anchor, classified by the game's expectation file.

Every differing byte is classified on its own, then bytes of one class at
most --gap apart are merged into a range for the report. Every count below
is a count of DIFFERING bytes; the span of a range is printed beside it. A
byte goes to the first class that matches:

  populated       [[populated]] rows: a PSYQ mirror openpete writes. A
                  differing byte here is UNEXPECTED (the mirror is wrong).
  kernel          0x0000-0xFFFF: the BIOS and psx_bios.c differ by construction.
  psyq_internal   the PSYQ library's own data/sbss/bss extents from the link
                  map ([[psyq_internal]] rows): the class-2 mirror gaps. The
                  class is ratcheted: [psyq_internal_baseline.<anchor>] holds
                  the differing-byte count, range count, a digest of the
                  differing offsets and the hit measured when the row was
                  written; any change in either direction fails the run (a
                  drop tightens the baseline in the same change), and a run
                  at another hit is refused.
  bios_stack      [low, 0x200000): the BIOS's call frames under the guest's
                  initial stack pointer, which psx_bios.c never pushes.
  load_in_flight  computed, never listed: zero on the reference, non-zero on
                  openpete, inside a CdRead destination openpete's log shows
                  issued before the halt. Any such byte means the reference
                  is mid-load at this anchor; the run does not PASS.
  <named>         [[expect]] rows: class, start, end, name, reason, owner.
  unexpected      everything else; any such range fails the run.

Exit 0: no unexpected byte, no populated byte, no load_in_flight byte, the
ratchet counts equal to the baseline, and the extent the listed classes
blind (kernel + psyq_internal + bios_stack + [[expect]] spans) under 25% of
RAM. Exit 2 otherwise (first offending range named). Exit 64 on a missing
input.
"""
import argparse
import bisect
import hashlib
import re
import sys

try:
    import tomli
except ImportError:  # pragma: no cover
    tomli = None

RAM_SIZE = 0x200000
RAM_MASK = 0x1FFFFF
CAP = 0.25


def die(msg, code=64):
    print(f"[ram-diff] {msg}", file=sys.stderr)
    sys.exit(code)


def load_map(path):
    """psx.map -> sorted (offset, symbol) for symbol lookup by address."""
    syms = []
    if not path:
        return syms
    try:
        with open(path, errors="replace") as f:
            for line in f:
                m = re.match(r"\s+0x00000000(8[0-9a-f]{7})\s+(\S+)\s*$", line)
                if m:
                    syms.append((int(m.group(1), 16) & RAM_MASK, m.group(2)))
    except OSError:
        return []
    syms.sort()
    return syms


def owner(syms, off):
    if not syms:
        return "?"
    keys = [a for a, _ in syms]
    i = bisect.bisect_right(keys, off) - 1
    if i < 0:
        return "?"
    a, n = syms[i]
    return f"{n}+0x{off - a:X}"


def cd_reads_before_halt(log_path):
    """CdRead destinations (offset, length) openpete issued before the halt."""
    reads = []
    if not log_path:
        return reads
    with open(log_path, errors="replace") as f:
        for line in f:
            if "halt-at-pc: halted" in line:
                break
            m = re.search(r"CdRead sectors=(\d+) buf=0x([0-9A-Fa-f]+)", line)
            if m:
                reads.append((int(m.group(2), 16) & RAM_MASK,
                              int(m.group(1)) * 2048))
    return reads


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ref", required=True, help="reference (DuckStation) 2 MB dump")
    ap.add_argument("--op", required=True, help="openpete 2 MB dump")
    ap.add_argument("--expect", required=True, help="games/<game>/manifest/ram_diff_expect.toml")
    ap.add_argument("--anchor", required=True,
                    help="anchor key for the psyq_internal ratchet: first, loop, or the 0xHEX spelling")
    ap.add_argument("--hit", type=int, default=1, help="openpete hit the dump was taken at")
    ap.add_argument("--op-log", help="openpete log of the halt run (CdRead destinations)")
    ap.add_argument("--map", help="decomp psx.map for symbol names")
    ap.add_argument("--regions", help="gameplay_state_regions.toml: report unexpected "
                                      "ranges intersecting a direct region (calibration)")
    ap.add_argument("--gap", type=int, default=16)
    ap.add_argument("--list", type=int, default=40, help="unexpected ranges to print")
    args = ap.parse_args()

    if tomli is None:
        die("python module tomli missing")
    try:
        ref = open(args.ref, "rb").read()
        op = open(args.op, "rb").read()
    except OSError as e:
        die(f"cannot read dump: {e}")
    if len(ref) != RAM_SIZE or len(op) != RAM_SIZE:
        die(f"dump sizes {len(ref)} / {len(op)} != {RAM_SIZE}")
    with open(args.expect, "rb") as f:
        expect = tomli.load(f)

    def span(r):
        s = r["start"] & RAM_MASK
        return s, s + (r["length"] if "length" in r else (r["end"] & RAM_MASK) - s)

    populated = [(*span(r), r.get("name", "")) for r in expect.get("populated", [])]
    psyq = [span(r) for r in expect.get("psyq_internal", [])]
    stack_low = expect.get("bios_stack", {}).get("low", RAM_SIZE) & RAM_MASK
    named = [(*span(r), r) for r in expect.get("expect", [])]
    baseline = expect.get("psyq_internal_baseline", {}).get(args.anchor)
    reads = cd_reads_before_halt(args.op_log)
    syms = load_map(args.map)

    # Per-byte classification; ranges are formed inside a class afterwards,
    # so a merged range can never carry a byte across a class boundary.
    reads_sorted = sorted(reads)

    def in_flight_byte(k):
        if ref[k] != 0 or op[k] == 0:
            return False
        return any(s <= k < s + n for s, n in reads_sorted)

    def classify(k):
        if any(lo <= k < hi for lo, hi, _ in populated):
            return "populated"
        if k < 0x10000:
            return "kernel"
        if any(lo <= k < hi for lo, hi in psyq):
            return "psyq_internal"
        if k >= stack_low:
            return "bios_stack"
        if in_flight_byte(k):
            return "load_in_flight"
        for lo, hi, row in named:
            if lo <= k < hi:
                return row["class"]
        return "unexpected"

    by_class = {}
    for k in range(RAM_SIZE):
        if ref[k] == op[k]:
            continue
        cls = classify(k)
        rs = by_class.setdefault(cls, [])
        if rs and k - (rs[-1][1] - 1) <= args.gap:
            rs[-1] = (rs[-1][0], k + 1, rs[-1][2] + 1)
        else:
            rs.append((k, k + 1, 1))

    fixed = ("populated", "kernel", "psyq_internal", "bios_stack", "load_in_flight", "unexpected")
    classes = {c: by_class.get(c, []) for c in fixed}
    named_hits = {c: r for c, r in by_class.items() if c not in fixed}
    ranges = [r for rs in by_class.values() for r in rs]
    n_bytes = sum(c for _, _, c in ranges)
    n_span = sum(j - i for i, j, _ in ranges)
    print(f"[ram-diff] {len(ranges)} ranges, {n_bytes} differing bytes ({n_span} span), "
          f"gap {args.gap}; CdReads before halt: {len(reads)}; anchor {args.anchor or '-'}")

    def line(name, rs):
        by = sum(c for _, _, c in rs)
        sp = sum(j - i for i, j, _ in rs)
        print(f"[ram-diff]   {name:16s} ranges={len(rs):5d} bytes={by:8d} span={sp:8d}")
        return by, len(rs)

    reasons = []
    pop_bytes, _ = line("populated", classes["populated"])
    line("kernel", classes["kernel"])
    psyq_bytes, psyq_ranges = line("psyq_internal", classes["psyq_internal"])
    line("bios_stack", classes["bios_stack"])
    flight_bytes, _ = line("load_in_flight", classes["load_in_flight"])
    for cls, rows in sorted(named_hits.items()):
        line(cls, rows)
    unexp_bytes, _ = line("unexpected", classes["unexpected"])

    # The cap is on the extent the listed classes blind, not on what differs
    # today: an expectation set covering most of live RAM cannot see a change.
    blind = bytearray(RAM_SIZE)
    blind[0:0x10000] = b"\x01" * 0x10000
    for lo, hi in psyq:
        blind[lo:hi] = b"\x01" * (hi - lo)
    blind[stack_low:RAM_SIZE] = b"\x01" * (RAM_SIZE - stack_low)
    for lo, hi, _ in named:
        blind[lo:hi] = b"\x01" * (hi - lo)
    blinded = sum(blind)
    frac = blinded / RAM_SIZE
    expected_bytes = n_bytes - unexp_bytes - pop_bytes - flight_bytes
    print(f"[ram-diff]   expected classes blind {blinded} bytes = {frac * 100:.3f}% of RAM "
          f"(cap {CAP * 100:.0f}%); {expected_bytes} differing bytes fall inside them")
    if frac > CAP:
        reasons.append(f"listed classes blind {frac * 100:.1f}% of RAM (cap {CAP * 100:.0f}%)")
    if flight_bytes:
        reasons.append(f"{flight_bytes} bytes still in flight on the reference: the anchor sits "
                       f"inside a load; choose a later hit")
    # The ratchet compares counts and a digest of which bytes differ, so a
    # byte moving inside the class at constant counts is a change too.
    psyq_offsets = [k for k in range(RAM_SIZE) if ref[k] != op[k]
                    and any(lo <= k < hi for lo, hi in psyq)
                    and not any(lo <= k < hi for lo, hi, _ in populated)]
    digest = hashlib.sha256(",".join(str(k) for k in psyq_offsets).encode()).hexdigest()[:16]
    if baseline is not None:
        b_bytes, b_ranges = baseline.get("bytes", 0), baseline.get("ranges", 0)
        b_digest, b_hit = baseline.get("digest", ""), baseline.get("hit", 1)
        print(f"[ram-diff]   psyq_internal ratchet ({args.anchor}, hit {args.hit}): measured "
              f"{psyq_bytes} B / {psyq_ranges} ranges / {digest}, baseline {b_bytes} B / "
              f"{b_ranges} ranges / {b_digest or '-'} at hit {b_hit}")
        if b_hit != args.hit:
            reasons.append(f"baseline for anchor {args.anchor} was measured at hit {b_hit}, "
                           f"this run is hit {args.hit}")
        elif psyq_bytes != b_bytes or psyq_ranges != b_ranges or (b_digest and b_digest != digest):
            reasons.append(f"psyq_internal moved from its baseline at anchor {args.anchor}: "
                           f"{psyq_bytes} B / {psyq_ranges} ranges / {digest} vs {b_bytes} B / "
                           f"{b_ranges} ranges / {b_digest} (a drop tightens the baseline in the "
                           f"same change as the fix)")
    else:
        print(f"[ram-diff]   psyq_internal ratchet ({args.anchor}, hit {args.hit}): no baseline "
              f"recorded (measured {psyq_bytes} B / {psyq_ranges} ranges / {digest})")
        reasons.append(f"no psyq_internal baseline for anchor {args.anchor}")

    if args.regions:
        with open(args.regions, "rb") as f:
            regs = tomli.load(f).get("region", [])
        direct = [((r["start"] & RAM_MASK), (r["start"] & RAM_MASK) + r["length"], r["name"])
                  for r in regs if r.get("kind", "direct") == "direct"]
        hits = [(i, j, nm) for i, j, _ in classes["unexpected"] + classes["populated"]
                for lo, hi, nm in direct if i < hi and j > lo]
        print(f"[ram-diff]   unexpected ranges intersecting a direct manifest region: {len(hits)}")
        for i, j, nm in hits[:10]:
            print(f"[ram-diff]     0x{i:06X}-0x{j:06X} {nm}")

    for i, j, c in classes["populated"]:
        nm = next(n for lo, hi, n in populated if lo <= i < hi)
        print(f"[ram-diff]   populated mirror differs 0x{i:06X}-0x{j:06X} {c:5d} B  {nm}")
    for i, j, c in classes["unexpected"][:args.list]:
        z_ref = sum(1 for k in range(i, j) if ref[k] == 0)
        z_op = sum(1 for k in range(i, j) if op[k] == 0)
        print(f"[ram-diff]   unexpected 0x{i:06X}-0x{j:06X} {c:5d} B (span {j - i:5d})  "
              f"{owner(syms, i):40s} zero: ref {z_ref} op {z_op}")
    if len(classes["unexpected"]) > args.list:
        print(f"[ram-diff]   ... {len(classes['unexpected']) - args.list} more")

    if classes["populated"]:
        i, j, _ = classes["populated"][0]
        reasons.insert(0, f"{len(classes['populated'])} populated mirror range(s) differ; first "
                          f"0x{i:06X}-0x{j:06X}")
    if classes["unexpected"]:
        i, j, _ = classes["unexpected"][0]
        reasons.insert(0, f"{len(classes['unexpected'])} unexpected range(s); first "
                          f"0x{i:06X}-0x{j:06X} ({owner(syms, i)})")
    if reasons:
        for r in reasons:
            print(f"[ram-diff] FAIL: {r}")
        sys.exit(2)
    print("[ram-diff] PASS: every differing range is in an expected class")
    sys.exit(0)


if __name__ == "__main__":
    main()
