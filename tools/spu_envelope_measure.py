#!/usr/bin/env python3
"""spu_envelope_measure.py - measure the oracle reference's SPU envelope levels.

Regenerates tests/unit/data/spu-envelope/reference.env: for each case below,
the level of one SPU level register of voice 0 (its ENVX, its current left
volume, or the current main left volume) at every 44.1 kHz sample boundary
of the case, as the reference test binary (third_party/duckstation-patches)
reports it to a program polling the register.

Method. tools/spu_envelope_probe is a PS-EXE (built here with
mipsel-linux-gnu-gcc) that runs every case on voice 0 with interrupts off:
it writes the case's ADSR words, makes the case's register stores at whole
samples after the case start (a time in the middle of a sample period),
and polls the level register every few dozen CPU cycles against root
counter 2, logging each change with the counter time of the poll that saw
it and the time since the poll before. The reference boots the PS-EXE in
place of a disc (scripts/capture_psx_oracle_ds.sh --disc), and the log
leaves through the capture's words recorder (--words), 63 words per
record. This script maps each change to the sample boundary inside its
poll interval: the boundaries lie on one grid of 768 system clocks, whose
phase is the intersection of every change's poll interval; a change whose
interval holds no grid point or more than one is refused.

usage (from the repo root):
  tools/spu_envelope_measure.py WORKDIR [--out PATH]
Needs the patched DuckStation test binary (scripts/setup_duckstation.sh;
OPENPETE_DS_DIR names another checkout's) and mipsel-linux-gnu-gcc.
About two minutes.
"""

from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROBE = os.path.join(REPO, "tools", "spu_envelope_probe")
DEFAULT_OUT = os.path.join(REPO, "tests/unit/data/spu-envelope/reference.env")
SAMPLE = 768
WINDOW = 0x8003F000
POLLS = ("envx", "voll", "mainl")
EVENTS = {"kon": 1, "koff": 2, "adsr1": 3, "adsr2": 4, "voll": 5, "mainl": 6}
EVENT_NAMES = {v: k for k, v in EVENTS.items()}


def a1(mode: int, shift: int, step: int, dshift: int, sl: int) -> int:
    """ADSR1 from its fields: attack mode, shift, step; decay shift; sustain level."""
    return mode << 15 | shift << 10 | step << 8 | dshift << 4 | sl


def a2(smode: int, sdir: int, sshift: int, sstep: int, rmode: int, rshift: int) -> int:
    """ADSR2 from its fields: sustain mode, direction, shift, step; release mode, shift."""
    return smode << 15 | sdir << 14 | sshift << 8 | sstep << 6 | rmode << 5 | rshift


def hold(rmode: int = 0, rshift: int = 0) -> int:
    """ADSR2 whose sustain fields are all ones (linear increase, shift 1Fh, step 3), with a release."""
    return a2(0, 0, 0x1F, 3, rmode, rshift)


FAST = a1(0, 0, 0, 0, 0xF)   # linear attack, shift 0 step 0; decay shift 0; sustain level Fh


def env(name, adsr1, adsr2, total, events, poll="envx", vol_init=0x3FFF):
    return dict(name=name, poll=poll, vol_init=vol_init, adsr1=adsr1, adsr2=adsr2, total=total,
                events=[(0, "kon", 0)] + events if poll == "envx" else events)


