#!/usr/bin/env python3
"""disc_boot_order.py - per-game disc boot-order profiles for the disc cache
loader, and the loader simulation that measures them.

A profile (games/<game>/manifest/disc_boot_order.toml) is the ordered list
of sector ranges a fresh boot reads, each with the VBL it is first needed,
from boot through the first level load. The disc cache loader
(core/psx/disc_cache.c) reads the profile's ranges first, then the rest of
the image in file order; a chunk a game or XA read missed on goes ahead of
both.

Subcommands:

  generate --trace T --game G --end-vbl N --out TOML
      Profile from an OPENPETE_DISC_TRACE file. Rows kept: the game read
      paths (libcd, xa, exe, extract, wad; the paths that steer the loader)
      with 0 < vbl <= N. A VBL 0 read (the EXE, read as the cache attaches)
      precedes any loader progress and is left out. Rows are ordered by VBL,
      then by their order in the trace (one reading thread writes its rows
      in read order); sectors already listed are dropped and a range that
      continues the previous one merges into it. Only VBL stamps enter; the
      ns column (host clock) is ignored. Same trace, same file, byte for
      byte.

  header --manifest TOML --out H
      The C table the game compiles (games/common/disc_boot_order.c).
      Refuses a malformed profile: wrong format number, a range that is not
      three non-negative integers, a zero count, a range overlapping an
      earlier one, VBLs out of order, or a VBL past window_end_vbl.

  simulate --trace T --manifest TOML [--json]
      The loader over the trace's reads on the NTSC VBL clock
        (2250000/37609 Hz) at 10, 25, 50, 100, 200 and 500 MB/s (1 MB =
      2^20 bytes): file order, profile order, and profile order loaded in
      whole chunks (the profile's chunks in order, deduplicated).

  check --game G [--fixture-dir D] [--manifest-dir D]
      The ctest assertion for one game (exit 0 PASS, 3 VACUOUS, 1 FAIL).

Simulation model, mirroring the loader:
  - The device moves B bytes per second of VBL time from VBL 0. The loader
    and the direct reads share it: an account A holds every byte the loader
    has read plus every byte read directly; a loader read of L bytes adds L
    and completes (its sectors become resident) at the first time t with
    B * t >= A. OPENPETE_DISC_CACHE_BPS throttles the live loader the same
    way.
  - One loader read is one run of missing sectors, chosen in this order:
    the chunk of the last game-path miss (libcd, extract, wad; the EXE
    read steers nothing), the chunk of the last XA miss,
    the next profile range, the next chunk in file order. A chunk is
    DC_CHUNK_SECTORS sectors (8 MB rounded down to whole sectors).
  - A libcd or XA row is read sector by sector (PsyCross reads one sector
    per call): each non-resident sector is a miss, read directly, and its
    chunk becomes the steer target. Any other row is one read: a miss when
    any of its sectors is not resident, charging all of them.
  - A game miss counts against the order; a VBL 0 read (always direct) and
    a host-path read (woah NSF; never in the profile, never steering) are
    reported apart.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

try:
    import tomllib  # py 3.11+
except ModuleNotFoundError:  # pragma: no cover
    import tomli as tomllib  # type: ignore

SECTOR = 2352
CHUNK_SECTORS = (1 << 23) // SECTOR          # matches DC_CHUNK_SECTORS
VBL_HZ = 2250000 / 37609
MB = 1 << 20
BANDWIDTHS_MB = (10, 25, 50, 100, 200, 500)
FORMAT = 1

GAME_TAGS = ("libcd", "xa", "exe", "extract", "wad")   # steer the loader
SECTOR_TAGS = ("libcd", "xa")                           # PsyCross reads per sector
STEER_SLOT = {"libcd": 0, "extract": 0, "wad": 0, "xa": 1}   # disc_cache.c steer_slot
KNOWN_TAGS = ("none", "libcd", "xa", "exe", "serial", "extract", "woah", "sfx",
              "wad", "ingest", "cache-load", "iso", "mod", "font")

class ProfileError(ValueError):
    """A malformed profile."""


def read_trace(path):
    """Parse an OPENPETE_DISC_TRACE file into row dicts, in file order."""
    rows = []
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            p = line.split("\t")
            if len(p) != 8:
                raise ValueError(f"{path}:{n}: {len(p)} fields, want 8")
            if p[2] not in KNOWN_TAGS:
                raise ValueError(f"{path}:{n}: unknown tag {p[2]!r}")
            rows.append({"line": n, "vbl": int(p[1]), "tag": p[2], "lba": int(p[3]),
                         "count": int(p[4]), "result": p[5], "offset": int(p[6]),
                         "length": int(p[7])})
    return rows


def image_sectors(rows):
    """Image size in sectors from the cache-load rows (the loader reads every byte)."""
    end = max((r["offset"] + r["length"] for r in rows if r["tag"] == "cache-load"), default=0)
    if end == 0:
        raise ValueError("trace has no cache-load rows; the image size is unknown")
    return (end + SECTOR - 1) // SECTOR


def generate(rows, end_vbl):
    """Profile ranges [lba, count, first_vbl, tag] from trace rows."""
    keep = [r for r in rows if r["tag"] in GAME_TAGS and 0 < r["vbl"] <= end_vbl]
    keep.sort(key=lambda r: (r["vbl"], r["line"]))
    seen = set()
    out = []
    for r in keep:
        s = r["lba"]
        e = r["lba"] + r["count"]
        while s < e:
            if s in seen:
                s += 1
                continue
            t = s
            while t < e and t not in seen:
                seen.add(t)
                t += 1
            if out and out[-1][0] + out[-1][1] == s:
                out[-1][1] += t - s
            else:
                out.append([s, t - s, r["vbl"], r["tag"]])
            s = t
    return out


def format_profile(game, end_vbl, ranges):
    """The profile's TOML text."""
    lines = [
        f"# Disc boot-order profile for {game}: the disc cache loader reads these",
        "# sector ranges first, in order, then the rest of the image in file order.",
        "# Generated by tools/disc_boot_order.py generate from an OPENPETE_DISC_TRACE",
        "# boot capture; regenerate, never edit.",
        f"format = {FORMAT}",
        f'game = "{game}"',
        f"sector_bytes = {SECTOR}",
        f"window_end_vbl = {end_vbl}",
        "# [lba_first, lba_count, first_vbl]  first reader",
        "ranges = [",
    ]
    for lba, cnt, vbl, tag in ranges:
        lines.append(f"  [{lba}, {cnt}, {vbl}],  # {tag}")
    lines.append("]")
    return "\n".join(lines) + "\n"


