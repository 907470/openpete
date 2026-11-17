#!/usr/bin/env python3
"""livepad.py - timed pad streams for the live-pad harness (tests/harness/check_livepad.sh).

A stream is the input of one harness row: text, one event per line,
`<t> <control> <value>`, `#` starting a comment. t is a time on openpete's
VBL clock (openpete_vbl, fractional: an event at n - 1 + f reaches the
guest at VBL n), control an SDL gamepad axis name (leftx lefty rightx righty
lefttrigger righttrigger) with a value in -32768..32767, or an SDL gamepad
button name (a b x y back start leftshoulder rightshoulder leftstick
rightstick dpup dpdown dpleft dpright) with 0 or 1. openpete reads it through
OPENPETE_PAD_STREAM (core/input/host_input.c, the first virtual pad).

Every stick value is a member of STICK_VALUES: the multiples of 256 with
magnitude at most 24320, and the two full deflections 32767 and -32768. On
that set both of openpete's stick responses, linear `v / 256 + 128` and
console (the reference's measured live response,
tests/unit/data/stick-response/console.runs), are injective, so the
stick byte the guest read under either one names the axis value that
produced it, and `phys` hands the reference the stream's own values.

Subcommands:
  gen GAME OUT        write the committed stream of GAME (deterministic)
  check STREAM        validate a stream: order, controls, stick values
  phys IN OUT MAP     a movie openpete recorded (--bk2-record) under the
                      stick response MAP (linear|console) to the movie
                      the reference plays with -bk2-phys: player 1's stick
                      bytes become the axis values that produced them
  report ...          the harness verdict (see `report --help`)
  divergences LOG     a verify log's divergent records per region, in the
                      exclusion file's form (games/<game>/livepad-exclusions.txt)
"""

from __future__ import annotations

import argparse
import math
import os
import sys
import zipfile

AXES = ("leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger")
STICKS = AXES[:4]
BUTTONS = ("a", "b", "x", "y", "back", "start", "leftshoulder", "rightshoulder",
           "leftstick", "rightstick", "dpup", "dpdown", "dpleft", "dpright")
STICK_VALUES = frozenset(list(range(-24320, 24321, 256)) + [32767, -32768])


def linear_byte(v: int) -> int:
    """openpete's built-in live decode (PsyCross PsyX_Pad_UpdateGameControllerInput): v / 256 + 128, C division."""
    return int(v / 256) + 128


MEASURED = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "tests", "unit", "data", "stick-response", "console.runs")
_measured: dict[int, int] = {}


def measured_byte(v: int) -> int:
    """The reference's live stick byte for SDL axis value v: a lookup in the measured table (rows `first last byte`)."""
    if not _measured:
        with open(MEASURED) as f:
            for line in f:
                if line.startswith("#") or not line.strip():
                    continue
                lo, hi, b = (int(x) for x in line.split())
                for x in range(lo, hi + 1):
                    _measured[x] = b
        if len(_measured) != 65536:
            raise SystemExit(f"livepad: {MEASURED} covers {len(_measured)} of 65536 axis values")
    return _measured[v]


MAPPINGS = {"linear": linear_byte, "console": measured_byte}


def inverse(mapping: str) -> dict[int, int]:
    """Byte -> the one STICK_VALUES member that maps to it; refuses a non-injective mapping."""
    fn = MAPPINGS[mapping]
    inv: dict[int, int] = {}
    for v in sorted(STICK_VALUES):
        b = fn(v)
        if b in inv:
            raise SystemExit(f"livepad: {mapping} maps {inv[b]} and {v} to byte {b}; STICK_VALUES is not injective")
        inv[b] = v
    return inv


# ---- streams ----------------------------------------------------------------

