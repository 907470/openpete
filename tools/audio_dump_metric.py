#!/usr/bin/env python3
"""Window statistics of an OPENPETE_AUDIO_DUMP capture.

The dump is the mixed SPU output as raw s16 stereo 44.1 kHz frames, written
every VSync by the game's headless run. Each window prints its RMS in dB,
zero-crossing rate and spectral flatness: speech and music read as low
rate and flatness near 0; ADPCM decoded from data that is not a sample
reads as broadband (rate above 0.2, flatness above 0.01); digital silence
reads below -90 dB.

usage: audio_dump_metric.py DUMP.raw [--window SEC] [--vbl-offset N]
                            [--from FRAME] [--to FRAME]

The time axis is VBLs on the engine's audio clock, 735 frames per VBL
(the SPU envelope cadence psyq_VSync advances; not the NTSC VBL period, a
0.3% difference this axis does not carry); --vbl-offset maps VBL to
openpete_frame (the fast-boot VBLs precede frame 0; the VSync log line
"vbl=N frame=M" gives N-M).
"""
import argparse
import numpy as np

SR = 44100
FRAMES_PER_VBL = 735


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--window", type=float, default=0.5)
    ap.add_argument("--vbl-offset", type=int, default=0)
    ap.add_argument("--from", dest="fr0", type=float, default=-1e9)
    ap.add_argument("--to", dest="fr1", type=float, default=1e9)
    a = ap.parse_args()
    pcm = np.fromfile(a.dump, dtype="<i2")
    pcm = pcm[: len(pcm) - len(pcm) % 2].reshape(-1, 2).astype(np.float32) / 32768
    mono = pcm.mean(axis=1)
    n = len(mono)
    w = int(SR * a.window)
    print(f"frames={n} seconds={n / SR:.2f} vbls={n / FRAMES_PER_VBL:.0f}")
    print(" win  frame  rms_dB    zcr  flatness")
    for i in range(0, n - w, w):
        frame = i / FRAMES_PER_VBL - a.vbl_offset
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
