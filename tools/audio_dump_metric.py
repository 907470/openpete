#!/usr/bin/env python3
"""Window statistics of an OPENPETE_AUDIO_DUMP capture.

The dump is the mixed SPU output as raw s16 stereo 44.1 kHz frames, written
every VSync by the game's headless run. Each window prints its RMS in dB,
zero-crossing rate and spectral flatness: speech and music read as low
rate and flatness near 0; ADPCM decoded from data that is not a sample
reads as broadband (rate above 0.2, flatness above 0.01); digital silence
reads below -90 dB.

usage: audio_dump_metric.py DUMP.raw [--window SEC] [--vbl-offset N]
                            [--from FRAME] [--to FRAME] [--rate R]

The time axis is VBLs as the dump recorded them: DUMP.raw.idx holds one
`vbl file_frames produced` line per VBL, and a window's VBL is
interpolated from it, at whatever rate the SPU clock charged. A dump
without the index needs --rate (frames per VBL) and counts VBLs from the
file's first frame. --vbl-offset maps VBL to openpete_frame (the fast-boot
VBLs precede frame 0; the VSync log line "vbl=N frame=M" gives N-M).
"""
import os
import argparse
import numpy as np

SR = 44100


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--window", type=float, default=0.5)
    ap.add_argument("--vbl-offset", type=int, default=0)
    ap.add_argument("--from", dest="fr0", type=float, default=-1e9)
    ap.add_argument("--to", dest="fr1", type=float, default=1e9)
    ap.add_argument("--rate", type=float, default=0.0)
    a = ap.parse_args()
    idx_path = a.dump + ".idx"
    if os.path.exists(idx_path):
        idx = np.loadtxt(idx_path, comments="#", dtype=np.float64, ndmin=2)
        keep = np.concatenate([[True], np.diff(idx[:, 1]) > 0])
        idx = idx[keep]
        vbl_of = lambda i: float(np.interp(i, idx[:, 1], idx[:, 0]))
        live = idx[idx[:, 1] > 0]
        span = live[-1, 0] - live[0, 0] if len(live) else 0
        rate = (live[-1, 1] - live[0, 1]) / span if span > 0 else float("nan")
    elif a.rate > 0:
        vbl_of = lambda i: i / a.rate
        rate = a.rate
    else:
        ap.error(f"{idx_path} missing: pass --rate (frames per VBL)")
    pcm = np.fromfile(a.dump, dtype="<i2")
    pcm = pcm[: len(pcm) - len(pcm) % 2].reshape(-1, 2).astype(np.float32) / 32768
    mono = pcm.mean(axis=1)
    n = len(mono)
    w = int(SR * a.window)
    print(f"frames={n} seconds={n / SR:.2f} rate={rate:.3f} frames/VBL")
    print(" win  frame  rms_dB    zcr  flatness")
    for i in range(0, n - w, w):
        frame = vbl_of(i) - a.vbl_offset
        if frame < a.fr0 or frame > a.fr1:
            continue
        x = mono[i : i + w]
        rms = np.sqrt(np.mean(x * x)) + 1e-9
        zcr = np.mean(np.abs(np.diff(np.sign(x))) > 0)
        spec = np.abs(np.fft.rfft(x * np.hanning(w))) ** 2 + 1e-12
        flat = np.exp(np.mean(np.log(spec))) / np.mean(spec)
        print(f"{i // w:4d} {frame:6.0f} {20 * np.log10(rms):7.1f} {zcr:6.3f} {flat:8.4f}")


if __name__ == "__main__":
    main()