def read_stream(path: str) -> list[tuple[float, str, int]]:
    events = []
    with open(path) as f:
        for n, line in enumerate(f, 1):
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            t, name, v = line.split()
            events.append((float(t), name, int(v), n))
    out, last = [], -math.inf
    for t, name, v, n in events:
        if t < last:
            raise SystemExit(f"{path}:{n}: t {t} before {last}")
        last = t
        if name in STICKS:
            if v not in STICK_VALUES:
                raise SystemExit(f"{path}:{n}: {name} {v} is not a stick value (multiple of 256 within 24320, or full)")
        elif name in AXES:
            if not 0 <= v <= 32767:
                raise SystemExit(f"{path}:{n}: {name} {v} out of 0..32767")
        elif name in BUTTONS:
            if v not in (0, 1):
                raise SystemExit(f"{path}:{n}: {name} {v} is not 0|1")
        else:
            raise SystemExit(f"{path}:{n}: unknown control {name}")
        out.append((t, name, v))
    return out


class Gen:
    """Builds a stream; times are VBLs, every event's fraction is drawn from a fixed cycle."""

    FRACS = (0.13, 0.5, 0.87, 0.31, 0.0, 0.69, 0.05, 0.94, 0.42)

    def __init__(self) -> None:
        self.ev: list[tuple[float, str, int]] = []
        self.k = 0

    def frac(self) -> float:
        self.k += 1
        return self.FRACS[self.k % len(self.FRACS)]

    def at(self, t: float, name: str, v: int) -> None:
        self.ev.append((t, name, v))

    def tap(self, vbl: int, name: str, hold: int = 8) -> None:
        f = self.frac()
        self.at(vbl + f, name, 1)
        self.at(vbl + hold + self.frac(), name, 0)

    def stick(self, t: float, x: int | None = None, y: int | None = None) -> None:
        if x is not None:
            self.at(t, "leftx", q(x))
        if y is not None:
            self.at(t, "lefty", q(y))

    def lines(self) -> list[str]:
        self.ev.sort(key=lambda e: e[0])
        return [f"{t:.3f} {n} {v}" for t, n, v in self.ev]


def q(v: float) -> int:
    """Nearest STICK_VALUES member at or inside v's magnitude (full deflection kept)."""
    v = int(round(v))
    if v >= 32767:
        return 32767
    if v <= -32768:
        return -32768
    v = max(-24320, min(24320, v))
    return int(v / 256) * 256


# Menu prefixes: the button taps that start a new game, at the VBLs the
# committed corpus movies press them (openpete_vbl; Spyro 2 and 3 count the
# 573 fast-boot VBLs), each at a sub-VBL fraction. Then the first VBL the
# player controls Spyro in the first level.
PREFIX = {
    # games/spyro-1/bk2/start-new-game.bk2: its row r is read at
    # openpete_vbl r + 436 (the first pad read follows 436 boot VBLs). The
    # intro plays out; Spyro is placed in Artisans at VBL 4709 and the
    # level-entry fly-in ends before 4900.
    "spyro-1": ([(r + 436, "a") for r in (327, 380, 485, 526)], 4900),
    # games/spyro-2/bk2/start-new-game-cutscene-skip.bk2
    "spyro-2": ([(v, "a") for v in (1157, 1387, 1514, 1625, 1646, 1684, 1726, 1837, 1872, 1911, 1946)], 2340),
    # games/spyro-3/bk2/new-game-title.bk2, then Cross every 30 VBLs through
    # the opening dialogue
    "spyro-3": ([(2046, "a"), (2146, "a"), (2206, "dpright"), (2246, "a")]
                + [(v, "a") for v in range(2573, 3264, 30)], 3440),
}


