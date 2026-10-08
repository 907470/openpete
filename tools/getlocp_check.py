#!/usr/bin/env python3
"""Replay openpete's CdlGetlocP head model against a reference drive's answers.

Input: a golden's .cdirq sidecar (psxtrace-cdirq 2: `C` command records,
`I` interrupt records with the response bytes). A streaming run starts at
a CdlReadS (0x1B) at the last CdlSetloc's LBA and ends at CdlPause,
CdlStop or CdlReadN; every 8-byte response to a CdlGetlocP (0x11) inside
it is one reference answer, its absolute position at bytes 5..7 (BCD).

Model `openpete` mirrors the drive head in
games/common/psyq/psyq_thunks.cpp (cd_head_at, cd_head_read and the
commands that move it), replayed over the reference's own commands: a data
read (CdlReadN) rests the head after its last sector (the Setloc LBA plus
its data-ready interrupts), CdlSeekL/CdlSeekP rest it at the Setloc LBA,
CdlPlay starts it at the Setloc LBA, CdlPause/CdlStop stop it where it is,
and a CdlReadS starts it at the Setloc LBA after the seek time of the
distance from its current position (plus the speed-change term when the
mode's speed differs from the last read's or play's). A moving head
advances rate * 37609 / 30000 LBA per VBL. --source names that file; the
check refuses (exit 64) when a mirrored expression is absent from it, so a
changed model fails loudly here until this mirror follows it.
Model `calibrate` is the positive control: the drive rate 37609/15000 LBA
per VBL from the VBL at which a least squares fit of the run's answers
crosses the start LBA (the seek latency read off the reference itself).

Pass: |model - reference| <= --tol LBA at every answer of every run, at
least --min-runs runs. Engagement: answers compared; zero is VACUOUS.
Verdict line: [getlocp] runs=N answers=A over=O max_abs_diff=D
verdict=PASS|FAIL|VACUOUS. Exit 0 PASS, 2 FAIL or VACUOUS, 64 input missing.
"""
import argparse
import math
import os
import sys

MIRRORED = (
    "#define CD_HEAD_TU 30000u",
    "#define CD_SEEK_C0            499349u",
    "#define CD_SEEK_C1_PER_1E5    215288u",
    "#define CD_SEEK_C2_PER_1E5     73043u",
    "#define CD_SEEK_SPEED_CHANGE  520500u",
    "const uint64_t k = (uint64_t)s_head_rate * 37609u * per;",
)
TU = 30000
SEEK_C0, SEEK_C1, SEEK_C2, SEEK_SPEED = 499349, 215288, 73043, 520500


def bcd(s):
    """Value of a hex-spelled BCD byte."""
    v = int(s, 16)
    return v // 16 * 10 + v % 16


def msf_lba(m, s, f):
    """LBA of a BCD minute, second, sector position (150-sector lead-in)."""
    return (bcd(m) * 60 + bcd(s)) * 75 + bcd(f) - 150


class Head:
    """The drive head of cd_head_at / cd_head_read: rest or anchor LBA, anchor time (1/TU VBL), rate, last speed."""

    def __init__(self):
        self.lba, self.t, self.rate, self.speed = 0, 0, 0, 2

    def at(self, t):
        if not self.rate or t <= self.t:
            return self.lba
        return self.lba + (t - self.t) * self.rate * 37609 // 900000000

    def read(self, lba, rate, now):
        d = abs(lba - self.at(now))
        seek = SEEK_C0 + d * SEEK_C1 // 100000 + math.isqrt(d * 1000000) * SEEK_C2 // 100000
        if rate != self.speed:
            seek += SEEK_SPEED
        self.lba, self.t, self.rate, self.speed = lba, now + seek, rate, rate

    def play(self, lba, rate, now):
        self.lba, self.t, self.rate, self.speed = lba, now, rate, rate

    def stop(self, now):
        if self.rate:
            self.lba, self.rate = self.at(now), 0

    def rest(self, lba, rate):
        self.lba, self.rate, self.speed = lba, 0, rate