# Each case: name, ADSR words, samples polled, register stores (sample, kind, value).
CASES = [
    # Attack: mode, shift and step across the range; release at the end.
    env("attack_lin_s00_st0", a1(0, 0x00, 0, 0, 0xF), hold(), 600, [(300, "koff", 0)]),
    env("attack_lin_s00_st3", a1(0, 0x00, 3, 0, 0xF), hold(), 600, [(300, "koff", 0)]),
    env("attack_lin_s04_st1", a1(0, 0x04, 1, 0, 0xF), hold(), 800, [(500, "koff", 0)]),
    env("attack_lin_s0a_st2", a1(0, 0x0A, 2, 0, 0xF), hold(), 5000, [(4500, "koff", 0)]),
    env("attack_lin_s0b_st0", a1(0, 0x0B, 0, 0, 0xF), hold(), 6000, [(5500, "koff", 0)]),
    env("attack_lin_s0c_st3", a1(0, 0x0C, 3, 0, 0xF), hold(), 18000, [(17500, "koff", 0)]),
    env("attack_lin_s0f_st0", a1(0, 0x0F, 0, 0, 0xF), hold(), 76000, []),
    env("attack_lin_s16_st0", a1(0, 0x16, 0, 0, 0xF), hold(), 30000, []),
    env("attack_lin_s1f_st3", a1(0, 0x1F, 3, 0, 0xF), hold(), 3000, []),
    env("attack_exp_s00_st0", a1(1, 0x00, 0, 0, 0xF), hold(), 600, [(300, "koff", 0)]),
    env("attack_exp_s07_st1", a1(1, 0x07, 1, 0, 0xF), hold(), 1500, [(1200, "koff", 0)]),
    env("attack_exp_s09_st3", a1(1, 0x09, 3, 0, 0xF), hold(), 5000, [(4500, "koff", 0)]),
    env("attack_exp_s0a_st0", a1(1, 0x0A, 0, 0, 0xF), hold(), 6000, [(5500, "koff", 0)]),
    env("attack_exp_s0b_st2", a1(1, 0x0B, 2, 0, 0xF), hold(), 14000, [(13500, "koff", 0)]),
    env("attack_exp_s0d_st0", a1(1, 0x0D, 0, 0, 0xF), hold(), 36000, []),
    env("attack_exp_s1f_st3", a1(1, 0x1F, 3, 0, 0xF), hold(), 3000, []),
    # Decay: shift across 0..Fh, sustain levels including Fh.
    env("decay_s0_sl0", a1(0, 0, 0, 0x0, 0x0), hold(), 600, [(300, "koff", 0)]),
    env("decay_s3_sl7", a1(0, 0, 0, 0x3, 0x7), hold(), 600, [(300, "koff", 0)]),
    env("decay_s5_slf", a1(0, 0, 0, 0x5, 0xF), hold(), 600, [(300, "koff", 0)]),
    env("decay_s7_sl3", a1(0, 0, 0, 0x7, 0x3), hold(), 1500, [(1200, "koff", 0)]),
    env("decay_s9_sle", a1(0, 0, 0, 0x9, 0xE), hold(), 600, [(400, "koff", 0)]),
    env("decay_sa_sla", a1(0, 0, 0, 0xA, 0xA), hold(), 3000, [(2500, "koff", 0)]),
    env("decay_sc_sl5", a1(0, 0, 0, 0xC, 0x5), hold(), 14000, [(13000, "koff", 0)]),
    env("decay_sf_sle", a1(0, 0, 0, 0xF, 0xE), hold(), 9000, [(8000, "koff", 0)]),
    env("decay_sf_sl0", a1(0, 0, 0, 0xF, 0x0), hold(), 200000, []),
    # Sustain: mode, direction, shift and step, from several sustain levels.
    env("sustain_lin_inc_s0a_st0_sl7", a1(0, 0, 0, 0, 0x7), a2(0, 0, 0x0A, 0, 0, 0), 3000, [(2500, "koff", 0)]),
    env("sustain_lin_dec_s0c_st1_slf", FAST, a2(0, 1, 0x0C, 1, 0, 0), 13000, [(12500, "koff", 0)]),
    env("sustain_exp_dec_s08_st0_slb", a1(0, 0, 0, 0, 0xB), a2(1, 1, 0x08, 0, 0, 0), 8000, [(7500, "koff", 0)]),
    env("sustain_exp_inc_s09_st2_sl3", a1(0, 0, 0, 0, 0x3), a2(1, 0, 0x09, 2, 0, 0), 4000, [(3500, "koff", 0)]),
    env("sustain_exp_inc_s0a_st0_sl0", a1(0, 0, 0, 0, 0x0), a2(1, 0, 0x0A, 0, 0, 0), 6000, [(5500, "koff", 0)]),
    env("sustain_exp_inc_s0e_st0_slb", a1(0, 0, 0, 0, 0xB), a2(1, 0, 0x0E, 0, 0, 0), 40000, []),
    env("sustain_lin_dec_s00_st3_slf", FAST, a2(0, 1, 0x00, 3, 0, 0), 500, []),
    env("sustain_exp_dec_s1b_st0_slf", FAST, a2(1, 1, 0x1B, 0, 0, 0), 140000, []),
    env("sustain_lin_dec_s1f_st3_sl7", a1(0, 0, 0, 0, 0x7), a2(0, 1, 0x1F, 3, 0, 0), 3000, []),
    env("sustain_exp_dec_s0f_st2_slf", FAST, a2(1, 1, 0x0F, 2, 0, 0), 30000, []),
    env("sustain_exp_dec_s04_st3_slf", FAST, a2(1, 1, 0x04, 3, 0, 0), 2000, []),
    # Release: mode and shift across 0..1Fh, from the hold at 7FFFh.
    env("release_lin_s00", FAST, hold(0, 0x00), 600, [(300, "koff", 0)]),
    env("release_lin_s05", FAST, hold(0, 0x05), 600, [(300, "koff", 0)]),
    env("release_lin_s0b", FAST, hold(0, 0x0B), 4800, [(300, "koff", 0)]),
    env("release_lin_s0d", FAST, hold(0, 0x0D), 17000, [(300, "koff", 0)]),
    env("release_lin_s17", FAST, hold(0, 0x17), 40000, [(300, "koff", 0)]),
    env("release_lin_s1f", FAST, hold(0, 0x1F), 3000, [(300, "koff", 0)]),
    env("release_exp_s00", FAST, hold(1, 0x00), 600, [(300, "koff", 0)]),
    env("release_exp_s06", FAST, hold(1, 0x06), 6000, [(300, "koff", 0)]),
    env("release_exp_s0b", FAST, hold(1, 0x0B), 14000, [(300, "koff", 0)]),
    env("release_exp_s0f", FAST, hold(1, 0x0F), 60000, [(300, "koff", 0)]),
    env("release_exp_s1f", FAST, hold(1, 0x1F), 3000, [(300, "koff", 0)]),
    # Key-off at different phases and times; key-on during release.
    env("koff_at_1_attack", a1(0, 0x0C, 0, 0, 0xF), hold(1, 0x04), 2000, [(1, "koff", 0)]),
    env("koff_with_kon", a1(0, 0x0C, 0, 0, 0xF), hold(0, 0x08), 500, [(0, "koff", 0)]),
    env("koff_lin_attack", a1(0, 0x0E, 0, 0, 0xF), hold(0, 0x09), 11000, [(10000, "koff", 0)]),
    env("koff_exp_attack_low", a1(1, 0x0C, 0, 0, 0xF), hold(0, 0x0C), 5000, [(3000, "koff", 0)]),
    env("koff_exp_attack_high", a1(1, 0x0C, 0, 0, 0xF), hold(1, 0x08), 16000, [(12000, "koff", 0)]),
    env("koff_decay", a1(0, 0, 0, 0xD, 0x0), hold(0, 0x0A), 6000, [(3000, "koff", 0)]),
    env("koff_sustain_exp_dec", FAST, a2(1, 1, 0x09, 0, 1, 0x03), 3000, [(1500, "koff", 0)]),
    env("kon_during_release", FAST, hold(0, 0x0F), 2500,
        [(200, "koff", 0), (1000, "kon", 0), (1500, "koff", 0)]),
    # Register writes inside a phase.
    env("write_adsr2_in_sustain", FAST, hold(), 4000, [(2000, "adsr2", a2(0, 1, 0x08, 0, 0, 0))]),
    env("write_adsr1_in_attack", a1(0, 0x0E, 0, 0, 0xF), hold(), 5000, [(3000, "adsr1", a1(0, 0, 0, 0, 0xF))]),
    env("write_adsr2_in_release", FAST, hold(0, 0x0C), 4000, [(300, "koff", 0), (2000, "adsr2", hold(0, 0x02))]),
    env("write_adsr1_in_decay", a1(0, 0, 0, 0xE, 0x0), hold(), 8000, [(2000, "adsr1", a1(0, 0, 0, 0xE, 0xC))]),
    # Volume sweeps: voice 0 current left volume, and the main left volume.
    env("sweep_lin_inc_s08", FAST, hold(), 2000, [(0, "kon", 0), (100, "voll", 0x8000 | 8 << 2)], "voll", 0),
    env("sweep_lin_dec_s0a_st1", FAST, hold(), 4000, [(0, "kon", 0), (100, "voll", 0xA000 | 10 << 2 | 1)], "voll"),
    env("sweep_exp_inc_s09", FAST, hold(), 5000, [(0, "kon", 0), (100, "voll", 0xC000 | 9 << 2)], "voll", 0),
    env("sweep_exp_dec_s06", FAST, hold(), 4000, [(0, "kon", 0), (100, "voll", 0xE000 | 6 << 2)], "voll"),
    env("sweep_lin_inc_neg_s08", FAST, hold(), 2000, [(0, "kon", 0), (100, "voll", 0x9000 | 8 << 2)], "voll", 0),
    env("sweep_lin_dec_neg_s0a", FAST, hold(), 4000, [(0, "kon", 0), (100, "voll", 0xB000 | 10 << 2)], "voll"),
    env("sweep_exp_inc_s0c", FAST, hold(), 40000, [(0, "kon", 0), (100, "voll", 0xC000 | 12 << 2)], "voll", 0),
    env("sweep_direct_2000", FAST, hold(), 300, [(0, "kon", 0), (100, "voll", 0x1234)], "voll", 0x2000),
    env("sweep_main_lin_dec_s09", FAST, hold(), 3000, [(0, "kon", 0), (100, "mainl", 0xA000 | 9 << 2)], "mainl"),
]