def gameplay(g: Gen, t0: int) -> int:
    """Analog play from VBL t0: sweeps, partial holds, diagonals, a circle, flicks inside one VBL, d-pad and buttons."""
    t = t0
    g.stick(t + 0.5, 0, 0)
    t += 30
    # Sweep right to 24320 over 80 VBLs, an event every 2 VBLs at a fraction.
    for k in range(41):
        g.stick(t + 2 * k + g.frac(), x=k * 608)
    t += 84
    for x in (12288, 6144, 18432, 32767):
        g.stick(t + g.frac(), x=x)
        t += 50
    # Sweep left to -24320 over 100 VBLs, jumping on the way.
    for k in range(51):
        g.stick(t + 2 * k + g.frac(), x=24320 - k * 973)
    g.tap(t + 30, "a", 6)
    g.tap(t + 70, "a", 20)
    t += 104
    for x in (-9216, -32768, -512):
        g.stick(t + g.frac(), x=x)
        t += 50
    # Forward (negative y) sweep and hold, with a glide (hold Cross).
    g.stick(t + g.frac(), x=0)
    for k in range(41):
        g.stick(t + 2 * k + g.frac(), y=-k * 608)
    t += 84
    g.stick(t + g.frac(), y=-12288)
    g.tap(t + 10, "a", 4)
    g.tap(t + 20, "a", 40)
    t += 90
    # Diagonals at partial and full deflection.
    for x, y, hold in ((17152, -17152, 90), (-17152, -17152, 90), (-17152, 17152, 60),
                       (17152, 17152, 60), (8704, -20480, 90), (32767, -32768, 40),
                       (-32768, 32767, 40)):
        g.stick(t + g.frac(), x, y)
        t += hold
    # Two loops of a circle of radius 20480, an event every 1.6 VBLs.
    n = 120
    for k in range(n):
        a = 2 * math.pi * k / 60
        g.stick(t + 1.6 * k, 20480 * math.cos(a), 20480 * math.sin(a))
    t += int(1.6 * n) + 2
    # Charge (Square) with the stick held, then flame (Circle) taps.
    g.stick(t + g.frac(), 0, -24320)
    g.tap(t + 5, "x", 60)
    t += 80
    g.stick(t + g.frac(), 4096, -2048)
    for k in range(4):
        g.tap(t + 15 * k, "b", 3)
    t += 70
    # Flicks inside one VBL: full deflection set and released before the
    # sample, which the guest never reads; then near-centre values.
    g.stick(t + 0.5, 0, 0)
    for k in range(5):
        g.at(t + 10 * k + 2.2, "leftx", 32767)
        g.at(t + 10 * k + 2.6, "leftx", 0)
    t += 60
    for v in (256, -256, 512, -768, 1280, -2304, 3328, 0):
        g.stick(t + g.frac(), x=v, y=-v)
        t += 20
    # Events on VBL boundaries (t a whole number reaches the guest a VBL later).
    for k in range(6):
        g.at(float(t + 4 * k), "leftx", q(4096 * (k + 1)))
        g.at(float(t + 4 * k), "lefty", q(-3072 * (k + 1)))
    t += 30
    g.stick(t + 0.5, 0, 0)
    # D-pad walking, then camera shoulders.
    for name, hold in (("dpup", 40), ("dpleft", 20), ("dpup", 30), ("dpright", 25), ("dpdown", 20)):
        g.tap(t, name, hold)
        t += hold + 6
    for name in ("leftshoulder", "rightshoulder", "rightshoulder", "leftshoulder"):
        g.tap(t, name, 10)
        t += 18
    for name in ("lefttrigger", "righttrigger"):
        f = g.frac()
        g.at(t + f, name, 32767)
        g.at(t + 12 + f, name, 0)
        t += 20
    # Right stick, then a slow forward walk at 0.2 deflection with jumps.
    for x, y in ((12288, 0), (-12288, 4096), (0, 0)):
        f = g.frac()
        g.at(t + f, "rightx", q(x))
        g.at(t + f, "righty", q(y))
        t += 25
    g.stick(t + g.frac(), 1536, -6656)
    for k in range(6):
        g.tap(t + 20 + 40 * k, "a", 12)
    t += 260
    g.stick(t + 0.5, 0, 0)
    return t + 60


