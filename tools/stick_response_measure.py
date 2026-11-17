#!/usr/bin/env python3
"""stick_response_measure.py - measure the oracle reference's live stick response.

Regenerates tests/unit/data/stick-response/console.runs, the byte the
reference emulator's live input path (regtest -bk2-phys, the path
tests/harness/check_livepad.sh compares against) gives the game for every
SDL axis value, on every stick axis, at its default settings.

Method. A movie (one row per VBL) holds each axis value for two rows after
1500 centred rows; its 16 button columns carry the value's position in the
sweep as an identity code. Spyro 1 boots under it with the pad library's
buffer copy store (0x8006B828) as the record anchor, and -psxtrace-words
reads the port 1 pad buffer (g_PadBuffer, 0x800786A0, 8 bytes) before each
store: the first record of each VBL holds the whole previous poll, its
buttons naming the value polled and its four stick bytes the response. Four
sweeps are taken: ascending with every axis equal, ascending with the axes
paired against the mirrored value (LX = v, LY = -1 - v, RX = -1 - v,
RY = v), descending, and ascending with every value followed by two centred
rows. The table is written only when each sweep saw all 65536 values on all
four axes, no value's samples disagree, every axis of every sweep gives the
same table, and the table is monotonic. A power-on probe (one capture)
holds each axis at values inside and just outside -96..96 in turn, LY
moving opposite to LX, RX starting later and RY never moving, and checks
every poll against the power-on rule the header states.

usage (from the repo root):
  tools/stick_response_measure.py WORKDIR [--out PATH] [--jobs N]
Needs the patched DuckStation test binary (scripts/setup_duckstation.sh) and
the Spyro 1 disc build. About 15 minutes at two jobs.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import subprocess
import sys
import zipfile
from collections import defaultdict

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OUT = os.path.join(REPO, "tests/unit/data/stick-response/console.runs")
PAD_COPY_PC = "0x8006B828"   # Spyro 1: the pad library's buffer copy store
PAD_BUFFER = "0x800786A0"    # Spyro 1: g_PadBuffer (status, type, buttons, RX, RY, LX, LY)
LEAD = 1500
HOLD = 2
# Movie button column i -> PSX pad button bit (Up Down Left Right Select Start
# Square Triangle Circle Cross L1 R1 L2 R2 L3 R3).
COL_TO_PSX = (4, 6, 7, 5, 0, 3, 15, 12, 13, 14, 10, 11, 8, 9, 1, 2)
AXES = ("RX", "RY", "LX", "LY")  # pad buffer bytes 4..7
HEADER = ("MovieVersion BizHawk v2.0\nAuthor openpete\nemuVersion openpete-stick-response-measure\n"
          "Platform PSX\nCore Octoshock\nrerecordCount 0\n")
LOGKEY = ("[Input]\nLogKey:#Disc Select|Open|Close|Reset|#P1 LStick X|P1 LStick Y|P1 RStick X|P1 RStick Y|"
          "P1 Up|P1 Down|P1 Left|P1 Right|P1 Select|P1 Start|P1 Square|P1 Triangle|P1 Circle|P1 Cross|"
          "P1 L1|P1 R1|P1 L2|P1 R2|P1 L3|P1 R3|P1 MODE|\n")
RECIPE = """GOLDEN=""
FRAMES=1
DISC="games/spyro-1/spyro-1/build/SCUS-94228.cue"
MANIFEST="games/spyro-1/manifest/gameplay_state_regions.toml"
ANCHOR_PC={pc}
CAPTURE_ARGS=()
RUN_ARGS=()
"""
SWEEPS = ("same", "cross", "desc", "alt")


def movie_row(lx: int, ly: int, rx: int, ry: int, code: int) -> str:
    btn = "".join("P" if (code >> i) & 1 else "." for i in range(16)) + "."
    return "|    1,...|" + ",".join("%6d" % a for a in (lx, ly, rx, ry)) + "," + btn + "|\n"


def write_movie(path: str, rows: list[str]) -> None:
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("Header.txt", HEADER)
        z.writestr("Input Log.txt", LOGKEY + "".join(rows) + "[/Input]\n")


def sweep_values(kind: str) -> list[int]:
    v = list(range(-32768, 32768))
    return v[::-1] if kind == "desc" else v


def sweep_movie(path: str, kind: str) -> int:
    """Write sweep `kind`; return its row count past the lead."""
    rows = [movie_row(0, 0, 0, 0, 0)] * LEAD
    for k, v in enumerate(sweep_values(kind)):
        if kind == "cross":
            rows += [movie_row(v, -1 - v, -1 - v, v, k)] * HOLD
        else:
            rows += [movie_row(v, v, v, v, k)] * HOLD
        if kind == "alt":
            rows += [movie_row(0, 0, 0, 0, k)] * HOLD
    write_movie(path, rows)
    return len(rows) - LEAD


def capture(work: str, name: str, vbls: int) -> str:
    """Run the reference on WORK/NAME.bk2; return the .words path."""
    recipe = os.path.join(work, "recipe.conf")
    out = os.path.join(work, name + ".trace")
    words = out + ".words"
    if os.path.exists(words):
        os.remove(words)
    cmd = ["scripts/capture_psx_oracle_ds.sh", "--game", "spyro-1", "--recipe", recipe, "--out", out,
           "--frames", str(vbls * 8), "--max-runtime-frames", str(vbls), "--no-card",
           "--bk2", os.path.join(work, name + ".bk2"), "--bk2-phys", "--words", PAD_BUFFER + ":2"]
    with open(os.path.join(work, name + ".log"), "w") as log:
        rc = subprocess.run(cmd, cwd=REPO, stdout=log, stderr=subprocess.STDOUT).returncode
    if rc != 0 or not os.path.exists(words):
        raise SystemExit(f"stick_response_measure: capture {name} failed (rc={rc}); see {work}/{name}.log")
    return words


def code_of(btn: int) -> int:
    pressed = ~btn & 0xFFFF
    return sum(1 << i for i, b in enumerate(COL_TO_PSX) if (pressed >> b) & 1)


def polls(words: str) -> list[tuple[int, int, list[int]]]:
    """(VBL, identity code, [RX, RY, LX, LY]) per VBL: its first record, the previous poll whole."""
    out, prev = [], None
    with open(words) as f:
        for line in f:
            if line.startswith(("#", "psxtrace")):
                continue
            _, vbl, w0, w1 = (int(x) for x in line.split())
            if vbl == prev:
                continue
            prev = vbl
            w0 &= 0xFFFFFFFF
            w1 &= 0xFFFFFFFF
            if (w0 & 0xFFFF) != 0x7300:
                raise SystemExit(f"stick_response_measure: {words} VBL {vbl}: pad status/type "
                                 f"{w0 & 0xFFFF:04x}, not an analog pad (00 73)")
            out.append((vbl, code_of(w0 >> 16), [(w1 >> (8 * i)) & 0xFF for i in range(4)]))
    return out


def decode(words: str, kind: str) -> dict[str, list[int]]:
    """The sweep's byte per axis value, per axis; refuses a sweep that missed or disagrees."""
    per_value = 2 * HOLD if kind == "alt" else HOLD
    samples = polls(words)
    # Row r is polled at VBL r + off; the second code's first poll fixes off.
    off = next(v for v, c, _ in samples if c == 1) - (LEAD + per_value)
    lo, hi = LEAD + off, LEAD + 65536 * per_value - 1 + off
    by_code: dict[int, list[list[int]]] = defaultdict(list)
    for vbl, c, b in samples:
        if lo <= vbl <= hi:
            by_code[c].append(b)
    if len(by_code) != 65536:
        raise SystemExit(f"stick_response_measure: {kind}: {len(by_code)} of 65536 values polled")
    values = sweep_values(kind)
    tables = {a: [0] * 65536 for a in AXES}
    rests = set()
    for c, s in by_code.items():
        v = values[c]
        # alt: the first poll is the value, the last the centre after it (a
        # VBL without a pad copy can drop one of the four).
        val_polls = s[:1] if kind == "alt" else s
        for i, a in enumerate(AXES):
            got = {p[i] for p in val_polls}
            if len(got) != 1:
                raise SystemExit(f"stick_response_measure: {kind}: value {v} axis {a} polled as {sorted(got)}")
            x = -1 - v if kind == "cross" and a in ("LY", "RX") else v
            tables[a][x + 32768] = got.pop()
            if kind == "alt":
                rests.add(s[-1][i])
    if kind == "alt" and len(rests) != 1:
        raise SystemExit(f"stick_response_measure: alt: the centre after a value reads {sorted(rests)}")
    counts = defaultdict(int)
    for s in by_code.values():
        counts[len(s)] += 1
    print(f"  {kind}: 65536 values on 4 axes, polls per value {dict(sorted(counts.items()))}")
    return tables