def write_cases_h(path: str) -> None:
    lines = ["/* cases.h - written by tools/spu_envelope_measure.py; not edited by hand. */",
             "static const ProbeCase k_cases[] = {"]
    for c in CASES:
        ev = ", ".join("{%du, %d, 0x%04Xu}" % (at, EVENTS[k], v) for at, k, v in c["events"])
        lines.append("\t{ %d, 0x%04Xu, 0x%04Xu, 0x%04Xu, %du, %du, { %s } },  /* %s */" % (
            POLLS.index(c["poll"]), c["vol_init"], c["adsr1"], c["adsr2"], c["total"], len(c["events"]),
            ev or "{0, 0, 0}", c["name"]))
    lines += ["};", "#define N_CASES %du" % len(CASES), ""]
    with open(path, "w") as f:
        f.write("\n".join(lines))


def build_exe(work: str) -> tuple[str, int]:
    """Compile the probe; return the PS-EXE path and probe_anchor's address."""
    write_cases_h(os.path.join(work, "cases.h"))
    elf = os.path.join(work, "probe.elf")
    cc = ["mipsel-linux-gnu-gcc", "-O2", "-march=r3000", "-mabi=32", "-mno-abicalls", "-fno-pic", "-msoft-float",
          "-G0", "-ffreestanding", "-fno-builtin", "-fno-zero-initialized-in-bss", "-fno-tree-loop-distribute-patterns", "-nostdlib", "-static",
          "-I", work, "-T", os.path.join(PROBE, "probe.ld"), "-Wl,--build-id=none",
          os.path.join(PROBE, "crt0.S"), os.path.join(PROBE, "probe.c"), "-o", elf]
    subprocess.run(cc, check=True)
    raw = os.path.join(work, "probe.bin")
    subprocess.run(["mipsel-linux-gnu-objcopy", "-O", "binary", elf, raw], check=True)
    nm = subprocess.run(["mipsel-linux-gnu-nm", elf], check=True, capture_output=True, text=True).stdout
    sym = {l.split()[2]: int(l.split()[0], 16) & 0xFFFFFFFF for l in nm.splitlines() if len(l.split()) == 3}
    body = open(raw, "rb").read()
    body += b"\0" * (-len(body) % 2048)
    hdr = bytearray(2048)
    hdr[0:8] = b"PS-X EXE"
    struct.pack_into("<IIII", hdr, 0x10, sym["_start"], 0, 0x80010000, len(body))
    struct.pack_into("<II", hdr, 0x30, 0x801FFF00, 0)
    hdr[0x4C:0x4C + 47] = b"Sony Computer Entertainment Inc. for North America area"[:47]
    exe = os.path.join(work, "probe.exe")
    with open(exe, "wb") as f:
        f.write(bytes(hdr) + body)
    return exe, sym["probe_anchor"]


