#!/usr/bin/env python3
"""rumble_response_measure.py - measure the oracle reference's motor intensity per actuator byte.

Regenerates tests/unit/data/rumble-response/reference.tsv, the intensity
(0..1, an IEEE-754 single) the reference emulator's port 1 analog controller
hands its host input layer for each actuator byte of the small and the large
motor, at its default settings.

Method. tools/rumble_probe/ is a PS-EXE that talks to the port 1 controller
over SIO0 with interrupts off: it enters configuration mode, selects analog
mode, maps actuator byte 0 to the small motor and byte 1 to the large one,
then polls every byte pair twice, once with the small byte in the outer loop
and once with the large byte in the outer loop (see probe.c). The reference
test binary runs it with -motor-log (third_party/duckstation-patches),
which writes one row per change of a motor's state: the motor, its new byte,
the other motor's byte and the intensity's bits. The table is written only
when every (byte, other byte) combination of each motor was logged, the
row count is the one the poll order implies, and each motor's intensity is
a function of its own byte alone (every other-byte value gives the same
bits).

usage (from the repo root):
  tools/rumble_response_measure.py WORKDIR [--out PATH]
Needs the patched DuckStation test binary (scripts/setup_duckstation.sh;
OPENPETE_DS_DIR names another checkout), its SCPH-1001 BIOS and
mipsel-linux-gnu-gcc. About ten seconds.
"""

from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROBE = os.path.join(REPO, "tools", "rumble_probe")
DEFAULT_OUT = os.path.join(REPO, "tests/unit/data/rumble-response/reference.tsv")
VBLS = 6000
# Rows the poll order in probe.c produces: the four prologue polls, the
# small-outer sweep (255 large changes at small byte 0, then per small byte
# 1..255 a small change, a large change back to 0 and 255 large changes),
# the large-outer sweep (the same with the motors swapped, starting from
# (255, 255), so its first poll changes both) and the closing (0, 0).
EXPECTED_ROWS = 4 + (255 + 255 * 257) + (2 + 255 + 255 * 257) + 2


def build_exe(work: str) -> str:
    """Compile the probe and wrap it as a PS-EXE; return its path."""
    elf = os.path.join(work, "probe.elf")
    cc = ["mipsel-linux-gnu-gcc", "-O2", "-march=r3000", "-mabi=32", "-mno-abicalls", "-fno-pic", "-msoft-float",
          "-G0", "-ffreestanding", "-fno-builtin", "-fno-tree-loop-distribute-patterns", "-nostdlib", "-static",
          "-T", os.path.join(PROBE, "probe.ld"), "-Wl,--build-id=none",
          os.path.join(PROBE, "crt0.S"), os.path.join(PROBE, "probe.c"), "-o", elf]
    subprocess.run(cc, check=True)
    raw = os.path.join(work, "probe.bin")
    subprocess.run(["mipsel-linux-gnu-objcopy", "-O", "binary", elf, raw], check=True)
    body = open(raw, "rb").read()
    body += b"\0" * (-len(body) % 2048)
    hdr = bytearray(2048)
    hdr[0:8] = b"PS-X EXE"
    struct.pack_into("<IIII", hdr, 0x10, 0x80010000, 0, 0x80010000, len(body))
    struct.pack_into("<II", hdr, 0x30, 0x801FFF00, 0)
    hdr[0x4C:0x4C + 47] = b"Sony Computer Entertainment Inc. for North America area"[:47]
    exe = os.path.join(work, "probe.exe")
    with open(exe, "wb") as f:
        f.write(bytes(hdr) + body)
    return exe