# Power-on probe segments, (LX, LY, RX, RY) held for PROBE_HOLD rows each.
PROBE = ((96, -96, 0, 0), (-50, 50, 0, 0), (97, -97, 50, 0), (-50, 50, -50, 0), (96, -96, 96, 0),
         (0, 0, -96, 0), (-200, 200, -97, 0), (50, -50, 50, 0), (0, 0, 0, 0))
PROBE_HOLD = 30
POWER_ON = ("an axis reads 128 for every value inside -96..96 until it first receives a value outside "
            "that range, and the table's byte from that row on in both directions; each axis latches on "
            "its own (checked at 0, +-50, +-96, +-97 and +-200 on three axes while the fourth stays at 0)")


def boot_probe(work: str, table: list[int]) -> str:
    """Check every poll of the power-on probe against POWER_ON; return POWER_ON."""
    rows = [movie_row(0, 0, 0, 0, 0)] * LEAD
    for k, seg in enumerate(PROBE, 1):
        rows += [movie_row(*seg, k)] * PROBE_HOLD
    write_movie(os.path.join(work, "power-on.bk2"), rows)
    samples = polls(capture(work, "power-on", len(rows) + 200))
    first_input = next(vbl for vbl, c, _ in samples if c == 1)
    latched = [False] * 4
    expect = {0: (128, 128, 128, 128)}
    for k, (lx, ly, rx, ry) in enumerate(PROBE, 1):
        vals = (rx, ry, lx, ly)  # pad buffer order
        latched = [l or abs(v) > 96 for l, v in zip(latched, vals)]
        expect[k] = tuple(table[v + 32768] if l else 128 for l, v in zip(latched, vals))
    seen = defaultdict(int)
    for vbl, c, b in samples:
        if c > len(PROBE) or (c == 0 and not 400 < vbl < first_input):
            continue
        if tuple(b) != expect[c]:
            raise SystemExit(f"stick_response_measure: power-on probe segment {c} read {b}, expected {expect[c]}")
        seen[c] += 1
    if len(seen) != len(PROBE) + 1:
        raise SystemExit(f"stick_response_measure: power-on probe polled {len(seen)} of {len(PROBE) + 1} segments")
    return POWER_ON


