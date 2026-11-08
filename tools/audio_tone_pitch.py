#!/usr/bin/env python3
"""Read the pitch of a known tone in the audio device feed's output tap.

Input: a raw s16 stereo 44.1 kHz device-output tap (OPENPETE_AUDIO_FEED_TAP,
or audio_feed_sim --tap) carrying a sine of known frequency in its left
channel, produced in guest time (OPENPETE_AUDIO_FEED_TONE, or the driver's
own 440 Hz tone).

Instrument: per Hann window (8192 frames, hop 2048), the FFT peak near the
expected frequency, zero-padded 4x and refined by a parabola through the
log magnitudes of the peak bin and its neighbours; a window with any frame
below a quarter of the tone's amplitude (a gap, a fade, a splice into
silence) or with energy outside the peak (a crossfade or splice: residual
over 1% of the window's energy after the fitted sine is removed) is skipped
and counted. The pitch is 1200 log2(f / f_expected) in cents, where
f_expected is the tone times the tick scale in force over the device's rate
(1 + drift): the guest stream shifted by 1200 log2(tick scale).

Tick scale: --scale S (constant) or --steps T:S,... (device seconds at
which the scale changes, tap time); windows within --guard seconds of a
step are excluded from the bound and reported as transitions.

THD+N (--thdn): per window not in a gap (the envelope test above; no
splice skip), the residual energy after the fitted sine over the window's
energy, in dB: the distortion and noise a stretch's splices add. Reported
as median, p99 and max over the windows; calibrated (--calibrate) on the
tone with white noise at -40 dB and -60 dB SNR, each read within 1 dB.

Output: one line per run: windows read, skipped, median, p99 of |cents|,
max |cents| (signed), and with --spans A-B,... the same per span of tap
seconds. --trace PATH writes t_s cents per window.

Calibration (--calibrate): synthesizes the tone at +1% (expected +17.23 c)
and at 0, through the same read; prints both and fails (exit 2) when either
is off by more than 0.05 c.

Exit 0 when read, 2 on a failed calibration or bound (--p99 / --max), 3 on
zero windows read.
"""
import argparse
import math
import sys

import numpy as np

RATE = 44100
WIN = 8192
HOP = 2048
PAD = 4