def capture(work: str, exe: str, anchor: int, records: int, vbls: int) -> str:
    out = os.path.join(work, "probe.trace")
    words = out + ".words"
    if os.path.exists(words):
        os.remove(words)
    cmd = ["scripts/capture_psx_oracle_ds.sh", "--disc", exe, "--out", out, "--anchor-pc", "0x%08X" % anchor,
           "--frames", str(records), "--max-runtime-frames", str(vbls), "--no-card",
           "--words", "0x%08X:64" % WINDOW]
    with open(os.path.join(work, "capture.log"), "w") as log:
        rc = subprocess.run(cmd, cwd=REPO, stdout=log, stderr=subprocess.STDOUT).returncode
    if rc != 0 or not os.path.exists(words):
        raise SystemExit(f"spu_envelope_measure: capture failed (rc={rc}); see {work}/capture.log")
    return words


def read_log(words: str) -> tuple[list[int], int, int]:
    """The probe's log words, the calibrated phase and the case count from the .words rows."""
    rows = []
    with open(words) as f:
        for line in f:
            if line.startswith(("#", "psxtrace")):
                continue
            rows.append([int(x) & 0xFFFFFFFF for x in line.split()[2:]])
    head = next((r for r in rows if r[0] == 0xFFFFFFFF), None)
    if head is None:
        raise SystemExit("spu_envelope_measure: no header record")
    n, overflow, phase, ncases = head[1], head[2], head[3], head[4]
    if overflow:
        raise SystemExit("spu_envelope_measure: the probe's log overflowed")
    if ncases != len(CASES):
        raise SystemExit(f"spu_envelope_measure: probe ran {ncases} cases, the table has {len(CASES)}")
    chunks = {r[0]: r[1:] for r in rows if r[0] != 0xFFFFFFFF}
    need = (n + 62) // 63
    if any(c not in chunks for c in range(need)):
        raise SystemExit(f"spu_envelope_measure: {need} chunks needed, records end early")
    log = [w for c in range(need) for w in chunks[c]][:n]
    return log, phase, ncases