def runs_of(table: list[int]) -> list[tuple[int, int, int]]:
    runs, start = [], 0
    for i in range(1, 65537):
        if i == 65536 or table[i] != table[start]:
            runs.append((start - 32768, i - 1 - 32768, table[start]))
            start = i
    return runs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("work", help="scratch directory for movies and captures")
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--jobs", type=int, default=2, help="reference processes at once")
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    with open(os.path.join(a.work, "recipe.conf"), "w") as f:
        f.write(RECIPE.format(pc=PAD_COPY_PC))
    head = open(os.path.join(REPO, "third_party/duckstation-patches/applied-head.txt")).read().strip()

    def one(kind: str) -> dict[str, list[int]]:
        n = sweep_movie(os.path.join(a.work, kind + ".bk2"), kind)
        return decode(capture(a.work, kind, LEAD + n + 200), kind)

    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        results = dict(zip(SWEEPS, ex.map(one, SWEEPS)))
    ref = results["same"]["LX"]
    for kind, tables in results.items():
        for ax, t in tables.items():
            if t != ref:
                i = next(i for i in range(65536) if t[i] != ref[i])
                raise SystemExit(f"stick_response_measure: {kind} {ax} differs at value {i - 32768}")
    if any(ref[i] > ref[i + 1] for i in range(65535)):
        raise SystemExit("stick_response_measure: the table is not monotonic")
    boot = boot_probe(a.work, ref)
    print(f"  boot probe: {boot}")
    runs = runs_of(ref)
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, "w") as f:
        f.write(
            "# --stick-response console: the oracle reference's live stick response, a\n"
            "# measurement, not a derivation; it is not a measurement of a DualShock.\n"
            "# For every SDL axis value (-32768..32767) the byte the game read from the port 1\n"
            "# analog pad when the reference test binary (third_party/duckstation-patches,\n"
            f"# applied head {head}) delivered the value through its live\n"
            "# input path (regtest -bk2-phys) at its default settings, on every stick axis.\n"
            "# Taken by tools/stick_response_measure.py: four sweeps (ascending, axes paired\n"
            "# with the mirrored value, descending, each value followed by the centre), 65536\n"
            "# values on each of four axes per sweep, every axis and sweep the same table.\n"
            "# Regenerate (repo root, about 15 minutes): tools/stick_response_measure.py WORKDIR\n"
            f"# Power-on: {boot}.\n"
            "# Rows: first last byte - every value from first to last reads byte.\n")
        for lo, hi, b in runs:
            f.write(f"{lo} {hi} {b}\n")
    print(f"stick_response_measure: {len(runs)} runs -> {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