def read_tap(path):
    raw = np.fromfile(path, dtype="<i2")
    raw = raw[: (raw.size // 2) * 2].reshape(-1, 2)
    return raw[:, 0].astype(np.float64)


def peak_hz(seg, want_hz, hann):
    spec = np.fft.rfft(seg * hann, n=WIN * PAD)
    mag = np.abs(spec)
    df = RATE / (WIN * PAD)
    lo = max(1, int(want_hz * 0.8 / df))
    hi = min(mag.size - 2, int(want_hz * 1.25 / df) + 1)
    k = lo + int(np.argmax(mag[lo:hi]))
    a, b, c = (math.log(max(mag[k + d], 1e-12)) for d in (-1, 0, 1))
    den = a - 2 * b + c
    off = 0.5 * (a - c) / den if den != 0 else 0.0
    return (k + off) * df


def residual_frac(seg, hz):
    n = np.arange(seg.size)
    w = 2 * math.pi * hz / RATE
    basis = np.stack([np.sin(w * n), np.cos(w * n), np.ones_like(n, dtype=np.float64)], axis=1)
    coef, *_ = np.linalg.lstsq(basis, seg, rcond=None)
    res = seg - basis @ coef
    e = float(np.dot(seg, seg))
    return float(np.dot(res, res)) / e if e > 0 else 1.0


def thdn(x, tone, amp, scale_at, drift):
    """THD+N in dB per window not in a gap."""
    hann = np.hanning(WIN)
    out = []
    for s in range(0, x.size - WIN, HOP):
        seg = x[s : s + WIN]
        env = np.max(np.abs(seg.reshape(-1, 256)), axis=1)
        if np.min(env) < 0.25 * amp:
            continue
        t = (s + WIN / 2) / RATE
        hz = peak_hz(seg, tone * scale_at(t) / (1.0 + drift), hann)
        out.append((t, 10.0 * math.log10(max(residual_frac(seg, hz), 1e-12))))
    return out


def read(x, tone, amp, scale_at, drift, guard_at):
    hann = np.hanning(WIN)
    out = []
    skipped = 0
    for s in range(0, x.size - WIN, HOP):
        seg = x[s : s + WIN]
        t = (s + WIN / 2) / RATE
        env = np.max(np.abs(seg.reshape(-1, 256)), axis=1)
        if np.min(env) < 0.25 * amp:
            skipped += 1
            continue
        want = tone * scale_at(t) / (1.0 + drift)
        hz = peak_hz(seg, want, hann)
        if residual_frac(seg, hz) > 0.01:
            skipped += 1
            continue
        out.append((t, 1200.0 * math.log2(hz / want), guard_at(t)))
    return out, skipped


def stats(c):
    a = np.abs(np.array(c))
    if a.size == 0:
        return None
    med = float(np.median(np.array(c)))
    p99 = float(np.percentile(a, 99))
    i = int(np.argmax(a))
    return med, p99, float(np.array(c)[i])


def fmt(tag, c):
    s = stats(c)
    if s is None:
        return f"{tag} n=0"
    return f"{tag} n={len(c)} median={s[0]:+.3f}c p99={s[1]:.3f}c max={s[2]:+.3f}c"


def calibrate(tone, amp):
    n = np.arange(RATE * 4)
    ok = True
    for ratio, want in ((1.0, 0.0), (1.01, 1200 * math.log2(1.01))):
        x = amp * np.sin(2 * math.pi * tone * ratio * n / RATE)
        r, _ = read(x, tone, amp, lambda t: 1.0, 0.0, lambda t: False)
        med = float(np.median([c for _, c, _ in r]))
        err = med - want
        print(f"calibrate ratio={ratio:.2f} expected={want:+.3f}c read={med:+.3f}c error={err:+.4f}c")
        ok = ok and abs(err) <= 0.05
    rng = np.random.default_rng(1)
    for snr in (40.0, 60.0):
        sig = amp * np.sin(2 * math.pi * tone * n / RATE)
        noise = rng.standard_normal(n.size) * amp / math.sqrt(2) * 10 ** (-snr / 20)
        r = thdn(sig + noise, tone, amp, lambda t: 1.0, 0.0)
        med = float(np.median([d for _, d in r]))
        print(f"calibrate thdn snr={snr:.0f}dB expected={-snr:+.1f}dB read={med:+.2f}dB")
        ok = ok and abs(med + snr) <= 1.0
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tap")
    ap.add_argument("--tone", type=float, default=440.0)
    ap.add_argument("--amp", type=float, default=12000.0)
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--steps", default="")
    ap.add_argument("--guard", type=float, default=1.0)
    ap.add_argument("--drift", type=float, default=0.0)
    ap.add_argument("--from-s", type=float, default=0.0)
    ap.add_argument("--spans", default="")
    ap.add_argument("--trace")
    ap.add_argument("--p99", type=float)
    ap.add_argument("--max", type=float)
    ap.add_argument("--tag", default="pitch")
    ap.add_argument("--calibrate", action="store_true")
    ap.add_argument("--thdn", action="store_true")
    a = ap.parse_args()
    if a.calibrate:
        ok = calibrate(a.tone, a.amp)
        print(f"[{a.tag}] calibration {'PASS' if ok else 'FAIL'}")
        return 0 if ok else 2
    steps = []
    for e in filter(None, a.steps.split(",")):
        t, s = e.split(":")
        steps.append((float(t), float(s)))

    def scale_at(t):
        s = a.scale
        for st, sv in steps:
            if t >= st:
                s = sv
        return s

    def guard_at(t):
        return any(abs(t - st) < a.guard for st, _ in steps)

    x = read_tap(a.tap)
    r, skipped = read(x, a.tone, a.amp, scale_at, a.drift, guard_at)
    r = [e for e in r if e[0] >= a.from_s]
    if a.trace:
        with open(a.trace, "w") as f:
            f.write("t_s cents guard\n")
            for t, c, g in r:
                f.write(f"{t:.4f} {c:+.4f} {int(g)}\n")
    held = [c for _, c, g in r if not g]
    trans = [c for _, c, g in r if g]
    print(f"[{a.tag}] tap_s={x.size / RATE:.1f} windows={len(r)} skipped={skipped} "
          + fmt("steady", held) + " | " + fmt("transition", trans))
    if a.thdn:
        d = [(t, v) for t, v in thdn(x, a.tone, a.amp, scale_at, a.drift) if t >= a.from_s]
        def tf(tag, v):
            if not v:
                return f"{tag} n=0"
            v = np.array(v)
            return f"{tag} n={v.size} median={np.median(v):+.1f}dB p99={np.percentile(v, 99):+.1f}dB max={v.max():+.1f}dB"
        print(f"[{a.tag}] " + tf("thdn", [v for _, v in d]))
        for sp in filter(None, a.spans.split(",")):
            lo, hi = (float(v) for v in sp.split("-"))
            print(f"[{a.tag}] " + tf(f"thdn span {lo:g}-{hi:g}s", [v for t, v in d if lo <= t < hi]))
    for sp in filter(None, a.spans.split(",")):
        lo, hi = (float(v) for v in sp.split("-"))
        print(f"[{a.tag}] " + fmt(f"span {lo:g}-{hi:g}s", [c for t, c, g in r if lo <= t < hi and not g]))
    if not held:
        print(f"[{a.tag}] verdict=REFUSED (zero windows read)")
        return 3
    s = stats(held)
    fails = []
    if a.p99 is not None and s[1] > a.p99:
        fails.append(f"p99 {s[1]:.3f} > {a.p99}")
    if a.max is not None and abs(s[2]) > a.max:
        fails.append(f"max {abs(s[2]):.3f} > {a.max}")
    if a.p99 is not None or a.max is not None:
        print(f"[{a.tag}] verdict=" + ("PASS" if not fails else "FAIL: " + "; ".join(fails)))
    return 2 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