def parse_cases(log: list[int]) -> list[dict]:
    cases, cur = [], None
    for i in range(0, len(log), 2):
        w0, t = log[i], log[i + 1]
        tag = w0 >> 24
        if tag == 0xFC:
            cur = dict(idx=w0 & 0xFFFFFF, t0=t, changes=[], events=[], initial=None)
            cases.append(cur)
        elif tag == 0xFE:
            cur["maxgap"] = w0 & 0xFFFFFF
            cur["t_end"] = t
        elif tag == 0xF0:
            cur["events"].append(((w0 >> 16) & 0xFF, w0 & 0xFFFF, t))
        elif cur["initial"] is None:
            cur["initial"] = w0 & 0xFFFF
        else:
            cur["changes"].append((w0 >> 16, w0 & 0xFFFF, t))
    return cases


DRIFT = 0.003      # clocks per sample the boundary phase may move beyond the fitted period
STORE_LAG = 32     # clocks from an event's logged counter read to its store


def sample_period(cases: list[dict]) -> float:
    """The SPU sample period in root counter 2 clocks: 768 plus the drift of the
    boundaries against the counter, fitted over the cases whose changes span
    more than 20000 samples (first and last change, weighted by span)."""
    num = den = 0.0
    for c in cases:
        t0, prev, pts = c["t0"], None, []
        for _, _, t in c["changes"]:
            x = t - t0
            k = round((x - SAMPLE / 2) / SAMPLE) + 1 if prev is None else \
                prev[0] + round((x - (prev[1] + SAMPLE * (prev[0] - 1))) / SAMPLE)
            prev = (k, x - SAMPLE * (k - 1))
            pts.append(prev)
        if len(pts) > 1 and pts[-1][0] - pts[0][0] > 20000:
            num += pts[-1][1] - pts[0][1]
            den += pts[-1][0] - pts[0][0]
    if den == 0:
        raise SystemExit("spu_envelope_measure: no case long enough to fit the sample period")
    return SAMPLE + num / den