def parse_profile(text):
    """Validate profile text; return (game, window_end_vbl, [(lba, count, vbl)])."""
    try:
        d = tomllib.loads(text)
    except tomllib.TOMLDecodeError as e:
        raise ProfileError(f"not TOML: {e}") from e
    if d.get("format") != FORMAT:
        raise ProfileError(f"format {d.get('format')!r}, want {FORMAT}")
    if d.get("sector_bytes") != SECTOR:
        raise ProfileError(f"sector_bytes {d.get('sector_bytes')!r}, want {SECTOR}")
    game = d.get("game")
    end = d.get("window_end_vbl")
    if not isinstance(game, str) or not game:
        raise ProfileError("game missing")
    if not isinstance(end, int) or isinstance(end, bool) or end <= 0:
        raise ProfileError("window_end_vbl missing or not a positive integer")
    ranges = d.get("ranges")
    if not isinstance(ranges, list) or not ranges:
        raise ProfileError("ranges missing or empty")
    out = []
    taken = []
    last_vbl = 0
    for i, r in enumerate(ranges):
        if (not isinstance(r, list) or len(r) != 3 or
                any(not isinstance(v, int) or isinstance(v, bool) or v < 0 for v in r)):
            raise ProfileError(f"range {i}: want [lba_first, lba_count, first_vbl] of integers >= 0")
        lba, cnt, vbl = r
        if cnt == 0:
            raise ProfileError(f"range {i}: zero count")
        if lba + cnt > 0xFFFFFFFF:
            raise ProfileError(f"range {i}: past 32-bit LBA")
        if vbl < last_vbl:
            raise ProfileError(f"range {i}: first_vbl {vbl} before the previous range's {last_vbl}")
        if vbl > end:
            raise ProfileError(f"range {i}: first_vbl {vbl} past window_end_vbl {end}")
        for a, b in taken:
            if lba < b and a < lba + cnt:
                raise ProfileError(f"range {i}: overlaps [{a}, {b})")
        taken.append((lba, lba + cnt))
        last_vbl = vbl
        out.append((lba, cnt, vbl))
    return game, end, out