def capture(work: str, exe: str) -> str:
    """Run the probe under the reference with the motor log; return the log path."""
    ds_dir = os.environ.get("OPENPETE_DS_DIR", os.path.join(REPO, "third_party/duckstation"))
    ds_bin = os.path.join(ds_dir, "build-regtest/bin/duckstation-regtest")
    if not os.access(ds_bin, os.X_OK):
        raise SystemExit(f"rumble_response_measure: {ds_bin} missing; run scripts/setup_duckstation.sh")
    log = os.path.join(work, "probe.motor")
    if os.path.exists(log):
        os.remove(log)
    env = dict(os.environ, LIBGL_ALWAYS_SOFTWARE="1", GALLIUM_DRIVER=os.environ.get("GALLIUM_DRIVER", "llvmpipe"))
    cmd = [ds_bin, "-frames", str(VBLS), "-motor-log", log, "-console", exe]
    with open(os.path.join(work, "capture.log"), "w") as out:
        subprocess.run(["sh", "-c", '"$@" 2>&1; exit 0', "_"] + cmd, stdout=out, stderr=subprocess.STDOUT,
                       env=env, check=False)
    if not os.path.exists(log):
        raise SystemExit(f"rumble_response_measure: no motor log; see {work}/capture.log")
    return log


def decode(log: str) -> tuple[str, list[list[int]], int]:
    """Return the build hash, the per-motor table of intensity bits and the row count.

    Refuses unless every (byte, other byte) combination of each motor was
    logged, the rows close with the trailer, their count is EXPECTED_ROWS,
    and each motor's bits depend on its own byte alone."""
    build = ""
    rows = 0
    trailer = None
    seen: list[dict[tuple[int, int], int]] = [{}, {}]
    for line in open(log):
        if line.startswith("motor-log"):
            build = line.split("build=")[1].strip()
            continue
        if line.startswith("# rows="):
            trailer = int(line.split("=")[1])
            continue
        if line.startswith("#"):
            continue
        _frame, pad, motor, value, other, bits, _dec = line.split()
        if pad != "0":
            raise SystemExit(f"rumble_response_measure: a row for pad {pad}")
        rows += 1
        key = (int(value), int(other))
        b = int(bits, 16)
        m = int(motor)
        if key in seen[m] and seen[m][key] != b:
            raise SystemExit(f"rumble_response_measure: motor {m} {key} logged with two values")
        seen[m][key] = b
    if trailer != rows:
        raise SystemExit(f"rumble_response_measure: trailer rows={trailer}, read {rows}")
    if rows != EXPECTED_ROWS:
        raise SystemExit(f"rumble_response_measure: {rows} rows, the poll order gives {EXPECTED_ROWS}")
    table: list[list[int]] = [[0] * 256, [0] * 256]
    for m in (0, 1):
        if len(seen[m]) != 65536:
            raise SystemExit(f"rumble_response_measure: motor {m}: {len(seen[m])} of 65536 combinations")
        for v in range(256):
            vals = {seen[m][(v, o)] for o in range(256)}
            if len(vals) != 1:
                raise SystemExit(f"rumble_response_measure: motor {m} byte {v} depends on the other motor")
            table[m][v] = vals.pop()
    return build, table, rows


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("workdir")
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--log", help="decode an existing motor log instead of capturing")
    args = ap.parse_args()
    os.makedirs(args.workdir, exist_ok=True)
    log = args.log or capture(args.workdir, build_exe(args.workdir))
    build, table, rows = decode(log)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w") as f:
        f.write(
            "# The oracle reference's motor intensity per actuator byte: a measurement, not\n"
            "# a derivation, and not a measurement of a DualShock motor.\n"
            "# For each actuator byte the intensity (0..1, IEEE-754 single bits in hex) the\n"
            "# reference test binary's port 1 analog controller (third_party/duckstation-\n"
            f"# patches, applied head {build}) hands its host input\n"
            "# layer at its default settings, for the small and the large motor.\n"
            "# Taken by tools/rumble_response_measure.py: the PS-EXE tools/rumble_probe/ sends\n"
            "# every byte pair through the controller protocol (analog mode, byte 0 small\n"
            f"# motor, byte 1 large motor) under regtest -motor-log; {rows} logged\n"
            "# changes cover all 65536 pairs for each motor, and each motor's intensity\n"
            "# depends on its own byte alone.\n"
            "# Regenerate (repo root, about ten seconds): tools/rumble_response_measure.py WORKDIR\n"
            "# Rows: byte small large\n")
        for v in range(256):
            f.write(f"{v} {table[0][v]:08X} {table[1][v]:08X}\n")
    print(f"rumble_response_measure: {rows} rows, build {build}, wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
