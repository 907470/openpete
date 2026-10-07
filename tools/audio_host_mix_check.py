#!/usr/bin/env python3
"""audio_host_mix_check.py - count host clip copies in an audio dump and in a
recording's audio track (tests/harness/check_audio_host_mix.sh).

--write-tone PATH writes the probe clip: 1 kHz, 0.25 s, mono s16 44100 Hz at
0.5 full scale.

Dump leg: DIFF = mod dump - base dump (stereo s16, the same SPU render with
and without the clips). Every maximal run of nonzero DIFF samples, merged
across gaps shorter than the tone, is one copy; a copy matches when it is the
tone's length (its leading zero samples counted) and DIFF equals the tone on both channels to within the
render's saturation (corr >= 0.999 per channel). Pass: copies == the mod's
play lines (`sfx-play-probe: play ... rc=0`) > 0, every copy matches, and no
nonzero DIFF sample lies outside a copy.

Record leg: the recording's first audio track decoded by ffmpeg to s16
stereo 44100, its mid channel cross-correlated with the tone (normalized
over the tone's length); peaks >= 0.9 at least 1 s apart are copies. Pass:
copies == the record leg's play lines > 0.

Exit 0 PASS, 2 FAIL.
"""
import argparse, re, subprocess, sys, wave
import numpy as np

SR = 44100


def tone():
    n = SR // 4
    t = np.arange(n) / SR
    return np.round(0.5 * 32767 * np.sin(2 * np.pi * 1000 * t)).astype(np.int16)


def write_tone(path):
    w = wave.open(path, "wb")
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(SR)
    w.writeframes(tone().tobytes())
    w.close()


def plays(log):
    return len(re.findall(r"sfx-play-probe: play at CameraUpdate call #\d+ rc=0", open(log, errors="replace").read()))


def stereo(path):
    a = np.fromfile(path, dtype="<i2")
    return a[: len(a) // 2 * 2].reshape(-1, 2).astype(np.int64)


def dump_leg(a, tn):
    base, mod = stereo(a.base), stereo(a.mod)
    n = min(len(base), len(mod))
    d = mod[:n] - base[:n]
    nz = np.nonzero((d != 0).any(axis=1))[0]
    runs = []
    for i in nz:
        if runs and i - runs[-1][1] < len(tn):
            runs[-1][1] = i
        else:
            runs.append([i, i])
    lead = int(np.argmax(tn != 0))   # the tone's leading zero samples
    runs = [[s - lead, e] for s, e in runs]
    good, bad = 0, []
    for s, e in runs:
        seg = d[s: s + len(tn)]
        if len(seg) < len(tn) or e - s + 1 > len(tn):
            bad.append((s, e, "length"))
            continue
        cs = [np.corrcoef(seg[:, c], tn)[0, 1] for c in (0, 1)]
        if min(cs) >= 0.999:
            good += 1
        else:
            bad.append((s, e, f"corr {min(cs):.4f}"))
    exact = sum(int(np.all(d[s: s + len(tn), 0] == tn) and np.all(d[s: s + len(tn), 1] == tn)) for s, _ in runs)
    p = plays(a.mod_log)
    print(f"[audio-host-mix] dump: frames {n} plays {p} copies {len(runs)} matched {good} "
          f"exact {exact} first_copy_s {runs[0][0] / SR if runs else -1:.3f}")
    for s, e, why in bad:
        print(f"  bad copy at {s / SR:.3f}s ({e - s + 1} samples): {why}")
    fails = []
    if p == 0:
        fails.append("no plays (engagement 0)")
    if len(runs) != p:
        fails.append(f"{len(runs)} copies, {p} plays")
    if bad:
        fails.append(f"{len(bad)} copies do not match the tone")
    return fails


def record_leg(a, tn):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", a.record, "-map", "0:a:0", "-f", "s16le",
                          "-ac", "2", "-ar", str(SR), "-"], capture_output=True, check=False).stdout
    pcm = np.frombuffer(raw, dtype="<i2")
    pcm = pcm[: len(pcm) // 2 * 2].reshape(-1, 2).astype(np.float64)
    mid = pcm.mean(axis=1)
    t = tn.astype(np.float64)
    t = (t - t.mean()) / np.sqrt(((t - t.mean()) ** 2).sum())
    copies = 0
    if len(mid) > len(t):
        m = 1 << (len(mid) + len(t)).bit_length()
        c = np.fft.irfft(np.fft.rfft(mid, m) * np.conj(np.fft.rfft(t, m)), m)[: len(mid) - len(t) + 1]
        cs = np.concatenate([[0.0], np.cumsum(mid)])
        cq = np.concatenate([[0.0], np.cumsum(mid * mid)])
        L = len(t)
        mean = (cs[L:] - cs[:-L]) / L
        var = (cq[L:] - cq[:-L]) - L * mean * mean
        corr = c / (np.sqrt(np.maximum(var, 1e-9)))
        i, last = 0, -SR
        idx = np.nonzero(corr >= 0.9)[0]
        for i in idx:
            if i - last >= SR:
                copies += 1
                last = i
    p = plays(a.record_log)
    print(f"[audio-host-mix] record: audio frames {len(mid)} ({len(mid) / SR:.1f}s) plays {p} copies {copies}")
    fails = []
    if len(mid) == 0:
        fails.append("recording has no audio track")
    if p == 0:
        fails.append("record leg made no plays (engagement 0)")
    if copies != p:
        fails.append(f"record: {copies} copies, {p} plays")
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write-tone")
    ap.add_argument("--tone")
    ap.add_argument("--base")
    ap.add_argument("--mod")
    ap.add_argument("--mod-log")
    ap.add_argument("--record")
    ap.add_argument("--record-log")
    a = ap.parse_args()
    if a.write_tone:
        write_tone(a.write_tone)
        return 0
    tn = tone().astype(np.int64)
    fails = dump_leg(a, tn)
    if a.record:
        fails += record_leg(a, tn)
    print("[audio-host-mix] verdict=" + ("PASS" if not fails else "FAIL: " + "; ".join(fails)))
    return 0 if not fails else 2


if __name__ == "__main__":
    sys.exit(main())