def format_header(game, ranges):
    """The generated C table for games/common/disc_boot_order.c."""
    lines = [
        f"/* Generated by tools/disc_boot_order.py header from games/{game}/manifest/",
        " * disc_boot_order.toml; regenerate, never edit. */",
        "#ifndef OPENPETE_DISC_BOOT_ORDER_GEN_H",
        "#define OPENPETE_DISC_BOOT_ORDER_GEN_H",
        '#include "openpete_disc_cache.h"',
        f"#define OPENPETE_DISC_BOOT_ORDER_COUNT {len(ranges)}u",
        "static const openpete_disc_range_t openpete_disc_boot_order_table[] = {",
    ]
    for lba, cnt, _ in ranges:
        lines.append(f"    {{ {lba}u, {cnt}u }},")
    lines += ["};", "#endif", ""]
    return "\n".join(lines)


# --------------------------------------------------------------- simulation

class Loader:
    """The disc cache loader at sector residency (core/psx/disc_cache.c)."""

    def __init__(self, nsec, profile, granularity):
        self.nsec = nsec
        self.res = bytearray(nsec)                # 1 = resident
        self.nchunks = (nsec + CHUNK_SECTORS - 1) // CHUNK_SECTORS
        self.steer = [-1, -1]
        self.seq = 0
        self.prof_at = 0
        if granularity == "chunk":
            # Profile ranges mapped to their chunks, deduplicated, whole chunks.
            chunks = []
            for lba, cnt, _ in profile:
                for c in range(lba // CHUNK_SECTORS, (lba + cnt - 1) // CHUNK_SECTORS + 1):
                    if c not in chunks:
                        chunks.append(c)
            self.profile = [(c * CHUNK_SECTORS, CHUNK_SECTORS) for c in chunks]
        else:
            self.profile = [(lba, cnt) for lba, cnt, _ in profile]

    def chunk_span(self, c):
        """Sectors [lo, hi) of chunk c."""
        return c * CHUNK_SECTORS, min((c + 1) * CHUNK_SECTORS, self.nsec)

    def run_in(self, lo, hi):
        """First run of missing sectors in [lo, hi), or None."""
        hi = min(hi, self.nsec)
        s = lo
        while s < hi and self.res[s]:
            s += 1
        if s >= hi:
            return None
        e = s
        while e < hi and not self.res[e]:
            e += 1
        return s, e

    def next_run(self):
        """The loader's next read, or None when every sector is resident."""
        for k in range(2):
            c = self.steer[k]
            if c >= 0:
                r = self.run_in(*self.chunk_span(c))
                if r:
                    return r
                self.steer[k] = -1
        while self.prof_at < len(self.profile):
            lba, cnt = self.profile[self.prof_at]
            r = self.run_in(lba, lba + cnt)
            if r:
                return r
            self.prof_at += 1
        while self.seq < self.nchunks:
            r = self.run_in(*self.chunk_span(self.seq))
            if r:
                return r
            self.seq += 1
        return None


def simulate(rows, nsec, profile, end_vbl, mbps, order, granularity="sector"):
    """One loader run over the trace's reads; returns the metric dict."""
    bw = mbps * MB
    ld = Loader(nsec, profile if order == "profile" else [], granularity)
    reads = [r for r in rows if r["tag"] != "cache-load" and 0 <= r["vbl"] <= end_vbl]
    reads.sort(key=lambda r: (r["vbl"], r["line"]))
    acct = 0
    op = ld.next_run()
    if op:
        acct += (op[1] - op[0]) * SECTOR
    m = {"game_miss_rows": 0, "game_miss_sectors": 0, "game_direct_bytes": 0,
         "vbl0_direct_bytes": 0, "host_direct_bytes": 0, "host_miss_rows": 0,
         "resident_at_end": 0}

    def advance(t):
        nonlocal op, acct
        while op and acct <= bw * t:
            for s in range(op[0], op[1]):
                ld.res[s] = 1
            op = ld.next_run()
            if op:
                acct += (op[1] - op[0]) * SECTOR

    for r in reads:
        t = r["vbl"] / VBL_HZ
        advance(t)
        secs = range(r["lba"], min(r["lba"] + r["count"], nsec))
        missing = [s for s in secs if not ld.res[s]]
        if not missing:
            continue
        if r["tag"] in SECTOR_TAGS:
            direct = len(missing) * SECTOR
            steer_at = missing[-1]          # each sector's miss steers; the last one stands
        else:
            direct = len(secs) * SECTOR
            steer_at = missing[0]
        acct += direct
        if r["vbl"] == 0:
            m["vbl0_direct_bytes"] += direct
        elif r["tag"] in GAME_TAGS:
            m["game_miss_rows"] += 1
            m["game_miss_sectors"] += len(missing) if r["tag"] in SECTOR_TAGS else r["count"]
            m["game_direct_bytes"] += direct
        else:
            m["host_miss_rows"] += 1
            m["host_direct_bytes"] += direct
        if r["tag"] in STEER_SLOT:
            c = steer_at // CHUNK_SECTORS
            if ld.run_in(*ld.chunk_span(c)):
                ld.steer[STEER_SLOT[r["tag"]]] = c
    advance(end_vbl / VBL_HZ)
    window_s = end_vbl / VBL_HZ
    direct = m["game_direct_bytes"] + m["host_direct_bytes"] + m["vbl0_direct_bytes"]
    m["window_s"] = window_s
    m["loader_mbps_taken"] = direct / window_s / MB
    m["loader_share_pct"] = 100.0 * direct / (bw * window_s)
    m["resident_at_end"] = sum(ld.res) / nsec
    return m


def sim_table(rows, profile, end_vbl):
    """simulate() over every modelled bandwidth, file and profile order."""
    nsec = image_sectors(rows)
    table = []
    for mbps in BANDWIDTHS_MB:
        row = {"mbps": mbps}
        for order in ("file", "profile"):
            row[order] = simulate(rows, nsec, profile, end_vbl, mbps, order)
        row["profile_chunk"] = simulate(rows, nsec, profile, end_vbl, mbps, "profile", "chunk")
        table.append(row)
    return table


def verdict(table):
    """(verdict, lowest passing file-order MB/s, step-below MB/s, reason)."""
    lowest = next((r["mbps"] for r in table if r["file"]["game_miss_rows"] == 0), None)
    if lowest is None:
        return "FAIL", None, None, "file order misses at every modelled bandwidth"
    if lowest == BANDWIDTHS_MB[0]:
        return "VACUOUS", lowest, None, f"file order misses nothing even at {lowest} MB/s"
    below = BANDWIDTHS_MB[BANDWIDTHS_MB.index(lowest) - 1]
    r = next(x for x in table if x["mbps"] == below)
    if r["profile"]["game_miss_rows"] != 0:
        return "FAIL", lowest, below, (f"profile order misses {r['profile']['game_miss_rows']} "
                                       f"read(s) at {below} MB/s")
    return "PASS", lowest, below, (f"at {below} MB/s file order misses "
                                   f"{r['file']['game_miss_rows']} read(s), profile order none")


def print_table(game, table):
    """The simulation table, one line per bandwidth."""
    print(f"[disc-boot-order] {game}: mbps | file: miss_rows miss_sectors direct_MB "
          f"loader_MBps_taken share% | profile: same | profile@chunk: miss_rows")
    for r in table:
        f, p, c = r["file"], r["profile"], r["profile_chunk"]
        print(f"  {r['mbps']:>4} | file {f['game_miss_rows']:>3} {f['game_miss_sectors']:>5} "
              f"{f['game_direct_bytes'] / MB:7.3f} {f['loader_mbps_taken']:7.3f} "
              f"{f['loader_share_pct']:6.2f}% | profile {p['game_miss_rows']:>3} "
              f"{p['game_miss_sectors']:>5} {p['game_direct_bytes'] / MB:7.3f} "
              f"{p['loader_mbps_taken']:7.3f} {p['loader_share_pct']:6.2f}% | "
              f"chunk {c['game_miss_rows']:>3} | vbl0 {f['vbl0_direct_bytes'] / MB:.3f} MB "
              f"host file/profile {f['host_direct_bytes'] / MB:.3f}/"
              f"{p['host_direct_bytes'] / MB:.3f} MB")


def load_profile(path):
    """parse_profile over a file."""
    return parse_profile(Path(path).read_text(encoding="utf-8"))


def main(argv=None):
    """Command-line entry."""
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("generate")
    g.add_argument("--trace", required=True)
    g.add_argument("--game", required=True)
    g.add_argument("--end-vbl", type=int, required=True)
    g.add_argument("--out", required=True)
    h = sub.add_parser("header")
    h.add_argument("--manifest", required=True)
    h.add_argument("--out", required=True)
    s = sub.add_parser("simulate")
    s.add_argument("--trace", required=True)
    s.add_argument("--manifest", required=True)
    s.add_argument("--json", action="store_true")
    c = sub.add_parser("check")
    c.add_argument("--game", required=True)
    c.add_argument("--fixture-dir", default="tests/harness/disc_boot_order")
    c.add_argument("--manifest-dir", default=None)
    a = ap.parse_args(argv)

    if a.cmd == "generate":
        rows = read_trace(a.trace)
        text = format_profile(a.game, a.end_vbl, generate(rows, a.end_vbl))
        parse_profile(text)
        Path(a.out).write_text(text, encoding="utf-8")
        return 0
    if a.cmd == "header":
        try:
            game, _, ranges = load_profile(a.manifest)
        except ProfileError as e:
            print(f"disc_boot_order: {a.manifest}: {e}", file=sys.stderr)
            return 1
        Path(a.out).write_text(format_header(game, ranges), encoding="utf-8")
        return 0
    if a.cmd == "simulate":
        rows = read_trace(a.trace)
        game, end, ranges = load_profile(a.manifest)
        table = sim_table(rows, ranges, end)
        if a.json:
            json.dump(table, sys.stdout, indent=1)
            print()
        else:
            print_table(game, table)
        v, lowest, below, why = verdict(table)
        print(f"[disc-boot-order] {game} file_order_lowest_passing_mbps={lowest} "
              f"step_below_mbps={below} verdict={v} ({why})")
        return 0
    if a.cmd == "check":
        mdir = Path(a.manifest_dir or f"games/{a.game}/manifest")
        trace = Path(a.fixture_dir) / f"{a.game}.tsv"
        manifest = mdir / "disc_boot_order.toml"
        rows = read_trace(trace)
        try:
            game, end, ranges = load_profile(manifest)
        except ProfileError as e:
            print(f"[disc-boot-order] {a.game}: {manifest}: {e} verdict=FAIL")
            return 1
        regen = format_profile(game, end, generate(rows, end))
        if regen != manifest.read_text(encoding="utf-8"):
            print(f"[disc-boot-order] {a.game}: {manifest} differs from generate() over "
                  f"{trace} verdict=FAIL")
            return 1
        table = sim_table(rows, ranges, end)
        print_table(game, table)
        v, lowest, below, why = verdict(table)
        print(f"[disc-boot-order] {game} file_order_lowest_passing_mbps={lowest} "
              f"step_below_mbps={below} verdict={v} ({why})")
        return {"PASS": 0, "VACUOUS": 3}.get(v, 1)
    return 2


if __name__ == "__main__":
    sys.exit(main())