def gen(game: str, out: str) -> None:
    taps, t0 = PREFIX[game]
    g = Gen()
    for vbl, name in taps:
        g.tap(vbl, name)
    gameplay(g, t0)
    body = g.lines()
    with open(out, "w") as f:
        f.write(f"# livepad stream {game}: written by tools/livepad.py gen {game}\n"
                f"# t (openpete_vbl, fractional) control value; {len(body)} events, last at {g.ev[-1][0]:.3f}\n")
        f.write("\n".join(body) + "\n")
    read_stream(out)


# ---- movies -------------------------------------------------------------------

def phys(src: str, dst: str, mapping: str) -> None:
    """Rewrite player 1's stick bytes of a recorded movie as the axis values that produced them."""
    inv = inverse(mapping)
    rows = off = 0
    with zipfile.ZipFile(src) as zi, zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zo:
        for it in zi.infolist():
            data = zi.read(it.filename)
            if it.filename == "Input Log.txt":
                out = []
                for n, line in enumerate(data.decode().splitlines(keepends=True), 1):
                    if line.startswith("|") and not line.startswith("|["):
                        parts = line.split("|")
                        vals = parts[2].split(",")
                        for a in range(4):
                            b = int(vals[a])
                            if b not in inv:
                                raise SystemExit(f"livepad phys: {src} row {n}: stick byte {b} has no "
                                                 f"STICK_VALUES preimage under {mapping}; the run used another mapping")
                            vals[a] = "%6d" % inv[b]
                        off += inv[int(parts[2].split(",")[0])] != 0 or inv[int(parts[2].split(",")[1])] != 0
                        parts[2] = ",".join(vals)
                        line = "|".join(parts)
                        rows += 1
                    out.append(line)
                data = "".join(out).encode()
            zo.writestr(it, data)
    print(f"livepad phys: rows={rows} off_centre={off} mapping={mapping}")


# ---- report -------------------------------------------------------------------

def read_words(path: str) -> dict[int, tuple[int, ...]]:
    out = {}
    with open(path) as f:
        for line in f:
            if line.startswith(("#", "psxtrace-words")):
                continue
            v = [int(x) for x in line.split()]
            out[v[0]] = tuple(v[2:])
    return out


def parse_runs(text: str) -> set[int]:
    """`a-b,c,...` to the set of records it names."""
    out: set[int] = set()
    for part in text.split(","):
        lo, _, hi = part.partition("-")
        out.update(range(int(lo), int(hi or lo) + 1))
    return out


def format_runs(recs: set[int]) -> str:
    """The set of records as `a-b,c,...`, ascending."""
    r = sorted(recs)
    runs, start, prev = [], r[0], r[0]
    for v in r[1:]:
        if v != prev + 1:
            runs.append((start, prev))
            start = v
        prev = v
    runs.append((start, prev))
    return ",".join(f"{a}-{b}" if a != b else f"{a}" for a, b in runs)


def read_exclusions(path: str | None) -> list[tuple[str, set[int], str]]:
    """Lines `region records reason...`, records `a-b,c,...`: a region's divergence at exactly those records is listed, not failed."""
    out = []
    if not path:
        return out
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            region, recs, reason = line.split(None, 2)
            out.append((region, parse_runs(recs), reason))
    return out


def read_divergences(log: str) -> dict[str, set[int]]:
    """Region -> records at which the verify run logged it divergent (OPENPETE_PSXTRACE_DUMP rows)."""
    div: dict[str, set[int]] = {}
    with open(log, errors="replace") as f:
        for line in f:
            i = line.find("psxtrace: frame=")
            if i < 0 or " region=" not in line:
                continue
            rec = int(line[i + len("psxtrace: frame="):].split()[0])
            name = line[line.find("(", i) + 1:line.find(")", i)]
            div.setdefault(name, set()).add(rec)
    return div