def decode_case(c: dict, period: float) -> dict:
    """Boundary index of every change and event of one case.

    Boundary k (k >= 1) lies at t0 + phi + period (k - 1) on root counter
    2, phi the offset of boundary 1 from the case start. phi starts as the
    middle half of a sample (the probe starts each case in the middle of a
    sample period after calibrating) and narrows to the intersection of the
    poll intervals of the changes, widened by DRIFT per sample between two
    changes, since the counter drifts slowly against the SPU's sample clock.
    An event takes the phase window of the change nearest to it in time,
    widened the same way. A change whose poll interval admits no boundary
    index or more than one, or an event within the window's reach of a
    boundary, is refused."""
    spec = CASES[c["idx"]]
    name, t0 = spec["name"], c["t0"]
    lo, hi, last_k = SAMPLE / 4, 3 * SAMPLE / 4, 0
    rows, track = [(0, c["initial"])], []
    for gap, v, t in c["changes"]:
        if gap >= SAMPLE:
            raise SystemExit(f"{name}: a poll gap of {gap} clocks at a change")
        fits = []
        k0 = int((t - t0 - hi) // period)
        for k in range(max(1, k0 - 1), k0 + 4):
            a = DRIFT * max(0, k - last_k) + 1
            l2 = max(lo - a, t - gap + 1 - t0 - period * (k - 1))
            h2 = min(hi + a, t - t0 - period * (k - 1))
            if l2 <= h2:
                fits.append((k, l2, h2))
        if len(fits) != 1:
            raise SystemExit(f"{name}: change at t-t0={t - t0} admits {len(fits)} boundaries")
        k, lo, hi = fits[0]
        if k <= rows[-1][0]:
            raise SystemExit(f"{name}: two changes at boundary {k}")
        rows.append((k, v))
        track.append((t, lo, hi))
        last_k = k
    evs = []
    for what, val, t in c["events"]:
        if track:
            tt, l, h = min(track, key=lambda x: abs(x[0] - t))
            a = DRIFT * abs(tt - t) / SAMPLE + 1
            l, h = l - a, h + a
        else:
            l, h = SAMPLE / 4, 3 * SAMPLE / 4
        ks = {int((ts - t0 - ph) // period) + 1 for ph in (l, h) for ts in (t, t + STORE_LAG)}
        if len(ks) != 1:
            raise SystemExit(f"{name}: event {EVENT_NAMES[what]} at t-t0={t - t0} lies within reach of a boundary")
        evs.append((ks.pop(), EVENT_NAMES[what], val))
    want = [(at, k, v) for at, k, v in spec["events"]]
    if evs != want:
        raise SystemExit(f"{name}: events at {evs}, wanted {want}")
    return dict(spec=spec, rows=rows, events=evs, maxgap=c["maxgap"],
                phase_width=round(max(h - l for _, l, h in track), 1) if track else None)


def applied_head() -> str:
    with open(os.path.join(REPO, "third_party/duckstation-patches/applied-head.txt")) as f:
        return f.read().strip()


def runs(rows: list[tuple[int, int]]) -> list[list[int]]:
    """The changes of a level series as runs [count, boundary step, value step]."""
    out: list[list[int]] = []
    for (k1, v1), (k2, v2) in zip(rows, rows[1:]):
        if out and out[-1][1] == k2 - k1 and out[-1][2] == v2 - v1:
            out[-1][0] += 1
        else:
            out.append([1, k2 - k1, v2 - v1])
    return out


def write_out(path: str, dec: list[dict], period: float) -> None:
    maxgap = max(d["maxgap"] for d in dec)
    lines = [
        "# SPU envelope levels of the oracle reference: a measurement, not a derivation.",
        "# The reference test binary (third_party/duckstation-patches, applied head",
        "# %s) ran tools/spu_envelope_probe, a PS-EXE that" % applied_head(),
        "# programs voice 0 and polls one level register of the SPU in a loop with",
        "# interrupts off; tools/spu_envelope_measure.py maps every change it saw to",
        "# the 44.1 kHz sample boundary inside the poll interval that saw it.",
        "# Regenerate (repo root, about two minutes): tools/spu_envelope_measure.py WORKDIR",
        "# Exactness: the poll interval of every change admits exactly one boundary",
        "# (largest poll interval %d clocks of the 768 a sample lasts; the boundary" % maxgap,
        "# phase is tracked per case on a fitted period of %.5f counter clocks per" % period,
        "# sample, with %.3f clocks per sample of slack), and every store lies at" % DRIFT,
        "# least %d clocks inside its sample, so each value is the register's value" % STORE_LAG,
        "# from its boundary to the next, exactly.",
        "#",
        "# Format, one block per case:",
        "#   case NAME poll=envx|voll|mainl adsr1=HEX adsr2=HEX vol_init=HEX samples=N",
        "#     poll: the register read; envx voice 0's ENVX, voll voice 0's current",
        "#     left volume, mainl the current main left volume. Before the case the",
        "#     voice is keyed off and silent (ENVX 0), its ADSR words and left volume",
        "#     register hold adsr1, adsr2 and vol_init, the main volume registers 3FFFh.",
        "#   event K kon|koff|adsr1 HEX|adsr2 HEX|voll HEX|mainl HEX",
        "#     the store lies between boundary K and boundary K+1 (K = 0: between the",
        "#     case start and boundary 1), in the order listed.",
        "#   level 0 V",
        "#     the register reads V (hex, 16 bits) before boundary 1.",
        "#   run C DK DV",
        "#     C changes of the value, each DK boundaries after the one before (the",
        "#     first after the previous row's last change, or after the case start)",
        "#     and DV (signed decimal) from it; the value holds between changes and",
        "#     after the last change to boundary N.",
        "#   end",
    ]
    for d in dec:
        s = d["spec"]
        lines.append("case %s poll=%s adsr1=%04X adsr2=%04X vol_init=%04X samples=%d" % (
            s["name"], s["poll"], s["adsr1"], s["adsr2"], s["vol_init"], s["total"]))
        for at, kind, val in d["events"]:
            lines.append("event %d %s" % (at, kind) + (" %04X" % val if kind not in ("kon", "koff") else ""))
        lines.append("level 0 %04X" % d["rows"][0][1])
        for c, dk, dv in runs(d["rows"]):
            lines.append("run %d %d %d" % (c, dk, dv))
        lines.append("end")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("work")
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--records", type=int, default=7000)
    ap.add_argument("--vbls", type=int, default=4000)
    args = ap.parse_args()
    work = os.path.abspath(args.work)
    os.makedirs(work, exist_ok=True)
    exe, anchor = build_exe(work)
    words = capture(work, exe, anchor, args.records, args.vbls)
    log, phase, _ = read_log(words)
    cases = parse_cases(log)
    if len(cases) != len(CASES):
        raise SystemExit(f"spu_envelope_measure: {len(cases)} cases in the log")
    period = sample_period(cases)
    print(f"sample period {period:.5f} root counter 2 clocks")
    dec = [decode_case(c, period) for c in cases]
    for d in dec:
        print("  %-32s %6d rows  max poll gap %4d  phase window %s" % (
            d["spec"]["name"], len(d["rows"]), d["maxgap"], d["phase_width"]))
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    write_out(args.out, dec, period)
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