def runs(path):
    """Streaming runs of the .cdirq at path: dicts with vbl, lba, pts [(vbl, lba)], and the
    mirrored head as it stood at the run's CdlReadS (key head)."""
    out, cur, setloc, pend = [], None, None, None
    head, mode, readn = Head(), 0, None
    with open(path) as fh:
        for line in fh:
            f = line.split()
            if not f or f[0] not in ("C", "I"):
                continue
            if f[0] == "C":
                cmd = f[5]
                pend = cmd
                now = int(f[3]) * TU
                rate = 2 if mode & 0x80 else 1
                if cmd == "0x02" and int(f[6]) == 3:
                    setloc = msf_lba(f[7], f[8], f[9])
                elif cmd == "0x0E" and int(f[6]) >= 1:
                    mode = int(f[7], 16)
                elif cmd in ("0x15", "0x16") and setloc is not None:
                    head.rest(setloc, head.speed)
                elif cmd == "0x03" and setloc is not None:
                    head.play(setloc, rate, now)
                elif cmd == "0x06":
                    readn = [setloc or 0, 0, rate]
                    head.rate = 0
                elif cmd == "0x1B":
                    if setloc is not None:
                        head.read(setloc, rate, now)
                    snap = Head()
                    snap.lba, snap.t, snap.rate, snap.speed = head.lba, head.t, head.rate, head.speed
                    cur = {"vbl": int(f[3]), "lba": setloc, "pts": [], "head": snap}
                    out.append(cur)
                elif cmd in ("0x09", "0x08"):
                    if readn:
                        head.rest(readn[0] + readn[1], readn[2])
                        readn = None
                    head.stop(now)
                    cur = None
            else:
                if f[4] == "1" and readn:
                    readn[1] += 1
                if pend == "0x11" and f[5] == "8" and cur is not None:
                    r = f[6:14]
                    cur["pts"].append((int(f[3]), msf_lba(r[5], r[6], r[7])))
                    pend = None
    return out


def model_openpete(run, vbl):
    """openpete's drive head at reference VBL vbl, as it stood after the run's CdlReadS."""
    return run["head"].at(vbl * TU)


def make_calibrate(run):
    """Drive-rate head anchored at the fitted VBL the head reaches the start LBA."""
    p = run["pts"]
    n = len(p)
    mx = sum(v for v, _ in p) / n
    my = sum(s for _, s in p) / n
    k = sum((v - mx) * (s - my) for v, s in p) / sum((v - mx) ** 2 for v, _ in p)
    t0 = round(mx + (run["lba"] - my) / k)
    return lambda r, vbl: r["lba"] + max(vbl - t0, 0) * 37609 // 15000


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("cdirq")
    ap.add_argument("--model", choices=("openpete", "calibrate"), default="openpete")
    ap.add_argument("--source", default="games/common/psyq/psyq_thunks.cpp")
    ap.add_argument("--tol", type=int, default=2)
    ap.add_argument("--min-runs", type=int, default=1)
    ap.add_argument("--min-answers", type=int, default=10,
                    help="runs with fewer answers are listed and not compared")
    ap.add_argument("--tag", default="")
    a = ap.parse_args()
    if not os.path.isfile(a.cdirq):
        print(f"[getlocp] {a.cdirq} missing", file=sys.stderr)
        return 64
    if a.model == "openpete":
        try:
            src = open(a.source).read()
        except OSError:
            print(f"[getlocp] model source {a.source} missing", file=sys.stderr)
            return 64
        missing = [m for m in MIRRORED if m not in src]
        if missing:
            print(f"[getlocp] {a.source} no longer contains `{missing[0]}`: update the Head mirror",
                  file=sys.stderr)
            return 64
    nruns = answers = over = 0
    worst = 0
    for r in runs(a.cdirq):
        if r["lba"] is None or len(r["pts"]) < a.min_answers:
            print(f"ReadS vbl {r['vbl']} LBA {r['lba']}: {len(r['pts'])} answers, not compared")
            continue
        m = model_openpete if a.model == "openpete" else make_calibrate(r)
        d = [m(r, v) - s for v, s in r["pts"]]
        o = sum(abs(x) > a.tol for x in d)
        mx = max(d, key=abs)
        nruns += 1
        answers += len(d)
        over += o
        worst = max(worst, abs(mx))
        print(f"ReadS vbl {r['vbl']} LBA {r['lba']}: answers {len(d)} vbl {r['pts'][0][0]}-{r['pts'][-1][0]} "
              f"diff first {d[0]:+d} last {d[-1]:+d} max {mx:+d} over {o} {'ok' if o == 0 else 'FAIL'}")
    v = "VACUOUS" if answers == 0 else ("FAIL" if over or nruns < a.min_runs else "PASS")
    print(f"[getlocp] {a.tag + ' ' if a.tag else ''}model={a.model} runs={nruns} min_runs={a.min_runs} "
          f"answers={answers} over={over} max_abs_diff={worst} tol={a.tol} verdict={v}")
    return 0 if v == "PASS" else 2


if __name__ == "__main__":
    sys.exit(main())