def report(a: argparse.Namespace) -> int:
    ref, op = read_words(a.ref_words), read_words(a.op_words)
    recs = sorted(set(ref) & set(op))
    if not recs:
        print("[livepad] position: no record in both .words files; VACUOUS")
        return 3
    dist = [(r, math.dist(ref[r], op[r])) for r in recs]
    differing = [(r, d) for r, d in dist if d > 0]
    print(f"[livepad] position: {len(recs)} records compared (reference {len(ref)}, openpete {len(op)}); "
          f"{len(differing)} differ; first {differing[0][0] if differing else '-'}; "
          f"max {max(d for _, d in dist):.0f}")
    step = max(1, len(dist) // 10)
    print("[livepad] position error over the run (record: distance):",
          " ".join(f"{r}:{d:.0f}" for r, d in dist[::step]))
    excl = read_exclusions(a.exclude)
    div = read_divergences(a.log)
    listed = unlisted = 0
    first_unlisted: tuple[int, str] | None = None
    for name, rs in div.items():
        allowed: set[int] = set()
        for region, xr, _ in excl:
            if region == name:
                allowed |= xr
        listed += len(rs & allowed)
        extra = rs - allowed
        unlisted += len(extra)
        if extra and (first_unlisted is None or min(extra) < first_unlisted[0]):
            first_unlisted = (min(extra), name)
    for region, xr, reason in excl:
        hit = div.get(region, set()) & xr
        print(f"[livepad] listed exclusion: {region} at {len(xr)} record(s) {format_runs(xr)[:60]}"
              f"{'...' if len(format_runs(xr)) > 60 else ''}: {reason}")
        if len(hit) != len(xr):
            print(f"[livepad] exclusion {region}: {len(xr) - len(hit)} listed record(s) did not diverge (stale)")
    print(f"[livepad] regions: {listed + unlisted} divergent region-record(s), {listed} listed, {unlisted} unlisted")
    rc = 0
    if first_unlisted:
        print(f"[livepad] DIVERGENCE: first unlisted region {first_unlisted[1]} at record {first_unlisted[0]}")
        rc = 2
    if differing and rc == 0:
        print(f"[livepad] DIVERGENCE: Spyro's position differs from record {differing[0][0]}")
        rc = 2
    return rc


def divergences(a: argparse.Namespace) -> int:
    """Print a verify log's divergence sets in the exclusion file's form, a reason to be written for each."""
    for name, rs in sorted(read_divergences(a.log).items(), key=lambda kv: min(kv[1])):
        print(f"{name} {format_runs(rs)} REASON")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sp = ap.add_subparsers(dest="cmd", required=True)
    p = sp.add_parser("gen"); p.add_argument("game", choices=sorted(PREFIX)); p.add_argument("out")
    p = sp.add_parser("check"); p.add_argument("stream")
    p = sp.add_parser("phys"); p.add_argument("src"); p.add_argument("dst"); p.add_argument("mapping", choices=sorted(MAPPINGS))
    p = sp.add_parser("report", help="position error and region verdict of one harness run")
    p.add_argument("--ref-words", required=True, help="the reference capture's <trace>.words")
    p.add_argument("--op-words", required=True, help="openpete's OPENPETE_PSXTRACE_WORDS output")
    p.add_argument("--log", required=True, help="the verify run's log (OPENPETE_PSXTRACE_DUMP=1)")
    p.add_argument("--exclude", help="the game's listed exclusions")
    p = sp.add_parser("divergences", help="a verify log's divergent records per region, in the exclusion form")
    p.add_argument("log")
    a = ap.parse_args()
    if a.cmd == "gen":
        gen(a.game, a.out)
    elif a.cmd == "check":
        ev = read_stream(a.stream)
        mid = sum(1 for t, _, _ in ev if t != math.floor(t))
        print(f"livepad check: {len(ev)} events, {mid} mid-VBL, last at {ev[-1][0]:.3f}")
    elif a.cmd == "phys":
        phys(a.src, a.dst, a.mapping)
    elif a.cmd == "divergences":
        return divergences(a)
    else:
        return report(a)
    return 0


if __name__ == "__main__":
    sys.exit(main())
